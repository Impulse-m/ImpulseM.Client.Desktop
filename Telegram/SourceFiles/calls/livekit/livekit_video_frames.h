// Frame helpers of the LiveKit calls, shared by the 1:1 call instance and the
// group call instance. They copy the frames between the webrtc pipeline and the
// LiveKit adapter, so the adapter gets bytes it owns and a webrtc frame can be
// released as soon as it is delivered.
#pragma once

#include "calls/livekit/livekit_adapter_interface.h"

#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>

#include <functional>
#include <optional>

namespace Impulse {

// Receives the frames of a capture on the capture thread. The frame is handed to
// the deliver function, which posts it to the owner thread.
class LiveKitCaptureSink final : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
	explicit LiveKitCaptureSink(
		std::function<void(const webrtc::VideoFrame &)> deliverFrame);

	void OnFrame(const webrtc::VideoFrame &frame) override;

private:
	std::function<void(const webrtc::VideoFrame &)> deliver;

};

// Copies the I420 planes of a webrtc frame into a captured frame of the adapter.
// The result has no size when the buffer has no I420 form.
[[nodiscard]] LiveKitCapturedFrame ToCapturedFrame(
	const webrtc::VideoFrame &frame);

// Builds the webrtc frame of a decoded remote video frame. A frame whose planes
// are too short for its size is dropped, and so is one without a size.
[[nodiscard]] std::optional<webrtc::VideoFrame> ToNativeFrame(
	const LiveKitVideoFrame &frame);

} // namespace Impulse
