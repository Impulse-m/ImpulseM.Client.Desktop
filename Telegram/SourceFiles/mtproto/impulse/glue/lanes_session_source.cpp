#include "mtproto/impulse/glue/lanes_session_source.h"

#include "data/data_channel.h"
#include "data/data_folder.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_session.h"
#include "mtproto/impulse/core/lanes.h"
#include "mtproto/impulse/glue/impulse_connection.h"
#include "mtproto/mtp_instance.h"

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
	connection->setLanes(std::move(candidates), 0);
}

} // namespace Impulse
