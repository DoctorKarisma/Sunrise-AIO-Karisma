#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/vendors/definition.h"
#include "layout.h"
#include "vendor_build.h"

namespace sunrise::client::content::vendors {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace domain = state::build_data::vendors;

/** Every extracted row, kept off the caller stack. */
struct Storage {
    std::vector<std::byte> blob{};
    std::array<domain::IndexEntry, domain::kIndexCapacity> index{};
    std::array<domain::Definition, domain::kDefinitionCapacity> definitions{};
    std::array<domain::SaleRow, domain::kSaleRowCapacity> saleRows{};
    std::array<domain::InstalledRow, domain::kInstalledRowCapacity> installedRows{};
    std::size_t indexCount{};
    std::size_t definitionCount{};
    std::size_t saleRowCount{};
    std::size_t installedRowCount{};
};

/** One array a definition or a sale row declares, reduced to what the catalog stores. */
struct ArrayView {
    std::uint32_t base{};
    std::uint32_t classId{};
    std::uint16_t count{};
};

/** @param blob Source bytes. @param offset Field offset. @param value Receives the field. */
template <typename Value>
[[nodiscard]] bool
read(std::span<const std::byte> blob, std::size_t offset, Value& value) noexcept {
    if (offset > blob.size() || blob.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return true;
}

/**
 * Reads one array descriptor and bounds it against the blob holding it.
 * The raw count is read first, because the shared resolver reports absent and corrupt alike.
 * @param blob Whole blob owning the descriptor.
 * @param descriptor Descriptor offset.
 * @param stride One row's size.
 * @param output Receives the array, or an absent array.
 * @return True when the array is absent, or resolves and ends inside the blob.
 */
[[nodiscard]] bool read_array(std::span<const std::byte> blob,
                              std::size_t descriptor,
                              std::size_t stride,
                              ArrayView& output) noexcept {
    /** Row counts are stored as unsigned 16-bit values. */
    constexpr std::uint64_t kMaximumCount = (std::numeric_limits<std::uint16_t>::max)();
    output = {};
    std::uint64_t declared = 0;
    if (!read(blob, descriptor, declared)) {
        return false;
    }
    if (declared == 0) {
        return true;
    }
    tables::Array array{};
    if (!tables::find_array_at(blob, descriptor, array) || array.count > kMaximumCount) {
        return false;
    }
    const std::uint64_t end = array.dataOffset + (array.count * stride);
    if (end > blob.size()) {
        return false;
    }
    output = {static_cast<std::uint32_t>(array.dataOffset),
              array.elementClass,
              static_cast<std::uint16_t>(array.count)};
    return true;
}

/**
 * Reads one array's declared count from a sale row, without resolving its header.
 * @param blob Whole definition blob.
 * @param descriptor Descriptor offset inside the blob.
 * @param count Receives the declared count.
 * @return True when the count is inside the blob and fits the stored width.
 */
[[nodiscard]] bool
read_count(std::span<const std::byte> blob, std::size_t descriptor, std::uint32_t& count) noexcept {
    /** Sale-row array counts are stored as unsigned 32-bit values. */
    constexpr std::uint64_t kMaximumCount = (std::numeric_limits<std::uint32_t>::max)();
    count = 0;
    std::uint64_t declared = 0;
    if (!read(blob, descriptor, declared) || declared > kMaximumCount) {
        return false;
    }
    count = static_cast<std::uint32_t>(declared);
    return true;
}

/**
 * Reads the whole installed vendor index.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Pass storage receiving the index rows.
 * @return True when the index blob reads and every row fits.
 */
[[nodiscard]] bool
read_index(const reader::Source& source, reader::Scratch& scratch, Storage& storage) noexcept {
    std::uint32_t classId = 0;
    tables::Array array{};
    if (!reader::read_tag(source, scratch, kIndexRootTag, storage.blob, classId)
        || classId != domain::kIndexWrapperClass) {
        return false;
    }
    const std::span<const std::byte> blob{storage.blob};
    if (!tables::find_array_at(blob, tables::kTableArrayDescriptor, array)
        || array.elementClass != domain::kIndexRowClass || array.count > domain::kIndexCapacity) {
        return false;
    }
    for (std::uint64_t row = 0; row < array.count; ++row) {
        tables::IndexRow entry{};
        if (!tables::index_row(blob, array, row, entry)) {
            return false;
        }
        storage.index[storage.indexCount] = {
            entry.definitionHash, entry.targetTag, static_cast<std::uint16_t>(row)};
        ++storage.indexCount;
    }
    return storage.indexCount != 0;
}

/**
 * Reads every sale row of one definition into the flat bank.
 * @param blob Whole definition blob.
 * @param definition Definition whose sale array was already resolved.
 * @param storage Pass storage receiving the rows.
 * @return True when every row is inside the blob and the bank holds them all.
 */
[[nodiscard]] bool read_sale_rows(std::span<const std::byte> blob,
                                  const domain::Definition& definition,
                                  Storage& storage) noexcept {
    if (definition.saleCount > domain::kSaleRowCapacity - storage.saleRowCount) {
        return false;
    }
    for (std::size_t row = 0; row < definition.saleCount; ++row) {
        const std::size_t at = definition.saleRowBase + (row * domain::kSaleRowStride);
        domain::SaleRow& value = storage.saleRows[storage.saleRowCount + row];
        value = {};
        value.vendorIndex = definition.index;
        value.rowIndex = static_cast<std::uint16_t>(row);
        if (!read(blob, at + kSaleItemIndexOffset, value.itemIndex)
            || !read(blob, at + kSaleSecondaryItemOffset, value.secondaryItemIndex)
            || !read(blob, at + kSaleInstalledIndexOffset, value.installedIndex)
            || !read(blob, at + kSaleRaw104Offset, value.raw104)
            || !read(blob, at + kSaleRaw108Offset, value.raw108)
            || !read(blob, at + kSaleRaw172Offset, value.raw172)
            || !read(blob, at + kSaleFeatureBranchOffset, value.featureBranch)
            || !read_count(blob, at + kSaleExpression8Offset, value.expressionCount8)
            || !read_count(blob, at + kSaleNestedRecordOffset, value.nestedRecordCount)
            || !read_count(blob, at + kSaleExpression120Offset, value.expressionCount120)
            || !read_count(blob, at + kSaleCount136Offset, value.count136)
            || !read_count(blob, at + kSaleExpression160Offset, value.expressionCount160)) {
            return false;
        }
    }
    storage.saleRowCount += definition.saleCount;
    return true;
}

/**
 * Reads every installed row of one definition into the flat bank, whole.
 * @param blob Whole definition blob.
 * @param definition Definition whose installed array was already resolved.
 * @param storage Pass storage receiving the rows.
 * @return True when every row is inside the blob and the bank holds them all.
 */
[[nodiscard]] bool read_installed_rows(std::span<const std::byte> blob,
                                       const domain::Definition& definition,
                                       Storage& storage) noexcept {
    if (definition.installedCount > domain::kInstalledRowCapacity - storage.installedRowCount) {
        return false;
    }
    for (std::size_t row = 0; row < definition.installedCount; ++row) {
        const std::size_t at = definition.installedRowBase + (row * domain::kInstalledRowStride);
        domain::InstalledRow& value = storage.installedRows[storage.installedRowCount + row];
        value = {};
        value.vendorIndex = definition.index;
        value.rowIndex = static_cast<std::uint16_t>(row);
        if (!read(blob, at, value.raw)) {
            return false;
        }
    }
    storage.installedRowCount += definition.installedCount;
    return true;
}

/**
 * Reads one vendor definition and both of its row arrays.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param entry Index row naming the definition.
 * @param storage Pass storage receiving the definition and its rows.
 * @return True when the definition blob reads and every array ends inside it.
 */
[[nodiscard]] bool read_definition(const reader::Source& source,
                                   reader::Scratch& scratch,
                                   const domain::IndexEntry& entry,
                                   Storage& storage) noexcept {
    /** Definition sizes are stored as unsigned 32-bit values. */
    constexpr std::size_t kMaximumSize = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t classId = 0;
    if (storage.definitionCount == domain::kDefinitionCapacity
        || !reader::read_tag(source, scratch, entry.definitionTag, storage.blob, classId)
        || classId != domain::kDefinitionClass || storage.blob.size() > kMaximumSize) {
        return false;
    }
    const std::span<const std::byte> blob{storage.blob};
    ArrayView installed{};
    ArrayView sale{};
    ArrayView third{};
    if (!read_array(blob, kInstalledArrayDescriptor, domain::kInstalledRowStride, installed)
        || !read_array(blob, kSaleArrayDescriptor, domain::kSaleRowStride, sale)
        || !read_array(blob, kThirdArrayDescriptor, domain::kThirdRowStride, third)) {
        return false;
    }
    domain::Definition definition{};
    definition.definitionHash = entry.definitionHash;
    definition.definitionTag = entry.definitionTag;
    definition.definitionClass = classId;
    definition.definitionSize = static_cast<std::uint32_t>(blob.size());
    definition.index = entry.index;
    definition.installedRowBase = installed.base;
    definition.installedRowClass = installed.classId;
    definition.installedCount = installed.count;
    definition.saleRowBase = sale.base;
    definition.saleRowClass = sale.classId;
    definition.saleCount = sale.count;
    definition.thirdRowBase = third.base;
    definition.thirdRowClass = third.classId;
    definition.thirdCount = third.count;
    definition.saleRowOffset = static_cast<std::uint32_t>(storage.saleRowCount);
    definition.installedRowOffset = static_cast<std::uint32_t>(storage.installedRowCount);
    if (!read(blob, kResetIntervalOffset, definition.resetIntervalRaw)
        || !read(blob, kResetPhaseOffset, definition.resetPhaseRaw)
        || !read_sale_rows(blob, definition, storage)
        || !read_installed_rows(blob, definition, storage)) {
        return false;
    }
    storage.definitions[storage.definitionCount] = definition;
    ++storage.definitionCount;
    return true;
}

/** @param hashes Requested hashes. @param hash Index row hash. @return True when requested. */
[[nodiscard]] bool requested(std::span<const std::uint32_t> hashes, std::uint32_t hash) noexcept {
    for (const std::uint32_t value : hashes) {
        if (value == hash) {
            return true;
        }
    }
    return false;
}

/**
 * Reports the pass so a boot with no vendor catalog says which step lost the rows.
 * @param storage Pass storage holding every count.
 * @param skipped Requested definitions that could not be read or could not fit.
 * @param result Outcome text for the log line.
 */
void report(const Storage& storage, std::size_t skipped, const char* result) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=build_data stage=vendors index=%zu definitions=%zu "
                                      "sale=%zu installed=%zu skipped=%zu result=%s",
                                      storage.indexCount,
                                      storage.definitionCount,
                                      storage.saleRowCount,
                                      storage.installedRowCount,
                                      skipped,
                                      result);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         storage.indexCount != 0 && skipped == 0 ? core::log::Level::info
                                                                 : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Extracts and publishes the vendor catalog from the installed packages. */
bool build(const reader::Source& source,
           reader::Scratch& scratch,
           std::span<const std::uint32_t> definitionHashes) noexcept {
    if (state::build_data::vendor_catalog_ready()) {
        return true;
    }
    static Storage storage{};
    storage = {};
    if (!read_index(source, scratch, storage)) {
        report(storage, 0, "index");
        return false;
    }
    // Walking the index in order gives the ascending definition order the catalog requires.
    //
    // A definition that cannot be read - or cannot fit the definition or row banks - costs that
    // vendor alone, not the pass. Failing whole here is what a full bank used to do, and it was
    // the worst failure this domain had: the empty catalog was cached, every later boot restored
    // it, and every vendor stayed unresolvable with one boot-time line to say why.
    std::size_t skipped = 0;
    for (std::size_t row = 0; row < storage.indexCount; ++row) {
        const domain::IndexEntry entry = storage.index[row];
        if (!requested(definitionHashes, entry.definitionHash)) {
            continue;
        }
        if (read_definition(source, scratch, entry, storage)) {
            continue;
        }
        ++skipped;
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=build_data stage=vendors result=skip hash=0x%08X "
                                          "row=%zu definitions=%zu sale=%zu",
                                          entry.definitionHash,
                                          row,
                                          storage.definitionCount,
                                          storage.saleRowCount);
        if (written > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    const bool published = state::build_data::publish_vendor_catalog(
        std::span(storage.index).first(storage.indexCount),
        std::span(storage.definitions).first(storage.definitionCount),
        std::span(storage.saleRows).first(storage.saleRowCount),
        std::span(storage.installedRows).first(storage.installedRowCount));
    report(storage, skipped, published ? "ok" : "publish");
    return published;
}

} // namespace sunrise::client::content::vendors
