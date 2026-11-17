#include "src/crawl.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <format>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/http.h"
#include "src/journal.h"
#include "src/progress.h"
#include "src/result.h"
#include "src/run.h"
#include "src/text.h"

namespace khanbot {
namespace {
constexpr char kJournalName[] = "crawl.journal";
constexpr char kYoutubeIdsName[] = "youtube_ids.txt";
constexpr char kOrphanUrlsName[] = "no_youtube.txt";
constexpr char kFailureDumpName[] = "failed-response.txt";

constexpr std::string_view kLocOpen = "<loc>";
constexpr std::string_view kLocClose = "</loc>";
constexpr std::string_view kUrlOpen = "<url>";
constexpr std::string_view kYoutubeMarker = "ka-youtube-converted/";

struct SitemapRecord {
    std::string path;
    std::vector<std::string> youtube_ids;
    std::vector<std::string> orphan_urls;
};

struct ExportStats {
    std::size_t total_ids = 0;
    std::size_t unique_ids = 0;
    std::size_t orphan_urls = 0;
};

std::optional<std::string_view>
Between(std::string_view text, std::string_view open, std::string_view close) {
    std::size_t begin = text.find(open);
    if (begin == kNpos)
        return std::nullopt;
    begin += open.size();
    const std::size_t end = text.find(close, begin);
    if (end == kNpos)
        return std::nullopt;
    return text.substr(begin, end - begin);
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && IsAsciiSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && IsAsciiSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

bool HasWhitespace(std::string_view text) {
    return std::ranges::any_of(text, IsAsciiSpace);
}

std::vector<std::string> ExtractLocs(std::string_view xml) {
    std::vector<std::string> locs;
    std::size_t position = 0;
    while (true) {
        const std::size_t begin = xml.find(kLocOpen, position);
        if (begin == kNpos)
            break;
        const std::size_t value_begin = begin + kLocOpen.size();
        const std::size_t end = xml.find(kLocClose, value_begin);
        if (end == kNpos)
            break;
        const std::string_view loc =
            Trim(xml.substr(value_begin, end - value_begin));
        if (!loc.empty() && !HasWhitespace(loc))
            locs.emplace_back(loc);
        position = end + kLocClose.size();
    }
    return locs;
}

std::string PathOf(std::string_view url) {
    const std::size_t scheme = url.find("://");
    if (scheme == kNpos)
        return std::string(url);
    const std::size_t slash = url.find('/', scheme + 3);
    return slash == kNpos ? std::string("/") : std::string(url.substr(slash));
}

bool IsVideoUrl(std::string_view url) {
    const std::size_t marker = url.rfind("/v/");
    return marker != kNpos && marker + 3 < url.size() &&
           url.find('/', marker + 3) == kNpos;
}

std::string ExtractYoutubeId(std::string_view block) {
    const std::size_t marker = block.find(kYoutubeMarker);
    if (marker == kNpos)
        return {};
    const std::string_view id =
        block.substr(marker + kYoutubeMarker.size(), kYoutubeIdLength);
    return IsYoutubeId(id) ? std::string(id) : std::string();
}

SitemapRecord ParseUrlset(const std::string &path, std::string_view xml) {
    SitemapRecord record;
    record.path = path;
    std::size_t position = xml.find(kUrlOpen);
    while (position != kNpos) {
        const std::size_t block_begin = position + kUrlOpen.size();
        const std::size_t following = xml.find(kUrlOpen, block_begin);
        const std::string_view block = xml.substr(
            block_begin, following == kNpos ? kNpos : following - block_begin);
        position = following;
        const std::optional<std::string_view> loc =
            Between(block, kLocOpen, kLocClose);
        if (!loc)
            continue;
        const std::string_view url = Trim(*loc);
        if (!IsVideoUrl(url) || HasWhitespace(url))
            continue;
        std::string youtube_id = ExtractYoutubeId(block);
        if (youtube_id.empty()) {
            record.orphan_urls.emplace_back(url);
        } else {
            record.youtube_ids.push_back(std::move(youtube_id));
        }
    }
    return record;
}

std::vector<std::string_view> Split(std::string_view text, char delimiter) {
    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    while (true) {
        const std::size_t end = text.find(delimiter, begin);
        if (end == kNpos) {
            parts.push_back(text.substr(begin));
            return parts;
        }
        parts.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
}

std::vector<std::string> ParseList(std::string_view field) {
    std::vector<std::string> items;
    for (const std::string_view item : Split(field, ' ')) {
        if (!item.empty())
            items.emplace_back(item);
    }
    return items;
}

std::string Join(const std::vector<std::string> &items) {
    std::string joined;
    for (const std::string &item : items) {
        if (!joined.empty())
            joined += ' ';
        joined += item;
    }
    return joined;
}

std::string FormatJournalLine(const SitemapRecord &record) {
    return record.path + '\t' + Join(record.youtube_ids) + '\t' +
           Join(record.orphan_urls);
}

std::optional<SitemapRecord> ParseJournalLine(std::string_view line) {
    const std::vector<std::string_view> fields = Split(line, '\t');
    if (fields.size() != 3 || fields[0].empty())
        return std::nullopt;
    return SitemapRecord{.path = std::string(fields[0]),
                         .youtube_ids = ParseList(fields[1]),
                         .orphan_urls = ParseList(fields[2])};
}

bool IsValidJournalLine(std::string_view line) {
    return ParseJournalLine(line).has_value();
}

std::vector<SitemapRecord> LoadRecords(const Journal &journal) {
    std::vector<SitemapRecord> records;
    for (const std::string &line : journal.Snapshot()) {
        if (std::optional<SitemapRecord> record = ParseJournalLine(line))
            records.push_back(std::move(*record));
    }
    return records;
}

Result<std::string, FetchFailure> FetchXml(Http &http, const std::string &url,
                                           std::string_view marker,
                                           const std::atomic<bool> &stop) {
    return Retry<std::string>(stop, [&]() -> Result<std::string, FetchFailure> {
        Result<Response> result = http.Get(url);
        if (!result)
            return Failed(std::move(result.error()));
        if (result->status == kHttpOk && result->body.contains(marker))
            return std::move(result->body);
        std::string message = DescribeResponse(*result);
        return Failed(std::move(message), std::move(*result));
    });
}

Result<ExportStats> ExportResults(const std::vector<SitemapRecord> &records,
                                  const std::filesystem::path &directory) {
    ExportStats stats;
    std::vector<std::string> ids;
    std::vector<std::string> orphans;
    for (const SitemapRecord &record : records) {
        stats.total_ids += record.youtube_ids.size();
        ids.insert(ids.end(), record.youtube_ids.begin(),
                   record.youtube_ids.end());
        orphans.insert(orphans.end(), record.orphan_urls.begin(),
                       record.orphan_urls.end());
    }
    SortUnique(ids);
    SortUnique(orphans);
    stats.unique_ids = ids.size();
    stats.orphan_urls = orphans.size();
    if (auto written = WriteLinesAtomically(directory / kYoutubeIdsName, ids);
        !written) {
        return std::unexpected(written.error());
    }
    if (auto written =
            WriteLinesAtomically(directory / kOrphanUrlsName, orphans);
        !written) {
        return std::unexpected(written.error());
    }
    return stats;
}

class Crawler {
  public:
    Crawler(const CommandOptions &options, const std::atomic<bool> &stop)
        : options_(options), stop_(stop) {}

    RunStatus Run();

  private:
    Result<std::vector<std::string>> LoadSitemapPaths(Http &http);
    void Work(Http &http, Progress &progress);
    RunStatus Finish(const std::vector<std::string> &paths);

    const CommandOptions options_;
    const std::atomic<bool> &stop_;
    std::unique_ptr<Journal> journal_;
    std::vector<std::string> pending_;
    std::atomic<std::size_t> next_{0};
    FailureLog log_;
};

Result<std::vector<std::string>> Crawler::LoadSitemapPaths(Http &http) {
    const std::string url = options_.base_url + "/sitemap.xml";
    Result<std::string, FetchFailure> index =
        FetchXml(http, url, "<sitemapindex", stop_);
    if (!index) {
        const FetchFailure &failure = index.error();
        std::string message =
            "could not load the sitemap index: " + failure.message;
        if (failure.response.status != 0) {
            const std::filesystem::path dump =
                options_.data_dir / kFailureDumpName;
            WriteResponseDump(dump, url, failure.response);
            message += "\nsaved the full response to " + dump.string();
        }
        return std::unexpected(std::move(message));
    }
    std::vector<std::string> paths;
    std::unordered_set<std::string> seen;
    for (const std::string &loc : ExtractLocs(*index)) {
        std::string path = PathOf(loc);
        if (seen.insert(path).second)
            paths.push_back(std::move(path));
    }
    if (paths.empty()) {
        return std::unexpected(
            std::string("the sitemap index lists no sitemaps"));
    }
    return paths;
}

void Crawler::Work(Http &http, Progress &progress) {
    while (!stop_.load() && !log_.aborted()) {
        const std::size_t index = next_.fetch_add(1);
        if (index >= pending_.size())
            return;
        const std::string &path = pending_[index];
        Result<std::string, FetchFailure> body =
            FetchXml(http, options_.base_url + path, "<urlset", stop_);
        if (!body) {
            if (stop_.load())
                return;
            progress.Fail();
            log_.Record(path, std::move(body.error().message));
            continue;
        }
        const SitemapRecord record = ParseUrlset(path, *body);
        if (!journal_->Append(FormatJournalLine(record))) {
            log_.Abort("cannot write " + std::string(kJournalName));
            return;
        }
        log_.Succeeded();
        progress.Advance(record.youtube_ids.size() + record.orphan_urls.size());
        WaitFor(stop_, kRequestDelay);
    }
}

RunStatus Crawler::Finish(const std::vector<std::string> &paths) {
    const Result<ExportStats> exported =
        ExportResults(LoadRecords(*journal_), options_.data_dir);
    if (!exported)
        return Fatal(exported.error());

    const auto crawled = static_cast<std::size_t>(
        std::ranges::count_if(paths, [&](const std::string &path) {
            return journal_->Contains(path);
        }));
    const std::string details = std::format(
        "  Sitemaps  {} of {} crawled\n"
        "  Videos    {} unique YouTube IDs from {} entries\n"
        "  No ID     {} video URLs without a YouTube ID\n"
        "  Output    {}\n"
        "            {}",
        FormatCount(crawled), FormatCount(paths.size()),
        FormatCount(exported->unique_ids), FormatCount(exported->total_ids),
        FormatCount(exported->orphan_urls),
        (options_.data_dir / kYoutubeIdsName).string(),
        (options_.data_dir / kOrphanUrlsName).string());
    return Report("Crawl", "sitemaps", stop_.load(), log_, details);
}

RunStatus Crawler::Run() {
    if (const Result<void> made = EnsureDirectory(options_.data_dir); !made)
        return Fatal(made.error());

    Result<std::unique_ptr<Journal>> journal = Journal::Open(
        options_.data_dir / kJournalName, options_.fresh, IsValidJournalLine);
    if (!journal)
        return Fatal(journal.error());
    journal_ = std::move(*journal);

    const Result<std::vector<std::unique_ptr<Http>>> clients =
        CreateClients(kWorkers);
    if (!clients)
        return Fatal(clients.error());

    const Result<std::vector<std::string>> paths =
        LoadSitemapPaths(*clients->front());
    if (!paths) {
        if (stop_.load())
            return RunStatus::kInterrupted;
        return Fatal(paths.error());
    }

    std::ranges::copy_if(
        *paths, std::back_inserter(pending_),
        [&](const std::string &path) { return !journal_->Contains(path); });
    std::size_t known_items = 0;
    for (const SitemapRecord &record : LoadRecords(*journal_))
        known_items += record.youtube_ids.size() + record.orphan_urls.size();
    std::cerr << std::format(
        "Sitemap index: {} sitemaps, {} already crawled, {} to fetch\n",
        FormatCount(paths->size()),
        FormatCount(paths->size() - pending_.size()),
        FormatCount(pending_.size()));

    if (!pending_.empty()) {
        Progress progress(Progress::Options{
            .title = "Crawling sitemaps",
            .extra_label = "videos",
            .total = paths->size(),
            .completed = paths->size() - pending_.size(),
            .extra = known_items,
        });
        RunWorkers(*clients, [&](Http &http) { Work(http, progress); });
        progress.Finish();
    }
    return Finish(*paths);
}

} // namespace

RunStatus RunCrawl(const CommandOptions &options,
                   const std::atomic<bool> &stop) {
    return Crawler(options, stop).Run();
}

} // namespace khanbot
