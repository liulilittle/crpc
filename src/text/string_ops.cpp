// CRPC -- string operations. See include/crpc/text/string_ops.hpp.
#include <crpc/text/string_ops.hpp>

namespace crpc {
namespace text {

namespace {

bool is_in_charset(char c, std::string_view charset) noexcept {
    for (const char candidate : charset) {
        if (candidate == c) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string_view trim_left(std::string_view text, std::string_view charset) noexcept {
    std::size_t first = 0;
    while (first < text.size() && is_in_charset(text[first], charset)) {
        ++first;
    }
    return text.substr(first);
}

std::string_view trim_right(std::string_view text, std::string_view charset) noexcept {
    std::size_t last = text.size();
    while (last > 0 && is_in_charset(text[last - 1], charset)) {
        --last;
    }
    return text.substr(0, last);
}

std::string_view trim(std::string_view text, std::string_view charset) noexcept {
    return trim_left(trim_right(text, charset), charset);
}

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> out;
    std::size_t begin = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != delimiter) {
            continue;
        }
        out.push_back(text.substr(begin, i - begin));
        begin = i + 1;
    }
    // The tail after the last delimiter. It is emitted unless it is empty
    // because the input ended with a delimiter or was empty.
    if (begin < text.size()) {
        out.push_back(text.substr(begin));
    }
    return out;
}

std::vector<std::string_view> split(std::string_view text,
                                    std::string_view delimiter) {
    if (delimiter.empty()) {
        return {text};
    }
    std::vector<std::string_view> out;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t pos = text.find(delimiter, begin);
        if (pos == std::string_view::npos) {
            break;
        }
        out.push_back(text.substr(begin, pos - begin));
        begin = pos + delimiter.size();
    }
    if (begin < text.size()) {
        out.push_back(text.substr(begin));
    }
    return out;
}

result<std::string> replace_all(std::string_view text, std::string_view from,
                                std::string_view to) {
    if (from.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "replace_all: the 'from' pattern must not be empty"};
    }
    std::string out;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t pos = text.find(from, begin);
        if (pos == std::string_view::npos) {
            out.append(text.substr(begin));
            break;
        }
        out.append(text.substr(begin, pos - begin));
        out.append(to.data(), to.size());
        begin = pos + from.size();
    }
    return out;
}

} // namespace text
} // namespace crpc