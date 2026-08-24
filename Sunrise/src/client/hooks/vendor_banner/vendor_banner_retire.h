#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::vendor_banner {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * The vendor picker's per-interaction retire test.
 *
 * The picker walks a vendor's interaction rows and keeps the highest-priority one this test does
 * not reject, so answering true for a row makes the picker skip it and choose the next. It is the
 * game's own mechanism for dropping a banner the player has answered: the picker state carries a
 * list of answered interactions, and the test returns true once a row's stored hash matches a
 * freshly computed one.
 *
 * Nothing appends to that list offline, so the test always answers false and the same banner is
 * chosen on every open however the player answered it - a quest stays offered after it is taken.
 * The prologue is clean and non-Arxan, and the signature is unique in the image.
 */
inline constexpr std::string_view kRetireSignatureText =
    "48 89 5C 24 ? 48 89 6C 24 ? 56 48 83 EC ? 44 8B 49 ? 33 ED 0F B7 DA 48 8B F1";
/** Compiled pattern bytes of the signature text above. */
inline constexpr auto kRetireSignature =
    signature<signature_length(kRetireSignatureText)>(kRetireSignatureText);

/** Fields of one vendor's picker state, as byte offsets from its base. */
struct StateLayout {
    /** Vendor index row, which is the index the wire carries. */
    static constexpr std::size_t vendorIndex = 0;
    /** Interaction the vendor is showing right now, or -1 while it shows none. */
    static constexpr std::size_t selectedInteraction = 2;
};

/** Vendors tracked, which matches the installed vendor index. */
inline constexpr std::size_t kVendorCapacity = 512;

/** Interactions that can be held retired at once, across every vendor. */
inline constexpr std::size_t kSuppressionCapacity = 256;

/** Value of a vendor or interaction slot that names nothing. */
inline constexpr std::uint16_t kAbsentIndex = 0xFFFFU;

/**
 * Attaches the retire gate.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the gate and forgets every retirement. */
void uninstall() noexcept;

/**
 * Retires the interaction one vendor is showing right now.
 *
 * Called once a vendor request has been answered, which is the point the shipped game appends its
 * own entry.
 *
 * @param vendorIndex Vendor whose banner was answered.
 * @return True when an interaction was showing and is now retired.
 */
bool suppress_current(std::uint16_t vendorIndex) noexcept;

} // namespace sunrise::client::hooks::vendor_banner
