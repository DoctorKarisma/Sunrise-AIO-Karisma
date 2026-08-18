# Emote ownership flags

How this build decides an emote is owned, how the flags behind that were
recovered, and the data itself. None of this is public: the Bungie manifest
exposes only the failure message for these rules, never the expression behind
it, so everything here was read out of the installed packages directly.

## Where ownership actually lives

**An emote's ownership is gated by its own item definition, not by a
collectible.** Each emote item carries a plug rule -- the one whose failure
message is *"You do not own this emote"* -- and that rule's unlock expression
sits at **item-definition offset 720**. The enabled rule repeats the same
expression at offset 800.

The collectible table's acquired expression (`+112`) is a dead end for emotes:
only 94 of this build's 307 emote items have a collectible row at all, so a
collectible-driven pass cannot reach the other 213 no matter how it is written.

Decoding offset 720 for every item in the individual-emote bucket (**41**)
gives:

| | count |
| --- | ---: |
| emote items in this build | 307 |
| gated by an ownership flag | 291 |
| distinct flag slots behind them | 288 |
| carrying no expression at all | 16 |

The 16 ungated ones are always owned -- that is why Yes, Nope and Cheer worked
before any of this.

## Slots are not indices

This is the part that silently wastes a day.

`state.unlocks.account_flag_runs` in `default_settings.json` does **not** hold
flag slot numbers. It fills the account object's acquired-flag byte array, and
the client addresses that array **by row number in the unlock flag mapping
table**, not by slot. The two are unrelated number spaces.

The mapping table for this bank:

| | |
| --- | --- |
| investment root slot | 111 |
| tag | `81319322` |
| rows | 11923 |
| row shape | `{ u32 unlock_hash; i16 destination_slot; u16 zero; }` |

To set flag slot `s`, find the row whose `destination_slot == s`; that row's
**number** is the index to write. Writing `s` itself sets an unrelated flag.

Symptoms of getting this wrong, both observed here:

- Writing slot numbers as indices does nothing visible, because the indices
  that happen to be hit belong to unrelated slots.
- Filling the whole bank "works" for emotes but also sets every entitlement
  flag, which leaves the account unable to open the Director, the map, or
  orbit. Do not blanket fill.

Translating the 288 slots through the table yields **282 row indices**. The 6
that do not resolve have no row in this table (they are reachable only through
the family-5 override list, which is capped at 127 rows and so is not a route
for a set this size).

## The data

Ownership flag **slots** (288) -- what the item definitions name:

```
229, 237-239, 264, 271, 2218, 2222, 2232-2238, 2240, 2242-2259, 3813-3820,
3822-3836, 5200-5205, 5209-5220, 5228-5231, 5426-5427, 5429-5436, 6245-6264,
6896-6902, 7350-7371, 7373-7374, 8159-8165, 8167-8182, 8999-9010, 9012-9013,
9016-9021, 10408-10428, 10907-10932, 11431-11457, 11740-11770
```

Mapping-table **row indices** (282) -- what `account_flag_runs` must contain:

```
807, 811, 821-827, 829, 831-848, 2090-2097, 2099-2113, 3080-3085, 3089-3100,
3107-3110, 3217-3218, 3220-3227, 3729-3748, 4195-4201, 4428-4449, 4451-4452,
5057-5063, 5065-5080, 5676-5687, 5689-5690, 5693-5698, 6353-6373, 6734-6759,
7148-7174, 7404-7434
```

161 of those indices were already set by the authored data, which is the 179
emotes that were already owned (161 gated + 16 ungated + 2 sharing a flag).
The change added the remaining 121.

None of the 288 slots collides with any documented entitlement, platform, or
pre-release slot. The Director, map and orbit were verified working afterwards.

## Regenerating this

The flag list is specific to this build; a content change invalidates it. To
rebuild it, three temporary passes over the package data are needed:

1. For every item with `bucketId == 41`, decode the unlock expression at
   definition offset 720. A single `opcode 1` instruction carries the flag slot
   as its operand. Expressions resolve as
   `target = (offset of the pointer field) + (value stored there) + 16`, the
   same self-relative form plus 16-byte block header that `find_array_at` uses.
2. Read the mapping table at investment root slot 111 and build
   `destination_slot -> row index`.
3. Run-length-encode the union of the existing runs and the new indices.

Watch the cache while doing this. `stale_format()` treats only
`version < kCacheFormatVersion` as rebuildable, so a `build_data.bin` written
by a *newer* format version is rejected outright rather than regenerated, which
fails state initialisation and surfaces in the client as
*"Verify integrity of game files"*. If the cache format version is changed and
then reverted, delete `build_data.bin`.

## Known gap

These flags are recorded as a set. Which flag belongs to which *named* emote
was never captured -- the extraction logged slots without their item hashes. It
matters only if emotes ever need unlocking selectively rather than all at once.
