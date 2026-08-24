#pragma once

#include <cstdint>

#include "../../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode904 {

/** Web Service opcode for acquiring a quest or other pursuit from a vendor. */
inline constexpr std::uint16_t kOpcode = 904;

/**
 * One decoded quest-acquire request.
 *
 * Structurally a sibling of the opcode-901 purchase: 16-bit fields biased by `0x8000`. It carries
 * three of them and no clock, where 901 carries two and an optional clock, and then one 32-bit
 * field biased by `0x80000000` that names the sale row.
 */
struct Request {
    /** Index into the vendor table, the same table 901 indexes. */
    std::int16_t vendorIndex{};
    /**
     * UI slot the click landed on. Not a sale row: indexing sale rows with it grants armour mods.
     */
    std::int16_t slotIndex{};
    /** Third field. Zero in every captured request; role open. */
    std::int16_t third{};
    /**
     * Sale row of the vendor definition, as a 32-bit field biased by `0x80000000`.
     *
     * This is the only value that changes per quest. Four captures settle both the width and the
     * bias: `80000104`, `800000C3` and `80000109` are rows 260, 195 and 265, and `7FFFFFFF` is
     * **-1**, which is the same absent marker a sale row's own installed index uses. Reading only
     * the low half - as this codec first did - turns that -1 into 65535 and caps the row at 32767.
     */
    std::int32_t saleIndex{};
    /** True when the body carried the sale-row field. */
    bool hasSaleIndex{};
};

/**
 * Decodes one quest-acquire request body.
 *
 * Unlike 901 this does **not** refuse a body with whole bytes to spare. A captured request is 11
 * bytes and the four fields account for ten, so one byte follows that is not yet understood.
 * Refusing on it would reject every real request; reading it as a field would be inventing a
 * layout. It is skipped, and deliberately.
 *
 * @param message Parsed Web Service envelope.
 * @param output Receives the request only when the three leading fields decode.
 * @return True when the opcode matches and those fields are present.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& output) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode904
