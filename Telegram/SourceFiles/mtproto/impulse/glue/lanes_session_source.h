// Feeds the channel lanes of one account from the chat list of its session.
// The candidates are the channel dialogs the account is still in, from the
// main chat list and the archive folder, with their last message dates. The
// channel open in any window of the session is passed as the opened channel
// and clears when that chat closes. The lanes observer of the account
// transport selects and subscribes them. The source is owned by the session
// and runs on the main thread.
#pragma once

#include "base/basic_types.h"
#include "base/flat_map.h"

#include <rpl/lifetime.h>

#include <cstdint>

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Impulse {

class LanesSessionSource final {
public:
	explicit LanesSessionSource(not_null<Main::Session*> owner);
	LanesSessionSource(const LanesSessionSource &other) = delete;
	LanesSessionSource &operator=(const LanesSessionSource &other) = delete;

	// Follows the chat shown in the window until the window is closed.
	void watchWindow(not_null<Window::SessionController*> window);

private:
	void push();
	void setShownChannel(
		not_null<Window::SessionController*> window,
		std::int64_t channelId);
	void forgetWindow(not_null<Window::SessionController*> window);
	void reconcileOpenedChannel();
	[[nodiscard]] bool isChannelShown(std::int64_t channelId) const;
	[[nodiscard]] std::int64_t firstShownChannel() const;

	const not_null<Main::Session*> session;
	base::flat_map<not_null<Window::SessionController*>, std::int64_t>
		shownChannels;
	std::int64_t openedChannelId = 0;
	rpl::lifetime lifetime;
};

} // namespace Impulse
