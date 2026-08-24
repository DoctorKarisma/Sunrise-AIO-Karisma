#pragma once

#include <cstdint>

#include "account_state.h"

namespace sunrise::state::account {

/**
 * Reports whether an item is a pursuit the selected character already holds.
 *
 * A pursuit - quest, bounty, token - is unique per character: the vendor offers it once and holds
 * it until it is completed or abandoned. Two kinds of item are deliberately not pursuits, because
 * holding several of each is legitimate:
 *
 * - **Gear**, told apart by carrying an equipment slot. Several of one weapon is normal.
 * - **Consumables and materials**, told apart by declaring a stack larger than one.
 *
 * The stack size is what separates the second group, *not* the instanced state. A pursuit carries
 * no instance data either, so it is marked stackable exactly as a consumable is, and testing that
 * flag let every quest through - three copies of one bounty in the tab. Every classification is
 * logged as `ev=pursuit stage=classify` so this stays settled by data rather than by assumption.
 *
 * This lives here rather than beside either caller because two of them apply it and they must not
 * drift: the Web Service refuses a duplicate grant with it, and the client's vendor-row gate hides
 * the row with it. A row still offered for an item the grant would refuse is the bug it exists to
 * prevent.
 *
 * @param itemDefinitionIndex Item to classify.
 * @return True when this is a pursuit the selected character already holds.
 */
[[nodiscard]] bool holds_pursuit(std::uint16_t itemDefinitionIndex) noexcept;

/**
 * The same rule, against an account view the caller already holds.
 *
 * Reading the account copies the whole of it, so a walk over many candidates - a bounty roll tests
 * every item in the vendor's pool - takes one view and reuses it rather than copying per candidate.
 *
 * @param account Account view to test against.
 * @param itemDefinitionIndex Item to classify.
 * @return True when this is a pursuit that view's selected character already holds.
 */
[[nodiscard]] bool holds_pursuit(const AccountState& account,
                                 std::uint16_t itemDefinitionIndex) noexcept;

} // namespace sunrise::state::account
