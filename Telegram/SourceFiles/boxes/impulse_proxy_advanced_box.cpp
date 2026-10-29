/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/impulse_proxy_advanced_box.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/wrap.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace {

using Advanced = Impulse::ProxyState::Advanced;

constexpr const char *kFragmentOff = "off";
constexpr const char *kFragmentClassic = "classic";
constexpr const char *kFragmentFinalMask = "finalmask";
constexpr const char *kPacketsDefault = "tlshello";
constexpr const char *kPacketsExample = "1-3";
constexpr const char *kDnsSystem = "system";
constexpr const char *kDnsCloudflare = "cloudflare";
constexpr const char *kDnsGoogle = "google";
constexpr const char *kDnsCustom = "custom";
constexpr const char *kPingBatch = "batch";
constexpr const char *kPingSequential = "sequential";
constexpr int kHttpsPrefixSize = 8;
constexpr int kMaxPort = 65535;
constexpr int kMaxKeepAlive = 86400;
constexpr int kMaxPingPause = 10000;
constexpr int kMaxPingTimeout = 30;
constexpr int kMaxMuxConcurrency = 128;
constexpr int kMinMss = 64;
constexpr int kMaxMss = 1460;

enum class NumberUnit {
	Plain,
	Millis,
	Seconds,
};

[[nodiscard]] const std::vector<std::string> &FragmentModes() {
	static const std::vector<std::string> modes = std::vector<std::string>{
		kFragmentOff,
		kFragmentClassic,
		kFragmentFinalMask,
	};
	return modes;
}

[[nodiscard]] const std::vector<std::string> &DnsModes() {
	static const std::vector<std::string> modes = std::vector<std::string>{
		kDnsSystem,
		kDnsCloudflare,
		kDnsGoogle,
		kDnsCustom,
	};
	return modes;
}

[[nodiscard]] const std::vector<std::string> &PingModes() {
	static const std::vector<std::string> modes = std::vector<std::string>{
		kPingBatch,
		kPingSequential,
	};
	return modes;
}

[[nodiscard]] const std::vector<std::string> &Fingerprints() {
	static const std::vector<std::string> fingerprints = std::vector<std::string>{
		"chrome",
		"firefox",
		"safari",
		"ios",
		"android",
		"edge",
		"360",
		"qq",
		"random",
		"randomized",
	};
	return fingerprints;
}

[[nodiscard]] int IndexOf(
		const std::vector<std::string> &values,
		const std::string &value) {
	for (int i = 0; i < int(values.size()); ++i) {
		if (values[i] == value) {
			return i;
		}
	}
	return 0;
}

[[nodiscard]] QString Text(std::string_view value) {
	return QString::fromUtf8(value.data(), int(value.size()));
}

// Digits only, within min..max; nothing when invalid.
[[nodiscard]] std::optional<int> ParseNumber(
		const QString &text,
		int min,
		int max) {
	if (text.isEmpty() || text.size() > 9) {
		return std::nullopt;
	}
	int value = 0;
	for (const QChar symbol : text) {
		const char16_t code = symbol.unicode();
		if (code < u'0' || code > u'9') {
			return std::nullopt;
		}
		value = value * 10 + (code - u'0');
	}
	if (value < min || value > max) {
		return std::nullopt;
	}
	return value;
}

// "N" or "N-M" with 0 <= N <= M <= 65535.
[[nodiscard]] bool IsRangeText(const QString &text) {
	const qsizetype dash = text.indexOf(u'-');
	if (dash < 0) {
		return ParseNumber(text, 0, kMaxPort).has_value();
	}
	const std::optional<int> low = ParseNumber(text.left(dash), 0, kMaxPort);
	const std::optional<int> high = ParseNumber(text.mid(dash + 1), 0, kMaxPort);
	return low && high && (*low <= *high);
}

[[nodiscard]] bool IsPacketsText(const QString &text) {
	return (text.toStdString() == kPacketsDefault) || IsRangeText(text);
}

[[nodiscard]] bool IsHttpsAddressText(const QString &text) {
	return text.startsWith(u"https://"_q)
		&& (text.size() > kHttpsPrefixSize)
		&& (text.indexOf(u' ') < 0);
}

// An https:// URL without spaces, or an IPv4 address.
[[nodiscard]] bool IsDnsServerText(const QString &text) {
	if (text.startsWith(u"https://"_q)) {
		return IsHttpsAddressText(text);
	}
	const QStringList parts = text.split(u'.', Qt::KeepEmptyParts);
	if (parts.size() != 4) {
		return false;
	}
	for (const QString &part : parts) {
		if (part.size() > 3 || !ParseNumber(part, 0, 255)) {
			return false;
		}
	}
	return true;
}


[[nodiscard]] QString FragmentModeName(const std::string &mode) {
	if (mode == kFragmentClassic) {
		return tr::lng_impulse_proxy_adv_classic(tr::now);
	} else if (mode == kFragmentFinalMask) {
		return tr::lng_impulse_proxy_adv_final_mask(tr::now);
	}
	return tr::lng_impulse_proxy_adv_off(tr::now);
}

[[nodiscard]] QString DnsName(const Advanced &advanced) {
	if (advanced.dnsMode == kDnsCloudflare) {
		return tr::lng_impulse_proxy_adv_dns_cloudflare(tr::now);
	} else if (advanced.dnsMode == kDnsGoogle) {
		return tr::lng_impulse_proxy_adv_dns_google(tr::now);
	} else if (advanced.dnsMode == kDnsCustom) {
		return Text(advanced.dnsCustom);
	}
	return tr::lng_impulse_proxy_adv_system(tr::now);
}

[[nodiscard]] QString PingModeName(const std::string &mode) {
	if (mode == kPingSequential) {
		return tr::lng_impulse_proxy_adv_checks_one(tr::now);
	}
	return tr::lng_impulse_proxy_adv_checks_batch(tr::now);
}

[[nodiscard]] QString MillisText(const QString &value) {
	return tr::lng_impulse_proxy_adv_ms_value(tr::now, lt_value, value);
}

[[nodiscard]] QString SecondsText(const QString &value) {
	return tr::lng_impulse_proxy_adv_s_value(tr::now, lt_value, value);
}

[[nodiscard]] QString SecondsOrSystem(int value) {
	return (value == 0)
		? tr::lng_impulse_proxy_adv_system(tr::now)
		: SecondsText(QString::number(value));
}

[[nodiscard]] QString SystemOr(int value) {
	return (value == 0)
		? tr::lng_impulse_proxy_adv_system(tr::now)
		: QString::number(value);
}

[[nodiscard]] QString NumberError(NumberUnit unit, int min, int max) {
	const QString low = QString::number(min);
	const QString high = QString::number(max);
	if (unit == NumberUnit::Millis) {
		return tr::lng_impulse_proxy_adv_ms_error(
			tr::now,
			lt_min,
			low,
			lt_max,
			high);
	} else if (unit == NumberUnit::Seconds) {
		return tr::lng_impulse_proxy_adv_s_error(
			tr::now,
			lt_min,
			low,
			lt_max,
			high);
	}
	return tr::lng_impulse_proxy_adv_number_error(
		tr::now,
		lt_min,
		low,
		lt_max,
		high);
}

// Writes the whole advanced block. The restart of the core is decided by the
// application, from the core part of the settings.
void SaveAdvanced(const Advanced &advanced) {
	Impulse::ProxyState::State state = Core::App().settings().proxy().impulseProxyState();
	state.advanced = advanced;
	Core::App().settings().proxy().setImpulseProxyState(std::move(state));
	Core::App().saveSettingsDelayed();
	Core::App().refreshGlobalProxy();
}

// A box with one radio button per label. Picking a label closes the box.
void ShowChoice(
		const std::shared_ptr<Ui::Show> &show,
		const QString &title,
		const std::vector<QString> &labels,
		int selected,
		Fn<void(int)> chosen) {
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));

		const std::shared_ptr<Ui::RadiobuttonGroup> group = std::make_shared<Ui::RadiobuttonGroup>(selected);
		for (int i = 0; i < int(labels.size()); ++i) {
			box->addRow(object_ptr<Ui::Radiobutton>(
				box,
				group,
				i,
				labels[i],
				st::defaultBoxCheckbox));
		}
		group->setChangedCallback([=](int value) {
			chosen(value);
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

// A box with one text field. The apply callback returns false for an invalid
// text; the field then shows its error and the box stays open.
void ShowTextInput(
		const std::shared_ptr<Ui::Show> &show,
		const QString &title,
		const QString &initial,
		const QString &error,
		bool numeric,
		Fn<bool(const QString &)> apply) {
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));

		const not_null<Ui::VerticalLayout*> inner = box->verticalLayout();
		Ui::InputField *const field = inner->add(
			object_ptr<Ui::InputField>(
				inner,
				st::defaultInputField,
				rpl::single(title),
				initial),
			st::boxRowPadding);
		if (numeric) {
			field->setInputMethodHints(Qt::ImhDigitsOnly);
		}
		Ui::AddDividerText(inner, rpl::single(error));

		const auto submit = [=] {
			const QString text = field->getLastText().trimmed();
			if (apply(text)) {
				box->closeBox();
			} else {
				field->showError();
			}
		};
		field->submits() | rpl::on_next(submit, field->lifetime());
		box->setFocusCallback([=] { field->setFocusFast(); });
		box->addButton(tr::lng_settings_save(), submit);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ShowNumberInput(
		const std::shared_ptr<Ui::Show> &show,
		const QString &title,
		int initial,
		int min,
		int max,
		NumberUnit unit,
		Fn<void(int)> apply) {
	ShowTextInput(
		show,
		title,
		QString::number(initial),
		NumberError(unit, min, max),
		true,
		[=](const QString &text) {
			const std::optional<int> value = ParseNumber(text, min, max);
			if (!value) {
				return false;
			}
			apply(*value);
			return true;
		});
}

} // namespace

ImpulseProxyAdvancedBox::ImpulseProxyAdvancedBox(QWidget*) {
}

void ImpulseProxyAdvancedBox::prepare() {
	setupContent();
}

void ImpulseProxyAdvancedBox::setupContent() {
	setTitle(tr::lng_impulse_proxy_adv_title());

	addButton(tr::lng_box_ok(), [=] { closeBox(); });

	object_ptr<Ui::VerticalLayout> wrap = object_ptr<Ui::VerticalLayout>(this);
	Ui::VerticalLayout *const content = wrap.data();
	setInnerWidget(object_ptr<Ui::OverrideMargins>(this, std::move(wrap)));

	const std::shared_ptr<Ui::Show> show = uiShow();
	rpl::variable<Advanced> *const advanced = Ui::CreateChild<rpl::variable<Advanced>>(
		content,
		Core::App().settings().proxy().impulseProxyState().advanced);

	// Stores one edit. Nothing is saved when the edit changes nothing.
	const auto change = [=](Fn<void(Advanced &)> edit) {
		Advanced next = advanced->current();
		edit(next);
		if (next == advanced->current()) {
			return;
		}
		*advanced = next;
		SaveAdvanced(next);
	};

	const auto addRow = [=](
			not_null<Ui::VerticalLayout*> parent,
			rpl::producer<QString> text,
			rpl::producer<QString> value,
			Fn<void()> clicked) {
		Settings::AddButtonWithLabel(
			parent,
			std::move(text),
			std::move(value),
			st::settingsButton
		)->addClickHandler(std::move(clicked));
	};

	const auto addToggle = [=](
			not_null<Ui::VerticalLayout*> parent,
			rpl::producer<QString> text,
			bool Advanced::*field) {
		Ui::SettingsButton *const button = parent->add(object_ptr<Ui::SettingsButton>(
			parent,
			std::move(text),
			st::settingsButtonNoIcon));
		button->toggleOn(advanced->value() | rpl::map(
			[=](const Advanced &value) {
				return value.*field;
			}));
		button->toggledChanges() | rpl::on_next([=](bool checked) {
			change([=](Advanced &edited) {
				edited.*field = checked;
			});
		}, button->lifetime());
	};

	// Fragmentation.
	Ui::AddSubsectionTitle(content, tr::lng_impulse_proxy_adv_fragmentation());
	addRow(
		content,
		tr::lng_impulse_proxy_adv_mode(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return FragmentModeName(value.fragmentMode);
		}),
		[=] {
			const std::string current = advanced->current().fragmentMode;
			ShowChoice(
				show,
				tr::lng_impulse_proxy_adv_mode(tr::now),
				{
					tr::lng_impulse_proxy_adv_off(tr::now),
					tr::lng_impulse_proxy_adv_classic(tr::now),
					tr::lng_impulse_proxy_adv_final_mask(tr::now),
				},
				IndexOf(FragmentModes(), current),
				[=](int index) {
					change([=](Advanced &edited) {
						edited.fragmentMode = FragmentModes()[index];
					});
				});
		});

	Ui::SlideWrap<Ui::VerticalLayout> *const fragment = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	Ui::VerticalLayout *const fragmentRows = fragment->entity();
	advanced->value() | rpl::on_next([=](const Advanced &value) {
		fragment->toggle(
			value.fragmentMode != kFragmentOff,
			anim::type::instant);
	}, fragment->lifetime());

	addRow(
		fragmentRows,
		tr::lng_impulse_proxy_adv_packets(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return Text(value.fragmentPackets);
		}),
		[=] {
			const std::string current = advanced->current().fragmentPackets;
			const int selected = (current == kPacketsDefault)
				? 0
				: (current == kPacketsExample)
				? 1
				: 2;
			ShowChoice(
				show,
				tr::lng_impulse_proxy_adv_packets(tr::now),
				{
					Text(kPacketsDefault),
					Text(kPacketsExample),
					tr::lng_impulse_proxy_adv_custom(tr::now),
				},
				selected,
				[=](int index) {
					if (index == 0) {
						change([=](Advanced &edited) {
							edited.fragmentPackets = kPacketsDefault;
						});
					} else if (index == 1) {
						change([=](Advanced &edited) {
							edited.fragmentPackets = kPacketsExample;
						});
					} else {
						ShowTextInput(
							show,
							tr::lng_impulse_proxy_adv_packets(tr::now),
							Text(current),
							tr::lng_impulse_proxy_adv_packets_error(tr::now),
							false,
							[=](const QString &text) {
								if (!IsPacketsText(text)) {
									return false;
								}
								change([=](Advanced &edited) {
									edited.fragmentPackets = text.toStdString();
								});
								return true;
							});
					}
				});
		});

	addRow(
		fragmentRows,
		tr::lng_impulse_proxy_adv_length(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return Text(value.fragmentLength);
		}),
		[=] {
			ShowTextInput(
				show,
				tr::lng_impulse_proxy_adv_length(tr::now),
				Text(advanced->current().fragmentLength),
				tr::lng_impulse_proxy_adv_range_error(tr::now),
				false,
				[=](const QString &text) {
					if (!IsRangeText(text)) {
						return false;
					}
					change([=](Advanced &edited) {
						edited.fragmentLength = text.toStdString();
					});
					return true;
				});
		});

	addRow(
		fragmentRows,
		tr::lng_impulse_proxy_adv_interval(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return MillisText(Text(value.fragmentInterval));
		}),
		[=] {
			ShowTextInput(
				show,
				tr::lng_impulse_proxy_adv_interval(tr::now),
				Text(advanced->current().fragmentInterval),
				tr::lng_impulse_proxy_adv_range_error(tr::now),
				false,
				[=](const QString &text) {
					if (!IsRangeText(text)) {
						return false;
					}
					change([=](Advanced &edited) {
						edited.fragmentInterval = text.toStdString();
					});
					return true;
				});
		});

	addToggle(
		content,
		tr::lng_impulse_proxy_adv_from_link(),
		&Advanced::fragmentFromLink);
	Ui::AddDividerText(content, tr::lng_impulse_proxy_adv_fragment_info());

	// TLS fingerprint.
	Ui::AddSubsectionTitle(content, tr::lng_impulse_proxy_adv_fingerprint());
	addRow(
		content,
		tr::lng_impulse_proxy_adv_fingerprint(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return value.fingerprint.empty()
				? tr::lng_impulse_proxy_adv_as_in_link(tr::now)
				: Text(value.fingerprint);
		}),
		[=] {
			std::vector<QString> labels = std::vector<QString>{
				tr::lng_impulse_proxy_adv_as_in_link(tr::now),
			};
			for (const std::string &name : Fingerprints()) {
				labels.push_back(Text(name));
			}
			const std::string current = advanced->current().fingerprint;
			ShowChoice(
				show,
				tr::lng_impulse_proxy_adv_fingerprint(tr::now),
				labels,
				current.empty() ? 0 : IndexOf(Fingerprints(), current) + 1,
				[=](int index) {
					change([=](Advanced &edited) {
						edited.fingerprint = (index == 0)
							? std::string()
							: Fingerprints()[index - 1];
					});
				});
		});
	Ui::AddDividerText(content, tr::lng_impulse_proxy_adv_fingerprint_info());

	// Multiplexing.
	Ui::AddSubsectionTitle(content, tr::lng_impulse_proxy_adv_multiplexing());
	addToggle(content, tr::lng_impulse_proxy_adv_mux(), &Advanced::muxEnabled);
	Ui::SlideWrap<Ui::VerticalLayout> *const mux = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	advanced->value() | rpl::on_next([=](const Advanced &value) {
		mux->toggle(value.muxEnabled, anim::type::instant);
	}, mux->lifetime());
	addRow(
		mux->entity(),
		tr::lng_impulse_proxy_adv_mux_concurrency(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return QString::number(value.muxConcurrency);
		}),
		[=] {
			ShowNumberInput(
				show,
				tr::lng_impulse_proxy_adv_mux_concurrency(tr::now),
				advanced->current().muxConcurrency,
				1,
				kMaxMuxConcurrency,
				NumberUnit::Plain,
				[=](int value) {
					change([=](Advanced &edited) {
						edited.muxConcurrency = value;
					});
				});
		});
	Ui::AddDividerText(content, tr::lng_impulse_proxy_adv_mux_info());

	// TCP.
	Ui::AddSubsectionTitle(content, tr::lng_impulse_proxy_adv_tcp());
	addRow(
		content,
		tr::lng_impulse_proxy_adv_keep_idle(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return SecondsOrSystem(value.keepAliveIdle);
		}),
		[=] {
			ShowNumberInput(
				show,
				tr::lng_impulse_proxy_adv_keep_idle(tr::now),
				advanced->current().keepAliveIdle,
				0,
				kMaxKeepAlive,
				NumberUnit::Seconds,
				[=](int value) {
					change([=](Advanced &edited) {
						edited.keepAliveIdle = value;
					});
				});
		});
	addRow(
		content,
		tr::lng_impulse_proxy_adv_keep_interval(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return SecondsOrSystem(value.keepAliveInterval);
		}),
		[=] {
			ShowNumberInput(
				show,
				tr::lng_impulse_proxy_adv_keep_interval(tr::now),
				advanced->current().keepAliveInterval,
				0,
				kMaxKeepAlive,
				NumberUnit::Seconds,
				[=](int value) {
					change([=](Advanced &edited) {
						edited.keepAliveInterval = value;
					});
				});
		});
	addToggle(content, tr::lng_impulse_proxy_adv_fast_open(), &Advanced::tcpFastOpen);
	addRow(
		content,
		tr::lng_impulse_proxy_adv_mss(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return SystemOr(value.tcpMaxSeg);
		}),
		[=] {
			ShowTextInput(
				show,
				tr::lng_impulse_proxy_adv_mss(tr::now),
				QString::number(advanced->current().tcpMaxSeg),
				tr::lng_impulse_proxy_adv_mss_error(tr::now),
				true,
				[=](const QString &text) {
					const std::optional<int> value = ParseNumber(text, 0, kMaxMss);
					if (!value || (*value != 0 && *value < kMinMss)) {
						return false;
					}
					change([=](Advanced &edited) {
						edited.tcpMaxSeg = *value;
					});
					return true;
				});
		});
	Ui::AddDividerText(content, tr::lng_impulse_proxy_adv_tcp_info());

	// DNS for the server addresses.
	Ui::AddSubsectionTitle(content, tr::lng_impulse_proxy_adv_dns());
	addRow(
		content,
		tr::lng_impulse_proxy_adv_dns_server(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return DnsName(value);
		}),
		[=] {
			const Advanced current = advanced->current();
			ShowChoice(
				show,
				tr::lng_impulse_proxy_adv_dns_server(tr::now),
				{
					tr::lng_impulse_proxy_adv_system(tr::now),
					tr::lng_impulse_proxy_adv_dns_cloudflare(tr::now),
					tr::lng_impulse_proxy_adv_dns_google(tr::now),
					tr::lng_impulse_proxy_adv_custom(tr::now),
				},
				IndexOf(DnsModes(), current.dnsMode),
				[=](int index) {
					if (index < 3) {
						change([=](Advanced &edited) {
							edited.dnsMode = DnsModes()[index];
						});
						return;
					}
					ShowTextInput(
						show,
						tr::lng_impulse_proxy_adv_dns_server(tr::now),
						Text(current.dnsCustom),
						tr::lng_impulse_proxy_adv_dns_error(tr::now),
						false,
						[=](const QString &text) {
							if (!IsDnsServerText(text)) {
								return false;
							}
							change([=](Advanced &edited) {
								edited.dnsMode = kDnsCustom;
								edited.dnsCustom = text.toStdString();
							});
							return true;
						});
				});
		});
	Ui::AddDividerText(content, tr::lng_impulse_proxy_adv_dns_info());

	// Server checks. None of these settings restarts the core.
	Ui::AddSubsectionTitle(content, tr::lng_impulse_proxy_adv_checks());
	addRow(
		content,
		tr::lng_impulse_proxy_adv_mode(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return PingModeName(value.pingMode);
		}),
		[=] {
			ShowChoice(
				show,
				tr::lng_impulse_proxy_adv_mode(tr::now),
				{
					tr::lng_impulse_proxy_adv_checks_batch(tr::now),
					tr::lng_impulse_proxy_adv_checks_one(tr::now),
				},
				IndexOf(PingModes(), advanced->current().pingMode),
				[=](int index) {
					change([=](Advanced &edited) {
						edited.pingMode = PingModes()[index];
					});
				});
		});
	addRow(
		content,
		tr::lng_impulse_proxy_adv_pause(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return MillisText(QString::number(value.pingPauseMillis));
		}),
		[=] {
			ShowNumberInput(
				show,
				tr::lng_impulse_proxy_adv_pause(tr::now),
				advanced->current().pingPauseMillis,
				0,
				kMaxPingPause,
				NumberUnit::Millis,
				[=](int value) {
					change([=](Advanced &edited) {
						edited.pingPauseMillis = value;
					});
				});
		});
	addRow(
		content,
		tr::lng_impulse_proxy_adv_timeout(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return SecondsText(QString::number(value.pingTimeoutSeconds));
		}),
		[=] {
			ShowNumberInput(
				show,
				tr::lng_impulse_proxy_adv_timeout(tr::now),
				advanced->current().pingTimeoutSeconds,
				1,
				kMaxPingTimeout,
				NumberUnit::Seconds,
				[=](int value) {
					change([=](Advanced &edited) {
						edited.pingTimeoutSeconds = value;
					});
				});
		});
	addRow(
		content,
		tr::lng_impulse_proxy_adv_url(),
		advanced->value() | rpl::map([](const Advanced &value) {
			return Text(value.pingUrl);
		}),
		[=] {
			ShowTextInput(
				show,
				tr::lng_impulse_proxy_adv_url(tr::now),
				Text(advanced->current().pingUrl),
				tr::lng_impulse_proxy_adv_url_error(tr::now),
				false,
				[=](const QString &text) {
					if (!IsHttpsAddressText(text)) {
						return false;
					}
					change([=](Advanced &edited) {
						edited.pingUrl = text.toStdString();
					});
					return true;
				});
		});
	addToggle(
		content,
		tr::lng_impulse_proxy_adv_skip_connected(),
		&Advanced::pingSkipWhileConnected);
	Ui::AddDividerText(content, tr::lng_impulse_proxy_adv_checks_info());

	// Reset to the defaults, after a confirmation.
	Ui::AddSkip(content);
	content->add(object_ptr<Ui::SettingsButton>(
		content,
		tr::lng_impulse_proxy_adv_reset(),
		st::settingsAttentionButton
	))->addClickHandler([=] {
		show->showBox(Ui::MakeConfirmBox({
			.text = tr::lng_impulse_proxy_adv_reset_title(),
			.confirmed = [=] {
				*advanced = Advanced();
				SaveAdvanced(Advanced());
			},
			.confirmText = tr::lng_impulse_proxy_adv_reset_button(),
			.confirmStyle = &st::attentionBoxButton,
		}));
	});
	Ui::AddSkip(content);
}

void ShowImpulseProxyAdvancedBox(std::shared_ptr<Ui::Show> show) {
	show->showBox(Box<ImpulseProxyAdvancedBox>());
}
