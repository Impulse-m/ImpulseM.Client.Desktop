#pragma once

#include "mtproto/impulse/core/errors.h"

#include <string>
#include <string_view>

namespace Impulse {

struct RouteRequest {
	std::string method;
	bool mapped = true;
	bool signedIn = false;
	bool takeout = false;
};

struct RouteDecision {
	bool refused = false;
	std::string refusalType;
	CarrierKind carrier = CarrierKind::Update;
	bool forwardTakeout = false;
};

inline constexpr int kClientRefusalCode = 400;
inline constexpr std::string_view kIdentityRefusedType = "CLIENT_IDENTITY_REFUSED";
inline constexpr std::string_view kMethodNotMappedType = "CLIENT_METHOD_NOT_MAPPED";

[[nodiscard]] bool isLoginOnlyMethod(std::string_view method);
[[nodiscard]] bool isIdentitySocketMethod(std::string_view method);
[[nodiscard]] bool isMediaMethod(std::string_view method);
[[nodiscard]] bool isPoolOnlyMethod(std::string_view method);

// A signed-out socket is the identity socket and allows only the identity set;
// a signed-in socket allows everything except the login-only methods. Takeout
// never refuses locally: it is forwarded to the server with the inner method.
[[nodiscard]] RouteDecision routeMethod(const RouteRequest &request);

} // namespace Impulse
