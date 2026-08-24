#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace sunrise::core::path {

/** Characters reserved for Windows extended paths. */
inline constexpr std::size_t kExtendedPathCapacity = 32768;

/** Fixed-size mutable Windows path. */
struct Buffer {
    std::array<wchar_t, kExtendedPathCapacity> chars{};
    std::size_t length{};
};

/** Replaces one path buffer with caller text. */
[[nodiscard]] bool assign(Buffer& path, std::wstring_view value) noexcept;

/** Resolves the directory containing one loaded module. */
[[nodiscard]] bool module_directory(void* module, Buffer& output) noexcept;

/** Resolves and creates the one Sunrise-owned generated-artifact directory. */
[[nodiscard]] bool artifact_directory(void* module, Buffer& output) noexcept;

/**
 * Resolves one Sunrise-owned file beside this DLL, creating any directory it needs.
 *
 * Callers get `<directory holding steam_api64.dll>\Sunrise\<relative>`, which is where
 * `settings.json`, `logs` and `cache` already live. This finds the module from its own address, so
 * nothing has to thread a handle down, and moving the whole install needs no rebuild.
 *
 * @param relative File name, optionally with one leading subdirectory such as `exports\x.txt`.
 * @param output Receives the full path.
 * @return True when the path fits and every directory in it exists or was created.
 */
[[nodiscard]] bool artifact_file(std::wstring_view relative, Buffer& output) noexcept;

/** Appends one suffix without exceeding fixed path storage. */
[[nodiscard]] bool append(Buffer& path, std::wstring_view suffix) noexcept;

/**
 * Reads one Sunrise-owned text file whole, into caller storage, terminated.
 *
 * Every authored rule file is read this way: opened fresh each time it is consulted, so editing one
 * takes effect without a relaunch, and never partially - a file too large for the caller's storage
 * is refused rather than truncated into a half-rule that would parse as something else.
 *
 * @param relative File name, resolved the same way `artifact_file` resolves one.
 * @param text Caller storage. Receives the file's bytes followed by a terminating NUL, so one byte
 *        of it is always spent on the terminator.
 * @return True only when the file opened, fitted, and held at least one byte.
 */
[[nodiscard]] bool read_artifact_text(std::wstring_view relative, std::span<char> text) noexcept;

} // namespace sunrise::core::path
