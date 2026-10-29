// MediaPool of media_fleet.h over the Rust rpc pool: one pool per media
// endpoint, built on the update client with the fleet limits and the main
// client's token lent to each pool connection. Owner thread only.
#pragma once

#include "mtproto/impulse/glue/media_fleet.h"

namespace Impulse {

class CentrifugoRust;

// Builds the factory that MediaFleet calls to open the pool of one media
// endpoint. The client must outlive the fleet that uses the factory. The
// client closes every pool it still holds when it is destroyed, so no pool
// outlives it.
[[nodiscard]] MediaPoolFactory makeRustMediaPoolFactory(CentrifugoRust *client);

} // namespace Impulse
