#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::client::spawn {

inline constexpr std::uint32_t kNoKey = 0;

enum class Action : std::uint8_t {
    mainPlayer,
    mainCrosshair,
    projectilePlayer,
    projectileCrosshair,
    lootPlayer,
    lootCrosshair,
    count,
};

inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::count);
inline constexpr std::uint64_t kDefaultHiddenMainTypes =
    (1ULL << 2) | (1ULL << 3) | (1ULL << 4) | (1ULL << 5) | (1ULL << 6) | (1ULL << 7) | (1ULL << 9)
    | (1ULL << 14) | (1ULL << 22) | (1ULL << 23) | (1ULL << 24) | (1ULL << 25) | (1ULL << 28)
    | (1ULL << 63);

struct Keybinds {
    std::array<std::uint32_t, kActionCount> virtualKeys{};
    std::uint64_t hiddenMainTypes{kDefaultHiddenMainTypes};
};

struct MapPoint {
    std::uint32_t tag{};
    std::array<float, 3> position{};
};

inline constexpr std::size_t kMapCapacity = 2048;

void initialize(void* module) noexcept;
void shutdown() noexcept;
[[nodiscard]] Keybinds get() noexcept;
bool publish(const Keybinds& keybinds) noexcept;

[[nodiscard]] bool load_map(std::string_view destination) noexcept;
[[nodiscard]] bool save_map(std::string_view destination) noexcept;
void clear_map() noexcept;
[[nodiscard]] bool add_map_point(const MapPoint& point) noexcept;
[[nodiscard]] bool remove_last_map_point() noexcept;
[[nodiscard]] std::size_t map_size() noexcept;
[[nodiscard]] std::size_t copy_map(std::span<MapPoint> output) noexcept;

} // namespace sunrise::client::spawn
