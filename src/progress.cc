#include "src/progress.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace khanbot {
namespace {
constexpr auto kRefreshInterval = std::chrono::milliseconds(100);
constexpr auto kPlainInterval = std::chrono::seconds(10);
constexpr auto kRateWindow = std::chrono::seconds(10);
constexpr int kDefaultColumns = 80;
constexpr int kMinBarWidth = 12;
constexpr int kMaxBarWidth = 40;
constexpr int kPercentWidth = 6;
constexpr int kRateWidth = 7;
constexpr double kMaxEtaSeconds = 100.0 * 3600.0;

constexpr std::string_view kReset = "\x1b[0m";
constexpr std::string_view kBold = "\x1b[1m";
constexpr std::string_view kDim = "\x1b[2m";
constexpr std::string_view kRed = "\x1b[31m";
constexpr std::string_view kGreen = "\x1b[32m";
constexpr std::string_view kCyan = "\x1b[36m";
constexpr std::string_view kClearLine = "\x1b[K";
constexpr std::string_view kCursorUp = "\x1b[1A";
constexpr std::string_view kNoStyle = "";

constexpr std::string_view kEighths[] = {"▏", "▎", "▍", "▌", "▋", "▊", "▉"};

struct Segment {
    std::string text;
    std::string_view style;
    int drop_rank;
};

bool IsInteractive() {
    const char *term = std::getenv("TERM");
    const bool dumb = term != nullptr && std::string_view(term) == "dumb";
    return isatty(STDERR_FILENO) != 0 && !dumb;
}

int TerminalColumns() {
    winsize size{};
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return size.ws_col;
    }
    return kDefaultColumns;
}

std::string FormatFixed(double value, int precision) {
    return std::format("{:.{}f}", value, precision);
}

std::string FormatDuration(double seconds) {
    const auto total = std::lround(seconds);
    const auto hours = total / 3600;
    const auto minutes = (total % 3600) / 60;
    return hours > 0 ? std::format("{}:{:02}:{:02}", hours, minutes, total % 60)
                     : std::format("{}:{:02}", minutes, total % 60);
}

std::string PadLeft(const std::string &text, int width) {
    return std::format("{:>{}}", text, width);
}

std::string RenderBar(double fraction, int width) {
    const int eighths = static_cast<int>(std::lround(fraction * width * 8));
    const int full = eighths / 8;
    const int partial = eighths % 8;
    std::string bar;
    int used = full;
    for (int i = 0; i < full; ++i)
        bar += "█";
    if (partial > 0 && full < width) {
        bar += kEighths[static_cast<std::size_t>(partial - 1)];
        ++used;
    }
    bar.append(static_cast<std::size_t>(width - used), ' ');
    return bar;
}

int FixedWidth(std::size_t title_size, const std::vector<Segment> &segments) {
    std::size_t width = title_size;
    for (const Segment &segment : segments) {
        width += segment.text.size() + (width > 0 ? 2 : 0);
    }
    return static_cast<int>(width);
}

} // namespace

std::string FormatCount(std::size_t value) {
    std::string digits = std::to_string(value);
    for (std::size_t position = digits.size(); position > 3; position -= 3) {
        digits.insert(position - 3, 1, ',');
    }
    return digits;
}

Progress::Progress(Options options)
    : options_(std::move(options)), interactive_(IsInteractive()),
      color_(interactive_ && std::getenv("NO_COLOR") == nullptr),
      start_(Clock::now()), done_(options_.completed), extra_(options_.extra) {
    samples_.push_back({start_, options_.completed});
    reporter_ = std::jthread([this](std::stop_token token) { Run(token); });
}

Progress::~Progress() { Finish(); }

void Progress::Advance(std::size_t extra) {
    done_.fetch_add(1, std::memory_order_relaxed);
    extra_.fetch_add(extra, std::memory_order_relaxed);
}

void Progress::Fail() {
    done_.fetch_add(1, std::memory_order_relaxed);
    failed_.fetch_add(1, std::memory_order_relaxed);
}

void Progress::SetEta(std::chrono::duration<double> remaining) {
    const Clock::time_point deadline =
        Clock::now() + std::chrono::duration_cast<Clock::duration>(remaining);
    eta_deadline_.store(deadline.time_since_epoch().count(),
                        std::memory_order_relaxed);
}

void Progress::SetCurrent(double position, double duration, double rate) {
    std::lock_guard lock(current_mutex_);
    current_ = CurrentItem{.active = true,
                           .position = position,
                           .duration = duration,
                           .rate = rate,
                           .at = Clock::now()};
}

void Progress::Finish() {
    if (finished_)
        return;
    finished_ = true;
    reporter_.request_stop();
    if (reporter_.joinable())
        reporter_.join();
    Draw(true);
}

void Progress::Run(std::stop_token token) {
    Clock::time_point next_plain_draw = start_ + kPlainInterval;
    std::mutex wake_mutex;
    std::condition_variable_any wake;
    while (!token.stop_requested()) {
        const Clock::time_point now = Clock::now();
        samples_.push_back({now, done_.load(std::memory_order_relaxed)});
        while (samples_.size() > 2 &&
               now - samples_.front().time > kRateWindow) {
            samples_.pop_front();
        }
        if (interactive_) {
            Draw(false);
        } else if (now >= next_plain_draw) {
            Draw(false);
            next_plain_draw = now + kPlainInterval;
        }

        std::unique_lock lock(wake_mutex);
        wake.wait_for(lock, token, kRefreshInterval, [] { return false; });
    }
}

void Progress::Draw(bool final) {
    std::string line = Render(final);
    const std::string current = final ? std::string() : RenderCurrent();
    if (interactive_) {
        line.insert(0, "\r");
        line += kClearLine;
        if (!current.empty() || second_line_drawn_) {
            line += '\n';
            line += current;
            line += kClearLine;
            line += kCursorUp;
            second_line_drawn_ = !current.empty();
        }
    }
    if (final || !interactive_)
        line += '\n';
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

std::optional<double> Progress::Rate(bool overall, Clock::time_point now,
                                     std::size_t done) const {
    const Sample origin =
        overall ? Sample{start_, options_.completed} : samples_.front();
    const double elapsed =
        std::chrono::duration<double>(now - origin.time).count();
    if (elapsed < 1.0 || done <= origin.done)
        return std::nullopt;
    return static_cast<double>(done - origin.done) / elapsed;
}

std::optional<double> Progress::EtaHint(Clock::time_point now) const {
    const Clock::rep raw = eta_deadline_.load(std::memory_order_relaxed);
    if (raw == std::numeric_limits<Clock::rep>::min())
        return std::nullopt;
    const Clock::time_point deadline{Clock::duration{raw}};
    return std::max(0.0, std::chrono::duration<double>(deadline - now).count());
}

std::string Progress::Paint(std::string_view text,
                            std::string_view style) const {
    if (!color_ || style.empty())
        return std::string(text);
    std::string painted;
    painted.append(style).append(text).append(kReset);
    return painted;
}

std::string Progress::RenderCurrent() const {
    if (!interactive_ || bar_width_ <= 0)
        return {};
    CurrentItem item;
    {
        std::lock_guard lock(current_mutex_);
        item = current_;
    }
    if (!item.active)
        return {};

    const double elapsed =
        std::chrono::duration<double>(Clock::now() - item.at).count();
    const double position =
        std::min(item.duration, item.position + elapsed * item.rate);
    const double fraction = item.duration > 0 ? position / item.duration : 1.0;
    const std::string percent =
        PadLeft(FormatFixed(fraction * 100.0, 1) + "%", kPercentWidth);
    std::string times =
        FormatDuration(position) + "/" + FormatDuration(item.duration);

    const int columns = TerminalColumns();
    const int lead = title_width_ > 0 ? title_width_ + 2 : 0;
    const auto width = [&] {
        return lead + bar_width_ + 2 + 2 + static_cast<int>(percent.size()) +
               (times.empty() ? 0 : 2 + static_cast<int>(times.size()));
    };
    if (width() > columns - 1)
        times.clear();
    if (width() > columns - 1)
        return {};

    std::string line;
    if (title_width_ > 0) {
        std::string label = options_.current_label;
        label.resize(static_cast<std::size_t>(title_width_), ' ');
        line += Paint(label, kBold) + "  ";
    }
    line += Paint("[", kDim) + Paint(RenderBar(fraction, bar_width_), kCyan) +
            Paint("]", kDim) + "  " + Paint(percent, kBold);
    if (!times.empty())
        line += "  " + Paint(times, kDim);
    return line;
}

std::string Progress::Render(bool final) const {
    const Clock::time_point now = Clock::now();
    const std::size_t done = done_.load(std::memory_order_relaxed);
    const std::size_t extra = extra_.load(std::memory_order_relaxed);
    const std::size_t failed = failed_.load(std::memory_order_relaxed);
    const double elapsed = std::chrono::duration<double>(now - start_).count();
    const double fraction =
        options_.total == 0
            ? 1.0
            : std::min(1.0, static_cast<double>(done) /
                                static_cast<double>(options_.total));
    const std::optional<double> rate = Rate(final, now, done);

    std::string time_text;
    if (final) {
        time_text = "took " + FormatDuration(elapsed);
    } else if (done >= options_.total) {
        time_text = "ETA 0:00";
    } else if (const std::optional<double> hint = EtaHint(now)) {
        time_text = "ETA " + FormatDuration(*hint);
    } else if (rate.has_value()) {
        const double eta = static_cast<double>(options_.total - done) / *rate;
        time_text =
            eta < kMaxEtaSeconds ? "ETA " + FormatDuration(eta) : "ETA --:--";
    } else {
        time_text = "ETA --:--";
    }

    const std::string total_text = FormatCount(options_.total);
    std::vector<Segment> segments;
    segments.push_back(
        {PadLeft(FormatFixed(fraction * 100.0, 1) + "%", kPercentWidth), kBold,
         0});
    segments.push_back(
        {PadLeft(FormatCount(done), static_cast<int>(total_text.size())) + "/" +
             total_text,
         kNoStyle, 0});
    segments.push_back(
        {PadLeft(rate.has_value() ? FormatFixed(*rate, 1) + "/s" : "--/s",
                 kRateWidth),
         kDim, 3});
    segments.push_back({time_text, kDim, 0});
    segments.push_back(
        {FormatCount(extra) + " " + options_.extra_label, kNoStyle, 2});
    if (failed > 0) {
        segments.push_back({FormatCount(failed) + " failed", kRed, 1});
    }

    std::size_t title_size = options_.title.size();
    int bar_width = 0;
    if (interactive_) {
        const int columns = TerminalColumns();
        while (true) {
            const int fixed = FixedWidth(title_size, segments);
            const int available = columns - 1 - fixed - 4;
            if (available >= kMinBarWidth) {
                bar_width = std::min(available, kMaxBarWidth);
                break;
            }
            auto victim =
                std::max_element(segments.begin(), segments.end(),
                                 [](const Segment &a, const Segment &b) {
                                     return a.drop_rank < b.drop_rank;
                                 });
            if (victim != segments.end() && victim->drop_rank > 0) {
                segments.erase(victim);
                continue;
            }
            if (fixed > columns - 1)
                title_size = 0;
            break;
        }
    }

    title_width_ = static_cast<int>(title_size);
    bar_width_ = bar_width;

    std::string line;
    const auto append = [&line](const std::string &piece) {
        if (!line.empty())
            line += "  ";
        line += piece;
    };
    if (title_size > 0)
        append(Paint(options_.title, kBold));
    if (bar_width > 0) {
        const bool clean_finish =
            final && failed == 0 && done >= options_.total;
        append(Paint("[", kDim) +
               Paint(RenderBar(fraction, bar_width),
                     clean_finish ? kGreen : kCyan) +
               Paint("]", kDim));
    }
    for (const Segment &segment : segments) {
        append(Paint(segment.text, segment.style));
    }
    return line;
}

} // namespace khanbot
