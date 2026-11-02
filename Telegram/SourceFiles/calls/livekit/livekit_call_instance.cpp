#include "calls/livekit/livekit_call_instance.h"

#include "calls/livekit/livekit_ffi_loader.h"
#include "calls/livekit/livekit_video_frames.h"
#include "logs.h"

#include <api/video/video_source_interface.h>
#include <tgcalls/VideoCaptureInterface.h>
#include <tgcalls/VideoCaptureInterfaceImpl.h>

#include <QtCore/QString>

#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace Impulse {
namespace {

// The bars value reported on connect, as Android reports it for a 1:1 call.
constexpr int kConnectedSignalBars = 4;

// The session stats are requested this often while the room is connected.
constexpr int kStatsIntervalMs = 5000;

[[nodiscard]] const char *CaptureKindName(LiveKitPublishKind kind) {
	return (kind == LiveKitPublishKind::ScreenShare)
		? "screen share"
		: "camera";
}

// Runs the task on the media thread of the capture, where the capture object
// lives. A capture that is not the tgcalls implementation has no such object,
// so the task is skipped for it. Returns false when the task was skipped.
bool PerformOnCapture(
		const std::shared_ptr<tgcalls::VideoCaptureInterface> &capture,
		std::function<void(tgcalls::VideoCaptureInterfaceObject *)> task) {
	tgcalls::VideoCaptureInterfaceImpl *impl
		= dynamic_cast<tgcalls::VideoCaptureInterfaceImpl *>(capture.get());
	if (!impl) {
		return false;
	}
	impl->object()->perform(std::move(task));
	return true;
}

// The capture object lives on the media thread, so the sink is attached there.
void AttachCaptureSink(
		const std::shared_ptr<tgcalls::VideoCaptureInterface> &capture,
		const std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> &sink) {
	PerformOnCapture(capture, [sink](
			tgcalls::VideoCaptureInterfaceObject *object) {
		const webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source
			= object->source();
		if (source) {
			source->AddOrUpdateSink(sink.get(), rtc::VideoSinkWants());
		}
	});
}

// Removes the sink from the source of the capture, on the media thread. The
// task keeps the sink alive until the removal ran.
void DetachCaptureSink(
		const std::shared_ptr<tgcalls::VideoCaptureInterface> &capture,
		const std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> &sink) {
	PerformOnCapture(capture, [sink](
			tgcalls::VideoCaptureInterfaceObject *object) {
		const webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source
			= object->source();
		if (source) {
			source->RemoveSink(sink.get());
		}
	});
}

} // namespace

LiveKitInstance::LiveKitInstance(
		tgcalls::Descriptor &&descriptor,
		LiveKit::JoinParameters join,
		const std::wstring &dllPath,
		LiveKitAdapter::Poster post)
: stateUpdated(std::move(descriptor.stateUpdated))
, signalBarsUpdated(std::move(descriptor.signalBarsUpdated))
, remoteMediaStateUpdated(std::move(descriptor.remoteMediaStateUpdated))
, poster(post)
, joinParameters(std::move(join))
, adapter(std::make_unique<LiveKitAdapter>(
	LiveKitFfiLoader::Process(dllPath),
	static_cast<LiveKitAdapterDelegate &>(*this),
	std::move(post))) {
	LOG(("LiveKit: 1:1 call instance created"));
	networkType = descriptor.initialNetworkType;

	// The devices of the call are stored by the adapter until the platform
	// audio exists, so they are set before the room connects.
	const std::string inputId = descriptor.mediaDevicesConfig.audioInputId;
	const std::string outputId = descriptor.mediaDevicesConfig.audioOutputId;
	LOG(("LiveKit: input device %1, output device %2").arg(
		QString::fromStdString(inputId),
		QString::fromStdString(outputId)));
	if (!adapter->setRecordingDevice(inputId)) {
		LOG(("LiveKit: input device failed"));
	}
	if (!adapter->setPlayoutDevice(outputId)) {
		LOG(("LiveKit: output device failed"));
	}
	adapter->setVideoSink(this);
	// No remote video is shown until the first media snapshot names one, so the
	// adapter copies no remote frame before that.
	adapter->setShownVideoTrack(std::string());
	// A 1:1 call publishes its camera and its screen share without simulcast,
	// as Android does.
	adapter->setVideoSimulcast(false);
	statsTimer.setCallback([this] {
		requestStats();
	});

	// The raw 256-byte call key is the E2EE key material of a 1:1 call, the
	// same bytes Android passes to its key provider. It is set before the
	// room connects, so the first published frame is already encrypted.
	if (descriptor.encryptionKey.value) {
		const std::array<std::uint8_t, tgcalls::EncryptionKey::kSize> &key
			= *descriptor.encryptionKey.value;
		adapter->setSharedKey(std::vector<std::uint8_t>(key.begin(), key.end()));
		hasSharedKey = true;
	}

	// The captured frames of the call come from the capture thread. Each one is
	// posted to the owner thread, and the guard keeps a frame that arrives after
	// the instance is gone from touching it.
	const std::shared_ptr<bool> guard = alive;
	const LiveKitAdapter::Poster ownerPost = poster;
	captureSink = std::make_shared<LiveKitCaptureSink>(
		[this, guard, ownerPost](const webrtc::VideoFrame &frame) {
			// The planes are copied here, on the capture thread, so the owner
			// thread gets only the owned copy of the frame.
			const LiveKitCapturedFrame captured = ToCapturedFrame(frame);
			ownerPost([this, guard, captured]() {
				if (*guard) {
					handleCapturedFrame(captured);
				}
			});
		});

	// A call that starts with its camera on has its capture already.
	if (descriptor.videoCapture) {
		setVideoCapture(std::move(descriptor.videoCapture));
	}

	// The connect is deferred to the owner thread, so the call controller
	// finishes its setup before the first state can be reported. The guard
	// keeps a task that runs after the instance is gone from touching it.
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
	statsTimer.cancel();
	releaseCapture();
	adapter->setVideoSink(nullptr);
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

void LiveKitInstance::setAudioInputDevice(std::string id) {
	if (stopped) {
		return;
	}
	LOG(("LiveKit: input device %1").arg(QString::fromStdString(id)));
	if (!adapter->setRecordingDevice(id)) {
		LOG(("LiveKit: input device failed"));
	}
}

void LiveKitInstance::setAudioOutputDevice(std::string id) {
	if (stopped) {
		return;
	}
	LOG(("LiveKit: output device %1").arg(QString::fromStdString(id)));
	if (!adapter->setPlayoutDevice(id)) {
		LOG(("LiveKit: output device failed"));
	}
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
	// The debug log is sent to the server as JSON, so it is an object.
	return BuildLiveKitDebugJson(adapter->connected(), lastError);
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
	if (stopped) {
		return;
	}
	if (capture && capture == videoCapture) {
		return;
	}
	// The previous capture is released first. Its track is unpublished, and its
	// sink leaves the source.
	releaseCapture();
	if (capture) {
		videoCapture = std::move(capture);
		AttachCaptureSink(videoCapture, captureSink);
		checkCaptureKind(outgoingVideo.attachCapture());
	}
}

void LiveKitInstance::sendVideoDeviceUpdated() {
	if (stopped || !videoCapture) {
		return;
	}
	// The device switch is queued on the media thread before the kind check, so
	// the kind is read after the switch.
	const std::optional<std::uint64_t> generation = outgoingVideo.deviceSwitched();
	if (generation) {
		checkCaptureKind(*generation);
	}
}

void LiveKitInstance::setRequestedVideoAspect(float) {
	// The LiveKit path sends the published video size as it is.
}

void LiveKitInstance::stop(std::function<void(tgcalls::FinalState)> completion) {
	// The debug log is taken before the room is left, so it shows the state of
	// the call at the stop.
	tgcalls::FinalState finalState;
	finalState.debugLog = getDebugInfo();
	if (!stopped) {
		stopped = true;
		statsTimer.cancel();
		releaseCapture();
		adapter->disconnect();
		LOG(("LiveKit: call stopped"));
	}
	// The completion always fires, also when the room is still connecting or
	// has already failed.
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
	LOG(("LiveKit: connect started"));
	adapter->connect(joinParameters.url, joinParameters.token);
}

void LiveKitInstance::liveKitConnected() {
	if (stopped) {
		return;
	}
	LOG(("LiveKit: room connected"));
	// The microphone is published with the mute state of the call. A call that
	// starts muted publishes a muted track, so no live audio is sent. A failed
	// mute or publish has already reported the failure through liveKitFailed,
	// and then the state is not set to established.
	adapter->setMicrophoneMuted(muteMicrophone);
	if (!adapter->publishMicrophone()) {
		return;
	}
	// The camera and the screen share are published only from here on.
	outgoingVideo.roomConnected();
	statsTimer.callEach(kStatsIntervalMs);
	LOG(("LiveKit: established"));
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

void LiveKitInstance::liveKitUnpublished(LiveKitPublishKind kind) {
	LOG(("LiveKit: unpublish confirmed: %1").arg(
		QString::fromLatin1(CaptureKindName(kind))));
	outgoingVideo.unpublishConfirmed(kind);
}

void LiveKitInstance::liveKitFailed(const std::string &message) {
	fail(message);
}

void LiveKitInstance::liveKitDisconnected() {
	// Only a requested disconnect reaches here, and stop has already reported
	// the final state.
}

void LiveKitInstance::liveKitVideoFrame(const LiveKitVideoFrame &frame) {
	// Only the chosen remote video is shown, so the frames of the other tracks
	// are dropped here.
	if (stopped || shownVideoSid.empty() || frame.trackSid != shownVideoSid) {
		return;
	}
	const std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> output
		= incomingVideoOutput.lock();
	if (!output) {
		return;
	}
	const std::optional<webrtc::VideoFrame> native = ToNativeFrame(frame);
	if (native) {
		output->OnFrame(*native);
	}
	if (native && loggedFirstFrameSid != frame.trackSid) {
		loggedFirstFrameSid = frame.trackSid;
		const char *name = (frame.source == LiveKitTrackSource::ScreenShare)
			? "screen share"
			: "camera";
		LOG(("LiveKit: remote video first frame: %1 %2x%3")
			.arg(QString::fromLatin1(name))
			.arg(QString::number(frame.width))
			.arg(QString::number(frame.height)));
	}
}

void LiveKitInstance::liveKitSessionStats(const LiveKitSessionStats &stats) {
	if (stopped) {
		return;
	}
	// The stats arrive every few seconds, so the line is written for debugging only.
	if (Logs::DebugEnabled()) {
		LOG(("LiveKit: %1").arg(QString::fromStdString(
			FormatStatsLine(stats, captureCounters))));
	}
	if (stats.error) {
		return;
	}
	// The captured frames are counted per stats interval, so the counters restart
	// with every answered sample.
	captureCounters = LiveKitCaptureCounters();
	const LiveKitStatsSummary summary = SummarizeStats(stats);
	const std::uint64_t sent = summary.outboundAudioBytes
		+ summary.outboundVideoBytes;
	const std::uint64_t received = summary.inboundAudioBytes
		+ summary.inboundVideoBytes;
	// The counters are cumulative, so a sample adds only its growth since the
	// previous sample. The growth goes to the bucket of the current network type.
	// A counter that went down, when a stream is gone, adds nothing.
	const std::uint64_t sentDelta = (sent >= lastSentBytes)
		? (sent - lastSentBytes)
		: 0;
	const std::uint64_t receivedDelta = (received >= lastReceivedBytes)
		? (received - lastReceivedBytes)
		: 0;
	lastSentBytes = sent;
	lastReceivedBytes = received;
	if (networkType == tgcalls::NetworkType::WiFi
		|| networkType == tgcalls::NetworkType::Ethernet) {
		trafficStats.bytesSentWifi += sentDelta;
		trafficStats.bytesReceivedWifi += receivedDelta;
	} else {
		trafficStats.bytesSentMobile += sentDelta;
		trafficStats.bytesReceivedMobile += receivedDelta;
	}
}

void LiveKitInstance::liveKitRemoteMediaChanged(
		const std::vector<LiveKitRemoteMedia> &media) {
	if (stopped) {
		return;
	}
	// The peer is the only remote participant of a 1:1 room. A second identity
	// is reported once, and its tracks are treated as the peer's.
	const std::size_t identities = CountRemoteIdentities(media);
	if (identities > 1 && !warnedIdentities) {
		warnedIdentities = true;
		LOG(("LiveKit: warning, %1 remote participants in a 1:1 call").arg(
			QString::number(identities)));
	}

	const std::optional<LiveKitRemoteMedia> video = ChooseRemoteVideo(media);
	const std::string sid = video ? video->trackSid : std::string();
	if (sid != shownVideoSid) {
		shownVideoSid = sid;
		adapter->setShownVideoTrack(sid);
		const char *name = "none";
		if (video) {
			name = (video->source == LiveKitTrackSource::ScreenShare)
				? "screen share"
				: "camera";
		}
		LOG(("LiveKit: remote video: %1").arg(QString::fromLatin1(name)));
	}

	const LiveKitRemoteState state = ComputeRemoteState(media);
	if (state.audioActive == remoteState.audioActive
		&& state.videoActive == remoteState.videoActive) {
		return;
	}
	remoteState = state;
	LOG(("LiveKit: remote audio %1, remote video %2").arg(
		QString::fromLatin1(state.audioActive ? "active" : "muted"),
		QString::fromLatin1(state.videoActive ? "active" : "inactive")));
	if (remoteMediaStateUpdated) {
		remoteMediaStateUpdated(
			state.audioActive
				? tgcalls::AudioState::Active
				: tgcalls::AudioState::Muted,
			state.videoActive
				? tgcalls::VideoState::Active
				: tgcalls::VideoState::Inactive);
	}
}

void LiveKitInstance::liveKitLog(const std::string &line) {
	LOG(("LiveKit: %1").arg(QString::fromStdString(line)));
}

void LiveKitInstance::requestStats() {
	if (!stopped && adapter->connected()) {
		adapter->requestSessionStats();
	}
}

void LiveKitInstance::checkCaptureKind(std::uint64_t generation) {
	const std::shared_ptr<bool> guard = alive;
	const LiveKitAdapter::Poster ownerPost = poster;
	// The kind is read on the media thread, where the capture object lives. The
	// answer goes back to the owner thread with the generation of the check. A
	// capture without that object never answers, so its frames wait as pending.
	const bool reached = PerformOnCapture(
		videoCapture,
		[this, guard, ownerPost, generation](
				tgcalls::VideoCaptureInterfaceObject *object) {
			const bool isScreen = object->isScreenCapture();
			ownerPost([this, guard, generation, isScreen]() {
				if (*guard) {
					captureKindConfirmed(generation, isScreen);
				}
			});
		});
	if (!reached) {
		LOG(("LiveKit: capture kind check unavailable"));
	}
}

void LiveKitInstance::captureKindConfirmed(
		std::uint64_t generation,
		bool isScreen) {
	if (stopped) {
		return;
	}
	const LiveKitVideoKindResult result = outgoingVideo.kindConfirmed(
		generation,
		isScreen);
	if (!result.accepted) {
		return;
	}
	const LiveKitPublishKind kind = isScreen
		? LiveKitPublishKind::ScreenShare
		: LiveKitPublishKind::Camera;
	LOG(("LiveKit: video kind confirmed: %1").arg(
		QString::fromLatin1(CaptureKindName(kind))));
	if (result.unpublish) {
		unpublishKind(*result.unpublish);
	}
}

void LiveKitInstance::releaseCapture() {
	if (videoCapture) {
		DetachCaptureSink(videoCapture, captureSink);
		videoCapture = nullptr;
	}
	const std::optional<LiveKitPublishKind> unpublish
		= outgoingVideo.releaseCapture();
	// A stopped call leaves the room with its tracks, so nothing is unpublished.
	if (unpublish && !stopped) {
		unpublishKind(*unpublish);
	}
}

void LiveKitInstance::unpublishKind(LiveKitPublishKind kind) {
	const char *name = CaptureKindName(kind);
	LOG(("LiveKit: unpublish requested: %1").arg(QString::fromLatin1(name)));
	const bool requested = (kind == LiveKitPublishKind::ScreenShare)
		? adapter->unpublishScreenShare()
		: adapter->unpublishCamera();
	if (!requested) {
		LOG(("LiveKit: unpublish not accepted: %1").arg(
			QString::fromLatin1(name)));
	}
}

void LiveKitInstance::handleCapturedFrame(const LiveKitCapturedFrame &captured) {
	if (stopped) {
		return;
	}
	++captureCounters.received;
	const LiveKitVideoFrameDecision decision = outgoingVideo.frame(
		captured.width,
		captured.height);
	if (decision.action == LiveKitVideoFrameAction::Drop) {
		captureCounters.addDrop(decision.drop);
		return;
	}
	if (decision.action == LiveKitVideoFrameAction::PublishThenPush) {
		// The source is created with the size of the first frame. A publish that
		// fails fails the room. A publish that returns false while the room stays
		// connected is already in flight, so this frame is dropped and the next
		// frame asks again.
		const bool screen = (decision.kind == LiveKitPublishKind::ScreenShare);
		const bool started = screen
			? adapter->publishScreenShare(captured.width, captured.height)
			: adapter->publishCamera(captured.width, captured.height);
		outgoingVideo.publishResult(decision.kind, started);
		if (!started) {
			captureCounters.addDrop(LiveKitVideoDrop::PublishInFlight);
			return;
		}
		LOG(("LiveKit: %1 published, %2x%3")
			.arg(QString::fromLatin1(CaptureKindName(decision.kind)))
			.arg(QString::number(captured.width))
			.arg(QString::number(captured.height)));
	}
	if (adapter->captureVideoFrame(decision.kind, captured)) {
		++captureCounters.pushed;
	} else {
		captureCounters.addDrop(LiveKitVideoDrop::LibraryRejected);
	}
}

void LiveKitInstance::fail(const std::string &message) {
	lastError = message;
	statsTimer.cancel();
	LOG(("LiveKit: failed: %1").arg(QString::fromStdString(message)));
	if (stopped) {
		return;
	}
	// A failed state ends the call. The call controller hangs up on it.
	if (stateUpdated) {
		stateUpdated(tgcalls::State::Failed);
	}
}

} // namespace Impulse
