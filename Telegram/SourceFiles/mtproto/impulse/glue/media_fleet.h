// Media fleet: upload.saveFilePart, upload.saveBigFilePart, upload.getFile and
// upload.getWebFile go to the media endpoint of this DC, taken from the dc
// options of help.getConfig. Calls made before the endpoint is published wait
// in a queue for it. Without a media endpoint the calls fail; none of them
// falls back to the update socket. Owner thread only: every method and every
// callback runs on the thread that owns the fleet.
#pragma once

#include "mtproto/impulse/glue/carrier_errors.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Impulse {

inline constexpr std::int64_t kMediaCallTimeoutMs = 10000;
inline constexpr std::size_t kMediaRequestCapBytes = 557056;
inline constexpr std::size_t kMediaPartSizeBytes = 524288;
inline constexpr int kMediaConnections = 2;
inline constexpr int kMediaInflightPerConnection = 2;
inline constexpr int kMediaMaxQueued = 64;

// The fields of one help.getConfig dc option that the media fleet reads.
struct MediaDcOption {
	int id = 0;
	std::string host;
	int port = 0;
	bool mediaOnly = false;
};

// The media endpoint URL of thisDc: the first option with mediaOnly set and
// id equal to thisDc. Nullopt when there is no such option or its address is
// malformed.
[[nodiscard]] std::optional<std::string> mediaEndpointFor(
	const std::vector<MediaDcOption> &options,
	int thisDc);

struct MediaReply {
	CarrierOutcome outcome;
	std::vector<std::uint8_t> data;
};

using MediaDone = std::function<void(MediaReply reply)>;

// A pool of connections to one media endpoint. The glue implements it over
// the Rust rpc pool. The outcome given to a done callback is already mapped
// with mapCarrierResult(..., CarrierKind::Media). Every done callback runs
// exactly once on the owner thread.
class MediaPool {
public:
	virtual ~MediaPool() = default;

	// Starts a call and returns its id for cancel(). A call that cannot start
	// still settles through done.
	[[nodiscard]] virtual std::uint64_t send(
		const std::string &method,
		std::vector<std::uint8_t> data,
		std::int64_t timeoutMs,
		MediaDone done) = 0;

	// A call not sent yet fails at once; a sent call fails with a cancelled
	// outcome.
	virtual void cancel(std::uint64_t callId) = 0;

	// Closes every connection. Waiting and in-flight calls settle before this
	// returns.
	virtual void close() = 0;
};

// Builds a pool for one endpoint with the fleet limits: connections, calls in
// flight per connection, and calls queued in the pool.
using MediaPoolFactory = std::function<std::unique_ptr<MediaPool>(
	const std::string &endpointUrl,
	int connections,
	int inflightPerConnection,
	int maxQueued)>;

// Runs task after delayMs on the owner thread.
using MediaScheduler = std::function<void(
	std::int64_t delayMs,
	std::function<void()> task)>;

struct MediaFleetDependencies {
	MediaPoolFactory createPool;
	MediaScheduler schedule;
};

class MediaFleet {
private:
	struct Call {
		std::uint64_t id = 0;
		std::string method;
		std::vector<std::uint8_t> data;
		std::int64_t timeoutMs = 0;
		MediaDone done;
		std::uint64_t poolCallId = 0;
		std::uint64_t poolGeneration = 0;
		bool settled = false;
	};

	MediaPoolFactory createPool;
	MediaScheduler schedule;
	std::shared_ptr<bool> alive;
	std::unique_ptr<MediaPool> pool;
	std::uint64_t poolGeneration = 0;
	std::optional<std::string> endpoint;
	std::vector<std::shared_ptr<Call>> queue;
	std::map<std::uint64_t, std::shared_ptr<Call>> active;
	std::uint64_t nextId = 1;
	bool published = false;
	bool closed = false;

public:
	explicit MediaFleet(MediaFleetDependencies dependencies);
	MediaFleet(const MediaFleet &other) = delete;
	MediaFleet &operator=(const MediaFleet &other) = delete;

	// Callers must call close() first: the destructor drops the queued calls
	// without running their callbacks.
	~MediaFleet();

	// Publishes the media endpoint of this DC (nullopt when the config has no
	// media_only option for it). The first call publishes the config and sends
	// the calls queued before it. A changed endpoint retires the current pool.
	void setEndpoint(std::optional<std::string> endpointUrl);

	// The session changed: the current pool is closed, and the next call
	// builds a new one.
	void sessionChanged();

	// Sends one media call and returns its id. The timeout is capped at 10 s.
	// A zero timeout means the cap. The done callback runs exactly once.
	std::uint64_t call(
		std::string method,
		std::vector<std::uint8_t> data,
		std::int64_t timeoutMs,
		MediaDone done);

	// Cancels a queued call at once, or a sent one through the pool. The done
	// callback of the cancelled call runs once.
	void cancel(std::uint64_t callId);

	// Closes the pool and fails every queued and in-flight call. The fleet
	// accepts no call afterwards.
	void close();

private:
	std::shared_ptr<Call> takeQueued(std::uint64_t callId);
	MediaPool *poolForEndpoint();
	void dispatch(const std::shared_ptr<Call> &pending);
	void settle(const std::shared_ptr<Call> &pending, MediaReply reply);
	void expireQueued(std::uint64_t callId);
};

} // namespace Impulse
