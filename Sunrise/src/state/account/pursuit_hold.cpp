#include "pursuit_hold.h"

#include <array>
#include <cstddef>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../build_data/runtime.h"
#include "../runtime/runtime.h"
#include "account_state.h"

namespace sunrise::state::account {
namespace {

namespace detail_domain = build_data::items::details;

/**
 * Writes the fields the pursuit rule turns on, for one classified item.
 *
 * The rule has been wrong twice, in both directions: testing the equipment slot alone refused the
 * second of every consumable, and adding the instanced state let every quest through, because a
 * pursuit is non-instanced and so is marked stackable exactly as a consumable is. Neither mistake
 * was visible without knowing what the item actually declares, so it is now logged for every
 * classification and the rule can be settled from the values rather than from another guess.
 *
 * @param itemDefinitionIndex Item being classified.
 * @param detail Its configured detail row.
 */
void report_classification(std::uint16_t itemDefinitionIndex,
                           const detail_domain::Definition& detail) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=pursuit stage=classify item=%u bucket=%u slot=%d "
                                    "instanced=%u max_stack=%d",
                                    static_cast<unsigned>(itemDefinitionIndex),
                                    static_cast<unsigned>(detail.bucketId),
                                    detail.equipmentSlot.has_value()
                                        ? static_cast<int>(*detail.equipmentSlot)
                                        : -1,
                                    static_cast<unsigned>(detail.instancedDefinitionState),
                                    detail.maxStackSize);
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

} // namespace

/** Reports whether an item is a pursuit the selected character already holds. */
bool holds_pursuit(std::uint16_t itemDefinitionIndex) noexcept {
    return holds_pursuit(account_snapshot(), itemDefinitionIndex);
}

/** The same rule, against an account view the caller already holds. */
bool holds_pursuit(const AccountState& account, std::uint16_t itemDefinitionIndex) noexcept {
    detail_domain::Definition detail{};
    if (!build_data::find_configured_item_detail(itemDefinitionIndex, detail)) {
        return false;
    }
    report_classification(itemDefinitionIndex, detail);
    if (detail.equipmentSlot.has_value() || detail.maxStackSize > 1) {
        return false;
    }
    build_data::items::Definition definition{};
    if (!build_data::find_item_definition_index(itemDefinitionIndex, definition)) {
        return false;
    }
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        const CharacterState& character = account.characters[index];
        if (!character.selected) {
            continue;
        }
        for (std::size_t item = 0; item < character.inventory.count; ++item) {
            if (character.inventory.values[item].definitionHash == definition.definitionHash) {
                return true;
            }
        }
    }
    return false;
}

} // namespace sunrise::state::account
