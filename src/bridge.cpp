#include "bridge.hpp"

#include "games.hpp"
#include "http.hpp"
#include "obs-state.hpp"
#include "relay-output.hpp"
#include "vertical/canvas.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace {

const quint16 PREFERRED_PORT = 14590;
const qint64 MAX_BODY = 4 * 1024 * 1024;

QByteArray reason(int code)
{
	switch (code) {
	case 200:
		return "OK";
	case 204:
		return "No Content";
	case 400:
		return "Bad Request";
	case 401:
		return "Unauthorized";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 409:
		return "Conflict";
	case 413:
		return "Payload Too Large";
	case 431:
		return "Request Header Fields Too Large";
	case 502:
		return "Bad Gateway";
	default:
		return "Status";
	}
}

void reply(QTcpSocket *s, int code, const QByteArray &type, const QByteArray &body)
{
	if (!s)
		return;
	QByteArray out = "HTTP/1.1 " + QByteArray::number(code) + " " + reason(code) + "\r\n";
	out += "Content-Type: " + type + "\r\n";
	out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
	out += "Cache-Control: no-store\r\n";
	out += "X-Content-Type-Options: nosniff\r\n";
	out += "Referrer-Policy: no-referrer\r\n";
	out += "Connection: close\r\n\r\n";
	out += body;
	s->write(out);
	s->disconnectFromHost();
}

void replyJson(QTcpSocket *s, int code, const QJsonObject &o)
{
	reply(s, code, "application/json; charset=utf-8", QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject errorJson(const QString &message)
{
	QJsonObject o;
	o["error"] = message;
	return o;
}

QJsonObject okJson()
{
	QJsonObject o;
	o["ok"] = true;
	return o;
}

QString configPath(const char *name)
{
	char *p = obs_module_config_path(name);
	QString s = QString::fromUtf8(p);
	bfree(p);
	return s;
}

QByteArray typeOf(const QByteArray &path)
{
	if (path.endsWith(".html"))
		return "text/html; charset=utf-8";
	if (path.endsWith(".js") || path.endsWith(".mjs"))
		return "text/javascript; charset=utf-8";
	if (path.endsWith(".css"))
		return "text/css; charset=utf-8";
	if (path.endsWith(".svg"))
		return "image/svg+xml";
	if (path.endsWith(".png"))
		return "image/png";
	if (path.endsWith(".woff2"))
		return "font/woff2";
	if (path.endsWith(".json"))
		return "application/json";
	return "application/octet-stream";
}

QByteArray userAgent()
{
	return QByteArray("Multistream-OBS-Plugin/") + PLUGIN_VERSION + " OBS/" + obs_get_version_string();
}

// Shown when the dock page has never loaded and Multistream can't be reached.
const char *FALLBACK_PAGE =
	"<!doctype html><html><head><meta charset=\"utf-8\"><title>Multistream</title>"
	"<style>html,body{margin:0;height:100%;background:#0e0e13;color:#e8e7f2;font:14px system-ui,sans-serif}"
	"main{padding:24px 18px}h1{font-size:18px;font-weight:600;margin:0 0 6px}p{color:#8a89a0;line-height:1.5;margin:0 0 16px}"
	"button{border:0;border-radius:99px;padding:10px 18px;background:#a99cff;color:#0e0e13;font:inherit;font-weight:700;cursor:pointer}"
	"</style></head><body><main><h1>Can't reach Multistream</h1>"
	"<p>The Multistream dock loads from multistream.gg the first time. Check your internet connection, then try again.</p>"
	"<button onclick=\"location.reload()\">Try again</button></main></body></html>";

struct Conn {
	QByteArray buf;
	bool done = false;
};

} // namespace

Bridge::Bridge(RelayOutput *relay, RelayOutput *verticalRelay, VerticalCanvas *vertical)
	: relay_(relay),
	  verticalRelay_(verticalRelay),
	  vertical_(vertical)
{
}

Bridge::~Bridge()
{
	// Requests still running finish first: their answers go to server_.
	delete http_;
	delete server_;
}

bool Bridge::start()
{
	cloud_ = qEnvironmentVariable("MULTISTREAM_CLOUD_URL", "https://multistream.gg");
	while (cloud_.endsWith('/'))
		cloud_.chop(1);
	app_ = qEnvironmentVariable("MULTISTREAM_APP_URL", "http://127.0.0.1:14563");
	while (app_.endsWith('/'))
		app_.chop(1);

	for (int i = 0; i < 4; i++)
		secret_ += QString::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, '0');

	server_ = new QTcpServer();
	http_ = new HttpClient(userAgent());
	QObject::connect(server_, &QTcpServer::newConnection, server_, [this]() { accept(); });
	// A fixed port when it's free, so the page keeps the same address (and
	// its small browser-side settings) between OBS sessions.
	for (quint16 p = PREFERRED_PORT; p < PREFERRED_PORT + 10 && !server_->isListening(); p++)
		server_->listen(QHostAddress::LocalHost, p);
	if (!server_->isListening() && !server_->listen(QHostAddress::LocalHost, 0)) {
		obs_log(LOG_WARNING, "Multistream dock: no local port: %s", server_->errorString().toUtf8().constData());
		return false;
	}
	port_ = server_->serverPort();
	loadToken();
	obs_log(LOG_INFO, "Multistream dock on 127.0.0.1:%d, cloud %s", int(port_), cloud_.toUtf8().constData());
	return true;
}

QString Bridge::pageUrl() const
{
	return QString("http://127.0.0.1:%1/").arg(port_);
}

void Bridge::loadToken()
{
	const QByteArray file = configPath("account.json").toUtf8();
	if (obs_data_t *d = obs_data_create_from_json_file_safe(file.constData(), "bak")) {
		token_ = QString::fromUtf8(obs_data_get_string(d, "token"));
		obs_data_release(d);
	}
}

void Bridge::saveToken(const QString &token)
{
	token_ = token;
	QDir().mkpath(configPath(""));
	const QByteArray file = configPath("account.json").toUtf8();
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "token", token.toUtf8().constData());
	obs_data_save_json_safe(d, file.constData(), "tmp", "bak");
	obs_data_release(d);
}

void Bridge::accept()
{
	while (QTcpSocket *s = server_->nextPendingConnection()) {
		auto c = std::make_shared<Conn>();
		QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
		// A connection that never finishes its request is dropped.
		QTimer::singleShot(30000, s, [s]() { s->abort(); });
		QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, c]() {
			if (c->done) {
				s->readAll();
				return;
			}
			c->buf += s->readAll();
			const qsizetype he = c->buf.indexOf("\r\n\r\n");
			if (he < 0) {
				if (c->buf.size() > 64 * 1024) {
					c->done = true;
					reply(s, 431, "text/plain", "Too large");
				}
				return;
			}
			HttpRequest r;
			const QList<QByteArray> lines = c->buf.left(he).split('\n');
			const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
			if (first.size() < 2) {
				c->done = true;
				reply(s, 400, "text/plain", "Bad request");
				return;
			}
			r.method = first[0];
			const QByteArray target = first[1];
			const qsizetype q = target.indexOf('?');
			r.path = q >= 0 ? target.left(q) : target;
			r.query = q >= 0 ? target.mid(q + 1) : QByteArray();
			for (qsizetype i = 1; i < lines.size(); i++) {
				const QByteArray l = lines[i].trimmed();
				const qsizetype colon = l.indexOf(':');
				if (colon > 0)
					r.headers.insert(l.left(colon).trimmed().toLower(), l.mid(colon + 1).trimmed());
			}
			const qint64 len = r.headers.value("content-length").toLongLong();
			if (len < 0 || len > MAX_BODY) {
				c->done = true;
				reply(s, 413, "text/plain", "Too large");
				return;
			}
			if (c->buf.size() - (he + 4) < len)
				return;
			r.body = c->buf.mid(he + 4, len);
			c->done = true;
			handle(s, r);
		});
	}
}

void Bridge::handle(QTcpSocket *s, const HttpRequest &r)
{
	const QByteArray host = r.headers.value("host");
	const QByteArray port = QByteArray::number(port_);
	if (host != "127.0.0.1:" + port && host != "localhost:" + port) {
		reply(s, 403, "text/plain", "Wrong host");
		return;
	}
	if (r.path.contains("..") || r.path.contains('\\')) {
		reply(s, 400, "text/plain", "Bad path");
		return;
	}
	const bool api = r.path.startsWith("/bridge/") || r.path.startsWith("/cloud/") || r.path.startsWith("/app/");
	if (api) {
		if (secret_.isEmpty() || r.headers.value("x-ms-bridge") != secret_.toUtf8()) {
			replyJson(s, 403, errorJson("Not allowed."));
			return;
		}
		if (r.path.startsWith("/bridge/")) {
			bridgeRoute(s, r);
			return;
		}
		const bool cloud = r.path.startsWith("/cloud/");
		QByteArray rest = r.path.mid(cloud ? 6 : 4);
		if (!r.query.isEmpty())
			rest += "?" + r.query;
		proxy(s, r, (cloud ? cloud_ : app_) + QString::fromUtf8(rest), cloud);
		return;
	}
	if (r.method != "GET" && r.method != "HEAD") {
		reply(s, 405, "text/plain", "Method not allowed");
		return;
	}
	servePage(s, r.path);
}

void Bridge::servePage(QTcpSocket *s, const QByteArray &rawPath)
{
	static const QRegularExpression safe("^/[A-Za-z0-9._/-]*$");
	const QByteArray path = rawPath == "/" ? QByteArray("/index.html") : rawPath;
	if (!safe.match(QString::fromUtf8(path)).hasMatch()) {
		reply(s, 404, "text/plain", "Not found");
		return;
	}
	const bool index = path == "/index.html";
	const QString cacheFile = configPath("dock-cache/") + QString::fromUtf8(path.mid(1)).replace('/', '_');

	HttpRequestSpec req;
	req.url = cloud_ + "/web/obs-dock" + QString::fromUtf8(path);
	req.timeoutMs = index ? 6000 : 15000;
	req.headers.insert("Cache-Control", "no-cache");
	QPointer<QTcpSocket> sp(s);
	http_->send(req, server_, [this, sp, path, index, cacheFile](const HttpResult &res) {
		QByteArray body;
		if (res.status == 200) {
			body = res.body;
			QDir().mkpath(QFileInfo(cacheFile).path());
			QFile f(cacheFile);
			if (f.open(QIODevice::WriteOnly))
				f.write(body);
		} else {
			QFile f(cacheFile);
			if (f.open(QIODevice::ReadOnly))
				body = f.readAll();
			else if (index)
				body = FALLBACK_PAGE;
			else {
				reply(sp, 404, "text/plain", "Not found");
				return;
			}
		}
		if (index) {
			QJsonObject cfg;
			cfg["bridge"] = secret_;
			cfg["version"] = PLUGIN_VERSION;
			cfg["cloud"] = cloud_;
			cfg["obs"] = obs_get_version_string();
			const QByteArray tag = "<script>window.__MS=" + QJsonDocument(cfg).toJson(QJsonDocument::Compact) +
					       "</script>";
			const qsizetype at = body.indexOf("<head>");
			if (at >= 0)
				body.insert(at + 6, tag);
			else
				body.prepend(tag);
		}
		reply(sp, 200, typeOf(path), body);
	});
}

void Bridge::proxy(QTcpSocket *s, const HttpRequest &r, const QString &url, bool cloud)
{
	HttpRequestSpec req;
	req.method = r.method;
	req.url = url;
	req.body = r.body;
	req.timeoutMs = 20000;
	req.followRedirects = false;
	const QByteArray type = r.headers.value("content-type");
	if (!type.isEmpty())
		req.headers.insert("Content-Type", type);
	req.headers.insert("Accept", "application/json");
	req.headers.insert("X-Multistream-Client", "obs-plugin");
	if (cloud && !token_.isEmpty())
		req.headers.insert("Authorization", "Bearer " + token_.toUtf8());

	QPointer<QTcpSocket> sp(s);
	const bool poll = cloud && r.path == "/cloud/api/auth/app-link/poll";
	const bool logout = cloud && r.path == "/cloud/api/auth/logout";
	http_->send(req, server_, [this, sp, cloud, poll, logout](const HttpResult &res) {
		const int code = res.status;
		if (code == 0) {
			QJsonObject o = errorJson(cloud ? "Can't reach Multistream." : "The Multistream app isn't open.");
			o["unreachable"] = true;
			replyJson(sp, 502, o);
			return;
		}
		QByteArray body = res.body;
		QByteArray ctype = res.contentType;
		if (ctype.isEmpty())
			ctype = "application/json";
		// "Sign in with your browser" finished: the plugin keeps the token,
		// the page only hears that it worked.
		if (poll && code == 200) {
			QJsonObject o = QJsonDocument::fromJson(body).object();
			if (o.contains("token")) {
				saveToken(o.value("token").toString());
				o.remove("token");
				o["linked"] = true;
				body = QJsonDocument(o).toJson(QJsonDocument::Compact);
			}
		}
		if (logout)
			saveToken("");
		reply(sp, code, ctype, body);
	});
}

void Bridge::bridgeRoute(QTcpSocket *s, const HttpRequest &r)
{
	const QByteArray p = r.path.mid(8);
	const QJsonObject in = QJsonDocument::fromJson(r.body).object();

	if (p == "state" && r.method == "GET") {
		QJsonObject o;
		o["obs"] = obsstate::snapshot();
		o["relay"] = relay_->state();
		QJsonObject v = vertical_->state();
		v["output"] = verticalRelay_->state();
		o["vertical"] = v;
		o["signedIn"] = !token_.isEmpty();
		o["version"] = PLUGIN_VERSION;
		o["cloud"] = cloud_;
		replyJson(s, 200, o);
		return;
	}
	if (p == "games" && r.method == "GET") {
		replyJson(s, 200, games::state());
		return;
	}
	if (r.method != "POST") {
		replyJson(s, 405, errorJson("Use POST."));
		return;
	}
	QString error;
	if (p == "start") {
		replyJson(s, 200, okJson());
		// After the answer: OBS may ask "Are you sure?" in a dialog.
		QTimer::singleShot(0, []() {
			if (!obs_frontend_streaming_active())
				obs_frontend_streaming_start();
		});
	} else if (p == "stop") {
		replyJson(s, 200, okJson());
		QTimer::singleShot(0, []() {
			if (obs_frontend_streaming_active())
				obs_frontend_streaming_stop();
		});
	} else if (p == "service") {
		if (obsstate::setService(in.value("server").toString(), in.value("key").toString(), error))
			replyJson(s, 200, okJson());
		else
			replyJson(s, 409, errorJson(error));
	} else if (p == "service/restore") {
		if (obsstate::restoreService(error))
			replyJson(s, 200, okJson());
		else
			replyJson(s, 409, errorJson(error));
	} else if (p == "encoder") {
		if (obsstate::setEncoder(in.value("bitrate").toInt(), in.value("keyintSec").toInt(), error))
			replyJson(s, 200, okJson());
		else
			replyJson(s, 409, errorJson(error));
	} else if (p == "relay") {
		relay_->configure(in.value("enabled").toBool(), in.value("server").toString(), in.value("key").toString(),
				  in.value("bitrate").toInt(6000));
		replyJson(s, 200, relay_->state());
	} else if (p == "vertical") {
		verticalRelay_->configure(in.value("enabled").toBool(), in.value("server").toString(),
					  in.value("key").toString(), in.value("bitrate").toInt(6000));
		replyJson(s, 200, verticalRelay_->state());
	} else if (p == "vertical/start") {
		if (verticalRelay_->startNow())
			replyJson(s, 200, verticalRelay_->state());
		else
			replyJson(s, 409, errorJson(verticalRelay_->state().value("error").toString()));
	} else if (p == "vertical/stop") {
		verticalRelay_->stopNow();
		replyJson(s, 200, verticalRelay_->state());
	} else if (p == "vertical/scene") {
		if (vertical_->setScene(in.value("scene").toString(), error))
			replyJson(s, 200, vertical_->state());
		else
			replyJson(s, 409, errorJson(error));
	} else if (p == "open") {
		const QString url = in.value("url").toString();
		if (!url.startsWith(cloud_ + "/")) {
			replyJson(s, 400, errorJson("Only Multistream pages open from here."));
			return;
		}
		QDesktopServices::openUrl(QUrl(url));
		replyJson(s, 200, okJson());
	} else if (p == "games/follow") {
		games::follow(in.value("exe").toString());
		replyJson(s, 200, okJson());
	} else if (p == "games/settings") {
		QStringList names;
		for (const QJsonValue &v : in.value("follow").toArray())
			names.append(v.toString());
		games::setFollowSources(names);
		replyJson(s, 200, games::state());
	} else if (p == "signout") {
		saveToken("");
		replyJson(s, 200, okJson());
	} else {
		replyJson(s, 404, errorJson("No such action."));
	}
}
