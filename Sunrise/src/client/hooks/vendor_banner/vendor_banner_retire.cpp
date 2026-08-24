/**
 * Retires a vendor banner the player has answered.
 *
 * A vendor's banner is not a sale row: it is an interaction, chosen by a fixed type-priority table
 * that keeps the highest-priority row the picker's retire test does not reject. That test is the
 * shipped game's own mechanism for dropping an answered banner, and offline nothing ever answers
 * it - so a quest that has already been taken keeps being offered on every open, and no other
 * interaction can ever reach the screen.
 *
 * Answering it here is the whole fix. Every catalog-side attempt failed before this because they
 * were aimed at sale rows, which the banner does not read.
 */

#include "vendor_banner_retire.h"

#include <array>
#include <atomic>
#include <cstring>

#include "../../../state/build_data/vendors/definition.h"
#include "../../hooking/detour.h"

namespace sunrise::client::hooks::vendor_banner {

// The banner tracker is indexed by the installed vendor index, so it has to span the same range the
// vendor index itself does. A tracker that fell short would simply stop retiring banners for the
// vendors past its end, and nothing else would say so.
static_assert(kVendorCapacity >= state::build_data::vendors::kIndexCapacity);

namespace {

/** The picker's per-interaction retire test: `(picker state, interaction index) -> skip`. */
using RetireFn = bool(__fastcall*)(void*, std::uint16_t);

hooking::detour::Handle g_handle{};
std::atomic<RetireFn> g_original{nullptr};
std::atomic_bool g_installed{false};

/** Retired interactions, packed as `vendorIndex << 16 | interactionIndex`. Append only. */
std::array<std::atomic<std::uint32_t>, kSuppressionCapacity> g_suppressed{};
std::atomic<std::size_t> g_suppressedCount{0};

/** Interaction each vendor is showing, kept so an answered banner can be named afterwards. */
std::array<std::atomic<std::uint16_t>, kVendorCapacity> g_shown{};

/** @param vendorIndex Vendor row. @param interactionIndex Interaction row. @return Packed key. */
[[nodiscard]] constexpr std::uint32_t pack(std::uint16_t vendorIndex,
                                           std::uint16_t interactionIndex) noexcept {
    return (static_cast<std::uint32_t>(vendorIndex) << 16) | interactionIndex;
}

/** @param key Packed pair. @return True while the pair is retired. */
[[nodiscard]] bool is_suppressed(std::uint32_t key) noexcept {
    const std::size_t held = g_suppressedCount.load(std::memory_order_acquire);
    for (std::size_t slot = 0; slot < held; ++slot) {
        if (g_suppressed[slot].load(std::memory_order_relaxed) == key) {
            return true;
        }
    }
    return false;
}

/**
 * Answers the picker's retire test, skipping an interaction this vendor has already answered.
 *
 * It also records which vendor and interaction the picker is looking at, because this test runs
 * first for every row and is the only place both are readable together.
 *
 * @param self Borrowed picker state for one vendor.
 * @param interactionIndex Interaction row being tested.
 * @return True when the picker must skip the row.
 */
__declspec(noinline) bool __fastcall retired(void* self, std::uint16_t interactionIndex) noexcept {
    const RetireFn original = g_original.load(std::memory_order_acquire);
    if (self != nullptr) {
        const auto* const state = static_cast<const std::byte*>(self);
        std::uint16_t vendorIndex = 0;
        std::uint16_t selected = 0;
        std::memcpy(&vendorIndex, state + StateLayout::vendorIndex, sizeof vendorIndex);
        std::memcpy(&selected, state + StateLayout::selectedInteraction, sizeof selected);
        if (vendorIndex < kVendorCapacity) {
            g_shown[vendorIndex].store(selected, std::memory_order_relaxed);
            if (is_suppressed(pack(vendorIndex, interactionIndex))) {
                return true;
            }
        }
    }
    return original == nullptr ? false : original(self, interactionIndex);
}

} // namespace

/** Retires the interaction one vendor is showing right now. */
bool suppress_current(std::uint16_t vendorIndex) noexcept {
    if (vendorIndex >= kVendorCapacity) {
        return false;
    }
    const std::uint16_t shown = g_shown[vendorIndex].load(std::memory_order_relaxed);
    if (shown == kAbsentIndex) {
        return false;
    }
    const std::uint32_t key = pack(vendorIndex, shown);
    if (is_suppressed(key)) {
        return true;
    }
    const std::size_t slot = g_suppressedCount.load(std::memory_order_relaxed);
    if (slot >= kSuppressionCapacity) {
        return false;
    }
    g_suppressed[slot].store(key, std::memory_order_relaxed);
    g_suppressedCount.store(slot + 1, std::memory_order_release);
    return true;
}

/** Attaches the retire gate. */
bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    for (auto& shown : g_shown) {
        shown.store(kAbsentIndex, std::memory_order_relaxed);
    }
    std::byte* const target = scan_main_image_unique(kRetireSignature, "vendor_banner_retire");
    if (target == nullptr) {
        return false;
    }
    if (!hooking::detour::install({target, reinterpret_cast<void*>(&retired)}, g_handle)) {
        return false;
    }
    g_original.store(reinterpret_cast<RetireFn>(g_handle.original), std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    return true;
}

/** Detaches the gate and forgets every retirement. */
void uninstall() noexcept {
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    (void)hooking::detour::uninstall(g_handle);
    g_original.store(nullptr, std::memory_order_release);
    g_suppressedCount.store(0, std::memory_order_release);
}

} // namespace sunrise::client::hooks::vendor_banner
