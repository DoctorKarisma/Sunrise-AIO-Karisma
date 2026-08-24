/**
 * Opcode 904 acquires a quest or other pursuit from a vendor.
 *
 * Identified by clicking one Banshee-44 quest with nothing else in the window: the only non-poll
 * request produced was
 * `opcode=904 payload_bytes=11 payload_hex=8016801C80008000010400`. The `701`/`702` pairs that
 * surround it in every log recur on a timer and are a poll.
 *
 * Six captured requests settle the layout. Three 16-bit fields biased by `0x8000` - vendor 22, the
 * clicked UI slot, and a field zero in every capture - then one 32-bit field biased by
 * `0x80000000` naming the sale row, then one trailing byte that is skipped rather than guessed at.
 *
 * | payload | vendor | slot | third | sale row |
 * |---|---|---|---|---|
 * | `8016 801C 8000 80000104 00` | 22 | 28 | 0 | 260 |
 * | `8016 801D 8000 800000C3 00` | 22 | 29 | 0 | 195 |
 * | `8016 8020 8000 80000109 00` | 22 | 32 | 0 | 265 |
 * | `8016 8015 8000 7FFFFFFF 00` | 22 | 21 | 0 | -1  |
 *
 * The last row is what fixes the width. `7FFFFFFF` is -1 under the 32-bit bias, the same absent
 * marker a sale row's own installed index carries; read as a bare 16-bit field it is 65535, a row
 * index that happens to be out of range for every vendor and so fails for the wrong reason.
 */

#include "opcode904_codec.h"

#include "../../../encoding/bit_reader.h"

namespace sunrise::middleware::web_service::messages::opcode904 {
namespace {

/** The leading fields are 16-bit signed values, as in 901. */
constexpr std::uint8_t kIndexWidth = 16;
/** Their descriptor bias is the signed 16-bit midpoint, as in 901. */
constexpr std::int32_t kIndexBias = 0x8000;
/** The sale row is a 32-bit signed value. */
constexpr std::uint8_t kSaleIndexWidth = 32;
/** Its bias is the signed 32-bit midpoint, which is the same rule one width up. */
constexpr std::int64_t kSaleIndexBias = 0x80000000;

/**
 * Reads one biased 16-bit index field.
 * @param reader Open reader.
 * @param output Receives the logical index.
 * @return True when the field was present.
 */
[[nodiscard]] bool read_index(encoding::bits::Reader& reader, std::int16_t& output) noexcept {
    std::uint64_t stored = 0;
    if (!reader.read(kIndexWidth, stored)) {
        return false;
    }
    output = static_cast<std::int16_t>(static_cast<std::int32_t>(stored) - kIndexBias);
    return true;
}

} // namespace

/** Decodes one quest-acquire request body. */
bool parse_request(const Message& message, Request& output) noexcept {
    if (message.opcode != kOpcode) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    Request candidate{};
    if (!read_index(reader, candidate.vendorIndex) || !read_index(reader, candidate.slotIndex)
        || !read_index(reader, candidate.third)) {
        return false;
    }
    std::uint64_t saleIndex = 0;
    if (reader.read(kSaleIndexWidth, saleIndex)) {
        candidate.saleIndex =
            static_cast<std::int32_t>(static_cast<std::int64_t>(saleIndex) - kSaleIndexBias);
        candidate.hasSaleIndex = true;
    }
    output = candidate;
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode904
