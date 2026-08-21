/** World-time rate control. The game multiplies elapsed time by a RIP-relative scalar. */

#include "world_speed.h"

#include <Windows.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../patterns/image_scan.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::hooks::world_speed {
namespace {

constexpr std::string_view kWorldSpeedText = "F3 0F 59 0D ? ? ? ? 0F 2F C8 72";
constexpr auto kWorldSpeed =
    patterns::signature<patterns::signature_length(kWorldSpeedText)>(kWorldSpeedText);
/** The four-byte RIP displacement starts after the MULSS opcode and ends with the instruction. */
constexpr std::size_t kDisplacementOffset = 4;
constexpr std::size_t kInstructionLength = 8;
/** Scalar used by an unmodified game. Dividing it increases the rate of game time. */
constexpr float kNormalScalar = 673200.0F;

float* g_scalar{};

/** Writes through the process API so a read-only image page can be changed safely. */
[[nodiscard]] bool write_scalar(float value) noexcept {
    if (g_scalar == nullptr) {
        return false;
    }
    DWORD oldProtection = 0;
    if (VirtualProtect(g_scalar, sizeof value, PAGE_READWRITE, &oldProtection) == FALSE) {
        return false;
    }
    std::memcpy(g_scalar, &value, sizeof value);
    DWORD ignored = 0;
    return VirtualProtect(g_scalar, sizeof value, oldProtection, &ignored) != FALSE;
}

/** Reports a failed resolve or write without making main-image activation fatal. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=world_speed stage=apply result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

} // namespace

/** Resolves the scalar addressed by the MULSS and applies the saved multiplier. */
bool install() noexcept {
    if (g_scalar != nullptr) {
        return true;
    }
    std::byte* const match =
        patterns::scan_main_image_unique(kWorldSpeed, "world_speed_scalar");
    if (match == nullptr) {
        return fail("signature");
    }
    g_scalar = reinterpret_cast<float*>(patterns::resolve_relative(
        match + kDisplacementOffset, match + kInstructionLength));
    if (!apply(client::player::get().worldSpeed)) {
        g_scalar = nullptr;
        return fail("write");
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=world_speed stage=install result=ok");
    return true;
}

/** Applies a game-speed multiplier by dividing the game's normal scalar. */
bool apply(float multiplier) noexcept {
    if (!std::isfinite(multiplier) || multiplier < client::player::kMinimumWorldSpeed
        || multiplier > client::player::kMaximumWorldSpeed) {
        return false;
    }
    return write_scalar(kNormalScalar / multiplier);
}

/** Restores normal game time before releasing the address. */
void uninstall() noexcept {
    if (g_scalar != nullptr) {
        (void)write_scalar(kNormalScalar);
        g_scalar = nullptr;
    }
}

} // namespace sunrise::client::hooks::world_speed
