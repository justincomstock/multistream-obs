#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

#include <functional>

class QObject;

// Outgoing HTTP(S) for the bridge. Not Qt's network stack: OBS ships Qt
// without its TLS plugins, so Qt's network stack can't open https:// at
// all ("TLS initialization failed"). On Windows this is WinHTTP (the
// system's own TLS), on macOS the system's libcurl, one request per pool
// thread; `done` runs on `context`'s thread. A status of 0 means no answer
// (offline, timeout, bad TLS).
struct HttpResult {
	int status = 0;
	QByteArray body;
	QByteArray contentType;
};

struct HttpRequestSpec {
	QByteArray method = "GET";
	QString url;
	QMap<QByteArray, QByteArray> headers;
	QByteArray body;
	int timeoutMs = 20000;
	bool followRedirects = true;
};

class HttpClient {
public:
	explicit HttpClient(const QByteArray &userAgent);
	~HttpClient(); // waits for requests still running
	void send(const HttpRequestSpec &spec, QObject *context, std::function<void(const HttpResult &)> done);

private:
	struct Impl;
	Impl *d_;
};
