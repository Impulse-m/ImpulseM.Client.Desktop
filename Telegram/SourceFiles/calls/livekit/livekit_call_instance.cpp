#include "calls/livekit/livekit_call_instance.h"

#include "calls/livekit/livekit_ffi_loader.h"

#include <array>
#include <utility>
#include <vector>

namespace Impulse {
namespace {

// The bars value reported on connect, as Android reports it for a 1:1 call.
constexpr int kConnectedSignalBars = 4;

} // namespace

LiveKitInstance::LiveKitInstance(
		tgcalls::Descriptor &&descriptor,
		LiveKit::JoinParameters join,
		const std::wstring &dllPath,
		LiveKitAdapter::Poster post)
: stateUpdated(std::move(descriptor.stateUpdated))
, signalBarsUpdated(std::move(descriptor.signalBarsUpdated))
, poster(post)
, joinParameters(std::move(join))
, adapter(std::make_unique<LiveKitAdapter>(
	LiveKitFfiLoader::Process(dllPath),
	static_cast<LiveKitAdapterDelegate &>(*this),
	std::move(post))) {
	// The raw 256-byte call key is the E2EE key material of a 1:1 call, the
	// same bytes Android passes to its key provider. It is set before the
	// room connects, so the first published frame is already encrypted.
	if (descriptor.encryptionKey.value) {
		const std::array<std::uint8_t, tgcalls::EncryptionKey::kSize> &key
			= *descriptor.encryptionKey.value;
		adapter->setSharedKey(std::vector<std::uint8_t>(key.begin(), key.end()));
		hasSharedKey = true;
	}

	// The connect is deferred to the owner thread, so the call controller
	// finishes its setup before the first state can be reported. The guard
	// keeps a task that runs after the instance is gone from touching it.
	const std::shared_ptr<bool> guard = alive;
	poster([this, guard]() {
		if (*guard) {
			connectRoom();
		}
	});
}

LiveKitInstance::~LiveKitInstance() {
	// The adapter is destroyed after this body and releases its handles
	// without calling back, so no state is reported from here on.
	*alive = false;
	stopped = true;
}

void LiveKitInstance::setNetworkType(tgcalls::NetworkType type) {
	networkType = type;
}

void LiveKitInstance::setMuteMicrophone(bool mute) {
	muteMicrophone = mute;
	if (!stopped) {
		adapter->setMicrophoneMuted(mute);
	}
}

void LiveKitInstance::setAudioOutputGainControlEnabled(bool) {
	// LiveKit owns audio processing, so the tgcalls switch has no effect here.
}

void LiveKitInstance::setEchoCancellationStrength(int) {
	// LiveKit owns audio processing, so the tgcalls strength has no effect here.
}

bool LiveKitInstance::supportsVideo() {
	return true;
}

void LiveKitInstance::setIncomingVideoOutput(
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	incomingVideoOutput = std::move(sink);
}

void LiveKitInstance::setAudioInputDevice(std::string) {
	// The platform audio module owns device selection on the LiveKit path.
}

void LiveKitInstance::setAudioOutputDevice(std::string) {
	// The platform audio module owns device selection on the LiveKit path.
}

void LiveKitInstance::setInputVolume(float) {
	// Volume is not applied by the shell. The media wiring applies it.
}

void LiveKitInstance::setOutputVolume(float) {
	// Volume is not applied by the shell. The media wiring applies it.
}

void LiveKitInstance::setAudioOutputDuckingEnabled(bool) {
	// Ducking is not part of the LiveKit path.
}

void LiveKitInstance::setIsLowBatteryLevel(bool) {
	// The battery level does not change the LiveKit media path.
}

std::string LiveKitInstance::getLastError() {
	return lastError;
}

std::string LiveKitInstance::getDebugInfo() {
	return std::string("ImpulseM LiveKit; connected=")
		+ (adapter->connected() ? "true" : "false")
		+ "; error="
		+ lastError;
}

int64_t LiveKitInstance::getPreferredRelayId() {
	// LiveKit does not use tgcalls relays.
	return 0;
}

tgcalls::TrafficStats LiveKitInstance::getTrafficStats() {
	return trafficStats;
}

tgcalls::PersistentState LiveKitInstance::getPersistentState() {
	// The LiveKit path keeps no state between calls.
	return tgcalls::PersistentState();
}

void LiveKitInstance::receiveSignalingData(const std::vector<uint8_t> &) {
	// LiveKit signalling runs inside the room connection, not through the
	// tgcalls signalling channel.
}

void LiveKitInstance::setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface> capture) {
	videoCapture = std::move(capture);
}

void LiveKitInstance::sendVideoDeviceUpdated() {
	// Device updates reach the LiveKit path through the platform audio module.
}

void LiveKitInstance::setRequestedVideoAspect(float) {
	// The LiveKit path sends the published video size as it is.
}

void LiveKitInstance::stop(std::function<void(tgcalls::FinalState)> completion) {
	if (!stopped) {
		stopped = true;
		adapter->disconnect();
	}
	// The completion always fires, also when the room is still connecting or
	// has already failed.
	tgcalls::FinalState finalState;
	finalState.debugLog = getDebugInfo();
	finalState.trafficStats = trafficStats;
	if (completion) {
		completion(std::move(finalState));
	}
}

void LiveKitInstance::connectRoom() {
	if (stopped) {
		return;
	}
	if (!hasSharedKey) {
		// A call without its media key must not connect unencrypted.
		fail("LiveKit call media key is missing");
		return;
	}
	// The result arrives through liveKitConnected or liveKitFailed.
	adapter->connect(joinParameters.url, joinParameters.token);
}

void LiveKitInstance::liveKitConnected() {
	if (stopped) {
		return;
	}
	// The microphone is published with the mute state of the call. A call that
	// starts muted publishes a muted track, so no live audio is sent. A failed
	// mute or publish has already reported the failure through liveKitFailed,
	// and then the state is not set to established.
	adapter->setMicrophoneMuted(muteMicrophone);
	if (!adapter->publishMicrophone()) {
		return;
	}
	if (stateUpdated) {
		stateUpdated(tgcalls::State::Established);
	}
	if (signalBarsUpdated) {
		signalBarsUpdated(kConnectedSignalBars);
	}
}

void LiveKitInstance::liveKitPublished(LiveKitPublishKind) {
	// A publication does not change the call state. Media wiring follows it.
}

void LiveKitInstance::liveKitFailed(const std::string &message) {
	fail(message);
}

void LiveKitInstance::liveKitDisconnected() {
	// Only a requested disconnect reaches here, and stop has already reported
	// the final state.
}

void LiveKitInstance::fail(const std::string &message) {
	lastError = message;
	if (stopped) {
		return;
	}
	// A failed state ends the call. The call controller hangs up on it.
	if (stateUpdated) {
		stateUpdated(tgcalls::State::Failed);
	}
}

} // namespace Impulse
