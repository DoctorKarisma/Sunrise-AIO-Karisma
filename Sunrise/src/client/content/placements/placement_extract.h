#pragma once

#include <cstddef>
#include <string_view>

namespace sunrise::client::content::placements {

struct ExtractResult {
    std::size_t placements{};
    std::size_t kept{};
    std::size_t notCombatant{};
    std::size_t notResident{};
    std::size_t overflowed{};
    std::size_t notPublic{};
    bool budgetHit{};
};

[[nodiscard]] bool extract(std::string_view destination,
                           bool combatantsOnly,
                           bool publicOnly,
                           ExtractResult& result) noexcept;

} // namespace sunrise::client::content::placements
