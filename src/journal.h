#ifndef SRC_JOURNAL_H_
#define SRC_JOURNAL_H_

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/result.h"

namespace khanbot {
class Journal {
  public:
    using Validator = bool (*)(std::string_view line);

    static Result<std::unique_ptr<Journal>>
    Open(const std::filesystem::path &path, bool fresh, Validator is_valid);

    [[nodiscard]] bool Contains(const std::string &key) const {
        std::scoped_lock lock(mutex_);
        return lines_.contains(key);
    }

    [[nodiscard]] bool Append(std::string_view line) {
        const std::string key(KeyOf(line));
        std::scoped_lock lock(mutex_);
        out_ << line << '\n';
        out_.flush();
        if (!out_)
            return false;
        lines_[key] = std::string(line);
        return true;
    }

    std::vector<std::string> Snapshot() const {
        std::scoped_lock lock(mutex_);
        std::vector<std::string> lines;
        lines.reserve(lines_.size());
        for (const auto &entry : lines_)
            lines.push_back(entry.second);
        return lines;
    }

  private:
    Journal() = default;

    static std::string_view KeyOf(std::string_view line) {
        return line.substr(0, line.find('\t'));
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> lines_;
    std::ofstream out_;
};

template <typename Lines>
Result<void> WriteLinesAtomically(const std::filesystem::path &path,
                                  const Lines &lines) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        for (const std::string &line : lines)
            out << line << '\n';
        out.flush();
        if (!out)
            return std::unexpected("cannot write " + temporary.string());
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        return std::unexpected("cannot replace " + path.string() + ": " +
                               error.message());
    }
    return {};
}

} // namespace khanbot

#endif
