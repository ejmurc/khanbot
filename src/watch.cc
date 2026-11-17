#include "src/watch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <glaze/json.hpp>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
constexpr double kSpeed = 2.0;
constexpr std::chrono::seconds kTick{20};
constexpr std::chrono::seconds kMaxElapsed{30};

constexpr char kKaasEnv[] = "KAAS";
constexpr char kKaasCookiePrefix[] = "KAAS=";
constexpr char kInputName[] = "content_ids.tsv";
constexpr char kJournalName[] = "watch.journal";
constexpr char kFailureDumpName[] = "watch-failed-response.txt";
constexpr std::string_view kDoneMarker = "done";

constexpr std::string_view kSafelistUrl =
    "https://cdn.jsdelivr.net/gh/bhavjitChauhan/khan-api@safelist";
constexpr std::string_view kSafelistKinds[] = {"query", "mutation"};
constexpr std::string_view kMutationOperation = "updateUserVideoProgress";
constexpr const char *kPostHeaders[] = {"content-type: application/json",
                                        "x-ka-fkey: 1"};

constexpr glz::opts kReadOptions{.error_on_unknown_keys = false};

struct GraphqlError {
    std::string message;

    struct glaze {
        using T = GraphqlError;
        static constexpr auto value = glz::object("message", &T::message);
    };
};

struct ItemProgress {
    bool completed = false;

    struct glaze {
        using T = ItemProgress;
        static constexpr auto value = glz::object("completed", &T::completed);
    };
};

struct UpdateError {
    std::optional<std::string> code;
    std::optional<std::string> debug_message;

    struct glaze {
        using T = UpdateError;
        static constexpr auto value =
            glz::object("code", &T::code, "debugMessage", &T::debug_message);
    };
};

struct Update {
    std::optional<ItemProgress> item_progress;
    std::optional<UpdateError> error;

    struct glaze {
        using T = Update;
        static constexpr auto value = glz::object(
            "videoItemProgress", &T::item_progress, "error", &T::error);
    };
};

struct UpdateData {
    std::optional<Update> update;

    struct glaze {
        using T = UpdateData;
        static constexpr auto value =
            glz::object("updateUserVideoProgress", &T::update);
    };
};

struct UpdateReply {
    std::optional<UpdateData> data;
    std::optional<std::vector<GraphqlError>> errors;

    struct glaze {
        using T = UpdateReply;
        static constexpr auto value =
            glz::object("data", &T::data, "errors", &T::errors);
    };
};

struct ProgressInput {
    std::string content_id;
    double seconds_watched;
    double last_second_watched;
    long duration_seconds;
    std::string captions_ka_locale;
    bool fallback_player;
    long local_timezone_offset_seconds;

    struct glaze {
        using T = ProgressInput;
        static constexpr auto value = glz::object(
            "contentId", &T::content_id, "secondsWatched", &T::seconds_watched,
            "lastSecondWatched", &T::last_second_watched, "durationSeconds",
            &T::duration_seconds, "captionsKaLocale", &T::captions_ka_locale,
            "fallbackPlayer", &T::fallback_player, "localTimezoneOffsetSeconds",
            &T::local_timezone_offset_seconds);
    };
};

struct ProgressVariables {
    ProgressInput input;

    struct glaze {
        using T = ProgressVariables;
        static constexpr auto value = glz::object("input", &T::input);
    };
};

template <typename Variables> struct Request {
    std::string_view operation_name;
    Variables variables;
    std::string_view query;

    struct glaze {
        using T = Request<Variables>;
        static constexpr auto value =
            glz::object("operationName", &T::operation_name, "variables",
                        &T::variables, "query", &T::query);
    };
};

struct Document {
    std::string text;
    int version = 0;
    std::uint32_t hash = 0;
};

struct Video {
    std::string content_id;
    long duration = 0;
};

enum class Stage {
    kStart,
    kWatch,
};

std::chrono::milliseconds GetJitteredDelay(int min_ms, int max_ms) {
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<int> distribution(min_ms, max_ms);
    return std::chrono::milliseconds(distribution(generator));
}

double Seconds(Clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

long LocalOffsetSeconds() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return static_cast<long>(local.tm_gmtoff);
}

bool IsSafelistMiss(const Response &response) {
    return response.body.contains("not found in safelist");
}

bool IsValidJournalLine(std::string_view line) {
    const std::size_t tab = line.find('\t');
    return tab != kNpos && IsContentId(line.substr(0, tab)) &&
           line.substr(tab + 1) == kDoneMarker;
}

std::string ApiUrl(const std::string &base_url, std::string_view operation) {
    std::string url = base_url + "/api/internal/graphql/";
    url.append(operation).append("?lang=en&app=khanacademy");
    return url;
}

template <typename Variables>
std::string Body(std::string_view operation, const Variables &variables,
                 std::string_view query) {
    const Request<Variables> request{operation, variables, query};
    return glz::write_json(request).value_or(std::string());
}

template <typename Reply> Result<Reply> ParseReply(const std::string &body) {
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
    return reply;
}

Result<std::vector<Video>> LoadVideos(const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in) {
        return std::unexpected("cannot read " + path.string() +
                               "; run the resolve command first");
    }
    std::vector<Video> videos;
    std::unordered_set<std::string> seen;
    std::size_t skipped = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        std::optional<Entry> entry = ParseEntry(line);
        if (!entry || !entry->lookup.found()) {
            ++skipped;
            continue;
        }
        if (!seen.insert(entry->lookup.content_id).second)
            continue;
        videos.push_back(
            {std::move(entry->lookup.content_id), entry->lookup.duration});
    }
    if (skipped > 0) {
        std::cerr << std::format(
            "skipped {} rows without a content ID and duration in {}\n",
            FormatCount(skipped), path.string());
    }
    if (videos.empty()) {
        return std::unexpected("no videos with durations in " + path.string() +
                               "; run the resolve command again");
    }
    return videos;
}

class Safelist {
  public:
    explicit Safelist(std::unique_ptr<Http> http) : http_(std::move(http)) {}

    Result<std::shared_ptr<const Document>> Get(const std::string &operation,
                                                int stale = 0) {
        std::shared_ptr<const Document> &held = documents_[operation];
        if ((held ? held->version : 0) > stale)
            return held;
        Result<std::string> text = Download(operation);
        if (!text)
            return std::unexpected(std::move(text.error()));
        const std::uint32_t hash = HashDocument(*text);
        held = std::make_shared<const Document>(
            Document{std::move(*text), stale + 1, hash});
        return held;
    }

  private:
    Result<std::string> Download(const std::string &operation) {
        for (const std::string_view kind : kSafelistKinds) {
            const std::string url =
                std::format("{}/{}/{}", kSafelistUrl, kind, operation);
            Result<Response> response = http_->Get(url);
            if (!response)
                return std::unexpected("safelist: " + response.error());
            if (response->status == kHttpOk)
                return std::move(response->body);
            if (response->status != kHttpNotFound) {
                return std::unexpected("safelist: " +
                                       DescribeResponse(*response));
            }
        }
        return std::unexpected("safelist has no " + operation);
    }

    std::unique_ptr<Http> http_;
    std::unordered_map<std::string, std::shared_ptr<const Document>> documents_;
};

template <typename Reply, typename Variables>
Result<Reply, FetchFailure>
Execute(Http &http, Safelist &safelist, const std::string &url,
        const std::string &operation, const Variables &variables,
        const std::atomic<bool> &stop) {
    int stale = 0;
    bool refreshed = false;
    return Retry<Reply>(stop, [&]() -> Result<Reply, FetchFailure> {
        const auto document = safelist.Get(operation, stale);
        if (!document)
            return Failed(document.error());
        const std::string body = Body(operation, variables, (*document)->text);
        Result<Response> sent = http.Post(url, body, kPostHeaders);
        if (!sent)
            return Failed(std::move(sent.error()));
        Response &response = *sent;

        const auto refresh = [&](std::string message) {
            refreshed = true;
            stale = (*document)->version;
            FetchFailure failure =
                MakeFailure(std::move(message), std::move(response));
            failure.retry_now = true;
            return std::unexpected(std::move(failure));
        };

        if (IsSafelistMiss(response)) {
            std::string message =
                std::format("operation {} is not in KA's safelist: {}",
                            operation, DescribeResponse(response));
            if (!refreshed)
                return refresh(std::move(message));
            return Rejected(
                std::move(message), std::move(response),
                std::format(
                    "KA rejected the {} document (hash {}) as not safelisted, "
                    "even after re-downloading it. The safelist copy is stale "
                    "or the operation no longer exists in KA's frontend; "
                    "regenerate the document from KA's current JS bundles",
                    operation, (*document)->hash));
        }
        if (response.status == kHttpUnauthorized ||
            response.status == kHttpForbidden) {
            std::string message = DescribeResponse(response);
            return Rejected(std::move(message), std::move(response));
        }
        if (response.status == kHttpBadRequest && !refreshed)
            return refresh(DescribeResponse(response));
        if (response.status == kHttpOk) {
            Result<Reply> reply = ParseReply<Reply>(response.body);
            if (reply)
                return std::move(*reply);
            return Failed(std::move(reply.error()), std::move(response));
        }
        std::string message = DescribeResponse(response);
        return Failed(std::move(message), std::move(response));
    });
}

struct Session {
    Http &http;
    Safelist &safelist;
    Progress &progress;
    const std::string url;
};

class Watcher {
  public:
    Watcher(const CommandOptions &options, const std::atomic<bool> &stop)
        : options_(options), stop_(stop) {}

    RunStatus Run();

  private:
    void WatchVideo(const Video &video, Session &session);
    void Complete(const std::string &content_id, bool watched,
                  Session &session);
    void Fail(const std::string &content_id, FetchFailure failure,
              Session &session);
    RunStatus Finish(std::size_t total);

    const CommandOptions options_;
    const std::atomic<bool> &stop_;
    std::unique_ptr<Journal> journal_;
    std::vector<Video> pending_;
    std::size_t watched_ = 0;
    std::size_t skipped_ = 0;
    FailureLog log_;
};

void Watcher::Complete(const std::string &content_id, bool watched,
                       Session &session) {
    if (!journal_->Append(content_id + '\t' + std::string(kDoneMarker))) {
        log_.Abort("cannot write " + std::string(kJournalName));
        return;
    }
    log_.Succeeded();
    ++(watched ? watched_ : skipped_);
    session.progress.Advance(watched ? 1 : 0);
}

void Watcher::Fail(const std::string &content_id, FetchFailure failure,
                   Session &session) {
    if (stop_.load())
        return;
    session.progress.Fail();
    if (failure.rejected) {
        log_.Abort([&] {
            const std::filesystem::path dump =
                options_.data_dir / kFailureDumpName;
            WriteResponseDump(dump, session.url, failure.response);
            std::string reason = failure.abort_hint;
            if (reason.empty()) {
                reason = std::format(
                    "HTTP {}: {} may have expired, or the site is challenging "
                    "requests",
                    failure.response.status, kKaasEnv);
            }
            return std::format("{}\nsaved the full response to {}", reason,
                               dump.string());
        });
        return;
    }
    log_.Record(content_id, std::move(failure.message));
}

void Watcher::WatchVideo(const Video &video, Session &session) {
    const double duration = static_cast<double>(video.duration);
    Stage stage = Stage::kStart;
    double position = 0;
    Clock::time_point due = Clock::now();
    Clock::time_point last_sent;

    while (WaitUntil(stop_, due)) {
        const Clock::time_point now = Clock::now();
        const double remaining = duration - position;
        double watched = 0;
        if (stage == Stage::kWatch) {
            const double elapsed =
                std::min(Seconds(now - last_sent), Seconds(kMaxElapsed));
            watched = std::min(elapsed * kSpeed, remaining);
        }
        const bool last = stage == Stage::kWatch && watched >= remaining;
        const double new_position = last ? duration : position + watched;

        const ProgressVariables variables{
            {.content_id = video.content_id,
             .seconds_watched = watched,
             .last_second_watched = new_position,
             .duration_seconds = std::lround(duration),
             .captions_ka_locale = "en",
             .fallback_player = false,
             .local_timezone_offset_seconds = LocalOffsetSeconds()}};
        Result<UpdateReply, FetchFailure> reply = Execute<UpdateReply>(
            session.http, session.safelist, session.url,
            std::string(kMutationOperation), variables, stop_);
        if (!reply)
            return Fail(video.content_id, std::move(reply.error()), session);

        const std::optional<Update> update =
            reply->data.and_then(&UpdateData::update);
        if (!update || update->error || !update->item_progress) {
            std::string message = "response has no progress";
            if (update && update->error) {
                message = std::format(
                    "{}: {}", update->error->code.value_or("unknown error"),
                    update->error->debug_message.value_or(""));
            }
            return Fail(video.content_id, MakeFailure(std::move(message)),
                        session);
        }

        position = new_position;
        last_sent = now;
        const bool completed = update->item_progress->completed;
        if (stage == Stage::kStart && completed)
            return Complete(video.content_id, false, session);
        if (last) {
            if (completed)
                return Complete(video.content_id, true, session);
            return Fail(video.content_id,
                        MakeFailure("server did not mark the video completed"),
                        session);
        }
        const auto jitter = GetJitteredDelay(0, 2000);
        due = stage == Stage::kStart ? now + kTick + jitter
                                     : std::max(due + kTick + jitter, now);
        stage = Stage::kWatch;
    }
}

RunStatus Watcher::Finish(std::size_t total) {
    const std::size_t completed = total - pending_.size() + watched_ + skipped_;
    const std::string details = std::format(
        "  Completed  {} of {} videos\n"
        "  Watched    {} videos this run\n"
        "  Journal    {}",
        FormatCount(completed), FormatCount(total), FormatCount(watched_),
        (options_.data_dir / kJournalName).string());
    return Report("Watch", "videos", stop_.load(), log_, details);
}

RunStatus Watcher::Run() {
    const char *kaas = std::getenv(kKaasEnv);
    if (kaas == nullptr || *kaas == '\0')
        return Fatal(std::string(kKaasEnv) + " is not set");

    const Result<std::vector<Video>> videos =
        LoadVideos(options_.data_dir / kInputName);
    if (!videos)
        return Fatal(videos.error());

    Result<std::unique_ptr<Journal>> journal = Journal::Open(
        options_.data_dir / kJournalName, options_.fresh, IsValidJournalLine);
    if (!journal)
        return Fatal(journal.error());
    journal_ = std::move(*journal);

    std::ranges::copy_if(*videos, std::back_inserter(pending_),
                         [&](const Video &video) {
                             return !journal_->Contains(video.content_id);
                         });
    std::cerr << std::format("Videos: {}, {} already watched, {} to watch\n",
                             FormatCount(videos->size()),
                             FormatCount(videos->size() - pending_.size()),
                             FormatCount(pending_.size()));

    if (!pending_.empty()) {
        Result<std::unique_ptr<Http>> api =
            Http::Create(kKaasCookiePrefix + std::string(kaas));
        if (!api)
            return Fatal(api.error());
        Result<std::unique_ptr<Http>> cdn = Http::Create();
        if (!cdn)
            return Fatal(cdn.error());
        Safelist safelist(std::move(*cdn));
        Progress progress(Progress::Options{
            .title = "Watching videos",
            .extra_label = "watched",
            .total = videos->size(),
            .completed = videos->size() - pending_.size(),
            .extra = 0,
        });
        Session session{**api, safelist, progress,
                        ApiUrl(options_.base_url, kMutationOperation)};
        for (const Video &video : pending_) {
            if (stop_.load() || log_.aborted())
                break;
            WatchVideo(video, session);
        }
        progress.Finish();
    }
    return Finish(videos->size());
}

} // namespace

RunStatus RunWatch(const CommandOptions &options,
                   const std::atomic<bool> &stop) {
    return Watcher(options, stop).Run();
}

} // namespace khanbot
