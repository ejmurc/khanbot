#include "src/watch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <filesystem>
#include <fstream>
#include <glaze/json.hpp>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/http.h"
#include "src/journal.h"
#include "src/progress.h"

namespace khanbot {
namespace {

using Clock = std::chrono::steady_clock;

std::chrono::milliseconds GetJitteredDelay(int min_ms, int max_ms) {
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<int> distribution(min_ms, max_ms);
    return std::chrono::milliseconds(distribution(generator));
}

constexpr int kAttempts = 3;
constexpr int kMaxConsecutiveFailures = 5;
constexpr int kMaxFailuresShown = 10;
constexpr int kHttpOk = 200;
constexpr int kHttpBadRequest = 400;
constexpr int kHttpUnauthorized = 401;
constexpr int kHttpForbidden = 403;
constexpr int kHttpNotFound = 404;
constexpr double kSpeed = 2.0;
constexpr std::chrono::seconds kTick{20};
constexpr std::chrono::seconds kMaxElapsed{30};
constexpr std::chrono::milliseconds kRetryBackoff{500};
constexpr std::chrono::milliseconds kPoll{100};
constexpr std::size_t kNpos = std::string_view::npos;

constexpr char kKaasEnv[] = "KA_KAAS";
constexpr char kInputName[] = "content_ids.tsv";
constexpr char kJournalName[] = "watch.journal";
constexpr char kFailureDumpName[] = "watch-failed-response.txt";
constexpr char kDebugEnv[] = "KA_DEBUG";
constexpr char kImpersonateEnv[] = "KA_IMPERSONATE";
constexpr char kDebugDirName[] = "watch-debug";
constexpr char kDebugLogName[] = "watch.log";
constexpr std::size_t kLogPreview = 600;
constexpr int kMaxDumpsPerKind = 3;
constexpr std::string_view kDoneMarker = "done";

constexpr std::string_view kSafelistUrl =
    "https://cdn.jsdelivr.net/gh/bhavjitChauhan/khan-api@safelist";
constexpr std::string_view kSafelistKinds[] = {"query", "mutation"};
constexpr std::string_view kMutationOperation = "updateUserVideoProgress";

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
};

enum class Stage {
    kStart,
    kWatch,
};

struct Video {
    std::string content_id;
    long duration = 0;
};

struct Task {
    std::string content_id;
    Stage stage = Stage::kStart;
    double duration = 0;
    double position = 0;
    Clock::time_point due;
    Clock::time_point last_sent;
};

struct Later {
    bool operator()(const Task &a, const Task &b) const {
        return a.due > b.due;
    }
};

struct StepFailure {
    std::string message;
    Response response;
    bool rejected = false;
    std::string abort_hint;
};

StepFailure Failure(std::string message) {
    StepFailure failure;
    failure.message = std::move(message);
    return failure;
}

struct Failed {
    std::string content_id;
    std::string message;
};

long long ElapsedMs(Clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                 since)
        .count();
}

class DebugLog {
  public:
    bool enabled() const { return enabled_; }

    void Open(const std::filesystem::path &dir) {
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        if (error) {
            std::cerr << "warning: cannot create " << dir.string() << ": "
                      << error.message() << '\n';
            return;
        }
        dir_ = dir;
        start_ = Clock::now();
        out_.open(dir / kDebugLogName, std::ios::binary | std::ios::trunc);
        enabled_ = static_cast<bool>(out_);
        if (enabled_) {
            std::cerr << "debug log: " << (dir / kDebugLogName).string()
                      << '\n';
        }
    }

    void Log(const std::string &message) {
        if (!enabled_)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        out_ << '[' << std::setw(8) << ElapsedMs(start_) << " ms] " << message
             << '\n';
        out_.flush();
    }

    void Dump(const std::string &name, const std::string &content) {
        if (!enabled_)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream(dir_ / name, std::ios::binary | std::ios::trunc)
            << content;
    }

    std::string DumpLimited(const std::string &kind,
                            const std::string &content) {
        if (!enabled_)
            return {};
        std::lock_guard<std::mutex> lock(mutex_);
        int &count = dump_counts_[kind];
        if (count >= kMaxDumpsPerKind)
            return {};
        const std::filesystem::path path =
            dir_ / (kind + "-" + std::to_string(++count) + ".txt");
        std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
        return path.string();
    }

  private:
    bool enabled_ = false;
    Clock::time_point start_ = Clock::now();
    std::filesystem::path dir_;
    std::ofstream out_;
    std::mutex mutex_;
    std::unordered_map<std::string, int> dump_counts_;
};

DebugLog &Debug() {
    static DebugLog log;
    return log;
}

std::string Preview(std::string_view text, std::size_t limit = kLogPreview) {
    std::string out(text.substr(0, limit));
    std::replace_if(
        out.begin(), out.end(),
        [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
    if (text.size() > limit) {
        out += "... [" + std::to_string(text.size()) + " bytes total]";
    }
    return out;
}

std::string LowerAscii(std::string_view text) {
    std::string out(text);
    for (char &c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string SanitizeHeaders(const std::string &headers) {
    std::string out;
    std::size_t start = 0;
    while (start < headers.size()) {
        std::size_t end = headers.find('\n', start);
        if (end == kNpos)
            end = headers.size();
        std::string line = headers.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (LowerAscii(line).starts_with("set-cookie:")) {
            const std::size_t equals = line.find('=');
            if (equals != kNpos)
                line = line.substr(0, equals + 1) + "<redacted>";
        }
        out += "    " + line + "\n";
    }
    return out;
}

std::string Redact(std::string_view secret) {
    if (secret.empty())
        return "(empty)";
    return std::to_string(secret.size()) + " chars, starts \"" +
           std::string(secret.substr(0, 3)) + "\"";
}

std::string DescribeExchange(const std::string &url,
                             const std::string &request_body,
                             const Response &response) {
    std::string out;
    out += "url: " + url + "\n";
    out += "request body (" + std::to_string(request_body.size()) +
           " bytes):\n" + request_body + "\n\n";
    out += "status: " + std::to_string(response.status) + "\n";
    out += "effective url: " + response.effective_url + "\n";
    out += "response headers:\n" + SanitizeHeaders(response.headers) + "\n";
    out += "response body (" + std::to_string(response.body.size()) +
           " bytes):\n" + response.body + "\n";
    return out;
}

std::uint32_t HashDocument(std::string_view text) {
    std::uint32_t hash = 5381;
    for (auto it = text.rbegin(); it != text.rend(); ++it) {
        hash = (hash * 33U) ^
               static_cast<std::uint32_t>(static_cast<unsigned char>(*it));
    }
    return hash;
}

const char *StageName(Stage stage) {
    switch (stage) {
    case Stage::kStart:
        return "start";
    case Stage::kWatch:
        return "watch";
    }
    return "?";
}

bool IsSafelistMiss(const Response &response) {
    return response.body.find("not found in safelist") != kNpos;
}

bool IsContentId(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0;
    });
}

bool IsValidJournalLine(std::string_view line) {
    const std::size_t tab = line.find('\t');
    return tab != kNpos && IsContentId(line.substr(0, tab)) &&
           line.substr(tab + 1) == kDoneMarker;
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

std::expected<std::vector<Video>, std::string>
LoadVideos(const std::filesystem::path &path) {
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
        const std::size_t first = line.find('\t');
        const std::size_t second =
            first == kNpos ? kNpos : line.find('\t', first + 1);
        if (second == kNpos || line.find('\t', second + 1) != kNpos) {
            ++skipped;
            continue;
        }
        std::string id = line.substr(first + 1, second - first - 1);
        const std::string_view duration =
            std::string_view(line).substr(second + 1);
        long seconds = 0;
        const auto parsed = std::from_chars(
            duration.data(), duration.data() + duration.size(), seconds);
        if (!IsContentId(id) || parsed.ec != std::errc() ||
            parsed.ptr != duration.data() + duration.size() || seconds < 1) {
            ++skipped;
            continue;
        }
        if (seen.insert(id).second)
            videos.push_back({std::move(id), seconds});
    }
    if (skipped > 0) {
        std::cerr << "skipped " << FormatCount(skipped) << " rows without a "
                  << "content ID and duration in " << path.string() << '\n';
    }
    if (videos.empty()) {
        return std::unexpected("no videos with durations in " + path.string() +
                               "; run the resolve command again");
    }
    return videos;
}

template <typename Reply>
std::expected<Reply, std::string> ParseReply(const std::string &body) {
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
    return reply;
}

class Safelist {
  public:
    std::expected<Document, std::string>
    Get(Http &http, const std::string &operation, int stale = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        Document &document = documents_[operation];
        if (document.version <= stale) {
            DebugLog &debug = Debug();
            debug.Log("safelist: fetching " + operation + " (held v" +
                      std::to_string(document.version) + ", want newer than v" +
                      std::to_string(stale) + ")");
            std::expected<std::string, std::string> text =
                Download(http, operation);
            if (!text.has_value()) {
                debug.Log("safelist: fetch of " + operation +
                          " failed: " + text.error());
                return std::unexpected(std::move(text.error()));
            }
            if (debug.enabled())
                LogDocument(operation, document, *text);
            document = {std::move(*text), stale + 1};
            debug.Dump(operation + ".v" + std::to_string(document.version) +
                           ".graphql",
                       document.text);
        }
        return document;
    }

  private:
    static void LogDocument(const std::string &operation,
                            const Document &previous, const std::string &text) {
        const std::size_t newline = text.find('\n');
        std::string message =
            "safelist: " + operation + " is " + std::to_string(text.size()) +
            " bytes, hash " + std::to_string(HashDocument(text)) +
            ", trailing newline " +
            (!text.empty() && text.back() == '\n' ? "yes" : "no") +
            ", __typename " +
            (text.find("__typename") != kNpos ? "yes" : "no") +
            "\n    first line: " + Preview(text.substr(0, newline), 200);
        if (previous.version > 0) {
            message += previous.text == text
                           ? "\n    identical to the copy already held (hash "
                             "unchanged)"
                           : "\n    DIFFERENT from the copy already held";
        }
        Debug().Log(message);
    }

    static std::expected<std::string, std::string>
    Download(Http &http, const std::string &operation) {
        DebugLog &debug = Debug();
        for (const std::string_view kind : kSafelistKinds) {
            std::string url(kSafelistUrl);
            url.append("/").append(kind).append("/").append(operation);
            debug.Log("safelist: GET " + url);
            const Clock::time_point began = Clock::now();
            std::expected<Response, std::string> response = http.Get(url);
            if (!response.has_value()) {
                debug.Log("safelist: transport error after " +
                          std::to_string(ElapsedMs(began)) +
                          " ms: " + response.error());
                return std::unexpected("safelist: " + response.error());
            }
            if (debug.enabled()) {
                std::string message =
                    "safelist: status " + std::to_string(response->status) +
                    " in " + std::to_string(ElapsedMs(began)) + " ms, " +
                    std::to_string(response->body.size()) +
                    " bytes, final url " + response->effective_url + "\n" +
                    SanitizeHeaders(response->headers);
                if (response->status != kHttpOk) {
                    message += "    body: " + Preview(response->body);
                }
                debug.Log(message);
            }
            if (response->status == kHttpOk)
                return std::move(response->body);
            if (response->status != kHttpNotFound) {
                return std::unexpected("safelist: " +
                                       DescribeResponse(*response));
            }
        }
        return std::unexpected("safelist has no " + operation);
    }

    std::mutex mutex_;
    std::unordered_map<std::string, Document> documents_;
};

bool WaitFor(const std::atomic<bool> &stop,
             std::chrono::milliseconds duration) {
    std::mutex mutex;
    std::condition_variable ready;
    std::unique_lock<std::mutex> lock(mutex);
    const Clock::time_point deadline = Clock::now() + duration;
    while (!stop.load()) {
        const Clock::time_point now = Clock::now();
        if (now >= deadline)
            return true;
        ready.wait_until(lock, std::min(deadline, now + kPoll));
    }
    return false;
}

template <typename Reply, typename Variables>
std::expected<Reply, StepFailure>
Execute(Http &http, Safelist &safelist, const std::string &url,
        const std::string &operation, const Variables &variables,
        const std::atomic<bool> &stop) {
    DebugLog &debug = Debug();
    StepFailure failure;
    int stale = 0;
    bool refreshed = false;
    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
        if (stop.load()) {
            failure.message = "interrupted";
            return std::unexpected(std::move(failure));
        }
        std::expected<Document, std::string> document =
            safelist.Get(http, operation, stale);
        if (!document.has_value()) {
            failure.message = std::move(document.error());
        } else {
            const std::string body = Body(operation, variables, document->text);
            const std::uint32_t document_hash = HashDocument(document->text);
            if (debug.enabled()) {
                debug.Log("POST " + operation + " attempt " +
                          std::to_string(attempt) + "/" +
                          std::to_string(kAttempts) + ", document v" +
                          std::to_string(document->version) + " hash " +
                          std::to_string(document_hash) + ", body " +
                          std::to_string(body.size()) + " bytes, url " + url);
                if (body.empty()) {
                    debug.Log(
                        "WARNING: JSON serialization produced an empty body");
                }
                debug.DumpLimited("request-" + operation,
                                  url + "\n" + body + "\n");
            }
            const Clock::time_point began = Clock::now();
            std::expected<Response, std::string> result = http.Post(url, body);
            if (!result.has_value()) {
                debug.Log("transport error on " + operation + " after " +
                          std::to_string(ElapsedMs(began)) +
                          " ms: " + result.error());
                failure.message = result.error();
            } else {
                failure.response = std::move(*result);
                const int status = failure.response.status;
                if (debug.enabled()) {
                    std::string message =
                        "response to " + operation + ": status " +
                        std::to_string(status) + " in " +
                        std::to_string(ElapsedMs(began)) + " ms, " +
                        std::to_string(failure.response.body.size()) + " bytes";
                    if (failure.response.effective_url != url) {
                        message +=
                            ", final url " + failure.response.effective_url;
                    }
                    if (status != kHttpOk) {
                        message +=
                            "\n" + SanitizeHeaders(failure.response.headers);
                    }
                    message += "\n    body: " + Preview(failure.response.body);
                    debug.Log(message);
                    if (status != kHttpOk) {
                        const std::string saved = debug.DumpLimited(
                            "response-" + operation + "-" +
                                std::to_string(status),
                            DescribeExchange(url, body, failure.response));
                        if (!saved.empty())
                            debug.Log("saved full exchange to " + saved);
                    }
                }
                if (IsSafelistMiss(failure.response)) {
                    failure.message = "operation " + operation +
                                      " is not in KA's safelist: " +
                                      DescribeResponse(failure.response);
                    if (!refreshed) {
                        refreshed = true;
                        stale = document->version;
                        debug.Log("safelist miss for " + operation +
                                  "; re-downloading the document and retrying");
                        continue;
                    }
                    failure.rejected = true;
                    failure.abort_hint = "KA rejected the " + operation +
                                         " document (hash " +
                                         std::to_string(document_hash) +
                                         ") as not safelisted, even after "
                                         "re-downloading it. The "
                                         "safelist copy is stale or the "
                                         "operation no longer exists in "
                                         "KA's frontend; regenerate the "
                                         "document from KA's current JS "
                                         "bundles";
                    return std::unexpected(std::move(failure));
                }
                if (status == kHttpUnauthorized || status == kHttpForbidden) {
                    failure.rejected = true;
                    failure.message = DescribeResponse(failure.response);
                    return std::unexpected(std::move(failure));
                }
                if (status == kHttpBadRequest && !refreshed) {
                    refreshed = true;
                    stale = document->version;
                    failure.message = DescribeResponse(failure.response);
                    continue;
                }
                if (status == kHttpOk) {
                    std::expected<Reply, std::string> reply =
                        ParseReply<Reply>(failure.response.body);
                    if (reply.has_value())
                        return std::move(*reply);
                    debug.Log("parse failure for " + operation + ": " +
                              reply.error());
                    if (debug.enabled()) {
                        debug.DumpLimited(
                            "parse-" + operation,
                            DescribeExchange(url, body, failure.response));
                    }
                    failure.message = std::move(reply.error());
                } else {
                    failure.message = DescribeResponse(failure.response);
                }
            }
        }
        if (attempt < kAttempts && !WaitFor(stop, kRetryBackoff * attempt)) {
            failure.message = "interrupted";
            return std::unexpected(std::move(failure));
        }
    }
    failure.message =
        "after " + std::to_string(kAttempts) + " attempts: " + failure.message;
    return std::unexpected(std::move(failure));
}

class Watcher {
  public:
    Watcher(const CommandOptions &options, const std::atomic<bool> &stop)
        : options_(options), stop_(stop) {}

    RunStatus Run();

  private:
    void Work(Http &http);
    void Step(Http &http, Task task);
    void Requeue(Task task);
    void Complete(const Task &task, bool watched);
    void Fail(const Task &task, StepFailure failure, const std::string &url);
    void Launch();
    void Abort(std::string reason);
    RunStatus Finish(std::size_t total);
    RunStatus Fatal(const std::string &message) const;

    const CommandOptions options_;
    const std::atomic<bool> &stop_;
    std::unique_ptr<Journal> journal_;
    Safelist safelist_;
    Progress *progress_ = nullptr;
    std::vector<Video> pending_;
    std::atomic<std::size_t> watched_{0};
    std::atomic<std::size_t> skipped_{0};
    std::atomic<int> consecutive_failures_{0};
    std::atomic<bool> abort_{false};
    std::mutex mutex_;
    std::condition_variable ready_;
    std::priority_queue<Task, std::vector<Task>, Later> queue_;
    std::size_t next_ = 0;
    std::size_t live_ = 0;
    std::vector<Failed> failures_;
    std::string abort_reason_;
};

RunStatus Watcher::Fatal(const std::string &message) const {
    std::cerr << "error: " << message << '\n';
    return RunStatus::kFatal;
}

void Watcher::Abort(std::string reason) {
    Debug().Log("ABORT: " + reason);
    if (abort_reason_.empty())
        abort_reason_ = std::move(reason);
    abort_.store(true);
    ready_.notify_all();
}

void Watcher::Launch() {
    if (live_ != 0 || next_ >= pending_.size())
        return;
    Task task;
    const Video &video = pending_[next_++];
    task.content_id = video.content_id;
    task.duration = static_cast<double>(video.duration);
    task.due = Clock::now();
    queue_.push(std::move(task));
    ++live_;
}

void Watcher::Requeue(Task task) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(task));
    ready_.notify_one();
}

void Watcher::Complete(const Task &task, bool watched) {
    Debug().Log("DONE " + task.content_id +
                (watched ? " (watched this run)" : " (already complete)"));
    if (!journal_->Append(task.content_id + '\t' + std::string(kDoneMarker))) {
        std::lock_guard<std::mutex> lock(mutex_);
        Abort("cannot write " + std::string(kJournalName));
        return;
    }
    consecutive_failures_.store(0);
    (watched ? watched_ : skipped_).fetch_add(1);
    progress_->Advance(watched ? 1 : 0);
    std::lock_guard<std::mutex> lock(mutex_);
    --live_;
    Launch();
    ready_.notify_all();
}

void Watcher::Fail(const Task &task, StepFailure failure,
                   const std::string &url) {
    if (stop_.load())
        return;
    Debug().Log("FAIL " + task.content_id + " (" + StageName(task.stage) +
                "): " + failure.message +
                (failure.rejected ? " [rejected]" : ""));
    progress_->Fail();
    std::lock_guard<std::mutex> lock(mutex_);
    --live_;
    if (failure.rejected) {
        if (abort_reason_.empty()) {
            const std::filesystem::path dump =
                options_.data_dir / kFailureDumpName;
            WriteResponseDump(dump, url, failure.response);
            std::string reason = failure.abort_hint;
            if (reason.empty()) {
                reason =
                    "HTTP " + std::to_string(failure.response.status) + ": " +
                    kKaasEnv +
                    " may have expired, or the site is challenging requests";
            }
            Abort(reason + "\nsaved the full response to " + dump.string());
        }
        return;
    }
    failures_.push_back({task.content_id, std::move(failure.message)});
    if (consecutive_failures_.fetch_add(1) + 1 >= kMaxConsecutiveFailures) {
        Abort(std::to_string(kMaxConsecutiveFailures) +
              " consecutive failures; the site may be challenging requests");
        return;
    }
    Launch();
    ready_.notify_all();
}

void Watcher::Step(Http &http, Task task) {
    Debug().Log("step " + task.content_id + " stage " + StageName(task.stage) +
                ", position " + std::to_string(task.position) + "/" +
                std::to_string(task.duration));
    const Clock::time_point now = Clock::now();
    const double remaining = task.duration - task.position;
    double watched = 0;
    if (task.stage == Stage::kWatch) {
        const double elapsed =
            std::min(Seconds(now - task.last_sent), Seconds(kMaxElapsed));
        watched = std::min(elapsed * kSpeed, remaining);
    }
    const bool last = task.stage == Stage::kWatch && watched >= remaining;
    const double position = last ? task.duration : task.position + watched;

    const std::string url = ApiUrl(options_.base_url, kMutationOperation);
    const ProgressVariables variables{
        {.content_id = task.content_id,
         .seconds_watched = watched,
         .last_second_watched = position,
         .duration_seconds = std::lround(task.duration),
         .captions_ka_locale = "en",
         .fallback_player = false,
         .local_timezone_offset_seconds = LocalOffsetSeconds()}};
    std::expected<UpdateReply, StepFailure> reply =
        Execute<UpdateReply>(http, safelist_, url,
                             std::string(kMutationOperation), variables, stop_);
    if (!reply.has_value())
        return Fail(task, std::move(reply.error()), url);

    const std::optional<Update> update =
        reply->data ? reply->data->update : std::nullopt;
    if (!update.has_value() || update->error.has_value() ||
        !update->item_progress.has_value()) {
        std::string message = "response has no progress";
        if (update.has_value() && update->error.has_value()) {
            message = update->error->code.value_or("unknown error") + ": " +
                      update->error->debug_message.value_or("");
        }
        return Fail(task, Failure(std::move(message)), url);
    }

    task.position = position;
    task.last_sent = now;
    const bool completed = update->item_progress->completed;
    Debug().Log("progress " + task.content_id + ": sent watched " +
                std::to_string(watched) + " s, position " +
                std::to_string(position) + "/" + std::to_string(task.duration) +
                ", server completed=" + (completed ? "true" : "false"));
    if (task.stage == Stage::kStart && completed)
        return Complete(task, false);
    if (last) {
        if (completed)
            return Complete(task, true);
        return Fail(task, Failure("server did not mark the video completed"),
                    url);
    }
    auto jitter = GetJitteredDelay(0, 2000);
    task.due = task.stage == Stage::kStart
                   ? now + kTick + jitter
                   : std::max(task.due + kTick + jitter, now);
    task.stage = Stage::kWatch;
    Requeue(std::move(task));
}

void Watcher::Work(Http &http) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_.load() && !abort_.load() && live_ > 0) {
        const Clock::time_point now = Clock::now();
        if (queue_.empty() || queue_.top().due > now) {
            Clock::time_point wake = now + kPoll;
            if (!queue_.empty())
                wake = std::min(wake, queue_.top().due);
            ready_.wait_until(lock, wake);
            continue;
        }
        Task task = queue_.top();
        queue_.pop();
        lock.unlock();
        Step(http, std::move(task));
        lock.lock();
    }
}

RunStatus Watcher::Finish(std::size_t total) {
    const bool interrupted = stop_.load();
    const bool incomplete = abort_.load() || !failures_.empty();
    const std::size_t completed =
        total - pending_.size() + watched_.load() + skipped_.load();
    Debug().Log(
        "finish: interrupted=" + std::string(interrupted ? "yes" : "no") +
        " incomplete=" + (incomplete ? "yes" : "no") + " completed=" +
        std::to_string(completed) + "/" + std::to_string(total) + " failures=" +
        std::to_string(failures_.size()) + " abort=\"" + abort_reason_ + "\"");

    std::cout << (interrupted  ? "Watch interrupted"
                  : incomplete ? "Watch incomplete"
                               : "Watch complete")
              << '\n'
              << "  Completed  " << FormatCount(completed) << " of "
              << FormatCount(total) << " videos\n"
              << "  Watched    " << FormatCount(watched_.load())
              << " videos this run\n"
              << "  Journal    " << (options_.data_dir / kJournalName).string()
              << '\n';

    int shown = 0;
    for (const Failed &failed : failures_) {
        if (shown++ == kMaxFailuresShown) {
            std::cerr << "  ... and "
                      << FormatCount(failures_.size() - kMaxFailuresShown)
                      << " more\n";
            break;
        }
        std::cerr << "  failed  " << failed.content_id << "  " << failed.message
                  << '\n';
    }
    if (abort_.load())
        std::cerr << "Stopped early: " << abort_reason_ << '\n';
    if (interrupted) {
        std::cerr << "Progress is saved; run again to resume.\n";
        return RunStatus::kInterrupted;
    }
    if (incomplete) {
        std::cerr << "Run again to retry the videos that did not finish.\n";
        return RunStatus::kIncomplete;
    }
    return RunStatus::kComplete;
}

RunStatus Watcher::Run() {
    const char *debug_flag = std::getenv(kDebugEnv);
    if (debug_flag != nullptr && *debug_flag != '\0' &&
        std::string_view(debug_flag) != "0") {
        Debug().Open(options_.data_dir / kDebugDirName);
    }
    const char *kaas = std::getenv(kKaasEnv);
    const char *impersonate = std::getenv(kImpersonateEnv);
    Debug().Log(std::string("config: base_url=") + options_.base_url +
                " data_dir=" + options_.data_dir.string() + " fresh=" +
                (options_.fresh ? "true" : "false") + " impersonate=" +
                (impersonate != nullptr ? impersonate : "(default)") + " " +
                kKaasEnv + "=" + Redact(kaas != nullptr ? kaas : ""));
    if (kaas == nullptr || *kaas == '\0') {
        return Fatal(std::string(kKaasEnv) + " is not set");
    }

    const std::expected<std::vector<Video>, std::string> ids =
        LoadVideos(options_.data_dir / kInputName);
    if (!ids.has_value())
        return Fatal(ids.error());

    std::expected<std::unique_ptr<Journal>, std::string> journal =
        Journal::Open(options_.data_dir / kJournalName, options_.fresh,
                      IsValidJournalLine);
    if (!journal.has_value())
        return Fatal(journal.error());
    journal_ = std::move(*journal);

    for (const Video &video : *ids) {
        if (!journal_->Contains(video.content_id))
            pending_.push_back(video);
    }
    Debug().Log("loaded " + std::to_string(ids->size()) + " content ids, " +
                std::to_string(pending_.size()) + " pending");
    std::cerr << "Videos: " << FormatCount(ids->size()) << ", "
              << FormatCount(ids->size() - pending_.size())
              << " already watched, " << FormatCount(pending_.size())
              << " to watch\n";

    if (!pending_.empty()) {
        std::expected<std::unique_ptr<Http>, std::string> client =
            Http::Create(kaas);
        if (!client.has_value())
            return Fatal(client.error());
        if (Debug().enabled()) {
            const std::expected<Document, std::string> document =
                safelist_.Get(**client, std::string(kMutationOperation));
            if (!document.has_value()) {
                Debug().Log("preflight: " + std::string(kMutationOperation) +
                            ": " + document.error());
            }
        }
        Progress progress(Progress::Options{
            .title = "Watching videos",
            .extra_label = "watched",
            .total = ids->size(),
            .completed = ids->size() - pending_.size(),
            .extra = 0,
        });
        progress_ = &progress;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Launch();
        }
        Work(**client);
        progress.Finish();
        progress_ = nullptr;
    }
    return Finish(ids->size());
}

} // namespace

RunStatus RunWatch(const CommandOptions &options,
                   const std::atomic<bool> &stop) {
    return Watcher(options, stop).Run();
}

} // namespace khanbot
