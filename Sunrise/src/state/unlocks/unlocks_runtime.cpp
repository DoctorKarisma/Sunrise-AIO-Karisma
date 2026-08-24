#include "unlocks_runtime.h"

#include <Windows.h>

namespace sunrise::state::unlocks {
namespace {

Table g_table{};
SRWLOCK g_lock{SRWLOCK_INIT};

} // namespace

/** Publishes the immutable unlock policy for this process. */
void publish(const Table& table) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_table = table;
    ReleaseSRWLockExclusive(&g_lock);
}

/** @return The active unlock policy, or an empty policy when none was published. */
const Table& get() noexcept {
    return g_table;
}

/** Writes all three lanes of one progression. */
bool set_progression(build_data::progressions::Scope scope,
                     std::uint16_t definitionIndex,
                     ProgressionLanes lanes) noexcept {
    if (definitionIndex >= build_data::progressions::kDefinitionCapacity
        || scope == build_data::progressions::Scope::unreplicated) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    ProgressionBank& bank = scope == build_data::progressions::Scope::account
                                ? g_table.accountProgressions
                                : g_table.characterProgressions;
    bank[definitionIndex] = lanes;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Restores the empty unlock policy. */
void clear() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_table = Table{};
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::state::unlocks
