/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/connection_box.h"

#include "base/call_delayed.h"
#include "base/qt/qt_key_modifiers.h"
#include "base/qthelp_regex.h"
#include "base/qthelp_url.h"
#include "base/weak_ptr.h"
#include "boxes/impulse_proxy_advanced_box.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/impulse_features.h"
#include "core/impulse_proxy/proxy_ping.h"
#include "core/impulse_proxy/proxy_service.h"
#include "core/local_url_handlers.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/facade.h"
#include "mtproto/impulse/core/proxy_input.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/proxy_check.h"
#include "mtproto/web_proxy/web_proxy_transport.h"
#include "qr/qr_generate.h"
#include "settings/settings_common.h"
#include "storage/localstorage.h"
#include "ui/basic_click_handlers.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/peer_qr_box.h"
#include "ui/effects/animations.h"
#include "ui/effects/radial_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/text_options.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/dropdown_menu.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/number_input.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/table_layout.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "ui/ui_utility.h"
#include "boxes/abstract_box.h" // Ui::show().
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_connection_box.h"
#include "styles/style_info.h"
#include "styles/style_intro.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtWidgets/QTextEdit>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr auto kSaveSettingsDelayedTimeout = crl::time(1000);

// Proxied calls need the loopback relay, which is not shipped yet. Until it is,
// the calls toggle stays visible but disabled, so calls never go through the
// proxy.
constexpr auto kProxiedCallsRelayReady = false;

[[nodiscard]] bool ProxiedCallsAvailable() {
	return kProxiedCallsRelayReady && Core::App().impulseXrayRunning();
}

using ProxyData = MTP::ProxyData;

struct TopButtonMenuState {
	~TopButtonMenuState() {
		closing = true;
	}

	bool closing = false;
	base::unique_qptr<Ui::PopupMenu> menu;
};

[[nodiscard]] std::vector<QString> ExtractLinkCandidates(const QString &input) {
	auto urls = std::vector<QString>();
	static const auto urlRegex = QRegularExpression(
		R"((?:https?:\/\/[^\s]+|tg:\/\/[^\s]+|vless:\/\/[^\s]+|(?:www\.)?(?:t\.me|telegram\.me|telegram\.dog)\/[^\s]+))",
		QRegularExpression::CaseInsensitiveOption);

	auto it = urlRegex.globalMatch(input);
	while (it.hasNext()) {
		urls.push_back(it.next().captured(0));
	}

	return urls;
}

[[nodiscard]] bool ProxyDataIsShareable(const ProxyData &proxy) {
	using Type = ProxyData::Type;
	return (proxy.type == Type::Socks5)
		|| (proxy.type == Type::Mtproto)
		|| (proxy.type == Type::Web);
}

[[nodiscard]] QString ProxyDataToQueryPath(const ProxyData &proxy) {
	using Type = ProxyData::Type;
	const auto path = [&] {
		switch (proxy.type) {
		case Type::Socks5: return u"socks"_q;
		case Type::Mtproto: return u"proxy"_q;
		case Type::Web: return u"webproxy"_q;
		case Type::None:
		case Type::Http: return QString();
		}
		Unexpected("Proxy type in ProxyDataToQueryPath.");
	}();
	if (path.isEmpty()) {
		return QString();
	}
	const auto server = (proxy.type == Type::Web)
		? qthelp::url_encode(proxy.webAddress())
		: proxy.host;
	return path
		+ "?server=" + server
		+ ((proxy.type != Type::Web)
			? "&port=" + QString::number(proxy.port) : "")
		+ ((proxy.type == Type::Socks5 && !proxy.user.isEmpty())
			? "&user=" + qthelp::url_encode(proxy.user) : "")
		+ ((proxy.type == Type::Socks5 && !proxy.password.isEmpty())
			? "&pass=" + qthelp::url_encode(proxy.password) : "")
		+ (((proxy.type == Type::Mtproto || proxy.type == Type::Web)
				&& !proxy.password.isEmpty())
			? "&secret=" + ((proxy.type == Type::Web)
				? MTP::EncodeWebProxyLinkSecret(proxy)
				: proxy.password) : "");
}

[[nodiscard]] QString ProxyDataToLocalLink(const ProxyData &proxy) {
	const auto queryPath = ProxyDataToQueryPath(proxy);
	return queryPath.isEmpty() ? QString() : (u"tg://"_q + queryPath);
}

[[nodiscard]] QString ProxyDataToPublicLink(
		not_null<Main::Account*> account,
		const ProxyData &proxy) {
	const auto queryPath = ProxyDataToQueryPath(proxy);
	if (queryPath.isEmpty()) {
		return QString();
	}
	if (const auto session = account->maybeSession()) {
		return session->createInternalLinkFull(queryPath);
	}
	return QString();
}

[[nodiscard]] QColor ProxyQrActiveColor() {
	return QColor(0x40, 0xA7, 0xE3);
}

[[nodiscard]] QImage ProxyQr(const Qr::Data &data, int pixel, int max = 0) {
	Expects(data.size > 0);

	const auto available = max
		? std::max(max - 2 * st::introQrBackgroundSkip, 1)
		: 0;
	if (available > 0 && data.size * pixel > available) {
		pixel = std::max(available / data.size, 1);
	}
	return Qr::Generate(
		data,
		pixel * style::DevicePixelRatio(),
		Qt::black,
		Qt::white);
}

[[nodiscard]] QImage ProxyQrLogo() {
	const auto size = QSize(st::introQrCenterSize, st::introQrCenterSize);
	auto result = QImage(
		size * style::DevicePixelRatio(),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	result.setDevicePixelRatio(style::DevicePixelRatio());
	{
		auto p = QPainter(&result);
		auto hq = PainterHighQualityEnabler(p);
		p.setBrush(ProxyQrActiveColor());
		p.setPen(Qt::NoPen);
		p.drawEllipse(QRect(QPoint(), size));
		st::introQrPlane.paintInCenter(p, QRect(QPoint(), size));
	}
	return result;
}

[[nodiscard]] QImage ProxyQrTile(const QString &link, int max = 0) {
	const auto data = Qr::Encode(link, Qr::Redundancy::Quartile);
	const auto qr = ProxyQr(
		data,
		st::introQrPixel,
		max);
	const auto qrSize = qr.width() / style::DevicePixelRatio();
	const auto skip = st::introQrBackgroundSkip;
	const auto size = QSize(qrSize + 2 * skip, qrSize + 2 * skip);
	auto result = QImage(
		size * style::DevicePixelRatio(),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	result.setDevicePixelRatio(style::DevicePixelRatio());
	{
		auto p = QPainter(&result);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		p.drawRoundedRect(
			QRect(QPoint(), size),
			st::introQrBackgroundRadius,
			st::introQrBackgroundRadius);
		p.drawImage(QRect(skip, skip, qrSize, qrSize), qr);
		const auto logo = ProxyQrLogo();
		p.drawImage(
			QRect(
				(size.width() - st::introQrCenterSize) / 2,
				(size.height() - st::introQrCenterSize) / 2,
				st::introQrCenterSize,
				st::introQrCenterSize),
			logo);
	}
	return result;
}

[[nodiscard]] QImage ProxyQrForShare(const QString &link) {
	return ProxyQrTile(
		link,
		st::boxWidth - st::boxRowPadding.left() - st::boxRowPadding.right());
}

void ShowProxyQrBox(std::shared_ptr<Ui::Show> show, const QString &link) {
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_proxy_edit_share_qr_box_title());
		box->addButton(tr::lng_about_done(), [=] { box->closeBox(); });

		const auto copyCallback = [=, image = ProxyQrForShare(link)] {
			QGuiApplication::clipboard()->setImage(image);
			show->showToast({
				.text = { tr::lng_group_invite_qr_copied(tr::now) },
				.iconLottie = u"toast/copy"_q,
				.iconLottieSize = st::toastLottieIconSize,
			});
		};

		const auto qr = ProxyQrTile(
			link,
			st::boxWidth
				- st::boxRowPadding.left()
				- st::boxRowPadding.right());
		const auto size = qr.width() / style::DevicePixelRatio();
		const auto height = st::inviteLinkQrSkip * 2 + size;
		const auto container = box->addRow(
			object_ptr<Ui::BoxContentDivider>(box, height),
			st::inviteLinkQrMargin);
		const auto button = Ui::CreateChild<Ui::AbstractButton>(container);
		button->resize(size, size);
		button->paintRequest(
		) | rpl::on_next([=] {
			QPainter(button).drawImage(QRect(0, 0, size, size), qr);
		}, button->lifetime());
		container->widthValue(
		) | rpl::on_next([=](int width) {
			button->move((width - size) / 2, st::inviteLinkQrSkip);
		}, button->lifetime());
		button->setClickedCallback(copyCallback);

		box->addLeftButton(tr::lng_group_invite_context_copy(), copyCallback);
	}));
}

void ShareProxy(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account,
		const ProxyData &proxy,
		bool qr) {
	if (!ProxyDataIsShareable(proxy)) {
		return;
	}
	const auto qrLink = ProxyDataToLocalLink(proxy);
	if (qrLink.isEmpty()) {
		return;
	}
	if (qr) {
		if (account->sessionExists()) {
			show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
				Ui::FillPeerQrBox(
					box,
					nullptr,
					qrLink,
					rpl::single(QString()),
					tr::lng_proxy_edit_share_qr_box_title());
			}));
		} else {
			ShowProxyQrBox(show, qrLink);
		}
		return;
	}
	const auto internal = base::IsCtrlPressed()
		|| base::IsAltPressed()
		|| base::IsShiftPressed();
	const auto shareLink = internal
		? qrLink
		: ProxyDataToPublicLink(account, proxy);
	if (shareLink.isEmpty()) {
		return;
	}
	TextUtilities::SetClipboardText(TextForMimeData::Simple(shareLink));
	show->showToast({
		.text = { tr::lng_username_copied(tr::now) },
		.iconLottie = u"toast/voip_invite"_q,
		.iconLottieSize = st::toastLottieIconSize,
	});
}

[[nodiscard]] ProxyData ProxyDataFromFields(
		ProxyData::Type type,
		const QMap<QString, QString> &fields) {
	auto proxy = ProxyData();
	proxy.type = type;
	const auto web = (type == ProxyData::Type::Web);
	const auto server = fields.value(u"server"_q);
	proxy.port = web ? 443 : fields.value(u"port"_q).toUInt();
	if (web) {
		proxy.setWebAddress(
			server.isEmpty() ? fields.value(u"host"_q) : server);
	} else {
		proxy.host = server;
	}
	if (type == ProxyData::Type::Socks5) {
		proxy.user = fields.value(u"user"_q);
		proxy.password = fields.value(u"pass"_q);
	} else if (type == ProxyData::Type::Mtproto || web) {
		proxy.password = fields.value(u"secret"_q);
		proxy.password.replace('+', '-').replace('/', '_');
		if (web) {
			proxy.password = MTP::DecodeWebProxyLinkSecret(
				proxy.password,
				!proxy.webBasePath().isEmpty());
		}
	}
	return proxy;
};

[[nodiscard]] ProxyData ProxyDataFromLocalUrl(const QString &local) {
	const auto protocol = u"tg://"_q;
	const auto proxyString = u"proxy"_q;
	const auto socksString = u"socks"_q;
	const auto webproxyString = u"webproxy"_q;
	if (!local.startsWith(protocol + proxyString, Qt::CaseInsensitive)
		&& !local.startsWith(protocol + socksString, Qt::CaseInsensitive)
		&& !local.startsWith(protocol + webproxyString, Qt::CaseInsensitive)) {
		return ProxyData();
	}
	const auto command = base::StringViewMid(local, protocol.size(), 8192);
	using namespace qthelp;
	const auto options = RegExOption::CaseInsensitive;
	for (const auto &[expression, _] : Core::LocalUrlHandlers()) {
		const auto midExpression = base::StringViewMid(expression, 1);
		const auto isSocks = midExpression.startsWith(socksString);
		const auto isWeb = midExpression.startsWith(webproxyString);
		if (!midExpression.startsWith(proxyString) && !isSocks && !isWeb) {
			continue;
		}
		const auto match = regex_match(expression, command, options);
		if (!match) {
			continue;
		}
		const auto type = isWeb
			? ProxyData::Type::Web
			: isSocks
			? ProxyData::Type::Socks5
			: ProxyData::Type::Mtproto;
		auto fields = url_parse_params(
			match->captured(1),
			qthelp::UrlParamNameTransform::ToLower);
		return ProxyDataFromFields(type, fields);
	}
	return ProxyData();
}

[[nodiscard]] ProxyData ProxyDataFromClipboard() {
	const auto candidates = ExtractLinkCandidates(
		QGuiApplication::clipboard()->text());
	if (candidates.size() != 1) {
		return ProxyData();
	}
	const auto trimmed = candidates.front().trimmed();
	const auto converted = Core::TryConvertUrlToLocal(trimmed);
	const auto local = converted.isEmpty() ? trimmed : converted;
	const auto proxy = ProxyDataFromLocalUrl(local);
	return proxy.valid() ? proxy : ProxyData();
}

[[nodiscard]] QString ImpulseFetchError(const std::string &error) {
	if (error.starts_with(Impulse::ProxyInput::kErrorHttp)) {
		return tr::lng_impulse_proxy_list_error_http(
			tr::now,
			lt_code,
			QString::fromStdString(
				error.substr(Impulse::ProxyInput::kErrorHttp.size() + 1)));
	} else if (error == Impulse::ProxyInput::kErrorNetwork) {
		return tr::lng_impulse_proxy_list_error_network(tr::now);
	} else if (error.starts_with(Impulse::ProxyInput::kErrorNoLinks)) {
		return tr::lng_impulse_proxy_list_error_no_links(tr::now);
	}
	return tr::lng_impulse_proxy_list_error_invalid(tr::now);
}

[[nodiscard]] bool ImpulseHasServer(
		const Impulse::ProxyState::State &state,
		const std::string &id) {
	for (const Impulse::ProxyState::Server &server : state.manual) {
		if (server.id == id) {
			return true;
		}
	}
	for (const Impulse::ProxyState::Subscription &subscription
			: state.subscriptions) {
		for (const Impulse::ProxyState::Server &server
				: subscription.servers) {
			if (server.id == id) {
				return true;
			}
		}
	}
	return false;
}

// Every server that the check measures: the manual ones, then the ones of
// each subscription, in the order of the list.
[[nodiscard]] std::vector<Impulse::ProxyState::Server> ImpulseAllServers(
		const Impulse::ProxyState::State &state) {
	auto result = std::vector<Impulse::ProxyState::Server>();
	result.insert(result.end(), state.manual.begin(), state.manual.end());
	for (const Impulse::ProxyState::Subscription &subscription
			: state.subscriptions) {
		result.insert(
			result.end(),
			subscription.servers.begin(),
			subscription.servers.end());
	}
	return result;
}

// The first server of the list, which the proxy picks when it is turned on.
[[nodiscard]] std::optional<std::string> ImpulseFirstServerId(
		const Impulse::ProxyState::State &state) {
	const auto all = ImpulseAllServers(state);
	if (all.empty()) {
		return std::nullopt;
	}
	return all.front().id;
}

void ShowImpulseToast(
		const std::shared_ptr<Ui::Show> &show,
		const QString &text) {
	if (show) {
		show->showToast(text);
	}
}

void ShareImpulseLink(
		const std::shared_ptr<Ui::Show> &show,
		const QString &link,
		bool qr) {
	if (link.isEmpty() || !show) {
		return;
	}
	if (qr) {
		ShowProxyQrBox(show, link);
		return;
	}
	TextUtilities::SetClipboardText(TextForMimeData::Simple(link));
	show->showToast({
		.text = { tr::lng_username_copied(tr::now) },
		.iconLottie = u"toast/voip_invite"_q,
		.iconLottieSize = st::toastLottieIconSize,
	});
}

// A box with one field for a vless:// link, several links or a subscription
// URL. The text goes to the controller, which converts it.
void ShowImpulseAddBox(
		const std::shared_ptr<Ui::Show> &show,
		not_null<ProxiesBoxController*> controller) {
	const base::weak_ptr<ProxiesBoxController> weak = base::make_weak(controller.get());
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_impulse_proxy_list_add_title());

		const auto inner = box->verticalLayout();
		const auto field = inner->add(
			object_ptr<Ui::InputField>(
				inner,
				st::defaultInputField,
				tr::lng_impulse_proxy_list_add_placeholder(),
				QString()),
			st::boxRowPadding);
		Ui::AddDividerText(
			inner,
			tr::lng_impulse_proxy_list_add_about());

		const auto submit = [=] {
			const auto text = field->getLastText().trimmed();
			if (text.isEmpty()) {
				field->showError();
				return;
			}
			if (ProxiesBoxController *const strong = weak.get()) {
				strong->importText(text);
			}
			box->closeBox();
		};
		field->submits() | rpl::on_next(submit, field->lifetime());
		box->setFocusCallback([=] { field->setFocusFast(); });
		box->addButton(tr::lng_proxy_add(), submit);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

class HostInput : public Ui::MaskedInputField {
public:
	HostInput(
		QWidget *parent,
		const style::InputField &st,
		rpl::producer<QString> placeholder,
		const QString &val);

protected:
	void correctValue(
		const QString &was,
		int wasCursor,
		QString &now,
		int &nowCursor) override;
};

HostInput::HostInput(
	QWidget *parent,
	const style::InputField &st,
	rpl::producer<QString> placeholder,
	const QString &val)
: MaskedInputField(parent, st, std::move(placeholder), val) {
	setInputMethodHints(Qt::ImhUrlCharactersOnly
		| Qt::ImhNoAutoUppercase
		| Qt::ImhNoPredictiveText);
}

void HostInput::correctValue(
		const QString &was,
		int wasCursor,
		QString &now,
		int &nowCursor) {
	QString newText;
	int newCursor = nowCursor;
	newText.reserve(now.size());
	for (auto i = 0, l = int(now.size()); i < l; ++i) {
		if (now[i] == ',') {
			newText.append('.');
		} else {
			newText.append(now[i]);
		}
	}
	setCorrectedText(now, nowCursor, newText, newCursor);
}

class Base64UrlInput : public Ui::MaskedInputField {
public:
	Base64UrlInput(
		QWidget *parent,
		const style::InputField &st,
		rpl::producer<QString> placeholder,
		const QString &val);

protected:
	void correctValue(
		const QString &was,
		int wasCursor,
		QString &now,
		int &nowCursor) override;

};

Base64UrlInput::Base64UrlInput(
	QWidget *parent,
	const style::InputField &st,
	rpl::producer<QString> placeholder,
	const QString &val)
: MaskedInputField(parent, st, std::move(placeholder), val) {
	setInputMethodHints(Qt::ImhLatinOnly
		| Qt::ImhNoAutoUppercase
		| Qt::ImhNoPredictiveText
		| Qt::ImhSensitiveData);
	static const auto RegExp = QRegularExpression("^[a-zA-Z0-9_\\-]+$");
	if (!RegExp.match(val).hasMatch()) {
		setText(QString());
	}
}

void Base64UrlInput::correctValue(
		const QString &was,
		int wasCursor,
		QString &now,
		int &nowCursor) {
	QString newText;
	newText.reserve(now.size());
	auto newPos = nowCursor;
	for (auto i = 0, l = int(now.size()); i < l; ++i) {
		const auto ch = now[i];
		if ((ch >= '0' && ch <= '9')
			|| (ch >= 'a' && ch <= 'z')
			|| (ch >= 'A' && ch <= 'Z')
			|| (ch == '-')
			|| (ch == '_')) {
			newText.append(ch);
		} else if (i < nowCursor) {
			--newPos;
		}
	}
	setCorrectedText(now, nowCursor, newText, newPos);
}

class ProxyRow : public Ui::RippleButton {
public:
	using View = ProxiesBoxController::ItemView;
	using State = ProxiesBoxController::ItemState;

	ProxyRow(QWidget *parent, View &&view);

	void updateFields(View &&view);

	rpl::producer<> deleteClicks() const;
	rpl::producer<> restoreClicks() const;
	rpl::producer<> editClicks() const;
	rpl::producer<> shareClicks() const;
	rpl::producer<> showQrClicks() const;
	rpl::producer<> openBrowserClicks() const;
	rpl::producer<> refreshClicks() const;

protected:
	int resizeGetHeight(int newWidth) override;

	void paintEvent(QPaintEvent *e) override;

private:
	void setupControls(View &&view);
	int countAvailableWidth() const;
	void radialAnimationCallback();
	void paintCheck(Painter &p);
	void showMenu();

	View _view;

	Ui::Text::String _title;
	object_ptr<Ui::IconButton> _menuToggle;
	rpl::event_stream<> _deleteClicks;
	rpl::event_stream<> _restoreClicks;
	rpl::event_stream<> _editClicks;
	rpl::event_stream<> _shareClicks;
	rpl::event_stream<> _showQrClicks;
	rpl::event_stream<> _openBrowserClicks;
	rpl::event_stream<> refreshClickStream;
	base::unique_qptr<Ui::DropdownMenu> _menu;

	bool _set = false;
	Ui::Animations::Simple _toggled;
	Ui::Animations::Simple _setAnimation;
	std::unique_ptr<Ui::InfiniteRadialAnimation> _progress;
	std::unique_ptr<Ui::InfiniteRadialAnimation> _checking;

	int _skipLeft = 0;
	int _skipRight = 0;

};

// The ImpulseM VLESS list: the enable switch, the two transport toggles, the
// manual servers and the subscriptions with their servers, and the check,
// advanced settings and share actions. Rows are rebuilt from the proxy state
// whenever the list, the core status or a check changes.
class ProxiesBox : public Ui::BoxContent {
public:
	ProxiesBox(
		QWidget*,
		not_null<ProxiesBoxController*> controller,
		Core::SettingsProxy &settings,
		const QString &highlightId = QString());

protected:
	void prepare() override;
	void showFinished() override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	// What one row stands for. The index in rowKeys is the row id.
	struct RowKey {
		enum class Kind {
			Manual,
			Member,
			Subscription,
		};

		Kind kind = Kind::Manual;
		std::string id;
	};

	void setupContent();
	void setupTopButton();
	void addNewProxy();
	void rebuildRows();
	void addServerRow(
		not_null<Ui::VerticalLayout*> wrap,
		const Impulse::ProxyState::State &state,
		const Impulse::ProxyState::Server &server,
		RowKey::Kind kind);
	void addSubscriptionRow(
		not_null<Ui::VerticalLayout*> wrap,
		const Impulse::ProxyState::Subscription &subscription);
	void setupRowButtons(int id, not_null<ProxyRow*> button);
	void refreshControls();
	int rowHeight() const;

	not_null<ProxiesBoxController*> _controller;
	Core::SettingsProxy &_settings;
	QPointer<Ui::Checkbox> enabledToggle;
	QPointer<Ui::Checkbox> transportToggle;
	QPointer<Ui::SlideWrap<Ui::Checkbox>> callsToggle;
	QPointer<Ui::DividerLabel> _about;
	QPointer<Ui::VerticalLayout> _wrap;
	QPointer<Ui::SlideWrap<Ui::VerticalLayout>> shareListWrap;
	std::vector<RowKey> rowKeys;

	QPointer<Ui::RpWidget> _addProxyButton;
	QPointer<Ui::RpWidget> _shareListButton;
	QString _highlightId;

};

class ProxyBox final : public Ui::BoxContent {
public:
	ProxyBox(
		QWidget*,
		const ProxyData &data,
		Fn<void(ProxyData)> callback,
		Fn<void(ProxyData)> shareCallback);

private:
	using Type = ProxyData::Type;

	void prepare() override;
	void setInnerFocus() override {
		if (_type->current() == Type::Web) {
			_webHost->setFocusFast();
		} else {
			_host->setFocusFast();
		}
	}

	void refreshButtons();
	ProxyData collectData();
	void save();
	void share();
	void setupControls(const ProxyData &data);
	void setupTypes();
	void setupSocketAddress(const ProxyData &data);
	void setupWebAddress(const ProxyData &data);
	void setupCredentials(const ProxyData &data);
	void setupMtprotoCredentials(const ProxyData &data);

	void addLabel(
		not_null<Ui::VerticalLayout*> parent,
		const QString &text) const;

	const bool _allowShare = false;
	Fn<void(ProxyData)> _callback;
	Fn<void(ProxyData)> _shareCallback;

	object_ptr<Ui::VerticalLayout> _content;

	std::shared_ptr<Ui::RadioenumGroup<Type>> _type;

	QPointer<Ui::SlideWrap<>> _aboutSponsored;
	QPointer<HostInput> _host;
	QPointer<Ui::NumberInput> _port;
	QPointer<Ui::InputField> _user;
	QPointer<Ui::PasswordInput> _password;
	QPointer<Base64UrlInput> _secret;
	QPointer<Ui::InputField> _webHost;

	QPointer<Ui::SlideWrap<Ui::VerticalLayout>> _socketAddress;
	QPointer<Ui::SlideWrap<Ui::VerticalLayout>> _webAddress;
	QPointer<Ui::SlideWrap<Ui::VerticalLayout>> _credentials;
	QPointer<Ui::SlideWrap<Ui::VerticalLayout>> _mtprotoCredentials;

};

ProxyRow::ProxyRow(QWidget *parent, View &&view)
: RippleButton(parent, st::proxyRowRipple)
, _menuToggle(this, st::topBarMenuToggle) {
	setupControls(std::move(view));
}

rpl::producer<> ProxyRow::deleteClicks() const {
	return _deleteClicks.events();
}

rpl::producer<> ProxyRow::restoreClicks() const {
	return _restoreClicks.events();
}

rpl::producer<> ProxyRow::editClicks() const {
	return _editClicks.events();
}

rpl::producer<> ProxyRow::shareClicks() const {
	return _shareClicks.events();
}

rpl::producer<> ProxyRow::showQrClicks() const {
	return _showQrClicks.events();
}

rpl::producer<> ProxyRow::openBrowserClicks() const {
	return _openBrowserClicks.events();
}

rpl::producer<> ProxyRow::refreshClicks() const {
	return refreshClickStream.events();
}

void ProxyRow::setupControls(View &&view) {
	updateFields(std::move(view));
	_toggled.stop();
	_setAnimation.stop();

	_menuToggle->addClickHandler([=] { showMenu(); });
}

int ProxyRow::countAvailableWidth() const {
	return width() - _skipLeft - _skipRight;
}

void ProxyRow::updateFields(View &&view) {
	if (_view.selected != view.selected) {
		_toggled.start(
			[=] { update(); },
			view.selected ? 0. : 1.,
			view.selected ? 1. : 0.,
			st::defaultRadio.duration);
	}
	_view = std::move(view);
	const auto endpoint = (_view.web || _view.port == 0)
		? _view.host
		: _view.host + ':' + QString::number(_view.port);
	_title.setMarkedText(
		st::proxyRowTitleStyle,
		TextWithEntities()
			.append(_view.type)
			.append(' ')
			.append(tr::link(endpoint, QString())),
		Ui::ItemTextDefaultOptions());

	const auto state = _view.state;
	if (state == State::Connecting) {
		if (!_progress) {
			_progress = std::make_unique<Ui::InfiniteRadialAnimation>(
				[=] { radialAnimationCallback(); },
				st::proxyCheckingAnimation);
		}
		_progress->start();
	} else if (_progress) {
		_progress->stop();
	}
	if (state == State::Checking) {
		if (!_checking) {
			_checking = std::make_unique<Ui::InfiniteRadialAnimation>(
				[=] { radialAnimationCallback(); },
				st::proxyCheckingAnimation);
			_checking->start();
		}
	} else {
		_checking = nullptr;
	}
	const auto set = (state == State::Connecting || state == State::Online);
	if (_set != set) {
		_set = set;
		_setAnimation.start(
			[=] { update(); },
			_set ? 0. : 1.,
			_set ? 1. : 0.,
			st::defaultRadio.duration);
	}

	setPointerCursor(!_view.deleted);

	update();
}

void ProxyRow::radialAnimationCallback() {
	if (!anim::Disabled()) {
		update();
	}
}

int ProxyRow::resizeGetHeight(int newWidth) {
	const auto result = st::proxyRowPadding.top()
		+ st::semiboldFont->height
		+ st::proxyRowSkip
		+ st::normalFont->height
		+ st::proxyRowPadding.bottom();
	auto right = st::proxyRowPadding.right();
	_menuToggle->moveToRight(
		right,
		(result - _menuToggle->height()) / 2,
		newWidth);
	right += _menuToggle->width();
	_skipRight = right;
	_skipLeft = st::proxyRowPadding.left()
		+ st::proxyRowIconSkip;
	return result;
}

void ProxyRow::paintEvent(QPaintEvent *e) {
	Painter p(this);

	if (!_view.deleted) {
		paintRipple(p, 0, 0);
	}

	const auto left = _skipLeft;
	const auto availableWidth = countAvailableWidth();
	auto top = st::proxyRowPadding.top();

	if (_view.deleted) {
		p.setOpacity(st::stickersRowDisabledOpacity);
	}

	paintCheck(p);

	p.setPen(st::proxyRowTitleFg);
	p.setFont(st::semiboldFont);
	p.setTextPalette(st::proxyRowTitlePalette);
	_title.drawLeftElided(p, left, top, availableWidth, width());
	top += st::semiboldFont->height + st::proxyRowSkip;

	const auto statusFg = [&] {
		switch (_view.state) {
		case State::Online:
			return st::proxyRowStatusFgOnline;
		case State::Unavailable:
			return st::proxyRowStatusFgOffline;
		case State::WaitingForBrowser:
			return st::proxyRowStatusFg;
		case State::Available:
			return st::proxyRowStatusFgAvailable;
		default:
			return st::proxyRowStatusFg;
		}
	}();
	const auto status = [&] {
		switch (_view.state) {
		case State::Available:
			return tr::lng_proxy_available(
				tr::now,
				lt_ping,
				QString::number(_view.ping));
		case State::Checking:
			return tr::lng_proxy_checking(tr::now);
		case State::NotTested:
			return tr::lng_proxy_not_tested(tr::now);
		case State::Connecting:
			return tr::lng_proxy_connecting(tr::now);
		case State::Online:
			return tr::lng_proxy_online(tr::now);
		case State::Unavailable:
			return tr::lng_proxy_unavailable(tr::now);
		case State::WaitingForBrowser:
			return tr::lng_proxy_web_waiting(tr::now);
		}
		Unexpected("State in ProxyRow::paintEvent.");
	}();
	p.setPen(_view.deleted ? st::proxyRowStatusFg : statusFg);
	p.setFont(st::normalFont);

	auto statusLeft = left;
	if (_checking) {
		_checking->draw(
			p,
			{
				st::proxyCheckingPosition.x() + statusLeft,
				st::proxyCheckingPosition.y() + top
			},
			width());
		statusLeft += st::proxyCheckingPosition.x()
			+ st::proxyCheckingAnimation.size.width()
			+ st::proxyCheckingSkip;
	}
	p.drawTextLeft(statusLeft, top, width(), status);
	top += st::normalFont->height + st::proxyRowPadding.bottom();
}

void ProxyRow::paintCheck(Painter &p) {
	const auto loading = _progress
		? _progress->computeState()
		: Ui::RadialState{ 0., 0, arc::kFullLength };
	const auto toggled = _toggled.value(_view.selected ? 1. : 0.)
		* (1. - loading.shown);
	const auto _st = &st::defaultRadio;
	const auto set = _setAnimation.value(_set ? 1. : 0.);

	PainterHighQualityEnabler hq(p);

	const auto left = st::proxyRowPadding.left();
	const auto top = (height() - _st->diameter - _st->thickness) / 2;
	const auto outerWidth = width();

	auto pen = anim::pen(_st->untoggledFg, _st->toggledFg, toggled * set);
	pen.setWidth(_st->thickness);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(_st->bg);
	const auto rect = style::rtlrect(QRectF(left, top, _st->diameter, _st->diameter).marginsRemoved(QMarginsF(_st->thickness / 2., _st->thickness / 2., _st->thickness / 2., _st->thickness / 2.)), outerWidth);
	if (_progress && loading.shown > 0 && anim::Disabled()) {
		anim::DrawStaticLoading(
			p,
			rect,
			_st->thickness,
			pen.color(),
			_st->bg);
	} else if (loading.arcLength < arc::kFullLength) {
		p.drawArc(rect, loading.arcFrom, loading.arcLength);
	} else {
		p.drawEllipse(rect);
	}

	if (toggled > 0 && (!_progress || !anim::Disabled())) {
		p.setPen(Qt::NoPen);
		p.setBrush(anim::brush(_st->untoggledFg, _st->toggledFg, toggled * set));

		auto skip0 = _st->diameter / 2., skip1 = _st->skip / 10., checkSkip = skip0 * (1. - toggled) + skip1 * toggled;
		p.drawEllipse(style::rtlrect(QRectF(left, top, _st->diameter, _st->diameter).marginsRemoved(QMarginsF(checkSkip, checkSkip, checkSkip, checkSkip)), outerWidth));
	}
}

void ProxyRow::showMenu() {
	if (_menu) {
		return;
	}
	const auto hasActions = _view.supportsShare
		|| _view.refreshable
		|| _view.deletable
		|| _view.deleted;
	if (!hasActions) {
		return;
	}
	_menu = base::make_unique_q<Ui::DropdownMenu>(
		window(),
		st::dropdownMenuWithIcons);
	const auto weak = _menu.get();
	_menu->setHiddenCallback([=] {
		weak->deleteLater();
		if (_menu == weak) {
			_menuToggle->setForceRippled(false);
		}
	});
	_menu->setShowStartCallback([=] {
		if (_menu == weak) {
			_menuToggle->setForceRippled(true);
		}
	});
	_menu->setHideStartCallback([=] {
		if (_menu == weak) {
			_menuToggle->setForceRippled(false);
		}
	});
	_menuToggle->installEventFilter(_menu);
	const auto addAction = [&](
			const QString &text,
			Fn<void()> callback,
			const style::icon *icon) {
		return _menu->addAction(text, std::move(callback), icon);
	};
	if (_view.refreshable) {
		addAction(tr::lng_impulse_proxy_list_refresh(tr::now), [=] {
			refreshClickStream.fire({});
		}, &st::menuIconRestore);
	}
	if (_view.canOpenBrowser) {
		addAction(tr::lng_proxy_web_open(tr::now), [=] {
			_openBrowserClicks.fire({});
		}, &st::menuIconLink);
	}
	if (_view.supportsShare) {
		addAction(tr::lng_proxy_edit_share(tr::now), [=] {
			_shareClicks.fire({});
		}, &st::menuIconShare);
		addAction(tr::lng_group_invite_context_qr(tr::now), [=] {
			_showQrClicks.fire({});
		}, &st::menuIconQrCode);
	}
	if (_view.deleted) {
		addAction(tr::lng_proxy_menu_restore(tr::now), [=] {
			_restoreClicks.fire({});
		}, &st::menuIconRestore);
	} else if (_view.deletable) {
		addAction(tr::lng_proxy_menu_delete(tr::now), [=] {
			_deleteClicks.fire({});
		}, &st::menuIconDelete);
	}
	const auto parentTopLeft = window()->mapToGlobal(QPoint());
	const auto buttonTopLeft = _menuToggle->mapToGlobal(QPoint());
	const auto parent = QRect(parentTopLeft, window()->size());
	const auto button = QRect(buttonTopLeft, _menuToggle->size());
	const auto bottom = button.y()
		+ st::proxyDropdownDownPosition.y()
		+ _menu->height()
		- parent.y();
	const auto top = button.y()
		+ st::proxyDropdownUpPosition.y()
		- _menu->height()
		- parent.y();
	if (bottom > parent.height() && top >= 0) {
		const auto left = button.x()
			+ button.width()
			+ st::proxyDropdownUpPosition.x()
			- _menu->width()
			- parent.x();
		_menu->move(left, top);
		_menu->showAnimated(Ui::PanelAnimation::Origin::BottomRight);
	} else {
		const auto left = button.x()
			+ button.width()
			+ st::proxyDropdownDownPosition.x()
			- _menu->width()
			- parent.x();
		_menu->move(left, bottom - _menu->height());
		_menu->showAnimated(Ui::PanelAnimation::Origin::TopRight);
	}
}

ProxiesBox::ProxiesBox(
	QWidget*,
	not_null<ProxiesBoxController*> controller,
	Core::SettingsProxy &settings,
	const QString &highlightId)
: _controller(controller)
, _settings(settings)
, _highlightId(highlightId) {
}

void ProxiesBox::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Copy
		|| (e->key() == Qt::Key_C && e->modifiers() == Qt::ControlModifier)) {
		_controller->shareImpulseList();
	} else if (e->key() == Qt::Key_Paste
		|| (e->key() == Qt::Key_V && e->modifiers() == Qt::ControlModifier)) {
		_controller->importText(QGuiApplication::clipboard()->text());
	} else {
		BoxContent::keyPressEvent(e);
	}
}

void ProxiesBox::prepare() {
	setTitle(tr::lng_proxy_settings());

	_addProxyButton = addButton(tr::lng_proxy_add(), [=] { addNewProxy(); });
	addButton(tr::lng_close(), [=] { closeBox(); });

	setupTopButton();
	setupContent();
}

void ProxiesBox::showFinished() {
	if (_highlightId == u"proxy/add-proxy"_q) {
		if (_addProxyButton) {
			_highlightId = QString();
			Settings::HighlightWidget(
				_addProxyButton,
				{ .rippleShape = true });
		}
	} else if (_highlightId == u"proxy/share-list"_q) {
		if (_shareListButton) {
			_highlightId = QString();
			Settings::HighlightWidget(_shareListButton);
		}
	}
}

void ProxiesBox::setupTopButton() {
	const auto top = addTopButton(st::infoTopBarMenu);
	const auto state = top->lifetime().make_state<TopButtonMenuState>();

	top->setClickedCallback([=] {
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			top,
			st::popupMenuWithIcons);
		const auto raw = state->menu.get();
		const auto addAction = Ui::Menu::CreateAddActionCallback(raw);
		addAction({
			.text = tr::lng_proxy_add_from_clipboard(tr::now),
			.handler = [=] {
				_controller->importText(QGuiApplication::clipboard()->text());
			},
			.icon = &st::menuIconImportTheme,
		});
		raw->setForcedOrigin(Ui::PanelAnimation::Origin::TopRight);
		top->setForceRippled(true);
		raw->setDestroyedCallback([=] {
			if (!state->closing) {
				top->setForceRippled(false);
			}
		});
		raw->popup(
			top->mapToGlobal(
				QPoint(top->width(), top->height() - st::lineWidth * 3)));
		return true;
	});
}

void ProxiesBox::setupContent() {
	const auto inner = setInnerWidget(object_ptr<Ui::VerticalLayout>(this));

	const Impulse::ProxyState::State &startState = _settings.impulseProxyState();
	Ui::Checkbox *const enabled = inner->add(
		object_ptr<Ui::Checkbox>(
			inner,
			tr::lng_proxy_use_custom(tr::now),
			startState.enabled),
		st::proxyUsePadding);
	Ui::Checkbox *const transport = inner->add(
		object_ptr<Ui::Checkbox>(
			inner,
			tr::lng_impulse_proxy_list_transport(tr::now),
			startState.useForTransport),
		st::proxyUsePadding);
	Ui::SlideWrap<Ui::Checkbox> *const calls = inner->add(
		object_ptr<Ui::SlideWrap<Ui::Checkbox>>(
			inner,
			object_ptr<Ui::Checkbox>(
				inner,
				tr::lng_proxy_use_for_calls(tr::now),
				startState.useForCalls),
			style::margins(
				0,
				st::proxyUsePadding.top(),
				0,
				st::proxyUsePadding.bottom())),
		style::margins(
			st::proxyTryIPv6Padding.left(),
			0,
			st::proxyTryIPv6Padding.right(),
			st::proxyTryIPv6Padding.top()));
	enabledToggle = enabled;
	transportToggle = transport;
	callsToggle = calls;

	_about = inner->add(
		object_ptr<Ui::DividerLabel>(
			inner,
			object_ptr<Ui::FlatLabel>(
				inner,
				tr::lng_proxy_about(tr::now),
				st::boxDividerLabel),
			st::proxyAboutPadding),
		style::margins(0, 0, 0, st::proxyRowPadding.top()));

	_wrap = inner->add(object_ptr<Ui::VerticalLayout>(inner));
	inner->add(object_ptr<Ui::FixedHeightWidget>(
		inner,
		st::proxyRowPadding.bottom()));

	Settings::AddButtonWithIcon(
		inner,
		tr::lng_impulse_proxy_list_check(),
		st::settingsButton
	)->setClickedCallback([=] {
		_controller->checkImpulseServers();
	});
	Settings::AddButtonWithIcon(
		inner,
		tr::lng_impulse_proxy_list_advanced(),
		st::settingsButton
	)->setClickedCallback([=] {
		ShowImpulseProxyAdvancedBox(uiShow());
	});

	{
		Ui::SlideWrap<Ui::VerticalLayout> *const wrap = inner->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				inner,
				object_ptr<Ui::VerticalLayout>(inner)));
		shareListWrap = wrap;
		Settings::Button *const shareList = Settings::AddButtonWithIcon(
			wrap->entity(),
			tr::lng_proxy_edit_share_list_button(),
			st::settingsButton,
			{ &st::menuIconCopy });
		_shareListButton = shareList;
		shareList->setClickedCallback([=] {
			_controller->shareImpulseList();
		});
		wrap->finishAnimating();
	}

	enabled->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		_controller->setImpulseEnabled(checked);
	}, enabled->lifetime());
	transport->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		if (!_controller->setImpulseUseForTransport(checked)) {
			transport->setChecked(
				!checked,
				Ui::Checkbox::NotifyAboutChange::DontNotify);
		}
	}, transport->lifetime());
	calls->entity()->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		if (!_controller->setImpulseUseForCalls(checked)) {
			calls->entity()->setChecked(
				!checked,
				Ui::Checkbox::NotifyAboutChange::DontNotify);
		}
	}, calls->lifetime());

	// Rows are rebuilt after the event that changed them, because that event
	// can come from one of the rows, which must not be deleted in its handler.
	_controller->impulseChanges(
	) | rpl::on_next([=] {
		Ui::PostponeCall(this, [=] {
			refreshControls();
			rebuildRows();
		});
	}, inner->lifetime());
	Core::App().impulseProxy().stateValue(
	) | rpl::on_next([=](Impulse::Xray::Status) {
		Ui::PostponeCall(this, [=] {
			rebuildRows();
		});
	}, inner->lifetime());

	refreshControls();
	rebuildRows();
	calls->finishAnimating();
	shareListWrap->finishAnimating();

	inner->resizeToWidth(st::boxWideWidth);

	inner->heightValue(
	) | rpl::map([=](int height) {
		return std::min(
			std::max(height, _about->y()
				+ _about->height()
				+ 3 * rowHeight()),
			st::boxMaxListHeight);
	}) | rpl::distinct_until_changed(
	) | rpl::on_next([=](int height) {
		setDimensions(st::boxWideWidth, height);
	}, inner->lifetime());
}

void ProxiesBox::refreshControls() {
	const Impulse::ProxyState::State &state = _settings.impulseProxyState();
	if (enabledToggle) {
		enabledToggle->setChecked(
			state.enabled,
			Ui::Checkbox::NotifyAboutChange::DontNotify);
	}
	if (transportToggle) {
		transportToggle->setChecked(
			state.useForTransport,
			Ui::Checkbox::NotifyAboutChange::DontNotify);
	}
	if (callsToggle) {
		callsToggle->entity()->setChecked(
			state.useForCalls,
			Ui::Checkbox::NotifyAboutChange::DontNotify);
		callsToggle->entity()->setDisabled(!ProxiedCallsAvailable());
		callsToggle->toggle(state.enabled, anim::type::normal);
	}
	if (shareListWrap) {
		const bool hasShare = std::any_of(
			state.manual.begin(),
			state.manual.end(),
			[](const Impulse::ProxyState::Server &server) {
				return server.shareLink.has_value();
			});
		shareListWrap->toggle(hasShare, anim::type::normal);
	}
}

int ProxiesBox::rowHeight() const {
	return st::proxyRowPadding.top()
		+ st::semiboldFont->height
		+ st::proxyRowSkip
		+ st::normalFont->height
		+ st::proxyRowPadding.bottom();
}

void ProxiesBox::addNewProxy() {
	ShowImpulseAddBox(uiShow(), _controller);
}

void ProxiesBox::rebuildRows() {
	if (!_wrap) {
		return;
	}
	Ui::VerticalLayout *const wrap = _wrap.data();
	wrap->clear();
	rowKeys.clear();

	const Impulse::ProxyState::State &state = _settings.impulseProxyState();
	for (const Impulse::ProxyState::Server &server : state.manual) {
		addServerRow(wrap, state, server, RowKey::Kind::Manual);
	}
	for (const Impulse::ProxyState::Subscription &subscription
			: state.subscriptions) {
		addSubscriptionRow(wrap, subscription);
		for (const Impulse::ProxyState::Server &server
				: subscription.servers) {
			addServerRow(wrap, state, server, RowKey::Kind::Member);
		}
	}
	if (rowKeys.empty()) {
		wrap->add(
			object_ptr<Ui::FlatLabel>(
				wrap,
				tr::lng_impulse_proxy_list_empty(tr::now),
				st::proxyEmptyListLabel),
			st::proxyEmptyListPadding);
	}
	wrap->resizeToWidth(width());
}

void ProxiesBox::addServerRow(
		not_null<Ui::VerticalLayout*> wrap,
		const Impulse::ProxyState::State &state,
		const Impulse::ProxyState::Server &server,
		RowKey::Kind kind) {
	const std::optional<std::int64_t> ping = _controller->impulsePing(
		server.id);
	const bool selected = state.selectedId.has_value()
		&& *state.selectedId == server.id;
	const bool running = Core::App().impulseXrayRunning();

	ProxyRow::View view;
	view.id = int(rowKeys.size());
	view.type = u"VLESS"_q;
	view.host = QString::fromStdString(server.host);
	view.port = std::uint32_t(std::max(server.port, 0));
	view.selected = selected;
	view.supportsShare = (kind == RowKey::Kind::Manual)
		&& server.shareLink.has_value();
	view.deletable = (kind == RowKey::Kind::Manual);
	view.ping = ping.has_value() ? int(std::max<std::int64_t>(*ping, 0)) : 0;
	view.state = ProxyRow::State::NotTested;
	if (_controller->impulseChecking()) {
		view.state = ProxyRow::State::Checking;
	} else if (selected && running) {
		view.state = ProxyRow::State::Online;
	} else if (ping.has_value()) {
		view.state = (*ping >= 0)
			? ProxyRow::State::Available
			: ProxyRow::State::Unavailable;
	}

	const int id = view.id;
	rowKeys.push_back(RowKey{ kind, server.id });
	ProxyRow *const row = wrap->add(
		object_ptr<ProxyRow>(wrap, std::move(view)));
	setupRowButtons(id, row);
}

void ProxiesBox::addSubscriptionRow(
		not_null<Ui::VerticalLayout*> wrap,
		const Impulse::ProxyState::Subscription &subscription) {
	ProxyRow::View view;
	view.id = int(rowKeys.size());
	view.type = (subscription.meta.title.has_value()
			&& !subscription.meta.title->empty())
		? QString::fromStdString(*subscription.meta.title)
		: tr::lng_impulse_proxy_list_subscription(tr::now);
	view.host = tr::lng_impulse_proxy_list_servers(
		tr::now,
		lt_count,
		int(subscription.servers.size()));
	view.port = 0;
	view.refreshable = true;
	view.deletable = true;
	view.state = subscription.lastError.has_value()
		? ProxyRow::State::Unavailable
		: ProxyRow::State::NotTested;

	const int id = view.id;
	rowKeys.push_back(RowKey{ RowKey::Kind::Subscription, subscription.id });
	ProxyRow *const row = wrap->add(
		object_ptr<ProxyRow>(wrap, std::move(view)));
	setupRowButtons(id, row);
}

void ProxiesBox::setupRowButtons(int id, not_null<ProxyRow*> button) {
	const RowKey key = rowKeys[std::size_t(id)];
	const not_null<ProxiesBoxController*> controller = _controller;

	button->deleteClicks(
	) | rpl::on_next([=] {
		if (key.kind == RowKey::Kind::Manual) {
			controller->deleteImpulseServer(key.id);
		} else if (key.kind == RowKey::Kind::Subscription) {
			controller->deleteImpulseSubscription(key.id);
		}
	}, button->lifetime());

	button->refreshClicks(
	) | rpl::on_next([=] {
		controller->refreshImpulseSubscription(key.id);
	}, button->lifetime());

	rpl::merge(
		button->shareClicks() | rpl::map_to(false),
		button->showQrClicks() | rpl::map_to(true)
	) | rpl::on_next([=](bool qr) {
		controller->shareImpulseServer(key.id, qr);
	}, button->lifetime());

	button->clicks(
	) | rpl::on_next([=] {
		if (key.kind != RowKey::Kind::Subscription) {
			controller->selectImpulseServer(key.id);
		}
	}, button->lifetime());
}

ProxyBox::ProxyBox(
	QWidget*,
	const ProxyData &data,
	Fn<void(ProxyData)> callback,
	Fn<void(ProxyData)> shareCallback)
: _allowShare(data.type != Type::None)
, _callback(std::move(callback))
, _shareCallback(std::move(shareCallback))
, _content(this) {
	setupControls(data);
}

void ProxyBox::prepare() {
	setTitle(tr::lng_proxy_edit());

	connect(_host.data(), &HostInput::changed, [=] {
		Ui::PostponeCall(_host, [=] {
			const auto host = _host->getLastText().trimmed();
			static const auto mask = QRegularExpression(
				u"^\\d+\\.\\d+\\.\\d+\\.\\d+:(\\d*)$"_q);
			const auto match = mask.match(host);
			if (_host->cursorPosition() == host.size()
				&& match.hasMatch()) {
				const auto port = match.captured(1);
				_port->setText(port);
				_port->setCursorPosition(port.size());
				_port->setFocus();
				_host->setText(host.mid(0, host.size() - port.size() - 1));
			}
		});
	});
	_port.data()->events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::KeyPress
			&& (static_cast<QKeyEvent*>(e.get())->key() == Qt::Key_Backspace)
			&& _port->cursorPosition() == 0) {
			_host->setCursorPosition(_host->getLastText().size());
			_host->setFocus();
		}
	}, _port->lifetime());

	const auto submit = [=] {
		if (_webHost->hasFocus()
			&& !_webHost->getLastText().trimmed().isEmpty()) {
			_secret->setFocus();
		} else if (_host->hasFocus()
			&& !_host->getLastText().trimmed().isEmpty()) {
			_port->setFocus();
		} else if (_port->hasFocus()
			&& !_port->getLastText().trimmed().isEmpty()) {
			if (_type->current() == Type::Mtproto) {
				_secret->setFocus();
			} else {
				_user->setFocus();
			}
		} else if (_user->hasFocus()) {
			_password->setFocus();
		} else {
			save();
		}
	};
	connect(_host.data(), &Ui::MaskedInputField::submitted, submit);
	connect(_port.data(), &Ui::MaskedInputField::submitted, submit);
	_user->submits(
	) | rpl::on_next(submit, _user->lifetime());
	_webHost->submits(
	) | rpl::on_next(submit, _webHost->lifetime());
	connect(_password.data(), &Ui::MaskedInputField::submitted, submit);
	connect(_secret.data(), &Ui::MaskedInputField::submitted, submit);

	refreshButtons();
	setDimensionsToContent(st::boxWideWidth, _content);
}

void ProxyBox::refreshButtons() {
	clearButtons();
	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	const auto type = _type->current();
	if (_allowShare
		&& (type == Type::Socks5
			|| type == Type::Mtproto
			|| type == Type::Web)) {
		addLeftButton(tr::lng_proxy_share(), [=] { share(); });
	}
}

void ProxyBox::save() {
	if (const auto data = collectData()) {
		_callback(data);
		closeBox();
	}
}

void ProxyBox::share() {
	if (const auto data = collectData()) {
		_shareCallback(data);
	}
}

ProxyData ProxyBox::collectData() {
	auto result = ProxyData();
	result.type = _type->current();
	const auto web = (result.type == Type::Web);
	result.host = web
		? QString()
		: _host->getLastText().trimmed();
	result.port = web
		? 443
		: _port->getLastText().trimmed().toInt();
	result.user = (result.type == Type::Mtproto || web)
		? QString()
		: _user->getLastText();
	result.password = (result.type == Type::Mtproto || web)
		? _secret->getLastText()
		: _password->getLastText();
	if (web) {
		result.setWebAddress(_webHost->getLastText());
	}
	if (result.host.isEmpty()) {
		if (web) {
			_webHost->showError();
		} else {
			_host->showError();
		}
	} else if (!web && !result.port) {
		_port->showError();
	} else if ((result.type == Type::Http || result.type == Type::Socks5)
		&& !result.password.isEmpty() && result.user.isEmpty()) {
		_user->showError();
	} else if ((result.type == Type::Mtproto || web) && !result.valid()) {
		_secret->showError();
	} else if (!result) {
		if (web) {
			_webHost->showError();
		} else {
			_host->showError();
		}
	} else {
		return result;
	}
	return ProxyData();
}

void ProxyBox::setupTypes() {
	const auto types = std::vector<std::pair<Type, QString>>{
		{ Type::Mtproto, u"MTPROTO"_q },
		{ Type::Socks5, u"SOCKS5"_q },
		{ Type::Http, u"HTTP"_q },
		{ Type::Web, u"WEB"_q },
	};
	for (const auto &[type, label] : types) {
		if (!ImpulseFeatures::PROXY
			&& (type == Type::Mtproto || type == Type::Socks5)) {
			continue;
		}
		_content->add(
			object_ptr<Ui::Radioenum<Type>>(
				_content,
				_type,
				type,
				label),
			st::proxyEditTypePadding);
	}
	auto warning = _type->value(
	) | rpl::map([](Type type) {
		return (type == Type::Web)
			? tr::lng_proxy_web_warning(tr::now)
			: tr::lng_proxy_sponsor_warning(tr::now);
	});
	_aboutSponsored = _content->add(object_ptr<Ui::SlideWrap<>>(
		_content,
		object_ptr<Ui::PaddingWrap<>>(
			_content,
			object_ptr<Ui::FlatLabel>(
				_content,
				std::move(warning),
				st::boxDividerLabel),
			st::proxyAboutSponsorPadding)));
}

void ProxyBox::setupSocketAddress(const ProxyData &data) {
	_socketAddress = _content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			_content,
			object_ptr<Ui::VerticalLayout>(_content)));
	const auto content = _socketAddress->entity();
	addLabel(content, tr::lng_proxy_address_label(tr::now));
	const auto address = content->add(
		object_ptr<Ui::FixedHeightWidget>(
			content,
			st::connectionHostInputField.heightMin),
		st::proxyEditInputPadding);
	_host = Ui::CreateChild<HostInput>(
		address,
		st::connectionHostInputField,
		tr::lng_connection_host_ph(),
		data.host);
	_port = Ui::CreateChild<Ui::NumberInput>(
		address,
		st::connectionPortInputField,
		tr::lng_connection_port_ph(),
		data.port ? QString::number(data.port) : QString(),
		65535);
	address->widthValue(
	) | rpl::on_next([=](int width) {
		_port->moveToRight(0, 0);
		_host->resize(
			width - _port->width() - st::proxyEditSkip,
			_host->height());
		_host->moveToLeft(0, 0);
	}, address->lifetime());
}

void ProxyBox::setupWebAddress(const ProxyData &data) {
	_webAddress = _content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			_content,
			object_ptr<Ui::VerticalLayout>(_content)));
	const auto content = _webAddress->entity();
	addLabel(content, tr::lng_proxy_web_server_label(tr::now));
	_webHost = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::connectionUserInputField,
			tr::lng_proxy_web_host_ph(),
			(data.type == Type::Web) ? data.webAddress() : QString()),
		st::proxyEditInputPadding);
	_webHost->setInputMethodHints(Qt::ImhUrlCharactersOnly
		| Qt::ImhNoAutoUppercase
		| Qt::ImhNoPredictiveText);
}

void ProxyBox::setupCredentials(const ProxyData &data) {
	_credentials = _content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			_content,
			object_ptr<Ui::VerticalLayout>(_content)));
	const auto credentials = _credentials->entity();
	addLabel(credentials, tr::lng_proxy_credentials_optional(tr::now));
	_user = credentials->add(
		object_ptr<Ui::InputField>(
			credentials,
			st::connectionUserInputField,
			tr::lng_connection_user_ph(),
			data.user),
		st::proxyEditInputPadding);
	_user->setInputMethodHints(Qt::ImhNoAutoUppercase
		| Qt::ImhNoPredictiveText);

	auto passwordWrap = object_ptr<Ui::RpWidget>(credentials);
	_password = Ui::CreateChild<Ui::PasswordInput>(
		passwordWrap.data(),
		st::connectionPasswordInputField,
		tr::lng_connection_password_ph(),
		(data.type == Type::Mtproto || data.type == Type::Web)
			? QString()
			: data.password);
	_password->move(0, 0);
	_password->heightValue(
	) | rpl::on_next([=, wrap = passwordWrap.data()](int height) {
		wrap->resize(wrap->width(), height);
	}, _password->lifetime());
	passwordWrap->widthValue(
	) | rpl::on_next([=](int width) {
		_password->resize(width, _password->height());
	}, _password->lifetime());
	credentials->add(std::move(passwordWrap), st::proxyEditInputPadding);
}

void ProxyBox::setupMtprotoCredentials(const ProxyData &data) {
	_mtprotoCredentials = _content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			_content,
			object_ptr<Ui::VerticalLayout>(_content)));
	const auto mtproto = _mtprotoCredentials->entity();
	addLabel(mtproto, tr::lng_proxy_credentials(tr::now));

	auto secretWrap = object_ptr<Ui::RpWidget>(mtproto);
	_secret = Ui::CreateChild<Base64UrlInput>(
		secretWrap.data(),
		st::connectionUserInputField,
		tr::lng_connection_proxy_secret_ph(),
		(data.type == Type::Mtproto || data.type == Type::Web)
			? data.password
			: QString());
	_secret->move(0, 0);
	_secret->heightValue(
	) | rpl::on_next([=, wrap = secretWrap.data()](int height) {
		wrap->resize(wrap->width(), height);
	}, _secret->lifetime());
	secretWrap->widthValue(
	) | rpl::on_next([=](int width) {
		_secret->resize(width, _secret->height());
	}, _secret->lifetime());
	mtproto->add(std::move(secretWrap), st::proxyEditInputPadding);
}

void ProxyBox::setupControls(const ProxyData &data) {
	_type = std::make_shared<Ui::RadioenumGroup<Type>>(
		(data.type == Type::None
			? Type::Mtproto
			: data.type));
	_content.create(this);
	_content->resizeToWidth(st::boxWideWidth);
	_content->moveToLeft(0, 0);

	setupTypes();
	setupSocketAddress(data);
	setupWebAddress(data);
	setupCredentials(data);
	setupMtprotoCredentials(data);

	const auto handleType = [=](Type type) {
		const auto web = (type == Type::Web);
		const auto credentialsShown
			= (type == Type::Http || type == Type::Socks5);
		const auto mtprotoShown = (type == Type::Mtproto || web);
		_socketAddress->toggle(!web, anim::type::instant);
		_webAddress->toggle(web, anim::type::instant);
		_credentials->toggle(credentialsShown, anim::type::instant);
		_mtprotoCredentials->toggle(mtprotoShown, anim::type::instant);
		_aboutSponsored->toggle(mtprotoShown, anim::type::instant);
		const auto credentialsPolicy = credentialsShown
			? Qt::StrongFocus
			: Qt::NoFocus;
		_user->rawTextEdit()->setFocusPolicy(credentialsPolicy);
		_password->setFocusPolicy(credentialsPolicy);
		_secret->setFocusPolicy(
			mtprotoShown ? Qt::StrongFocus : Qt::NoFocus);
		_webHost->setFocusPolicy(web ? Qt::StrongFocus : Qt::NoFocus);
	};
	_type->setChangedCallback([=](Type type) {
		handleType(type);
		refreshButtons();
	});
	handleType(_type->current());
}

void ProxyBox::addLabel(
		not_null<Ui::VerticalLayout*> parent,
		const QString &text) const {
	parent->add(
		object_ptr<Ui::FlatLabel>(
			parent,
			text,
			st::proxyEditTitle),
		st::proxyEditTitlePadding);
}

using Connection = MTP::details::AbstractConnection;
using Checker = MTP::ProxyCheckConnection;

} // namespace

ProxiesBoxController::ProxiesBoxController(not_null<Main::Account*> account)
: _account(account)
, _settings(Core::App().settings().proxy())
, _saveTimer([] { Local::writeSettings(); }) {
	_list = ranges::views::all(
		_settings.list()
	) | ranges::views::transform([&](const ProxyData &proxy) {
		return Item{ ++_idCounter, proxy };
	}) | ranges::to_vector;

	_settings.connectionTypeChanges(
	) | rpl::on_next([=] {
		_proxySettingsChanges.fire_copy(_settings.settings());
		const auto i = findByProxy(_settings.selected());
		if (i != end(_list)) {
			updateView(*i);
		}
	}, _lifetime);

	MTP::WebProxy::Transport::StateChanges(
	) | rpl::on_next([=](const MTP::WebProxy::Transport::StateChange &change) {
		const auto i = findByProxy(change.proxy);
		if (i != end(_list)) {
			updateView(*i);
		}
	}, _lifetime);
}

ProxyData ProxiesBoxController::ProxyFromLink(const QString &link) {
	const auto trimmed = link.trimmed();
	const auto converted = Core::TryConvertUrlToLocal(trimmed);
	return ProxyDataFromLocalUrl(converted.isEmpty() ? trimmed : converted);
}

void ProxiesBoxController::ShowApplyConfirmation(
		Window::SessionController *controller,
		Type type,
		const QMap<QString, QString> &fields) {
	const auto proxy = ProxyDataFromFields(type, fields);
	if (!proxy) {
		const auto status = proxy.status();
		auto box = Ui::MakeInformBox(
			((status == ProxyData::Status::Unsupported)
				? tr::lng_proxy_unsupported(tr::now, tr::rich)
				: (status == ProxyData::Status::IncorrectSecret)
				? tr::lng_proxy_incorrect_secret(tr::now, tr::rich)
				: tr::lng_proxy_invalid(tr::now, tr::rich)));
		if (controller) {
			controller->uiShow()->showBox(std::move(box));
		} else {
			Ui::show(std::move(box));
		}
		return;
	}
	static const auto UrlStartRegExp = QRegularExpression(
		"^https://",
		QRegularExpression::CaseInsensitiveOption);
	static const auto UrlEndRegExp = QRegularExpression("/$");
	const auto displayed = "https://"
		+ ((type == Type::Web) ? proxy.webAddress() : proxy.host)
		+ "/";
	const auto parsed = QUrl::fromUserInput(displayed);
	const auto displayUrl = !UrlClickHandler::IsSuspicious(displayed)
		? displayed
		: parsed.isValid()
		? QString::fromUtf8(parsed.toEncoded())
		: UrlClickHandler::ShowEncoded(displayed);
	const auto displayServer = QString(
		displayUrl
	).replace(
		UrlStartRegExp,
		QString()
	).replace(UrlEndRegExp, QString());
	const auto box = [=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_proxy_box_table_title());
		box->setStyle(st::proxyApplyBox);
		box->addTopButton(st::boxTitleClose, [=] {
			box->closeBox();
		});

		if (ProxyDataIsShareable(proxy) && type != Type::Web) {
			const auto account = controller
				? &controller->session().account()
				: &Core::App().activeAccount();
			const auto top = box->addTopButton(st::boxTitleMenu);
			const auto state = top->lifetime().make_state<
				TopButtonMenuState>();
			top->setClickedCallback([=] {
				state->menu = base::make_unique_q<Ui::PopupMenu>(
					top,
					st::popupMenuWithIcons);
				const auto raw = state->menu.get();
				const auto addAction = Ui::Menu::CreateAddActionCallback(raw);
				addAction({
					.text = tr::lng_proxy_edit_share(tr::now),
					.handler = [=] {
						ShareProxy(box->uiShow(), account, proxy, false);
					},
					.icon = &st::menuIconShare,
				});
				addAction({
					.text = tr::lng_group_invite_context_qr(tr::now),
					.handler = [=] {
						ShareProxy(box->uiShow(), account, proxy, true);
					},
					.icon = &st::menuIconQrCode,
				});
				raw->setForcedOrigin(Ui::PanelAnimation::Origin::TopRight);
				top->setForceRippled(true);
				raw->setDestroyedCallback([=] {
					if (!state->closing) {
						top->setForceRippled(false);
					}
				});
				raw->popup(top->mapToGlobal(QPoint(
					top->width(),
					top->height() - st::lineWidth * 3)));
				return true;
			});
		}

		const auto table = box->addRow(
			object_ptr<Ui::TableLayout>(
				box,
				st::proxyApplyBoxTable),
			st::proxyApplyBoxTableMargin);
		const auto addRow = [&](
				rpl::producer<QString> label,
				object_ptr<Ui::RpWidget> value) {
			table->addRow(
				object_ptr<Ui::FlatLabel>(
					table,
					std::move(label),
					table->st().defaultLabel),
				std::move(value),
				st::proxyApplyBoxTableLabelMargin,
				st::proxyApplyBoxTableValueMargin);
		};
		const auto add = [&](
				const QString &value,
				rpl::producer<QString> label) {
			if (!value.isEmpty()) {
				constexpr auto kOneLineCount = 20;
				const auto oneLine = value.length() <= kOneLineCount;
				auto widget = object_ptr<Ui::FlatLabel>(
					table,
					rpl::single(Ui::Text::Wrapped(
						{ value },
						EntityType::Code,
						{})),
					(oneLine
						? table->st().defaultValue
						: st::proxyApplyBoxValueMultiline),
					st::defaultPopupMenu);
				addRow(std::move(label), std::move(widget));
			}
		};
		if (!displayServer.isEmpty()) {
			add(displayServer, tr::lng_proxy_box_server());
		}
		if (type != Type::Web) {
			add(QString::number(proxy.port), tr::lng_proxy_box_port());
		}
		if (type == Type::Socks5) {
			add(proxy.user, tr::lng_proxy_box_username());
			add(proxy.password, tr::lng_proxy_box_password());
		} else if (type == Type::Mtproto || type == Type::Web) {
			add(proxy.password, tr::lng_proxy_box_secret());
		}

		if (type != Type::Web) {
			struct ProxyCheckStatusState {
				Checker v4;
				Checker v6;
				rpl::variable<TextWithEntities> statusValue;
				bool finished = false;
			};
			const auto state
				= box->lifetime().make_state<ProxyCheckStatusState>();
			state->statusValue = Ui::Text::Link(
				tr::lng_proxy_box_check_status(tr::now));
			const auto weak = base::make_weak(box);
			auto statusWidget = object_ptr<Ui::FlatLabel>(
				table,
				state->statusValue.value(),
				table->st().defaultValue,
				st::defaultPopupMenu);
			const auto statusLabel = statusWidget.data();
			addRow(tr::lng_proxy_box_status(), std::move(statusWidget));
			const auto setUnavailable = [=] {
				state->statusValue = TextWithEntities{
					tr::lng_proxy_box_table_unavailable(tr::now),
				};
				statusLabel->setTextColorOverride(
					st::proxyRowStatusFgOffline->c);
			};
			const auto runCheck = [=] {
				if (!weak) {
					return;
				}
				const auto account = controller
					? &controller->session().account()
					: &Core::App().activeAccount();
				state->finished = false;
				state->statusValue = TextWithEntities{
					tr::lng_proxy_box_table_checking(tr::now),
				};
				statusLabel->setTextColorOverride(st::proxyRowStatusFg->c);
				MTP::StartProxyCheck(
					&account->mtp(),
					proxy,
					Core::App().settings().proxy().tryIPv6(),
					state->v4,
					state->v6,
					[=](Connection *raw, int ping) {
						if (!weak || state->finished) {
							return;
						}
						MTP::DropProxyChecker(state->v4, state->v6, raw);
						state->finished = true;
						MTP::ResetProxyCheckers(state->v4, state->v6);
						state->statusValue = TextWithEntities{
							tr::lng_proxy_box_table_available(
								tr::now,
								lt_ping,
								QString::number(ping)),
						};
						statusLabel->setTextColorOverride(
							st::proxyRowStatusFgAvailable->c);
					},
					[=](Connection *raw) {
						if (!weak || state->finished) {
							return;
						}
						MTP::DropProxyChecker(state->v4, state->v6, raw);
						if (!MTP::HasProxyCheckers(state->v4, state->v6)) {
							state->finished = true;
							setUnavailable();
						}
					});
				if (!MTP::HasProxyCheckers(state->v4, state->v6)) {
					state->finished = true;
					setUnavailable();
				}
			};
			statusLabel->setClickHandlerFilter([=](const auto &...) {
				auto &proxy = Core::App().settings().proxy();
				if (proxy.checkIpWarningShown()) {
					runCheck();
				} else {
					box->uiShow()->showBox(Ui::MakeConfirmBox({
						.text = tr::lng_proxy_check_ip_warning(),
						.confirmed = [=](Fn<void()> close) {
							auto &proxy = Core::App().settings().proxy();
							proxy.setCheckIpWarningShown(true);
							Local::writeSettings();
							close();
							runCheck();
						},
						.confirmText = tr::lng_proxy_check_ip_proceed(),
						.title = tr::lng_proxy_check_ip_warning_title(),
					}));
				}
				return false;
			});
		}

		if (type == Type::Mtproto || type == Type::Web) {
			table->addRow(
				object_ptr<Ui::FlatLabel>(
					table,
					(type == Type::Web)
						? tr::lng_proxy_web_warning()
						: tr::lng_proxy_sponsor_warning(),
					st::proxyApplyBoxSponsorLabel),
				object_ptr<Ui::RpWidget>(nullptr),
				st::proxyApplyBoxSponsorMargin,
				st::proxyApplyBoxSponsorMargin);
		}

		const auto enableButton = box->addButton(
			tr::lng_proxy_box_table_button(),
			[=] {
				auto &settings = Core::App().settings().proxy();
				if (settings.indexInList(proxy) < 0) {
					settings.addToList(proxy);
				}
				Core::App().setCurrentProxy(
					proxy,
					ProxyData::Settings::Enabled);
				Local::writeSettings();
				box->closeBox();
			});
		enableButton->setFullRadius(true);
		box->events() | rpl::on_next([=](not_null<QEvent*> e) {
			if ((e->type() != QEvent::KeyPress) || !enableButton) {
				return;
			}
			const auto k = static_cast<QKeyEvent*>(e.get());
			if (k->key() == Qt::Key_Enter || k->key() == Qt::Key_Return) {
				enableButton->clicked({}, Qt::LeftButton);
			}
		}, box->lifetime());
	};
	if (controller) {
		controller->uiShow()->showBox(Box(box));
	} else {
		Ui::show(Box(box));
	}
}

auto ProxiesBoxController::proxySettingsValue() const
-> rpl::producer<ProxyData::Settings> {
	return _proxySettingsChanges.events_starting_with_copy(
		_settings.settings()
	) | rpl::distinct_until_changed();
}

void ProxiesBoxController::refreshChecker(Item &item) {
	if (item.data.type == Type::Web) {
		MTP::ResetProxyCheckers(item.checker, item.checkerv6);
		item.state = ItemState::NotTested;
		item.ping = 0;
		updateView(item);
		return;
	}
	item.state = ItemState::Checking;
	const auto id = item.id;
	MTP::StartProxyCheck(
		&_account->mtp(),
		item.data,
		Core::App().settings().proxy().tryIPv6(),
		item.checker,
		item.checkerv6,
		[=](Connection *raw, int pingTime) {
			const auto item = ranges::find(
				_list,
				id,
				[](const Item &item) { return item.id; });
			if (item == end(_list)) {
				return;
			}
			MTP::DropProxyChecker(item->checker, item->checkerv6, raw);
			MTP::ResetProxyCheckers(item->checker, item->checkerv6);
			if (item->state == ItemState::Checking) {
				item->state = ItemState::Available;
				item->ping = pingTime;
				updateView(*item);
			}
		},
		[=](Connection *raw) {
			const auto item = ranges::find(
				_list,
				id,
				[](const Item &item) { return item.id; });
			if (item == end(_list)) {
				return;
			}
			MTP::DropProxyChecker(item->checker, item->checkerv6, raw);
			if (!MTP::HasProxyCheckers(item->checker, item->checkerv6)
				&& item->state == ItemState::Checking) {
				item->state = ItemState::Unavailable;
				updateView(*item);
			}
		});
	if (!MTP::HasProxyCheckers(item.checker, item.checkerv6)) {
		item.state = ItemState::Unavailable;
	}
}

object_ptr<Ui::BoxContent> ProxiesBoxController::CreateOwningBox(
		not_null<Main::Account*> account,
		const QString &highlightId) {
	auto controller = std::make_unique<ProxiesBoxController>(account);
	auto box = controller->create(highlightId);
	Ui::AttachAsChild(box, std::move(controller));
	return box;
}

object_ptr<Ui::BoxContent> ProxiesBoxController::create(
		const QString &highlightId) {
	auto result = Box<ProxiesBox>(this, _settings, highlightId);
	_show = result->uiShow();
	return result;
}

auto ProxiesBoxController::findById(int id) -> std::vector<Item>::iterator {
	const auto result = ranges::find(
		_list,
		id,
		[](const Item &item) { return item.id; });
	Assert(result != end(_list));
	return result;
}

auto ProxiesBoxController::findByProxy(const ProxyData &proxy)
->std::vector<Item>::iterator {
	return ranges::find(
		_list,
		proxy,
		[](const Item &item) { return item.data; });
}

void ProxiesBoxController::deleteItem(int id) {
	setDeleted(id, true);
}

void ProxiesBoxController::deleteItems() {
	for (const auto &item : _list) {
		setDeleted(item.id, true);
	}
}

void ProxiesBoxController::restoreItem(int id) {
	setDeleted(id, false);
}

void ProxiesBoxController::shareItem(int id, bool qr) {
	share(findById(id)->data, qr);
}

void ProxiesBoxController::shareItems() {
	auto result = QString();
	for (const auto &item : _list) {
		if (!item.deleted && ProxyDataIsShareable(item.data)) {
			const auto link = ProxyDataToPublicLink(_account, item.data);
			if (!link.isEmpty()) {
				result += (result.isEmpty() ? QString() : u"\n\n"_q) + link;
			}
		}
	}
	if (result.isEmpty()) {
		return;
	}
	QGuiApplication::clipboard()->setText(result);
	_show->showToast({
		.text = { tr::lng_proxy_edit_share_list_toast(tr::now) },
		.iconLottie = u"toast/copy"_q,
		.iconLottieSize = st::toastLottieIconSize,
	});
}

void ProxiesBoxController::applyItem(int id) {
	auto item = findById(id);
	if (_settings.isEnabled() && (_settings.selected() == item->data)) {
		return;
	} else if (item->deleted) {
		return;
	}

	auto j = findByProxy(_settings.selected());

	Core::App().setCurrentProxy(
		item->data,
		ProxyData::Settings::Enabled);
	saveDelayed();

	if (j != end(_list)) {
		updateView(*j);
	}
	updateView(*item);
}

void ProxiesBoxController::openBrowser(int id) {
	const auto item = findById(id);
	if (_settings.isEnabled()
		&& _settings.selected() == item->data
		&& item->data.type == Type::Web) {
		MTP::WebProxy::Transport::OpenBrowser(item->data);
	}
}

void ProxiesBoxController::setDeleted(int id, bool deleted) {
	auto item = findById(id);
	if (item->deleted == deleted) {
		return;
	}
	item->deleted = deleted;

	if (deleted) {
		const auto removed = _settings.removeFromList(item->data);
		Assert(removed);

		if (item->data == _settings.selected()) {
			_lastSelectedProxy = _settings.selected();
			_settings.setSelected(MTP::ProxyData());
			if (_settings.isEnabled()) {
				_lastSelectedProxyUsed = true;
				Core::App().setCurrentProxy(
					ProxyData(),
					ProxyData::Settings::System);
				saveDelayed();
			} else {
				_lastSelectedProxyUsed = false;
			}
		}
	} else {
		if (_settings.indexInList(item->data) < 0) {
			const auto &proxies = _settings.list();
			auto insertBefore = item + 1;
			while (insertBefore != end(_list) && insertBefore->deleted) {
				++insertBefore;
			}
			const auto foundIndex = (insertBefore == end(_list))
				? int(proxies.size())
				: _settings.indexInList(insertBefore->data);
			const auto insertIndex = (foundIndex >= 0)
				? foundIndex
				: int(proxies.size());
			_settings.insertToList(insertIndex, item->data);
		}

		if (!_settings.selected() && _lastSelectedProxy == item->data) {
			Assert(!_settings.isEnabled());

			if (base::take(_lastSelectedProxyUsed)) {
				Core::App().setCurrentProxy(
					base::take(_lastSelectedProxy),
					ProxyData::Settings::Enabled);
			} else {
				_settings.setSelected(base::take(_lastSelectedProxy));
			}
		}
	}
	saveDelayed();
	updateView(*item);
}

object_ptr<Ui::BoxContent> ProxiesBoxController::editItemBox(int id) {
	return Box<ProxyBox>(findById(id)->data, [=](const ProxyData &result) {
		auto i = findById(id);
		auto j = ranges::find(
			_list,
			result,
			[](const Item &item) { return item.data; });
		if (j != end(_list) && j != i) {
			replaceItemWith(i, j);
		} else {
			replaceItemValue(i, result);
		}
	}, [=](const ProxyData &proxy) {
		share(proxy);
	});
}

void ProxiesBoxController::replaceItemWith(
		std::vector<Item>::iterator which,
		std::vector<Item>::iterator with) {
	// Read before the erase below: _list is a vector, so erasing `which`
	// shifts everything after it and leaves `with` naming the wrong item.
	const auto withId = with->id;
	const auto withDeleted = with->deleted;

	if (which->deleted) {
		// A deleted item is not in the settings list at all, so it has to
		// go back in before it can be removed - the same restore-first
		// order replaceItemValue() uses right below.
		restoreItem(which->id);
	}
	const auto removed = _settings.removeFromList(which->data);
	Assert(removed);

	_views.fire({ which->id });
	_list.erase(which);

	if (withDeleted) {
		restoreItem(withId);
	}
	applyItem(withId);
	saveDelayed();
}

void ProxiesBoxController::replaceItemValue(
		std::vector<Item>::iterator which,
		const ProxyData &proxy) {
	if (which->deleted) {
		restoreItem(which->id);
	}

	const auto replaced = _settings.replaceInList(which->data, proxy);
	Assert(replaced);
	which->data = proxy;
	refreshChecker(*which);

	applyItem(which->id);
	saveDelayed();
}

object_ptr<Ui::BoxContent> ProxiesBoxController::addNewItemBox() {
	const auto fromClipboard = ProxyDataFromClipboard();
	return Box<ProxyBox>(fromClipboard, [=](const ProxyData &result) {
		auto j = ranges::find(
			_list,
			result,
			[](const Item &item) { return item.data; });
		if (j != end(_list)) {
			if (j->deleted) {
				restoreItem(j->id);
			}
			applyItem(j->id);
		} else {
			addNewItem(result);
		}
	}, [=](const ProxyData &proxy) {
		share(proxy);
	});
}

bool ProxiesBoxController::contains(const ProxyData &proxy) const {
	const auto j = ranges::find(
		_list,
		proxy,
		[](const Item &item) { return item.data; });
	return (j != end(_list));
}

void ProxiesBoxController::addNewItem(const ProxyData &proxy) {
	_settings.addToList(proxy);

	_list.push_back({ ++_idCounter, proxy });
	refreshChecker(_list.back());
	applyItem(_list.back().id);
}

bool ProxiesBoxController::setProxySettings(ProxyData::Settings value) {
	if (_settings.settings() == value) {
		return true;
	} else if (value == ProxyData::Settings::Enabled) {
		if (_settings.list().empty()) {
			return false;
		} else if (!_settings.selected()) {
			_settings.setSelected(_settings.list().back());
			auto j = findByProxy(_settings.selected());
			if (j != end(_list)) {
				updateView(*j);
			}
		}
	}
	Core::App().setCurrentProxy(_settings.selected(), value);
	saveDelayed();
	return true;
}

bool ProxiesBoxController::setProxyForCalls(bool enabled) {
	if (_settings.useProxyForCalls() == enabled) {
		return true;
	}
	Impulse::ProxyState::State state = _settings.impulseProxyState();
	if (enabled && !ProxiedCallsAvailable()) {
		return false;
	} else if (!enabled && !state.useForTransport) {
		// At least one of the two transports stays on.
		return false;
	}
	_settings.setUseProxyForCalls(enabled);
	state.useForCalls = enabled;
	_settings.setImpulseProxyState(std::move(state));
	if (_settings.isEnabled() && _settings.selected().supportsCalls()) {
		_settings.connectionTypeChangesNotify();
	}
	saveDelayed();
	return true;
}

void ProxiesBoxController::setProxyRotationEnabled(bool enabled) {
	if (_settings.proxyRotationEnabled() == enabled) {
		return;
	}
	_settings.setProxyRotationEnabled(enabled);
	saveDelayed();
}

void ProxiesBoxController::setProxyRotationTimeout(int value) {
	if (_settings.proxyRotationTimeout() == value) {
		return;
	}
	_settings.setProxyRotationTimeout(value);
	saveDelayed();
}

void ProxiesBoxController::setTryIPv6(bool enabled) {
	if (Core::App().settings().proxy().tryIPv6() == enabled) {
		return;
	}
	Core::App().settings().proxy().setTryIPv6(enabled);
	_account->mtp().restart();
	_settings.connectionTypeChangesNotify();
	saveDelayed();
}

void ProxiesBoxController::saveDelayed() {
	Core::App().proxyRotationSettingsChanged();
	_saveTimer.callOnce(kSaveSettingsDelayedTimeout);
}

auto ProxiesBoxController::views() const -> rpl::producer<ItemView> {
	return _views.events();
}

rpl::producer<bool> ProxiesBoxController::listShareableChanges() const {
	return _views.events_starting_with(ItemView()) | rpl::map([=] {
		for (const auto &item : _list) {
			if (!item.deleted && ProxyDataIsShareable(item.data)) {
				return true;
			}
		}
		return false;
	});
}

void ProxiesBoxController::updateView(const Item &item) {
	const auto selected = (_settings.selected() == item.data);
	const auto deleted = item.deleted;
	const auto type = [&] {
		switch (item.data.type) {
		case Type::Http: return u"HTTP"_q;
		case Type::Socks5: return u"SOCKS5"_q;
		case Type::Mtproto: return u"MTPROTO"_q;
		case Type::Web: return u"WEB"_q;
		}
		Unexpected("Proxy type in ProxiesBoxController::updateView.");
	}();
	const auto state = [&] {
		if (!selected || !_settings.isEnabled()) {
			return item.state;
		} else if (item.data.type == Type::Web) {
			switch (MTP::WebProxy::Transport::CurrentState(item.data)) {
			case MTP::WebProxy::Transport::State::WaitingForBrowser:
				return ItemState::WaitingForBrowser;
			case MTP::WebProxy::Transport::State::Failed:
				return ItemState::Unavailable;
			case MTP::WebProxy::Transport::State::Connected:
				return ItemState::Online;
			case MTP::WebProxy::Transport::State::Idle:
				return ItemState::NotTested;
			case MTP::WebProxy::Transport::State::Connecting:
				return ItemState::Connecting;
			}
			Unexpected("Web proxy transport state.");
		} else if (_account->mtp().dcstate() == MTP::ConnectedState) {
			return ItemState::Online;
		}
		return ItemState::Connecting;
	}();
	const auto supportsShare = ProxyDataIsShareable(item.data);
	const auto supportsCalls = item.data.supportsCalls();
	_views.fire({
		item.id,
		type,
		((item.data.type == Type::Web)
			? item.data.webAddress()
			: item.data.host),
		item.data.port,
		item.ping,
		!deleted && selected,
		deleted,
		!deleted && supportsShare,
		supportsCalls,
		item.data.type == Type::Web,
		!deleted
			&& selected
			&& _settings.isEnabled()
			&& item.data.type == Type::Web
			&& (state == ItemState::WaitingForBrowser
				|| state == ItemState::Unavailable),
		state,
	});
}

void ProxiesBoxController::share(const ProxyData &proxy, bool qr) {
	ShareProxy(_show, _account, proxy, qr);
}

void ProxiesBoxController::setImpulseEnabled(bool enabled) {
	Impulse::ProxyState::State state = _settings.impulseProxyState();
	if (state.enabled == enabled) {
		return;
	}
	state.enabled = enabled;
	if (enabled && !state.selectedId) {
		state.selectedId = ImpulseFirstServerId(state);
	}
	saveImpulse(std::move(state));
}

bool ProxiesBoxController::setImpulseUseForTransport(bool enabled) {
	Impulse::ProxyState::State state = _settings.impulseProxyState();
	if (state.useForTransport == enabled) {
		return true;
	} else if (!enabled && !state.useForCalls) {
		// At least one of the two transports stays on.
		return false;
	}
	state.useForTransport = enabled;
	saveImpulse(std::move(state));
	return true;
}

bool ProxiesBoxController::setImpulseUseForCalls(bool enabled) {
	Impulse::ProxyState::State state = _settings.impulseProxyState();
	if (state.useForCalls == enabled) {
		return true;
	} else if (enabled && !ProxiedCallsAvailable()) {
		return false;
	} else if (!enabled && !state.useForTransport) {
		return false;
	}
	state.useForCalls = enabled;
	saveImpulse(std::move(state));
	return true;
}

void ProxiesBoxController::importText(const QString &text) {
	const QString normalized = QString(text).trimmed().replace(
		QRegularExpression(u"\\s+"_q),
		u"\n"_q);
	const std::string utf8 = normalized.toStdString();
	const Impulse::ProxyInput::InputKind kind = Impulse::ProxyInput::Detect(
		utf8);
	if (kind == Impulse::ProxyInput::InputKind::Links) {
		const base::weak_ptr<ProxiesBoxController> weak = base::make_weak(this);
		Core::App().impulseProxy().importLinks(
			utf8,
			[=](std::vector<Impulse::ProxyState::Server> servers) {
				if (ProxiesBoxController *const that = weak.get()) {
					that->addImpulseServers(std::move(servers));
				}
			});
	} else if (kind == Impulse::ProxyInput::InputKind::Subscription) {
		const std::string url(Impulse::ProxyInput::Trim(utf8));
		fetchImpulseSubscription(
			url,
			Impulse::ProxyInput::SubscriptionId(url));
	} else {
		showImpulseToast(
			tr::lng_proxy_add_from_clipboard_failed_toast(tr::now));
	}
}

void ProxiesBoxController::refreshImpulseSubscription(const std::string &id) {
	const Impulse::ProxyState::State state = _settings.impulseProxyState();
	const auto found = std::find_if(
		state.subscriptions.begin(),
		state.subscriptions.end(),
		[&](const Impulse::ProxyState::Subscription &subscription) {
			return subscription.id == id;
		});
	if (found == state.subscriptions.end()) {
		return;
	}
	fetchImpulseSubscription(found->url, id);
}

void ProxiesBoxController::deleteImpulseServer(const std::string &id) {
	Impulse::ProxyState::State state = _settings.impulseProxyState();
	const std::size_t removed = std::erase_if(
		state.manual,
		[&](const Impulse::ProxyState::Server &server) {
			return server.id == id;
		});
	if (removed == 0) {
		return;
	}
	if (state.selectedId && *state.selectedId == id) {
		state.selectedId.reset();
	}
	saveImpulse(std::move(state));
}

void ProxiesBoxController::deleteImpulseSubscription(const std::string &id) {
	Impulse::ProxyState::State state = _settings.impulseProxyState();
	const auto found = std::find_if(
		state.subscriptions.begin(),
		state.subscriptions.end(),
		[&](const Impulse::ProxyState::Subscription &subscription) {
			return subscription.id == id;
		});
	if (found == state.subscriptions.end()) {
		return;
	}
	if (state.selectedId) {
		const std::string selected = *state.selectedId;
		const bool selectedInside = std::any_of(
			found->servers.begin(),
			found->servers.end(),
			[&](const Impulse::ProxyState::Server &server) {
				return server.id == selected;
			});
		if (selectedInside) {
			state.selectedId.reset();
		}
	}
	state.subscriptions.erase(found);
	saveImpulse(std::move(state));
}

void ProxiesBoxController::selectImpulseServer(const std::string &id) {
	Impulse::ProxyState::State state = _settings.impulseProxyState();
	if (state.selectedId && *state.selectedId == id) {
		return;
	} else if (!ImpulseHasServer(state, id)) {
		return;
	}
	state.selectedId = id;
	saveImpulse(std::move(state));
}

void ProxiesBoxController::shareImpulseServer(const std::string &id, bool qr) {
	const Impulse::ProxyState::State &state = _settings.impulseProxyState();
	const auto found = std::find_if(
		state.manual.begin(),
		state.manual.end(),
		[&](const Impulse::ProxyState::Server &server) {
			return server.id == id;
		});
	if (found == state.manual.end() || !found->shareLink) {
		return;
	}
	ShareImpulseLink(_show, QString::fromStdString(*found->shareLink), qr);
}

void ProxiesBoxController::shareImpulseList() {
	const Impulse::ProxyState::State &state = _settings.impulseProxyState();
	QString text;
	for (const Impulse::ProxyState::Server &server : state.manual) {
		if (!server.shareLink) {
			continue;
		}
		if (!text.isEmpty()) {
			text += '\n';
		}
		text += QString::fromStdString(*server.shareLink);
	}
	ShareImpulseLink(_show, text, false);
}

void ProxiesBoxController::checkImpulseServers() {
	if (impulseCheckRunning) {
		return;
	}
	const Impulse::ProxyState::State state = _settings.impulseProxyState();
	const std::vector<Impulse::ProxyState::Server> servers
		= ImpulseAllServers(state);
	if (servers.empty()) {
		return;
	}
	impulseCheckRunning = true;
	impulseChangeStream.fire({});
	const base::weak_ptr<ProxiesBoxController> weak = base::make_weak(this);
	Core::App().impulseProxy().checkServers(
		state,
		servers,
		[=](std::vector<Impulse::Xray::PingResult> results) {
			if (ProxiesBoxController *const that = weak.get()) {
				that->finishImpulseCheck(servers, results);
			}
		});
}

bool ProxiesBoxController::impulseChecking() const {
	return impulseCheckRunning;
}

std::optional<std::int64_t> ProxiesBoxController::impulsePing(
		const std::string &id) const {
	const auto found = impulsePingResults.find(id);
	if (found == impulsePingResults.end()) {
		return std::nullopt;
	}
	return found->second;
}

rpl::producer<> ProxiesBoxController::impulseChanges() const {
	return impulseChangeStream.events();
}

void ProxiesBoxController::saveImpulse(Impulse::ProxyState::State state) {
	_settings.setImpulseProxyState(std::move(state));
	Core::App().saveSettingsDelayed();
	Core::App().refreshGlobalProxy();
	impulseChangeStream.fire({});
}

void ProxiesBoxController::addImpulseServers(
		std::vector<Impulse::ProxyState::Server> servers) {
	if (servers.empty()) {
		showImpulseToast(
			tr::lng_proxy_add_from_clipboard_failed_toast(tr::now));
		return;
	}
	Impulse::ProxyState::State state = _settings.impulseProxyState();
	int added = 0;
	for (Impulse::ProxyState::Server &server : servers) {
		if (ImpulseHasServer(state, server.id)) {
			continue;
		}
		state.manual.push_back(std::move(server));
		++added;
	}
	if (added == 0) {
		showImpulseToast(
			tr::lng_proxy_add_from_clipboard_existing_toast(tr::now));
		return;
	}
	saveImpulse(std::move(state));
	showImpulseToast(tr::lng_impulse_proxy_list_added(
		tr::now,
		lt_count,
		added));
}

void ProxiesBoxController::fetchImpulseSubscription(
		const std::string &url,
		const std::string &id) {
	const base::weak_ptr<ProxiesBoxController> weak = base::make_weak(this);
	Core::App().impulseProxy().refreshSubscription(
		url,
		[=](Impulse::SubscriptionRefresh result) {
			if (ProxiesBoxController *const that = weak.get()) {
				that->applyImpulseSubscription(url, id, std::move(result));
			}
		});
}

void ProxiesBoxController::applyImpulseSubscription(
		const std::string &url,
		const std::string &id,
		Impulse::SubscriptionRefresh result) {
	Impulse::ProxyState::State state = _settings.impulseProxyState();
	const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
	const auto found = std::find_if(
		state.subscriptions.begin(),
		state.subscriptions.end(),
		[&](const Impulse::ProxyState::Subscription &subscription) {
			return subscription.id == id;
		});
	if (!result.error.empty()) {
		// A failed refresh keeps the servers of the subscription; a first
		// failed download is not stored at all.
		if (found != state.subscriptions.end()) {
			found->lastError = result.error;
			found->updatedAt = now;
			saveImpulse(std::move(state));
		}
		showImpulseToast(ImpulseFetchError(result.error));
		return;
	}
	const int count = int(result.servers.size());
	Impulse::ProxyState::Subscription next;
	next.id = id;
	next.url = url;
	next.meta.title = result.meta.title;
	next.meta.upload = result.meta.upload;
	next.meta.download = result.meta.download;
	next.meta.total = result.meta.total;
	next.meta.expire = result.meta.expire;
	next.meta.updateIntervalHours = result.meta.updateIntervalHours;
	next.updatedAt = now;
	next.skipped = result.skipped;
	next.servers = std::move(result.servers);
	if (found != state.subscriptions.end()) {
		*found = std::move(next);
	} else {
		state.subscriptions.push_back(std::move(next));
	}
	saveImpulse(std::move(state));
	showImpulseToast(tr::lng_impulse_proxy_list_added(
		tr::now,
		lt_count,
		count));
}

void ProxiesBoxController::finishImpulseCheck(
		const std::vector<Impulse::ProxyState::Server> &servers,
		const std::vector<Impulse::Xray::PingResult> &results) {
	impulsePingResults.clear();
	for (std::size_t i = 0; i < servers.size() && i < results.size(); ++i) {
		impulsePingResults[servers[i].id] = results[i].delay;
	}
	impulseCheckRunning = false;
	impulseChangeStream.fire({});
}

void ProxiesBoxController::showImpulseToast(const QString &text) {
	ShowImpulseToast(_show, text);
}

void ProxiesBoxController::Show(
		not_null<Window::SessionController*> controller,
		const QString &highlightId) {
	controller->show(
		CreateOwningBox(&controller->session().account(), highlightId));
}

ProxiesBoxController::~ProxiesBoxController() {
	if (_saveTimer.isActive()) {
		base::call_delayed(
			kSaveSettingsDelayedTimeout,
			QCoreApplication::instance(),
			[] { Local::writeSettings(); });
	}
}
