/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/layers/box_content.h"

#include <memory>

namespace Ui {
class Show;
} // namespace Ui

// Proxied calls need the loopback relay, which is not shipped yet. While this
// is false, the "Use VPN for" section is hidden and calls never go through
// the proxy.
inline constexpr auto kProxiedCallsRelayReady = false;

// Advanced settings of the ImpulseM VLESS proxy: fragmentation, TLS
// fingerprint, multiplexing, TCP options, DNS and server checks. Every
// change is saved at once. Only a change of a setting that the Xray core
// uses restarts the core; ping settings never do.
class ImpulseProxyAdvancedBox final : public Ui::BoxContent {
public:
	ImpulseProxyAdvancedBox(QWidget*);

protected:
	void prepare() override;

private:
	void setupContent();

};

void ShowImpulseProxyAdvancedBox(std::shared_ptr<Ui::Show> show);
