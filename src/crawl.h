#ifndef SRC_CRAWL_H_
#define SRC_CRAWL_H_

#include <atomic>

#include "src/command.h"

namespace khanbot {

RunStatus RunCrawl(const CommandOptions &options,
                   const std::atomic<bool> &stop);

}

#endif
