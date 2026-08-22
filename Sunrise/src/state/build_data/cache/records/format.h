#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../content/content_catalog.h"
#include "../../abilities/definition.h"
#include "../../collectibles/collectible_catalog.h"
#include "../../constants/definition.h"
#include "../../definition.h"
#include "../../entity_names/definition.h"
#include "../../hash_names/definition.h"
#include "../../items/catalysts/definition.h"
#include "../../items/details/definition.h"
#include "../../items/item_catalog.h"
#include "../../items/socket_plugs/definition.h"
#include "../../material_requirements/material_requirement_catalog.h"
#include "../../progressions/definition.h"
#include "../../records/definition.h"
#include "../../scenarios/definition.h"
#include "../../spawn_sets/definition.h"
#include "../../vendors/definition.h"

namespace sunrise::state::build_data::cache::records {

inline constexpr std::array<char, 8> kCacheMagic{'S', 'U', 'N', 'R', 'I', 'S', 'E', 'B'};

inline constexpr std::uint32_t kCacheFormatVersion = 51;

/** Signed -1 on disk means there is no equipment slot. */
inline constexpr std::int8_t kAbsentEquipmentSlot = -1;

inline constexpr std::uint64_t kChecksumOffsetBasis = 14695981039346656037ULL;

inline constexpr std::uint64_t kChecksumPrime = 1099511628211ULL;

#pragma pack(push, 1)

struct Prefix {
    std::array<char, kCacheMagic.size()> magic{};
    std::uint32_t version{};
};

struct InvestmentConstants {
    std::uint8_t lightStatRow{};

    std::array<std::uint8_t, constants::kCharacterStatRowCount> characterStatRows{};

    std::uint8_t extracted{};
};

struct Header {
    std::array<char, kCacheMagic.size()> magic{};
    std::uint32_t version{};
    std::uint32_t imageTimestamp{};
    std::uint32_t imageSize{};
    std::uint64_t configuredEquipmentHash{};

    std::uint32_t namedCount{};
    std::uint32_t itemCount{};
    std::uint32_t collectibleCount{};
    std::uint32_t materialRequirementSetCount{};
    std::uint32_t itemDetailCount{};
    std::uint32_t socketPlugRuleCount{};
    std::uint32_t socketPlugPoolCount{};
    std::uint32_t socketPlugMemberCount{};
    std::uint32_t exoticCatalystCount{};
    std::uint32_t inventoryBucketCount{};
    std::uint32_t socketEntryListCount{};
    std::uint32_t socketEntryTableCount{};
    std::uint32_t abilityBucketCount{};
    std::uint32_t progressionCount{};
    std::uint32_t recordCount{};
    std::uint32_t scenarioCount{};
    std::uint32_t rosterGroupCount{};
    std::uint32_t spawnStemCount{};
    std::uint32_t spawnNameHashCount{};
    std::uint32_t spawnPointCount{};
    std::uint32_t hashNameCount{};
    std::uint32_t entityNameCount{};
    std::uint32_t vendorIndexCount{};
    std::uint32_t vendorDefinitionCount{};
    std::uint32_t vendorSaleRowCount{};
    std::uint32_t vendorInstalledRowCount{};

    InvestmentConstants constants{};

    std::uint64_t payloadChecksum{};
};

struct NamedRecord {
    std::array<char, content::kDefinitionNameCapacity> name{};
    std::uint16_t nameLength{};
    std::uint16_t reserved{};
    std::uint32_t tag{};
    std::uint32_t classId{};
};

struct ItemRecord {
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{};
    std::uint8_t bucketId{items::kUnresolvedBucketId};
    std::uint8_t tier{};

    std::uint16_t insertionMaterialRequirementSetIndex{
        items::kUnavailableMaterialRequirementSetIndex};

    std::uint16_t enabledMaterialRequirementSetIndex{
        items::kUnavailableMaterialRequirementSetIndex};

    std::uint32_t plugCategoryHash{};
    std::uint16_t rollSetIndex{};
    std::uint16_t linkedPlugIndex{items::kUnavailableLinkedPlugIndex};
};

struct MaterialRequirementRecord {
    std::uint32_t quantity{};

    std::uint16_t itemDefinitionIndex{collectibles::kUnavailableItemDefinitionIndex};

    std::uint16_t condition{material_requirements::kUnconditionalRequirement};

    std::uint8_t deleteOnAction{};
    std::uint8_t omitFromRequirements{};
};

struct CollectibleRecord {
    std::uint32_t collectibleHash{};
    std::uint32_t materialRequirementSetHash{};

    std::uint16_t collectibleIndex{};

    std::uint16_t itemDefinitionIndex{collectibles::kUnavailableItemDefinitionIndex};

    std::uint16_t materialRequirementSetIndex{
        collectibles::kUnavailableMaterialRequirementSetIndex};

    std::uint8_t materialRequirementCount{};
    std::uint8_t reserved{};

    std::array<MaterialRequirementRecord, collectibles::kMaterialRequirementCapacity>
        materialRequirements{};
};

struct MaterialRequirementSetRecord {
    std::uint32_t requirementSetHash{};

    std::uint16_t requirementSetIndex{material_requirements::kUnavailableSetIndex};

    std::uint8_t requirementCount{};
    std::uint8_t reserved{};

    std::array<MaterialRequirementRecord, material_requirements::kRequirementCapacity>
        requirements{};
};

struct ItemDetailRecord {
    std::uint16_t definitionIndex{};
    std::uint8_t bucketId{};
    std::int8_t equipmentSlot{kAbsentEquipmentSlot};
    std::uint8_t instancedDefinition{};
    std::uint8_t ordinarySocketState{};
    std::uint8_t ordinarySocketCount{};
    std::int32_t maxStackSize{};

    std::uint16_t socketEntryListIndex{};

    std::array<std::uint16_t, items::details::kInitialPlugCapacity> initialPlugIndices{};

    std::array<std::uint16_t, items::details::kInitialPlugCapacity> socketTypes{};

    std::uint8_t statCount{};

    std::array<std::uint8_t, items::details::kStatCapacity> statRows{};

    std::array<std::int32_t, items::details::kStatCapacity> statValues{};

    std::uint32_t definitionHash{};
    std::uint16_t gearArtIndex{};

    std::array<std::uint16_t, items::details::kArtClassCapacity> artArrangementIndices{};

    std::uint8_t sandboxPerkCount{};

    std::array<std::uint16_t, items::details::kSandboxPerkCapacity> sandboxPerks{};

    std::uint8_t renderOverrideCount{};

    std::array<std::uint8_t, items::details::kRenderOverrideCapacity> overrideStages{};

    std::array<std::int8_t, items::details::kRenderOverrideCapacity> overrideKeys{};

    std::array<std::uint16_t, items::details::kRenderOverrideCapacity> overrideValues{};
};

struct SocketPlugRuleRecord {
    std::uint16_t itemDefinitionIndex{};
    std::uint8_t lane{};
    std::uint8_t reserved{};
    std::uint32_t poolIndex{};
};

struct SocketPlugPoolRecord {
    std::uint32_t memberOffset{};
    std::uint32_t memberCount{};
};

struct SocketPlugMemberRecord {
    std::uint16_t itemDefinitionIndex{};
};

struct ExoticCatalystRecord {
    std::uint32_t itemDefinitionHash{};

    std::uint16_t itemDefinitionIndex{};
    std::uint16_t completedPlugDefinitionIndex{};
    std::uint16_t progressPlugDefinitionIndex{};
    std::uint16_t effectDefinitionIndex{};
    std::uint16_t acquisitionDefinitionIndex{};

    std::array<std::uint16_t, items::catalysts::kCompletionFlagCapacity>
        completionFlagDefinitionIndices{};

    std::array<std::uint16_t, items::catalysts::kCompletionValueCapacity> completionValueIndices{};

    std::uint16_t objectiveDefinitionIndex{items::catalysts::kUnavailableObjectiveIndex};

    std::uint8_t socketLane{};
    std::uint8_t availability{};
    std::uint8_t completionFlagCount{};
    std::uint8_t completionValueCount{};

    std::array<std::int32_t, items::catalysts::kCompletionValueCapacity> completionValues{};

    std::int32_t objectiveValue{};
};

struct InventoryBucketRecord {
    std::uint8_t bucketId{};
    std::uint8_t arraySelector{};
    std::uint16_t firstSlot{};
    std::uint16_t slotCount{};
    std::int8_t equipmentSlot{inventory::buckets::kUnavailableEquipmentSlot};
    std::uint8_t reserved{};
};

struct AbilityBucketRecord {
    std::uint16_t socketEntryListIndex{};

    std::uint8_t movementEntry{};
    std::uint8_t grenadeEntry{};
    std::uint8_t superEntry{};
    std::uint8_t meleeEntry{};
    std::uint8_t classEntry{};
    std::uint8_t overflowCount{};

    std::array<std::uint8_t, abilities::kBucketCapacity> bucketKinds{};

    std::array<std::uint8_t, abilities::kBucketCapacity> bucketHashCounts{};

    std::array<std::uint32_t, abilities::kBucketCapacity * abilities::kBucketHashCapacity>
        bucketHashes{};

    std::array<std::uint32_t, abilities::kOverflowCapacity> overflow{};
};

struct ProgressionRecord {
    std::uint16_t definitionIndex{};
    std::uint8_t scope{};
    std::uint8_t reserved{};
};

struct RecordDefinitionRecord {
    std::uint16_t definitionIndex{};
    std::uint16_t completionFlagIndex{};
    std::uint16_t scoreValue{};
    std::uint16_t reserved{};
};

struct SocketEntryListRecord {
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{};
    std::uint8_t entryCount{};
    std::uint8_t reserved{};
    std::uint64_t readyMask{};
};

struct SocketEntryTableRecord {
    std::uint16_t definitionIndex{};
    std::array<std::uint8_t, 2> reserved{};

    std::array<std::uint32_t, socket_entry_lists::kEntryCapacity> plugSources{};

    std::array<std::uint8_t, socket_entry_lists::kEntryCapacity> groups{};

    std::array<std::uint8_t, socket_entry_lists::kEntryCapacity> kinds{};
};

struct ScenarioRecord {
    std::array<char, scenarios::kNameCapacity> name{};
    std::uint32_t tag{};

    std::uint8_t nameLength{};
    std::uint8_t bubbleCount{};
    std::uint8_t truncated{};
    std::uint8_t rosterGroupCount{};
    std::uint8_t spawnStemLength{};
    std::uint8_t bubbleGroupCount{};

    std::array<std::uint8_t, 2> reserved{};

    std::array<char, scenarios::kSpawnStemCapacity> spawnStem{};

    std::array<std::uint8_t, scenarios::kBubbleCapacity> bubbleStates{};

    std::array<std::uint32_t, scenarios::kBubbleCapacity> bubbleHashes{};

    std::array<std::uint8_t, scenarios::kBubbleCapacity> bubbleStateCounts{};

    std::array<std::uint16_t, scenarios::kDestinationGroupCapacity> rosterGroups{};

    std::array<std::uint16_t, scenarios::kDestinationBubbleGroupCapacity> bubbleGroups{};

    std::array<std::array<std::uint8_t, scenarios::kBubbleMaskBytes>,
               scenarios::kDestinationBubbleGroupCapacity>
        bubbleGroupMasks{};

    std::array<std::uint16_t, scenarios::kBubbleCapacity> bubbleMapIndices{};

    std::uint8_t packageCount{};
    std::uint8_t packageReserved{};

    std::array<std::uint16_t, scenarios::kDestinationPackageCapacity> packages{};
};

struct SpawnStemRecord {
    std::array<char, spawn_sets::kStemNameCapacity> name{};
    std::uint32_t pointCount{};
    std::uint16_t setCount{};
    std::uint16_t nameHashOffset{};
    std::uint16_t nameHashCount{};
    std::uint8_t nameLength{};
    std::uint8_t reserved{};
};

struct HashNameRecord {
    std::array<char, hash_names::kNameLength> name{};
    std::uint32_t hash{};
    std::uint8_t nameLength{};
    std::array<std::uint8_t, 3> reserved{};
};

struct EntityNameRecord {
    std::array<char, entity_names::kNameLength> text{};
    std::uint32_t tag{};
    std::uint8_t length{};
    std::array<std::uint8_t, 3> reserved{};
};

struct SpawnNameHashRecord {
    std::uint32_t value{};
    std::uint32_t pointCount{};
    std::uint16_t stemIndex{};
    std::array<std::uint8_t, 2> reserved{};

    std::array<std::uint8_t, spawn_sets::kBubbleMaskBytes> bubbleMask{};

    std::uint8_t unbound{};
    std::uint8_t inMapPackage{};
    std::uint8_t activityPackageCount{};
    std::uint8_t activityPackageOverflow{};

    std::array<std::uint16_t, spawn_sets::kPackageCapacity> activityPackages{};
};

struct SpawnPointRecord {
    std::array<float, spawn_sets::kPositionComponents> position{};

    std::uint32_t nameHash{};
    std::uint16_t stemIndex{};
    std::array<std::uint8_t, 2> reserved{};
};

struct VendorIndexRecord {
    std::uint32_t definitionHash{};
    std::uint32_t definitionTag{};
    std::uint16_t index{};
    std::uint16_t reserved{};
};

struct VendorDefinitionRecord {
    std::uint32_t definitionHash{};
    std::uint32_t definitionTag{};
    std::uint32_t definitionClass{};
    std::uint32_t definitionSize{};
    std::uint32_t installedRowBase{};
    std::uint32_t installedRowClass{};
    std::uint32_t saleRowBase{};
    std::uint32_t saleRowClass{};
    std::uint32_t thirdRowBase{};
    std::uint32_t thirdRowClass{};
    std::uint32_t saleRowOffset{};
    std::uint32_t installedRowOffset{};
    std::uint32_t resetIntervalRaw{};
    std::uint32_t resetPhaseRaw{};

    std::uint16_t index{};
    std::uint16_t installedCount{};
    std::uint16_t saleCount{};
    std::uint16_t thirdCount{};
};

struct VendorSaleRowRecord {
    std::uint16_t vendorIndex{};
    std::uint16_t rowIndex{};
    std::uint16_t itemIndex{};
    std::uint16_t secondaryItemIndex{};

    std::int32_t installedIndex{};

    std::uint32_t raw104{};
    std::uint32_t raw108{};
    std::int32_t raw172{};

    std::uint32_t expressionCount8{};
    std::uint32_t nestedRecordCount{};
    std::uint32_t expressionCount120{};
    std::uint32_t count136{};
    std::uint32_t expressionCount160{};

    std::uint8_t featureBranch{};
    std::array<std::uint8_t, 3> reserved{};
};

struct VendorInstalledRowRecord {
    std::uint16_t vendorIndex{};
    std::uint16_t rowIndex{};

    std::array<std::uint8_t, vendors::kInstalledRowStride> raw{};
};

struct RosterGroupRecord {
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotCount{};

    std::array<std::uint8_t, scenarios::kRosterSlotCapacity> slotTypes{};

    std::array<std::uint8_t, scenarios::kRosterSlotCapacity> slotFlags{};

    std::array<std::uint16_t, scenarios::kRosterSlotCapacity> slotIndices{};
};

#pragma pack(pop)

static_assert(sizeof(Prefix) == kCacheMagic.size() + sizeof(std::uint32_t));

static_assert(sizeof(InvestmentConstants)
              == constants::kCharacterStatRowCount + 2 * sizeof(std::uint8_t));

/*
 * Header contains:
 *
 *   8 bytes  magic
 *   29 x uint32_t
 *   2 x uint64_t
 *   InvestmentConstants
 *
 * The previous assertion used 28 uint32_t values even though this
 * Header contains 26 count fields in addition to version/imageTimestamp/
 * imageSize. That made the assertion fail by exactly 4 bytes.
 */
static_assert(sizeof(Header)
              == kCacheMagic.size() + 29 * sizeof(std::uint32_t) + 2 * sizeof(std::uint64_t)
                     + sizeof(InvestmentConstants));

static_assert(sizeof(SpawnPointRecord)
              == spawn_sets::kPositionComponents * sizeof(float) + sizeof(std::uint32_t)
                     + sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t));

static_assert(sizeof(VendorIndexRecord) == 2 * sizeof(std::uint32_t) + 2 * sizeof(std::uint16_t));

static_assert(sizeof(VendorDefinitionRecord)
              == 14 * sizeof(std::uint32_t) + 4 * sizeof(std::uint16_t));

static_assert(sizeof(VendorSaleRowRecord)
              == 4 * sizeof(std::uint16_t) + 9 * sizeof(std::uint32_t) + 4 * sizeof(std::uint8_t));

static_assert(sizeof(VendorInstalledRowRecord)
              == 2 * sizeof(std::uint16_t) + vendors::kInstalledRowStride);

static_assert(sizeof(HashNameRecord)
              == hash_names::kNameLength + sizeof(std::uint32_t) + 4 * sizeof(std::uint8_t));

static_assert(sizeof(EntityNameRecord)
              == entity_names::kNameLength + sizeof(std::uint32_t) + 4 * sizeof(std::uint8_t));

static_assert(sizeof(ScenarioRecord)
              == scenarios::kNameCapacity + sizeof(std::uint32_t) + 10 * sizeof(std::uint8_t)
                     + scenarios::kSpawnStemCapacity
                     + 2 * scenarios::kBubbleCapacity * sizeof(std::uint8_t)
                     + scenarios::kBubbleCapacity * sizeof(std::uint32_t)
                     + scenarios::kBubbleCapacity * sizeof(std::uint16_t)
                     + (scenarios::kDestinationGroupCapacity
                        + scenarios::kDestinationPackageCapacity
                        + scenarios::kDestinationBubbleGroupCapacity)
                           * sizeof(std::uint16_t)
                     + scenarios::kDestinationBubbleGroupCapacity * scenarios::kBubbleMaskBytes);

static_assert(sizeof(SpawnStemRecord)
              == spawn_sets::kStemNameCapacity + sizeof(std::uint32_t) + 3 * sizeof(std::uint16_t)
                     + 2 * sizeof(std::uint8_t));

static_assert(sizeof(SpawnNameHashRecord)
              == 2 * sizeof(std::uint32_t)
                     + (1 + spawn_sets::kPackageCapacity) * sizeof(std::uint16_t)
                     + 6 * sizeof(std::uint8_t) + spawn_sets::kBubbleMaskBytes);

static_assert(sizeof(RosterGroupRecord)
              == 2 * sizeof(std::uint32_t) + sizeof(std::uint16_t)
                     + 2 * scenarios::kRosterSlotCapacity * sizeof(std::uint8_t)
                     + scenarios::kRosterSlotCapacity * sizeof(std::uint16_t));

static_assert(sizeof(ProgressionRecord) == sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t));

static_assert(sizeof(RecordDefinitionRecord) == 4 * sizeof(std::uint16_t));

static_assert(sizeof(AbilityBucketRecord)
              == sizeof(std::uint16_t) + 6 * sizeof(std::uint8_t)
                     + 2 * abilities::kBucketCapacity * sizeof(std::uint8_t)
                     + (abilities::kBucketCapacity * abilities::kBucketHashCapacity
                        + abilities::kOverflowCapacity)
                           * sizeof(std::uint32_t));

static_assert(sizeof(NamedRecord)
              == content::kDefinitionNameCapacity + 2 * sizeof(std::uint16_t)
                     + 2 * sizeof(std::uint32_t));

static_assert(sizeof(ItemRecord)
              == 2 * sizeof(std::uint32_t) + 5 * sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t));

static_assert(sizeof(MaterialRequirementRecord)
              == sizeof(std::uint32_t) + 2 * sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t));

static_assert(sizeof(CollectibleRecord)
              == 2 * sizeof(std::uint32_t) + 3 * sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t)
                     + collectibles::kMaterialRequirementCapacity
                           * sizeof(MaterialRequirementRecord));

static_assert(sizeof(MaterialRequirementSetRecord)
              == sizeof(std::uint32_t) + sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t)
                     + material_requirements::kRequirementCapacity
                           * sizeof(MaterialRequirementRecord));

static_assert(sizeof(ItemDetailRecord)
              == 7 * sizeof(std::uint16_t) + 8 * sizeof(std::uint8_t) + sizeof(std::int32_t)
                     + sizeof(std::uint32_t)
                     + 2 * items::details::kInitialPlugCapacity * sizeof(std::uint16_t)
                     + items::details::kStatCapacity * (sizeof(std::uint8_t) + sizeof(std::int32_t))
                     + items::details::kSandboxPerkCapacity * sizeof(std::uint16_t)
                     + items::details::kRenderOverrideCapacity
                           * (2 * sizeof(std::uint8_t) + sizeof(std::uint16_t)));

static_assert(sizeof(SocketPlugRuleRecord)
              == sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t) + sizeof(std::uint32_t));

static_assert(sizeof(SocketPlugPoolRecord) == 2 * sizeof(std::uint32_t));

static_assert(sizeof(SocketPlugMemberRecord) == sizeof(std::uint16_t));

static_assert(sizeof(ExoticCatalystRecord)
              == 6 * sizeof(std::uint32_t) + 14 * sizeof(std::uint16_t) + 4 * sizeof(std::uint8_t));

static_assert(sizeof(InventoryBucketRecord)
              == 4 * sizeof(std::uint8_t) + 2 * sizeof(std::uint16_t));

static_assert(sizeof(SocketEntryListRecord)
              == sizeof(std::uint32_t) + sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t)
                     + sizeof(std::uint64_t));

static_assert(sizeof(SocketEntryTableRecord)
              == sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t)
                     + socket_entry_lists::kEntryCapacity
                           * (sizeof(std::uint32_t) + 2 * sizeof(std::uint8_t)));

/**
 * Extends the cache payload checksum with one written record.
 *
 * @tparam Value Trivially copied packed cache record.
 * @param checksum Current checksum state.
 * @param value Record bytes in their disk form.
 * @return Checksum after every record byte is mixed in.
 */
template <typename Value>
[[nodiscard]] std::uint64_t checksum_value(std::uint64_t checksum, const Value& value) noexcept {

    const auto bytes = std::as_bytes(std::span(&value, 1));

    for (const std::byte byte : bytes) {
        checksum ^= std::to_integer<std::uint8_t>(byte);
        checksum *= kChecksumPrime;
    }

    return checksum;
}

} // namespace sunrise::state::build_data::cache::records
