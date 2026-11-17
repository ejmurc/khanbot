#include "src/run.h"

#include <algorithm>
#include <iostream>
#include <ranges>
#include <system_error>

#include "src/progress.h"

namespace khanbot {
namespace {
constexpr std::chrono::milliseconds kPoll{100};

}

bool WaitUntil(const std::atomic<bool> &stop, Clock::time_point deadline) {
    while (!stop.load()) {
        const Clock::time_point now = Clock::now();
        if (now >= deadline)
            return true;
        std::this_thread::sleep_for(
            std::min<Clock::duration>(deadline - now, kPoll));
    }
    return false;
}

RunStatus Fatal(std::string_view message) {
    std::cerr << "error: " << message << '\n';
    return RunStatus::kFatal;
}

Result<void> EnsureDirectory(const std::filesystem::path &directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return std::unexpected("cannot create " + directory.string() + ": " +
                               error.message());
    }
    return {};
}

Result<std::vector<std::unique_ptr<Http>>> CreateClients(int count) {
    std::vector<std::unique_ptr<Http>> clients;
    for (int i = 0; i < count; ++i) {
        Result<std::unique_ptr<Http>> client = Http::Create();
        if (!client)
            return std::unexpected(std::move(client.error()));
        clients.push_back(std::move(*client));
    }
    return clients;
}

void FailureLog::Record(std::string subject, std::string message) {
    {
        std::scoped_lock lock(mutex_);
        failures_.push_back({std::move(subject), std::move(message)});
    }
    if (consecutive_.fetch_add(1) + 1 >= kMaxConsecutiveFailures) {
        Abort(std::format(
            "{} consecutive failures; the site may be challenging requests",
            kMaxConsecutiveFailures));
    }
}

bool FailureLog::incomplete() const {
    std::scoped_lock lock(mutex_);
    return aborted_.load() || !failures_.empty();
}

std::string FailureLog::reason() const {
    std::scoped_lock lock(mutex_);
    return reason_;
}

void FailureLog::PrintFailures(std::ostream &out) const {
    std::scoped_lock lock(mutex_);
    for (const Item &item : failures_ | std::views::take(kMaxFailuresShown)) {
        out << "  failed  " << item.subject << "  " << item.message << '\n';
    }
    if (failures_.size() > kMaxFailuresShown) {
        out << "  ... and " << FormatCount(failures_.size() - kMaxFailuresShown)
            << " more\n";
    }
}

RunStatus Report(std::string_view command, std::string_view unfinished,
                 bool interrupted, const FailureLog &log,
                 std::string_view details) {
    const bool incomplete = log.incomplete();
    std::cout << command << ' '
              << (interrupted  ? "interrupted"
                  : incomplete ? "incomplete"
                               : "complete")
              << '\n'
              << details << '\n';
    log.PrintFailures(std::cerr);
    if (log.aborted())
        std::cerr << "Stopped early: " << log.reason() << '\n';
    if (interrupted) {
        std::cerr << "Progress is saved; run again to resume.\n";
        return RunStatus::kInterrupted;
    }
    if (incomplete) {
        std::cerr << "Run again to retry the " << unfinished
                  << " that did not finish.\n";
        return RunStatus::kIncomplete;
    }
    return RunStatus::kComplete;
}

} // namespace khanbot
