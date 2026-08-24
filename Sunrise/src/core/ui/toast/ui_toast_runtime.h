#pragma once

#include <cstdint>
#include <string_view>

namespace sunrise::core::ui::toast {

enum class Type : std::uint8_t {
    info,
    success,
    warning,
    warn = warning,
    error,
};

/**
 * Pushes a new toast notification. Thread-safe and non-blocking.
 * @param message Text to display.
 * @param type Toast type (controls icon/color).
 * @param durationMs How long the toast stays on screen before fading out (default 2500ms).
 */
void post(std::string_view message, Type type = Type::info, std::uint32_t durationMs = 2500) noexcept;
void post(Type type, std::string_view message, std::uint32_t durationMs = 2500) noexcept;
void post(Type type, std::string_view title, std::string_view message, std::uint32_t durationMs = 2500) noexcept;

/**
 * Draws active toasts on the screen (bottom-right corner with smooth fade).
 * @return True when at least one toast was drawn.
 */
[[nodiscard]] bool draw() noexcept;

/** Clears all queued and active toasts. */
void clear() noexcept;

} // namespace sunrise::core::ui::toast
