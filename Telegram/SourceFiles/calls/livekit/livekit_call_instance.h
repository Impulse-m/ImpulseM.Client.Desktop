// LiveKit call instance for 1:1 calls. It implements the tgcalls::Instance
// interface on top of LiveKitAdapter, so the call controller drives LiveKit
// through the same calls it uses for tgcalls. The room is joined from the
// validated join parameters, with the call media key set as the E2EE shared
// key before the connect. The microphone is published with the mute state of
// the call. Connected and failed room states map to the tgcalls states, and a
// failed state ends the call. The stop completion fires on every path.
//
// The camera or the screen of the call is pushed into the room as I420 frames,
// taken from the capture of the call with a second sink, so the local preview
// keeps its own sink. The peer is shown with one remote video: its screen share
// when it plays, otherwise its camera. The audio devices of the call are applied
// through the adapter. Every five seconds, while connected, the session stats are
// requested and counted as traffic; they are logged only in debug logging.
//
// All public methods run on the owner thread. Adapter events are handed to the
// owner thread through the poster, and captured frames are posted from the
// capture thread to the owner thread.
#pragma once

#include "base/timer.h"
#include "calls/livekit/livekit_adapter.h"
#include "calls/livekit/livekit_call_logic.h"
#include "mtproto/impulse/core/livekit_contract.h"

#include <tgcalls/Instance.h>

#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Impulse {

class LiveKitInstance final
	: public tgcalls::Instance
	, private LiveKitAdapterDelegate
	, private LiveKitVideoSink {
private:
	std::function<void(tgcalls::State)> stateUpdated;
	std::function<void(int)> signalBarsUpdated;
	std::function<void(tgcalls::AudioState, tgcalls::VideoState)> remoteMediaStateUpdated;
	LiveKitAdapter::Poster poster;
	LiveKit::JoinParameters joinParameters;
	std::unique_ptr<LiveKitAdapter> adapter;
	std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> incomingVideoOutput;
	std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture;
	std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> captureSink;
	std::shared_ptr<std::atomic<bool>> screencastFlag
		= std::make_shared<std::atomic<bool>>(false);
	tgcalls::TrafficStats trafficStats;
	// The cumulative RTP byte totals of the last session stats sample.
	std::uint64_t lastSentBytes = 0;
	std::uint64_t lastReceivedBytes = 0;
	tgcalls::NetworkType networkType = tgcalls::NetworkType::Unknown;
	base::Timer statsTimer;
	LiveKitRemoteState remoteState;
	std::string shownVideoSid;
	std::string lastError;
	std::shared_ptr<bool> alive = std::make_shared<bool>(true);
	LiveKitPublishKind captureKind = LiveKitPublishKind::Camera;
	bool captureActive = false;
	bool cameraPublished = false;
	bool screenPublished = false;
	bool cameraEnabled = true;
	bool screenEnabled = true;
	bool hasSharedKey = false;
	// The microphone starts live, as a tgcalls instance does. A call that is
	// muted when it starts sets it through setMuteMicrophone before the connect.
	bool muteMicrophone = false;
	bool warnedIdentities = false;
	bool stopped = false;

public:
	LiveKitInstance(
		tgcalls::Descriptor &&descriptor,
		LiveKit::JoinParameters join,
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


	void liveKitVideoFrame(const LiveKitVideoFrame &frame) override;


	void liveKitSessionStats(const LiveKitSessionStats &stats) override;


	void liveKitRemoteMediaChanged(
		const std::vector<LiveKitRemoteMedia> &media) override;


	void liveKitLog(const std::string &line) override;


	void connectRoom();


	void requestStats();


	void releaseCapture();


	void applyCaptureState();


	void handleCapturedFrame(
		const LiveKitCapturedFrame &captured,
		LiveKitPublishKind kind);


	void fail(const std::string &message);
};

} // namespace Impulse
