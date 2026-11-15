#ifndef SRC_WATCH_H_
#define SRC_WATCH_H_

#include <atomic>

#include "src/command.h"

namespace khanbot {

RunStatus RunWatch(const CommandOptions &options,
                   const std::atomic<bool> &stop);

}

#endif
