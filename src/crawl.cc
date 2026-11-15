#include "src/crawl.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/http.h"
#include "src/journal.h"
#include "src/progress.h"

namespace khanbot {
namespace {

constexpr int kWorkers = 4;
constexpr int kAttempts = 3;
constexpr int kMaxConsecutiveFailures = 5;
constexpr int kMaxFailuresShown = 10;
constexpr std::size_t kYoutubeIdLength = 11;
constexpr std::chrono::milliseconds kRequestDelay{150};
constexpr std::chrono::milliseconds kRetryBackoff{500};
constexpr std::size_t kNpos = std::string_view::npos;

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

struct FetchFailure {
    std::string message;
    Response response;
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
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

bool HasWhitespace(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    });
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
    std::size_t position = block.find(kYoutubeMarker);
    if (position == kNpos)
        return {};
    position += kYoutubeMarker.size();
    std::size_t length = 0;
    while (length < kYoutubeIdLength && position + length < block.size()) {
        const char c = block[position + length];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' &&
            c != '-') {
            break;
        }
        ++length;
    }
    return length == kYoutubeIdLength
               ? std::string(block.substr(position, length))
               : std::string();
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
        if (!loc.has_value())
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
    SitemapRecord record;
    record.path = std::string(fields[0]);
    record.youtube_ids = ParseList(fields[1]);
    record.orphan_urls = ParseList(fields[2]);
    return record;
}

bool IsValidJournalLine(std::string_view line) {
    return ParseJournalLine(line).has_value();
}

std::vector<SitemapRecord> LoadRecords(const Journal &journal) {
    std::vector<SitemapRecord> records;
    for (const std::string &line : journal.Snapshot()) {
        std::optional<SitemapRecord> record = ParseJournalLine(line);
        if (record.has_value())
            records.push_back(std::move(*record));
    }
    return records;
}

std::expected<std::string, FetchFailure>
FetchXml(Http &http, const std::string &url, std::string_view marker,
         const std::atomic<bool> &stop) {
    FetchFailure failure;
    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
        if (stop.load()) {
            failure.message = "interrupted";
            return std::unexpected(std::move(failure));
        }
        std::expected<Response, std::string> result = http.Get(url);
        if (result.has_value() && result->status == 200 &&
            result->body.find(marker) != std::string::npos) {
            return std::move(result->body);
        }
        if (result.has_value()) {
            failure.response = std::move(*result);
            failure.message = DescribeResponse(failure.response);
        } else {
            failure.message = result.error();
        }
        if (attempt < kAttempts)
            std::this_thread::sleep_for(kRetryBackoff * attempt);
    }
    failure.message =
        "after " + std::to_string(kAttempts) + " attempts: " + failure.message;
    return std::unexpected(std::move(failure));
}

std::expected<ExportStats, std::string>
ExportResults(const std::vector<SitemapRecord> &records,
              const std::filesystem::path &directory) {
    ExportStats stats;
    std::set<std::string> ids;
    std::set<std::string> orphans;
    for (const SitemapRecord &record : records) {
        stats.total_ids += record.youtube_ids.size();
        ids.insert(record.youtube_ids.begin(), record.youtube_ids.end());
        orphans.insert(record.orphan_urls.begin(), record.orphan_urls.end());
    }
    stats.unique_ids = ids.size();
    stats.orphan_urls = orphans.size();
    if (auto written = WriteLinesAtomically(directory / kYoutubeIdsName, ids);
        !written.has_value()) {
        return std::unexpected(written.error());
    }
    if (auto written =
            WriteLinesAtomically(directory / kOrphanUrlsName, orphans);
        !written.has_value()) {
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
    struct Failed {
        std::string path;
        std::string message;
    };

    std::expected<std::vector<std::string>, std::string>
    LoadSitemapPaths(Http &http);
    void Work(Http &http, Progress &progress);
    void RecordFailure(const std::string &path, std::string message,
                       Progress &progress);
    void Abort(std::string reason);
    RunStatus Finish(const std::vector<std::string> &paths);
    RunStatus Fatal(const std::string &message) const;

    const CommandOptions options_;
    const std::atomic<bool> &stop_;
    std::unique_ptr<Journal> journal_;
    std::vector<std::string> pending_;
    std::atomic<std::size_t> next_{0};
    std::atomic<int> consecutive_failures_{0};
    std::atomic<bool> abort_{false};
    std::mutex mutex_;
    std::vector<Failed> failures_;
    std::string abort_reason_;
};

RunStatus Crawler::Fatal(const std::string &message) const {
    std::cerr << "error: " << message << '\n';
    return RunStatus::kFatal;
}

void Crawler::Abort(std::string reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (abort_reason_.empty())
        abort_reason_ = std::move(reason);
    abort_.store(true);
}

void Crawler::RecordFailure(const std::string &path, std::string message,
                            Progress &progress) {
    progress.Fail();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failures_.push_back({path, std::move(message)});
    }
    if (consecutive_failures_.fetch_add(1) + 1 >= kMaxConsecutiveFailures) {
        Abort(std::to_string(kMaxConsecutiveFailures) +
              " consecutive failures; the site may be challenging requests");
    }
}

std::expected<std::vector<std::string>, std::string>
Crawler::LoadSitemapPaths(Http &http) {
    const std::string url = options_.base_url + "/sitemap.xml";
    std::expected<std::string, FetchFailure> index =
        FetchXml(http, url, "<sitemapindex", stop_);
    if (!index.has_value()) {
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
    while (!stop_.load() && !abort_.load()) {
        const std::size_t index = next_.fetch_add(1);
        if (index >= pending_.size())
            return;
        const std::string &path = pending_[index];
        std::expected<std::string, FetchFailure> body =
            FetchXml(http, options_.base_url + path, "<urlset", stop_);
        if (!body.has_value()) {
            if (stop_.load())
                return;
            RecordFailure(path, std::move(body.error().message), progress);
            continue;
        }
        const SitemapRecord record = ParseUrlset(path, *body);
        if (!journal_->Append(FormatJournalLine(record))) {
            Abort("cannot write " + std::string(kJournalName));
            return;
        }
        consecutive_failures_.store(0);
        progress.Advance(record.youtube_ids.size() + record.orphan_urls.size());
        std::this_thread::sleep_for(kRequestDelay);
    }
}

RunStatus Crawler::Finish(const std::vector<std::string> &paths) {
    const std::vector<SitemapRecord> records = LoadRecords(*journal_);
    const std::expected<ExportStats, std::string> exported =
        ExportResults(records, options_.data_dir);
    if (!exported.has_value())
        return Fatal(exported.error());

    std::size_t crawled = 0;
    for (const std::string &path : paths) {
        if (journal_->Contains(path))
            ++crawled;
    }
    const bool interrupted = stop_.load();
    const bool incomplete = abort_.load() || !failures_.empty();

    std::cout << (interrupted  ? "Crawl interrupted"
                  : incomplete ? "Crawl incomplete"
                               : "Crawl complete")
              << '\n'
              << "  Sitemaps  " << FormatCount(crawled) << " of "
              << FormatCount(paths.size()) << " crawled\n"
              << "  Videos    " << FormatCount(exported->unique_ids)
              << " unique YouTube IDs from " << FormatCount(exported->total_ids)
              << " entries\n"
              << "  No ID     " << FormatCount(exported->orphan_urls)
              << " video URLs without a YouTube ID\n"
              << "  Output    "
              << (options_.data_dir / kYoutubeIdsName).string()
              << "\n            "
              << (options_.data_dir / kOrphanUrlsName).string() << '\n';

    int shown = 0;
    for (const Failed &failed : failures_) {
        if (shown++ == kMaxFailuresShown) {
            std::cerr << "  ... and "
                      << FormatCount(failures_.size() - kMaxFailuresShown)
                      << " more\n";
            break;
        }
        std::cerr << "  failed  " << failed.path << "  " << failed.message
                  << '\n';
    }
    if (abort_.load())
        std::cerr << "Stopped early: " << abort_reason_ << '\n';
    if (interrupted) {
        std::cerr << "Progress is saved; run again to resume.\n";
        return RunStatus::kInterrupted;
    }
    if (incomplete) {
        std::cerr << "Run again to retry the sitemaps that did not finish.\n";
        return RunStatus::kIncomplete;
    }
    return RunStatus::kComplete;
}

RunStatus Crawler::Run() {
    std::error_code error;
    std::filesystem::create_directories(options_.data_dir, error);
    if (error) {
        return Fatal("cannot create " + options_.data_dir.string() + ": " +
                     error.message());
    }

    std::expected<std::unique_ptr<Journal>, std::string> journal =
        Journal::Open(options_.data_dir / kJournalName, options_.fresh,
                      IsValidJournalLine);
    if (!journal.has_value())
        return Fatal(journal.error());
    journal_ = std::move(*journal);

    std::vector<std::unique_ptr<Http>> clients;
    for (int i = 0; i < kWorkers; ++i) {
        std::expected<std::unique_ptr<Http>, std::string> client =
            Http::Create();
        if (!client.has_value())
            return Fatal(client.error());
        clients.push_back(std::move(*client));
    }

    const std::expected<std::vector<std::string>, std::string> paths =
        LoadSitemapPaths(*clients.front());
    if (!paths.has_value()) {
        if (stop_.load())
            return RunStatus::kInterrupted;
        return Fatal(paths.error());
    }

    for (const std::string &path : *paths) {
        if (!journal_->Contains(path))
            pending_.push_back(path);
    }
    std::size_t known_items = 0;
    for (const SitemapRecord &record : LoadRecords(*journal_)) {
        known_items += record.youtube_ids.size() + record.orphan_urls.size();
    }
    std::cerr << "Sitemap index: " << FormatCount(paths->size())
              << " sitemaps, " << FormatCount(paths->size() - pending_.size())
              << " already crawled, " << FormatCount(pending_.size())
              << " to fetch\n";

    if (!pending_.empty()) {
        Progress progress(Progress::Options{
            .title = "Crawling sitemaps",
            .extra_label = "videos",
            .total = paths->size(),
            .completed = paths->size() - pending_.size(),
            .extra = known_items,
        });
        {
            std::vector<std::jthread> pool;
            for (const std::unique_ptr<Http> &client : clients) {
                pool.emplace_back(
                    [this, &client, &progress] { Work(*client, progress); });
            }
        }
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
