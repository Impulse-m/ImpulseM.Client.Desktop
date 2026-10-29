#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/core/connection_declaration.h"
#include "mtproto/impulse/core/connection_declarer.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

struct ScheduledTask {
	std::int64_t delayMs = 0;
	std::function<void()> task;
};

// Records the initConnection sends and the scheduled tasks. The test answers
// each send by hand, and runs the tasks without any timer.
struct Harness {
	std::vector<Impulse::ConnectionDeclaration> sent;
	std::vector<Impulse::DeclarationSendDone> replies;
	std::vector<ScheduledTask> tasks;
	std::string credential;

	// Runs the tasks that have no delay, until none is left.
	void runNow() {
		for (;;) {
			std::size_t found = tasks.size();
			for (std::size_t index = 0; index < tasks.size(); ++index) {
				if (tasks[index].delayMs == 0) {
					found = index;
					break;
				}
			}
			if (found == tasks.size()) {
				return;
			}
			std::function<void()> task = std::move(tasks[found].task);
			tasks.erase(tasks.begin() + static_cast<std::ptrdiff_t>(found));
			task();
		}
	}

	// Runs every task in order, the delayed retries too.
	void runDelayed() {
		while (!tasks.empty()) {
			std::function<void()> task = std::move(tasks.front().task);
			tasks.erase(tasks.begin());
			task();
			runNow();
		}
	}

	// Answers the pending send. The acknowledgement is the server's reply.
	void answer(bool acknowledged) {
		if (replies.empty()) {
			return;
		}
		Impulse::DeclarationSendDone done = std::move(replies.back());
		replies.pop_back();
		Impulse::DeclarationReply reply;
		reply.acknowledged = acknowledged;
		done(reply);
	}
};

std::unique_ptr<Impulse::ConnectionDeclarer> MakeDeclarer(Harness &harness) {
	return std::make_unique<Impulse::ConnectionDeclarer>(
		[&harness](
				const Impulse::ConnectionDeclaration &declaration,
				Impulse::DeclarationSendDone done) {
			harness.sent.push_back(declaration);
			harness.replies.push_back(std::move(done));
		},
		[&harness](std::int64_t delayMs, std::function<void()> task) {
			harness.tasks.push_back(ScheduledTask{ delayMs, std::move(task) });
		},
		[&harness]() -> std::string {
			return harness.credential;
		});
}

Impulse::ConnectionDeclaration SampleDeclaration() {
	Impulse::ConnectionDeclaration declaration;
	declaration.apiId = 4;
	declaration.deviceModel = "Desktop PC";
	declaration.systemVersion = "Windows 11 x64";
	declaration.appVersion = "7.3.3 x64";
	declaration.systemLangCode = "en-us";
	declaration.langPack = "tdesktop";
	declaration.langCode = "ru";
	return declaration;
}

} // namespace

IMPULSE_TEST(ConnectionDeclarationCarriesEveryField) {
	const Impulse::ConnectionDeclaration sample = SampleDeclaration();
	const Impulse::ConnectionDeclaration clamped
		= Impulse::ClampConnectionDeclaration(sample);
	IMPULSE_CHECK(clamped == sample);
	IMPULSE_CHECK(clamped.apiId == 4);
	IMPULSE_CHECK(clamped.deviceModel == "Desktop PC");
	IMPULSE_CHECK(clamped.systemVersion == "Windows 11 x64");
	IMPULSE_CHECK(clamped.appVersion == "7.3.3 x64");
	IMPULSE_CHECK(clamped.systemLangCode == "en-us");
	IMPULSE_CHECK(clamped.langPack == "tdesktop");
	IMPULSE_CHECK(clamped.langCode == "ru");
}

IMPULSE_TEST(ConnectionDeclarationFitsServerLimits) {
	Impulse::ConnectionDeclaration declaration = SampleDeclaration();
	declaration.deviceModel = std::string(150, 'd');
	declaration.systemLangCode = std::string(40, 's');
	declaration.langCode = "  ru  ";
	const Impulse::ConnectionDeclaration clamped
		= Impulse::ClampConnectionDeclaration(declaration);
	IMPULSE_CHECK(clamped.deviceModel.size() == Impulse::kDeviceModelMaxLength);
	IMPULSE_CHECK(clamped.systemLangCode.size() == Impulse::kSystemLangCodeMaxLength);
	IMPULSE_CHECK(clamped.langCode == "ru");
}

IMPULSE_TEST(ConnectionDeclarationCutsAtWholeCharacters) {
	// A four byte character is two UTF-16 code units, so 49 letters and one
	// such character are 51 units: the character is cut, not split.
	Impulse::ConnectionDeclaration declaration = SampleDeclaration();
	declaration.systemVersion = std::string(49, 'v') + "\xF0\x9F\x98\x80";
	const Impulse::ConnectionDeclaration cut
		= Impulse::ClampConnectionDeclaration(declaration);
	IMPULSE_CHECK(cut.systemVersion == std::string(49, 'v'));

	// 48 letters and the character are exactly 50 units and fit whole.
	declaration.systemVersion = std::string(48, 'v') + "\xF0\x9F\x98\x80";
	const Impulse::ConnectionDeclaration whole
		= Impulse::ClampConnectionDeclaration(declaration);
	IMPULSE_CHECK(whole.systemVersion == std::string(48, 'v') + "\xF0\x9F\x98\x80");
}

IMPULSE_TEST(ConnectionDeclarerSendsOncePerCredential) {
	Harness harness;
	harness.credential = "identity-token";
	const std::unique_ptr<Impulse::ConnectionDeclarer> declarer = MakeDeclarer(harness);

	declarer->setDeclaration(SampleDeclaration());
	harness.runNow();
	IMPULSE_CHECK(harness.sent.size() == 1);
	IMPULSE_CHECK(harness.sent[0] == SampleDeclaration());
	harness.answer(true);
	IMPULSE_CHECK(declarer->isDeclared());

	// The same pair is not declared again, whatever the trigger.
	declarer->credentialChanged();
	harness.runNow();
	IMPULSE_CHECK(harness.sent.size() == 1);
}

IMPULSE_TEST(ConnectionDeclarerSendsAgainAfterCredentialChange) {
	Harness harness;
	harness.credential = "identity-token";
	const std::unique_ptr<Impulse::ConnectionDeclarer> declarer = MakeDeclarer(harness);

	declarer->setDeclaration(SampleDeclaration());
	harness.runNow();
	harness.answer(true);
	IMPULSE_CHECK(harness.sent.size() == 1);

	// The login replaces the identity with the access token.
	harness.credential = "access-token";
	declarer->credentialChanged();
	harness.runNow();
	IMPULSE_CHECK(harness.sent.size() == 2);
	harness.answer(true);
	IMPULSE_CHECK(declarer->isDeclared());

	declarer->credentialChanged();
	harness.runNow();
	IMPULSE_CHECK(harness.sent.size() == 2);
}

IMPULSE_TEST(ConnectionDeclarerInvalidateDeclaresAgain) {
	Harness harness;
	harness.credential = "access-token";
	const std::unique_ptr<Impulse::ConnectionDeclarer> declarer = MakeDeclarer(harness);

	declarer->setDeclaration(SampleDeclaration());
	harness.runNow();
	harness.answer(true);
	IMPULSE_CHECK(harness.sent.size() == 1);

	declarer->invalidate();
	harness.runNow();
	IMPULSE_CHECK(harness.sent.size() == 2);
}

IMPULSE_TEST(ConnectionDeclarerRetriesThreeAttemptsThenStops) {
	Harness harness;
	harness.credential = "identity-token";
	const std::unique_ptr<Impulse::ConnectionDeclarer> declarer = MakeDeclarer(harness);

	declarer->setDeclaration(SampleDeclaration());
	harness.runNow();
	IMPULSE_CHECK(harness.sent.size() == 1);
	harness.answer(false);

	// The retry waits for its delay, and no send happens before it.
	harness.runNow();
	IMPULSE_CHECK(harness.sent.size() == 1);
	harness.runDelayed();
	IMPULSE_CHECK(harness.sent.size() == 2);
	harness.answer(false);

	harness.runDelayed();
	IMPULSE_CHECK(harness.sent.size() == 3);
	harness.answer(false);

	// The third attempt is the last one: nothing is scheduled after it.
	IMPULSE_CHECK(harness.tasks.empty());
	harness.runDelayed();
	IMPULSE_CHECK(harness.sent.size() == 3);
	IMPULSE_CHECK(!declarer->isDeclared());
}

IMPULSE_TEST(ConnectionDeclarerGateHoldsUntilAcknowledged) {
	Harness harness;
	harness.credential = "identity-token";
	const std::unique_ptr<Impulse::ConnectionDeclarer> declarer = MakeDeclarer(harness);

	declarer->setDeclaration(SampleDeclaration());
	bool released = false;
	declarer->awaitDeclared([&released]() {
		released = true;
	});
	IMPULSE_CHECK(!released);

	harness.runNow();
	IMPULSE_CHECK(harness.sent.size() == 1);
	IMPULSE_CHECK(!released);

	harness.answer(true);
	IMPULSE_CHECK(released);
	IMPULSE_CHECK(declarer->isDeclared());

	// Once declared, the gate passes at once.
	bool again = false;
	declarer->awaitDeclared([&again]() {
		again = true;
	});
	IMPULSE_CHECK(again);
}

IMPULSE_TEST(ConnectionDeclarerGateEndsWhenAttemptFails) {
	Harness harness;
	harness.credential = "identity-token";
	const std::unique_ptr<Impulse::ConnectionDeclarer> declarer = MakeDeclarer(harness);

	declarer->setDeclaration(SampleDeclaration());
	bool released = false;
	declarer->awaitDeclared([&released]() {
		released = true;
	});
	harness.runNow();
	IMPULSE_CHECK(!released);

	// The attempt failed, so the wait ends and the call goes on unbound.
	harness.answer(false);
	IMPULSE_CHECK(released);
	IMPULSE_CHECK(!declarer->isDeclared());
}

IMPULSE_TEST(ConnectionDeclarerGateEndsWithoutCredential) {
	Harness harness;
	harness.credential.clear();
	const std::unique_ptr<Impulse::ConnectionDeclarer> declarer = MakeDeclarer(harness);

	declarer->setDeclaration(SampleDeclaration());
	bool released = false;
	declarer->awaitDeclared([&released]() {
		released = true;
	});
	harness.runNow();
	IMPULSE_CHECK(released);
	IMPULSE_CHECK(harness.sent.empty());
}
