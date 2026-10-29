#include "calls/livekit/livekit_call_instance.h"

#include <utility>

namespace Impulse {
namespace {

// The bars value reported on connect, as Android reports it for a 1:1 call.
constexpr int kConnectedSignalBars = 4;

} // namespace

LiveKitInstance::LiveKitInstance(
		tgcalls::Descriptor &&descriptor,
		const std::wstring &dllPath,
		LiveKitAdapter::Poster post)
: stateUpdated(std::move(descriptor.stateUpdated))
, signalBarsUpdated(std::move(descriptor.signalBarsUpdated))
, adapter(std::make_unique<LiveKitAdapter>(
	dllPath,
	static_cast<LiveKitAdapterDelegate &>(*this),
	std::move(post))) {
}

LiveKitInstance::~LiveKitInstance() {
	// The adapter is destroyed after this body and releases its handles
	// without calling back, so no state is reported from here on.
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

void LiveKitInstance::liveKitConnected() {
	if (stopped) {
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
