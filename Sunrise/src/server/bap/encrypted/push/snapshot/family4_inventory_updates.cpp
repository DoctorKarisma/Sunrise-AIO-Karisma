#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/datagen/family4/account/account_encoder.h"
#include "../../../../../middleware/datagen/family4/account/layout.h"
#include "../../../../../middleware/datagen/family4/character/character_encoder.h"
#include "../../../../../middleware/datagen/family4/character/layout.h"
#include "../../../../../middleware/datagen/family4/instance/instance_encoder.h"
#include "../../../../../middleware/datagen/family4/instance/layout.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

namespace family4_datagen = middleware::datagen::family4;

namespace {

namespace account_layout = middleware::datagen::family4::account::layout;

/**
 * The change ring the native account observer reads.
 *
 * It compares profile quantities but only draws pickup feedback for a row whose mutation serial
 * also appears here, so a gain the ring does not name lands silently. The bank is local to one
 * incremental upsert - an ordinary snapshot encodes it empty - while the row's own rising serial
 * stays persistent State.
 */
constexpr std::uint8_t kChangeKind = 1;
/** Clear policy bits leave the record enabled; the observer skips any other pair. */
constexpr std::uint16_t kChangeFlags = 0;

/** @return True when the ring carries no record, which is how every snapshot encodes it. */
[[nodiscard]] bool ring_is_empty(const account_layout::Object& accountObject) noexcept {
    const auto recordIsZero = [](const account_layout::ProfileInventoryChangeRecord& record) {
        return record.sequence == 0 && record.reserved == 0 && record.mutationSerial == 0
               && record.kind == 0 && record.reservedKind == 0 && record.flags == 0;
    };
    return accountObject.profileInventoryChanges.writeSlot == 0
           && accountObject.profileInventoryChanges.nextSequence == 0
           && std::all_of(accountObject.profileInventoryChanges.records.cbegin(),
                          accountObject.profileInventoryChanges.records.cend(),
                          recordIsZero);
}

/** Points one ring record at one profile row. */
void name_row(account_layout::ProfileInventoryChangeRecord& record,
              std::size_t sequence,
              std::int32_t mutationSerial) noexcept {
    record.sequence = static_cast<std::uint16_t>(sequence);
    record.mutationSerial = mutationSerial;
    record.kind = kChangeKind;
    record.flags = kChangeFlags;
}

/**
 * Names every row an exchange credited, so each gain is drawn and repeats accumulate.
 *
 * @param accountObject Encoded account object being upserted.
 * @param mutation Prepared exchange carrying the rows it credited.
 * @return Null on success, or the reason the ring could not be written.
 */
[[nodiscard]] const char* write_exchange_changes(
    account_layout::Object& accountObject,
    const state::PendingProfileItemAcquisition& mutation) noexcept {
    if (mutation.changeCount > accountObject.profileInventoryChanges.records.size()
        || !ring_is_empty(accountObject)) {
        return "exchange_inventory_change_state";
    }
    for (std::size_t change = 0; change < mutation.changeCount; ++change) {
        const state::ProfileStackChange& announced = mutation.changes[change];
        std::size_t matchedRows = 0;
        for (const auto& row : accountObject.profileItems) {
            if (row.mutationSerial != announced.mutationSerial) {
                continue;
            }
            if (row.quantity != announced.afterQuantity) {
                return "exchange_change_quantity";
            }
            ++matchedRows;
        }
        if (matchedRows != 1) {
            return "exchange_change_row";
        }
        name_row(accountObject.profileInventoryChanges.records[change],
                 change,
                 announced.mutationSerial);
    }
    accountObject.profileInventoryChanges.writeSlot =
        static_cast<std::uint16_t>(mutation.changeCount);
    accountObject.profileInventoryChanges.nextSequence =
        static_cast<std::uint16_t>(mutation.changeCount);
    return nullptr;
}

/**
 * Names the one row an ordinary acquisition added to or grew.
 *
 * @param accountObject Encoded account object being upserted.
 * @param mutation Prepared acquisition naming its acquired row.
 * @param acquiredRow Receives that row's position, for the checkpoint line.
 * @return Null on success, or the reason the ring could not be written.
 */
[[nodiscard]] const char* write_acquisition_change(
    account_layout::Object& accountObject,
    const state::PendingProfileItemAcquisition& mutation,
    std::size_t& acquiredRow) noexcept {
    acquiredRow = accountObject.profileItems.size();
    for (std::size_t row = 0; row < accountObject.profileItems.size(); ++row) {
        if (accountObject.profileItems[row].mutationSerial != mutation.acquiredMutationSerial) {
            continue;
        }
        if (acquiredRow != accountObject.profileItems.size()) {
            return "profile_acquire_row_duplicate";
        }
        acquiredRow = row;
    }
    if (acquiredRow >= accountObject.profileItems.size()
        || accountObject.profileItems[acquiredRow].quantity != mutation.acquiredQuantity
        || !ring_is_empty(accountObject)) {
        return "profile_acquire_inventory_change_state";
    }
    accountObject.profileInventoryChanges.writeSlot = 1;
    accountObject.profileInventoryChanges.nextSequence = 1;
    name_row(accountObject.profileInventoryChanges.records.front(),
             0,
             mutation.acquiredMutationSerial);
    return nullptr;
}

} // namespace

/** Builds a single full account-object upsert from an uncommitted profile-stack after-image. */
bool prepare_profile_item_acquisition(Scratch& scratch,
                                      const queuez::ProfileItemAcquisition& acquisition,
                                      const state::PendingProfileItemAcquisition& mutation,
                                      Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("profile_acquire_reservation");
    }
    if (!mutation.prepared || mutation.accountSoid == 0
        || mutation.accountSoid != acquisition.accountSoid
        || mutation.acquiredInstanceSoid != acquisition.acquiredInstanceSoid
        || mutation.actionSource != acquisition.actionSource
        || acquisition.appendedResident != (mutation.appended && mutation.actionSource)
        || acquisition.accountSoid != acquisition.after.family4RootSoid
        || acquisition.accountDefinitionId == 0
        || (acquisition.appendedResident && acquisition.itemInstanceDefinitionId == 0)) {
        return report_failure("profile_acquire_mutation");
    }
    state::AccountState account{};
    if (!state::preview_profile_item_acquisition(mutation, account)
        || account.primarySoid != acquisition.accountSoid || !state::account::valid(account)) {
        return report_failure("profile_acquire_account");
    }

    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::account::layout::kObjectSize > rawStorage.size()) {
        return report_failure("profile_acquire_account_storage");
    }
    const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
    if (!family4_datagen::account::encode(account, accountBytes)) {
        return report_failure("profile_acquire_account_encode");
    }

    auto& accountObject =
        *reinterpret_cast<family4_datagen::account::layout::Object*>(accountBytes.data());
    std::size_t acquiredRow = accountObject.profileItems.size();
    // An exchange names every row it credited; an ordinary acquisition names the one row it added
    // to or grew. Both write the same kind of record, which is what the observer draws.
    const char* const ringFailure =
        mutation.changeCount != 0
            ? write_exchange_changes(accountObject, mutation)
            : write_acquisition_change(accountObject, mutation, acquiredRow);
    if (ringFailure != nullptr) {
        clear_after(scratch, reservation);
        return report_failure(ringFailure);
    }
    const std::uint16_t reportedChangeSlot = accountObject.profileInventoryChanges.writeSlot;
    const std::uint16_t reportedChangeSequence =
        accountObject.profileInventoryChanges.nextSequence;

    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::account::layout::kObjectSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    const std::size_t accountObjectIndex = acquisition.appendedResident ? 1U : 0U;
    if (!append_object(scratch,
                       accountBytes,
                       acquisition.accountDefinitionId,
                       acquisition.accountSoid,
                       staged.objects[accountObjectIndex],
                       compressedExtent)) {
        return report_failure("profile_acquire_account_object");
    }
    std::size_t objectCount = 1;
    if (acquisition.appendedResident) {
        if (mutation.profileIndex >= account.profileItemCount) {
            return report_failure("profile_acquire_instance_index");
        }
        family4_datagen::instance::ResolvedInstance instance{};
        const state::account::inventory::ProfileItem& profileItem =
            account.profileItems[mutation.profileIndex];
        const auto instanceBytes = rawStorage.first(family4_datagen::instance::layout::kObjectSize);
        if (!resolve_profile_item_instance(profileItem, instance)
            || instance.instanceSoid != acquisition.acquiredInstanceSoid
            || !family4_datagen::instance::encode(instance, instanceBytes)
            || !append_object(scratch,
                              instanceBytes,
                              acquisition.itemInstanceDefinitionId,
                              acquisition.acquiredInstanceSoid,
                              staged.objects.front(),
                              compressedExtent)) {
            return report_failure("profile_acquire_instance_object");
        }
        objectCount = 2;
    }
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        acquisition.after.family4RootSoid,
        acquisition.after.family4Version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("profile_acquire_commit");
    }

    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=profile_acquire stage=account_object result=ok family_version=%d "
        "account=0x%llX definition=%u item_count=%zu definition_hash=0x%08X quantity=%d "
        "native_row=%zu mutation_serial=%d change_slot=%u change_next_sequence=%u "
        "change_kind=%u account_payload_bytes=%zu objects=%zu object_order=%s",
        acquisition.after.family4Version,
        static_cast<unsigned long long>(acquisition.accountSoid),
        acquisition.accountDefinitionId,
        mutation.afterItemCount,
        mutation.acquiredDefinitionHash,
        mutation.acquiredQuantity,
        acquiredRow,
        mutation.acquiredMutationSerial,
        static_cast<unsigned>(reportedChangeSlot),
        static_cast<unsigned>(reportedChangeSequence),
        static_cast<unsigned>(kChangeKind),
        prepared.family.objects[accountObjectIndex].payload.size(),
        objectCount,
        acquisition.appendedResident ? "item-account" : "account");
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return true;
}

/** Builds a new item-instance upsert before its character after-image. */
bool prepare_item_acquisition(Scratch& scratch,
                              const queuez::ItemAcquisition& acquisition,
                              const state::PendingItemAcquisition& mutation,
                              Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("acquire_reservation");
    }

    state::AccountState account{};
    if (!mutation.prepared || mutation.characterSoid == 0 || mutation.acquiredInstanceSoid == 0
        || mutation.accountSoid == 0 || mutation.accountSoid != acquisition.accountSoid
        || mutation.characterSoid != acquisition.characterSoid
        || mutation.acquiredInstanceSoid != acquisition.acquiredInstanceSoid
        || mutation.profileChanged != acquisition.updatesAccount
        || acquisition.accountSoid != acquisition.after.family4RootSoid
        || acquisition.accountDefinitionId == 0 || acquisition.after.family4ResidentCount == 0
        || acquisition.after.family4Residents[acquisition.after.family4ResidentCount - 1U]
                   .objectSoid
               != mutation.acquiredInstanceSoid
        || acquisition.after.family4Residents[acquisition.after.family4ResidentCount - 1U]
                   .definitionId
               != acquisition.itemInstanceDefinitionId) {
        return report_failure("acquire_mutation");
    }
    if (!state::preview_item_acquisition(mutation, account)
        || mutation.characterIndex >= account.characterCount
        || account.primarySoid != acquisition.accountSoid
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid) {
        return report_failure("acquire_account");
    }

    Resolved selected{};
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || *selectedIndex != mutation.characterIndex
        || !resolve(account, mutation.characterIndex, selected)
        || selected.characterObjectId != acquisition.characterDefinitionId
        || selected.itemInstanceObjectId != acquisition.itemInstanceDefinitionId) {
        return report_failure("acquire_selection");
    }

    family4_datagen::loadout::ResolvedInstances acquired{};
    std::int32_t acquiredMutationSerial = -1;
    for (std::size_t index = 0; index < selected.loadout.itemCount; ++index) {
        const family4_datagen::loadout::ResolvedItem& item = selected.loadout.items[index];
        if (item.instance.instanceSoid != mutation.acquiredInstanceSoid) {
            continue;
        }
        if (acquired.itemCount != 0 || item.equipped || item.inventoryRow != mutation.inventoryRow
            || item.equipmentSlot != mutation.equipmentSlot) {
            return report_failure("acquire_item_row");
        }
        acquired.items[0] = {item.equipmentSlot, item.instance};
        acquired.itemCount = 1;
        acquiredMutationSerial = item.mutationSerial;
    }
    if (acquired.itemCount != 1 || acquiredMutationSerial < 0) {
        return report_failure("acquire_item_missing");
    }

    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
        return report_failure("acquire_character_storage");
    }
    const auto characterBytes = rawStorage.first(family4_datagen::character::layout::kObjectSize);
    if (!family4_datagen::character::encode(account.characters[mutation.characterIndex],
                                            selected.loadout,
                                            selected.lightEvaluation,
                                            characterBytes)) {
        return report_failure("acquire_character_object");
    }

    // The native character-object observer requires a transient inventory-change record whose
    // serial matches the newly filled row. Without it the row is accepted, but the observer never
    // queues the acquisition feedback. Keep this record local to this one acquisition push; the
    // canonical encoder leaves the bank empty on every later snapshot.
    constexpr std::size_t kBitsPerFlagByte = 8;
    constexpr std::int32_t kEncodedOccupiedRowWatermark = 1;
    constexpr std::uint16_t kAcquisitionChangeSequence = 0;
    constexpr std::uint16_t kAcquisitionChangeNextWriteSlot = 1;
    constexpr std::uint16_t kAcquisitionChangeNextSequence = 1;
    constexpr std::uint8_t kAcquisitionChangeKind = 1;
    constexpr std::uint16_t kAcquisitionChangeFlags = 0;
    auto& characterObject =
        *reinterpret_cast<family4_datagen::character::layout::Object*>(characterBytes.data());
    const std::size_t acquiredRow = mutation.inventoryRow;
    if (acquiredRow >= characterObject.inventoryItems.size()
        || acquiredRow >= characterObject.instanceProgressWatermarks.size()) {
        clear_after(scratch, reservation);
        return report_failure("acquire_new_item_row");
    }
    const std::size_t newItemFlagIndex = acquiredRow / kBitsPerFlagByte;
    const std::byte newItemFlagMask = std::byte{1U} << (acquiredRow % kBitsPerFlagByte);
    const auto recordIsZero =
        [](const family4_datagen::character::layout::InventoryChangeRecord& record) noexcept {
            return record.sequence == 0 && record.reserved == 0 && record.mutationSerial == 0
                   && record.kind == 0 && record.reservedKind == 0 && record.flags == 0;
        };
    const bool unknownIsZero =
        std::all_of(characterObject.inventoryChangeUnknown.cbegin(),
                    characterObject.inventoryChangeUnknown.cend(),
                    [](std::byte value) noexcept { return value == std::byte{}; });
    const bool recordsAreZero = std::all_of(characterObject.inventoryChanges.records.cbegin(),
                                            characterObject.inventoryChanges.records.cend(),
                                            recordIsZero);
    const auto& acquiredInventoryRow = characterObject.inventoryItems[acquiredRow];
    if (newItemFlagIndex >= characterObject.newItemFlags.size()
        || acquiredInventoryRow.instanceSoid != mutation.acquiredInstanceSoid
        || acquiredInventoryRow.mutationSerial != acquiredMutationSerial
        || (characterObject.newItemFlags[newItemFlagIndex] & newItemFlagMask) != newItemFlagMask
        || characterObject.instanceProgressWatermarks[acquiredRow] != kEncodedOccupiedRowWatermark
        || !unknownIsZero || characterObject.inventoryChanges.writeSlot != 0
        || characterObject.inventoryChanges.nextSequence != 0 || !recordsAreZero) {
        clear_after(scratch, reservation);
        return report_failure("acquire_inventory_change_state");
    }
    characterObject.inventoryChanges.writeSlot = kAcquisitionChangeNextWriteSlot;
    characterObject.inventoryChanges.nextSequence = kAcquisitionChangeNextSequence;
    auto& acquisitionChange = characterObject.inventoryChanges.records.front();
    acquisitionChange.sequence = kAcquisitionChangeSequence;
    acquisitionChange.mutationSerial = acquiredMutationSerial;
    acquisitionChange.kind = kAcquisitionChangeKind;
    acquisitionChange.flags = kAcquisitionChangeFlags;
    if (!std::all_of(characterObject.inventoryChanges.records.cbegin() + 1,
                     characterObject.inventoryChanges.records.cend(),
                     recordIsZero)) {
        clear_after(scratch, reservation);
        return report_failure("acquire_inventory_change_records");
    }

    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::character::layout::kObjectSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    if (!append_object(scratch,
                       characterBytes,
                       acquisition.characterDefinitionId,
                       acquisition.characterSoid,
                       staged.objects[0],
                       compressedExtent)) {
        return report_failure("acquire_character_object");
    }

    std::size_t itemCursor = 0;
    if (!append_items(scratch,
                      rawStorage,
                      acquisition.itemInstanceDefinitionId,
                      acquired,
                      1,
                      staged,
                      itemCursor,
                      compressedExtent)
        || itemCursor != 1) {
        clear_after(scratch, reservation);
        return report_failure("acquire_item_object");
    }

    const auto& acquiredObject =
        *reinterpret_cast<const family4_datagen::instance::layout::Object*>(rawStorage.data());
    if (acquiredObject.instanceSoid != mutation.acquiredInstanceSoid
        || acquiredObject.roll.progress
               != family4_datagen::instance::layout::kInitialInstanceProgress) {
        clear_after(scratch, reservation);
        return report_failure("acquire_item_progress");
    }
    const std::int32_t acquiredInstanceProgress = acquiredObject.roll.progress;

    std::size_t objectCount = 2;
    if (acquisition.updatesAccount) {
        if (family4_datagen::account::layout::kObjectSize > rawStorage.size()) {
            clear_after(scratch, reservation);
            return report_failure("acquire_account_storage");
        }
        const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
        if (!family4_datagen::account::encode(account, accountBytes)
            || !append_object(scratch,
                              accountBytes,
                              acquisition.accountDefinitionId,
                              acquisition.accountSoid,
                              staged.objects[2],
                              compressedExtent)) {
            clear_after(scratch, reservation);
            return report_failure("acquire_account_object");
        }
        staged.rawClearSize =
            (std::max)(staged.rawClearSize,
                       reservation.rawWriteOffset + family4_datagen::account::layout::kObjectSize);
        objectCount = 3;
    }

    // Creation increments publish the dependency before the reference to it. Compression order is
    // irrelevant because each descriptor already owns its sealed span, so exchange only the wire
    // descriptors: new item first, then the character after-image. Dismantle deliberately uses the
    // inverse dependency order (drop the character reference, then release the item).
    std::swap(staged.objects[0], staged.objects[1]);

    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        acquisition.after.family4RootSoid,
        acquisition.after.family4Version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("acquire_commit");
    }

    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=acquire stage=family4_objects result=ok family_version=%d root=0x%llX "
        "character=0x%llX character_definition=%u instance=0x%llX item_definition=%u "
        "definition_hash=0x%08X inventory_row=%u equipment_slot=%u next_serial=%u objects=%zu "
        "order=%s new_item_flag=1 watermark=1 acquired_row_serial=%d "
        "inventory_change_write_slot=%u inventory_change_next_sequence=%u "
        "inventory_change_record=0 inventory_change_sequence=%u "
        "inventory_change_serial=%d inventory_change_kind=%u inventory_change_flags=%u "
        "instance_progress=%d",
        acquisition.after.family4Version,
        static_cast<unsigned long long>(acquisition.after.family4RootSoid),
        static_cast<unsigned long long>(acquisition.characterSoid),
        acquisition.characterDefinitionId,
        static_cast<unsigned long long>(acquisition.acquiredInstanceSoid),
        acquisition.itemInstanceDefinitionId,
        mutation.acquiredDefinitionHash,
        static_cast<unsigned>(mutation.inventoryRow),
        static_cast<unsigned>(mutation.equipmentSlot),
        mutation.afterCharacter.nextInventorySerial,
        objectCount,
        acquisition.updatesAccount ? "item_character_account" : "item_character",
        acquiredMutationSerial,
        static_cast<unsigned>(kAcquisitionChangeNextWriteSlot),
        static_cast<unsigned>(kAcquisitionChangeNextSequence),
        static_cast<unsigned>(kAcquisitionChangeSequence),
        acquiredMutationSerial,
        static_cast<unsigned>(kAcquisitionChangeKind),
        static_cast<unsigned>(kAcquisitionChangeFlags),
        acquiredInstanceProgress);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return true;
}

/** Builds character removal, instance release, and any profile-reward pickup descriptors. */
bool prepare_item_dismantle(Scratch& scratch,
                            const queuez::ItemDismantle& dismantle,
                            const state::PendingItemDismantle& mutation,
                            Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("dismantle_reservation");
    }

    state::AccountState account{};
    if (!mutation.prepared || mutation.characterSoid == 0 || mutation.dismantledInstanceSoid == 0
        || mutation.dismantledItem.instanceSoid != mutation.dismantledInstanceSoid
        || mutation.accountSoid != dismantle.accountSoid
        || mutation.characterSoid != dismantle.characterSoid
        || mutation.dismantledInstanceSoid != dismantle.dismantledInstanceSoid
        || mutation.profileChanged != dismantle.updatesAccount
        || mutation.rewardCount > state::kDismantleRewardCapacity
        || mutation.profileChanged != (mutation.rewardCount != 0)
        || dismantle.accountDefinitionId == 0 || dismantle.characterDefinitionId == 0
        || dismantle.itemInstanceDefinitionId == 0
        || !state::preview_item_dismantle(mutation, account)
        || mutation.characterIndex >= account.characterCount
        || account.primarySoid != dismantle.accountSoid
        || account.characters[mutation.characterIndex].soid != mutation.characterSoid) {
        return report_failure("dismantle_mutation");
    }

    Resolved selected{};
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || *selectedIndex != mutation.characterIndex
        || !resolve(account, mutation.characterIndex, selected)
        || selected.characterObjectId != dismantle.characterDefinitionId
        || selected.itemInstanceObjectId != dismantle.itemInstanceDefinitionId) {
        return report_failure("dismantle_selection");
    }
    for (std::size_t index = 0; index < selected.loadout.itemCount; ++index) {
        if (selected.loadout.items[index].instance.instanceSoid
            == mutation.dismantledInstanceSoid) {
            return report_failure("dismantle_item_present");
        }
    }

    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
        return report_failure("dismantle_character_storage");
    }
    const auto characterBytes = rawStorage.first(family4_datagen::character::layout::kObjectSize);
    if (!family4_datagen::character::encode(account.characters[mutation.characterIndex],
                                            selected.loadout,
                                            selected.lightEvaluation,
                                            characterBytes)) {
        return report_failure("dismantle_character_object");
    }

    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::character::layout::kObjectSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    if (!append_object(scratch,
                       characterBytes,
                       dismantle.characterDefinitionId,
                       dismantle.characterSoid,
                       staged.objects[0],
                       compressedExtent)) {
        return report_failure("dismantle_character_object");
    }
    // Queuez represents a release with the ordinary object key and an empty payload. The
    // encoding selector is not read for empty descriptors; oodle matches the surrounding objects.
    staged.objects[1] = middleware::queuez::Object{
        dismantle.itemInstanceDefinitionId,
        dismantle.dismantledInstanceSoid,
        middleware::queuez::Encoding::oodle,
        {},
    };

    std::size_t objectCount = 2;
    if (dismantle.updatesAccount) {
        if (family4_datagen::account::layout::kObjectSize > rawStorage.size()) {
            clear_after(scratch, reservation);
            return report_failure("dismantle_account_storage");
        }
        const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
        if (!family4_datagen::account::encode(account, accountBytes)) {
            clear_after(scratch, reservation);
            return report_failure("dismantle_account_encode");
        }

        auto& accountObject =
            *reinterpret_cast<family4_datagen::account::layout::Object*>(accountBytes.data());
        const auto recordIsZero =
            [](const family4_datagen::account::layout::ProfileInventoryChangeRecord&
                   record) noexcept {
                return record.sequence == 0 && record.reserved == 0 && record.mutationSerial == 0
                       && record.kind == 0 && record.reservedKind == 0 && record.flags == 0;
            };
        if (accountObject.profileInventoryChanges.writeSlot != 0
            || accountObject.profileInventoryChanges.nextSequence != 0
            || !std::all_of(accountObject.profileInventoryChanges.records.cbegin(),
                            accountObject.profileInventoryChanges.records.cend(),
                            recordIsZero)
            || mutation.rewardCount == 0
            || mutation.rewardCount > accountObject.profileInventoryChanges.records.size()) {
            clear_after(scratch, reservation);
            return report_failure("dismantle_account_change_state");
        }

        // Kind 1 is the ordinary acquisition path, and clear policy bits leave it enabled. The
        // native observer skips a record with any other pair.
        constexpr std::uint8_t kRewardChangeKind = 1;
        constexpr std::uint16_t kRewardChangeFlags = 0;
        for (std::size_t rewardIndex = 0; rewardIndex < mutation.rewardCount; ++rewardIndex) {
            const state::DismantleReward& reward = mutation.rewards[rewardIndex];
            std::size_t matchedRows = 0;
            for (const auto& row : accountObject.profileItems) {
                if (row.mutationSerial != reward.mutationSerial) {
                    continue;
                }
                if (row.quantity != reward.afterQuantity) {
                    clear_after(scratch, reservation);
                    return report_failure("dismantle_reward_quantity");
                }
                ++matchedRows;
            }
            if (matchedRows != 1) {
                clear_after(scratch, reservation);
                return report_failure("dismantle_reward_row");
            }
            auto& change = accountObject.profileInventoryChanges.records[rewardIndex];
            change.sequence = static_cast<std::uint16_t>(rewardIndex);
            change.mutationSerial = reward.mutationSerial;
            change.kind = kRewardChangeKind;
            change.flags = kRewardChangeFlags;
        }
        accountObject.profileInventoryChanges.writeSlot =
            static_cast<std::uint16_t>(mutation.rewardCount);
        accountObject.profileInventoryChanges.nextSequence =
            static_cast<std::uint16_t>(mutation.rewardCount);

        if (!append_object(scratch,
                           accountBytes,
                           dismantle.accountDefinitionId,
                           dismantle.accountSoid,
                           staged.objects[2],
                           compressedExtent)) {
            clear_after(scratch, reservation);
            return report_failure("dismantle_account_object");
        }
        staged.rawClearSize =
            (std::max)(staged.rawClearSize,
                       reservation.rawWriteOffset + family4_datagen::account::layout::kObjectSize);
        objectCount = 3;
    }
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        dismantle.after.family4RootSoid,
        dismantle.after.family4Version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("dismantle_commit");
    }

    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=dismantle stage=family4_objects result=ok family_version=%d root=0x%llX "
        "character=0x%llX character_definition=%u instance=0x%llX item_definition=%u "
        "definition_hash=0x%08X inventory_index=%zu inventory_row=%u equipment_slot=%u "
        "moved_items=%zu items_after=%zu next_serial=%u rewards=%zu objects=%zu "
        "order=%s",
        dismantle.after.family4Version,
        static_cast<unsigned long long>(dismantle.after.family4RootSoid),
        static_cast<unsigned long long>(dismantle.characterSoid),
        dismantle.characterDefinitionId,
        static_cast<unsigned long long>(dismantle.dismantledInstanceSoid),
        dismantle.itemInstanceDefinitionId,
        mutation.dismantledItem.definitionHash,
        mutation.inventoryIndex,
        static_cast<unsigned>(mutation.inventoryRow),
        static_cast<unsigned>(mutation.equipmentSlot),
        mutation.movedInventoryItemCount,
        mutation.afterCharacter.inventory.count,
        mutation.afterCharacter.nextInventorySerial,
        mutation.rewardCount,
        objectCount,
        dismantle.updatesAccount ? "character_release_account" : "character_release");
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return true;
}
} // namespace sunrise::server::bap::encrypted::push::snapshot
