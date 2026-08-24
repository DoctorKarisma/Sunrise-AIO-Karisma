/**
 * Profile-inventory helpers: the account-wide stacks, their checks, and the material
 * costs an action charges against them.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <utility>

#include "../../core/logging/log.h"
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "runtime.h"
#include "state.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace runtime::detail {

namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;
namespace inventory_buckets = build_data::inventory::buckets;
namespace family4_loadout = middleware::datagen::family4::loadout;

/** Writes one bounded profile-stack acquisition checkpoint. */
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
                                bool appended) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=profile_acquire stage=%.*s result=%.*s reason=%.*s definition_hash=0x%08X "
        "account=0x%llX instance=0x%llX bucket=%u profile_index=%zu item_count=%zu "
        "quantity_before=%d "
        "quantity_after=%d appended=%u",
        static_cast<int>(stage.size()),
        stage.data(),
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        definitionHash,
        static_cast<unsigned long long>(accountSoid),
        static_cast<unsigned long long>(instanceSoid),
        static_cast<unsigned>(bucketId),
        profileIndex,
        itemCount,
        previousQuantity,
        acquiredQuantity,
        static_cast<unsigned>(appended));
    if (count > 0) {
        core::log::write(core::log::Channel::state,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** @return True when two profile stack rows carry identical authored values. */
[[nodiscard]] bool same_profile_item(const authored_inventory::ProfileItem& left,
                                     const authored_inventory::ProfileItem& right) noexcept {
    return left.instanceSoid == right.instanceSoid && left.definitionHash == right.definitionHash
           && left.quantity == right.quantity && left.mutationSerial == right.mutationSerial;
}

/** @return True when a complete fixed profile inventory equals one captured view. */
[[nodiscard]] bool
same_profile_inventory(const AccountState& account,
                       const std::array<authored_inventory::ProfileItem,
                                        authored_inventory::kProfileItemCapacity>& expected,
                       std::size_t expectedCount) noexcept {
    if (account.profileItemCount != expectedCount) {
        return false;
    }
    for (std::size_t index = 0; index < account.profileItems.size(); ++index) {
        if (!same_profile_item(account.profileItems[index], expected[index])) {
            return false;
        }
    }
    return true;
}

/** @return True when two fixed profile views, including their empty tails, are identical. */
[[nodiscard]] bool same_profile_views(
    const std::array<authored_inventory::ProfileItem, authored_inventory::kProfileItemCapacity>&
        left,
    std::size_t leftCount,
    const std::array<authored_inventory::ProfileItem, authored_inventory::kProfileItemCapacity>&
        right,
    std::size_t rightCount) noexcept {
    if (leftCount != rightCount) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!same_profile_item(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

/**
 * Checks every dense profile stack and mirrors the account encoder's bucket-row placement.
 * This keeps State rejection independent of whether a later push happens to have scratch space.
 */
[[nodiscard]] bool valid_profile_inventory(const AccountState& account) noexcept {
    if (account.profileItemCount > account.profileItems.size()) {
        return false;
    }
    // The bucket identity is one byte on the wire, so 256 covers every value one can carry.
    constexpr std::size_t kBucketIdentityCapacity = 256;
    std::array<std::uint16_t, kBucketIdentityCapacity> taken{};
    std::array<bool, inventory_buckets::kProfileSlotCapacity> occupied{};
    std::size_t actionSourceCount = 0;
    for (std::size_t index = 0; index < account.profileItems.size(); ++index) {
        const authored_inventory::ProfileItem& item = account.profileItems[index];
        if (index >= account.profileItemCount) {
            if (item.instanceSoid != 0 || item.definitionHash != 0 || item.quantity != 0
                || item.mutationSerial != 0) {
                return false;
            }
            continue;
        }
        build_data::items::Definition definition{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (item.quantity <= 0 || item.mutationSerial < 0
            || !build_data::find_item_definition_hash(item.definitionHash, definition)
            || definition.definitionHash != item.definitionHash
            || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
            || detail.definitionIndex != definition.definitionIndex
            || detail.definitionHash != definition.definitionHash
            || detail.bucketId != definition.bucketId
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
            || !build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
            || bucket.arraySelector != inventory_buckets::ArraySelector::profile) {
            return false;
        }
        const bool actionSource =
            build_data::is_profile_action_source(definition.definitionIndex, definition.bucketId);
        if (actionSource != (item.instanceSoid != 0)
            || (actionSource
                && ++actionSourceCount > authored_inventory::kProfileActionSourceCapacity)) {
            return false;
        }
        const std::uint16_t used = taken[definition.bucketId];
        if (used >= bucket.slotCount) {
            return false;
        }
        const std::size_t row = static_cast<std::size_t>(bucket.firstSlot) + used;
        if (row >= occupied.size() || occupied[row]) {
            return false;
        }
        occupied[row] = true;
        taken[definition.bucketId] = static_cast<std::uint16_t>(used + 1U);
    }
    return true;
}

/** Resolved, aggregated material charge for one installed native requirement set. */
struct MaterialCharge {
    std::uint32_t definitionHash{};
    std::uint64_t quantity{};
    bool deleteOnAction{};
};

/**
 * Validates one native material requirement set and applies its deletions to a copied account.
 * Requirements which are not deleted still gate the action by balance. Material rows must be
 * ordinary non-resident profile stacks; removing an instance-backed action source would also owe
 * a resident release and is deliberately rejected here.
 */
template <typename Requirement>
[[nodiscard]] bool apply_material_requirements(const AccountState& before,
                                               std::span<const Requirement> requirements,
                                               AccountState& after,
                                               bool& changed) noexcept {
    after = before;
    changed = false;
    if (requirements.size() > build_data::material_requirements::kRequirementCapacity) {
        return false;
    }

    std::array<MaterialCharge, build_data::material_requirements::kRequirementCapacity> charges{};
    std::size_t chargeCount = 0;
    for (const Requirement& requirement : requirements) {
        if (requirement.quantity == 0) {
            continue;
        }
        build_data::items::Definition definition{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (requirement.itemDefinitionIndex
                == build_data::material_requirements::kUnavailableItemDefinitionIndex
            || !build_data::find_item_definition_index(requirement.itemDefinitionIndex, definition)
            || definition.definitionIndex != requirement.itemDefinitionIndex
            || !build_data::find_configured_item_detail(requirement.itemDefinitionIndex, detail)
            || detail.definitionIndex != requirement.itemDefinitionIndex
            || detail.definitionHash != definition.definitionHash
            || detail.bucketId != definition.bucketId
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
            || !build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
            || bucket.arraySelector != inventory_buckets::ArraySelector::profile
            || build_data::is_profile_action_source(definition.definitionIndex,
                                                    definition.bucketId)) {
            return false;
        }
        std::size_t chargeIndex = chargeCount;
        for (std::size_t existing = 0; existing < chargeCount; ++existing) {
            if (charges[existing].definitionHash == definition.definitionHash
                && charges[existing].deleteOnAction == requirement.deleteOnAction) {
                chargeIndex = existing;
                break;
            }
        }
        if (chargeIndex == chargeCount) {
            if (chargeCount >= charges.size()) {
                return false;
            }
            charges[chargeCount].definitionHash = definition.definitionHash;
            charges[chargeCount].deleteOnAction = requirement.deleteOnAction;
            ++chargeCount;
        }
        if (charges[chargeIndex].quantity
            > (std::numeric_limits<std::uint64_t>::max)() - requirement.quantity) {
            return false;
        }
        charges[chargeIndex].quantity += requirement.quantity;
    }

    for (std::size_t charge = 0; charge < chargeCount; ++charge) {
        std::uint64_t available = 0;
        for (std::size_t index = 0; index < before.profileItemCount; ++index) {
            const authored_inventory::ProfileItem& item = before.profileItems[index];
            if (item.definitionHash != charges[charge].definitionHash) {
                continue;
            }
            if (item.instanceSoid != 0 || item.quantity <= 0
                || available > (std::numeric_limits<std::uint64_t>::max)()
                                   - static_cast<std::uint64_t>(item.quantity)) {
                return false;
            }
            available += static_cast<std::uint64_t>(item.quantity);
        }
        if (available < charges[charge].quantity) {
            return false;
        }
    }

    std::array<std::uint64_t, build_data::material_requirements::kRequirementCapacity> remaining{};
    bool hasDeletion = false;
    for (std::size_t charge = 0; charge < chargeCount; ++charge) {
        if (charges[charge].deleteOnAction) {
            remaining[charge] = charges[charge].quantity;
            hasDeletion = true;
        }
    }
    if (!hasDeletion) {
        return true;
    }

    std::array<authored_inventory::ProfileItem, authored_inventory::kProfileItemCapacity>
        compacted{};
    std::size_t compactedCount = 0;
    for (std::size_t index = 0; index < before.profileItemCount; ++index) {
        authored_inventory::ProfileItem item = before.profileItems[index];
        for (std::size_t charge = 0; charge < chargeCount; ++charge) {
            if (remaining[charge] == 0 || item.definitionHash != charges[charge].definitionHash) {
                continue;
            }
            const auto available = static_cast<std::uint64_t>(item.quantity);
            const auto consumed = (std::min)(available, remaining[charge]);
            item.quantity -= static_cast<std::int32_t>(consumed);
            remaining[charge] -= consumed;
        }
        if (item.quantity != 0) {
            if (compactedCount >= compacted.size()) {
                return false;
            }
            compacted[compactedCount++] = item;
        }
    }
    if (std::any_of(remaining.cbegin(),
                    remaining.cbegin() + static_cast<std::ptrdiff_t>(chargeCount),
                    [](std::uint64_t value) noexcept { return value != 0; })) {
        return false;
    }

    std::int32_t greatestMutationSerial = 0;
    for (std::size_t index = 0; index < before.profileItemCount; ++index) {
        greatestMutationSerial =
            (std::max)(greatestMutationSerial, before.profileItems[index].mutationSerial);
    }
    std::size_t changedRows = 0;
    for (std::size_t index = 0; index < compactedCount; ++index) {
        if (index >= before.profileItemCount
            || !same_profile_item(compacted[index], before.profileItems[index])) {
            ++changedRows;
        }
    }
    if (changedRows > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()
                                               - greatestMutationSerial)) {
        return false;
    }
    for (std::size_t index = 0; index < compactedCount; ++index) {
        if (index >= before.profileItemCount
            || !same_profile_item(compacted[index], before.profileItems[index])) {
            compacted[index].mutationSerial = ++greatestMutationSerial;
        }
    }
    after.profileItems = compacted;
    after.profileItemCount = compactedCount;
    changed = !same_profile_inventory(after, before.profileItems, before.profileItemCount);
    return changed && account::valid(after) && valid_profile_inventory(after);
}

/** Resolves one Collections row's installed cost without embedding any item or quantity policy. */
[[nodiscard]] bool
apply_collection_materials(const AccountState& before,
                           const build_data::collectibles::Definition& collectible,
                           AccountState& after,
                           bool& changed) noexcept {
    after = before;
    changed = false;
    if (collectible.materialRequirementCount == 0) {
        return collectible.materialRequirementSetIndex
                   == build_data::collectibles::kUnavailableMaterialRequirementSetIndex
               && collectible.materialRequirementSetHash == 0;
    }
    if (collectible.materialRequirementCount > collectible.materialRequirements.size()
        || collectible.materialRequirementSetIndex
               == build_data::collectibles::kUnavailableMaterialRequirementSetIndex
        || collectible.materialRequirementSetHash == 0) {
        return false;
    }
    return apply_material_requirements(
        before,
        std::span(collectible.materialRequirements)
            .first(static_cast<std::size_t>(collectible.materialRequirementCount)),
        after,
        changed);
}

/**
 * Answers whether the account holds one applicable stack of a socket action source.
 * @param account Account whose profile stacks are searched.
 * @param definitionHash Plug definition the Client asked to apply.
 * @return True when a profile stack of that definition holds at least one unit.
 */
[[nodiscard]] bool holds_plug_source(const AccountState& account,
                                     std::uint32_t definitionHash) noexcept {
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        const authored_inventory::ProfileItem& item = account.profileItems[index];
        if (item.definitionHash == definitionHash && item.quantity > 0) {
            return true;
        }
    }
    return false;
}

/**
 * Takes one unit of an owned socket action source, releasing the row when its last unit goes.
 *
 * An action source is an instanced profile row, so the authored-cost path cannot spend it. The
 * row keeps its identity and position while units remain, because the Client addresses it by that
 * identity. An emptied row is removed and the rows after it move up, which is the same shape the
 * authored-cost path leaves behind when a stack empties.
 *
 * @param account Account whose profile stacks are spent in place.
 * @param definitionHash Plug definition being applied.
 * @return True when one unit was taken.
 */
[[nodiscard]] bool spend_plug_source(AccountState& account, std::uint32_t definitionHash) noexcept {
    std::size_t row = account.profileItemCount;
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        if (account.profileItems[index].definitionHash == definitionHash
            && account.profileItems[index].quantity > 0) {
            row = index;
            break;
        }
    }
    if (row >= account.profileItemCount) {
        return false;
    }
    if (--account.profileItems[row].quantity > 0) {
        return true;
    }
    for (std::size_t index = row; index + 1U < account.profileItemCount; ++index) {
        account.profileItems[index] = account.profileItems[index + 1U];
    }
    account.profileItems[--account.profileItemCount] = {};
    return true;
}

/** Applies one dense installed action-cost set resolved from the selected plug or action row. */
[[nodiscard]] bool
apply_action_materials(const AccountState& before,
                       const build_data::material_requirements::Definition& definition,
                       AccountState& after,
                       bool& changed) noexcept {
    if (definition.requirementSetHash == 0
        || definition.requirementSetIndex == build_data::material_requirements::kUnavailableSetIndex
        || definition.requirementCount == 0
        || definition.requirementCount > definition.requirements.size()) {
        after = {};
        changed = false;
        return false;
    }
    for (std::size_t index = 0; index < definition.requirementCount; ++index) {
        if (definition.requirements[index].condition
            != build_data::material_requirements::kUnconditionalRequirement) {
            after = {};
            changed = false;
            return false;
        }
    }
    return apply_material_requirements(
        before,
        std::span(definition.requirements)
            .first(static_cast<std::size_t>(definition.requirementCount)),
        after,
        changed);
}

/** @return True when a pending profile acquisition carries canonical dense before/after images. */
[[nodiscard]] bool
valid_profile_mutation_shape(const PendingProfileItemAcquisition& mutation) noexcept {
    if (!mutation.prepared || mutation.accountSoid == 0
        || mutation.actionSource != (mutation.acquiredInstanceSoid != 0)
        || mutation.acquiredDefinitionHash == authored_inventory::kNoDefinitionHash
        || mutation.expectedItemCount > authored_inventory::kProfileItemCapacity
        || mutation.afterItemCount > authored_inventory::kProfileItemCapacity
        || mutation.profileIndex >= mutation.afterItemCount || mutation.previousQuantity < 0
        || mutation.acquiredQuantity <= mutation.previousQuantity
        || mutation.acquiredQuantity - mutation.previousQuantity != 1
        || mutation.previousMutationSerial < 0
        || mutation.acquiredMutationSerial <= mutation.previousMutationSerial) {
        return false;
    }
    if (mutation.appended) {
        if (mutation.afterItemCount == 0 || mutation.previousQuantity != 0) {
            return false;
        }
    } else if (mutation.previousQuantity == 0) {
        return false;
    }

    bool foundBeforeTarget = mutation.appended;
    for (std::size_t index = 0; index < mutation.beforeItems.size(); ++index) {
        const authored_inventory::ProfileItem& before = mutation.beforeItems[index];
        const authored_inventory::ProfileItem& after = mutation.afterItems[index];
        if (index < mutation.expectedItemCount
            && before.mutationSerial >= mutation.acquiredMutationSerial) {
            return false;
        }
        if (index >= mutation.expectedItemCount
            && (before.instanceSoid != 0 || before.definitionHash != 0 || before.quantity != 0
                || before.mutationSerial != 0)) {
            return false;
        }
        if (index >= mutation.afterItemCount
            && (after.instanceSoid != 0 || after.definitionHash != 0 || after.quantity != 0
                || after.mutationSerial != 0)) {
            return false;
        }
        if (!mutation.appended && index < mutation.expectedItemCount
            && before.instanceSoid == mutation.acquiredInstanceSoid
            && before.definitionHash == mutation.acquiredDefinitionHash
            && before.quantity == mutation.previousQuantity
            && before.mutationSerial == mutation.previousMutationSerial) {
            if (foundBeforeTarget) {
                return false;
            }
            foundBeforeTarget = true;
        }
    }
    const authored_inventory::ProfileItem& acquired = mutation.afterItems[mutation.profileIndex];
    return foundBeforeTarget && acquired.instanceSoid == mutation.acquiredInstanceSoid
           && acquired.definitionHash == mutation.acquiredDefinitionHash
           && acquired.quantity == mutation.acquiredQuantity
           && acquired.mutationSerial == mutation.acquiredMutationSerial;
}

/** Applies one validated pending profile after-image over a current, matching account. */
[[nodiscard]] bool materialize_profile_acquisition(const AccountState& current,
                                                   const PendingProfileItemAcquisition& mutation,
                                                   AccountState& after) noexcept {
    if (!valid_profile_mutation_shape(mutation) || current.primarySoid != mutation.accountSoid
        || !same_profile_inventory(current, mutation.beforeItems, mutation.expectedItemCount)) {
        return false;
    }
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    build_data::items::Definition item{};
    build_data::collectibles::Definition collectible{};
    // A vendor purchase names an item, never a collectible, and arrives with the sentinel. The
    // collectible steps are skipped for it rather than faked, exactly as prepare already does  - 
    // this is the same gate, and omitting it here is what took the connection down: prepare
    // succeeded, the reply went out saying so, then preview and commit both refused and the
    // Queuez frame was never staged.
    if (!build_data::find_item_definition_hash(mutation.acquiredDefinitionHash, item)) {
        return false;
    }
    if (mutation.collectibleIndex == build_data::collectibles::kNoCollectibleIndex) {
        // With no collectible to hold them, both cost fields must still be clear. This is the
        // rule `commit_item_acquisition` applies to the character path.
        if (mutation.materialRequirementSetHash != 0 || mutation.materialRequirementCount != 0) {
            return false;
        }
    } else if (!build_data::find_collectible_definition(mutation.collectibleIndex, collectible)
               || collectible.itemDefinitionIndex
                      == build_data::collectibles::kUnavailableItemDefinitionIndex
               || collectible.materialRequirementSetHash != mutation.materialRequirementSetHash
               || collectible.materialRequirementCount != mutation.materialRequirementCount
               || collectible.itemDefinitionIndex != item.definitionIndex) {
        return false;
    }
    if (!build_data::find_configured_item_detail(item.definitionIndex, detail)
        || detail.definitionHash != mutation.acquiredDefinitionHash
        || detail.definitionIndex != item.definitionIndex || detail.bucketId != item.bucketId
        || detail.bucketId != mutation.bucketId
        || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
        || detail.maxStackSize <= 0 || mutation.acquiredQuantity > detail.maxStackSize
        || !build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
        || bucket.arraySelector != inventory_buckets::ArraySelector::profile
        || build_data::is_profile_action_source(item.definitionIndex, item.bucketId)
               != mutation.actionSource) {
        return false;
    }
    after = current;
    after.profileItems = mutation.afterItems;
    after.profileItemCount = mutation.afterItemCount;
    return account::valid(after) && valid_profile_inventory(after);
}

} // namespace runtime::detail
} // namespace sunrise::state
