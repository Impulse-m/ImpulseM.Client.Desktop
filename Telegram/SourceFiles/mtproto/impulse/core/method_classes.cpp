#include "mtproto/impulse/core/method_classes.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace Impulse {
namespace {

constexpr std::array<std::string_view, 15> kLoginOnlyMethods = {
	"auth.sendCode",
	"auth.cancelCode",
	"auth.signIn",
	"auth.signUp",
	"auth.requestFirebaseSms",
	"auth.resetLoginEmail",
	"auth.reportMissingCode",
	"auth.checkPaidAuth",
	"auth.importAuthorization",
	"auth.importBotAuthorization",
	"auth.importWebTokenAuthorization",
	"auth.exportLoginToken",
	"auth.importLoginToken",
	"auth.initPasskeyLogin",
	"auth.finishPasskeyLogin",
};

constexpr std::array<std::string_view, 3> kDualUseMethods = {
	"auth.resendCode",
	"account.sendVerifyEmailCode",
	"account.verifyEmail",
};

constexpr std::array<std::string_view, 5> kPendingMethods = {
	"auth.checkPassword",
	"auth.requestPasswordRecovery",
	"auth.recoverPassword",
	"auth.checkRecoveryPassword",
	"account.getPassword",
};

constexpr std::array<std::string_view, 11> kPublicMethods = {
	"help.getConfig",
	"help.getAppConfig",
	"help.getNearestDc",
	"help.getCountriesList",
	"help.getDeepLinkInfo",
	"help.saveAppLog",
	"langpack.getLangPack",
	"langpack.getStrings",
	"langpack.getDifference",
	"langpack.getLanguage",
	"langpack.getLanguages",
};

constexpr std::array<std::string_view, 4> kIdentityExtraMethods = {
	"initConnection",
	"connectionIdentity",
	"refreshSession",
	"account.deleteAccount",
};

constexpr std::array<std::string_view, 4> kMediaMethods = {
	"upload.saveFilePart",
	"upload.saveBigFilePart",
	"upload.getFile",
	"upload.getWebFile",
};

constexpr std::array<std::string_view, 2> kPoolOnlyMethods = {
	"connectionIdentity",
	"refreshSession",
};

template <std::size_t Size>
[[nodiscard]] bool containsMethod(
	const std::array<std::string_view, Size> &list,
	std::string_view method) {
	return std::find(list.begin(), list.end(), method) != list.end();
}

} // namespace

bool isLoginOnlyMethod(std::string_view method) {
	return containsMethod(kLoginOnlyMethods, method);
}

bool isIdentitySocketMethod(std::string_view method) {
	return containsMethod(kLoginOnlyMethods, method)
		|| containsMethod(kDualUseMethods, method)
		|| containsMethod(kPendingMethods, method)
		|| containsMethod(kPublicMethods, method)
		|| containsMethod(kIdentityExtraMethods, method);
}

bool isMediaMethod(std::string_view method) {
	return containsMethod(kMediaMethods, method);
}

bool isPoolOnlyMethod(std::string_view method) {
	return containsMethod(kPoolOnlyMethods, method);
}

RouteDecision routeMethod(const RouteRequest &request) {
	RouteDecision decision;
	const std::string_view method = request.method;
	if (!request.mapped) {
		decision.refused = true;
		decision.refusalType = std::string(kMethodNotMappedType);
		return decision;
	}
	const bool allowed = request.signedIn
		? !isLoginOnlyMethod(method)
		: isIdentitySocketMethod(method);
	if (!allowed) {
		decision.refused = true;
		decision.refusalType = std::string(kIdentityRefusedType);
		return decision;
	}
	if (isPoolOnlyMethod(method)) {
		decision.carrier = CarrierKind::Pool;
	} else if (isMediaMethod(method)) {
		decision.carrier = CarrierKind::Media;
	} else {
		decision.carrier = CarrierKind::Update;
	}
	decision.forwardTakeout = request.takeout;
	return decision;
}

} // namespace Impulse
