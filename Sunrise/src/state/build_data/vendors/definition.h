#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::vendors {

/** Rows of the installed vendor index. The live table has 511. */
inline constexpr std::size_t kIndexCapacity = 512;
/** Vendor definitions this catalog holds rows for. A definition is read only when asked for. */
inline constexpr std::size_t kDefinitionCapacity = 48;
/** Sale rows across every held definition. One installed definition declares 277. */
inline constexpr std::size_t kSaleRowCapacity = 8192;
/** Installed rows across every held definition. One installed definition declares 43. */
inline constexpr std::size_t kInstalledRowCapacity = 2048;

/** One vendor index row is 24 bytes: hash at +0, definition tag at +16. */
inline constexpr std::size_t kIndexRowStride = 24;
/** One sale row is 184 bytes. */
inline constexpr std::size_t kSaleRowStride = 184;
/** One installed row is 24 bytes. */
inline constexpr std::size_t kInstalledRowStride = 24;
/** One row of the unnamed third array is 80 bytes. */
inline constexpr std::size_t kThirdRowStride = 80;

/** Wrapper class of the vendor index blob. */
inline constexpr std::uint32_t kIndexWrapperClass = 0x8080784AU;
/** Element class of the vendor index array. */
inline constexpr std::uint32_t kIndexRowClass = 0x8080784EU;
/** Class of a vendor definition blob. */
inline constexpr std::uint32_t kDefinitionClass = 0x80807850U;
/** Element class of a definition's installed array. */
inline constexpr std::uint32_t kInstalledRowClass = 0x80807860U;
/** Element class of a definition's sale array. */
inline constexpr std::uint32_t kSaleRowClass = 0x80807861U;

/** Sale row +176 carries this when the row names no secondary item. */
inline constexpr std::uint16_t kAbsentSecondaryItem = 0xFFFFU;
/** Sale row +100 carries this when it selects no installed row. The client tests for it. */
inline constexpr std::int32_t kAbsentInstalledIndex = -1;

/** One row of the installed vendor index, which maps a vendor hash to its definition tag. */
struct IndexEntry {
    std::uint32_t definitionHash{};
    std::uint32_t definitionTag{};
    /** Row position, which is the index the opcode-901 request carries. */
    std::uint16_t index{};
};

/** One extracted vendor definition and the flat-bank ranges its rows occupy. */
struct Definition {
    std::uint32_t definitionHash{};
    std::uint32_t definitionTag{};
    /** Class the package entry records, which must be `kDefinitionClass`. */
    std::uint32_t definitionClass{};
    /** Definition blob size. Every array must end inside it. */
    std::uint32_t definitionSize{};
    /** First installed row, as an offset into the definition blob. */
    std::uint32_t installedRowBase{};
    std::uint32_t installedRowClass{};
    /** First sale row, as an offset into the definition blob. */
    std::uint32_t saleRowBase{};
    std::uint32_t saleRowClass{};
    /** First row of the unnamed third array, as an offset into the definition blob. */
    std::uint32_t thirdRowBase{};
    std::uint32_t thirdRowClass{};
    /** First row of this definition's range in the flat sale bank. */
    std::uint32_t saleRowOffset{};
    /** First row of this definition's range in the flat installed bank. */
    std::uint32_t installedRowOffset{};
    /** Raw definition +20. Its unit, epoch and scope are open, so it is not converted. */
    std::uint32_t resetIntervalRaw{};
    /** Raw definition +24, paired with the interval and equally open. */
    std::uint32_t resetPhaseRaw{};
    /** Row of the vendor index this definition is named by. */
    std::uint16_t index{};
    std::uint16_t installedCount{};
    std::uint16_t saleCount{};
    std::uint16_t thirdCount{};
};

/**
 * One sale row of one vendor definition.
 * Do not name a raw field a cost, award, stock or quantity without its mutation reader.
 */
struct SaleRow {
    /** Vendor index row of the definition owning this sale row. */
    std::uint16_t vendorIndex{};
    /** Sale row ordinal inside that definition. */
    std::uint16_t rowIndex{};
    /** Row +70. Main sale item-definition index. */
    std::uint16_t itemIndex{};
    /** Row +176. `kAbsentSecondaryItem` when the row names none. */
    std::uint16_t secondaryItemIndex{};
    /** Row +100. Row of the owning definition's installed array, and of a parallel runtime table.
     */
    std::int32_t installedIndex{};
    /** Row +104, raw f32 bits. Role open. */
    std::uint32_t raw104{};
    /** Row +108. Role open. */
    std::uint32_t raw108{};
    /** Row +172. Role open, and it is not the field the runtime selector reads. */
    std::int32_t raw172{};
    /** Row +8 expression count. */
    std::uint32_t expressionCount8{};
    /** Row +32 nested-record count. */
    std::uint32_t nestedRecordCount{};
    /** Row +120 expression count. */
    std::uint32_t expressionCount120{};
    /** Row +136 array count. Role open. */
    std::uint32_t count136{};
    /** Row +160 inline expression count. */
    std::uint32_t expressionCount160{};
    /** Row +154 feature branch byte. */
    std::uint8_t featureBranch{};
};

/** One installed row, kept whole because no field of it has a closed consumer. */
struct InstalledRow {
    /** Vendor index row of the definition owning this installed row. */
    std::uint16_t vendorIndex{};
    /** Installed row ordinal inside that definition. */
    std::uint16_t rowIndex{};
    std::array<std::uint8_t, kInstalledRowStride> raw{};
};

} // namespace sunrise::state::build_data::vendors
