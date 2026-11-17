#ifndef SRC_RUN_H_
#define SRC_RUN_H_

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "src/command.h"
#include "src/http.h"
#include "src/result.h"

namespace khanbot {
using Clock = std::chrono::steady_clock;

inline constexpr int kWorkers = 4;
inline constexpr int kAttempts = 3;
inline constexpr int kMaxConsecutiveFailures = 5;
inline constexpr int kMaxFailuresShown = 10;
inline constexpr std::chrono::milliseconds kRequestDelay{150};
inline constexpr std::chrono::milliseconds kRetryBackoff{500};

bool WaitUntil(const std::atomic<bool> &stop, Clock::time_point deadline);

inline bool WaitFor(const std::atomic<bool> &stop,
                    std::chrono::milliseconds duration) {
    return WaitUntil(stop, Clock::now() + duration);
}

RunStatus Fatal(std::string_view message);

Result<void> EnsureDirectory(const std::filesystem::path &directory);

Result<std::vector<std::unique_ptr<Http>>> CreateClients(int count);

template <typename Work>
void RunWorkers(const std::vector<std::unique_ptr<Http>> &clients,
                Work &&work) {
    std::vector<std::jthread> pool;
    for (const std::unique_ptr<Http> &client : clients)
        pool.emplace_back([&work, &client] { work(*client); });
}

struct FetchFailure {
    std::string message;

    Response response;

    bool rejected = false;

    bool retry_now = false;

    std::string abort_hint;
};

inline FetchFailure MakeFailure(std::string message, Response response = {}) {
    FetchFailure failure;
    failure.message = std::move(message);
    failure.response = std::move(response);
    return failure;
}

inline std::unexpected<FetchFailure> Failed(std::string message,
                                            Response response = {}) {
    return std::unexpected(
        MakeFailure(std::move(message), std::move(response)));
}

inline std::unexpected<FetchFailure>
Rejected(std::string message, Response response, std::string abort_hint = {}) {
    FetchFailure failure = MakeFailure(std::move(message), std::move(response));
    failure.rejected = true;
    failure.abort_hint = std::move(abort_hint);
    return std::unexpected(std::move(failure));
}

template <typename T, typename Attempt>
Result<T, FetchFailure> Retry(const std::atomic<bool> &stop,
                              Attempt &&attempt) {
    const auto interrupted = [] {
        return std::unexpected(MakeFailure("interrupted"));
    };
    FetchFailure failure;
    for (int number = 1; number <= kAttempts; ++number) {
        if (stop.load())
            return interrupted();
        Result<T, FetchFailure> result = attempt();
        if (result)
            return result;
        failure = std::move(result.error());
        if (failure.rejected)
            return std::unexpected(std::move(failure));
        if (number < kAttempts && !failure.retry_now &&
            !WaitFor(stop, kRetryBackoff * number)) {
            return interrupted();
        }
    }
    failure.message =
        std::format("after {} attempts: {}", kAttempts, failure.message);
    return std::unexpected(std::move(failure));
}

class FailureLog {
  public:
    void Record(std::string subject, std::string message);

    void Succeeded() { consecutive_.store(0); }

    void Abort(std::string reason) {
        Abort([&] { return std::move(reason); });
    }

    template <std::invocable MakeReason> void Abort(MakeReason &&make_reason) {
        std::scoped_lock lock(mutex_);
        if (reason_.empty())
            reason_ = make_reason();
        aborted_.store(true);
    }

    bool aborted() const { return aborted_.load(); }
    bool incomplete() const;
    std::string reason() const;
    void PrintFailures(std::ostream &out) const;

  private:
    struct Item {
        std::string subject;
        std::string message;
    };

    mutable std::mutex mutex_;
    std::vector<Item> failures_;
    std::string reason_;
    std::atomic<int> consecutive_{0};
    std::atomic<bool> aborted_{false};
};

RunStatus Report(std::string_view command, std::string_view unfinished,
                 bool interrupted, const FailureLog &log,
                 std::string_view details);

} // namespace khanbot

#endif
