#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/items/details/definition.h"
#include "../build_data/items/details/item_detail_catalog.h"
#include "../build_data/items/item_catalog.h"
#include "../build_data/runtime.h"
#include "runtime.h"
#include "state.h"

namespace sunrise::state {
namespace runtime::detail {

namespace authored_inventory = account::inventory;
namespace family4_loadout = middleware::datagen::family4::loadout;

/** Stable location of one character-owned item inside authored State. */
struct CharacterItemLocation {
    std::size_t index{};
    bool equipped{};
};

/** Stable resolved position of one item inside the Family4 loadout. */
struct ResolvedPosition {
    std::uint16_t inventoryRow{};
    std::uint8_t equipmentSlot{};
    bool equipped{};
    std::int32_t mutationSerial{};
};

void report_equipment(std::string_view stage,
                      std::string_view result,
                      EquipmentMutationKind kind,
                      std::uint64_t characterSoid,
                      std::uint64_t previousSoid,
                      std::uint64_t requestedSoid,
                      std::size_t equipmentIndex,
                      std::size_t inventoryIndex,
                      std::uint8_t nativeSlot,
                      std::size_t movedItemCount,
                      std::uint32_t previousHash,
                      std::uint32_t requestedHash) noexcept;

void report_acquisition(std::string_view stage,
                        std::string_view result,
                        std::string_view reason,
                        std::uint32_t definitionHash,
                        std::uint64_t characterSoid,
                        std::uint64_t instanceSoid,
                        std::size_t inventoryIndex,
                        std::uint16_t inventoryRow,
                        std::uint8_t equipmentSlot,
                        std::uint32_t nextInventorySerial) noexcept;

void report_profile_acquisition(std::string_view stage,
                                std::string_view result,
                                std::string_view reason,
                                std::uint32_t definitionHash,
                                std::uint64_t accountSoid,
                                std::uint64_t instanceSoid,
                                std::uint8_t bucketId,
                                std::size_t profileIndex,
                                std::size_t itemCount,
                                std::int32_t previousQuantity,
                                std::int32_t acquiredQuantity,
                                bool appended) noexcept;

void report_dismantle(std::string_view stage,
                      std::string_view result,
                      std::string_view reason,
                      std::uint32_t definitionHash,
                      std::uint64_t characterSoid,
                      std::uint64_t instanceSoid,
                      std::size_t inventoryIndex,
                      std::uint16_t inventoryRow,
                      std::uint8_t equipmentSlot,
                      std::size_t movedItemCount,
                      std::uint32_t nextInventorySerial) noexcept;

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

void report_item_state(std::string_view stage,
                       std::string_view result,
                       std::string_view reason,
                       std::uint64_t characterSoid,
                       std::uint64_t instanceSoid,
                       std::uint16_t definitionIndex,
                       std::uint32_t beforeFlags,
                       std::uint32_t afterFlags,
                       bool equipped,
                       std::size_t itemIndex) noexcept;

/* -------------------------------------------------------------------------- */
/* Profile inventory helpers                                                  */
/* -------------------------------------------------------------------------- */

[[nodiscard]] bool
same_profile_inventory(const AccountState& account,
                       const std::array<authored_inventory::ProfileItem,
                                        authored_inventory::kProfileItemCapacity>& items,
                       std::size_t count) noexcept;

[[nodiscard]] bool same_profile_views(
    const std::array<authored_inventory::ProfileItem, authored_inventory::kProfileItemCapacity>&
        left,
    std::size_t leftCount,
    const std::array<authored_inventory::ProfileItem, authored_inventory::kProfileItemCapacity>&
        right,
    std::size_t rightCount) noexcept;

[[nodiscard]] bool valid_profile_inventory(const AccountState& account) noexcept;

/* -------------------------------------------------------------------------- */
/* Character/item identity helpers                                            */
/* -------------------------------------------------------------------------- */

[[nodiscard]] std::uint32_t character_item_definition_hash(const CharacterState& character,
                                                           std::uint64_t instanceSoid) noexcept;

[[nodiscard]] bool identity_uses_soid(const AccountState& account, std::uint64_t soid) noexcept;

[[nodiscard]] bool next_item_instance_soid(const AccountState& account,
                                           std::uint64_t& output) noexcept;

[[nodiscard]] bool next_profile_item_instance_soid(const AccountState& account,
                                                   std::uint64_t& output) noexcept;

[[nodiscard]] std::int32_t acquisition_level(const CharacterState& character) noexcept;

/* -------------------------------------------------------------------------- */
/* Loadout helpers                                                            */
/* -------------------------------------------------------------------------- */

[[nodiscard]] bool find_acquired_row(const family4_loadout::ResolvedLoadout& loadout,
                                     std::uint64_t instanceSoid,
                                     std::uint16_t& inventoryRow,
                                     std::uint8_t& equipmentSlot) noexcept;

[[nodiscard]] bool find_unequipped_row(const family4_loadout::ResolvedLoadout& loadout,
                                       std::uint64_t instanceSoid,
                                       std::uint16_t& inventoryRow,
                                       std::uint8_t& equipmentSlot) noexcept;

[[nodiscard]] bool loadout_contains(const family4_loadout::ResolvedLoadout& loadout,
                                    std::uint64_t instanceSoid) noexcept;

/* -------------------------------------------------------------------------- */
/* Equipment helpers                                                          */
/* -------------------------------------------------------------------------- */

[[nodiscard]] bool native_equipment_slot(const authored_inventory::Item& item,
                                         std::uint8_t& slot) noexcept;

[[nodiscard]] bool inventory_bucket_id(const authored_inventory::Item& item,
                                       std::uint8_t& bucketId) noexcept;

[[nodiscard]] bool semantic_equipment_slot(std::uint8_t nativeSlot,
                                           std::size_t& semanticIndex) noexcept;

/* -------------------------------------------------------------------------- */
/* Character item helpers                                                     */
/* -------------------------------------------------------------------------- */

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

[[nodiscard]] bool
finalize_equipment_transition(const AccountState& account,
                              std::size_t characterIndex,
                              std::uint64_t requestedInstanceSoid,
                              EquipmentMutationKind kind,
                              std::uint8_t expectedNativeSlot,
                              const family4_loadout::ResolvedLoadout& beforeLoadout,
                              CharacterState& after,
                              std::size_t& movedItemCount) noexcept;

[[nodiscard]] bool same_character(const CharacterState& left, const CharacterState& right) noexcept;

/* -------------------------------------------------------------------------- */
/* Material helpers                                                           */
/* -------------------------------------------------------------------------- */

/**
 * Applies one resolved native material requirement set to a copied account.
 *
 * Requirements which are marked deleteOnAction consume profile stacks.
 * Requirements which are not marked deleteOnAction only gate the action
 * by the available quantity.
 */
template <typename Requirement>
[[nodiscard]] bool apply_material_requirements(const AccountState& before,
                                               std::span<const Requirement> requirements,
                                               AccountState& after,
                                               bool& changed) noexcept;

/** Applies the material requirements belonging to one Collections definition. */
[[nodiscard]] bool
apply_collection_materials(const AccountState& before,
                           const build_data::collectibles::Definition& collectible,
                           AccountState& after,
                           bool& changed) noexcept;

/** Applies one installed action-cost requirement set. */
[[nodiscard]] bool
apply_action_materials(const AccountState& before,
                       const build_data::material_requirements::Definition& definition,
                       AccountState& after,
                       bool& changed) noexcept;

/* -------------------------------------------------------------------------- */
/* Socket helpers                                                             */
/* -------------------------------------------------------------------------- */

[[nodiscard]] bool holds_plug_source(const AccountState& account,
                                     std::uint32_t definitionHash) noexcept;

[[nodiscard]] bool spend_plug_source(AccountState& account, std::uint32_t definitionHash) noexcept;

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

/* -------------------------------------------------------------------------- */
/* Dismantle helpers                                                          */
/* -------------------------------------------------------------------------- */

[[nodiscard]] bool stage_item_dismantle(const AccountState& account,
                                        std::size_t characterIndex,
                                        std::uint64_t instanceSoid,
                                        PendingItemDismantle& mutation) noexcept;

[[nodiscard]] bool same_dismantle_transition(const PendingItemDismantle& left,
                                             const PendingItemDismantle& right) noexcept;

[[nodiscard]] bool materialize_item_dismantle(const AccountState& current,
                                              const PendingItemDismantle& mutation,
                                              AccountState& after) noexcept;

/* -------------------------------------------------------------------------- */
/* Profile acquisition helpers                                                */
/* -------------------------------------------------------------------------- */

[[nodiscard]] bool
valid_profile_mutation_shape(const PendingProfileItemAcquisition& mutation) noexcept;

[[nodiscard]] bool materialize_profile_acquisition(const AccountState& current,
                                                   const PendingProfileItemAcquisition& mutation,
                                                   AccountState& after) noexcept;

} // namespace runtime::detail
} // namespace sunrise::state
