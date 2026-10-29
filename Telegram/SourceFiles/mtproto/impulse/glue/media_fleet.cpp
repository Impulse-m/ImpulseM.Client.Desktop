#include "mtproto/impulse/glue/media_fleet.h"

#include "mtproto/impulse/core/endpoints.h"

#include <algorithm>
#include <utility>

namespace Impulse {
namespace {

[[nodiscard]] MediaReply localFailure(std::string text) {
	MediaReply reply;
	reply.outcome.kind = CarrierOutcomeKind::NetworkFailure;
	reply.outcome.error.text = std::move(text);
	return reply;
}

[[nodiscard]] std::int64_t deadlineMs(std::int64_t timeoutMs) {
	return timeoutMs <= 0
		? kMediaCallTimeoutMs
		: std::min(timeoutMs, kMediaCallTimeoutMs);
}

} // namespace

std::optional<std::string> mediaEndpointFor(
		const std::vector<MediaDcOption> &options,
		int thisDc) {
	for (const MediaDcOption &option : options) {
		if (option.mediaOnly && option.id == thisDc) {
			return Endpoints::MediaUrl(option.host, option.port);
		}
	}
	return std::nullopt;
}

MediaFleet::MediaFleet(MediaFleetDependencies dependencies)
: createPool(std::move(dependencies.createPool))
, schedule(std::move(dependencies.schedule))
, alive(std::make_shared<bool>(true)) {
}

MediaFleet::~MediaFleet() {
	*alive = false;
}

void MediaFleet::setEndpoint(std::optional<std::string> endpointUrl) {
	if (closed) {
		return;
	}
	const bool first = !published;
	published = true;
	if (!first && endpoint == endpointUrl) {
		return;
	}
	std::unique_ptr<MediaPool> retired = std::move(pool);
	pool.reset();
	endpoint = std::move(endpointUrl);
	std::vector<std::shared_ptr<Call>> toSend;
	if (first) {
		toSend = std::move(queue);
		queue.clear();
	}
	if (retired) {
		retired->close();
	}
	for (const std::shared_ptr<Call> &pending : toSend) {
		dispatch(pending);
	}
}

void MediaFleet::sessionChanged() {
	if (closed) {
		return;
	}
	std::unique_ptr<MediaPool> retired = std::move(pool);
	pool.reset();
	if (retired) {
		retired->close();
	}
}

std::uint64_t MediaFleet::call(
		std::string method,
		std::vector<std::uint8_t> data,
		std::int64_t timeoutMs,
		MediaDone done) {
	const std::shared_ptr<Call> pending = std::make_shared<Call>();
	pending->id = nextId++;
	pending->method = std::move(method);
	pending->data = std::move(data);
	pending->timeoutMs = deadlineMs(timeoutMs);
	pending->done = std::move(done);
	const std::uint64_t id = pending->id;
	if (closed) {
		settle(pending, localFailure("media fleet is closed"));
	} else if (pending->data.size() > kMediaRequestCapBytes) {
		settle(pending, localFailure("request exceeds the media cap for " + pending->method));
	} else if (!published) {
		if (queue.size() >= static_cast<std::size_t>(kMediaMaxQueued)) {
			settle(pending, localFailure("media queue is full for " + pending->method));
		} else {
			queue.push_back(pending);
			active.emplace(id, pending);
			const std::shared_ptr<bool> guard = alive;
			schedule(pending->timeoutMs, [this, guard, id]() {
				if (*guard) {
					expireQueued(id);
				}
			});
		}
	} else {
		active.emplace(id, pending);
		dispatch(pending);
	}
	return id;
}

void MediaFleet::cancel(std::uint64_t callId) {
	const std::shared_ptr<Call> queued = takeQueued(callId);
	if (queued) {
		settle(queued, localFailure("cancelled " + queued->method));
		return;
	}
	const std::map<std::uint64_t, std::shared_ptr<Call>>::iterator found = active.find(callId);
	if (found == active.end()) {
		return;
	}
	const std::shared_ptr<Call> inFlight = found->second;
	if (pool
		&& inFlight->poolCallId != 0
		&& inFlight->poolGeneration == poolGeneration) {
		pool->cancel(inFlight->poolCallId);
	}
}

void MediaFleet::close() {
	if (closed) {
		return;
	}
	closed = true;
	std::unique_ptr<MediaPool> retired = std::move(pool);
	pool.reset();
	if (retired) {
		retired->close();
	}
	std::vector<std::shared_ptr<Call>> drained = std::move(queue);
	queue.clear();
	for (const std::shared_ptr<Call> &queued : drained) {
		settle(queued, localFailure("media fleet is closed"));
	}
	std::vector<std::shared_ptr<Call>> rest;
	rest.reserve(active.size());
	for (const std::pair<const std::uint64_t, std::shared_ptr<Call>> &entry : active) {
		rest.push_back(entry.second);
	}
	for (const std::shared_ptr<Call> &leftover : rest) {
		settle(leftover, localFailure("media fleet is closed"));
	}
}

std::shared_ptr<MediaFleet::Call> MediaFleet::takeQueued(std::uint64_t callId) {
	for (std::vector<std::shared_ptr<Call>>::iterator it = queue.begin(); it != queue.end(); ++it) {
		if ((*it)->id == callId) {
			std::shared_ptr<Call> found = *it;
			queue.erase(it);
			return found;
		}
	}
	return nullptr;
}

MediaPool *MediaFleet::poolForEndpoint() {
	if (!pool && endpoint) {
		pool = createPool(
			*endpoint,
			kMediaConnections,
			kMediaInflightPerConnection,
			kMediaMaxQueued);
		++poolGeneration;
	}
	return pool.get();
}

void MediaFleet::dispatch(const std::shared_ptr<Call> &pending) {
	MediaPool *target = poolForEndpoint();
	if (!target) {
		settle(pending, localFailure("no media_only dc option for " + pending->method));
		return;
	}
	const std::shared_ptr<bool> guard = alive;
	const std::uint64_t callId = target->send(
		pending->method,
		pending->data,
		pending->timeoutMs,
		[this, guard, pending](MediaReply reply) {
			if (*guard) {
				settle(pending, std::move(reply));
			}
		});
	if (!pending->settled) {
		pending->poolCallId = callId;
		pending->poolGeneration = poolGeneration;
	}
}

void MediaFleet::settle(const std::shared_ptr<Call> &pending, MediaReply reply) {
	if (pending->settled) {
		return;
	}
	pending->settled = true;
	active.erase(pending->id);
	MediaDone done = std::move(pending->done);
	pending->done = nullptr;
	if (done) {
		done(std::move(reply));
	}
}

void MediaFleet::expireQueued(std::uint64_t callId) {
	const std::shared_ptr<Call> expired = takeQueued(callId);
	if (expired) {
		settle(expired, localFailure("no config yet for " + expired->method));
	}
}

} // namespace Impulse
