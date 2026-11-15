#ifndef SRC_JOURNAL_H_
#define SRC_JOURNAL_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace khanbot {

class Journal {
  public:
    using Validator = bool (*)(std::string_view line);

    static std::expected<std::unique_ptr<Journal>, std::string>
    Open(const std::filesystem::path &path, bool fresh, Validator is_valid) {
        std::error_code error;
        if (fresh) {
            std::filesystem::remove(path, error);
            if (error)
                return Failure("cannot reset", path, error);
        }
        std::unique_ptr<Journal> journal(new Journal());
        std::uintmax_t valid_bytes = 0;
        {
            std::ifstream in(path, std::ios::binary);
            std::string line;
            while (std::getline(in, line)) {
                if (in.eof())
                    break;
                valid_bytes += line.size() + 1;
                const std::string key(KeyOf(line));
                if (key.empty() || !is_valid(line))
                    continue;
                journal->lines_[key] = std::move(line);
            }
        }
        if (std::filesystem::exists(path, error)) {
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (!error && size > valid_bytes) {
                std::filesystem::resize_file(path, valid_bytes, error);
            }
        }
        if (error)
            return Failure("cannot repair", path, error);
        journal->out_.open(path, std::ios::binary | std::ios::app);
        if (!journal->out_) {
            return std::unexpected("cannot open " + path.string() +
                                   " for writing");
        }
        return journal;
    }

    bool Contains(const std::string &key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_.contains(key);
    }

    bool Append(std::string_view line) {
        const std::string key(KeyOf(line));
        std::lock_guard<std::mutex> lock(mutex_);
        out_ << line << '\n';
        out_.flush();
        if (!out_)
            return false;
        lines_[key] = std::string(line);
        return true;
    }

    std::vector<std::string> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
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

    static std::unexpected<std::string>
    Failure(std::string_view action, const std::filesystem::path &path,
            const std::error_code &error) {
        return std::unexpected(std::string(action) + " " + path.string() +
                               ": " + error.message());
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> lines_;
    std::ofstream out_;
};

template <typename Lines>
std::expected<void, std::string>
WriteLinesAtomically(const std::filesystem::path &path, const Lines &lines) {
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
