// Feeds the channel lanes of one account from the chat list of its session.
// The candidates are the channel dialogs the account is still in, from the
// main chat list and the archive folder, with their last message dates. The
// lanes observer of the account transport selects and subscribes them. The
// source is owned by the session and runs on the main thread.
#pragma once

#include "base/basic_types.h"

#include <rpl/lifetime.h>

namespace Main {
class Session;
} // namespace Main

namespace Impulse {

class LanesSessionSource final {
public:
	explicit LanesSessionSource(not_null<Main::Session*> owner);
	LanesSessionSource(const LanesSessionSource &other) = delete;
	LanesSessionSource &operator=(const LanesSessionSource &other) = delete;

private:
	void push();

	const not_null<Main::Session*> session;
	rpl::lifetime lifetime;
};

} // namespace Impulse
