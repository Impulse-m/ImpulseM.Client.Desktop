// LiveKit call instance for 1:1 calls. It implements the tgcalls::Instance
// interface on top of LiveKitAdapter, so the call controller drives LiveKit
// through the same calls it uses for tgcalls. This class is the shell: every
// pure virtual is implemented, the signalling methods are no-ops, and failure
// states end the call. The join parameters and the E2EE wiring are added by
// the next step.
//
// All public methods run on the owner thread. Adapter events are handed to the
// owner thread through the poster.
#pragma once

#include "calls/livekit/livekit_adapter.h"

#include <tgcalls/Instance.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Impulse {

class LiveKitInstance final
	: public tgcalls::Instance
	, private LiveKitAdapterDelegate {
private:
	std::function<void(tgcalls::State)> stateUpdated;
	std::function<void(int)> signalBarsUpdated;
	std::unique_ptr<LiveKitAdapter> adapter;
	std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> incomingVideoOutput;
	std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture;
	tgcalls::TrafficStats trafficStats;
	tgcalls::NetworkType networkType = tgcalls::NetworkType::Unknown;
	std::string lastError;
	bool muteMicrophone = false;
	bool stopped = false;

public:
	LiveKitInstance(
		tgcalls::Descriptor &&descriptor,
		const std::wstring &dllPath,
		LiveKitAdapter::Poster post);
	LiveKitInstance(const LiveKitInstance &other) = delete;
	LiveKitInstance &operator=(const LiveKitInstance &other) = delete;
	~LiveKitInstance() override;


	void setNetworkType(tgcalls::NetworkType type) override;


	void setMuteMicrophone(bool mute) override;


	void setAudioOutputGainControlEnabled(bool enabled) override;


	void setEchoCancellationStrength(int strength) override;


	bool supportsVideo() override;


	void setIncomingVideoOutput(
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) override;


	void setAudioInputDevice(std::string id) override;


	void setAudioOutputDevice(std::string id) override;


	void setInputVolume(float level) override;


	void setOutputVolume(float level) override;


	void setAudioOutputDuckingEnabled(bool enabled) override;


	void setIsLowBatteryLevel(bool isLowBatteryLevel) override;


	std::string getLastError() override;


	std::string getDebugInfo() override;


	int64_t getPreferredRelayId() override;


	tgcalls::TrafficStats getTrafficStats() override;


	tgcalls::PersistentState getPersistentState() override;


	void receiveSignalingData(const std::vector<uint8_t> &data) override;


	void setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface> capture) override;


	void sendVideoDeviceUpdated() override;


	void setRequestedVideoAspect(float aspect) override;


	void stop(std::function<void(tgcalls::FinalState)> completion) override;


private:
	void liveKitConnected() override;


	void liveKitPublished(LiveKitPublishKind kind) override;


	void liveKitFailed(const std::string &message) override;


	void liveKitDisconnected() override;


	void fail(const std::string &message);
};

} // namespace Impulse
