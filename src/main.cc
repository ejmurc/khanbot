#include <atomic>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/command.h"
#include "src/crawl.h"
#include "src/http.h"
#include "src/resolve.h"
#include "src/watch.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitIncomplete = 2;
constexpr int kExitInterrupted = 130;

constexpr char kUsage[] = R"(Usage: main <command> [options]

Commands:
  crawl    Collect YouTube IDs from the Khan Academy sitemaps
  resolve  Resolve collected YouTube IDs to Khan Academy content IDs
  watch    Watch the resolved videos on your account to mark them complete

Options:
  --fresh          Ignore saved progress and start over
  --data-dir DIR   Directory for state and output (default: data)
  --base-url URL   Site root (default: https://www.khanacademy.org)

Environment:
  KA_IMPERSONATE   libcurl-impersonate target (default: chrome152)
  KA_KAAS          KAAS session cookie of the account to watch as (watch)
)";

struct Command {
    std::string_view name;
    khanbot::RunStatus (*run)(const khanbot::CommandOptions &,
                              const std::atomic<bool> &);
};

constexpr Command kCommands[] = {
    {"crawl", khanbot::RunCrawl},
    {"resolve", khanbot::RunResolve},
    {"watch", khanbot::RunWatch},
};

std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void HandleSignal(int) {
    stop_requested.store(true);
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
}

const Command *FindCommand(std::string_view name) {
    for (const Command &command : kCommands) {
        if (command.name == name)
            return &command;
    }
    return nullptr;
}

std::optional<khanbot::CommandOptions>
ParseCommandOptions(const std::vector<std::string_view> &args) {
    khanbot::CommandOptions options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--fresh") {
            options.fresh = true;
        } else if (arg == "--data-dir" && i + 1 < args.size()) {
            options.data_dir = args[++i];
        } else if (arg == "--base-url" && i + 1 < args.size()) {
            options.base_url = std::string(args[++i]);
        } else {
            std::cerr << "unrecognized or incomplete argument: " << arg << '\n';
            return std::nullopt;
        }
    }
    while (!options.base_url.empty() && options.base_url.back() == '/') {
        options.base_url.pop_back();
    }
    return options;
}

int ExitCodeFor(khanbot::RunStatus status) {
    switch (status) {
    case khanbot::RunStatus::kComplete:
        return kExitOk;
    case khanbot::RunStatus::kIncomplete:
        return kExitIncomplete;
    case khanbot::RunStatus::kInterrupted:
        return kExitInterrupted;
    case khanbot::RunStatus::kFatal:
        return kExitFailure;
    }
    return kExitFailure;
}

} // namespace

int main(int argc, char **argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);

    for (const std::string_view arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::cout << kUsage;
            return kExitOk;
        }
    }
    if (args.empty()) {
        std::cerr << kUsage;
        return kExitFailure;
    }
    const Command *command = FindCommand(args[0]);
    if (command == nullptr) {
        std::cerr << "unknown command: " << args[0] << "\n\n" << kUsage;
        return kExitFailure;
    }

    const std::optional<khanbot::CommandOptions> options =
        ParseCommandOptions(args);
    if (!options.has_value()) {
        std::cerr << '\n' << kUsage;
        return kExitFailure;
    }

    const khanbot::CurlRuntime curl;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    return ExitCodeFor(command->run(*options, stop_requested));
}
