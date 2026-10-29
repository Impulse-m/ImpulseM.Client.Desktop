// Recognition of ImpulseM links. Qt-free.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Impulse::Links {

// Host of an absolute prefix such as "https://o.impulsem.net/", lowercase,
// without a port. Returns an empty string when the prefix has no host.
[[nodiscard]] std::string PrefixHost(std::string_view prefix);

// Returns the path that follows a short link host, without the leading
// slash, or nullopt when url is not a short link. Recognised hosts are
// t.me, telegram.me and telegram.dog (forwarded legacy links) and configHost,
// the host of the me_url_prefix sent by help.getConfig. An empty configHost
// recognises no ImpulseM host. An optional http:// or https:// scheme and an
// optional www. prefix are accepted. Host matching ignores case.
[[nodiscard]] std::optional<std::string> ShortLinkPath(
	std::string_view url,
	std::string_view configHost);

// True for internal tg:// URLs, the scheme is matched ignoring case.
[[nodiscard]] bool IsInternalScheme(std::string_view url);

} // namespace Impulse::Links
