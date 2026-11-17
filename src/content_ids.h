#ifndef SRC_CONTENT_IDS_H_
#define SRC_CONTENT_IDS_H_

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/text.h"

namespace khanbot {
inline constexpr long kMinDurationSeconds = 1;

struct Lookup {
    std::string content_id;
    long duration = 0;

    bool found() const { return !content_id.empty(); }
};

struct Entry {
    std::string youtube_id;
    Lookup lookup;
};

constexpr bool IsContentId(std::string_view text) {
    return !text.empty() && std::ranges::none_of(text, [](char c) {
        return IsAsciiSpace(c) || IsAsciiControl(c);
    });
}

inline std::string FormatEntry(std::string_view youtube_id,
                               const Lookup &lookup) {
    std::string line(youtube_id);
    line += '\t';
    line.append(lookup.content_id);
    line += '\t';
    if (lookup.found())
        line.append(std::to_string(lookup.duration));
    return line;
}

inline std::optional<Entry> ParseEntry(std::string_view line) {
    const std::size_t first = line.find('\t');
    if (first == kNpos)
        return std::nullopt;
    const std::size_t second = line.find('\t', first + 1);
    if (second == kNpos || line.find('\t', second + 1) != kNpos)
        return std::nullopt;
    const std::string_view youtube_id = line.substr(0, first);
    const std::string_view content_id =
        line.substr(first + 1, second - first - 1);
    const std::string_view duration = line.substr(second + 1);
    if (!IsYoutubeId(youtube_id))
        return std::nullopt;
    Entry entry{std::string(youtube_id), {}};
    if (content_id.empty())
        return duration.empty() ? std::optional<Entry>(std::move(entry))
                                : std::nullopt;
    const std::optional<long> seconds = ParseLong(duration);
    if (!IsContentId(content_id) || !seconds || *seconds < kMinDurationSeconds)
        return std::nullopt;
    entry.lookup = Lookup{std::string(content_id), *seconds};
    return entry;
}

static_assert(IsContentId("x1a2b3c4d"));
static_assert(IsContentId("x_1-2"));
static_assert(!IsContentId(""));
static_assert(!IsContentId("a b"));

} // namespace khanbot

#endif
