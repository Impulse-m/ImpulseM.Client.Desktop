// Declares the connection to the server with initConnection, as Android's
// ConnectionDeclarer does. The declaration is keyed by the pair (credential,
// declaration): it is sent once per pair, again when the credential changes,
// and at most kDeclarationMaxAttempts times for one attempt. Qt-free: the
// send, the retry timer and the credential are injected, and every call runs
// on the owner thread.
#pragma once

#include "mtproto/impulse/core/connection_declaration.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Impulse {

// The attempts of one run of the declarer, and the wait before a retry.
inline constexpr int kDeclarationMaxAttempts = 3;
inline constexpr std::int64_t kDeclarationRetryMs = 1000;

struct DeclarationReply {
	bool acknowledged = false;
};

using DeclarationSendDone = std::function<void(DeclarationReply reply)>;

// Sends initConnection with the declaration over the update socket, which
// presents the credential. The done callback runs once, on the owner thread.
using DeclarationSender = std::function<void(
	const ConnectionDeclaration &declaration,
	DeclarationSendDone done)>;

// Runs task after delayMs on the owner thread.
using DeclarationScheduler = std::function<void(
	std::int64_t delayMs,
	std::function<void()> task)>;

// The credential the update socket presents now: the access token with a
// session, the connection identity without one. Empty when there is none.
using DeclarationCredential = std::function<std::string()>;

class ConnectionDeclarer final {
public:
	ConnectionDeclarer(
		DeclarationSender sender,
		DeclarationScheduler schedule,
		DeclarationCredential credential);
	ConnectionDeclarer(const ConnectionDeclarer &other) = delete;
	ConnectionDeclarer &operator=(const ConnectionDeclarer &other) = delete;

	// Sets the declaration and declares it when the pair is new.
	void setDeclaration(ConnectionDeclaration declaration);

	// The socket presents another credential: a new identity, a login, a
	// refresh or a sign-out.
	void credentialChanged();

	// The server lost the declaration: the sent pair is forgotten and the
	// declaration is sent again.
	void invalidate();

	// Runs ready once the current pair is acknowledged, at once when it
	// already is. The wait also ends when the run ends without an
	// acknowledgement (no credential, a give-up or a retry), as Android's
	// awaitDeclared does, so the caller goes on and the server answers.
	void awaitDeclared(std::function<void()> ready);

	[[nodiscard]] bool isDeclared() const;

private:
	[[nodiscard]] bool isDeclaredFor(const std::string &current) const;
	void trigger();
	void attempt(int attemptNumber);
	void onReply(
		int attemptNumber,
		const std::string &current,
		const ConnectionDeclaration &sending,
		DeclarationReply reply);
	void retry(int attemptNumber);
	void endRun();

	DeclarationSender sender;
	DeclarationScheduler schedule;
	DeclarationCredential credential;

	std::optional<ConnectionDeclaration> declaration;
	std::optional<ConnectionDeclaration> sentDeclaration;
	std::string sentCredential;
	std::vector<std::function<void()>> waiters;
	bool running = false;
	bool rerun = false;
};

} // namespace Impulse
