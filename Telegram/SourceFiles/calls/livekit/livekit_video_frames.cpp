#include "calls/livekit/livekit_video_frames.h"

#include "api/video/i420_buffer.h"
#include "api/video/video_frame_buffer.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <utility>

namespace Impulse {
namespace {

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

} // namespace

LiveKitCaptureSink::LiveKitCaptureSink(
		std::function<void(const webrtc::VideoFrame &)> deliverFrame)
: deliver(std::move(deliverFrame)) {
}

void LiveKitCaptureSink::OnFrame(const webrtc::VideoFrame &frame) {
	if (deliver) {
		deliver(frame);
	}
}

LiveKitCapturedFrame ToCapturedFrame(const webrtc::VideoFrame &frame) {
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

std::optional<webrtc::VideoFrame> ToNativeFrame(const LiveKitVideoFrame &frame) {
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

} // namespace Impulse
