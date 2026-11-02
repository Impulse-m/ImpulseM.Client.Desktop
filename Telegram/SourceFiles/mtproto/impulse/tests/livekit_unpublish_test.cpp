/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/livekit/livekit_adapter.h"
#include "calls/livekit/livekit_adapter_interface.h"
#include "calls/livekit/livekit_event_router.h"
#include "calls/livekit/livekit_ffi_codec.h"
#include "calls/livekit/livekit_ffi_loader.h"
#include "mtproto/impulse/core/wire.h"
#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/tests/livekit_fake_room.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ImpulseLiveKitTest;

namespace {

// FfiEvent fields of the publish and the unpublish callbacks.
constexpr std::uint32_t kEventPublishTrack = 9;
constexpr std::uint32_t kEventUnpublishTrack = 10;

void ScriptCameraPublish(
		FakeFfi &fake,
		std::uint64_t sourceHandle,
		std::uint64_t trackHandle) {
	fake.responses.push_back(HandleResponse(kResponseNewVideoSource, sourceHandle));
	fake.responses.push_back(HandleResponse(kResponseCreateVideoTrack, trackHandle));
	fake.responses.push_back(MuteResponse(false));
	fake.responses.push_back(AsyncResponse(kResponsePublishTrack, 0));
}

// Scripts the requests of publishScreenShare. A screen share starts enabled, so
// no mute request is sent.
void ScriptScreenPublish(
		FakeFfi &fake,
		std::uint64_t sourceHandle,
		std::uint64_t trackHandle) {
	fake.responses.push_back(HandleResponse(kResponseNewVideoSource, sourceHandle));
	fake.responses.push_back(HandleResponse(kResponseCreateVideoTrack, trackHandle));
	fake.responses.push_back(AsyncResponse(kResponsePublishTrack, 0));
}

[[nodiscard]] Impulse::LiveKitCapturedFrame SmallFrame() {
	Impulse::LiveKitCapturedFrame frame;
	frame.width = 2;
	frame.height = 2;
	for (Impulse::LiveKitVideoPlane &plane : frame.planes) {
		plane.bytes = { 1, 2, 3, 4 };
		plane.stride = 2;
	}
	return frame;
}

} // namespace

IMPULSE_TEST(LiveKitUnpublishRequestBytes) {
	const Bytes request = Impulse::EncodeLiveKitUnpublishTrackRequest(
		7,
		"TR_camera",
		true,
		99);
	IMPULSE_CHECK(OuterField(request) == kRequestUnpublishTrack);
	const std::optional<Span> body = MessageField(request, kRequestUnpublishTrack);
	IMPULSE_CHECK(body.has_value());
	if (body) {
		IMPULSE_CHECK(VarintField(*body, 1) == 7);
		IMPULSE_CHECK(StringField(*body, 2) == "TR_camera");
		IMPULSE_CHECK(VarintField(*body, 3) == 1);
		IMPULSE_CHECK(VarintField(*body, 4) == 99);
	}

	// Without an async id the field is absent, and stop_on_unpublish is kept.
	const Bytes quiet = Impulse::EncodeLiveKitUnpublishTrackRequest(
		7,
		"TR_camera",
		false,
		std::nullopt);
	const std::optional<Span> quietBody = MessageField(quiet, kRequestUnpublishTrack);
	IMPULSE_CHECK(quietBody.has_value());
	if (quietBody) {
		IMPULSE_CHECK(VarintField(*quietBody, 3) == 0);
		IMPULSE_CHECK(!VarintField(*quietBody, 4).has_value());
	}
}

IMPULSE_TEST(LiveKitPublishCallbackDecodesSid) {
	// OwnedTrackPublication.handle is an FfiOwnedHandle, whose id is field 1.
	Impulse::ProtoWriter handle;
	handle.writeVarintField(1, 77);
	Impulse::ProtoWriter info;
	info.writeStringField(1, "TR_camera");
	Impulse::ProtoWriter publication;
	publication.writeMessageField(1, handle);
	publication.writeMessageField(2, info);
	Impulse::ProtoWriter callback;
	callback.writeVarintField(1, 5);
	callback.writeMessageField(3, publication);
	Impulse::ProtoWriter event;
	event.writeMessageField(kEventPublishTrack, callback);

	const Impulse::LiveKitEvent decoded = Impulse::DecodeLiveKitEvent(event.takeBytes());
	IMPULSE_CHECK(decoded.kind == Impulse::LiveKitEventKind::PublishTrack);
	IMPULSE_CHECK(decoded.asyncId == 5);
	IMPULSE_CHECK(decoded.publicationHandle.value_or(0) == 77);
	IMPULSE_CHECK(decoded.trackSid == "TR_camera");
	IMPULSE_CHECK(decoded.error.empty());

	// A failed publish has neither a publication nor a sid.
	Impulse::ProtoWriter failed;
	failed.writeVarintField(1, 6);
	failed.writeStringField(2, "boom");
	Impulse::ProtoWriter failedEvent;
	failedEvent.writeMessageField(kEventPublishTrack, failed);
	const Impulse::LiveKitEvent rejected = Impulse::DecodeLiveKitEvent(
		failedEvent.takeBytes());
	IMPULSE_CHECK(rejected.kind == Impulse::LiveKitEventKind::PublishTrack);
	IMPULSE_CHECK(rejected.error == "boom");
	IMPULSE_CHECK(!rejected.publicationHandle.has_value());
	IMPULSE_CHECK(rejected.trackSid.empty());
}

IMPULSE_TEST(LiveKitUnpublishCallbackDecodesAsyncIdAndError) {
	Impulse::ProtoWriter callback;
	callback.writeVarintField(1, 8);
	Impulse::ProtoWriter event;
	event.writeMessageField(kEventUnpublishTrack, callback);
	const Impulse::LiveKitEvent decoded = Impulse::DecodeLiveKitEvent(event.takeBytes());
	IMPULSE_CHECK(decoded.kind == Impulse::LiveKitEventKind::UnpublishTrack);
	IMPULSE_CHECK(decoded.asyncId == 8);
	IMPULSE_CHECK(decoded.error.empty());

	Impulse::ProtoWriter failed;
	failed.writeVarintField(1, 9);
	failed.writeStringField(2, "boom");
	Impulse::ProtoWriter failedEvent;
	failedEvent.writeMessageField(kEventUnpublishTrack, failed);
	const Impulse::LiveKitEvent rejected = Impulse::DecodeLiveKitEvent(
		failedEvent.takeBytes());
	IMPULSE_CHECK(rejected.kind == Impulse::LiveKitEventKind::UnpublishTrack);
	IMPULSE_CHECK(rejected.asyncId == 9);
	IMPULSE_CHECK(rejected.error == "boom");
}

IMPULSE_TEST(LiveKitUnpublishAfterConfirmedPublishReleasesAndRepublishes) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);
	IMPULSE_CHECK(delegate.connected == 1);

	ScriptCameraPublish(*fake, 41, 42);
	IMPULSE_CHECK(adapter.publishCamera(640, 360));
	const std::uint64_t publishId = AsyncIdAt(fake->sent, fake->sent.size() - 1);
	IMPULSE_CHECK(publishId != 0);
	fake->router.dispatch(PublishEvent(publishId, 77, "TR_camera"));
	IMPULSE_CHECK(delegate.cameraPublished == 1);

	// The unpublish carries the sid of the publication and stops the track.
	fake->responses.push_back(AsyncResponse(kResponseUnpublishTrack, 0));
	const std::size_t beforeUnpublish = fake->sent.size();
	IMPULSE_CHECK(adapter.unpublishCamera());
	IMPULSE_CHECK(fake->sent.size() == beforeUnpublish + 1);
	if (fake->sent.size() == beforeUnpublish + 1) {
		const Bytes &request = fake->sent.back();
		IMPULSE_CHECK(OuterField(request) == kRequestUnpublishTrack);
		const std::optional<Span> body = MessageField(request, kRequestUnpublishTrack);
		if (body) {
			IMPULSE_CHECK(StringField(*body, 2) == "TR_camera");
			IMPULSE_CHECK(VarintField(*body, 3) == 1);
		}
	}
	const std::uint64_t unpublishId = AsyncIdAt(fake->sent, fake->sent.size() - 1);
	IMPULSE_CHECK(unpublishId != 0);
	fake->router.dispatch(UnpublishEvent(unpublishId, ""));

	// The publication, the track and the source are disposed, once each, and the
	// delegate hears that the track is gone.
	IMPULSE_CHECK(DroppedCount(fake->dropped, 77) == 1);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 42) == 1);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 41) == 1);
	IMPULSE_CHECK(fake->dropped.size() == 3);
	IMPULSE_CHECK(delegate.cameraPublished == 1);
	IMPULSE_CHECK(delegate.unpublished == 1);

	// Without a source the camera takes no frame, and nothing is sent for it.
	const std::size_t beforeCapture = fake->sent.size();
	IMPULSE_CHECK(!adapter.captureVideoFrame(
		Impulse::LiveKitPublishKind::Camera,
		SmallFrame()));
	IMPULSE_CHECK(fake->sent.size() == beforeCapture);

	// A second publish creates a fresh source and track, and publishes again.
	ScriptCameraPublish(*fake, 43, 44);
	IMPULSE_CHECK(adapter.publishCamera(640, 360));
	const std::uint64_t secondPublishId = AsyncIdAt(fake->sent, fake->sent.size() - 1);
	fake->router.dispatch(PublishEvent(secondPublishId, 78, "TR_camera_2"));
	IMPULSE_CHECK(delegate.cameraPublished == 2);
	IMPULSE_CHECK(CountRequests(fake->sent, kRequestNewVideoSource) == 2);
	IMPULSE_CHECK(CountRequests(fake->sent, kRequestCreateVideoTrack) == 2);
	IMPULSE_CHECK(CountRequests(fake->sent, kRequestPublishTrack) == 2);
}

IMPULSE_TEST(LiveKitUnpublishWhilePublishInFlightSendsOnConfirm) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);

	ScriptCameraPublish(*fake, 41, 42);
	IMPULSE_CHECK(adapter.publishCamera(640, 360));
	const std::uint64_t publishId = AsyncIdAt(fake->sent, fake->sent.size() - 1);
	const std::size_t beforeUnpublish = fake->sent.size();

	// The publish is still in flight, so nothing is sent yet.
	fake->responses.push_back(AsyncResponse(kResponseUnpublishTrack, 0));
	IMPULSE_CHECK(adapter.unpublishCamera());
	IMPULSE_CHECK(fake->sent.size() == beforeUnpublish);

	// The confirm sends the unpublish at once, and nothing is reported as published.
	fake->router.dispatch(PublishEvent(publishId, 77, "TR_camera"));
	IMPULSE_CHECK(delegate.cameraPublished == 0);
	IMPULSE_CHECK(fake->sent.size() == beforeUnpublish + 1);
	if (fake->sent.size() == beforeUnpublish + 1) {
		const std::optional<Span> body = MessageField(
			fake->sent.back(),
			kRequestUnpublishTrack);
		IMPULSE_CHECK(OuterField(fake->sent.back()) == kRequestUnpublishTrack);
		if (body) {
			IMPULSE_CHECK(StringField(*body, 2) == "TR_camera");
		}
	}
	const std::uint64_t unpublishId = AsyncIdAt(fake->sent, fake->sent.size() - 1);
	fake->router.dispatch(UnpublishEvent(unpublishId, ""));
	IMPULSE_CHECK(DroppedCount(fake->dropped, 77) == 1);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 42) == 1);
	IMPULSE_CHECK(delegate.cameraPublished == 0);
	IMPULSE_CHECK(delegate.unpublished == 1);
	IMPULSE_CHECK(delegate.failed == 0);
}

IMPULSE_TEST(LiveKitUnpublishWithNothingPublishedReturnsFalse) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);

	// Before the room connects nothing is published, and no request is sent.
	IMPULSE_CHECK(!adapter.unpublishCamera());
	IMPULSE_CHECK(!adapter.unpublishScreenShare());
	IMPULSE_CHECK(fake->sent.empty());

	ConnectRoom(*fake, adapter);
	const std::size_t sentBefore = fake->sent.size();
	IMPULSE_CHECK(!adapter.unpublishCamera());
	IMPULSE_CHECK(!adapter.unpublishScreenShare());
	IMPULSE_CHECK(fake->sent.size() == sentBefore);
	IMPULSE_CHECK(delegate.failed == 0);
	IMPULSE_CHECK(adapter.connected());
}

IMPULSE_TEST(LiveKitUnpublishRejectedByLibraryFailsRoom) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);

	ScriptCameraPublish(*fake, 41, 42);
	IMPULSE_CHECK(adapter.publishCamera(640, 360));
	fake->router.dispatch(PublishEvent(
		AsyncIdAt(fake->sent, fake->sent.size() - 1),
		77,
		"TR_camera"));
	IMPULSE_CHECK(delegate.cameraPublished == 1);

	// The library refuses the unpublish request. The track may still be sent, so
	// the room is failed rather than kept with a frozen track.
	IMPULSE_CHECK(!adapter.unpublishCamera());
	IMPULSE_CHECK(CountRequests(fake->sent, kRequestUnpublishTrack) == 1);
	IMPULSE_CHECK(delegate.failed == 1);
	IMPULSE_CHECK(!adapter.connected());
	IMPULSE_CHECK(DroppedCount(fake->dropped, 77) == 1);
	IMPULSE_CHECK(delegate.unpublished == 0);
}

IMPULSE_TEST(LiveKitUnpublishFailedCallbackFailsRoom) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);

	ScriptCameraPublish(*fake, 41, 42);
	IMPULSE_CHECK(adapter.publishCamera(640, 360));
	fake->router.dispatch(PublishEvent(
		AsyncIdAt(fake->sent, fake->sent.size() - 1),
		77,
		"TR_camera"));

	// The library accepts the request and then fails its callback. The publication
	// is still owned by the room, so the room releases it when it fails.
	fake->responses.push_back(AsyncResponse(kResponseUnpublishTrack, 0));
	IMPULSE_CHECK(adapter.unpublishCamera());
	const std::uint64_t failedId = AsyncIdAt(fake->sent, fake->sent.size() - 1);
	fake->router.dispatch(UnpublishEvent(failedId, "boom"));
	IMPULSE_CHECK(delegate.failed == 1);
	IMPULSE_CHECK(!adapter.connected());
	IMPULSE_CHECK(delegate.unpublished == 0);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 77) == 1);
}

IMPULSE_TEST(LiveKitUnpublishRejectedAtConfirmFailsRoom) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);

	ScriptCameraPublish(*fake, 41, 42);
	IMPULSE_CHECK(adapter.publishCamera(640, 360));
	const std::uint64_t publishId = AsyncIdAt(fake->sent, fake->sent.size() - 1);

	// The unpublish is queued behind the publish in flight. No response is
	// scripted for it, so the library rejects the request sent on the confirm.
	IMPULSE_CHECK(adapter.unpublishCamera());
	fake->router.dispatch(PublishEvent(publishId, 77, "TR_camera"));
	IMPULSE_CHECK(CountRequests(fake->sent, kRequestUnpublishTrack) == 1);

	// The track must not stay published with no owner: the room fails, the failed
	// room leaves the room, and the publication is released by the failure.
	IMPULSE_CHECK(CountRequests(fake->sent, kRequestDisconnect) == 1);
	IMPULSE_CHECK(delegate.cameraPublished == 0);
	IMPULSE_CHECK(delegate.failed == 1);
	IMPULSE_CHECK(!adapter.connected());
	IMPULSE_CHECK(DroppedCount(fake->dropped, 77) == 1);
}

IMPULSE_TEST(LiveKitRepublishCancelsQueuedUnpublish) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);

	ScriptCameraPublish(*fake, 41, 42);
	IMPULSE_CHECK(adapter.publishCamera(640, 360));
	const std::uint64_t publishId = AsyncIdAt(fake->sent, fake->sent.size() - 1);
	const std::size_t beforeQueue = fake->sent.size();

	// The camera goes to a screen share and back while its publish is in flight.
	// The unpublish is queued, and the camera publish that follows cancels it.
	IMPULSE_CHECK(adapter.unpublishCamera());
	IMPULSE_CHECK(adapter.publishCamera(640, 360));
	IMPULSE_CHECK(fake->sent.size() == beforeQueue);

	// The confirm reports the publish as published, and no unpublish is sent.
	fake->router.dispatch(PublishEvent(publishId, 77, "TR_camera"));
	IMPULSE_CHECK(delegate.cameraPublished == 1);
	IMPULSE_CHECK(CountRequests(fake->sent, kRequestUnpublishTrack) == 0);
	IMPULSE_CHECK(fake->sent.size() == beforeQueue);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 77) == 0);
	IMPULSE_CHECK(delegate.unpublished == 0);
	IMPULSE_CHECK(delegate.failed == 0);
	IMPULSE_CHECK(adapter.connected());
}

IMPULSE_TEST(LiveKitUnpublishScreenShareReleasesOnlyItsKind) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);

	ScriptCameraPublish(*fake, 41, 42);
	IMPULSE_CHECK(adapter.publishCamera(640, 360));
	fake->router.dispatch(PublishEvent(
		AsyncIdAt(fake->sent, fake->sent.size() - 1),
		77,
		"TR_camera"));

	ScriptScreenPublish(*fake, 51, 52);
	IMPULSE_CHECK(adapter.publishScreenShare(1280, 720));
	fake->router.dispatch(PublishEvent(
		AsyncIdAt(fake->sent, fake->sent.size() - 1),
		88,
		"TR_screen"));
	IMPULSE_CHECK(delegate.screenPublished == 1);

	fake->responses.push_back(AsyncResponse(kResponseUnpublishTrack, 0));
	IMPULSE_CHECK(adapter.unpublishScreenShare());
	const std::optional<Span> body = MessageField(
		fake->sent.back(),
		kRequestUnpublishTrack);
	if (body) {
		IMPULSE_CHECK(StringField(*body, 2) == "TR_screen");
	}
	fake->router.dispatch(UnpublishEvent(
		AsyncIdAt(fake->sent, fake->sent.size() - 1),
		""));
	IMPULSE_CHECK(DroppedCount(fake->dropped, 88) == 1);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 52) == 1);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 51) == 1);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 77) == 0);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 42) == 0);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 41) == 0);

	// The camera is still published, so its unpublish is still sent.
	fake->responses.push_back(AsyncResponse(kResponseUnpublishTrack, 0));
	IMPULSE_CHECK(adapter.unpublishCamera());

	// The screen share publishes again with a fresh source and track.
	ScriptScreenPublish(*fake, 53, 54);
	IMPULSE_CHECK(adapter.publishScreenShare(1280, 720));
	fake->router.dispatch(PublishEvent(
		AsyncIdAt(fake->sent, fake->sent.size() - 1),
		89,
		"TR_screen_2"));
	IMPULSE_CHECK(delegate.screenPublished == 2);
	IMPULSE_CHECK(delegate.failed == 0);
}

IMPULSE_TEST(LiveKitTeardownAfterUnpublishReleasesPublicationOnce) {
	const std::shared_ptr<FakeFfi> fake = std::make_shared<FakeFfi>();
	RecordingDelegate delegate;
	Impulse::LiveKitAdapter adapter(fake, delegate, RunNow);
	ConnectRoom(*fake, adapter);

	ScriptCameraPublish(*fake, 41, 42);
	IMPULSE_CHECK(adapter.publishCamera(640, 360));
	fake->router.dispatch(PublishEvent(
		AsyncIdAt(fake->sent, fake->sent.size() - 1),
		77,
		"TR_camera"));
	fake->responses.push_back(AsyncResponse(kResponseUnpublishTrack, 0));
	IMPULSE_CHECK(adapter.unpublishCamera());
	fake->router.dispatch(UnpublishEvent(
		AsyncIdAt(fake->sent, fake->sent.size() - 1),
		""));

	// The leave releases the platform audio, the participant and the room. The
	// publication was released with the unpublish, so it is not released again.
	fake->responses.push_back(AsyncResponse(kResponseDisconnect, 0));
	adapter.disconnect();
	fake->router.dispatch(DisconnectEvent(AsyncIdAt(fake->sent, fake->sent.size() - 1)));
	IMPULSE_CHECK(delegate.disconnected == 1);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 77) == 1);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 42) == 1);
	IMPULSE_CHECK(DroppedCount(fake->dropped, 41) == 1);
	IMPULSE_CHECK(fake->dropped.size() == 6);
}
