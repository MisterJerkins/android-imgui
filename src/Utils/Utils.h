#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

struct Error {
    std::string message;
};

// What a function that can fail returns inside the library: its value, or what went wrong.
// There are no exceptions.
template <class T = void>
using Result = std::expected<T, Error>;

// Makes the error a function returns: `return Err("no display (status {})", status);`
template <class... Args>
std::unexpected<Error> Err(std::format_string<Args...> format, Args&&... args) {
    return std::unexpected(Error{std::format(format, std::forward<Args>(args)...)});
}

// A rectangle in display pixels.
struct Area {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    bool operator==(const Area&) const = default;

    constexpr float GetWidth() const {
        return right - left;
    }

    constexpr float GetHeight() const {
        return bottom - top;
    }

    constexpr std::array<float, 2> GetCenter() const {
        return {(left + right) / 2.0f, (top + bottom) / 2.0f};
    }

    // Whether a pixel is inside: the right and the bottom edge are the first ones outside.
    constexpr bool Contains(std::array<float, 2> position) const {
        return position[0] >= left && position[1] >= top && position[0] < right && position[1] < bottom;
    }

    // How far a point is from the rectangle, squared: nothing when it is inside or on an edge.
    constexpr float DistanceSq(std::array<float, 2> position) const {
        const float x = std::max({left - position[0], position[0] - right, 0.0f});
        const float y = std::max({top - position[1], position[1] - bottom, 0.0f});
        return x * x + y * y;
    }
};

// Small things more than one part of the library needs.
class Utils {
public:
    // Picks the lambda for whatever a variant holds: `std::visit(Utils::Overloaded{...}, value)`.
    template <class... Cases>
    struct Overloaded : Cases... {
        using Cases::operator()...;
    };

    template <class... Cases>
    Overloaded(Cases...) -> Overloaded<Cases...>;

    static constexpr uint64_t HashSeed = 0x9e37'79b9'7f4a'7c15;

    // Folds one more value into a hash that only has to tell things apart, quickly.
    static constexpr uint64_t HashMix(uint64_t hash, uint64_t value) {
        return (std::rotl(hash, 5) ^ value) * HashSeed;
    }

    static constexpr bool IsBlank(char letter) {
        return letter == ' ' || (letter >= '\t' && letter <= '\r');
    }

    static constexpr bool IsDigit(char letter) {
        return letter >= '0' && letter <= '9';
    }

    static constexpr char ToLower(char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    }

    // A text without the blanks around it.
    static constexpr std::string_view Trim(std::string_view text) {
        while (!text.empty() && IsBlank(text.front())) {
            text.remove_prefix(1);
        }
        while (!text.empty() && IsBlank(text.back())) {
            text.remove_suffix(1);
        }
        return text;
    }

    // Latin letters in lower case; everything else as it is.
    static std::string ToLower(std::string_view text) {
        std::string lower(text);
        std::ranges::transform(lower, lower.begin(), [](char letter) { return ToLower(letter); });
        return lower;
    }

    // Whether `text` is `lowercase` but for the case of its Latin letters.
    static constexpr bool EqualsIgnoreCase(std::string_view text, std::string_view lowercase) {
        return std::ranges::equal(text, lowercase, {}, [](char letter) { return ToLower(letter); });
    }

    // The whole number that `text` is, all of it, or nothing.
    template <std::integral Number>
    static constexpr std::optional<Number> ParseNumber(std::string_view text) {
        Number value{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) {
            return std::nullopt;
        }
        return value;
    }

    // The number that `text` is, all of it, or nothing. Not every C++ library reads floats
    // with `std::from_chars` yet.
    static std::optional<float> ParseFloat(std::string_view text) {
        const std::string copy(text);
        char* end = nullptr;
        const float value = std::strtof(copy.c_str(), &end);
        if (copy.empty() || end != copy.c_str() + copy.size()) {
            return std::nullopt;
        }
        return value;
    }

    // Adds a character to a text in UTF-8.
    static constexpr void AppendUtf8(std::string& text, char32_t character) {
        const auto byte = [&text](char32_t bits) { text.push_back(static_cast<char>(bits)); };
        if (character < 0x80) {
            byte(character);
        } else if (character < 0x800) {
            byte(0xC0 | character >> 6);
            byte(0x80 | (character & 0x3F));
        } else if (character < 0x10000) {
            byte(0xE0 | character >> 12);
            byte(0x80 | (character >> 6 & 0x3F));
            byte(0x80 | (character & 0x3F));
        } else {
            byte(0xF0 | character >> 18);
            byte(0x80 | (character >> 12 & 0x3F));
            byte(0x80 | (character >> 6 & 0x3F));
            byte(0x80 | (character & 0x3F));
        }
    }

    static constexpr std::string ToUtf8(char32_t character) {
        std::string text;
        AppendUtf8(text, character);
        return text;
    }

    // A text as the hexadecimal digits of its bytes, two each.
    static std::string ToHex(std::string_view text) {
        static constexpr char HexDigits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(text.size() * 2);
        for (const char letter : text) {
            const auto byte = static_cast<unsigned char>(letter);
            hex.push_back(HexDigits[byte >> 4]);
            hex.push_back(HexDigits[byte & 15]);
        }
        return hex;
    }

    // The text that hexadecimal digits spell, or nothing when some of them are no digits.
    static std::optional<std::string> FromHex(std::string_view hex) {
        const auto digit = [](char letter) -> int {
            if (letter >= '0' && letter <= '9') {
                return letter - '0';
            }
            if (letter >= 'a' && letter <= 'f') {
                return letter - 'a' + 10;
            }
            if (letter >= 'A' && letter <= 'F') {
                return letter - 'A' + 10;
            }
            return -1;
        };
        std::string text;
        text.reserve(hex.size() / 2);
        for (size_t index = 0; index + 1 < hex.size(); index += 2) {
            const int high = digit(hex[index]), low = digit(hex[index + 1]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            text.push_back(static_cast<char>(high << 4 | low));
        }
        return text;
    }

    // A file's contents, all of them.
    static Result<std::string> ReadFile(const std::string& path) {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) {
            return Err("cannot read {}: {}", path, std::strerror(errno));
        }
        std::string data;
        char chunk[4096];
        for (size_t read = 0; (read = std::fread(chunk, 1, sizeof chunk, file)) > 0;) {
            data.append(chunk, read);
        }
        const int failure = std::ferror(file) ? errno : 0;
        std::fclose(file);
        if (failure != 0) {
            return Err("cannot read {}: {}", path, std::strerror(failure));
        }
        return data;
    }

    // The value of a `Result` that is known to hold one: what `TRY` hands on.
    template <class T>
    static T TakeResult(Result<T>&& result) {
        return std::move(*result);
    }

    static void TakeResult(Result<void>&&) {
    }
};

#if defined(__clang__) || defined(__GNUC__)
// The value of a `Result`, or a return from the enclosing function with its error.
#define TRY(expression) \
    ({ \
        auto&& tryResult = (expression); \
        if (!tryResult) \
            return std::unexpected(std::move(tryResult).error()); \
        ::Utils::TakeResult(std::move(tryResult)); \
    })
#endif
