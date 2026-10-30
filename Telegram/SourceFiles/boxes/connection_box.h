/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/object_ptr.h"
#include "base/weak_ptr.h"
#include "core/core_settings_proxy.h"
#include "core/impulse_proxy/proxy_ping.h"
#include "core/impulse_proxy/proxy_service.h"
#include "mtproto/mtproto_proxy_data.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Ui {
class Show;
class BoxContent;
class InputField;
class PortInput;
class PasswordInput;
class Checkbox;
template <typename Enum>
class RadioenumGroup;
template <typename Enum>
class Radioenum;
} // namespace Ui

namespace Main {
class Account;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

class ProxiesBoxController : public base::has_weak_ptr {
public:
	using ProxyData = MTP::ProxyData;
	using Type = ProxyData::Type;

	explicit ProxiesBoxController(not_null<Main::Account*> account);

	static void ShowApplyConfirmation(
		Window::SessionController *controller,
		Type type,
		const QMap<QString, QString> &fields);

	[[nodiscard]] static ProxyData ProxyFromLink(const QString &link);

	static object_ptr<Ui::BoxContent> CreateOwningBox(
		not_null<Main::Account*> account,
		const QString &highlightId = QString());
	static void Show(
		not_null<Window::SessionController*> controller,
		const QString &highlightId = QString());
	object_ptr<Ui::BoxContent> create(const QString &highlightId = QString());

	enum class ItemState {
		Connecting,
		Online,
		Checking,
		NotTested,
		Available,
		Unavailable,
	};
	struct ItemView {
		QString name;
		int ping = 0;
		bool selected = false;
		ItemState state = ItemState::Checking;
	};

	// Called with the message to show, or an empty string when the text was
	// taken. A call with alive() false is dropped, so a closed or cancelled
	// Add box never receives a late result.
	using ImpulseImportDone = Fn<void(QString message)>;
	using ImpulseImportAlive = Fn<bool()>;

	// ImpulseM VLESS list. Every change is stored in the proxy state and
	// applied through the proxy service; impulseChanges() fires after each one.
	void setImpulseEnabled(bool enabled);
	void importImpulseText(
		const QString &text,
		ImpulseImportAlive alive,
		ImpulseImportDone done);
	void refreshImpulseSubscription(const std::string &id);
	void deleteImpulseServer(const std::string &id);
	void deleteImpulseSubscription(const std::string &id);
	void selectImpulseServer(const std::string &id);
	void shareImpulseList();
	void checkImpulseServers();
	void startImpulseAutoChecks();
	[[nodiscard]] const Impulse::ProxyState::State &impulseState() const;
	[[nodiscard]] std::optional<Impulse::ProxyState::Server> impulseServer(
		const std::string &id) const;
	[[nodiscard]] bool impulseServerChecking(const std::string &id) const;
	[[nodiscard]] bool impulseRefreshing(const std::string &id) const;
	[[nodiscard]] std::optional<std::int64_t> impulsePing(
		const std::string &id) const;
	// The servers of one block in the order of the last manual check. The
	// order is fixed when that check finishes, so automatic rounds never move
	// the rows.
	[[nodiscard]] std::vector<Impulse::ProxyState::Server> impulseSortedBlock(
		std::vector<Impulse::ProxyState::Server> block) const;
	[[nodiscard]] rpl::producer<> impulseChanges() const;

private:
	void saveImpulse(Impulse::ProxyState::State state);
	// Returns the message for the Add box, or an empty string when the links
	// were added or were all in the list already.
	[[nodiscard]] QString addImpulseServers(
		std::vector<Impulse::ProxyState::Server> servers);
	void fetchImpulseSubscription(
		const std::string &url,
		const std::string &id,
		ImpulseImportAlive alive,
		ImpulseImportDone done);
	void applyImpulseSubscription(
		const std::string &url,
		const std::string &id,
		ImpulseImportAlive alive,
		ImpulseImportDone done,
		Impulse::SubscriptionRefresh result);
	// Asks the service for a check round, and notifies the rows when one starts.
	void requestImpulseCheck(bool manual);
	void showImpulseToast(const QString &text);

	const not_null<Main::Account*> _account;
	Core::SettingsProxy &_settings;
	base::Timer _autoCheckTimer;
	rpl::event_stream<> impulseChangeStream;
	std::shared_ptr<Ui::Show> _show;
	std::vector<std::string> _refreshing;

};
