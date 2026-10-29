#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/core/rpc_response.h"

#include <cstdint>
#include <string>

IMPULSE_TEST(RpcResponseOuterMsgIdLowBitsAreOneZero) {
	const std::uint64_t id = Impulse::OuterMsgIdFromServerTime(1000);
	IMPULSE_CHECK((id & std::uint64_t{3}) == std::uint64_t{1});
	IMPULSE_CHECK(id == ((std::uint64_t{1} << 32) | std::uint64_t{1}));
}

IMPULSE_TEST(RpcResponseOuterMsgIdCarriesServerSeconds) {
	const std::uint64_t id = Impulse::OuterMsgIdFromServerTime(1700000000123);
	IMPULSE_CHECK((id >> 32) == std::uint64_t{1700000000});
	IMPULSE_CHECK((id & std::uint64_t{3}) == std::uint64_t{1});
}

IMPULSE_TEST(RpcResponseOuterMsgIdFractionValue) {
	// 123 * 2^32 / 1000 = 528280977.408, floored to 528280977 (low bits 01).
	const std::uint64_t id = Impulse::OuterMsgIdFromServerTime(1700000000123);
	IMPULSE_CHECK(id == ((std::uint64_t{1700000000} << 32)
		+ std::uint64_t{528280977}));
}

IMPULSE_TEST(RpcResponseErrorText) {
	IMPULSE_CHECK(Impulse::RpcErrorText("FLOOD_WAIT_5", "Too many requests")
		== "FLOOD_WAIT_5: Too many requests");
	IMPULSE_CHECK(Impulse::RpcErrorText("AUTH_TOKEN_INVALID", "")
		== "AUTH_TOKEN_INVALID: ");
}

IMPULSE_TEST(RpcResponseClientErrorIsFourHundredClass) {
	const Impulse::RpcErrorPayload error = Impulse::MakeClientError(
		"CLIENT_TRANSCODE_FAILED",
		"Encode failed");
	IMPULSE_CHECK(error.code == 400);
	IMPULSE_CHECK(error.type == "CLIENT_TRANSCODE_FAILED");
	IMPULSE_CHECK(error.description == "Encode failed");
	IMPULSE_CHECK(Impulse::RpcErrorText(error.type, error.description)
		== "CLIENT_TRANSCODE_FAILED: Encode failed");
	IMPULSE_CHECK(!Impulse::IsRetriedForever(error.code));
}

IMPULSE_TEST(RpcResponseClientErrorAddsPrefix) {
	const Impulse::RpcErrorPayload error = Impulse::MakeClientError(
		"CLIENT_METHOD_NOT_MAPPED",
		"x");
	IMPULSE_CHECK(error.type == "CLIENT_METHOD_NOT_MAPPED");
	const Impulse::RpcErrorPayload bare = Impulse::MakeClientError(
		"IDENTITY_REFUSED",
		"x");
	IMPULSE_CHECK(bare.type == "CLIENT_IDENTITY_REFUSED");
	IMPULSE_CHECK(bare.code == 400);
}

IMPULSE_TEST(RpcResponseRetriedForeverCodes) {
	IMPULSE_CHECK(Impulse::IsRetriedForever(500));
	IMPULSE_CHECK(Impulse::IsRetriedForever(503));
	IMPULSE_CHECK(Impulse::IsRetriedForever(-1));
	IMPULSE_CHECK(!Impulse::IsRetriedForever(499));
	IMPULSE_CHECK(!Impulse::IsRetriedForever(400));
	IMPULSE_CHECK(!Impulse::IsRetriedForever(0));
}
