#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

class QTcpServer;
class QTcpSocket;
class HttpClient;
class RelayOutput;

struct HttpRequest {
	QByteArray method;
	QByteArray path;
	QByteArray query;
	QMap<QByteArray, QByteArray> headers; // lower-case names
	QByteArray body;
};

// The dock's own little web server, on 127.0.0.1 only:
//   /, /assets/...  the dock page, fetched from Multistream (/web/obs-dock/)
//                   and kept on disk, so it still opens when offline
//   /cloud/...      Multistream Cloud's API, with the account's sign-in added
//   /app/...        the Multistream app on this PC (Free plan: This PC)
//   /bridge/...     OBS itself: state, Go live, End, settings fixes
// Everything but the page needs the X-MS-Bridge secret the page is served
// with, so no website can drive OBS through it; the Host must be this
// address (no DNS rebinding).
class Bridge {
public:
	explicit Bridge(RelayOutput *relay);
	~Bridge();
	bool start();
	QString pageUrl() const;

private:
	void accept();
	void handle(QTcpSocket *s, const HttpRequest &r);
	void servePage(QTcpSocket *s, const QByteArray &path);
	void proxy(QTcpSocket *s, const HttpRequest &r, const QString &url, bool cloud);
	void bridgeRoute(QTcpSocket *s, const HttpRequest &r);
	void loadToken();
	void saveToken(const QString &token);

	QTcpServer *server_ = nullptr;
	HttpClient *http_ = nullptr;
	RelayOutput *relay_ = nullptr;
	QString secret_;
	QString cloud_;
	QString app_;
	QString token_;
	quint16 port_ = 0;
};
