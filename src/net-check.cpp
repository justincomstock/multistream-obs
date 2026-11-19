#include "net-check.hpp"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QHostInfo>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace {

const int CONNECT_MS = 5000;

enum class Try { Open, Refused, NoAnswer };

// Opens a plain TCP connection to host:port and closes it again: no data is
// sent, so a stream server never sees a half-started stream.
void tryPort(const QHostAddress &addr, quint16 port, std::function<void(Try)> done)
{
	auto *sock = new QTcpSocket();
	auto *timer = new QTimer(sock);
	timer->setSingleShot(true);
	auto finished = std::make_shared<bool>(false);
	auto finish = [sock, finished, done](Try result) {
		if (*finished)
			return;
		*finished = true;
		sock->abort();
		sock->deleteLater();
		done(result);
	};
	QObject::connect(sock, &QTcpSocket::connected, sock, [finish]() { finish(Try::Open); });
	QObject::connect(sock, &QAbstractSocket::errorOccurred, sock, [finish](QAbstractSocket::SocketError e) {
		finish(e == QAbstractSocket::ConnectionRefusedError ? Try::Refused : Try::NoAnswer);
	});
	QObject::connect(timer, &QTimer::timeout, sock, [finish]() { finish(Try::NoAnswer); });
	timer->start(CONNECT_MS);
	sock->connectToHost(addr, port);
}

bool isThisPc(const QString &host)
{
	const QHostAddress a(host);
	return host.compare("localhost", Qt::CaseInsensitive) == 0 || (!a.isNull() && a.isLoopback());
}

} // namespace

namespace netcheck {

void diagnose(const QString &server, std::function<void(const QString &why)> done)
{
	const QUrl url(server.trimmed());
	const QString host = url.host();
	const bool secure = url.scheme().compare("rtmps", Qt::CaseInsensitive) == 0;
	const quint16 port = quint16(url.port(secure ? 443 : 1935));
	if (host.isEmpty()) {
		done("dns");
		return;
	}
	if (isThisPc(host)) {
		tryPort(QHostAddress(host.compare("localhost", Qt::CaseInsensitive) == 0 ? QString("127.0.0.1") : host), port,
			[done](Try t) { done(t == Try::Open ? "reached" : "app-closed"); });
		return;
	}
	QHostInfo::lookupHost(host, [host, port, done](const QHostInfo &info) {
		if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
			done("dns");
			return;
		}
		// IPv4 first: the one OBS's own RTMP output uses when there's a choice.
		QHostAddress addr = info.addresses().first();
		for (const auto &a : info.addresses())
			if (a.protocol() == QAbstractSocket::IPv4Protocol) {
				addr = a;
				break;
			}
		tryPort(addr, port, [addr, port, done](Try t) {
			if (t == Try::Open) {
				done("reached");
				return;
			}
			if (t == Try::Refused) {
				done("refused");
				return;
			}
			if (port == 443) {
				done("blocked");
				return;
			}
			// The stream port gave no answer: does the website's?
			tryPort(addr, 443, [done](Try web) { done(web == Try::NoAnswer ? "blocked" : "port-blocked"); });
		});
	});
}

} // namespace netcheck
