// Live connect check of the Rust Centrifugo client. Compiled into impulse_tests
// only with -DIMPULSE_CENTRIFUGO_SMOKE=ON (cmake/impulse/tests.cmake), because
// it needs the network and the Rust archive. It dials the update fleet directly.
#ifdef IMPULSE_CENTRIFUGO_SMOKE

#include "mtproto/impulse/glue/centrifugo_rust.h"
#include "mtproto/impulse/tests/impulse_test.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>

using Impulse::CentrifugoConfig;
using Impulse::CentrifugoRust;

namespace {

struct SmokeQueue {
	std::mutex mutex;
	std::condition_variable condition;
	std::deque<std::function<void()>> tasks;

	void post(std::function<void()> task) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			tasks.push_back(std::move(task));
		}
		condition.notify_one();
	}

	// An empty function means the deadline passed before a task arrived.
	std::function<void()> take(std::chrono::steady_clock::time_point deadline) {
		std::unique_lock<std::mutex> lock(mutex);
		if (!condition.wait_until(lock, deadline, [this]() { return !tasks.empty(); })) {
			return std::function<void()>();
		}
		std::function<void()> task = std::move(tasks.front());
		tasks.pop_front();
		return task;
	}
};

} // namespace

IMPULSE_TEST(CentrifugoRustSmoke) {
	SmokeQueue queue;
	CentrifugoRust client;
	bool connected = false;
	bool finished = false;

	CentrifugoConfig config;
	config.endpointUrl = "wss://rpc.impulsem.io/connection/websocket?cf_ws_frame_ping_pong=true";
	config.websocketSubprotocol = true;

	const std::int32_t created = client.create(
		config,
		[&queue](std::function<void()> task) {
			queue.post(std::move(task));
		});
	IMPULSE_CHECK(created == Impulse::kCentrifugoStatusOk);
	if (created != Impulse::kCentrifugoStatusOk) {
		return;
	}

	const Impulse::CentrifugoRust::EventCallback onEvent = [&](const Impulse::CentrifugoEvent &event) {
		std::printf(
			"[event] kind=%d code=%u server_code=%u text=%s\n",
			static_cast<int>(event.kind),
			static_cast<unsigned int>(event.code),
			static_cast<unsigned int>(event.serverCode),
			event.text.c_str());
		if (event.kind == Impulse::CentrifugoEventKind::Connected) {
			connected = true;
			finished = true;
		} else if (event.kind == Impulse::CentrifugoEventKind::Disconnected) {
			finished = true;
		}
	};
	const std::int32_t listening = client.setEventCallback(onEvent);
	IMPULSE_CHECK(listening == Impulse::kCentrifugoStatusOk);

	const std::int32_t connecting = client.connect();
	IMPULSE_CHECK(connecting == Impulse::kCentrifugoStatusOk);

	const std::chrono::steady_clock::time_point deadline
		= std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!finished) {
		std::function<void()> task = queue.take(deadline);
		if (!task) {
			break;
		}
		task();
	}

	IMPULSE_CHECK(connected);
	IMPULSE_CHECK(client.disconnect() == Impulse::kCentrifugoStatusOk);
}

#endif // IMPULSE_CENTRIFUGO_SMOKE
