#ifndef SRC_COMMAND_H_
#define SRC_COMMAND_H_

#include <filesystem>
#include <string>

namespace khanbot {

struct CommandOptions {
    std::string base_url = "https://www.khanacademy.org";
    std::filesystem::path data_dir = "data";
    bool fresh = false;
};

enum class RunStatus {
    kComplete,
    kIncomplete,
    kInterrupted,
    kFatal,
};

} // namespace khanbot

#endif
