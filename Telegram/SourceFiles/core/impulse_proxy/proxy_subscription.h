// HTTP download of a VLESS subscription. Uses Qt network, so FetchSubscription
// and its callback run on the Qt main thread. The request follows the
// application-wide proxy, which the sandbox sets to the running Xray core, so
// the download is routed like the rest of the traffic.
#pragma once

#include "mtproto/impulse/core/proxy_input.h"

#include <functional>
#include <string>

namespace Impulse::Xray {

// The outcome of one download.
struct SubscriptionFetch final {
	// Empty on success. Otherwise "err:http:<code>", "err:network" or
	// "err:invalidSubscription".
	std::string error;

	// The response body without a leading byte order mark. Empty on failure.
	std::string body;

	// The response headers. Defaults when the response has none.
	ProxyInput::SubscriptionMeta meta;
};

using SubscriptionFetchCallback = std::function<void(SubscriptionFetch result)>;

// Downloads the subscription with a GET that sends User-Agent v2rayNG/1.10.0,
// follows redirects and gives up after 30 seconds. A body over the 4 MiB cap
// is an invalid subscription. The URL is trimmed first. A URL that is not an
// http or https address calls back at once, before FetchSubscription returns.
void FetchSubscription(
	const std::string &url,
	SubscriptionFetchCallback callback);

} // namespace Impulse::Xray
