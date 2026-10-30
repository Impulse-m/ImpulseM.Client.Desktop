// Persisted ImpulseM proxy configuration: a versioned JSON document with the
// two transport toggles, the manual servers, the subscriptions and the
// advanced settings.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Impulse::ProxyState {

inline constexpr int kVersion = 1;

struct Server {
	std::string id;
	std::string name;
	std::string host;
	int port = 0;
	std::string network;
	std::string security;
	std::string outbound;

	// The original share line of a manually added server.
	std::optional<std::string> shareLink;

	bool operator==(const Server &other) const = default;
};

struct SubscriptionMeta {
	std::optional<std::string> title;
	std::int64_t upload = -1;
	std::int64_t download = -1;
	std::int64_t total = -1;
	std::int64_t expire = -1;
	int updateIntervalHours = 24;

	bool operator==(const SubscriptionMeta &other) const = default;
};

struct Subscription {
	std::string id;
	std::string url;
	SubscriptionMeta meta;
	std::int64_t updatedAt = 0;
	std::optional<std::string> lastError;
	int skipped = 0;
	std::vector<Server> servers;

	bool operator==(const Subscription &other) const = default;
};

struct Advanced {
	std::string fragmentMode = "off";
	std::string fragmentPackets = "tlshello";
	std::string fragmentLength = "100-200";
	std::string fragmentInterval = "10-20";
	bool fragmentFromLink = true;
	std::string fingerprint;
	bool muxEnabled = false;
	int muxConcurrency = 8;
	int keepAliveIdle = 0;
	int keepAliveInterval = 0;
	bool tcpFastOpen = false;
	int tcpMaxSeg = 0;
	std::string dnsMode = "system";
	std::string dnsCustom;
	std::string pingMode = "batch";
	int pingPauseMillis = 0;
	int pingTimeoutSeconds = 5;
	std::string pingUrl = "https://www.gstatic.com/generate_204";
	bool pingSkipWhileConnected = true;

	bool operator==(const Advanced &other) const = default;
};

struct State {
	bool enabled = false;

	// Main traffic goes through the HTTP inbound of the core.
	bool useForTransport = true;

	// Calls go through the SOCKS5 inbound of the core.
	bool useForCalls = true;

	std::optional<std::string> selectedId;
	std::vector<Server> manual;
	std::vector<Subscription> subscriptions;
	Advanced advanced;

	bool operator==(const State &other) const = default;
};

// The advanced settings that restart the Xray core, serialized. The ping
// settings are left out, so a change of them keeps the key equal.
[[nodiscard]] std::string CoreKey(const Advanced &advanced);

// Writes one JSON object that starts with "v":1. When both toggles are off the
// written state has useForTransport on, because at least one toggle must be on.
[[nodiscard]] std::string Encode(const State &state);

// Reads the state. Input that is not a JSON object gives the defaults. A
// missing or mistyped field keeps its default, except useForCalls, which
// reads as false when the stored value is missing or not a boolean, as
// Android does. A malformed server or subscription is skipped. A server
// security is read as stored. Advanced settings are sanitized. Both toggles
// off is repaired to useForTransport on.
[[nodiscard]] State Decode(std::string_view text);

// Every server of the state: the manual ones, then the ones of each
// subscription, in the order of the list.
[[nodiscard]] std::vector<Server> AllServers(const State &state);

// Whether the manual list or one of the subscriptions has the server with the
// id.
[[nodiscard]] bool HasServer(const State &state, const std::string &id);

} // namespace Impulse::ProxyState
