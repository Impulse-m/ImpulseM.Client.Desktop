// Group call instance on LiveKit. It takes the place of the tgcalls group
// instance for calls that use the ImpulseM transport. The join payload is the
// ssrc JSON that phone.joinGroupCall carries, and the join response carries
// the LiveKit connection parameters of the room. A presentation instance is a
// separate room that publishes no microphone.
//
// Conferences, RTMP and broadcast are not part of this instance. Methods with
// no LiveKit counterpart in this port (remote track volume, outgoing and
// incoming video sinks, device selection, external audio) do nothing.
//
// All public methods run on the owner thread. Adapter callbacks are posted to
// the owner thread by the adapter poster.
#pragma once

#include "calls/livekit/livekit_adapter.h"

#include <tgcalls/group/GroupInstanceImpl.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Impulse {

struct LiveKitGroupParams final {
	std::wstring dllPath;
	std::uint64_t groupCallId = 0;
	bool presentation = false;
};

class LiveKitGroupInstance final
	: public tgcalls::GroupInstanceInterface
	, private LiveKitAdapterDelegate {
private:
	std::function<void(tgcalls::GroupNetworkState)> networkStateUpdated;
	LiveKitGroupParams params;
	bool muted = false;
	bool connected = false;
	bool stopped = false;
	std::unique_ptr<LiveKitAdapter> adapter;

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


	void reportNetworkState(bool connected);

};

} // namespace Impulse
