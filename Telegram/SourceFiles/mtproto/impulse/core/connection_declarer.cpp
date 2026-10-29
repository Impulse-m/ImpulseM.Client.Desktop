#include "mtproto/impulse/core/connection_declarer.h"

#include <utility>

namespace Impulse {

ConnectionDeclarer::ConnectionDeclarer(
		DeclarationSender sendCall,
		DeclarationScheduler scheduleCall,
		DeclarationCredential credentialCall)
: sender(std::move(sendCall))
, schedule(std::move(scheduleCall))
, credential(std::move(credentialCall)) {
}

void ConnectionDeclarer::setDeclaration(ConnectionDeclaration value) {
	declaration = std::move(value);
	trigger();
}

void ConnectionDeclarer::credentialChanged() {
	trigger();
}

void ConnectionDeclarer::invalidate() {
	sentDeclaration.reset();
	sentCredential.clear();
	trigger();
}

void ConnectionDeclarer::awaitDeclared(std::function<void()> ready) {
	if (isDeclared()) {
		ready();
		return;
	}
	waiters.push_back(std::move(ready));
	if (!running) {
		trigger();
	}
}

bool ConnectionDeclarer::isDeclared() const {
	return isDeclaredFor(credential());
}

bool ConnectionDeclarer::isDeclaredFor(const std::string &current) const {
	return !current.empty()
		&& declaration.has_value()
		&& sentDeclaration.has_value()
		&& current == sentCredential
		&& *declaration == *sentDeclaration;
}

void ConnectionDeclarer::trigger() {
	// A trigger during a run starts one more run when that one ends. The
	// attempt reads the credential and the declaration as it runs, so the
	// trigger may be superseded by the attempt that is already in flight.
	if (running) {
		rerun = true;
		return;
	}
	running = true;
	schedule(0, [this]() {
		attempt(1);
	});
}

void ConnectionDeclarer::attempt(int attemptNumber) {
	const std::string current = credential();
	if (current.empty() || !declaration.has_value() || isDeclaredFor(current)) {
		endRun();
		return;
	}
	const ConnectionDeclaration sending = *declaration;
	sender(sending, [this, attemptNumber, current, sending](
			DeclarationReply reply) {
		onReply(attemptNumber, current, sending, reply);
	});
}

void ConnectionDeclarer::onReply(
		int attemptNumber,
		const std::string &current,
		const ConnectionDeclaration &sending,
		DeclarationReply reply) {
	if (reply.acknowledged) {
		sentCredential = current;
		sentDeclaration = sending;
		endRun();
		return;
	}
	if (attemptNumber < kDeclarationMaxAttempts) {
		// The retry is scheduled, and the run ends now: the waiters go on as
		// Android's awaitDeclared does when a retry is scheduled.
		schedule(kDeclarationRetryMs, [this, attemptNumber]() {
			retry(attemptNumber + 1);
		});
	}
	endRun();
}

void ConnectionDeclarer::retry(int attemptNumber) {
	// A run that started since the retry was scheduled covers the retry.
	if (running) {
		return;
	}
	running = true;
	attempt(attemptNumber);
}

void ConnectionDeclarer::endRun() {
	running = false;
	const bool restart = rerun;
	rerun = false;

	std::vector<std::function<void()>> ready = std::move(waiters);
	waiters.clear();
	for (const std::function<void()> &waiter : ready) {
		waiter();
	}

	if (restart && !running) {
		running = true;
		schedule(0, [this]() {
			attempt(1);
		});
	}
}

} // namespace Impulse
