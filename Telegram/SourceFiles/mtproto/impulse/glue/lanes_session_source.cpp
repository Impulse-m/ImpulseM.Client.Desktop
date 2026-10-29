#include "mtproto/impulse/glue/lanes_session_source.h"

#include "data/data_channel.h"
#include "data/data_folder.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_key.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_session.h"
#include "mtproto/impulse/core/lanes.h"
#include "mtproto/impulse/glue/impulse_connection.h"
#include "mtproto/mtp_instance.h"
#include "window/window_session_controller.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace Impulse {

namespace {

void AppendChannelCandidates(
		std::vector<LaneCandidate> &candidates,
		not_null<Dialogs::IndexedList*> list) {
	for (const not_null<Dialogs::Row*> row : *list) {
		History *const history = row->history();
		if (!history) {
			continue;
		}
		const not_null<PeerData*> peer = history->peer;
		if (!peer->isChannel() || !peer->asChannel()->amIn()) {
			continue;
		}
		LaneCandidate candidate;
		candidate.channelId = static_cast<std::int64_t>(
			peerToChannel(peer->id).bare);
		candidate.lastMessageDate = row->entry()->chatListTimeId();
		candidates.push_back(candidate);
	}
}

[[nodiscard]] std::int64_t ChannelIdOfChat(const Dialogs::Key &chat) {
	PeerData *const peer = chat.peer();
	if (!peer || !peer->isChannel()) {
		return 0;
	}
	return static_cast<std::int64_t>(peerToChannel(peer->id).bare);
}

} // namespace

LanesSessionSource::LanesSessionSource(not_null<Main::Session*> owner)
: session(owner) {
	session->data().chatsListChanges(
	) | rpl::on_next([=] {
		push();
	}, lifetime);

	session->data().chatsListLoadedEvents(
	) | rpl::on_next([=] {
		push();
	}, lifetime);

	push();
}

void LanesSessionSource::watchWindow(
		not_null<Window::SessionController*> window) {
	window->activeChatValue(
	) | rpl::on_next([=](Dialogs::Key chat) {
		setShownChannel(window, ChannelIdOfChat(chat));
	}, window->lifetime());

	window->lifetime().add([=] {
		forgetWindow(window);
	});
}

void LanesSessionSource::setShownChannel(
		not_null<Window::SessionController*> window,
		std::int64_t channelId) {
	if (channelId != 0) {
		shownChannels[window] = channelId;
		openedChannelId = channelId;
	} else {
		shownChannels.remove(window);
	}
	reconcileOpenedChannel();
	push();
}

void LanesSessionSource::forgetWindow(
		not_null<Window::SessionController*> window) {
	shownChannels.remove(window);
	reconcileOpenedChannel();
	push();
}

void LanesSessionSource::reconcileOpenedChannel() {
	if (openedChannelId != 0 && isChannelShown(openedChannelId)) {
		return;
	}
	openedChannelId = firstShownChannel();
}

bool LanesSessionSource::isChannelShown(std::int64_t channelId) const {
	return std::any_of(
		shownChannels.begin(),
		shownChannels.end(),
		[=](const auto &entry) {
			return entry.second == channelId;
		});
}

std::int64_t LanesSessionSource::firstShownChannel() const {
	for (const auto &entry : shownChannels) {
		if (entry.second != 0) {
			return entry.second;
		}
	}
	return 0;
}

void LanesSessionSource::push() {
	std::vector<LaneCandidate> candidates;
	AppendChannelCandidates(
		candidates,
		session->data().chatsList()->indexed());
	Data::Folder *const archive = session->data().folderLoaded(
		Data::Folder::kId);
	if (archive) {
		AppendChannelCandidates(
			candidates,
			session->data().chatsList(archive)->indexed());
	}

	ImpulseConnection *const connection = session->mtp().impulse();
	if (!connection) {
		return;
	}
	connection->setLanes(std::move(candidates), openedChannelId);
}

} // namespace Impulse
