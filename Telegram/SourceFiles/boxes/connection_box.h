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
#include "mtproto/connection_abstract.h"
#include "mtproto/mtproto_proxy_data.h"

#include <cstdint>
#include <map>
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
		WaitingForBrowser,
	};
	struct ItemView {
		int id = 0;
		QString type;
		QString host;
		uint32 port = 0;
		int ping = 0;
		bool selected = false;
		bool deleted = false;
		bool supportsShare = false;
		bool supportsCalls = false;
		bool web = false;
		bool canOpenBrowser = false;
		bool refreshable = false;
		bool deletable = false;
		ItemState state = ItemState::Checking;

	};

	// ImpulseM VLESS list. Every change is stored in the proxy state and
	// applied through the proxy service; impulseChanges() fires after each one.
	void setImpulseEnabled(bool enabled);
	[[nodiscard]] bool setImpulseUseForTransport(bool enabled);
	[[nodiscard]] bool setImpulseUseForCalls(bool enabled);
	void importText(const QString &text);
	void refreshImpulseSubscription(const std::string &id);
	void deleteImpulseServer(const std::string &id);
	void deleteImpulseSubscription(const std::string &id);
	void selectImpulseServer(const std::string &id);
	void shareImpulseServer(const std::string &id, bool qr);
	void shareImpulseList();
	void checkImpulseServers();
	[[nodiscard]] bool impulseChecking() const;
	[[nodiscard]] std::optional<std::int64_t> impulsePing(
		const std::string &id) const;
	[[nodiscard]] rpl::producer<> impulseChanges() const;

	void deleteItem(int id);
	void deleteItems();
	void restoreItem(int id);
	void shareItem(int id, bool qr);
	void shareItems();
	void applyItem(int id);
	void openBrowser(int id);
	object_ptr<Ui::BoxContent> editItemBox(int id);
	object_ptr<Ui::BoxContent> addNewItemBox();
	bool setProxySettings(ProxyData::Settings value);
	bool setProxyForCalls(bool enabled);
	void setProxyRotationEnabled(bool enabled);
	void setProxyRotationTimeout(int value);
	void setTryIPv6(bool enabled);
	rpl::producer<ProxyData::Settings> proxySettingsValue() const;

	[[nodiscard]] bool contains(const ProxyData &proxy) const;
	void addNewItem(const ProxyData &proxy);

	rpl::producer<ItemView> views() const;

	rpl::producer<bool> listShareableChanges() const;

	~ProxiesBoxController();

private:
	using Checker = MTP::details::ConnectionPointer;
	struct Item {
		int id = 0;
		ProxyData data;
		bool deleted = false;
		Checker checker;
		Checker checkerv6;
		ItemState state = ItemState::Checking;
		int ping = 0;

	};

	std::vector<Item>::iterator findById(int id);
	std::vector<Item>::iterator findByProxy(const ProxyData &proxy);
	void setDeleted(int id, bool deleted);
	void updateView(const Item &item);
	void share(const ProxyData &proxy, bool qr = false);
	void saveDelayed();
	void refreshChecker(Item &item);
	void saveImpulse(Impulse::ProxyState::State state);
	void addImpulseServers(std::vector<Impulse::ProxyState::Server> servers);
	void fetchImpulseSubscription(
		const std::string &url,
		const std::string &id);
	void applyImpulseSubscription(
		const std::string &url,
		const std::string &id,
		Impulse::SubscriptionRefresh result);
	void finishImpulseCheck(
		const std::vector<Impulse::ProxyState::Server> &servers,
		const std::vector<Impulse::Xray::PingResult> &results);
	void showImpulseToast(const QString &text);

	void replaceItemWith(
		std::vector<Item>::iterator which,
		std::vector<Item>::iterator with);
	void replaceItemValue(
		std::vector<Item>::iterator which,
		const ProxyData &proxy);

	const not_null<Main::Account*> _account;
	Core::SettingsProxy &_settings;
	int _idCounter = 0;
	std::vector<Item> _list;
	rpl::event_stream<ItemView> _views;
	base::Timer _saveTimer;
	rpl::event_stream<ProxyData::Settings> _proxySettingsChanges;
	rpl::event_stream<> impulseChangeStream;
	std::map<std::string, std::int64_t> impulsePingResults;
	bool impulseCheckRunning = false;
	std::shared_ptr<Ui::Show> _show;

	ProxyData _lastSelectedProxy;
	bool _lastSelectedProxyUsed = false;

	rpl::lifetime _lifetime;

};
