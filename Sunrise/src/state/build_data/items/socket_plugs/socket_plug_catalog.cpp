#include "socket_plug_catalog.h"

#include <algorithm>
#include <bitset>

#include "../../table.h"
#include "../details/item_detail_catalog.h"

namespace sunrise::state::build_data::items::socket_plugs {
namespace {

Lock g_lock;
Table<Rule, kRuleCapacity> g_rules;
Table<Pool, kPoolCapacity> g_pools;
Table<Member, kMemberCapacity> g_members;
std::bitset<details::kDefinitionCapacity> g_membership;

/** @return True when the first rule's item/lane key is strictly before the second. */
[[nodiscard]] bool rule_less(const Rule& left, const Rule& right) noexcept {
    return left.itemDefinitionIndex < right.itemDefinitionIndex
           || (left.itemDefinitionIndex == right.itemDefinitionIndex && left.lane < right.lane);
}

} // namespace

/** Clears the complete socket-plug catalog under one exclusive hold. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_rules.clear();
    g_pools.clear();
    g_members.clear();
    g_membership.reset();
}

/** Checks counts, strict rule order, contiguous pools, and sorted unique pool members. */
bool valid(std::span<const Rule> rules,
           std::span<const Pool> pools,
           std::span<const Member> members) noexcept {
    if (rules.empty() || rules.size() > kRuleCapacity || pools.empty()
        || pools.size() > kPoolCapacity || members.size() > kMemberCapacity
        || pools.front().memberOffset != 0 || pools.front().memberCount != 0) {
        return false;
    }

    for (std::size_t index = 0; index < rules.size(); ++index) {
        const Rule& rule = rules[index];
        if (rule.reserved != 0 || rule.lane >= kLaneCapacity || rule.poolIndex >= pools.size()
            || (index != 0 && !rule_less(rules[index - 1], rule))) {
            return false;
        }
    }

    std::size_t expectedOffset = 0;

    for (std::size_t index = 0; index < pools.size(); ++index) {
        const Pool& pool = pools[index];

        if (pool.memberOffset != expectedOffset || (index != 0 && pool.memberCount == 0)
            || pool.memberCount > members.size() - expectedOffset) {
            return false;
        }

        const auto range = members.subspan(expectedOffset, pool.memberCount);

        if (!std::is_sorted(range.begin(), range.end())
            || std::adjacent_find(range.begin(), range.end()) != range.end()
            || std::any_of(range.begin(), range.end(), [](Member member) {
                   return member >= details::kDefinitionCapacity;
               })) {
            return false;
        }

        expectedOffset += pool.memberCount;
    }

    return expectedOffset == members.size();
}

/** Replaces all three arrays while no reader can observe a partial relation. */
bool replace(std::span<const Rule> rules,
             std::span<const Pool> pools,
             std::span<const Member> members) noexcept {
    if (!valid(rules, pools, members)) {
        return false;
    }

    std::bitset<details::kDefinitionCapacity> membership;

    // Normal selectable socket-plug pool members.
    for (const Member member : members) {
        membership.set(member);
    }

    /*
     * Fixed/native plugs can exist only as a socket's initial plug and may
     * therefore never appear in an authored selectable pool.
     *
     * The Gear Editor needs these definitions to be recognized as valid
     * plug definitions so unrestricted socket editing can install them.
     *
     * This does NOT change allowed(). Normal socket compatibility remains
     * exactly as authored by the socket-plug rules.
     */
    for (std::size_t definitionIndex = 0; definitionIndex < details::kDefinitionCapacity;
         ++definitionIndex) {
        details::Definition detail{};

        if (!details::find(static_cast<std::uint16_t>(definitionIndex), detail)
            || detail.ordinarySocketState != details::OrdinarySocketState::present) {
            continue;
        }

        for (std::size_t lane = 0; lane < detail.ordinarySocketCount; ++lane) {
            const std::uint16_t initial = detail.initialPlugIndices[lane];

            if (initial != details::kUnavailableItemIndex && initial < membership.size()) {
                membership.set(initial);
            }
        }
    }

    const Lock::Exclusive guard(g_lock);

    if (!g_rules.replace(rules) || !g_pools.replace(pools) || !g_members.replace(members)) {
        return false;
    }

    g_membership = membership;
    return true;
}

/** Performs an exact item/lane rule lookup followed by a binary search in its plug pool. */
bool allowed(std::uint16_t itemDefinitionIndex,
             std::uint8_t lane,
             std::uint16_t plugDefinitionIndex) noexcept {
    if (lane >= kLaneCapacity) {
        return false;
    }

    const Lock::Shared guard(g_lock);
    const auto rules = g_rules.rows();
    const auto pools = g_pools.rows();
    const auto members = g_members.rows();

    const Rule key{itemDefinitionIndex, lane, 0, 0};

    const auto found = std::lower_bound(rules.begin(), rules.end(), key, rule_less);

    if (found == rules.end() || found->itemDefinitionIndex != itemDefinitionIndex
        || found->lane != lane || found->poolIndex >= pools.size()) {
        return false;
    }

    const Pool& pool = pools[found->poolIndex];

    if (pool.memberOffset > members.size()
        || pool.memberCount > members.size() - pool.memberOffset) {
        return false;
    }

    const auto range = members.subspan(pool.memberOffset, pool.memberCount);

    return std::binary_search(range.begin(), range.end(), plugDefinitionIndex);
}

/** Walks the members of one lane's pool under a shared hold. */
bool visit_pool(std::uint16_t itemDefinitionIndex,
                std::uint8_t lane,
                MemberVisitor visitor,
                void* context) noexcept {
    if (lane >= kLaneCapacity || visitor == nullptr) {
        return false;
    }

    const Lock::Shared guard(g_lock);
    const auto rules = g_rules.rows();
    const auto pools = g_pools.rows();
    const auto members = g_members.rows();

    const Rule key{itemDefinitionIndex, lane, 0, 0};

    const auto found = std::lower_bound(rules.begin(), rules.end(), key, rule_less);

    if (found == rules.end() || found->itemDefinitionIndex != itemDefinitionIndex
        || found->lane != lane || found->poolIndex >= pools.size()) {
        return false;
    }

    const Pool& pool = pools[found->poolIndex];

    if (pool.memberOffset > members.size()
        || pool.memberCount > members.size() - pool.memberOffset) {
        return false;
    }

    for (const Member member : members.subspan(pool.memberOffset, pool.memberCount)) {
        if (!visitor(context, member)) {
            return false;
        }
    }

    return true;
}

/** Answers whether one definition occurs in any installed ordinary-socket plug pool. */
bool contains(Member plugDefinitionIndex) noexcept {
    const Lock::Shared guard(g_lock);

    return plugDefinitionIndex < g_membership.size() && g_membership.test(plugDefinitionIndex);
}

/** Copies the three related arrays under the same shared hold. */
bool snapshot(std::span<Rule> rules,
              std::size_t& ruleCount,
              std::span<Pool> pools,
              std::size_t& poolCount,
              std::span<Member> members,
              std::size_t& memberCount) noexcept {
    ruleCount = 0;
    poolCount = 0;
    memberCount = 0;

    const Lock::Shared guard(g_lock);

    return g_rules.snapshot(rules, ruleCount) && g_pools.snapshot(pools, poolCount)
           && g_members.snapshot(members, memberCount);
}

/** Reports the published rule count under the catalog lock. */
std::size_t rule_count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_rules.count();
}

} // namespace sunrise::state::build_data::items::socket_plugs
