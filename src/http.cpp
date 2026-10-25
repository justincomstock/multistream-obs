#include "http.hpp"

#include <QMetaObject>
#include <QObject>
#include <QPointer>

#ifdef _WIN32

#include <QThreadPool>
#include <QUrl>

#include <mutex>
#include <set>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace {
const qsizetype MAX_RESPONSE = 32 * 1024 * 1024;
}

struct HttpClient::Impl {
	HINTERNET session = nullptr;
	QThreadPool pool;
	std::mutex lock;
	std::set<HINTERNET> open; // requests in flight, closed on shutdown
	bool stopping = false;

	HttpResult run(const HttpRequestSpec &spec);
};

HttpResult HttpClient::Impl::run(const HttpRequestSpec &spec)
{
	HttpResult res;
	const QUrl u(spec.url);
	const bool https = u.scheme() == "https";
	if (!session || !u.isValid() || (!https && u.scheme() != "http"))
		return res;
	HINTERNET con = WinHttpConnect(session, u.host().toStdWString().c_str(),
				       INTERNET_PORT(u.port(https ? 443 : 80)), 0);
	if (!con)
		return res;
	QString target = u.path(QUrl::FullyEncoded);
	if (target.isEmpty())
		target = "/";
	if (u.hasQuery())
		target += "?" + u.query(QUrl::FullyEncoded);
	HINTERNET req = WinHttpOpenRequest(con, QString::fromLatin1(spec.method).toStdWString().c_str(),
					   target.toStdWString().c_str(), nullptr, WINHTTP_NO_REFERER,
					   WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
	if (!req) {
		WinHttpCloseHandle(con);
		return res;
	}
	{
		std::lock_guard<std::mutex> g(lock);
		if (stopping) {
			WinHttpCloseHandle(req);
			WinHttpCloseHandle(con);
			return res;
		}
		open.insert(req);
	}

	const int t = spec.timeoutMs;
	WinHttpSetTimeouts(req, t, t, t, t);
	// The cloud's answers never set cookies here: the account is the bearer
	// token the bridge adds, nothing else.
	DWORD off = WINHTTP_DISABLE_COOKIES | (spec.followRedirects ? 0 : WINHTTP_DISABLE_REDIRECTS);
	WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));

	QByteArray head;
	for (auto it = spec.headers.cbegin(); it != spec.headers.cend(); ++it)
		head += it.key() + ": " + it.value() + "\r\n";
	const std::wstring wHead = QString::fromUtf8(head).toStdWString();
	const DWORD n = DWORD(spec.body.size());
	bool ok = WinHttpSendRequest(req, wHead.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wHead.c_str(),
				     wHead.empty() ? 0 : DWORD(-1L),
				     n ? LPVOID(spec.body.constData()) : WINHTTP_NO_REQUEST_DATA, n, n, 0) &&
		  WinHttpReceiveResponse(req, nullptr);

	DWORD code = 0;
	if (ok) {
		DWORD size = sizeof(code);
		ok = WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					 WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
	}
	if (ok) {
		DWORD len = 0;
		WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
				    &len, WINHTTP_NO_HEADER_INDEX);
		if (len) {
			std::wstring ct(len / sizeof(wchar_t) + 1, L'\0');
			if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ct.data(),
						&len, WINHTTP_NO_HEADER_INDEX))
				res.contentType = QString::fromWCharArray(ct.c_str(), int(len / sizeof(wchar_t))).toUtf8();
		}
		QByteArray body;
		for (;;) {
			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(req, &avail)) {
				ok = false;
				break;
			}
			if (!avail)
				break;
			if (body.size() + qsizetype(avail) > MAX_RESPONSE) {
				ok = false;
				break;
			}
			const qsizetype at = body.size();
			body.resize(at + avail);
			DWORD got = 0;
			if (!WinHttpReadData(req, body.data() + at, avail, &got)) {
				ok = false;
				break;
			}
			body.resize(at + got);
			if (!got)
				break;
		}
		if (ok) {
			res.status = int(code);
			res.body = body;
		}
	}

	{
		std::lock_guard<std::mutex> g(lock);
		// Shutdown may have closed it already, to cut the wait short.
		if (open.erase(req))
			WinHttpCloseHandle(req);
	}
	WinHttpCloseHandle(con);
	if (!ok)
		res = HttpResult();
	return res;
}

HttpClient::HttpClient(const QByteArray &userAgent) : d_(new Impl)
{
	d_->session = WinHttpOpen(QString::fromUtf8(userAgent).toStdWString().c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
				  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	d_->pool.setMaxThreadCount(8);
}

HttpClient::~HttpClient()
{
	{
		std::lock_guard<std::mutex> g(d_->lock);
		d_->stopping = true;
		for (HINTERNET h : d_->open)
			WinHttpCloseHandle(h); // fails the blocked call at once
		d_->open.clear();
	}
	d_->pool.waitForDone();
	if (d_->session)
		WinHttpCloseHandle(d_->session);
	delete d_;
}

void HttpClient::send(const HttpRequestSpec &spec, QObject *context, std::function<void(const HttpResult &)> done)
{
	Impl *d = d_;
	QPointer<QObject> ctx(context);
	d->pool.start([d, spec, ctx, done]() {
		const HttpResult res = d->run(spec);
		// Queued to the context's thread; dropped if it's gone by then.
		if (QObject *c = ctx.data())
			QMetaObject::invokeMethod(c, [done, res]() { done(res); }, Qt::QueuedConnection);
	});
}

#elif defined(__APPLE__)
// macOS: the system's libcurl (in the SDK, with the system's TLS), one
// request per pool thread like WinHTTP above. OBS's Qt on macOS may come
// without a TLS backend too, so Qt's network stack isn't relied on here.

#include <QThreadPool>
#include <QUrl>

#include <atomic>

#include <curl/curl.h>

namespace {
const qsizetype MAX_RESPONSE = 32 * 1024 * 1024;

struct Reply {
	QByteArray body;
	bool tooBig = false;
	const std::atomic<bool> *stopping = nullptr;
};

size_t onBody(char *data, size_t size, size_t n, void *user)
{
	auto *r = static_cast<Reply *>(user);
	const size_t len = size * n;
	if (r->body.size() + qsizetype(len) > MAX_RESPONSE) {
		r->tooBig = true;
		return 0; // stops the transfer
	}
	r->body.append(data, qsizetype(len));
	return len;
}

// Called often while a request runs: a non-zero answer aborts it, so
// shutting down never waits for a slow server.
int onProgress(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	auto *r = static_cast<Reply *>(user);
	return r->stopping && r->stopping->load() ? 1 : 0;
}
} // namespace

struct HttpClient::Impl {
	QByteArray agent;
	QThreadPool pool;
	std::atomic<bool> stopping{false};

	HttpResult run(const HttpRequestSpec &spec);
};

HttpResult HttpClient::Impl::run(const HttpRequestSpec &spec)
{
	HttpResult res;
	const QUrl u(spec.url);
	if (stopping || !u.isValid() || (u.scheme() != "https" && u.scheme() != "http"))
		return res;
	CURL *c = curl_easy_init();
	if (!c)
		return res;
	const QByteArray url = u.toEncoded();
	Reply reply;
	reply.stopping = &stopping;
	curl_slist *head = nullptr;
	for (auto it = spec.headers.cbegin(); it != spec.headers.cend(); ++it)
		head = curl_slist_append(head, QByteArray(it.key() + ": " + it.value()).constData());
	// An empty Expect: no "100-continue" wait before a body.
	head = curl_slist_append(head, "Expect:");

	curl_easy_setopt(c, CURLOPT_URL, url.constData());
	curl_easy_setopt(c, CURLOPT_USERAGENT, agent.constData());
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, head);
	curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, spec.method.constData());
	if (spec.method == "HEAD")
		curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
	if (!spec.body.isEmpty() || spec.method == "POST" || spec.method == "PUT" || spec.method == "PATCH") {
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, spec.body.constData());
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(spec.body.size()));
	}
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, spec.followRedirects ? 1L : 0L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, long(spec.timeoutMs));
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, long(spec.timeoutMs));
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); // whatever the server compresses with
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onBody);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &reply);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, &reply);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	// No cookie engine: the account is the bearer token the bridge adds.

	const CURLcode rc = curl_easy_perform(c);
	if (rc == CURLE_OK && !reply.tooBig) {
		long code = 0;
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
		char *ct = nullptr;
		curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
		res.status = int(code);
		res.body = reply.body;
		if (ct)
			res.contentType = QByteArray(ct);
	}
	curl_slist_free_all(head);
	curl_easy_cleanup(c);
	return res;
}

HttpClient::HttpClient(const QByteArray &userAgent) : d_(new Impl)
{
	static const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
	(void)ready;
	d_->agent = userAgent;
	d_->pool.setMaxThreadCount(8);
}

HttpClient::~HttpClient()
{
	d_->stopping = true;
	d_->pool.waitForDone();
	delete d_;
}

void HttpClient::send(const HttpRequestSpec &spec, QObject *context, std::function<void(const HttpResult &)> done)
{
	Impl *d = d_;
	QPointer<QObject> ctx(context);
	d->pool.start([d, spec, ctx, done]() {
		const HttpResult res = d->run(spec);
		if (QObject *c = ctx.data())
			QMetaObject::invokeMethod(c, [done, res]() { done(res); }, Qt::QueuedConnection);
	});
}

#else // Linux: Qt's network stack, which has TLS there.

#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace {
class NoCookies : public QNetworkCookieJar {
public:
	bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override { return false; }
	QList<QNetworkCookie> cookiesForUrl(const QUrl &) const override { return {}; }
};
} // namespace

struct HttpClient::Impl {
	QNetworkAccessManager net;
	QByteArray agent;
};

HttpClient::HttpClient(const QByteArray &userAgent) : d_(new Impl)
{
	d_->agent = userAgent;
	d_->net.setCookieJar(new NoCookies());
}

HttpClient::~HttpClient()
{
	delete d_;
}

void HttpClient::send(const HttpRequestSpec &spec, QObject *context, std::function<void(const HttpResult &)> done)
{
	QNetworkRequest req{QUrl(spec.url)};
	req.setTransferTimeout(spec.timeoutMs);
	if (!spec.followRedirects)
		req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	req.setRawHeader("User-Agent", d_->agent);
	for (auto it = spec.headers.cbegin(); it != spec.headers.cend(); ++it)
		req.setRawHeader(it.key(), it.value());
	QNetworkReply *rep = d_->net.sendCustomRequest(req, spec.method, spec.body);
	QPointer<QObject> ctx(context);
	QObject::connect(rep, &QNetworkReply::finished, rep, [rep, ctx, done]() {
		rep->deleteLater();
		HttpResult res;
		if (rep->error() == QNetworkReply::NoError || rep->error() >= QNetworkReply::ContentAccessDenied) {
			res.status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			res.body = rep->readAll();
			res.contentType = rep->header(QNetworkRequest::ContentTypeHeader).toByteArray();
		}
		if (ctx)
			done(res);
	});
}

#endif
