#pragma once

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

namespace helmet {
inline std::string_view message_field(std::string_view line, std::string_view key) {
    size_t begin = 0;
    while (begin < line.size()) {
        begin = line.find_first_not_of(" \t", begin);
        if (begin == std::string_view::npos)
            break;
        const size_t end = line.find_first_of(" \t", begin);
        auto token = line.substr(begin, end - begin);
        if (token.size() > key.size() && token.substr(0, key.size()) == key &&
            token[key.size()] == '=')
            return token.substr(key.size() + 1);
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return {};
}

inline int message_integer(std::string_view line, std::string_view key) {
    auto value = message_field(line, key);
    if (value.empty())
        return 0;
    int parsed = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() ? parsed : 0;
}

inline std::string escape_json(std::string_view value) {
    std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char byte : value) {
        if (byte == '"' || byte == '\\') {
            result += '\\';
            result += static_cast<char>(byte);
        } else if (byte < 0x20) {
            result += "\\u00";
            result += hex[byte >> 4];
            result += hex[byte & 15];
        } else
            result += static_cast<char>(byte);
    }
    return result;
}

template <size_t Capacity> class MessageLine {
    static_assert(Capacity > 1, "line capacity must include payload and terminator");
    char bytes_[Capacity]{};
    size_t size_ = 0;
    bool discarded_ = false;

  public:
    // Returned storage is valid until the next append. CRLF is one message;
    // oversized/binary lines are discarded entirely through their delimiter.
    const char *append(char byte) {
        if (byte == '\r' || byte == '\n') {
            const bool ready = !discarded_ && size_ > 0;
            bytes_[size_] = '\0';
            size_ = 0;
            discarded_ = false;
            return ready ? bytes_ : nullptr;
        }
        if (byte == '\0' || size_ == Capacity - 1)
            discarded_ = true;
        if (!discarded_)
            bytes_[size_++] = byte;
        return nullptr;
    }
};
} // namespace helmet
