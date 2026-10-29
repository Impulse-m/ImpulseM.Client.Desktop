#include "core/impulse_proxy/proxy_subscription.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Impulse::Xray {
namespace {

constexpr char kUserAgent[] = "v2rayNG/1.10.0";
constexpr int kTimeoutMillis = 30000;
constexpr int kHttpFirstSuccess = 200;
constexpr int kHttpLastSuccess = 299;
constexpr char kTitleHeader[] = "profile-title";
constexpr char kUserInfoHeader[] = "subscription-userinfo";
constexpr char kIntervalHeader[] = "profile-update-interval";

struct Collected final {
	QByteArray body;
	bool oversize = false;
};

[[nodiscard]] std::optional<std::string> RawHeader(
		QNetworkReply *reply,
		const char *name) {
	const QByteArray key(name);
	if (!reply->hasRawHeader(key)) {
		return std::nullopt;
	}
	const QByteArray value = reply->rawHeader(key);
	return std::string(
		value.constData(),
		static_cast<std::size_t>(value.size()));
}

[[nodiscard]] std::optional<std::string_view> View(
		const std::optional<std::string> &value) {
	if (!value) {
		return std::nullopt;
	}
	return std::string_view(*value);
}

// Appends the data received so far. A body over the cap drops what was read
// and aborts the download.
void Accept(const std::shared_ptr<Collected> &collected, QNetworkReply *reply) {
	if (collected->oversize) {
		return;
	}
	collected->body.append(reply->readAll());
	const std::size_t size = static_cast<std::size_t>(collected->body.size());
	if (!ProxyInput::BodyWithinCap(size)) {
		collected->oversize = true;
		collected->body.clear();
		reply->abort();
	}
}

// Maps the finished download. An HTTP status outside 2xx is reported before
// anything else, as the Android client does; an oversize body before a
// network failure, because the abort that enforces the cap is itself a
// network failure.
void Complete(
		QNetworkReply *reply,
		const std::shared_ptr<Collected> &collected,
		const SubscriptionFetchCallback &callback) {
	SubscriptionFetch result;
	const QVariant statusValue = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute);
	if (statusValue.isValid()) {
		const int status = statusValue.toInt();
		if (status < kHttpFirstSuccess || status > kHttpLastSuccess) {
			result.error = ProxyInput::HttpError(status);
			callback(std::move(result));
			return;
		}
	}
	if (collected->oversize) {
		result.error = std::string(ProxyInput::kErrorInvalidSubscription);
	} else if (reply->error() != QNetworkReply::NoError) {
		result.error = std::string(ProxyInput::kErrorNetwork);
	} else {
		const std::string raw(
			collected->body.constData(),
			static_cast<std::size_t>(collected->body.size()));
		result.body = std::string(ProxyInput::StripBom(raw));

		const std::optional<std::string> title = RawHeader(
			reply,
			kTitleHeader);
		const std::optional<std::string> userInfo = RawHeader(
			reply,
			kUserInfoHeader);
		const std::optional<std::string> interval = RawHeader(
			reply,
			kIntervalHeader);
		result.meta = ProxyInput::ParseMeta(
			View(title),
			View(userInfo),
			View(interval));
	}
	callback(std::move(result));
}

} // namespace

void FetchSubscription(
		const std::string &url,
		SubscriptionFetchCallback callback) {
	const std::string trimmed(ProxyInput::Trim(url));
	const QUrl target(QString::fromStdString(trimmed));
	const QString scheme = target.scheme().toLower();
	if (!target.isValid()
			|| (scheme != QStringLiteral("http")
				&& scheme != QStringLiteral("https"))) {
		SubscriptionFetch result;
		result.error = std::string(ProxyInput::kErrorInvalidSubscription);
		callback(std::move(result));
		return;
	}

	QNetworkAccessManager *manager = new QNetworkAccessManager();
	QNetworkRequest request(target);
	request.setRawHeader("User-Agent", kUserAgent);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	QNetworkReply *reply = manager->get(request);

	// The whole call is limited, not only the time between two packets.
	QTimer *timer = new QTimer(reply);
	timer->setSingleShot(true);
	QObject::connect(timer, &QTimer::timeout, reply, [reply]() {
		reply->abort();
	});
	timer->start(kTimeoutMillis);

	const std::shared_ptr<Collected> collected
		= std::make_shared<Collected>();
	QObject::connect(reply, &QNetworkReply::readyRead, reply, [
		reply,
		collected
	]() {
		Accept(collected, reply);
	});
	QObject::connect(reply, &QNetworkReply::finished, reply, [
		reply,
		collected,
		timer,
		manager,
		callback
	]() {
		timer->stop();
		Accept(collected, reply);
		Complete(reply, collected, callback);
		manager->deleteLater();
	});
}

} // namespace Impulse::Xray
