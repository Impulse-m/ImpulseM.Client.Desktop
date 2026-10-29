// Realtime decode path of one account. The publications of the subscribed
// lanes become TL Updates for the updates sink; a publication that cannot be
// read, a wrong constructor, an updatesTooLong frame and a gap of a
// subscription become difference requests. Qt-free. Owner thread only: the
// owner creates it on the main thread, which is where the centrifuge client
// delivers its subscription events.
#pragma once

#include "mtproto/impulse/core/gap_tracker.h"
#include "mtproto/impulse/glue/centrifuge_client.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Impulse {

class Transcoder;

class Realtime {
public:
	// The serialized TL Updates of one publication, constructor first.
	using UpdatesSink = std::function<void(const std::vector<std::uint8_t> &updates)>;

	// Asks for a difference. An empty channel means the account difference;
	// a channel name (channel:{id}) is a per-channel gap, and the owner runs
	// the channel difference and falls back to the account difference.
	using DifferenceRequest = std::function<void(const std::string &reason, const std::string &channel)>;

	// Runs after every subscribe of a user lane, so the owner recomputes the
	// channel lanes. May be empty.
	using UserLaneSubscribed = std::function<void()>;

private:
	const Transcoder &transcoder;
	UpdatesSink updatesSink;
	DifferenceRequest differenceRequest;
	UserLaneSubscribed userLaneSubscribed;
	GapTracker gaps;
	std::shared_ptr<bool> alive;

public:
	// The transcoder must outlive the realtime object.
	Realtime(
		const Transcoder &transcoderRef,
		UpdatesSink updates,
		DifferenceRequest difference,
		UserLaneSubscribed userLane);
	Realtime(const Realtime &other) = delete;
	Realtime &operator=(const Realtime &other) = delete;
	~Realtime();

	// Subscribes a lane of the client with the realtime event routing. The
	// client must be the update fleet client of the same account.
	[[nodiscard]] std::int32_t subscribe(
		CentrifugeClient &client,
		const std::string &channel);

	[[nodiscard]] std::int32_t unsubscribe(
		CentrifugeClient &client,
		const std::string &channel);

private:
	void onEvent(
		const std::string &channel,
		const CentrifugeSubscriptionEvent &event);
	void onSubscribed(
		const std::string &channel,
		bool recovered,
		bool wasRecovering);
	void onPublication(const std::vector<std::uint8_t> &data);
	void decodeAndDeliver(const std::vector<std::uint8_t> &updatesProto);
	void requestDifference(
		std::string_view reason,
		const std::string &channel);
};

} // namespace Impulse
