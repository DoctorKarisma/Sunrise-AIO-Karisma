#include "runtime_state_file.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../account/account_state.h"
#include "../runtime.h"
#include "../storage/internal.h"

namespace sunrise::state::persistence {
namespace {

namespace authored = account::inventory;

/** The overlay sits beside settings.json in the owned folder, as build_data.bin already does. */
constexpr std::wstring_view kFileSuffix = L"\\runtime-state.bin";
/** Sibling the write lands on before it is renamed over the target. */
constexpr std::wstring_view kTemporarySuffix = L"\\runtime-state.bin.tmp";
/** Identifies the file before a single byte of it is trusted. */
constexpr std::uint32_t kMagic = 0x53525354U;
/**
 * On-disk layout version. Raise it whenever a record below changes shape.
 *
 * A version this reader does not know drops the overlay and boots the authored account, which is
 * the safe direction. There are no older versions to migrate from yet, so an unknown version is
 * simply refused.
 */
constexpr std::uint32_t kVersion = 1;

#pragma pack(push, 1)

/**
 * One item, written field by field rather than as its in-memory shape.
 *
 * The in-memory item is not trivially copyable and its layout is free to change, so every field is
 * named here. That is what lets the struct above be edited without silently invalidating files.
 */
struct ItemRecord {
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t level{};
    std::int32_t quantity{};
    std::int32_t mutationSerial{};
    std::uint32_t flags{};
    /** 0 leaves the slot empty; the remaining fields are then ignored. */
    std::uint8_t present{};
    std::uint8_t socketPolicy{};
    std::uint8_t movementAbilityEntry{};
    std::uint8_t grenadeAbilityEntry{};
    std::uint8_t superAbilityEntry{};
    std::uint8_t meleeAbilityEntry{};
    std::uint8_t classAbilityEntry{};
    /**
     * Authoritative plug count, separate from plugPresent.
     *
     * `valid(Sockets)` rejects any plug at or past this count, so restoring the plugs without it
     * fails every item that carries one. plugPresent cannot stand in for it: a count may cover a
     * socket whose plug is empty.
     */
    std::uint8_t plugCount{};
    std::array<std::uint32_t, authored::kPlugCapacity> plugs{};
    std::array<std::uint8_t, authored::kPlugCapacity> plugPresent{};
};

/** One account-wide stack. */
struct ProfileItemRecord {
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    std::int32_t mutationSerial{};
    /** Slack, so one more field can be added without moving every record after it. */
    std::int32_t reserved{};
};

/**
 * The mutable half of the account itself.
 *
 * Balances live here rather than on a character because profile items are account-wide. Without
 * this record a dismantle's reward and a reacquisition's charge both evaporate at the next boot,
 * which reads like the economy having never run.
 */
struct AccountRecord {
    std::uint64_t accountSoid{};
    std::uint32_t profileItemCount{};
    std::uint32_t reserved{};
    std::array<ProfileItemRecord, authored::kProfileItemCapacity> profileItems{};
};

/** The mutable half of one character. Identity and appearance stay authored. */
struct CharacterRecord {
    std::uint64_t characterSoid{};
    /** Runtime-only mask of ability entries the player has selected at least once. */
    std::uint64_t acquiredSubclassAbilityMask{};
    std::uint32_t nextInventorySerial{};
    std::uint32_t inventoryCount{};
    std::uint32_t lastOrbitedDestination{};
    /** Slack, so one more field can be added without moving every record after it. */
    std::uint32_t reserved{};
    std::array<ItemRecord, authored::kEquipmentSlotCount> equipment{};
    std::array<ItemRecord, authored::kCharacterItemCapacity> inventory{};
};

/** Fixed prefix, checked in full before any record is read. */
struct Header {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t characterCount{};
    /** Slack, so one more header field can be added without moving the records after it. */
    std::uint32_t reserved{};
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<ItemRecord>);
static_assert(std::is_trivially_copyable_v<ProfileItemRecord>);
static_assert(std::is_trivially_copyable_v<AccountRecord>);
static_assert(std::is_trivially_copyable_v<CharacterRecord>);
static_assert(std::is_trivially_copyable_v<Header>);

/** The account record sits between the header and the character array. */
constexpr std::size_t kAccountOffset = sizeof(Header);
/** Character records start after the single account record. */
constexpr std::size_t kCharacterOffset = kAccountOffset + sizeof(AccountRecord);
/**
 * Largest overlay worth reading into memory.
 *
 * Derived from the format rather than picked: a full account at capacity is exactly this size, so
 * anything larger is not an overlay whatever else it is. The load below still requires an exact
 * size for the character count it declares; this only bounds the read itself.
 */
constexpr std::size_t kMaximumSize =
    kCharacterOffset + (kCharacterCapacity * sizeof(CharacterRecord));

/** Resolved once at load and reused by every save. Empty until a load has run. */
core::path::Buffer g_path{};
core::path::Buffer g_temporaryPath{};
bool g_pathResolved{};

/** @param stage Which half ran. @param reason Short key naming the outcome. */
void report(const char* stage, const char* reason) noexcept {
    std::array<char, 128> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=persist stage=%s result=%s", stage, reason);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Copies one item into its on-disk record. */
[[nodiscard]] ItemRecord to_record(const authored::Item& item) noexcept {
    ItemRecord record{};
    record.present = 1;
    record.instanceSoid = item.instanceSoid;
    record.definitionHash = item.definitionHash;
    record.level = item.level;
    record.quantity = item.quantity;
    record.mutationSerial = item.mutationSerial;
    record.flags = item.flags;
    record.socketPolicy = static_cast<std::uint8_t>(item.sockets.policy);
    record.movementAbilityEntry = item.movementAbilityEntry;
    record.grenadeAbilityEntry = item.grenadeAbilityEntry;
    record.superAbilityEntry = item.superAbilityEntry;
    record.meleeAbilityEntry = item.meleeAbilityEntry;
    record.classAbilityEntry = item.classAbilityEntry;
    record.plugCount = static_cast<std::uint8_t>(item.sockets.plugCount);
    for (std::size_t plug = 0; plug < item.sockets.plugs.size(); ++plug) {
        if (item.sockets.plugs[plug].has_value()) {
            record.plugs[plug] = *item.sockets.plugs[plug];
            record.plugPresent[plug] = 1;
        }
    }
    return record;
}

/** Copies one optional equipment slot into its on-disk record. */
[[nodiscard]] ItemRecord to_record(const std::optional<authored::Item>& item) noexcept {
    return item.has_value() ? to_record(*item) : ItemRecord{};
}

/** Rebuilds one item from its on-disk record. */
[[nodiscard]] authored::Item from_record(const ItemRecord& record) noexcept {
    authored::Item item{};
    item.instanceSoid = record.instanceSoid;
    item.definitionHash = record.definitionHash;
    item.level = record.level;
    item.quantity = record.quantity;
    item.mutationSerial = record.mutationSerial;
    item.flags = record.flags;
    item.sockets.policy = static_cast<authored::SocketPolicy>(record.socketPolicy);
    item.movementAbilityEntry = record.movementAbilityEntry;
    item.grenadeAbilityEntry = record.grenadeAbilityEntry;
    item.superAbilityEntry = record.superAbilityEntry;
    item.meleeAbilityEntry = record.meleeAbilityEntry;
    item.classAbilityEntry = record.classAbilityEntry;
    item.sockets.plugCount = record.plugCount <= item.sockets.plugs.size()
                                 ? record.plugCount
                                 : item.sockets.plugs.size();
    for (std::size_t plug = 0; plug < item.sockets.plugs.size(); ++plug) {
        if (record.plugPresent[plug] != 0) {
            item.sockets.plugs[plug] = record.plugs[plug];
        }
    }
    return item;
}

/** Rebuilds one equipment slot, which is empty when the record was written empty. */
[[nodiscard]] std::optional<authored::Item> to_slot(const ItemRecord& record) noexcept {
    if (record.present == 0) {
        return std::nullopt;
    }
    return from_record(record);
}

/**
 * Resolves the overlay path and its write sibling once.
 * @param module Loaded DLL that owns the artifact directory.
 * @return True when both paths fit their buffers.
 */
[[nodiscard]] bool resolve_paths(void* module) noexcept {
    if (g_pathResolved) {
        return true;
    }
    core::path::Buffer directory{};
    if (!core::path::artifact_directory(module, directory)) {
        return false;
    }
    g_path = directory;
    g_temporaryPath = directory;
    if (!core::path::append(g_path, kFileSuffix)
        || !core::path::append(g_temporaryPath, kTemporarySuffix)) {
        return false;
    }
    g_pathResolved = true;
    return true;
}

/**
 * Reads the whole overlay into memory.
 * @param contents Gets the file bytes, untouched when the file is absent or refused.
 * @return True only when the complete file was read.
 */
[[nodiscard]] bool read_file(std::vector<std::byte>& contents) noexcept {
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    const bool sized = GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0
                       && static_cast<std::uint64_t>(size.QuadPart) <= kMaximumSize;
    bool complete = false;
    if (sized) {
        contents.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        complete =
            ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr)
                != FALSE
            && read == contents.size();
    }
    (void)CloseHandle(file);
    if (!complete) {
        contents.clear();
    }
    return complete;
}

} // namespace

/** Writes the mutable half of the account, replacing any previous overlay. */
void save() noexcept {
    if (!g_pathResolved) {
        // Nothing has resolved a path, so there is nowhere to write. A load always runs first.
        return;
    }
    const AccountState account = account_snapshot();
    const Header header{kMagic, kVersion, static_cast<std::uint32_t>(account.characterCount), 0};
    std::vector<std::byte> contents(kCharacterOffset
                                    + account.characterCount * sizeof(CharacterRecord));
    std::memcpy(contents.data(), &header, sizeof header);

    AccountRecord accountRecord{};
    accountRecord.accountSoid = account.primarySoid;
    accountRecord.profileItemCount = static_cast<std::uint32_t>(account.profileItemCount);
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        const authored::ProfileItem& row = account.profileItems[index];
        accountRecord.profileItems[index] = ProfileItemRecord{
            row.instanceSoid, row.definitionHash, row.quantity, row.mutationSerial, 0};
    }
    std::memcpy(contents.data() + kAccountOffset, &accountRecord, sizeof accountRecord);

    for (std::size_t index = 0; index < account.characterCount; ++index) {
        const CharacterState& character = account.characters[index];
        CharacterRecord record{};
        record.characterSoid = character.soid;
        record.acquiredSubclassAbilityMask = character.acquiredSubclassAbilityMask;
        record.nextInventorySerial = character.nextInventorySerial;
        record.inventoryCount = static_cast<std::uint32_t>(character.inventory.count);
        record.lastOrbitedDestination = character.lastOrbitedDestination;
        for (std::size_t slot = 0; slot < character.equipment.slots.size(); ++slot) {
            record.equipment[slot] = to_record(character.equipment.slots[slot]);
        }
        for (std::size_t slot = 0; slot < character.inventory.count; ++slot) {
            record.inventory[slot] = to_record(character.inventory.values[slot]);
        }
        std::memcpy(
            contents.data() + kCharacterOffset + index * sizeof record, &record, sizeof record);
    }

    // Written to a sibling and renamed over the target. A crash mid-write then costs the newest
    // save, never the file the next boot reads.
    const HANDLE file = CreateFileW(g_temporaryPath.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        report("save", "fail_open");
        return;
    }
    DWORD written = 0;
    bool complete =
        WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr)
            != FALSE
        && written == contents.size();
    complete = CloseHandle(file) != FALSE && complete;
    if (!complete) {
        (void)DeleteFileW(g_temporaryPath.chars.data());
        report("save", "fail_write");
        return;
    }
    if (MoveFileExW(g_temporaryPath.chars.data(), g_path.chars.data(), MOVEFILE_REPLACE_EXISTING)
        == FALSE) {
        (void)DeleteFileW(g_temporaryPath.chars.data());
        report("save", "fail_replace");
        return;
    }
    report("save", "ok");
}

/** Applies the saved runtime overlay on top of the authored account, when one exists. */
void load(void* module) noexcept {
    if (!resolve_paths(module)) {
        report("load", "fail_path");
        return;
    }
    std::vector<std::byte> contents;
    if (!read_file(contents)) {
        // A first run has no overlay, which is the ordinary case and not a failure.
        report("load", "absent");
        return;
    }
    Header header{};
    if (contents.size() < sizeof header) {
        report("load", "fail_short");
        return;
    }
    std::memcpy(&header, contents.data(), sizeof header);
    const std::size_t expected =
        kCharacterOffset
        + static_cast<std::size_t>(header.characterCount) * sizeof(CharacterRecord);
    if (header.magic != kMagic || header.version != kVersion
        || header.characterCount > kCharacterCapacity || contents.size() != expected) {
        report("load", "fail_layout");
        return;
    }

    AccountState candidate = account_snapshot();
    AccountRecord accountRecord{};
    std::memcpy(&accountRecord, contents.data() + kAccountOffset, sizeof accountRecord);
    // Matched by key like the characters are. A saved balance belongs to the account it was earned
    // on, and applying it to a different one would invent currency out of a stale file.
    if (accountRecord.accountSoid != 0 && accountRecord.accountSoid == candidate.primarySoid
        && accountRecord.profileItemCount <= candidate.profileItems.size()) {
        for (std::size_t index = 0; index < candidate.profileItems.size(); ++index) {
            const ProfileItemRecord& row = accountRecord.profileItems[index];
            candidate.profileItems[index] = index < accountRecord.profileItemCount
                                                ? authored::ProfileItem{row.instanceSoid,
                                                                        row.definitionHash,
                                                                        row.quantity,
                                                                        row.mutationSerial}
                                                : authored::ProfileItem{};
        }
        candidate.profileItemCount = accountRecord.profileItemCount;
    }

    std::size_t applied = 0;
    for (std::uint32_t index = 0; index < header.characterCount; ++index) {
        CharacterRecord record{};
        std::memcpy(&record,
                    contents.data() + kCharacterOffset
                        + static_cast<std::size_t>(index) * sizeof record,
                    sizeof record);
        // Matched by key, never by position, so reordering the authored characters cannot hand one
        // character another's loadout.
        for (std::size_t slot = 0; slot < candidate.characterCount; ++slot) {
            CharacterState& character = candidate.characters[slot];
            if (record.characterSoid == 0 || character.soid != record.characterSoid) {
                continue;
            }
            if (record.inventoryCount > character.inventory.values.size()) {
                break;
            }
            for (std::size_t item = 0; item < character.equipment.slots.size(); ++item) {
                character.equipment.slots[item] = to_slot(record.equipment[item]);
            }
            for (std::size_t item = 0; item < character.inventory.values.size(); ++item) {
                character.inventory.values[item] = item < record.inventoryCount
                                                       ? from_record(record.inventory[item])
                                                       : authored::Item{};
            }
            character.inventory.count = record.inventoryCount;
            character.nextInventorySerial = record.nextInventorySerial;
            character.acquiredSubclassAbilityMask = record.acquiredSubclassAbilityMask;
            character.lastOrbitedDestination = record.lastOrbitedDestination;
            ++applied;
            break;
        }
    }

    // The authored account is already known good, so anything the overlay cannot produce validly is
    // dropped whole rather than partially applied.
    if (!account::valid(candidate)) {
        report("load", "fail_invalid");
        return;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);

    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=persist stage=load result=ok version=%u characters=%zu",
                                      static_cast<unsigned>(header.version),
                                      applied);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace sunrise::state::persistence
