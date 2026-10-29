#include "mtproto/impulse/core/errors.h"
#include "mtproto/impulse/tests/impulse_test.h"

namespace {

[[nodiscard]] Impulse::CarrierFailure sdkFailure(int sdkCode) {
	Impulse::CarrierFailure failure;
	failure.sdkCode = sdkCode;
	return failure;
}

[[nodiscard]] Impulse::CarrierFailure appFailure(
	const std::string &appType,
	int appCode) {
	Impulse::CarrierFailure failure;
	failure.appType = appType;
	failure.appCode = appCode;
	return failure;
}

[[nodiscard]] Impulse::RpcError rpc(int code, const std::string &text) {
	Impulse::RpcError error;
	error.code = code;
	error.text = text;
	return error;
}

} // namespace

IMPULSE_TEST(ErrorsRefusalFirstUpdateAndMedia) {
	const std::optional<Impulse::MappedError> update = Impulse::mapCarrierFailure(sdkFailure(103), Impulse::CarrierKind::Update);
	IMPULSE_CHECK(update.has_value());
	IMPULSE_CHECK(update->error.code == 401);
	IMPULSE_CHECK(update->error.text == "AUTH_KEY_UNREGISTERED");

	const std::optional<Impulse::MappedError> media = Impulse::mapCarrierFailure(sdkFailure(103), Impulse::CarrierKind::Media);
	IMPULSE_CHECK(media.has_value());
	IMPULSE_CHECK(media->error.code == 400);
	IMPULSE_CHECK(media->error.text == "AUTH_TOKEN_INVALID");

	Impulse::CarrierFailure withAppType = appFailure("FLOOD_WAIT_5", 420);
	withAppType.sdkCode = 103;
	const std::optional<Impulse::MappedError> refused = Impulse::mapCarrierFailure(withAppType, Impulse::CarrierKind::Update);
	IMPULSE_CHECK(refused.has_value());
	IMPULSE_CHECK(refused->error.code == 401);
	IMPULSE_CHECK(refused->error.text == "AUTH_KEY_UNREGISTERED");
}

IMPULSE_TEST(ErrorsRefusalTokenExpired) {
	Impulse::CarrierFailure failure = appFailure("SOMETHING", 500);
	failure.sdkCode = 109;
	const std::optional<Impulse::MappedError> mapped = Impulse::mapCarrierFailure(failure, Impulse::CarrierKind::Update);
	IMPULSE_CHECK(mapped.has_value());
	IMPULSE_CHECK(mapped->error.code == 400);
	IMPULSE_CHECK(mapped->error.text == "AUTH_TOKEN_EXPIRED");
}

IMPULSE_TEST(ErrorsTransportCodes) {
	const std::optional<Impulse::MappedError> methodInvalid = Impulse::mapCarrierFailure(sdkFailure(104), Impulse::CarrierKind::Update);
	IMPULSE_CHECK(methodInvalid.has_value());
	IMPULSE_CHECK(methodInvalid->error.code == 400);
	IMPULSE_CHECK(methodInvalid->error.text == "METHOD_INVALID");

	const std::optional<Impulse::MappedError> overloaded = Impulse::mapCarrierFailure(sdkFailure(111), Impulse::CarrierKind::Pool);
	IMPULSE_CHECK(overloaded.has_value());
	IMPULSE_CHECK(overloaded->error.code == 500);
	IMPULSE_CHECK(overloaded->error.text == "INTERNAL_SERVER_ERROR");

	const std::optional<Impulse::MappedError> tooLarge = Impulse::mapCarrierFailure(sdkFailure(106), Impulse::CarrierKind::Media);
	IMPULSE_CHECK(tooLarge.has_value());
	IMPULSE_CHECK(tooLarge->error.code == 500);
	IMPULSE_CHECK(tooLarge->error.text == "INTERNAL_SERVER_ERROR");
}

IMPULSE_TEST(ErrorsNetworkFailuresAreNotRpcErrors) {
	const int networkCodes[] = { 15, 16, 17, 100, 4, 12, 18 };
	for (const int sdkCode : networkCodes) {
		const std::optional<Impulse::MappedError> mapped = Impulse::mapCarrierFailure(sdkFailure(sdkCode), Impulse::CarrierKind::Update);
		IMPULSE_CHECK(!mapped.has_value());
	}
}

IMPULSE_TEST(ErrorsAppTypeMapping) {
	const std::optional<Impulse::MappedError> flood = Impulse::mapCarrierFailure(appFailure("FLOOD_WAIT_5", 420), Impulse::CarrierKind::Update);
	IMPULSE_CHECK(flood.has_value());
	IMPULSE_CHECK(flood->error.code == 420);
	IMPULSE_CHECK(flood->error.text == "FLOOD_WAIT_5");

	const std::optional<Impulse::MappedError> noCode = Impulse::mapCarrierFailure(appFailure("PHONE_INVALID", 0), Impulse::CarrierKind::Update);
	IMPULSE_CHECK(noCode.has_value());
	IMPULSE_CHECK(noCode->error.code == 400);

	Impulse::CarrierFailure migrate = appFailure("migrate", 0);
	migrate.migrateDc = true;
	const std::optional<Impulse::MappedError> moved = Impulse::mapCarrierFailure(migrate, Impulse::CarrierKind::Update);
	IMPULSE_CHECK(moved.has_value());
	IMPULSE_CHECK(moved->error.code == 303);

	const std::optional<Impulse::MappedError> unavailable = Impulse::mapCarrierFailure(appFailure("UNAVAILABLE", 0), Impulse::CarrierKind::Update);
	IMPULSE_CHECK(unavailable.has_value());
	IMPULSE_CHECK(unavailable->error.code == 500);
	IMPULSE_CHECK(unavailable->temporary);

	const std::optional<Impulse::MappedError> internal = Impulse::mapCarrierFailure(appFailure("INTERNAL", 0), Impulse::CarrierKind::Update);
	IMPULSE_CHECK(internal.has_value());
	IMPULSE_CHECK(internal->error.code == 500);
	IMPULSE_CHECK(!internal->temporary);
}

IMPULSE_TEST(ErrorsFloodWaitParsing) {
	Impulse::RpcError wait;
	wait.code = 420;
	wait.text = "FLOOD_WAIT_30";
	IMPULSE_CHECK(Impulse::floodWaitSeconds(wait) == 30);

	wait.text = "FLOOD_PREMIUM_WAIT_7";
	IMPULSE_CHECK(Impulse::floodWaitSeconds(wait) == 7);

	wait.text = "SLOWMODE_WAIT_9";
	IMPULSE_CHECK(Impulse::floodWaitSeconds(wait) == 9);

	wait.text = "FLOOD_WAIT_";
	IMPULSE_CHECK(Impulse::floodWaitSeconds(wait) == -1);

	wait.text = "AUTH_KEY_UNREGISTERED";
	IMPULSE_CHECK(Impulse::floodWaitSeconds(wait) == -1);
}

IMPULSE_TEST(ErrorsAuthClassification) {
	IMPULSE_CHECK(Impulse::classifyAuthError(rpc(401, "AUTH_KEY_UNREGISTERED")) == Impulse::AuthErrorClass::RefreshAndRetry);
	IMPULSE_CHECK(Impulse::classifyAuthError(rpc(401, "SESSION_REVOKED")) == Impulse::AuthErrorClass::ForceLogout);
	IMPULSE_CHECK(Impulse::classifyAuthError(rpc(400, "AUTH_KEY_INVALID")) == Impulse::AuthErrorClass::ForceLogout);
	IMPULSE_CHECK(Impulse::classifyAuthError(rpc(400, "PHONE_INVALID")) == Impulse::AuthErrorClass::None);
}

IMPULSE_TEST(ErrorsRpcRetryServerErrors) {
	Impulse::RetryOptions options;
	options.attempt = 3;

	const Impulse::RetryDecision unavailable = Impulse::decideRpcRetry(rpc(503, "UNAVAILABLE"), options);
	IMPULSE_CHECK(unavailable.action == Impulse::RetryAction::RetryAfter);
	IMPULSE_CHECK(unavailable.delayMs == 3000);

	options.attempt = 50;
	const Impulse::RetryDecision capped = Impulse::decideRpcRetry(rpc(-1, "NEGATIVE"), options);
	IMPULSE_CHECK(capped.action == Impulse::RetryAction::RetryAfter);
	IMPULSE_CHECK(capped.delayMs == 10000);

	options.failOnServerErrors = true;
	const Impulse::RetryDecision failed = Impulse::decideRpcRetry(rpc(500, "INTERNAL"), options);
	IMPULSE_CHECK(failed.action == Impulse::RetryAction::Deliver);
}

IMPULSE_TEST(ErrorsRpcRetryFloodWait) {
	const Impulse::RetryOptions options;

	const Impulse::RetryDecision seconds = Impulse::decideRpcRetry(rpc(420, "FLOOD_WAIT_5"), options);
	IMPULSE_CHECK(seconds.action == Impulse::RetryAction::RetryAfter);
	IMPULSE_CHECK(seconds.delayMs == 5000);

	const Impulse::RetryDecision premium = Impulse::decideRpcRetry(rpc(420, "FLOOD_PREMIUM_WAIT_8"), options);
	IMPULSE_CHECK(premium.delayMs == 8000);

	const Impulse::RetryDecision noNumber = Impulse::decideRpcRetry(rpc(420, "FLOOD_WAIT_"), options);
	IMPULSE_CHECK(noNumber.action == Impulse::RetryAction::RetryAfter);
	IMPULSE_CHECK(noNumber.delayMs == 2000);

	const Impulse::RetryDecision plain = Impulse::decideRpcRetry(rpc(420, "FLOOD"), options);
	IMPULSE_CHECK(plain.delayMs == 2000);

	const Impulse::RetryDecision slowmode = Impulse::decideRpcRetry(rpc(420, "SLOWMODE_WAIT_9"), options);
	IMPULSE_CHECK(slowmode.action == Impulse::RetryAction::Deliver);

	Impulse::RetryOptions noFlood;
	noFlood.failOnServerErrors = true;
	const Impulse::RetryDecision delivered = Impulse::decideRpcRetry(rpc(420, "FLOOD_WAIT_5"), noFlood);
	IMPULSE_CHECK(delivered.action == Impulse::RetryAction::Deliver);

	Impulse::RetryOptions exceptFlood;
	exceptFlood.failOnServerErrors = true;
	exceptFlood.failOnServerErrorsExceptFloodWait = true;
	const Impulse::RetryDecision retried = Impulse::decideRpcRetry(rpc(420, "FLOOD_WAIT_5"), exceptFlood);
	IMPULSE_CHECK(retried.action == Impulse::RetryAction::RetryAfter);
	IMPULSE_CHECK(retried.delayMs == 5000);
}

IMPULSE_TEST(ErrorsRpcRetryAuthAndMessageWait) {
	const Impulse::RetryOptions options;

	const Impulse::RetryDecision logout = Impulse::decideRpcRetry(rpc(401, "SESSION_REVOKED"), options);
	IMPULSE_CHECK(logout.action == Impulse::RetryAction::LogoutAndDeliver);

	const Impulse::RetryDecision refresh = Impulse::decideRpcRetry(rpc(401, "AUTH_KEY_UNREGISTERED"), options);
	IMPULSE_CHECK(refresh.action == Impulse::RetryAction::Deliver);

	const Impulse::RetryDecision msgWait = Impulse::decideRpcRetry(rpc(400, "MSG_WAIT_FAILED"), options);
	IMPULSE_CHECK(msgWait.action == Impulse::RetryAction::RetryAfter);
	IMPULSE_CHECK(msgWait.delayMs == 0);
}

IMPULSE_TEST(ErrorsNetworkRetryDownloadLimit) {
	Impulse::RetryOptions options;
	options.download = true;

	options.attempt = 5;
	const Impulse::RetryDecision belowLimit = Impulse::decideNetworkRetry(options);
	IMPULSE_CHECK(belowLimit.action == Impulse::RetryAction::RetryAfter);
	IMPULSE_CHECK(belowLimit.delayMs == 5000);

	options.attempt = 6;
	const Impulse::RetryDecision limit = Impulse::decideNetworkRetry(options);
	IMPULSE_CHECK(limit.action == Impulse::RetryAction::Deliver);
	IMPULSE_CHECK(limit.replacement.has_value());
	IMPULSE_CHECK(limit.replacement->code == -123);

	options.forceDownload = true;
	const Impulse::RetryDecision forced = Impulse::decideNetworkRetry(options);
	IMPULSE_CHECK(forced.action == Impulse::RetryAction::RetryAfter);

	Impulse::RetryOptions upload;
	upload.attempt = 20;
	const Impulse::RetryDecision capped = Impulse::decideNetworkRetry(upload);
	IMPULSE_CHECK(capped.action == Impulse::RetryAction::RetryAfter);
	IMPULSE_CHECK(capped.delayMs == 10000);
}
