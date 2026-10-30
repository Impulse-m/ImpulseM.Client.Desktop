// Links built from the IMPULSEM_* build variables. An unset variable gives an
// empty string: the caller then hides the link or shows its text without one.
#pragma once

#include "lang/lang_text_entity.h"
#include "mtproto/impulse/core/build_config.h"

#include <QtCore/QString>
#include <QtCore/QStringView>
#include <QtCore/QUrl>

namespace Impulse::BuildLinks {

[[nodiscard]] inline QString FromView(std::string_view value) {
	return QString::fromUtf8(value.data(), qsizetype(value.size()));
}

// SITE_URL followed by a page path that starts with a slash, e.g. "/faq".
[[nodiscard]] inline QString SitePage(QStringView path) {
	auto result = FromView(BuildConfig::SiteUrl());
	if (result.isEmpty() || path.isEmpty()) {
		return QString();
	}
	return result.append(path);
}

// The text as a link to a website page, or the plain text when SITE_URL is unset.
[[nodiscard]] inline TextWithEntities SiteLinkText(
		const QString &text,
		QStringView path) {
	const auto url = SitePage(path);
	return url.isEmpty() ? TextWithEntities{ text } : tr::link(text, url);
}

// Host of SITE_URL, used to open the website without a confirmation.
[[nodiscard]] inline QString SiteHost() {
	return QUrl(FromView(BuildConfig::SiteUrl())).host();
}

// Host of public share links, e.g. "o.example.org".
[[nodiscard]] inline QString LinkHost() {
	return FromView(BuildConfig::LinkHost());
}

// https:// followed by LINK_HOST and a path that starts with a slash.
[[nodiscard]] inline QString LinkPage(QStringView path) {
	const auto host = LinkHost();
	if (host.isEmpty() || path.isEmpty()) {
		return QString();
	}
	return QStringLiteral("https://").append(host).append(path);
}

} // namespace Impulse::BuildLinks
