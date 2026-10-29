// Group call instance on LiveKit. It takes the place of the tgcalls group
// instance for calls that use the ImpulseM transport. The join payload is the
// ssrc JSON that phone.joinGroupCall carries, and the join response carries
// the LiveKit connection parameters of the room. A presentation instance is a
// separate room that publishes no microphone and publishes the screen share.
//
// The instance drives its room through LiveKitAdapterInterface. The capture of
// the camera (or of the screen, for a presentation) is pushed into the room
// source as I420 frames, and the outgoing preview gets the same frames. Remote
// video frames go to the incoming outputs of the endpoint, which is the LiveKit
// identity of the participant; a presentation is the identity with the .p
// suffix, as on Android.
//
// Conferences, RTMP and broadcast are not part of this instance. The remote
// audio volume is not set, because the pinned protocol has no request for it,
// and device selection and external audio do nothing, as Android has no device
// override for LiveKit.
//
// All public methods run on the owner thread. Adapter callbacks are posted to
// the owner thread by the adapter poster, and captured frames are posted from
// the capture thread to the owner thread.
#pragma once

#include "base/weak_ptr.h"
#include "calls/livekit/livekit_adapter_interface.h"

#include <tgcalls/group/GroupInstanceImpl.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Impulse {

using LiveKitFrameOutput = std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>;

struct LiveKitGroupParams final {
	std::wstring dllPath;
	std::uint64_t groupCallId = 0;
	bool presentation = false;
};

class LiveKitGroupInstance final
	: public tgcalls::GroupInstanceInterface
	, public base::has_weak_ptr
	, private LiveKitAdapterDelegate
	, private LiveKitVideoSink {
private:
	std::function<void(tgcalls::GroupNetworkState)> networkStateUpdated;
	LiveKitGroupParams params;
	std::unique_ptr<LiveKitAdapterInterface> adapter;
	std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> captureSink;
	std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture;
	webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> videoSource;
	std::multimap<std::string, LiveKitFrameOutput> incomingOutputs;
	std::vector<LiveKitFrameOutput> outgoingOutputs;
	bool muted = false;
	bool noiseSuppression = true;
	bool connected = false;
	bool stopped = false;
	bool captureSourcePublished = false;

public:
	LiveKitGroupInstance(
		tgcalls::GroupInstanceDescriptor &&descriptor,
		const LiveKitGroupParams &params);
	LiveKitGroupInstance(const LiveKitGroupInstance &other) = delete;
	LiveKitGroupInstance &operator=(const LiveKitGroupInstance &other) = delete;
	~LiveKitGroupInstance();


	void stop(std::function<void()> completion) override;


	void setConnectionMode(
		tgcalls::GroupConnectionMode connectionMode,
		bool keepBroadcastIfWasEnabled,
		bool isUnifiedBroadcast) override;


	void emitJoinPayload(
		std::function<void(tgcalls::GroupJoinPayload const &)> completion) override;


	void setJoinResponsePayload(std::string const &payload) override;


	void removeSsrcs(std::vector<std::uint32_t> ssrcs) override;


	void removeIncomingVideoSource(std::uint32_t ssrc) override;


	void setIsMuted(bool isMuted) override;


	void setIsNoiseSuppressionEnabled(bool isNoiseSuppressionEnabled) override;


	void setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture) override;


	void setVideoSource(
		std::function<
			webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>()
		> getVideoSource) override;


	void setAudioOutputDevice(std::string id) override;


	void setAudioInputDevice(std::string id) override;


	void addExternalAudioSamples(std::vector<std::uint8_t> &&samples) override;


	void addOutgoingVideoOutput(
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) override;


	void addIncomingVideoOutput(
		std::string const &endpointId,
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) override;


	void setVolume(std::uint32_t ssrc, double volume) override;


	void setRequestedVideoChannels(
		std::vector<tgcalls::VideoChannelDescription> &&requestedVideoChannels) override;


	void getStats(std::function<void(tgcalls::GroupInstanceStats)> completion) override;


	void internal_addCustomNetworkEvent(bool isRemoteConnected) override;


private:
	void liveKitConnected() override;


	void liveKitPublished(LiveKitPublishKind kind) override;


	void liveKitFailed(const std::string &message) override;


	void liveKitDisconnected() override;


	void liveKitVideoFrame(const LiveKitVideoFrame &frame) override;


	void reportNetworkState(bool connected);


	void handleCapturedFrame(const webrtc::VideoFrame &frame);


	void releaseCaptures();

};

} // namespace Impulse
