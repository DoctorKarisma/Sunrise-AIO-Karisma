#pragma once

namespace sunrise::client::hooks::world_speed {

/** Resolves the game's world-time scalar and applies the saved multiplier. */
[[nodiscard]] bool install() noexcept;

/** Applies one multiplier to the resolved scalar. */
[[nodiscard]] bool apply(float multiplier) noexcept;

/** Restores the game's normal scalar and drops the resolved address. */
void uninstall() noexcept;

} // namespace sunrise::client::hooks::world_speed
