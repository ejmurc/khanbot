#include "src/resolve.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <glaze/json.hpp>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/content_ids.h"
#include "src/http.h"
#include "src/journal.h"
#include "src/progress.h"
#include "src/result.h"
#include "src/run.h"
#include "src/text.h"

namespace khanbot {
namespace {
constexpr double kMaxDurationSeconds = 1.0e6;

constexpr char kJournalName[] = "resolve.journal";
constexpr char kYoutubeIdsName[] = "youtube_ids.txt";
constexpr char kContentIdsName[] = "content_ids.tsv";
constexpr char kNotFoundName[] = "not_found.txt";
constexpr char kFailureDumpName[] = "resolve-failed-response.txt";

constexpr std::string_view kOperation = "videoByYoutubeId";
constexpr std::string_view kVariablesPrefix = "%7B%22youtubeId%22%3A%22";
constexpr std::string_view kVariablesSuffix = "%22%7D";

constexpr std::string_view kQuery =
    "query videoByYoutubeId($youtubeId: String!) {\n"
    "  video: videoByYoutubeId(youtubeId: $youtubeId) {\n"
    "    id\n"
    "    assessmentItemTags\n"
    "    contentKind\n"
    "    defaultUrlPath\n"
    "    kind\n"
    "    nodeSlug\n"
    "    slug\n"
    "    sponsored\n"
    "    title\n"
    "    translatedDescription\n"
    "    translatedTitle\n"
    "    translatedCustomTitleTag\n"
    "    authorNames\n"
    "    authorList {\n"
    "      name\n"
    "      __typename\n"
    "    }\n"
    "    clarificationsEnabled\n"
    "    creationDate\n"
    "    dateAdded\n"
    "    description\n"
    "    descriptionHtml\n"
    "    downloadUrls\n"
    "    duration\n"
    "    imageUrl\n"
    "    kaUrl\n"
    "    kaUserLicense\n"
    "    keywords\n"
    "    readableId\n"
    "    relativeUrl\n"
    "    sha\n"
    "    thumbnailUrls {\n"
    "      category\n"
    "      url\n"
    "      __typename\n"
    "    }\n"
    "    translatedDescriptionHtml\n"
    "    translatedYoutubeId\n"
    "    translatedYoutubeLang\n"
    "    youtubeId\n"
    "    augmentedTranscript\n"
    "    __typename\n"
    "  }\n"
    "}";

constexpr std::uint32_t kQueryHash = HashDocument(kQuery);
static_assert(kQuery.size() == 781);
static_assert(kQueryHash == 454155533u);

constexpr glz::opts kReadOptions{.error_on_unknown_keys = false};

struct Video {
    std::string id;
    std::string youtube_id;
    std::optional<double> duration;

    struct glaze {
        using T = Video;
        static constexpr auto value =
            glz::object("id", &T::id, "youtubeId", &T::youtube_id, "duration",
                        &T::duration);
    };
};

struct Data {
    std::optional<Video> video;

    struct glaze {
        using T = Data;
        static constexpr auto value = glz::object("video", &T::video);
    };
};

struct GraphqlError {
    std::string message;

    struct glaze {
        using T = GraphqlError;
        static constexpr auto value = glz::object("message", &T::message);
    };
};

struct Reply {
    std::optional<Data> data;
    std::optional<std::vector<GraphqlError>> errors;

    struct glaze {
        using T = Reply;
        static constexpr auto value =
            glz::object("data", &T::data, "errors", &T::errors);
    };
};

struct YoutubeIds {
    std::vector<std::string> ids;
    std::size_t invalid = 0;
};

struct ExportStats {
    std::size_t resolved = 0;
    std::size_t not_found = 0;
};

std::string RequestUrl(const std::string &base_url,
                       std::string_view youtube_id) {
    std::string url = base_url + "/api/internal/graphql/";
    url.append(kOperation)
        .append("?hash=")
        .append(std::to_string(kQueryHash))
        .append("&variables=")
        .append(kVariablesPrefix)
        .append(youtube_id)
        .append(kVariablesSuffix);
    return url;
}

bool IsValidJournalLine(std::string_view line) {
    return ParseEntry(line).has_value();
}

std::unordered_map<std::string, Lookup> LoadEntries(const Journal &journal) {
    std::unordered_map<std::string, Lookup> entries;
    for (const std::string &line : journal.Snapshot()) {
        if (std::optional<Entry> entry = ParseEntry(line))
            entries[entry->youtube_id] = std::move(entry->lookup);
    }
    return entries;
}

Result<YoutubeIds> LoadYoutubeIds(const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in) {
        return std::unexpected("cannot read " + path.string() +
                               "; run the crawl command first");
    }
    YoutubeIds result;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (IsYoutubeId(line)) {
            result.ids.push_back(line);
        } else {
            ++result.invalid;
        }
    }
    SortUnique(result.ids);
    if (result.ids.empty())
        return std::unexpected("no valid YouTube IDs in " + path.string());
    return result;
}

Result<Lookup> ExtractLookup(const std::string &body,
                             std::string_view youtube_id) {
    Reply reply;
    const glz::error_ctx error = glz::read<kReadOptions>(reply, body);
    if (error) {
        return std::unexpected("malformed response: " +
                               glz::format_error(error, body));
    }
    if (reply.errors && !reply.errors->empty()) {
        return std::unexpected("graphql error: " +
                               reply.errors->front().message);
    }
    if (!reply.data)
        return std::unexpected(std::string("response has no data"));
    if (!reply.data->video)
        return Lookup{};
    const Video &video = *reply.data->video;
    if (video.youtube_id != youtube_id) {
        return std::unexpected("response is for video '" + video.youtube_id +
                               "'");
    }
    if (!IsContentId(video.id))
        return std::unexpected(std::string("response has no usable id"));
    if (!video.duration ||
        !(*video.duration >= static_cast<double>(kMinDurationSeconds)) ||
        *video.duration > kMaxDurationSeconds) {
        return std::unexpected(std::string("response has no usable duration"));
    }
    return Lookup{video.id, std::lround(*video.duration)};
}

Result<Lookup, FetchFailure> FetchLookup(Http &http, const std::string &url,
                                         std::string_view youtube_id,
                                         const std::atomic<bool> &stop) {
    return Retry<Lookup>(stop, [&]() -> Result<Lookup, FetchFailure> {
        Result<Response> result = http.Get(url);
        if (!result)
            return Failed(std::move(result.error()));
        Response &response = *result;
        if (response.status == kHttpForbidden) {
            std::string message = DescribeResponse(response);
            return Rejected(std::move(message), std::move(response));
        }
        if (response.status == kHttpOk) {
            Result<Lookup> lookup = ExtractLookup(response.body, youtube_id);
            if (lookup)
                return std::move(*lookup);
            return Failed(std::move(lookup.error()), std::move(response));
        }
        std::string message = DescribeResponse(response);
        return Failed(std::move(message), std::move(response));
    });
}

Result<ExportStats>
ExportResults(const std::unordered_map<std::string, Lookup> &entries,
              const std::vector<std::string> &ids,
              const std::filesystem::path &directory) {
    std::vector<std::string> resolved;
    std::vector<std::string> not_found;
    for (const std::string &id : ids) {
        const auto entry = entries.find(id);
        if (entry == entries.end())
            continue;
        if (entry->second.found()) {
            resolved.push_back(FormatEntry(id, entry->second));
        } else {
            not_found.push_back(id);
        }
    }
    if (auto written =
            WriteLinesAtomically(directory / kContentIdsName, resolved);
        !written) {
        return std::unexpected(written.error());
    }
    if (auto written =
            WriteLinesAtomically(directory / kNotFoundName, not_found);
        !written) {
        return std::unexpected(written.error());
    }
    return ExportStats{resolved.size(), not_found.size()};
}

class Resolver {
  public:
    Resolver(const CommandOptions &options, const std::atomic<bool> &stop)
        : options_(options), stop_(stop) {}

    RunStatus Run();

  private:
    void Work(Http &http, Progress &progress);
    void RejectQuery(const std::string &url, const FetchFailure &failure);
    RunStatus Finish(const std::vector<std::string> &ids);

    const CommandOptions options_;
    const std::atomic<bool> &stop_;
    std::unique_ptr<Journal> journal_;
    std::vector<std::string> pending_;
    std::atomic<std::size_t> next_{0};
    FailureLog log_;
};

void Resolver::RejectQuery(const std::string &url,
                           const FetchFailure &failure) {
    log_.Abort([&] {
        const std::filesystem::path dump = options_.data_dir / kFailureDumpName;
        WriteResponseDump(dump, url, failure.response);
        return "HTTP 403: the safelisted query may be stale, or the site is "
               "challenging requests\nsaved the full response to " +
               dump.string();
    });
}

void Resolver::Work(Http &http, Progress &progress) {
    while (!stop_.load() && !log_.aborted()) {
        const std::size_t index = next_.fetch_add(1);
        if (index >= pending_.size())
            return;
        const std::string &youtube_id = pending_[index];
        const std::string url = RequestUrl(options_.base_url, youtube_id);
        Result<Lookup, FetchFailure> lookup =
            FetchLookup(http, url, youtube_id, stop_);
        if (!lookup) {
            if (stop_.load())
                return;
            if (lookup.error().rejected) {
                RejectQuery(url, lookup.error());
                return;
            }
            progress.Fail();
            log_.Record(youtube_id, std::move(lookup.error().message));
            continue;
        }
        if (!journal_->Append(FormatEntry(youtube_id, *lookup))) {
            log_.Abort("cannot write " + std::string(kJournalName));
            return;
        }
        log_.Succeeded();
        progress.Advance(lookup->found() ? 1 : 0);
        WaitFor(stop_, kRequestDelay);
    }
}

RunStatus Resolver::Finish(const std::vector<std::string> &ids) {
    const Result<ExportStats> exported =
        ExportResults(LoadEntries(*journal_), ids, options_.data_dir);
    if (!exported)
        return Fatal(exported.error());

    const std::string details =
        std::format("  Resolved   {} of {} YouTube IDs\n"
                    "  Not found  {} YouTube IDs unknown to Khan Academy\n"
                    "  Output     {}\n"
                    "             {}",
                    FormatCount(exported->resolved), FormatCount(ids.size()),
                    FormatCount(exported->not_found),
                    (options_.data_dir / kContentIdsName).string(),
                    (options_.data_dir / kNotFoundName).string());
    return Report("Resolve", "YouTube IDs", stop_.load(), log_, details);
}

RunStatus Resolver::Run() {
    if (const Result<void> made = EnsureDirectory(options_.data_dir); !made)
        return Fatal(made.error());

    const Result<YoutubeIds> input =
        LoadYoutubeIds(options_.data_dir / kYoutubeIdsName);
    if (!input)
        return Fatal(input.error());

    Result<std::unique_ptr<Journal>> journal = Journal::Open(
        options_.data_dir / kJournalName, options_.fresh, IsValidJournalLine);
    if (!journal)
        return Fatal(journal.error());
    journal_ = std::move(*journal);

    const std::unordered_map<std::string, Lookup> known =
        LoadEntries(*journal_);
    std::size_t known_resolved = 0;
    for (const std::string &id : input->ids) {
        const auto entry = known.find(id);
        if (entry == known.end()) {
            pending_.push_back(id);
        } else if (entry->second.found()) {
            ++known_resolved;
        }
    }
    std::cerr << std::format(
        "YouTube IDs: {}, {} already looked up, {} to fetch\n",
        FormatCount(input->ids.size()),
        FormatCount(input->ids.size() - pending_.size()),
        FormatCount(pending_.size()));
    if (input->invalid > 0) {
        std::cerr << std::format("  skipped {} malformed lines in {}\n",
                                 FormatCount(input->invalid), kYoutubeIdsName);
    }

    if (!pending_.empty()) {
        const Result<std::vector<std::unique_ptr<Http>>> clients =
            CreateClients(kWorkers);
        if (!clients)
            return Fatal(clients.error());
        Progress progress(Progress::Options{
            .title = "Resolving videos",
            .extra_label = "resolved",
            .total = input->ids.size(),
            .completed = input->ids.size() - pending_.size(),
            .extra = known_resolved,
        });
        RunWorkers(*clients, [&](Http &http) { Work(http, progress); });
        progress.Finish();
    }
    return Finish(input->ids);
}

} // namespace

RunStatus RunResolve(const CommandOptions &options,
                     const std::atomic<bool> &stop) {
    return Resolver(options, stop).Run();
}

} // namespace khanbot
