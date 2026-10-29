#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/core/method_classes.h"

#include <string>

namespace {

[[nodiscard]] Impulse::RouteRequest makeRequest(
	const std::string &method,
	bool signedIn) {
	Impulse::RouteRequest request;
	request.method = method;
	request.signedIn = signedIn;
	return request;
}

} // namespace

IMPULSE_TEST(MethodClassesIdentitySocketRefusesSessionMethod) {
	const Impulse::RouteDecision decision = Impulse::routeMethod(
		makeRequest("messages.sendMessage", false));
	IMPULSE_CHECK(decision.refused);
	IMPULSE_CHECK(decision.refusalType == "CLIENT_IDENTITY_REFUSED");
}

IMPULSE_TEST(MethodClassesIdentitySocketAllowsIdentitySet) {
	const char *identityMethods[] = {
		"help.getConfig",
		"langpack.getStrings",
		"auth.sendCode",
		"auth.resendCode",
		"account.getPassword",
		"initConnection",
		"account.deleteAccount",
	};
	for (const char *method : identityMethods) {
		const Impulse::RouteDecision decision = Impulse::routeMethod(makeRequest(method, false));
		IMPULSE_CHECK(!decision.refused);
	}
}

IMPULSE_TEST(MethodClassesPoolOnlyMethodsRouteToPool) {
	const Impulse::RouteDecision identity = Impulse::routeMethod(
		makeRequest("connectionIdentity", false));
	IMPULSE_CHECK(!identity.refused);
	IMPULSE_CHECK(identity.carrier == Impulse::CarrierKind::Pool);
	const Impulse::RouteDecision refresh = Impulse::routeMethod(
		makeRequest("refreshSession", true));
	IMPULSE_CHECK(!refresh.refused);
	IMPULSE_CHECK(refresh.carrier == Impulse::CarrierKind::Pool);
}

IMPULSE_TEST(MethodClassesSessionSocketRefusesLoginOnly) {
	const Impulse::RouteDecision decision = Impulse::routeMethod(makeRequest("auth.signIn", true));
	IMPULSE_CHECK(decision.refused);
	IMPULSE_CHECK(decision.refusalType == "CLIENT_IDENTITY_REFUSED");
}

IMPULSE_TEST(MethodClassesSessionSocketAllowsAnythingElse) {
	const Impulse::RouteDecision decision = Impulse::routeMethod(
		makeRequest("messages.sendMessage", true));
	IMPULSE_CHECK(!decision.refused);
	IMPULSE_CHECK(decision.carrier == Impulse::CarrierKind::Update);
}

IMPULSE_TEST(MethodClassesMediaMethodsRouteToMedia) {
	const char *mediaMethods[] = {
		"upload.saveFilePart",
		"upload.saveBigFilePart",
		"upload.getFile",
		"upload.getWebFile",
	};
	for (const char *method : mediaMethods) {
		const Impulse::RouteDecision decision = Impulse::routeMethod(makeRequest(method, true));
		IMPULSE_CHECK(!decision.refused);
		IMPULSE_CHECK(decision.carrier == Impulse::CarrierKind::Media);
	}
}

IMPULSE_TEST(MethodClassesMediaMethodsRefusedOnIdentitySocket) {
	const Impulse::RouteDecision decision = Impulse::routeMethod(
		makeRequest("upload.getFile", false));
	IMPULSE_CHECK(decision.refused);
	IMPULSE_CHECK(decision.refusalType == "CLIENT_IDENTITY_REFUSED");
}

IMPULSE_TEST(MethodClassesCdnAndHashMethodsRouteToUpdate) {
	const char *updateMethods[] = {
		"upload.getCdnFile",
		"upload.getFileHashes",
	};
	for (const char *method : updateMethods) {
		const Impulse::RouteDecision decision = Impulse::routeMethod(makeRequest(method, true));
		IMPULSE_CHECK(!decision.refused);
		IMPULSE_CHECK(decision.carrier == Impulse::CarrierKind::Update);
	}
}

IMPULSE_TEST(MethodClassesTakeoutIsForwardedNotAnsweredLocally) {
	Impulse::RouteRequest request = makeRequest("messages.sendMessage", true);
	request.takeout = true;
	const Impulse::RouteDecision decision = Impulse::routeMethod(request);
	IMPULSE_CHECK(!decision.refused);
	IMPULSE_CHECK(decision.forwardTakeout);
	IMPULSE_CHECK(decision.carrier == Impulse::CarrierKind::Update);
}

IMPULSE_TEST(MethodClassesUnmappedMethodRefused) {
	Impulse::RouteRequest request = makeRequest("messages.unknownMethod", true);
	request.mapped = false;
	const Impulse::RouteDecision decision = Impulse::routeMethod(request);
	IMPULSE_CHECK(decision.refused);
	IMPULSE_CHECK(decision.refusalType == "CLIENT_METHOD_NOT_MAPPED");
}
