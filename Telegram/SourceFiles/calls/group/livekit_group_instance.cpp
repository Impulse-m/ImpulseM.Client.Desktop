#include "calls/group/livekit_group_instance.h"

#ifdef _WIN32

#include "base/random.h"

#include <crl/crl_on_main.h>

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QUrl>

#include <optional>
#include <set>
#include <utility>

namespace Impulse {
namespace {

constexpr auto kInvalidJoinFailure = "Invalid LiveKit group parameters";

struct LiveKitJoin final {
	std::string url;
	std::string token;
};

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

} // namespace

LiveKitGroupInstance::LiveKitGroupInstance(
		tgcalls::GroupInstanceDescriptor &&descriptor,
		const LiveKitGroupParams &config)
: networkStateUpdated(std::move(descriptor.networkStateUpdated))
, params(config)
, adapter(std::make_unique<LiveKitAdapter>(
		config.dllPath,
		*this,
		[](std::function<void()> task) {
			crl::on_main(std::move(task));
		})) {
}

LiveKitGroupInstance::~LiveKitGroupInstance() {
	adapter.reset();
}

void LiveKitGroupInstance::stop(std::function<void()> completion) {
	stopped = true;
	connected = false;
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
	// The adapter publishes the microphone once and has no mute control, so a
	// later mute does not stop audio that is already published.
	if (!stopped && connected && !muted && !params.presentation && adapter) {
		adapter->publishMicrophone();
	}
}

void LiveKitGroupInstance::setIsNoiseSuppressionEnabled(bool) {
}

void LiveKitGroupInstance::setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface>) {
}

void LiveKitGroupInstance::setVideoSource(
		std::function<
			webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>()
		>) {
}

void LiveKitGroupInstance::setAudioOutputDevice(std::string) {
}

void LiveKitGroupInstance::setAudioInputDevice(std::string) {
}

void LiveKitGroupInstance::addExternalAudioSamples(
		std::vector<std::uint8_t> &&) {
}

void LiveKitGroupInstance::addOutgoingVideoOutput(
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>) {
}

void LiveKitGroupInstance::addIncomingVideoOutput(
		std::string const &,
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>) {
}

void LiveKitGroupInstance::setVolume(std::uint32_t, double) {
}

void LiveKitGroupInstance::setRequestedVideoChannels(
		std::vector<tgcalls::VideoChannelDescription> &&) {
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
	reportNetworkState(true);
	if (!params.presentation && !muted && adapter) {
		adapter->publishMicrophone();
	}
}

void LiveKitGroupInstance::liveKitPublished(LiveKitPublishKind) {
}

void LiveKitGroupInstance::liveKitFailed(const std::string &) {
	connected = false;
	reportNetworkState(false);
}

void LiveKitGroupInstance::liveKitDisconnected() {
	connected = false;
	reportNetworkState(false);
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
