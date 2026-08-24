#include <Windows.h>

#include <algorithm>
#include <array>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/reader/reader.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../middleware/content/packages/tables/items.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/build_data/abilities/definition.h"
#include "../../../../state/build_data/inventory/buckets/definition.h"
#include "../../../../state/build_data/items/details/definition.h"
#include "../../../../state/build_data/nodes/node_persistence.h"
#include "../../../../state/build_data/progressions/definition.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/build_data/socket_entry_lists/definition.h"
#include "../../../../state/build_data/vendors/vendor_catalog.h"
#include "../../../../state/content/content_catalog.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../memory/current_process_memory.h"
#include "../../../targets/game.h"
#include "../../entity_names/entity_name_build.h"
#include "../../hash_names/hash_name_build.h"
#include "../../scenarios/scenario_build.h"
#include "../../spawn_sets/spawn_set_build.h"
#include "../../vendors/vendor_build.h"
#include "build.h"
#include "internal.h"
#include "package_socket_plug_build.h"

namespace sunrise::client::content::items::packages {
namespace {

/**
 * Publishes the vendor catalog, index and definitions both.
 *
 * `vendors::build` always reads the whole index but reads a definition only for a hash it is asked
 * for, because each is over 100 KiB. The hashes only exist once the index is read, so this runs it
 * twice: once to learn them, then again to read every definition the index names. Without the
 * second pass a vendor purchase resolves its index row and then finds no definition behind it.
 *
 * @param source Package directory and borrowed block keys.
 * @param scratch Block storage shared with the other content passes.
 */
void build_vendor_catalog(const reader::Source& source, reader::Scratch& scratch) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    if (state::build_data::vendor_catalog_ready()) {
        return;
    }
    if (!content::vendors::build(source, scratch, {})) {
        return;
    }
    static std::array<vendor_domain::IndexEntry, vendor_domain::kIndexCapacity> index{};
    std::size_t count = 0;
    if (!vendor_domain::snapshot_index(index, count) || count == 0) {
        return;
    }
    // Only the first `kDefinitionCapacity` rows are read. The domain is sized for a handful of
    // vendors on purpose, because each definition is over 100 KiB: asking for all 511 overruns the
    // sale-row bank at 32 definitions and publishes nothing but the index. The Tower's vendors sit
    // low in the index, so the leading window covers them.
    const std::size_t wanted = (std::min)(count, vendor_domain::kDefinitionCapacity);
    static std::array<std::uint32_t, vendor_domain::kDefinitionCapacity> hashes{};
    for (std::size_t row = 0; row < wanted; ++row) {
        hashes[row] = index[row].definitionHash;
    }
    // The first pass published an index with no definitions behind it, so it has to be dropped
    // before the second pass will run at all.
    vendor_domain::clear();
    (void)content::vendors::build(source, scratch, std::span(hashes).first(wanted));
}

/** @return True when every domain owned by the package pass is published. */
[[nodiscard]] bool package_domains_ready() noexcept {
    return state::build_data::item_definitions_ready()
           && state::build_data::collectible_definitions_ready()
           && state::build_data::material_requirement_sets_ready()
           && state::build_data::configured_item_details_ready()
           && state::build_data::socket_plug_rules_ready()
           && state::build_data::inventory_bucket_descriptors_ready()
           && state::build_data::socket_entry_lists_ready()
           && state::build_data::ability_buckets_ready()
           && state::build_data::socket_entry_buckets_ready()
           && state::build_data::progression_definitions_ready()
           && state::build_data::scenario_layouts_ready() && state::build_data::spawn_sets_ready()
           && state::build_data::hash_names_ready() && state::build_data::entity_names_ready()
           && state::build_data::investment_constants_ready()
           && state::build_data::exotic_catalysts_ready();
}

/** @return True when every item and investment-root domain is published. */
[[nodiscard]] bool root_domains_ready() noexcept {
    return state::build_data::item_definitions_ready()
           && state::build_data::collectible_definitions_ready()
           && state::build_data::material_requirement_sets_ready()
           && state::build_data::configured_item_details_ready()
           && state::build_data::socket_plug_rules_ready()
           && state::build_data::inventory_bucket_descriptors_ready()
           && state::build_data::socket_entry_lists_ready()
           && state::build_data::ability_buckets_ready()
           && state::build_data::socket_entry_buckets_ready()
           && state::build_data::progression_definitions_ready()
           && state::build_data::investment_constants_ready()
           && state::build_data::exotic_catalysts_ready();
}

} // namespace

/** Publishes the dense item table from the installed packages, once. */
bool build() noexcept {
    if (package_domains_ready()) {
        return true;
    }

    static Storage storage{};
    reader::BlockKeys keys{};
    core::path::Buffer directory{};

    if (!collect_keys(keys)) {
        report(0, "keys");
        return false;
    }

    std::size_t rowCount = 0;
    const char* reason = "directory";

    if (!package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        report(0, reason);
        return false;
    }

    // The destination layouts and the spawn sets share this pass's directory, keys, and block
    // storage. Both are independent of the item table, so a failure here leaves it alone.
    {
        const reader::Source packageSource{directory.chars.data(), &keys};

        (void)content::scenarios::build(packageSource, storage.scratch);
        (void)content::spawn_sets::build(packageSource, storage.scratch);
        (void)content::hash_names::build(packageSource, storage.scratch);
        (void)content::entity_names::build(packageSource, storage.scratch);
        build_vendor_catalog(packageSource, storage.scratch);

        if (package_domains_ready()) {
            SecureZeroMemory(&keys, sizeof keys);
            return true;
        }
    }

    if (root_domains_ready()) {
        SecureZeroMemory(&keys, sizeof keys);
        return true;
    }

    reason = "tag";

    std::array<std::uint32_t, kContainerCandidates> candidates{};
    std::size_t candidateCount = 0;

    const bool named = investment_globals_tags(candidates, candidateCount);

    if (named) {
        const reader::Source source{directory.chars.data(), &keys};
        tables::Array table{};
        bool located = false;

        reason = "read";

        for (std::size_t candidate = 0; candidate < candidateCount && !located; ++candidate) {
            if (!reader::read_tag(
                    source, storage.scratch, candidates[candidate], storage.container)) {
                continue;
            }

            // Fixed navigation: globals child zero is the investment root, whose slot holds the
            // item table, whose array descriptor sits at a fixed offset.
            std::uint32_t rootTag = 0;
            std::uint32_t rootClass = 0;
            std::uint32_t tableTag = 0;

            reason = "root";

            if (!tables::child_tag(std::span<const std::byte>{storage.container},
                                   tables::kInvestmentRootChild,
                                   rootTag)
                || rootTag == 0 || tables::package_of(rootTag) == tables::kAbsentPackageId
                || !reader::read_tag(source, storage.scratch, rootTag, storage.child, rootClass)
                || rootClass != tables::kInvestmentRootClass) {
                continue;
            }

            // The same root names the bucket and socket-list tables.
            storage.root = storage.child;

            if (!state::build_data::socket_plug_rules_ready()) {
                std::uint32_t plugSetTag = 0;
                tables::Array plugSets{};

                reason = "plug_sets";

                if (!tables::slot_tag(std::span<const std::byte>{storage.root},
                                      tables::kPlugSetTableSlot,
                                      plugSetTag)
                    || plugSetTag == 0
                    || !reader::read_tag(source, storage.scratch, plugSetTag, storage.plugSetTable)
                    || !tables::find_array_at(std::span<const std::byte>{storage.plugSetTable},
                                              tables::kTableArrayDescriptor,
                                              plugSets)) {
                    continue;
                }
            }

            if (!state::build_data::exotic_catalysts_ready()) {
                reason = "catalyst_gates";

                if (!read_catalyst_acquisition_gates(source,
                                                     storage.scratch,
                                                     std::span<const std::byte>{storage.root},
                                                     storage.child,
                                                     storage.catalystAcquisitionGates)
                    || !read_catalyst_objective_values(source,
                                                       storage.scratch,
                                                       std::span<const std::byte>{storage.root},
                                                       storage.child,
                                                       storage.catalystObjectiveValues)) {
                    continue;
                }
            }

            reason = "buckets";

            if (!build_buckets(source, storage, std::span<const std::byte>{storage.root})) {
                continue;
            }

            (void)build_socket_entry_lists(
                source, storage, std::span<const std::byte>{storage.root});

            if (!state::build_data::progression_definitions_ready()) {
                std::size_t progressionCount = 0;

                if (build_progressions(source,
                                       storage.scratch,
                                       std::span<const std::byte>{storage.root},
                                       storage.child,
                                       storage.progressionRows,
                                       progressionCount)) {
                    (void)state::build_data::publish_progression_definitions(
                        std::span(storage.progressionRows).first(progressionCount));
                }
            }

            if (!state::build_data::node_definitions_ready()) {
                std::size_t nodeCount = 0;
                if (build_nodes(source,
                                storage.scratch,
                                std::span<const std::byte>{storage.root},
                                storage.child,
                                storage.nodeRows,
                                nodeCount)) {
                    if (state::build_data::publish_node_definitions(
                            std::span(storage.nodeRows).first(nodeCount))) {
                        // Kept in its own file, so the next start does not need this pass at all.
                        (void)state::build_data::nodes::store(
                            std::span(storage.nodeRows).first(nodeCount));
                    }
                }
            }

            if (!state::build_data::record_definitions_ready()) {
                std::size_t recordCount = 0;
                if (build_records(source,
                                  storage.scratch,
                                  std::span<const std::byte>{storage.root},
                                  storage.child,
                                  storage.recordRows,
                                  recordCount)) {
                    (void)state::build_data::publish_record_definitions(
                        std::span(storage.recordRows).first(recordCount));
                }
            }

            if (!state::build_data::investment_constants_ready()) {
                state::build_data::constants::InvestmentConstants extracted{};

                if (read_investment_constants(source,
                                              storage.scratch,
                                              std::span<const std::byte>{storage.root},
                                              storage.child,
                                              extracted)) {
                    (void)state::build_data::publish_investment_constants(extracted);
                }
            }

            reason = "slot";

            if (!tables::slot_tag(
                    std::span<const std::byte>{storage.root}, tables::kItemTableSlot, tableTag)
                || tableTag == 0
                || !reader::read_tag(source, storage.scratch, tableTag, storage.child)) {
                continue;
            }

            reason = "table";

            located = tables::find_array_at(std::span<const std::byte>{storage.child},
                                            tables::kTableArrayDescriptor,
                                            table)
                      && table.elementClass == tables::kItemIndexTableClass;
        }

        if (located && build_item_rows(source, storage, table, rowCount, reason)) {
            if (!build_material_requirements(
                    source, storage, std::span<const std::byte>{storage.root}, table.count)) {
                reason = "materials";
            } else if (!build_collectibles(source,
                                           storage,
                                           std::span<const std::byte>{storage.root},
                                           table.count)) {
                reason = "collectibles";
            }
        }
    }

    SecureZeroMemory(&keys, sizeof keys);

    const bool complete = package_domains_ready();
    const bool itemDomainsReady = root_domains_ready();

    if (complete) {
        // Nothing reads a package again until the next boot, so this reader's files go back now.
        reader::close_files(storage.scratch);
    }

    // Scenario, spawn-set, and hash-name extraction advance over later refresh slices and report
    // their own progress. Do not mislabel one of those pending domains as the last item substage.
    report(itemDomainsReady ? state::build_data::item_definition_count() : 0, reason);

    return complete;
}

} // namespace sunrise::client::content::items::packages
