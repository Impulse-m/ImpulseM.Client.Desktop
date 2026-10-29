#include "calls/group/livekit_group_instance.h"

#ifdef _WIN32

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "base/random.h"
#include "calls/livekit/livekit_adapter.h"
#include "calls/livekit/livekit_ffi_loader.h"

#include <api/media_stream_interface.h>
#include <crl/crl_on_main.h>
#include <tgcalls/VideoCaptureInterface.h>

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QUrl>

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <set>
#include <utility>

namespace Impulse {
namespace {

constexpr const char *kInvalidJoinFailure = "Invalid LiveKit group parameters";

struct LiveKitJoin final {
	std::string url;
	std::string token;
};

// Receives the frames of the capture on the capture thread. The owner posts the
// frame to the owner thread, so the adapter is only ever called there.
class LiveKitCaptureSink final : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
	explicit LiveKitCaptureSink(
		std::function<void(const webrtc::VideoFrame &)> deliverFrame);

	void OnFrame(const webrtc::VideoFrame &frame) override;

private:
	std::function<void(const webrtc::VideoFrame &)> deliver;

};

LiveKitCaptureSink::LiveKitCaptureSink(
		std::function<void(const webrtc::VideoFrame &)> deliverFrame)
: deliver(std::move(deliverFrame)) {
}

void LiveKitCaptureSink::OnFrame(const webrtc::VideoFrame &frame) {
	if (deliver) {
		deliver(frame);
	}
}

// Draws a source that is non-zero and not used yet in this payload.
[[nodiscard]] std::uint32_t UniqueSource(std::set<std::uint32_t> &used) {
	std::uint32_t source = 0;
	do {
		source = base::RandomValue<std::uint32_t>();
	} while (!source || !used.insert(source).second);
	return source;
}

[[nodiscard]] QJsonObject SourceGroup(
		const QString &semantics,
		const std::vector<std::uint32_t> &sources) {
	QJsonArray values;
	for (const std::uint32_t source : sources) {
		values.append(static_cast<qint64>(source));
	}
	QJsonObject group;
	group.insert(u"semantics"_q, semantics);
	group.insert(u"sources"_q, values);
	return group;
}

// Connection parameters of the room, checked the way the Android client checks
// them: a livekit object with url, token and room; a ws or wss URL with a host
// and no user info or fragment; a room named after the group call.
[[nodiscard]] std::optional<LiveKitJoin> ParseJoin(
		const std::string &payload,
		std::uint64_t groupCallId) {
	if (!groupCallId) {
		return std::nullopt;
	}
	const QJsonDocument document = QJsonDocument::fromJson(
		QByteArray::fromStdString(payload));
	const QJsonObject livekit = document.object().value(
		u"livekit"_q).toObject();
	const QString url = livekit.value(u"url"_q).toString();
	const QString token = livekit.value(u"token"_q).toString();
	const QString room = livekit.value(u"room"_q).toString();
	if (url.trimmed().isEmpty()
		|| token.trimmed().isEmpty()
		|| room.trimmed().isEmpty()) {
		return std::nullopt;
	}
	const QUrl parsed(url, QUrl::StrictMode);
	const QString scheme = parsed.scheme();
	if ((scheme != u"ws"_q && scheme != u"wss"_q)
		|| !parsed.isValid()
		|| parsed.host().isEmpty()
		|| !parsed.userInfo().isEmpty()
		|| parsed.hasFragment()) {
		return std::nullopt;
	}
	if (room != u"g."_q + QString::number(groupCallId)) {
		return std::nullopt;
	}
	LiveKitJoin result;
	result.url = url.toStdString();
	result.token = token.toStdString();
	return result;
}

// The presentation publishes the screen share, every other instance the camera.
[[nodiscard]] LiveKitPublishKind CaptureKind(bool presentation) {
	return presentation
		? LiveKitPublishKind::ScreenShare
		: LiveKitPublishKind::Camera;
}

// The requested layer of a remote video, as the Android client maps it: the
// full layer is high, the medium layer is medium, and the rest is low.
[[nodiscard]] LiveKitVideoQuality VideoQualityOf(
		tgcalls::VideoChannelDescription::Quality quality) {
	if (quality == tgcalls::VideoChannelDescription::Quality::Full) {
		return LiveKitVideoQuality::High;
	} else if (quality == tgcalls::VideoChannelDescription::Quality::Medium) {
		return LiveKitVideoQuality::Medium;
	}
	return LiveKitVideoQuality::Low;
}

// Copies one plane of an I420 buffer with its stride, so the library reads the
// bytes of the frame after the copy.
[[nodiscard]] LiveKitVideoPlane CopyPlane(
		const std::uint8_t *data,
		int stride,
		int rows) {
	LiveKitVideoPlane plane;
	if (!data || stride <= 0 || rows <= 0) {
		return plane;
	}
	plane.stride = static_cast<std::uint32_t>(stride);
	plane.bytes.assign(
		data,
		data + static_cast<std::size_t>(stride) * static_cast<std::size_t>(rows));
	return plane;
}

[[nodiscard]] LiveKitCapturedFrame ToCapturedFrame(
		const webrtc::VideoFrame &frame) {
	LiveKitCapturedFrame result;
	const webrtc::scoped_refptr<webrtc::I420BufferInterface> buffer
		= frame.video_frame_buffer()->ToI420();
	if (!buffer) {
		return result;
	}
	const int width = buffer->width();
	const int height = buffer->height();
	const int chromaHeight = (height + 1) / 2;
	result.width = static_cast<std::uint32_t>(width);
	result.height = static_cast<std::uint32_t>(height);
	result.timestampUs = frame.timestamp_us();
	result.planes[0] = CopyPlane(buffer->DataY(), buffer->StrideY(), height);
	result.planes[1] = CopyPlane(buffer->DataU(), buffer->StrideU(), chromaHeight);
	result.planes[2] = CopyPlane(buffer->DataV(), buffer->StrideV(), chromaHeight);
	return result;
}

// Builds the frame that a remote video output receives from the planes of the
// library. A frame whose planes are too short for its size is dropped.
[[nodiscard]] std::optional<webrtc::VideoFrame> ToNativeFrame(
		const LiveKitVideoFrame &frame) {
	if (!frame.width || !frame.height) {
		return std::nullopt;
	}
	const int width = static_cast<int>(frame.width);
	const int height = static_cast<int>(frame.height);
	const int chromaWidth = (width + 1) / 2;
	const int chromaHeight = (height + 1) / 2;
	const webrtc::scoped_refptr<webrtc::I420Buffer> buffer
		= webrtc::I420Buffer::Create(width, height);
	const std::array<std::size_t, 3> rowBytes = {
		static_cast<std::size_t>(width),
		static_cast<std::size_t>(chromaWidth),
		static_cast<std::size_t>(chromaWidth),
	};
	const std::array<std::size_t, 3> rowCount = {
		static_cast<std::size_t>(height),
		static_cast<std::size_t>(chromaHeight),
		static_cast<std::size_t>(chromaHeight),
	};
	const std::array<std::uint8_t*, 3> destinations = {
		buffer->MutableDataY(),
		buffer->MutableDataU(),
		buffer->MutableDataV(),
	};
	const std::array<std::size_t, 3> destinationStrides = {
		static_cast<std::size_t>(buffer->StrideY()),
		static_cast<std::size_t>(buffer->StrideU()),
		static_cast<std::size_t>(buffer->StrideV()),
	};
	for (std::size_t plane = 0; plane < 3; ++plane) {
		const LiveKitVideoPlane &source = frame.planes[plane];
		const std::size_t stride = source.stride;
		const std::size_t rows = rowCount[plane];
		const std::size_t bytesPerRow = rowBytes[plane];
		if (stride < bytesPerRow
			|| source.bytes.size() < stride * (rows - 1) + bytesPerRow) {
			return std::nullopt;
		}
		for (std::size_t row = 0; row < rows; ++row) {
			std::memcpy(
				destinations[plane] + row * destinationStrides[plane],
				source.bytes.data() + row * stride,
				bytesPerRow);
		}
	}
	return webrtc::VideoFrame::Builder()
		.set_video_frame_buffer(buffer)
		.set_timestamp_us(frame.timestampUs)
		.build();
}

} // namespace

LiveKitGroupInstance::LiveKitGroupInstance(
		tgcalls::GroupInstanceDescriptor &&descriptor,
		const LiveKitGroupParams &config)
: networkStateUpdated(std::move(descriptor.networkStateUpdated))
, params(config)
, adapter(std::make_unique<LiveKitAdapter>(
		LiveKitFfiLoader::Process(config.dllPath),
		static_cast<LiveKitAdapterDelegate &>(*this),
		[](std::function<void()> task) {
			crl::on_main(std::move(task));
		})) {
	const base::weak_ptr<LiveKitGroupInstance> weak = base::make_weak(this);
	captureSink = std::make_shared<LiveKitCaptureSink>(
		[weak](const webrtc::VideoFrame &frame) {
			crl::on_main(weak, [weak, copy = frame]() {
				LiveKitGroupInstance *that = weak.get();
				if (that) {
					that->handleCapturedFrame(copy);
				}
			});
		});
	adapter->setVideoSink(this);
	if (descriptor.videoCapture) {
		setVideoCapture(descriptor.videoCapture);
	}
	if (descriptor.getVideoSource) {
		setVideoSource(std::move(descriptor.getVideoSource));
	}
}

LiveKitGroupInstance::~LiveKitGroupInstance() {
	stopped = true;
	connected = false;
	releaseCaptures();
	adapter.reset();
}

void LiveKitGroupInstance::stop(std::function<void()> completion) {
	stopped = true;
	connected = false;
	releaseCaptures();
	adapter.reset();
	if (completion) {
		completion();
	}
}

void LiveKitGroupInstance::setConnectionMode(
		tgcalls::GroupConnectionMode,
		bool,
		bool) {
}

void LiveKitGroupInstance::emitJoinPayload(
		std::function<void(tgcalls::GroupJoinPayload const &)> completion) {
	std::set<std::uint32_t> used;
	const std::uint32_t audioSsrc = UniqueSource(used);

	// A presentation has one layer and no SIM group, as the main instance has
	// three layers. The server grants the screen share sources to it.
	const int layerCount = params.presentation ? 1 : 3;
	std::vector<std::uint32_t> layers;
	for (int i = 0; i < layerCount; ++i) {
		layers.push_back(UniqueSource(used));
	}

	QJsonArray groups;
	if (!params.presentation) {
		groups.append(SourceGroup(u"SIM"_q, layers));
	}
	for (const std::uint32_t layer : layers) {
		groups.append(SourceGroup(
			u"FID"_q,
			{ layer, UniqueSource(used) }));
	}

	QJsonObject json;
	json.insert(u"ssrc"_q, static_cast<qint64>(audioSsrc));
	json.insert(u"ssrc-groups"_q, groups);

	tgcalls::GroupJoinPayload payload;
	payload.audioSsrc = audioSsrc;
	payload.json = QJsonDocument(json).toJson(QJsonDocument::Compact).toStdString();
	if (completion) {
		completion(payload);
	}
}

void LiveKitGroupInstance::setJoinResponsePayload(std::string const &payload) {
	if (stopped || !adapter) {
		return;
	}
	const std::optional<LiveKitJoin> join = ParseJoin(
		payload,
		params.groupCallId);
	if (!join) {
		// The payload can carry the bearer token: it is never logged.
		liveKitFailed(kInvalidJoinFailure);
		return;
	}
	adapter->connect(join->url, join->token);
}

void LiveKitGroupInstance::removeSsrcs(std::vector<std::uint32_t>) {
}

void LiveKitGroupInstance::removeIncomingVideoSource(std::uint32_t) {
}

void LiveKitGroupInstance::setIsMuted(bool isMuted) {
	if (muted == isMuted) {
		return;
	}
	muted = isMuted;
	if (!stopped && !params.presentation && adapter) {
		adapter->setMicrophoneMuted(muted);
	}
}

void LiveKitGroupInstance::setIsNoiseSuppressionEnabled(
		bool isNoiseSuppressionEnabled) {
	noiseSuppression = isNoiseSuppressionEnabled;
	if (!stopped && !params.presentation && adapter) {
		// The library takes the value with the microphone source. A change while
		// the microphone is live is kept for the next microphone.
		adapter->setNoiseSuppression(noiseSuppression);
	}
}

void LiveKitGroupInstance::setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface> capture) {
	if (stopped) {
		return;
	}
	if (videoCapture && videoCapture != capture) {
		videoCapture->setOutput(nullptr);
	}
	videoCapture = std::move(capture);
	if (videoCapture) {
		videoCapture->setOutput(captureSink);
	}
	if (!params.presentation && adapter) {
		const bool enabled = (videoCapture != nullptr);
		adapter->setCameraEnabled(enabled);
	}
}

void LiveKitGroupInstance::setVideoSource(
		std::function<
			webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>()
		> getVideoSource) {
	if (stopped || !getVideoSource) {
		return;
	}
	webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source
		= getVideoSource();
	if (videoSource) {
		videoSource->RemoveSink(captureSink.get());
	}
	videoSource = std::move(source);
	if (videoSource) {
		videoSource->AddOrUpdateSink(
			captureSink.get(),
			rtc::VideoSinkWants());
	}
}

void LiveKitGroupInstance::setAudioOutputDevice(std::string) {
	// Device selection is a no-op: Android has no device override for LiveKit.
}

void LiveKitGroupInstance::setAudioInputDevice(std::string) {
	// Device selection is a no-op: Android has no device override for LiveKit.
}

void LiveKitGroupInstance::addExternalAudioSamples(
		std::vector<std::uint8_t> &&) {
}

void LiveKitGroupInstance::addOutgoingVideoOutput(
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	if (stopped) {
		return;
	}
	outgoingOutputs.push_back(std::move(sink));
}

void LiveKitGroupInstance::addIncomingVideoOutput(
		std::string const &endpointId,
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	if (stopped) {
		return;
	}
	incomingOutputs.emplace(endpointId, std::move(sink));
}

void LiveKitGroupInstance::setVolume(std::uint32_t, double) {
	// The remote audio volume has no request in the pinned protocol, so the
	// per-source volume of Android is not applied.
}

void LiveKitGroupInstance::setRequestedVideoChannels(
		std::vector<tgcalls::VideoChannelDescription> &&requestedVideoChannels) {
	if (stopped || !adapter) {
		return;
	}
	for (const tgcalls::VideoChannelDescription &channel
			: requestedVideoChannels) {
		if (!channel.endpointId.empty()) {
			adapter->setRemoteVideoQuality(
				channel.endpointId,
				VideoQualityOf(channel.maxQuality));
		}
	}
}

void LiveKitGroupInstance::getStats(
		std::function<void(tgcalls::GroupInstanceStats)> completion) {
	if (completion) {
		completion(tgcalls::GroupInstanceStats());
	}
}

void LiveKitGroupInstance::internal_addCustomNetworkEvent(bool) {
}

void LiveKitGroupInstance::liveKitConnected() {
	connected = true;
	captureSourcePublished = false;
	reportNetworkState(true);
	// A call that starts muted publishes the microphone muted, so no audio
	// leaves the device until the mute is lifted.
	if (!params.presentation && adapter) {
		adapter->setNoiseSuppression(noiseSuppression);
		adapter->setMicrophoneMuted(muted);
		adapter->publishMicrophone();
	}
}

void LiveKitGroupInstance::liveKitPublished(LiveKitPublishKind) {
}

void LiveKitGroupInstance::liveKitFailed(const std::string &) {
	connected = false;
	captureSourcePublished = false;
	reportNetworkState(false);
}

void LiveKitGroupInstance::liveKitDisconnected() {
	connected = false;
	captureSourcePublished = false;
	reportNetworkState(false);
}

void LiveKitGroupInstance::liveKitVideoFrame(const LiveKitVideoFrame &frame) {
	if (stopped || incomingOutputs.empty()) {
		return;
	}
	std::multimap<std::string, LiveKitFrameOutput>::iterator it
		= incomingOutputs.lower_bound(frame.participantIdentity);
	std::optional<webrtc::VideoFrame> native;
	while (it != incomingOutputs.end()
			&& it->first == frame.participantIdentity) {
		const std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> strong
			= it->second.lock();
		if (!strong) {
			it = incomingOutputs.erase(it);
			continue;
		}
		if (!native) {
			native = ToNativeFrame(frame);
		}
		if (!native) {
			return;
		}
		strong->OnFrame(*native);
		++it;
	}
}

void LiveKitGroupInstance::handleCapturedFrame(
		const webrtc::VideoFrame &frame) {
	if (stopped) {
		return;
	}
	std::vector<LiveKitFrameOutput>::iterator it = outgoingOutputs.begin();
	while (it != outgoingOutputs.end()) {
		const std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> strong
			= it->lock();
		if (strong) {
			strong->OnFrame(frame);
			++it;
		} else {
			it = outgoingOutputs.erase(it);
		}
	}
	if (!adapter || !connected) {
		return;
	}
	const LiveKitPublishKind kind = CaptureKind(params.presentation);
	LiveKitCapturedFrame captured = ToCapturedFrame(frame);
	if (!captured.width) {
		return;
	}
	if (!captureSourcePublished) {
		// The source is created with the size of the first frame. A failed
		// publish is retried with the next frame.
		captureSourcePublished = (kind == LiveKitPublishKind::ScreenShare)
			? adapter->publishScreenShare(captured.width, captured.height)
			: adapter->publishCamera(captured.width, captured.height);
	}
	if (captureSourcePublished) {
		adapter->captureVideoFrame(kind, captured);
	}
}

void LiveKitGroupInstance::releaseCaptures() {
	if (videoCapture) {
		videoCapture->setOutput(nullptr);
		videoCapture.reset();
	}
	if (videoSource) {
		videoSource->RemoveSink(captureSink.get());
		videoSource = nullptr;
	}
}

void LiveKitGroupInstance::reportNetworkState(bool connectedNow) {
	if (stopped || !networkStateUpdated) {
		return;
	}
	tgcalls::GroupNetworkState state;
	state.isConnected = connectedNow;
	networkStateUpdated(state);
}

} // namespace Impulse

#endif // _WIN32
