#include "src/journal.h"

#include <cstdint>
#include <system_error>
#include <utility>

namespace khanbot {
namespace {
std::unexpected<std::string> Failure(std::string_view action,
                                     const std::filesystem::path &path,
                                     const std::error_code &error) {
    return std::unexpected(std::string(action) + " " + path.string() + ": " +
                           error.message());
}

} // namespace

Result<std::unique_ptr<Journal>>
Journal::Open(const std::filesystem::path &path, bool fresh,
              Validator is_valid) {
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
            std::string key(KeyOf(line));
            if (key.empty() || !is_valid(line))
                continue;
            journal->lines_.insert_or_assign(std::move(key), std::move(line));
        }
    }
    if (std::filesystem::exists(path, error)) {
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (!error && size > valid_bytes)
            std::filesystem::resize_file(path, valid_bytes, error);
    }
    if (error)
        return Failure("cannot repair", path, error);
    journal->out_.open(path, std::ios::binary | std::ios::app);
    if (!journal->out_) {
        return std::unexpected("cannot open " + path.string() + " for writing");
    }
    return journal;
}

} // namespace khanbot
