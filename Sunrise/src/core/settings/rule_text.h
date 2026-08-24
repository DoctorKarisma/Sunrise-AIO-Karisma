#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::core::rule_text {

/**
 * Storage one rule file is read into.
 *
 * Deliberately far larger than any authored file needs. A file too large for its buffer is refused
 * whole rather than read in part, which silently turns off whatever it configures - and these are
 * hand-authored, where one more vendor is one more line. The cost is fixed storage that is written
 * once and never grows, so there is nothing to be gained by trimming it to fit today's files.
 */
inline constexpr std::size_t kRuleTextCapacity = 65536;

/** Line comments run from this character to the end of the line. */
inline constexpr char kCommentMark = '#';

/** @return True for a character `read_hex` accepts. */
[[nodiscard]] constexpr bool is_hex_digit(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')
           || (value >= 'A' && value <= 'F');
}

/** @return The value of one hex digit, or 0 for any other character. */
[[nodiscard]] constexpr std::uint32_t hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint32_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint32_t>(value - 'a') + 10U;
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<std::uint32_t>(value - 'A') + 10U;
    }
    return 0;
}

/**
 * Reads one authored rule file, a field at a time.
 *
 * Every rule file Sunrise authors has the same shape: `#` comments to end of line, blank lines
 * ignored, and rules made of hex and decimal fields separated by spaces. Each reader knows its own
 * field order and asks for fields in that order, so a hex field and a decimal one are never
 * confused - "100" is 256 read as hex and 100 read as decimal, and only the caller knows which the
 * file meant.
 *
 * Nothing here allocates or throws. The cursor borrows the caller's text and never runs past its
 * terminating NUL.
 */
class Cursor final {
public:
    /** @param text NUL-terminated rule text, borrowed for the cursor's lifetime. */
    explicit constexpr Cursor(const char* text) noexcept : at_{text} {}

    /**
     * @return True while the cursor sits on a field of the line it is already reading.
     *
     * A leading minus counts only in front of a digit, so it starts a negative decimal field and
     * never a hyphen inside a comment. Missing that is a silent sign flip rather than a refusal:
     * the minus would be stepped over as a separator and its digits read back as positive.
     */
    [[nodiscard]] constexpr bool at_field() const noexcept {
        return is_hex_digit(*at_) || (*at_ == '-' && at_[1] >= '0' && at_[1] <= '9');
    }

    /**
     * Advances to the next rule field, stepping over comments, blank lines and separators.
     * @return True when a field was found, false at the end of the text.
     */
    constexpr bool seek_field() noexcept {
        while (*at_ != '\0') {
            if (*at_ == kCommentMark) {
                while (*at_ != '\0' && *at_ != '\n') {
                    ++at_;
                }
                continue;
            }
            if (at_field()) {
                return true;
            }
            ++at_;
        }
        return false;
    }

    /** @return One hex field, and steps past it and any spaces after it. */
    constexpr std::uint32_t read_hex() noexcept {
        std::uint32_t value = 0;
        while (is_hex_digit(*at_)) {
            value = (value * 16U) + hex_value(*at_);
            ++at_;
        }
        skip_spaces();
        return value;
    }

    /**
     * @return One decimal field, and steps past it and any spaces after it.
     *
     * Digits accumulate wide and saturate. A field longer than the type can hold is a malformed
     * file rather than a rule, and overflowing a signed accumulator to find that out is undefined
     * behaviour, so the value stops at the end of the range instead of wrapping into a small or
     * negative one that would read as a plausible rule.
     */
    constexpr std::int32_t read_decimal() noexcept {
        const bool negative = *at_ == '-';
        if (negative) {
            ++at_;
        }
        constexpr std::int64_t kCeiling = 0x7FFFFFFF;
        std::int64_t value = 0;
        while (*at_ >= '0' && *at_ <= '9') {
            if (value <= kCeiling) {
                value = (value * 10) + (*at_ - '0');
            }
            ++at_;
        }
        skip_spaces();
        if (value > kCeiling) {
            value = kCeiling;
        }
        return static_cast<std::int32_t>(negative ? -value : value);
    }

private:
    constexpr void skip_spaces() noexcept {
        while (*at_ == ' ' || *at_ == '\t') {
            ++at_;
        }
    }

    const char* at_;
};

} // namespace sunrise::core::rule_text
