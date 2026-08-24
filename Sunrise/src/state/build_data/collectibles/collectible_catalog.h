#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::build_data::collectibles {

/** The Collections protocol carries a present bit followed by a 15-bit native row index. */
inline constexpr std::size_t kDefinitionCapacity = 1U << 15U;
/** Some collectible rows deliberately do not resolve to an inventory item. */
inline constexpr std::uint16_t kUnavailableItemDefinitionIndex = 0xFFFFU;

/**
 * Collectible row meaning "this item has no collectible".
 *
 * Vendor sale rows name an item, never a collectible, and bounties, quests and tokens have none at
 * all. An acquisition carrying this index skips every collectible step - validation, the material
 * charge, and the cost bookkeeping - and both prepare and commit must agree on it, so the
 * consistency guard still holds rather than being bypassed.
 */
inline constexpr std::uint16_t kNoCollectibleIndex = 0xFFFEU;
/** A collectible with no acquisition charge carries this native requirement-set sentinel. */
inline constexpr std::uint16_t kUnavailableMaterialRequirementSetIndex = 0xFFFFU;
/** Installed requirement sets contain at most six material rows. */
inline constexpr std::size_t kMaterialRequirementCapacity = 6;

/** One native material row attached to a Collections acquisition. */
struct MaterialRequirement {
    std::uint32_t quantity{};
    std::uint16_t itemDefinitionIndex{kUnavailableItemDefinitionIndex};
    bool deleteOnAction{};
    bool omitFromRequirements{};
};

/** One installed-build collectible row and the item-definition row it grants. */
struct Definition {
    std::uint32_t collectibleHash{};
    std::uint32_t materialRequirementSetHash{};
    std::uint16_t collectibleIndex{};
    std::uint16_t itemDefinitionIndex{kUnavailableItemDefinitionIndex};
    std::uint16_t materialRequirementSetIndex{kUnavailableMaterialRequirementSetIndex};
    std::uint8_t materialRequirementCount{};
    std::array<MaterialRequirement, kMaterialRequirementCapacity> materialRequirements{};
};

/** Clears every generated collectible mapping. */
void clear() noexcept;

/** @return True when every native collectible index appears exactly once. */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/** Replaces the complete dense collectible table in one publication. */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/** Finds one collectible by the native 15-bit index carried by the request. */
[[nodiscard]] bool find(std::uint16_t collectibleIndex, Definition& definition) noexcept;

/**
 * Answers whether any collectible grants one installed item row.
 * @param itemDefinitionIndex Installed item-definition row.
 * @return True when Collections can grant that item, so an account can come to own it.
 */
[[nodiscard]] bool grants_item(std::uint16_t itemDefinitionIndex) noexcept;

/** Copies every row in native collectible-index order. */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/** @return Number of installed-build collectible mappings. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::collectibles
