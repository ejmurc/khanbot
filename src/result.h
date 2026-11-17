#ifndef SRC_RESULT_H_
#define SRC_RESULT_H_

#include <expected>
#include <string>

namespace khanbot {
template <typename T, typename E = std::string>
using Result = std::expected<T, E>;

}

#endif
