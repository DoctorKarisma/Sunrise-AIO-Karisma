#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/items/item_definitions.h"
#include "../build_data/items/item_details.h"
#include "runtime.h"
#include "state.h"

namespace sunrise::state {
namespace runtime::detail {

namespace authored_inventory = account::inventory;
namespace family4_loadout = middleware::datagen::family4::loadout;

void report_socket_plug(std::string_view stage,
                        std::string_view result,
                        std::string_view reason,
                        std::uint64_t characterSoid,
                        std::uint64_t targetInstanceSoid,
                        std::uint16_t targetDefinitionIndex,
                        std::uint8_t socketLane,
                        std::uint16_t plugDefinitionIndex,
                        std::uint8_t targetBucketId,
                        std::uint8_t plugBucketId,
                        bool targetEquipped,
                        std::size_t itemIndex) noexcept;

[[nodiscard]] bool same_profile_inventory(
    const AccountState& account,
    const std::array<authored_inventory::Item, authored_inventory::kProfileItemCapacity>& items,
    std::size_t count) noexcept;

[[nodiscard]] bool valid_profile_inventory(const AccountState& account) noexcept;

[[nodiscard]] bool same_stationary_item(const authored_inventory::Item& left,
                                        const authored_inventory::Item& right) noexcept;

[[nodiscard]] bool find_character_item_location(const CharacterState& character,
                                                std::uint64_t instanceSoid,
                                                CharacterItemLocation& location) noexcept;

[[nodiscard]] authored_inventory::Item*
character_item_at(CharacterState& character, const CharacterItemLocation& location) noexcept;

[[nodiscard]] const authored_inventory::Item*
character_item_at(const CharacterState& character, const CharacterItemLocation& location) noexcept;

[[nodiscard]] bool find_resolved_position(const family4_loadout::ResolvedLoadout& loadout,
                                          std::uint64_t instanceSoid,
                                          ResolvedPosition& position) noexcept;

[[nodiscard]] bool same_position(const ResolvedPosition& left,
                                 const ResolvedPosition& right) noexcept;

[[nodiscard]] bool finalize_equipment_transition(
    const AccountState& account,
    std::size_t characterIndex,
    std::uint64_t requestedInstanceSoid,
    EquipmentMutationKind kind,
    std::uint8_t expectedNativeSlot,
    const middleware::datagen::family4::loadout::ResolvedLoadout& beforeLoadout,
    CharacterState& after,
    std::size_t& movedItemCount) noexcept;

[[nodiscard]] bool same_character(const CharacterState& left, const CharacterState& right) noexcept;

/**
 * @param pinnedPlugHash For a rolled socket's apply or re-roll, the result plug an earlier
 *        staging rolled, so a re-staging reproduces the same after-image; 0 rolls afresh.
 */
[[nodiscard]] bool stage_socket_plug(const AccountState& snapshot,
                                     std::size_t characterIndex,
                                     std::uint64_t targetInstanceSoid,
                                     std::uint8_t socketLane,
                                     std::uint16_t plugDefinitionIndex,
                                     bool unrestricted,
                                     PendingSocketPlug& mutation,
                                     std::uint32_t pinnedPlugHash = 0) noexcept;

[[nodiscard]] bool stage_item_state(const AccountState& snapshot,
                                    std::size_t characterIndex,
                                    std::uint64_t targetInstanceSoid,
                                    std::uint16_t targetDefinitionIndex,
                                    std::uint32_t flags,
                                    PendingItemState& mutation) noexcept;

[[nodiscard]] bool stage_subclass_selection(const AccountState& snapshot,
                                            std::size_t characterIndex,
                                            std::uint64_t subclassInstanceSoid,
                                            std::uint8_t requestedEntry,
                                            PendingSubclassSelection& mutation) noexcept;

[[nodiscard]] bool next_item_instance_soid(const AccountState& account,
                                           std::uint64_t& output) noexcept;

[[nodiscard]] bool next_profile_item_instance_soid(const AccountState& account,
                                                   std::uint64_t& output) noexcept;

[[nodiscard]] std::int32_t acquisition_level(const CharacterState& character) noexcept;

[[nodiscard]] bool
find_acquired_row(const middleware::datagen::family4::loadout::ResolvedLoadout& loadout,
                  std::uint64_t instanceSoid,
                  std::uint16_t& inventoryRow,
                  std::uint8_t& equipmentSlot) noexcept;

[[nodiscard]] bool stage_item_dismantle(const AccountState& account,
                                        std::size_t characterIndex,
                                        std::uint64_t instanceSoid,
                                        PendingItemDismantle& mutation) noexcept;

[[nodiscard]] bool same_dismantle_transition(const PendingItemDismantle& left,
                                             const PendingItemDismantle& right) noexcept;

[[nodiscard]] bool materialize_item_dismantle(const AccountState& current,
                                              const PendingItemDismantle& mutation,
                                              AccountState& after) noexcept;

} // namespace runtime::detail
} // namespace sunrise::state
