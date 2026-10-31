/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/connection_box.h"

#include "base/qt/qt_key_modifiers.h"
#include "base/qthelp_regex.h"
#include "base/qthelp_url.h"
#include "base/weak_ptr.h"
#include "boxes/impulse_proxy_advanced_box.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/impulse_proxy/proxy_ping.h"
#include "core/impulse_proxy/proxy_service.h"
#include "core/local_url_handlers.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/facade.h"
#include "mtproto/impulse/core/proxy_input.h"
#include "mtproto/impulse/core/proxy_routing.h"
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
#include "ui/text/format_values.h"
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
#include "ui/widgets/tooltip.h"
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
#include <QtCore/QUrl>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtWidgets/QTextEdit>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Automatic check rounds run at most this often while the box is open.
constexpr auto kImpulseAutoCheckMs = crl::time(60000);

// The Add box field grows up to this many lines, then it scrolls.
constexpr auto kImpulseAddMaxLines = 4;

using ProxyData = MTP::ProxyData;

struct TopButtonMenuState {
	~TopButtonMenuState() {
		closing = true;
	}

	bool closing = false;
	base::unique_qptr<Ui::PopupMenu> menu;
};

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
	return queryPath.isEmpty() ? QString() : (u"im://"_q + queryPath);
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
	return QColor(0x20, 0x85, 0xDF); // ImpulseM brand disc, as in the Android and web logo.
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

// The text of a core or subscription error code, as the proxy list shows it.
// An empty code means that the core is not running. A code that is not named
// here shows the generic core error.
[[nodiscard]] QString ImpulseErrorText(const std::string &code) {
	namespace Xray = Impulse::Xray;
	const std::string_view view = code;
	const std::string_view http = Impulse::ProxyInput::kErrorHttp;
	const std::string_view noLinks = Impulse::ProxyInput::kErrorNoLinks;
	const std::string_view notFound = "err:subscriptionNotFound";
	if (view.empty()) {
		return tr::lng_impulse_proxy_error_not_running(tr::now);
	} else if (view == Xray::ErrorCode(Xray::Error::NoServer)) {
		return tr::lng_impulse_proxy_error_no_server(tr::now);
	} else if (view == Xray::ErrorCode(Xray::Error::InvalidConfig)) {
		return tr::lng_impulse_proxy_error_invalid_config(tr::now);
	} else if (view == Xray::ErrorCode(Xray::Error::CoreStart)) {
		return tr::lng_impulse_proxy_error_core_start(tr::now);
	} else if (view == Xray::ErrorCode(Xray::Error::CoreStopped)) {
		return tr::lng_impulse_proxy_error_core_stopped(tr::now);
	} else if (view == Impulse::ProxyInput::kErrorInvalidSubscription) {
		return tr::lng_impulse_proxy_error_invalid_subscription(tr::now);
	} else if (view == notFound) {
		return tr::lng_impulse_proxy_error_subscription_not_found(tr::now);
	} else if (view == Impulse::ProxyInput::kErrorNetwork) {
		return tr::lng_impulse_proxy_error_network(tr::now);
	} else if (view.starts_with(std::string(http) + ':')) {
		return tr::lng_impulse_proxy_error_http(
			tr::now,
			lt_code,
			QString::fromStdString(code.substr(http.size() + 1)));
	} else if (view == noLinks) {
		return tr::lng_impulse_proxy_error_no_links(tr::now);
	} else if (view.starts_with(std::string(noLinks) + ':')) {
		const int skipped = QString::fromStdString(
			code.substr(noLinks.size() + 1)).toInt();
		return tr::lng_impulse_proxy_error_no_links_skipped(
			tr::now,
			lt_skipped,
			QString::number(skipped));
	}
	return tr::lng_impulse_proxy_error_core(tr::now);
}

// The first server of the list, which the proxy picks when it is turned on.
[[nodiscard]] std::optional<std::string> ImpulseFirstServerId(
		const Impulse::ProxyState::State &state) {
	const auto all = Impulse::ProxyState::AllServers(state);
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
		const QString &link) {
	if (link.isEmpty() || !show) {
		return;
	}
	TextUtilities::SetClipboardText(TextForMimeData::Simple(link));
	show->showToast({
		.text = { tr::lng_impulse_proxy_link_copied(tr::now) },
		.iconLottie = u"toast/copy"_q,
		.iconLottieSize = st::toastLottieIconSize,
	});
}

// A box with a field for vless:// links, several of them, or a subscription
// URL. The text goes to the controller, which converts it. A subscription
// keeps the box open with a loading state until it is downloaded. Cancel, or
// closing the box, drops the result that comes later.
void ShowImpulseAddBox(
		const std::shared_ptr<Ui::Show> &show,
		not_null<ProxiesBoxController*> controller) {
	const base::weak_ptr<ProxiesBoxController> weak = base::make_weak(
		controller.get());
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_impulse_proxy_add_title());
		const auto weakBox = base::make_weak(box.get());

		const auto inner = box->verticalLayout();
		const auto field = inner->add(
			object_ptr<Ui::InputField>(
				inner,
				st::defaultInputField,
				Ui::InputField::Mode::MultiLine,
				tr::lng_impulse_proxy_add_hint(),
				QString()),
			st::boxRowPadding);
		const auto lineHeight = st::defaultInputField.style.font->height;
		field->setMaxHeight(lineHeight * kImpulseAddMaxLines
			+ st::defaultInputField.textMargins.top()
			+ st::defaultInputField.textMargins.bottom());

		const auto paste = inner->add(
			object_ptr<Ui::LinkButton>(
				inner,
				tr::lng_impulse_proxy_paste(tr::now)),
			st::boxRowPadding);
		paste->setClickedCallback([=] {
			field->setText(QGuiApplication::clipboard()->text().trimmed());
			field->setFocusFast();
		});

		const auto errorLabel = inner->add(
			object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
				inner,
				object_ptr<Ui::FlatLabel>(
					inner,
					QString(),
					st::boxDividerLabel)),
			st::boxRowPadding);
		errorLabel->entity()->setTextColorOverride(st::boxTextFgError->c);
		errorLabel->hide(anim::type::instant);

		const auto showError = [=](const QString &message) {
			errorLabel->entity()->setText(message);
			errorLabel->toggle(true, anim::type::normal);
			field->showError();
		};
		field->changes() | rpl::on_next([=] {
			errorLabel->toggle(false, anim::type::normal);
			field->hideError();
		}, field->lifetime());

		box->setFocusCallback([=] { field->setFocusFast(); });

		const auto submitButton = box->addButton(
			tr::lng_impulse_proxy_add_submit(),
			[] {});
		const auto setBusy = [=](bool busy) {
			field->setEnabled(!busy);
			submitButton->setDisabled(busy);
			box->showLoading(busy);
		};
		const auto submit = [=] {
			const auto text = field->getLastText().trimmed();
			if (text.isEmpty()) {
				showError(tr::lng_impulse_proxy_invalid(tr::now));
				return;
			}
			ProxiesBoxController *const strong = weak.get();
			if (!strong) {
				box->closeBox();
				return;
			}
			setBusy(true);
			strong->importImpulseText(
				text,
				[=] { return weakBox.get() != nullptr; },
				[=](QString message) {
					const auto stillOpen = weakBox.get();
					if (!stillOpen) {
						return;
					} else if (message.isEmpty()) {
						stillOpen->closeBox();
						return;
					}
					setBusy(false);
					showError(message);
				});
		};
		submitButton->setClickedCallback(submit);
		field->submits() | rpl::on_next(submit, field->lifetime());
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

class ProxyRow : public Ui::RippleButton {
public:
	using View = ProxiesBoxController::ItemView;
	using State = ProxiesBoxController::ItemState;

	ProxyRow(QWidget *parent, View &&view);

	void updateFields(View &&view);

	rpl::producer<> detailsClicks() const;

protected:
	int resizeGetHeight(int newWidth) override;

	void paintEvent(QPaintEvent *e) override;

private:
	void setupControls(View &&view);
	int countAvailableWidth() const;
	void radialAnimationCallback();
	void paintCheck(Painter &p);

	View _view;

	Ui::Text::String _title;
	object_ptr<Ui::IconButton> _menuToggle;
	rpl::event_stream<> _detailsClicks;

	bool _set = false;
	Ui::Animations::Simple _toggled;
	Ui::Animations::Simple _setAnimation;
	std::unique_ptr<Ui::InfiniteRadialAnimation> _progress;
	std::unique_ptr<Ui::InfiniteRadialAnimation> _checking;

	int _skipLeft = 0;
	int _skipRight = 0;

};

// The ImpulseM VLESS list, as Android ImpulseProxyActivity lays it out: the
// use switch with its status, the advanced settings, my links, and one block
// per subscription. Rows are rebuilt from the proxy state whenever the list,
// the core status, the route or a check changes.
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
	void setupContent();
	void setupTopButton();
	void addNewProxy();
	void refreshAll();
	void refreshUse();
	void refreshStatus();
	void rebuildRows();
	void addServerRow(
		not_null<Ui::VerticalLayout*> wrap,
		const Impulse::ProxyState::State &state,
		const Impulse::ProxyState::Server &server,
		bool manual);
	void addSubscription(
		not_null<Ui::VerticalLayout*> wrap,
		const Impulse::ProxyState::State &state,
		const Impulse::ProxyState::Subscription &subscription);
	int rowHeight() const;

	not_null<ProxiesBoxController*> _controller;
	Core::SettingsProxy &_settings;
	rpl::variable<bool> _useValue{ false };
	QPointer<Ui::FlatLabel> _status;
	QPointer<Ui::VerticalLayout> _wrap;

	QPointer<Ui::RpWidget> _addProxyButton;
	QString _highlightId;

};

[[nodiscard]] QString ImpulseEndpointText(const Impulse::ProxyState::Server &server) {
	return QString::fromStdString(server.host)
		+ ':'
		+ QString::number(server.port);
}

// The name of a server, or its host and port when the name is empty.
[[nodiscard]] QString ImpulseServerName(const Impulse::ProxyState::Server &server) {
	const auto name = QString::fromStdString(server.name).trimmed();
	return name.isEmpty() ? ImpulseEndpointText(server) : name;
}

// The security and the network of a server, as the details show them, for
// example "REALITY · TCP". Xray's raw network is TCP.
[[nodiscard]] QString ImpulseTransportText(const Impulse::ProxyState::Server &server) {
	const auto security = QString::fromStdString(server.security);
	const auto network = QString::fromStdString(server.network);
	const auto networkName = (network == u"raw"_q)
		? u"TCP"_q
		: network.toUpper();
	const auto secured = !security.isEmpty() && security != u"none"_q;
	if (!secured) {
		return networkName;
	} else if (networkName.isEmpty()) {
		return security.toUpper();
	}
	return security.toUpper()
		+ u" "_q + QChar(0x00B7) + u" "_q
		+ networkName;
}

// The title of a subscription: the name from its panel, then the host of its
// URL, then the URL itself.
[[nodiscard]] QString ImpulseSubscriptionTitle(
		const Impulse::ProxyState::Subscription &subscription) {
	if (subscription.meta.title && !subscription.meta.title->empty()) {
		return QString::fromStdString(*subscription.meta.title);
	}
	const auto host = QUrl(QString::fromStdString(subscription.url)).host();
	return host.isEmpty()
		? QString::fromStdString(subscription.url)
		: host;
}

// Whether the main traffic goes through the core now. Only then the selected
// row and the status say "Connected".
[[nodiscard]] bool ImpulseConnected(const Impulse::ProxyState::State &state) {
	return Impulse::ProxyRouting::Decide(
		state.enabled,
		state.useForTransport,
		Core::App().impulseXrayRunning()) == Impulse::ProxyRouting::Route::Proxy;
}

[[nodiscard]] bool ImpulseServerInUse(
		const Impulse::ProxyState::State &state,
		const std::string &id) {
	return state.enabled && state.selectedId && *state.selectedId == id;
}

[[nodiscard]] bool ImpulseSubscriptionInUse(
		const Impulse::ProxyState::State &state,
		const Impulse::ProxyState::Subscription &subscription) {
	if (!state.enabled || !state.selectedId) {
		return false;
	}
	return std::any_of(
		subscription.servers.begin(),
		subscription.servers.end(),
		[&](const Impulse::ProxyState::Server &server) {
			return server.id == *state.selectedId;
		});
}

// Confirms the removal of a manual server. The warning is added when the proxy
// uses the server, as Android confirmDelete does.
void ShowImpulseDeleteServer(
		const std::shared_ptr<Ui::Show> &show,
		const base::weak_ptr<ProxiesBoxController> &controller,
		const std::string &id,
		const QString &name,
		bool inUse) {
	auto text = tr::lng_impulse_proxy_delete_server_text(
		tr::now,
		lt_name,
		name);
	if (inUse) {
		text += u"\n\n"_q
			+ tr::lng_impulse_proxy_delete_active_server(tr::now);
	}
	show->showBox(Ui::MakeConfirmBox({
		.text = text,
		.confirmed = [=] {
			if (ProxiesBoxController *const strong = controller.get()) {
				strong->deleteImpulseServer(id);
			}
		},
		.confirmText = tr::lng_impulse_proxy_delete(),
		.confirmStyle = &st::attentionBoxButton,
		.title = tr::lng_impulse_proxy_delete_server_title(),
	}));
}

// Confirms the removal of a subscription and all its servers.
void ShowImpulseDeleteSubscription(
		const std::shared_ptr<Ui::Show> &show,
		const base::weak_ptr<ProxiesBoxController> &controller,
		const std::string &id,
		const QString &title,
		bool inUse) {
	auto text = tr::lng_impulse_proxy_delete_subscription_text(
		tr::now,
		lt_title,
		title);
	if (inUse) {
		text += u"\n\n"_q
			+ tr::lng_impulse_proxy_delete_active_subscription(tr::now);
	}
	show->showBox(Ui::MakeConfirmBox({
		.text = text,
		.confirmed = [=] {
			if (ProxiesBoxController *const strong = controller.get()) {
				strong->deleteImpulseSubscription(id);
			}
		},
		.confirmText = tr::lng_impulse_proxy_delete(),
		.confirmStyle = &st::attentionBoxButton,
		.title = tr::lng_impulse_proxy_delete_subscription_title(),
	}));
}

// The details of one server: its endpoint and transport, the copy of its link
// and, for a manual server, its removal. Android showServerInfo.
void ShowImpulseServerDetails(
		const std::shared_ptr<Ui::Show> &show,
		not_null<ProxiesBoxController*> controller,
		const std::string &id,
		bool manual) {
	const std::optional<Impulse::ProxyState::Server> found
		= controller->impulseServer(id);
	if (!found) {
		return;
	}
	const Impulse::ProxyState::Server server = *found;
	const base::weak_ptr<ProxiesBoxController> weak = base::make_weak(
		controller.get());
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(ImpulseServerName(server)));

		const auto inner = box->verticalLayout();
		inner->add(
			object_ptr<Ui::FlatLabel>(
				inner,
				ImpulseEndpointText(server)
					+ '\n'
					+ ImpulseTransportText(server),
				st::boxLabel),
			st::boxRowPadding);

		if (server.shareLink) {
			const QString link = QString::fromStdString(*server.shareLink);
			box->addLeftButton(tr::lng_impulse_proxy_copy_link(), [=] {
				ShareImpulseLink(show, link);
			});
		}
		box->addButton(tr::lng_impulse_proxy_close(), [=] {
			box->closeBox();
		});
		if (manual) {
			box->addButton(tr::lng_impulse_proxy_delete(), [=] {
				box->closeBox();
				ProxiesBoxController *const strong = weak.get();
				if (!strong) {
					return;
				}
				ShowImpulseDeleteServer(
					show,
					weak,
					id,
					ImpulseServerName(server),
					ImpulseServerInUse(strong->impulseState(), id));
			}, st::attentionBoxButton);
		}
	}));
}

ProxyRow::ProxyRow(QWidget *parent, View &&view)
: RippleButton(parent, st::proxyRowRipple)
, _menuToggle(this, st::topBarMenuToggle) {
	setupControls(std::move(view));
}

rpl::producer<> ProxyRow::detailsClicks() const {
	return _detailsClicks.events();
}

void ProxyRow::setupControls(View &&view) {
	updateFields(std::move(view));
	_toggled.stop();
	_setAnimation.stop();

	_menuToggle->addClickHandler([=] { _detailsClicks.fire({}); });
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
	_title.setMarkedText(
		st::proxyRowTitleStyle,
		TextWithEntities().append(_view.name),
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

	setPointerCursor(true);

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

	paintRipple(p, 0, 0);

	const auto left = _skipLeft;
	const auto availableWidth = countAvailableWidth();
	auto top = st::proxyRowPadding.top();

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
		case State::Available:
			return st::proxyRowStatusFgAvailable;
		default:
			return st::proxyRowStatusFg;
		}
	}();
	const auto status = [&] {
		switch (_view.state) {
		case State::Available:
			return tr::lng_impulse_proxy_available(
				tr::now,
				lt_ping,
				QString::number(_view.ping));
		case State::Checking:
			return tr::lng_impulse_proxy_checking(tr::now);
		case State::NotTested:
			return tr::lng_impulse_proxy_not_checked(tr::now);
		case State::Connecting:
			return tr::lng_impulse_proxy_connecting(tr::now);
		case State::Online:
			return (_view.ping >= 0)
				? tr::lng_impulse_proxy_connected_ping(
					tr::now,
					lt_ping,
					QString::number(_view.ping))
				: tr::lng_impulse_proxy_status_connected(tr::now);
		case State::Unavailable:
			return tr::lng_impulse_proxy_unavailable(tr::now);
		}
		Unexpected("State in ProxyRow::paintEvent.");
	}();
	p.setPen(statusFg);
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
		const auto show = uiShow();
		_controller->importImpulseText(
			QGuiApplication::clipboard()->text(),
			[] { return true; },
			[=](QString message) {
				if (!message.isEmpty()) {
					ShowImpulseToast(show, message);
				}
			});
	} else {
		BoxContent::keyPressEvent(e);
	}
}

void ProxiesBox::prepare() {
	setTitle(tr::lng_impulse_proxy_title());

	addButton(tr::lng_impulse_proxy_close(), [=] { closeBox(); });

	setupTopButton();
	setupContent();
	_controller->startImpulseAutoChecks();
}

void ProxiesBox::showFinished() {
	if (_highlightId == u"proxy/add-proxy"_q) {
		if (_addProxyButton) {
			_highlightId = QString();
			Settings::HighlightWidget(
				_addProxyButton,
				{ .rippleShape = true });
		}
	}
}

void ProxiesBox::setupTopButton() {
	const auto top = addTopButton(st::proxyCheckTopButton);
	Ui::InstallTooltip(top.data(), [] {
		return tr::lng_impulse_proxy_list_check(tr::now);
	});
	const not_null<ProxiesBoxController*> controller = _controller;
	top->setClickedCallback([=] {
		controller->checkImpulseServers();
	});
}

void ProxiesBox::setupContent() {
	const auto inner = setInnerWidget(object_ptr<Ui::VerticalLayout>(this));
	const not_null<ProxiesBoxController*> controller = _controller;

	// The switch follows the state. A tap that the rules refuse is undone by
	// setting the value again, which the switch shows as a change back.
	Ui::SettingsButton *const use = inner->add(
		object_ptr<Ui::SettingsButton>(
			inner,
			tr::lng_impulse_proxy_use(),
			st::settingsButtonNoIcon));
	use->toggleOn(_useValue.value());
	use->toggledChanges() | rpl::on_next([=](bool checked) {
		const Impulse::ProxyState::State state = controller->impulseState();
		if (checked == state.enabled) {
			return;
		} else if (checked && Impulse::ProxyState::AllServers(state).empty()) {
			// Android opens the Add box when there is no server at all, and
			// the switch stays off.
			_useValue.force_assign(false);
			ShowImpulseAddBox(uiShow(), controller);
			return;
		}
		controller->setImpulseEnabled(checked);
	}, use->lifetime());

	_status = inner->add(
		object_ptr<Ui::FlatLabel>(
			inner,
			QString(),
			st::defaultFlatLabel),
		st::proxyUsePadding);

	Settings::AddButtonWithIcon(
		inner,
		tr::lng_impulse_proxy_list_advanced(),
		st::settingsButton
	)->setClickedCallback([=] {
		ShowImpulseProxyAdvancedBox(uiShow());
	});
	Ui::AddSkip(inner);

	_wrap = inner->add(object_ptr<Ui::VerticalLayout>(inner));

	// The rows, the status and the switch are rebuilt after the event that
	// changed them, because that event can come from one of the rows, which
	// must not be deleted in its handler.
	_controller->impulseChanges(
	) | rpl::on_next([=] {
		Ui::PostponeCall(this, [=] {
			refreshAll();
		});
	}, inner->lifetime());
	Core::App().impulseProxy().stateValue(
	) | rpl::on_next([=](Impulse::Xray::Status) {
		Ui::PostponeCall(this, [=] {
			refreshAll();
		});
	}, inner->lifetime());
	Core::App().impulseProxy().errorValue(
	) | rpl::on_next([=](Impulse::Xray::Error) {
		Ui::PostponeCall(this, [=] {
			refreshAll();
		});
	}, inner->lifetime());
	Core::App().impulseProxy().routeValue(
	) | rpl::on_next([=](Impulse::ProxyRouting::Route) {
		Ui::PostponeCall(this, [=] {
			refreshAll();
		});
	}, inner->lifetime());
	// A round can finish after the box was reopened, so the rows follow the
	// service and not the controller that started the round.
	Core::App().impulseProxy().checkFinishedValue(
	) | rpl::on_next([=] {
		Ui::PostponeCall(this, [=] {
			refreshAll();
		});
	}, inner->lifetime());

	refreshAll();

	inner->resizeToWidth(st::boxWideWidth);

	inner->heightValue(
	) | rpl::map([=](int height) {
		return std::min(
			std::max(height, 3 * rowHeight()),
			st::boxMaxListHeight);
	}) | rpl::distinct_until_changed(
	) | rpl::on_next([=](int height) {
		setDimensions(st::boxWideWidth, height);
	}, inner->lifetime());
}

void ProxiesBox::refreshAll() {
	refreshUse();
	refreshStatus();
	rebuildRows();
}

void ProxiesBox::refreshUse() {
	_useValue = _settings.impulseProxyState().enabled;
}

// The line under the switch, from the state and the core, as Android
// overallStatusItem does.
void ProxiesBox::refreshStatus() {
	if (!_status) {
		return;
	}
	const Impulse::ProxyState::State state = _settings.impulseProxyState();
	auto text = QString();
	const style::color *color = &st::windowSubTextFg;
	const Impulse::Xray::Error error = Core::App().impulseProxy().error();
	if (!state.enabled) {
		text = tr::lng_impulse_proxy_status_off(tr::now);
	} else if (error != Impulse::Xray::Error::None) {
		text = tr::lng_impulse_proxy_status_error(
			tr::now,
			lt_reason,
			ImpulseErrorText(std::string(Impulse::Xray::ErrorCode(error))));
		color = &st::boxTextFgError;
	} else if (ImpulseConnected(state)) {
		text = tr::lng_impulse_proxy_status_connected(tr::now);
		color = &st::windowActiveTextFg;
	} else {
		text = tr::lng_impulse_proxy_connecting(tr::now);
	}
	_status->setText(text);
	_status->setTextColorOverride((*color)->c);
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

	const Impulse::ProxyState::State state = _settings.impulseProxyState();
	Ui::AddSubsectionTitle(wrap, tr::lng_impulse_proxy_my_links());
	for (const Impulse::ProxyState::Server &server
			: _controller->impulseSortedBlock(state.manual)) {
		addServerRow(wrap, state, server, true);
	}
	Settings::Button *const addProxy = Settings::AddButtonWithIcon(
		wrap,
		tr::lng_impulse_proxy_add_title(),
		st::settingsButton,
		{ &st::menuIconAdd });
	addProxy->setClickedCallback([=] {
		addNewProxy();
	});
	_addProxyButton = addProxy;

	Ui::AddDivider(wrap);
	for (const Impulse::ProxyState::Subscription &subscription
			: state.subscriptions) {
		addSubscription(wrap, state, subscription);
	}
	wrap->resizeToWidth(width());
}

void ProxiesBox::addServerRow(
		not_null<Ui::VerticalLayout*> wrap,
		const Impulse::ProxyState::State &state,
		const Impulse::ProxyState::Server &server,
		bool manual) {
	const std::optional<std::int64_t> ping = _controller->impulsePing(
		server.id);
	const bool selected = state.selectedId.has_value()
		&& *state.selectedId == server.id;

	ProxyRow::View view;
	view.name = ImpulseServerName(server);
	view.selected = selected;
	view.ping = int(ping.value_or(-1));
	if (state.enabled && selected) {
		view.state = ImpulseConnected(state)
			? ProxyRow::State::Online
			: ProxyRow::State::Connecting;
	} else if (_controller->impulseServerChecking(server.id)) {
		view.state = ProxyRow::State::Checking;
	} else if (ping.has_value()) {
		view.state = (*ping >= 0)
			? ProxyRow::State::Available
			: ProxyRow::State::Unavailable;
	} else {
		view.state = ProxyRow::State::NotTested;
	}

	const not_null<ProxiesBoxController*> controller = _controller;
	const std::string id = server.id;
	ProxyRow *const row = wrap->add(
		object_ptr<ProxyRow>(wrap, std::move(view)));
	row->clicks(
	) | rpl::on_next([=] {
		controller->selectImpulseServer(id);
	}, row->lifetime());
	row->detailsClicks(
	) | rpl::on_next([=] {
		ShowImpulseServerDetails(uiShow(), controller, id, manual);
	}, row->lifetime());
}

void ProxiesBox::addSubscription(
		not_null<Ui::VerticalLayout*> wrap,
		const Impulse::ProxyState::State &state,
		const Impulse::ProxyState::Subscription &subscription) {
	const not_null<ProxiesBoxController*> controller = _controller;
	const std::string id = subscription.id;
	const auto &meta = subscription.meta;

	Ui::AddSubsectionTitle(
		wrap,
		rpl::single(ImpulseSubscriptionTitle(subscription)));
	for (const Impulse::ProxyState::Server &server
			: _controller->impulseSortedBlock(subscription.servers)) {
		addServerRow(wrap, state, server, false);
	}

	Settings::Button *const refresh = Settings::AddButtonWithIcon(
		wrap,
		tr::lng_impulse_proxy_refresh(),
		st::settingsButton,
		{ &st::menuIconRestore });
	refresh->setDisabled(_controller->impulseRefreshing(id));
	refresh->setClickedCallback([=] {
		controller->refreshImpulseSubscription(id);
	});

	Settings::Button *const deleteButton = Settings::AddButtonWithIcon(
		wrap,
		tr::lng_impulse_proxy_delete(),
		st::settingsAttentionButton);
	deleteButton->setClickedCallback([=] {
		ShowImpulseDeleteSubscription(
			uiShow(),
			base::make_weak(controller.get()),
			id,
			ImpulseSubscriptionTitle(subscription),
			ImpulseSubscriptionInUse(state, subscription));
	});

	// The info lines, one each, as Android subscriptionInfo builds them.
	const bool hasTotal = meta.total > 0;
	const bool hasExpire = meta.expire > 0;
	const auto expires = QDateTime::fromSecsSinceEpoch(meta.expire).date();
	auto lines = QStringList();
	if (hasTotal) {
		const auto used = std::max<std::int64_t>(meta.upload, 0)
			+ std::max<std::int64_t>(meta.download, 0);
		const auto usedText = Ui::FormatSizeText(used);
		const auto totalText = Ui::FormatSizeText(meta.total);
		lines.push_back(hasExpire
			? tr::lng_impulse_proxy_traffic(
				tr::now,
				lt_used,
				usedText,
				lt_total,
				totalText,
				lt_date,
				langDayOfMonthFull(expires))
			: tr::lng_impulse_proxy_traffic_no_expiry(
				tr::now,
				lt_used,
				usedText,
				lt_total,
				totalText));
	} else if (hasExpire) {
		lines.push_back(langDayOfMonthFull(expires));
	}
	if (subscription.skipped > 0) {
		lines.push_back(tr::lng_impulse_proxy_skipped(
			tr::now,
			lt_count,
			subscription.skipped));
	}
	if (subscription.updatedAt > 0) {
		lines.push_back(tr::lng_impulse_proxy_updated(
			tr::now,
			lt_date,
			langDateTime(QDateTime::fromMSecsSinceEpoch(
				subscription.updatedAt))));
	}
	if (!lines.isEmpty()) {
		Ui::AddDividerText(wrap, rpl::single(lines.join(u"\n"_q)));
	}
	if (subscription.lastError) {
		const auto error = Ui::AddDividerText(
			wrap,
			rpl::single(ImpulseErrorText(*subscription.lastError)));
		error->setTextColorOverride(st::boxTextFgError->c);
	}
}

using Connection = MTP::details::AbstractConnection;
using Checker = MTP::ProxyCheckConnection;

} // namespace

ProxiesBoxController::ProxiesBoxController(not_null<Main::Account*> account)
: _account(account)
, _settings(Core::App().settings().proxy()) {
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

void ProxiesBoxController::importImpulseText(
		const QString &text,
		ImpulseImportAlive alive,
		ImpulseImportDone done) {
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
				ProxiesBoxController *const that = weak.get();
				if (!that || !alive()) {
					return;
				}
				done(that->addImpulseServers(std::move(servers)));
			});
	} else if (kind == Impulse::ProxyInput::InputKind::Subscription) {
		const std::string url(Impulse::ProxyInput::Trim(utf8));
		fetchImpulseSubscription(
			url,
			Impulse::ProxyInput::SubscriptionId(url),
			std::move(alive),
			std::move(done));
	} else {
		done(tr::lng_impulse_proxy_invalid(tr::now));
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
	fetchImpulseSubscription(
		found->url,
		id,
		[] { return true; },
		[=](QString message) {
			if (!message.isEmpty()) {
				showImpulseToast(message);
			}
		});
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
	} else if (!Impulse::ProxyState::HasServer(state, id)) {
		return;
	}
	state.selectedId = id;
	saveImpulse(std::move(state));
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
	ShareImpulseLink(_show, text);
}

void ProxiesBoxController::checkImpulseServers() {
	requestImpulseCheck(true);
}

// Android runs a round when the box opens, if the last one is older than a
// minute, and then once a minute while the box is open.
void ProxiesBoxController::startImpulseAutoChecks() {
	const std::int64_t last = Core::App().impulseProxy().lastCheckTime();
	if (QDateTime::currentMSecsSinceEpoch() - last > kImpulseAutoCheckMs) {
		requestImpulseCheck(false);
	}
	_autoCheckTimer.setCallback([=] {
		requestImpulseCheck(false);
	});
	_autoCheckTimer.callEach(kImpulseAutoCheckMs);
}

void ProxiesBoxController::requestImpulseCheck(bool manual) {
	// The service keeps the round and the request that comes during it, so a
	// closed box does not lose them.
	if (Core::App().impulseProxy().requestCheck(manual)) {
		impulseChangeStream.fire({});
	}
}

const Impulse::ProxyState::State &ProxiesBoxController::impulseState() const {
	return _settings.impulseProxyState();
}

auto ProxiesBoxController::impulseServer(const std::string &id) const
-> std::optional<Impulse::ProxyState::Server> {
	for (const Impulse::ProxyState::Server &server
			: Impulse::ProxyState::AllServers(_settings.impulseProxyState())) {
		if (server.id == id) {
			return server;
		}
	}
	return std::nullopt;
}

bool ProxiesBoxController::impulseServerChecking(const std::string &id) const {
	return Core::App().impulseProxy().checkingServer(id);
}

bool ProxiesBoxController::impulseRefreshing(const std::string &id) const {
	return std::find(
		_refreshing.begin(),
		_refreshing.end(),
		id) != _refreshing.end();
}

std::vector<Impulse::ProxyState::Server> ProxiesBoxController::impulseSortedBlock(
		std::vector<Impulse::ProxyState::Server> block) const {
	const Impulse::ImpulseProxyService &service = Core::App().impulseProxy();
	std::stable_sort(
		block.begin(),
		block.end(),
		[&](
				const Impulse::ProxyState::Server &a,
				const Impulse::ProxyState::Server &b) {
			return service.sortKey(a.id) < service.sortKey(b.id);
		});
	return block;
}

std::optional<std::int64_t> ProxiesBoxController::impulsePing(
		const std::string &id) const {
	return Core::App().impulseProxy().pingResult(id);
}

rpl::producer<> ProxiesBoxController::impulseChanges() const {
	return impulseChangeStream.events();
}

void ProxiesBoxController::saveImpulse(Impulse::ProxyState::State state) {
	_settings.setImpulseProxyState(std::move(state));
	Core::App().saveSettingsDelayed();
	Core::App().refreshGlobalProxy();
	// The Connection type row reads the VLESS state, so it refreshes too.
	_settings.connectionTypeChangesNotify();
	impulseChangeStream.fire({});
}

QString ProxiesBoxController::addImpulseServers(
		std::vector<Impulse::ProxyState::Server> servers) {
	if (servers.empty()) {
		return tr::lng_impulse_proxy_invalid(tr::now);
	}
	Impulse::ProxyState::State state = _settings.impulseProxyState();
	int added = 0;
	for (Impulse::ProxyState::Server &server : servers) {
		if (Impulse::ProxyState::HasServer(state, server.id)) {
			continue;
		}
		state.manual.push_back(std::move(server));
		++added;
	}
	if (added == 0) {
		showImpulseToast(
			tr::lng_proxy_add_from_clipboard_existing_toast(tr::now));
		return QString();
	}
	saveImpulse(std::move(state));
	showImpulseToast(tr::lng_impulse_proxy_list_added(
		tr::now,
		lt_count,
		added));
	requestImpulseCheck(false);
	return QString();
}

void ProxiesBoxController::fetchImpulseSubscription(
		const std::string &url,
		const std::string &id,
		ImpulseImportAlive alive,
		ImpulseImportDone done) {
	if (!impulseRefreshing(id)) {
		_refreshing.push_back(id);
	}
	impulseChangeStream.fire({});
	const base::weak_ptr<ProxiesBoxController> weak = base::make_weak(this);
	Core::App().impulseProxy().refreshSubscription(
		url,
		[=](Impulse::SubscriptionRefresh result) {
			if (ProxiesBoxController *const that = weak.get()) {
				that->applyImpulseSubscription(
					url,
					id,
					alive,
					done,
					std::move(result));
			}
		});
}

void ProxiesBoxController::applyImpulseSubscription(
		const std::string &url,
		const std::string &id,
		ImpulseImportAlive alive,
		ImpulseImportDone done,
		Impulse::SubscriptionRefresh result) {
	std::erase(_refreshing, id);
	if (!alive()) {
		// The Add box was closed or cancelled: the result is dropped.
		impulseChangeStream.fire({});
		return;
	}
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
		} else {
			impulseChangeStream.fire({});
		}
		requestImpulseCheck(false);
		done(ImpulseErrorText(result.error));
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
	requestImpulseCheck(false);
	done(QString());
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