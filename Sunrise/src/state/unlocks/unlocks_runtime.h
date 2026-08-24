#pragma once

#include "definition.h"

namespace sunrise::state::unlocks {

/**
 * Publishes the immutable unlock policy for this process.
 * @param table Complete authored policy.
 */
void publish(const Table& table) noexcept;

/** @return The active unlock policy, or an empty policy when none was published. */
[[nodiscard]] const Table& get() noexcept;

/**
 * Writes all three lanes of one progression.
 *
 * Writing only, deliberately: nothing here republishes. A progression credited with no transaction
 * behind it leaves the peer holding the value it read at login, so the caller asks for its account
 * graph to be resent instead.
 *
 * The lanes are absolute, not a delta, because a rank is derived by walking the definition's step
 * totals against lane 0 rather than by counting rank-ups - so a caller reads the lane it means to
 * change, adds to it, and writes the whole set back.
 *
 * @param scope Which bank the definition belongs to.
 * @param definitionIndex Progression definition to write.
 * @param lanes Absolute values for all three lanes.
 * @return True when the definition is in range and the values were applied.
 */
[[nodiscard]] bool set_progression(build_data::progressions::Scope scope,
                                   std::uint16_t definitionIndex,
                                   ProgressionLanes lanes) noexcept;

/** Restores the empty unlock policy. */
void clear() noexcept;

} // namespace sunrise::state::unlocks
