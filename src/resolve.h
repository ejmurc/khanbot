#ifndef SRC_RESOLVE_H_
#define SRC_RESOLVE_H_

#include <atomic>

#include "src/command.h"

namespace khanbot {

RunStatus RunResolve(const CommandOptions &options,
                     const std::atomic<bool> &stop);

}

#endif
