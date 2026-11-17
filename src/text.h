#ifndef SRC_TEXT_H_
#define SRC_TEXT_H_

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace khanbot {
inline constexpr std::size_t kNpos = std::string_view::npos;
inline constexpr std::size_t kYoutubeIdLength = 11;

constexpr bool IsAsciiAlnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

constexpr bool IsAsciiSpace(char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

constexpr bool IsAsciiControl(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte < 0x20 || byte == 0x7f;
}

constexpr bool IsIdChar(char c) {
    return IsAsciiAlnum(c) || c == '_' || c == '-';
}

constexpr bool IsYoutubeId(std::string_view text) {
    return text.size() == kYoutubeIdLength &&
           std::ranges::all_of(text, IsIdChar);
}

inline std::optional<long> ParseLong(std::string_view text) {
    long value = 0;
    const char *const end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc() || stop != end)
        return std::nullopt;
    return value;
}

inline std::string Preview(std::string_view text, std::size_t limit) {
    std::string preview(text.substr(0, limit));
    std::ranges::replace_if(
        preview, [](char c) { return c == '\n' || c == '\r' || c == '\t'; },
        ' ');
    return preview;
}

template <typename T> void SortUnique(std::vector<T> &items) {
    std::ranges::sort(items);
    const auto duplicates = std::ranges::unique(items);
    items.erase(duplicates.begin(), duplicates.end());
}

constexpr std::uint32_t HashDocument(std::string_view document) {
    std::uint32_t hash = 5381;
    for (std::size_t i = document.size(); i > 0; --i) {
        hash = hash * 33u ^ static_cast<std::uint32_t>(
                                static_cast<unsigned char>(document[i - 1]));
    }
    return hash;
}

static_assert(IsYoutubeId("dQw4w9WgXcQ"));
static_assert(!IsYoutubeId("dQw4w9WgXc"));
static_assert(!IsYoutubeId("dQw4w9WgXc!"));

} // namespace khanbot

#endif
