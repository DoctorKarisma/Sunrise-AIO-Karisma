#pragma once

#include "../../client/network/consumer.h"

namespace sunrise::server::bap {

/** Arms a fresh account-graph push for authenticated sessions. */
[[nodiscard]] bool request_account_resync() noexcept;

/** Applies one connection-scoped BAP lifecycle event. */
[[nodiscard]] bool consume(const client::network::BapRequest& request,
                           client::network::BapResponse& response) noexcept;

/** Wipes every connection-owned nonce and transform buffer. */
void shutdown() noexcept;

} // namespace sunrise::server::bap
