#pragma once

namespace sunrise::state::persistence {

/**
 * Applies the saved runtime overlay on top of the authored account, when one exists.
 *
 * The authored `settings.json` stays the baseline and is never written to. Everything the player
 * changes at runtime, meaning what is worn, what is carried, and what has been destroyed, lives in
 * a separate file beside it. Deleting that file restores the authored set exactly, and a corrupt
 * one costs nothing but the session's changes.
 *
 * Fail-safe in every direction: a missing, short, mismatched or invalid overlay is reported and
 * skipped, leaving the authored account in place. Settings load before the log sinks exist, and a
 * boot that dies there leaves no log at all, so this never refuses a boot.
 *
 * @param module Loaded DLL, used to resolve the owned artifact directory.
 */
void load(void* module) noexcept;

/**
 * Writes the mutable half of the account, replacing any previous overlay.
 *
 * Written to a sibling temporary and renamed over the target, so an interrupted write cannot leave
 * a half-file behind for the next boot to read.
 */
void save() noexcept;

} // namespace sunrise::state::persistence
