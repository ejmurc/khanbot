#include "src/resolve.h"

#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <glaze/json.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
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
constexpr int kHttpOk = 200;
constexpr int kHttpForbidden = 403;
constexpr std::size_t kYoutubeIdLength = 11;
constexpr std::size_t kNpos = std::string_view::npos;
constexpr long kMinDurationSeconds = 1;
constexpr double kMaxDurationSeconds = 1.0e6;
constexpr std::chrono::milliseconds kRequestDelay{150};
constexpr std::chrono::milliseconds kRetryBackoff{500};

constexpr char kJournalName[] = "resolve.journal";
constexpr char kYoutubeIdsName[] = "youtube_ids.txt";
constexpr char kContentIdsName[] = "content_ids.tsv";
constexpr char kNotFoundName[] = "not_found.txt";
constexpr char kFailureDumpName[] = "resolve-failed-response.txt";

constexpr std::string_view kOperation = "videoByYoutubeId";
constexpr std::string_view kVariablesPrefix = "%7B%22youtubeId%22%3A%22";
constexpr std::string_view kVariablesSuffix = "%22%7D";
// Verbatim copy of query/videoByYoutubeId on the khan-api `safelist` branch
// (781 bytes, no trailing newline). KA looks the document up by its hash, so
// every byte must match.
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

constexpr std::uint32_t HashDocument(std::string_view document) {
    std::uint32_t hash = 5381;
    for (std::size_t i = document.size(); i > 0; --i) {
        hash = hash * 33u ^ static_cast<std::uint32_t>(
                                static_cast<unsigned char>(document[i - 1]));
    }
    return hash;
}

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

// What KA knows about one YouTube ID. An empty content_id means KA has no such
// video.
struct Lookup {
    std::string content_id;
    long duration = 0;

    bool found() const { return !content_id.empty(); }
};

struct Entry {
    std::string youtube_id;
    Lookup lookup;
};

struct YoutubeIds {
    std::vector<std::string> ids;
    std::size_t invalid = 0;
};

struct FetchFailure {
    std::string message;
    Response response;
    bool rejected = false;
};

struct ExportStats {
    std::size_t resolved = 0;
    std::size_t not_found = 0;
};

bool IsYoutubeId(std::string_view text) {
    if (text.size() != kYoutubeIdLength)
        return false;
    for (const char c : text) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' &&
            c != '-') {
            return false;
        }
    }
    return true;
}

bool IsContentId(std::string_view text) {
    if (text.empty())
        return false;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) ||
            std::iscntrl(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

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

// youtube_id <TAB> content_id <TAB> duration_seconds. A video KA does not know
// is recorded as "youtube_id<TAB><TAB>". This is also the row format of
// content_ids.tsv, which the watch command reads.
std::string FormatJournalLine(std::string_view youtube_id,
                              const Lookup &lookup) {
    std::string line(youtube_id);
    line += '\t';
    line.append(lookup.content_id);
    line += '\t';
    if (lookup.found())
        line.append(std::to_string(lookup.duration));
    return line;
}

std::optional<Entry> ParseJournalLine(std::string_view line) {
    const std::size_t first = line.find('\t');
    if (first == kNpos)
        return std::nullopt;
    const std::size_t second = line.find('\t', first + 1);
    if (second == kNpos || line.find('\t', second + 1) != kNpos) {
        return std::nullopt;
    }
    const std::string_view youtube_id = line.substr(0, first);
    const std::string_view content_id =
        line.substr(first + 1, second - first - 1);
    const std::string_view duration = line.substr(second + 1);
    if (!IsYoutubeId(youtube_id))
        return std::nullopt;
    Entry entry{std::string(youtube_id), {}};
    if (content_id.empty()) {
        if (!duration.empty())
            return std::nullopt;
        return entry;
    }
    long seconds = 0;
    const auto parsed = std::from_chars(
        duration.data(), duration.data() + duration.size(), seconds);
    if (!IsContentId(content_id) || parsed.ec != std::errc() ||
        parsed.ptr != duration.data() + duration.size() ||
        seconds < kMinDurationSeconds) {
        return std::nullopt;
    }
    entry.lookup = Lookup{std::string(content_id), seconds};
    return entry;
}

bool IsValidJournalLine(std::string_view line) {
    return ParseJournalLine(line).has_value();
}

std::unordered_map<std::string, Lookup> LoadEntries(const Journal &journal) {
    std::unordered_map<std::string, Lookup> entries;
    for (const std::string &line : journal.Snapshot()) {
        std::optional<Entry> entry = ParseJournalLine(line);
        if (entry.has_value()) {
            entries[entry->youtube_id] = std::move(entry->lookup);
        }
    }
    return entries;
}

std::expected<YoutubeIds, std::string>
LoadYoutubeIds(const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in) {
        return std::unexpected("cannot read " + path.string() +
                               "; run the crawl command first");
    }
    std::set<std::string> unique;
    YoutubeIds result;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (IsYoutubeId(line)) {
            unique.insert(line);
        } else {
            ++result.invalid;
        }
    }
    if (unique.empty()) {
        return std::unexpected("no valid YouTube IDs in " + path.string());
    }
    result.ids.assign(unique.begin(), unique.end());
    return result;
}

std::expected<Lookup, std::string> ExtractLookup(const std::string &body,
                                                 std::string_view youtube_id) {
    Reply reply;
    const glz::error_ctx error = glz::read<kReadOptions>(reply, body);
    if (error) {
        return std::unexpected("malformed response: " +
                               glz::format_error(error, body));
    }
    if (reply.errors.has_value() && !reply.errors->empty()) {
        return std::unexpected("graphql error: " +
                               reply.errors->front().message);
    }
    if (!reply.data.has_value()) {
        return std::unexpected(std::string("response has no data"));
    }
    if (!reply.data->video.has_value())
        return Lookup{};
    const Video &video = *reply.data->video;
    if (video.youtube_id != youtube_id) {
        return std::unexpected("response is for video '" + video.youtube_id +
                               "'");
    }
    if (!IsContentId(video.id)) {
        return std::unexpected(std::string("response has no usable id"));
    }
    if (!video.duration.has_value() || !(*video.duration >= 1.0) ||
        *video.duration > kMaxDurationSeconds) {
        return std::unexpected(std::string("response has no usable duration"));
    }
    return Lookup{video.id, std::lround(*video.duration)};
}

std::expected<Lookup, FetchFailure> FetchLookup(Http &http,
                                                const std::string &url,
                                                std::string_view youtube_id,
                                                const std::atomic<bool> &stop) {
    FetchFailure failure;
    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
        if (stop.load()) {
            failure.message = "interrupted";
            return std::unexpected(std::move(failure));
        }
        std::expected<Response, std::string> result = http.Get(url);
        if (!result.has_value()) {
            failure.message = result.error();
        } else {
            failure.response = std::move(*result);
            if (failure.response.status == kHttpForbidden) {
                failure.rejected = true;
                failure.message = DescribeResponse(failure.response);
                return std::unexpected(std::move(failure));
            }
            if (failure.response.status == kHttpOk) {
                std::expected<Lookup, std::string> lookup =
                    ExtractLookup(failure.response.body, youtube_id);
                if (lookup.has_value())
                    return std::move(*lookup);
                failure.message = std::move(lookup.error());
            } else {
                failure.message = DescribeResponse(failure.response);
            }
        }
        if (attempt < kAttempts) {
            std::this_thread::sleep_for(kRetryBackoff * attempt);
        }
    }
    failure.message =
        "after " + std::to_string(kAttempts) + " attempts: " + failure.message;
    return std::unexpected(std::move(failure));
}

std::expected<ExportStats, std::string>
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
            resolved.push_back(FormatJournalLine(id, entry->second));
        } else {
            not_found.push_back(id);
        }
    }
    if (auto written =
            WriteLinesAtomically(directory / kContentIdsName, resolved);
        !written.has_value()) {
        return std::unexpected(written.error());
    }
    if (auto written =
            WriteLinesAtomically(directory / kNotFoundName, not_found);
        !written.has_value()) {
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
    struct Failed {
        std::string youtube_id;
        std::string message;
    };

    void Work(Http &http, Progress &progress);
    void RecordFailure(const std::string &youtube_id, std::string message,
                       Progress &progress);
    void RejectQuery(const std::string &url, const FetchFailure &failure);
    void Abort(std::string reason);
    RunStatus Finish(const std::vector<std::string> &ids);
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

RunStatus Resolver::Fatal(const std::string &message) const {
    std::cerr << "error: " << message << '\n';
    return RunStatus::kFatal;
}

void Resolver::Abort(std::string reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (abort_reason_.empty())
        abort_reason_ = std::move(reason);
    abort_.store(true);
}

void Resolver::RecordFailure(const std::string &youtube_id, std::string message,
                             Progress &progress) {
    progress.Fail();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failures_.push_back({youtube_id, std::move(message)});
    }
    if (consecutive_failures_.fetch_add(1) + 1 >= kMaxConsecutiveFailures) {
        Abort(std::to_string(kMaxConsecutiveFailures) +
              " consecutive failures; the site may be challenging requests");
    }
}

void Resolver::RejectQuery(const std::string &url,
                           const FetchFailure &failure) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!abort_reason_.empty())
        return;
    const std::filesystem::path dump = options_.data_dir / kFailureDumpName;
    WriteResponseDump(dump, url, failure.response);
    abort_reason_ =
        "HTTP 403: the safelisted query may be stale, or the site is "
        "challenging requests\nsaved the full response to " +
        dump.string();
    abort_.store(true);
}

void Resolver::Work(Http &http, Progress &progress) {
    while (!stop_.load() && !abort_.load()) {
        const std::size_t index = next_.fetch_add(1);
        if (index >= pending_.size())
            return;
        const std::string &youtube_id = pending_[index];
        const std::string url = RequestUrl(options_.base_url, youtube_id);
        std::expected<Lookup, FetchFailure> lookup =
            FetchLookup(http, url, youtube_id, stop_);
        if (!lookup.has_value()) {
            if (stop_.load())
                return;
            if (lookup.error().rejected) {
                RejectQuery(url, lookup.error());
                return;
            }
            RecordFailure(youtube_id, std::move(lookup.error().message),
                          progress);
            continue;
        }
        if (!journal_->Append(FormatJournalLine(youtube_id, *lookup))) {
            Abort("cannot write " + std::string(kJournalName));
            return;
        }
        consecutive_failures_.store(0);
        progress.Advance(lookup->found() ? 1 : 0);
        std::this_thread::sleep_for(kRequestDelay);
    }
}

RunStatus Resolver::Finish(const std::vector<std::string> &ids) {
    const std::expected<ExportStats, std::string> exported =
        ExportResults(LoadEntries(*journal_), ids, options_.data_dir);
    if (!exported.has_value())
        return Fatal(exported.error());

    const bool interrupted = stop_.load();
    const bool incomplete = abort_.load() || !failures_.empty();

    std::cout << (interrupted  ? "Resolve interrupted"
                  : incomplete ? "Resolve incomplete"
                               : "Resolve complete")
              << '\n'
              << "  Resolved   " << FormatCount(exported->resolved) << " of "
              << FormatCount(ids.size()) << " YouTube IDs\n"
              << "  Not found  " << FormatCount(exported->not_found)
              << " YouTube IDs unknown to Khan Academy\n"
              << "  Output     "
              << (options_.data_dir / kContentIdsName).string()
              << "\n             "
              << (options_.data_dir / kNotFoundName).string() << '\n';

    int shown = 0;
    for (const Failed &failed : failures_) {
        if (shown++ == kMaxFailuresShown) {
            std::cerr << "  ... and "
                      << FormatCount(failures_.size() - kMaxFailuresShown)
                      << " more\n";
            break;
        }
        std::cerr << "  failed  " << failed.youtube_id << "  " << failed.message
                  << '\n';
    }
    if (abort_.load())
        std::cerr << "Stopped early: " << abort_reason_ << '\n';
    if (interrupted) {
        std::cerr << "Progress is saved; run again to resume.\n";
        return RunStatus::kInterrupted;
    }
    if (incomplete) {
        std::cerr
            << "Run again to retry the YouTube IDs that did not finish.\n";
        return RunStatus::kIncomplete;
    }
    return RunStatus::kComplete;
}

RunStatus Resolver::Run() {
    std::error_code error;
    std::filesystem::create_directories(options_.data_dir, error);
    if (error) {
        return Fatal("cannot create " + options_.data_dir.string() + ": " +
                     error.message());
    }

    const std::expected<YoutubeIds, std::string> input =
        LoadYoutubeIds(options_.data_dir / kYoutubeIdsName);
    if (!input.has_value())
        return Fatal(input.error());

    std::expected<std::unique_ptr<Journal>, std::string> journal =
        Journal::Open(options_.data_dir / kJournalName, options_.fresh,
                      IsValidJournalLine);
    if (!journal.has_value())
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
    std::cerr << "YouTube IDs: " << FormatCount(input->ids.size()) << ", "
              << FormatCount(input->ids.size() - pending_.size())
              << " already looked up, " << FormatCount(pending_.size())
              << " to fetch\n";
    if (input->invalid > 0) {
        std::cerr << "  skipped " << FormatCount(input->invalid)
                  << " malformed lines in " << kYoutubeIdsName << '\n';
    }

    if (!pending_.empty()) {
        std::vector<std::unique_ptr<Http>> clients;
        for (int i = 0; i < kWorkers; ++i) {
            std::expected<std::unique_ptr<Http>, std::string> client =
                Http::Create();
            if (!client.has_value())
                return Fatal(client.error());
            clients.push_back(std::move(*client));
        }
        Progress progress(Progress::Options{
            .title = "Resolving videos",
            .extra_label = "resolved",
            .total = input->ids.size(),
            .completed = input->ids.size() - pending_.size(),
            .extra = known_resolved,
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
    return Finish(input->ids);
}

} // namespace

RunStatus RunResolve(const CommandOptions &options,
                     const std::atomic<bool> &stop) {
    return Resolver(options, stop).Run();
}

} // namespace khanbot
