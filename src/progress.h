#ifndef SRC_PROGRESS_H_
#define SRC_PROGRESS_H_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace khanbot {

std::string FormatCount(std::size_t value);

class Progress {
  public:
    struct Options {
        std::string title;
        std::string extra_label;
        std::size_t total = 0;
        std::size_t completed = 0;
        std::size_t extra = 0;
        std::string current_label = "Current";
    };

    explicit Progress(Options options);
    ~Progress();

    Progress(const Progress &) = delete;
    Progress &operator=(const Progress &) = delete;

    void Advance(std::size_t extra = 0);
    void Fail();
    void Finish();

    void SetEta(std::chrono::duration<double> remaining);

    void SetCurrent(double position, double duration, double rate);

  private:
    using Clock = std::chrono::steady_clock;

    struct Sample {
        Clock::time_point time;
        std::size_t done;
    };

    struct CurrentItem {
        bool active = false;
        double position = 0;
        double duration = 0;
        double rate = 0;
        Clock::time_point at;
    };

    void Run(std::stop_token token);
    void Draw(bool final);
    std::string Render(bool final) const;
    std::string RenderCurrent() const;
    std::string Paint(std::string_view text, std::string_view style) const;
    std::optional<double> Rate(bool overall, Clock::time_point now,
                               std::size_t done) const;
    std::optional<double> EtaHint(Clock::time_point now) const;

    Options options_;
    bool interactive_;
    bool color_;
    Clock::time_point start_;
    std::atomic<std::size_t> done_;
    std::atomic<std::size_t> extra_;
    std::atomic<std::size_t> failed_{0};
    std::atomic<Clock::rep> eta_deadline_{
        std::numeric_limits<Clock::rep>::min()};
    mutable std::mutex current_mutex_;
    CurrentItem current_;
    mutable int title_width_ = 0;
    mutable int bar_width_ = 0;
    bool second_line_drawn_ = false;
    std::deque<Sample> samples_;
    bool finished_ = false;
    std::jthread reporter_;
};

} // namespace khanbot

#endif
