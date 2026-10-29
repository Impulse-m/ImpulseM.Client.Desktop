// Share links and subscription bodies converted to proxy servers through the
// libXray convertShareLinksToXrayJson call. Qt-free: the caller binds the
// invoker to the libXray worker and blocks on the result off the main thread.
#pragma once

#include "core/impulse_proxy/xray_runtime.h"
#include "mtproto/impulse/core/json.h"
#include "mtproto/impulse/core/proxy_state.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Impulse::Xray {

// The outcome of one convertShareLinksToXrayJson call.
struct LinkConversion final {
	// The VLESS outbounds that converted, in the order libXray returned them.
	std::vector<ProxyState::Server> servers;

	// Outbounds of other protocols and VLESS outbounds without an address.
	int skipped = 0;

	// The libXray failure text, "<method>: <error>". Empty when the call
	// succeeded.
	std::string failure;
};

// Converts one text, a share line, a list of share lines or a decoded
// subscription body, with one libXray call. Blocks until libXray answers.
[[nodiscard]] LinkConversion ConvertLinks(
	const Invoker &invoker,
	std::string_view text);

// Converts a pasted text line by line the way the manual "Add" box does. Only
// lines that start with vless:// (ignoring case, after trimming) are sent, and
// each server keeps the trimmed line as its share link. Lines that fail are
// dropped without an error.
[[nodiscard]] std::vector<ProxyState::Server> ConvertShareLines(
	const Invoker &invoker,
	std::string_view text);

// The error of a subscription body that gave no usable servers, or nullopt
// when the body converted. A refusal that names "no valid outbound" is
// err:noLinks; any other refusal is err:invalidSubscription. With no servers,
// err:noLinks carries the number of skipped outbounds when it is not zero.
[[nodiscard]] std::optional<std::string> SubscriptionBodyError(
	const LinkConversion &conversion);

// The server of one outbound object, or nullopt when the outbound is not VLESS
// or has no usable address and port. The id is the first 24 hex characters of
// the SHA-256 of the outbound JSON, and the name is the trimmed tag, falling
// back to the host.
[[nodiscard]] std::optional<ProxyState::Server> ServerFromOutbound(
	const Json::Value &outbound);

} // namespace Impulse::Xray
