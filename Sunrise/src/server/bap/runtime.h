#pragma once

#include "../../client/network/consumer.h"

namespace sunrise::server::bap {

/** Applies one connection-scoped BAP lifecycle event. */
[[nodiscard]] bool consume(const client::network::BapRequest& request,
                           client::network::BapResponse& response) noexcept;

/** Queues a fresh Family-4 account graph for active sessions. */
[[nodiscard]] bool request_account_resync() noexcept;

/** Wipes every connection-owned nonce and transform buffer. */
void shutdown() noexcept;

} // namespace sunrise::server::bap
