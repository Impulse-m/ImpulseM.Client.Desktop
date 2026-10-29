// Rules for what the user pastes into the proxy "Add" box: share links or a
// subscription URL, the subscription id, the body cap and the response headers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Impulse::ProxyInput {

inline constexpr std::size_t kMaxSubscriptionBodyBytes = 4 * 1024 * 1024;
inline constexpr int kDefaultUpdateIntervalHours = 24;
inline constexpr std::size_t kSubscriptionIdLength = 24;

inline constexpr std::string_view kErrorHttp = "err:http";
inline constexpr std::string_view kErrorNetwork = "err:network";
inline constexpr std::string_view kErrorInvalidSubscription = "err:invalidSubscription";
inline constexpr std::string_view kErrorNoLinks = "err:noLinks";

enum class InputKind {
	Links,
	Subscription,
	Unknown,
};

// Panel headers of a subscription response. Absent or unparsable values are
// reported as empty title or -1 for the byte counters and expiry, and as the
// default interval for the update period.
struct SubscriptionMeta {
	std::optional<std::string> title;
	std::int64_t upload = -1;
	std::int64_t download = -1;
	std::int64_t total = -1;
	std::int64_t expire = -1;
	int updateIntervalHours = kDefaultUpdateIntervalHours;
};

// Strips ASCII whitespace and control characters at both ends, the same set
// as Java String.trim().
[[nodiscard]] std::string_view Trim(std::string_view text);

// A subscription when the trimmed text starts with http:// or https:// and has
// no whitespace inside. Links when any line of the text starts with vless://
// (case-insensitive). Otherwise unknown.
[[nodiscard]] InputKind Detect(std::string_view text);

// Stable per URL: the first 24 hex characters of the SHA-256 of the trimmed
// URL.
[[nodiscard]] std::string SubscriptionId(std::string_view url);

// Removes a leading UTF-8 byte order mark.
[[nodiscard]] std::string_view StripBom(std::string_view text);

// Whether a response body fits the cap. A body of exactly the cap is accepted.
[[nodiscard]] bool BodyWithinCap(std::size_t bytes);

// "err:http:<code>" for a failed HTTP status.
[[nodiscard]] std::string HttpError(int code);

// "err:noLinks" when nothing was skipped, "err:noLinks:<skipped>" otherwise.
[[nodiscard]] std::string NoLinksError(int skipped);

// Parses profile-title (with an optional "base64:" prefix),
// subscription-userinfo (upload, download, total, expire) and
// profile-update-interval (hours, at least 1, default 24). Never fails: a
// missing or malformed value is reported as described on SubscriptionMeta.
[[nodiscard]] SubscriptionMeta ParseMeta(
	std::optional<std::string_view> profileTitle,
	std::optional<std::string_view> userInfo,
	std::optional<std::string_view> updateInterval);

} // namespace Impulse::ProxyInput
