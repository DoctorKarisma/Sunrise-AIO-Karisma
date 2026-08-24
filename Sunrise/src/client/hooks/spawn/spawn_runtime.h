#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../spawn/spawn_keybind_store.h"

namespace sunrise::client::hooks::spawn {

enum class Origin : std::uint8_t {
    player,
    crosshair,
};

struct Settings {
    float lift{1.0F};
    float rayDistance{100.0F};
    float scale{1.0F};
    std::array<float, 3> offset{};
    std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
    bool useCameraRotation{};
    bool overrideRotation{};
};

[[nodiscard]] bool install() noexcept;
void uninstall() noexcept;

[[nodiscard]] bool ready() noexcept;
[[nodiscard]] bool busy() noexcept;
[[nodiscard]] bool is_tag_resident(std::uint32_t tag) noexcept;
[[nodiscard]] bool object_type(std::uint32_t tag, std::uint8_t& type) noexcept;

[[nodiscard]] bool
request(std::uint32_t tag, Origin origin, std::uint32_t amount, const Settings& settings) noexcept;

[[nodiscard]] bool request_line(std::span<const std::uint32_t> tags,
                                Origin origin,
                                std::uint32_t itemsPerRow,
                                float spacing,
                                const Settings& settings) noexcept;

void configure_shortcut(client::spawn::Action action,
                        std::uint32_t tag,
                        std::uint32_t amount,
                        const Settings& settings) noexcept;

void cancel() noexcept;

/** Settings for the ambient world populator. */
struct PopulationSettings {
    bool enabled{};
    std::uint32_t target{12};
    float minimumRadius{18.0F};
    float maximumRadius{55.0F};
    float forgetRadius{140.0F};
    std::uint32_t intervalMs{600};
    float lift{0.5F};
    float scale{1.0F};
    bool useMap{};
    std::uint32_t respawnDelayMs{45000};
    bool snapToGround{true};
    bool autoOnLoad{};
};

/** One recorded map point the population system can fill. */
struct PopulationPoint {
    std::uint32_t tag{};
    std::array<float, 3> position{};
};

enum class PlacementOutcome : std::uint8_t {
    idle,
    placed,
    disabled,
    noPlayer,
    noPoints,
    atTarget,
    noneInRange,
    notResident,
    noGround,
    spawnFailed,
};

struct PopulationStatus {
    std::size_t points{};
    std::size_t live{};
    float nearest{-1.0F};
    PlacementOutcome last{PlacementOutcome::idle};
    std::array<float, 3> player{};
    std::array<float, 3> placed{};
    float snapped{};
};

void configure_population(const PopulationSettings& settings) noexcept;
[[nodiscard]] PopulationSettings population() noexcept;
void set_population_tags(std::span<const std::uint32_t> tags) noexcept;
[[nodiscard]] std::size_t population_live() noexcept;
[[nodiscard]] std::size_t population_source_count() noexcept;

void set_population_points(std::span<const PopulationPoint> points) noexcept;
[[nodiscard]] std::size_t population_point_count() noexcept;
[[nodiscard]] PopulationStatus population_status() noexcept;

void clear_population_tracking() noexcept;

} // namespace sunrise::client::hooks::spawn
