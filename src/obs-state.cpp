#include "obs-state.hpp"

#include "i18n.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QUrl>

#include <atomic>

namespace {

struct Runtime {
	bool starting = false;
	bool stopping = false;
	qint64 startedAt = 0;
	qint64 stoppedAt = 0;
	uint64_t lastBytes = 0;
	qint64 lastAt = 0;
	int kbps = 0;
	// OBS's CPU use since the last snapshot (os_cpu_usage_info_query).
	os_cpu_usage_info_t *cpu = os_cpu_usage_info_start();
	// This start got as far as STREAMING_STARTED.
	bool reached = false;
	// How the last stream ended when it wasn't the streamer's doing (OBS
	// refused to start, or gave up reconnecting): the dock says why.
	qint64 stopAt = 0;
	int stopCode = 0;
	QString stopError;
	bool stopStarted = false;
	// The streaming output whose signals are watched.
	obs_weak_output_t *watched = nullptr;
	// What previous-service.json holds ("Twitch", a server's host), "" for none.
	QString previous;
	bool previousLoaded = false;
};
Runtime rt;
// Set on the output's own thread: the stop code, and whether it started.
std::atomic<long long> outCode{0};
std::atomic<bool> outStarting{false};

qint64 now()
{
	return QDateTime::currentMSecsSinceEpoch();
}

QString str(const char *s)
{
	return QString::fromUtf8(s ? s : "");
}

QString profileFile(const char *name)
{
	char *dir = obs_frontend_get_current_profile_path();
	QString p = str(dir) + "/" + name;
	bfree(dir);
	return p;
}

bool advancedMode(config_t *cfg)
{
	return str(config_get_string(cfg, "Output", "Mode")).compare("Advanced", Qt::CaseInsensitive) == 0;
}

// Simple output mode names its encoders ("x264", "nvenc", "nvenc_hevc",
// "qsv_av1", ...): the codec is in the name.
QString codecOfSimple(const QString &name)
{
	const QString n = name.toLower();
	if (n.contains("hevc") || n.contains("h265"))
		return "hevc";
	if (n.contains("av1"))
		return "av1";
	return "h264";
}

QString configFile(const char *name)
{
	char *p = obs_module_config_path(name);
	QString s = str(p);
	bfree(p);
	return s;
}

void onOutputStarting(void *, calldata_t *)
{
	outStarting = true;
}

void onOutputStop(void *, calldata_t *cd)
{
	outCode = calldata_int(cd, "code");
}

void unwatch()
{
	if (!rt.watched)
		return;
	if (obs_output_t *old = obs_weak_output_get_output(rt.watched)) {
		signal_handler_t *sh = obs_output_get_signal_handler(old);
		signal_handler_disconnect(sh, "starting", onOutputStarting, nullptr);
		signal_handler_disconnect(sh, "stop", onOutputStop, nullptr);
		obs_output_release(old);
	}
	obs_weak_output_release(rt.watched);
	rt.watched = nullptr;
}

// OBS's frontend events carry no stop code: the output's own "stop" signal does.
void watchOutput()
{
	obs_output_t *out = obs_frontend_get_streaming_output();
	if (!out)
		return;
	if (!rt.watched || !obs_weak_output_references_output(rt.watched, out)) {
		unwatch();
		signal_handler_t *sh = obs_output_get_signal_handler(out);
		signal_handler_connect(sh, "starting", onOutputStarting, nullptr);
		signal_handler_connect(sh, "stop", onOutputStop, nullptr);
		rt.watched = obs_output_get_weak_output(out);
	}
	obs_output_release(out);
}

QString lastOutputError()
{
	QString e;
	if (obs_output_t *out = obs_frontend_get_streaming_output()) {
		e = str(obs_output_get_last_error(out));
		obs_output_release(out);
	}
	return e;
}

void noteStop(int code, bool started)
{
	rt.stopAt = now();
	rt.stopCode = code;
	rt.stopError = lastOutputError();
	rt.stopStarted = started;
}

// The host in "rtmp://host:1935/live".
QString hostOf(QString server)
{
	return QUrl(server.replace("rtmps://", "http://").replace("rtmp://", "http://")).host();
}

// "Twitch", or the host a custom server points at.
QString serviceLabel(obs_data_t *settings)
{
	const QString name = str(obs_data_get_string(settings, "service"));
	if (!name.isEmpty())
		return name;
	const QString host = hostOf(str(obs_data_get_string(settings, "server")));
	return host.isEmpty() ? mt("Obs.OldSetting") : host;
}

void loadPrevious()
{
	rt.previousLoaded = true;
	rt.previous.clear();
	const QByteArray file = configFile("previous-service.json").toUtf8();
	if (obs_data_t *d = obs_data_create_from_json_file_safe(file.constData(), "bak")) {
		if (obs_data_t *s = obs_data_get_obj(d, "settings")) {
			rt.previous = serviceLabel(s);
			obs_data_release(s);
		}
		obs_data_release(d);
	}
}

// For the uninstaller (installer/multistream-obs.nsi), which can't ask OBS:
// the profile whose service.json the dock replaced, and the host it now
// sends to (put back only while that's still there). UTF-16 with a BOM, so
// Windows' INI reader keeps any letters in the folder's name.
void writeRestoreIni(const QString &server)
{
	char *dir = obs_frontend_get_current_profile_path();
	const QString profile = QDir::toNativeSeparators(str(dir));
	bfree(dir);
	const QString ini = "[service]\r\nprofile=" + profile + "\r\nhost=" + hostOf(server) + "\r\n";
	QFile f(configFile("restore.ini"));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return;
	f.write("\xFF\xFE", 2);
	f.write(reinterpret_cast<const char *>(ini.utf16()), ini.size() * 2);
}

} // namespace

void obsstate::onEvent(int event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		rt.starting = true;
		rt.stopping = false;
		rt.reached = false;
		outCode = 0;
		outStarting = false;
		watchOutput();
		// OBS starts the output right after this event. When that fails at
		// once (an encoder that won't open), OBS shows its own message and
		// sends no further event: "starting" would stay on for good.
		QTimer::singleShot(0, []() {
			if (rt.starting && !outStarting && !obs_frontend_streaming_active()) {
				rt.starting = false;
				noteStop(-4, false);
			}
		});
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		rt.starting = false;
		rt.reached = true;
		rt.stopAt = 0;
		rt.startedAt = now();
		rt.lastBytes = 0;
		rt.lastAt = 0;
		rt.kbps = 0;
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		rt.stopping = true;
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		// A code other than 0 (OBS_OUTPUT_SUCCESS): refused, or lost for good.
		if (outCode != 0)
			noteStop(int(outCode), rt.reached);
		rt.starting = false;
		rt.stopping = false;
		rt.stoppedAt = now();
		rt.kbps = 0;
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		unwatch();
		break;
	default:
		break;
	}
}

void obsstate::tick()
{
	obs_output_t *out = obs_frontend_get_streaming_output();
	if (!out) {
		rt.kbps = 0;
		return;
	}
	if (obs_output_active(out)) {
		const uint64_t bytes = obs_output_get_total_bytes(out);
		const qint64 t = now();
		// bytes * 8 / ms = kilobits per second
		if (rt.lastAt && t > rt.lastAt && bytes >= rt.lastBytes)
			rt.kbps = int(double(bytes - rt.lastBytes) * 8.0 / double(t - rt.lastAt));
		rt.lastBytes = bytes;
		rt.lastAt = t;
	} else {
		rt.kbps = 0;
		rt.lastAt = 0;
	}
	obs_output_release(out);
}

QJsonObject obsstate::snapshot()
{
	QJsonObject o;
	o["obsVersion"] = str(obs_get_version_string());
	o["streaming"] = obs_frontend_streaming_active();
	o["starting"] = rt.starting;
	o["stopping"] = rt.stopping;
	o["startedAt"] = double(rt.startedAt);
	o["stoppedAt"] = double(rt.stoppedAt);
	o["kbps"] = rt.kbps;

	if (obs_output_t *out = obs_frontend_get_streaming_output()) {
		o["reconnecting"] = obs_output_reconnecting(out);
		o["dropped"] = obs_output_get_frames_dropped(out);
		o["frames"] = obs_output_get_total_frames(out);
		o["congestion"] = double(obs_output_get_congestion(out));
		o["lastError"] = str(obs_output_get_last_error(out));
		obs_output_release(out);
	}
	if (rt.stopAt) {
		QJsonObject ls;
		ls["at"] = double(rt.stopAt);
		ls["code"] = rt.stopCode;
		ls["error"] = rt.stopError;
		ls["started"] = rt.stopStarted;
		o["lastStop"] = ls;
	}
	if (!rt.previousLoaded)
		loadPrevious();
	if (!rt.previous.isEmpty())
		o["previous"] = rt.previous;

	if (obs_service_t *svc = obs_frontend_get_streaming_service()) {
		obs_data_t *s = obs_service_get_settings(svc);
		QJsonObject so;
		so["type"] = str(obs_service_get_type(svc));
		so["service"] = str(obs_data_get_string(s, "service"));
		so["server"] = str(obs_data_get_string(s, "server"));
		so["key"] = str(obs_data_get_string(s, "key"));
		so["url"] = str(obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL));
		obs_data_release(s);
		o["service"] = so;
	}

	// How OBS itself keeps up (the same numbers as OBS's Stats window), for
	// the stream's health log on multistream.gg: frames it missed drawing
	// (rendering lag) and encoding (encoding lag), counted since OBS started,
	// its CPU use and the frame rate it's really making.
	{
		QJsonObject pc;
		QJsonObject render;
		render["lagged"] = double(obs_get_lagged_frames());
		render["total"] = double(obs_get_total_frames());
		pc["render"] = render;
		if (video_t *video = obs_get_video()) {
			QJsonObject encode;
			encode["skipped"] = double(video_output_get_skipped_frames(video));
			encode["total"] = double(video_output_get_total_frames(video));
			pc["encode"] = encode;
		}
		if (rt.cpu)
			pc["cpu"] = os_cpu_usage_info_query(rt.cpu);
		pc["fps"] = obs_get_active_fps();
		pc["memoryMB"] = double(os_get_proc_resident_size() / (1024 * 1024));
		o["pc"] = pc;
	}

	obs_video_info ovi = {};
	if (obs_get_video_info(&ovi)) {
		QJsonObject v;
		v["baseWidth"] = int(ovi.base_width);
		v["baseHeight"] = int(ovi.base_height);
		v["width"] = int(ovi.output_width);
		v["height"] = int(ovi.output_height);
		v["fps"] = ovi.fps_den ? double(ovi.fps_num) / double(ovi.fps_den) : 0.0;
		o["video"] = v;
	}

	config_t *cfg = obs_frontend_get_profile_config();
	if (cfg) {
		QJsonObject e;
		if (!advancedMode(cfg)) {
			const QString enc = str(config_get_string(cfg, "SimpleOutput", "StreamEncoder"));
			e["mode"] = "simple";
			e["encoder"] = enc;
			e["codec"] = codecOfSimple(enc);
			e["bitrate"] = int(config_get_uint(cfg, "SimpleOutput", "VBitrate"));
			e["keyintSec"] = 2;
			e["rateControl"] = "CBR";
		} else {
			const char *id = config_get_string(cfg, "AdvOut", "Encoder");
			e["mode"] = "advanced";
			e["encoder"] = str(id);
			e["codec"] = str(id ? obs_get_encoder_codec(id) : nullptr);
			obs_data_t *merged = id ? obs_encoder_defaults(id) : obs_data_create();
			if (!merged)
				merged = obs_data_create();
			const QByteArray path = profileFile("streamEncoder.json").toUtf8();
			if (obs_data_t *saved = obs_data_create_from_json_file_safe(path.constData(), "bak")) {
				obs_data_apply(merged, saved);
				obs_data_release(saved);
			}
			e["bitrate"] = int(obs_data_get_int(merged, "bitrate"));
			e["keyintSec"] = int(obs_data_get_int(merged, "keyint_sec"));
			e["rateControl"] = str(obs_data_get_string(merged, "rate_control"));
			obs_data_release(merged);
		}
		o["encoder"] = e;
	}
	return o;
}

bool obsstate::setService(const QString &server, const QString &key, QString &error)
{
	if (obs_frontend_streaming_active()) {
		error = mt("Obs.EndStreamFirst");
		return false;
	}
	if (server.isEmpty() || key.isEmpty()) {
		error = mt("Obs.ServerKeyMissing");
		return false;
	}
	const QByteArray srv = server.toUtf8(), k = key.toUtf8();

	// Keep the service it replaces (once: not when it's already ours).
	if (obs_service_t *old = obs_frontend_get_streaming_service()) {
		obs_data_t *os = obs_service_get_settings(old);
		if (str(obs_data_get_string(os, "server")) != server) {
			char *dir = obs_module_config_path("");
			os_mkdirs(dir);
			bfree(dir);
			obs_data_t *keep = obs_data_create();
			obs_data_set_string(keep, "type", obs_service_get_type(old));
			obs_data_set_obj(keep, "settings", os);
			char *file = obs_module_config_path("previous-service.json");
			obs_data_save_json_safe(keep, file, "tmp", "bak");
			bfree(file);
			obs_data_release(keep);
			writeRestoreIni(server);
			rt.previousLoaded = false;
		}
		obs_data_release(os);
	}

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "server", srv.constData());
	obs_data_set_string(s, "key", k.constData());
	obs_data_set_bool(s, "use_auth", false);
	obs_service_t *svc = obs_service_create("rtmp_custom", "default_service", s, nullptr);
	obs_data_release(s);
	if (!svc) {
		error = mt("Obs.NoCustomService");
		return false;
	}
	obs_frontend_set_streaming_service(svc);
	obs_frontend_save_streaming_service();
	obs_service_release(svc);
	return true;
}

bool obsstate::setEncoder(int bitrate, int keyintSec, QString &error)
{
	if (obs_frontend_streaming_active()) {
		error = mt("Obs.EndStreamFirst");
		return false;
	}
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg) {
		error = mt("Obs.NoProfile");
		return false;
	}
	if (!advancedMode(cfg)) {
		if (bitrate > 0)
			config_set_uint(cfg, "SimpleOutput", "VBitrate", uint64_t(bitrate));
		config_save_safe(cfg, "tmp", nullptr);
		return true;
	}
	const QByteArray path = profileFile("streamEncoder.json").toUtf8();
	obs_data_t *js = obs_data_create_from_json_file_safe(path.constData(), "bak");
	if (!js)
		js = obs_data_create();
	if (bitrate > 0)
		obs_data_set_int(js, "bitrate", bitrate);
	if (keyintSec > 0)
		obs_data_set_int(js, "keyint_sec", keyintSec);
	const bool ok = obs_data_save_json_safe(js, path.constData(), "tmp", "bak");
	obs_data_release(js);
	if (!ok)
		error = mt("Obs.EncoderNotSaved");
	return ok;
}

bool obsstate::restoreService(QString &error)
{
	if (obs_frontend_streaming_active()) {
		error = mt("Obs.EndStreamFirst");
		return false;
	}
	const QString path = configFile("previous-service.json");
	const QByteArray file = path.toUtf8();
	obs_data_t *d = obs_data_create_from_json_file_safe(file.constData(), "bak");
	if (!d) {
		error = mt("Obs.NoEarlierSetting");
		return false;
	}
	obs_data_t *s = obs_data_get_obj(d, "settings");
	const QByteArray type(obs_data_get_string(d, "type"));
	obs_service_t *svc = s && !type.isEmpty() ? obs_service_create(type.constData(), "default_service", s, nullptr) : nullptr;
	if (s)
		obs_data_release(s);
	obs_data_release(d);
	if (!svc) {
		error = mt("Obs.CantRestore");
		return false;
	}
	obs_frontend_set_streaming_service(svc);
	obs_frontend_save_streaming_service();
	obs_service_release(svc);
	QFile::remove(path);
	QFile::remove(path + ".bak");
	QFile::remove(configFile("restore.ini"));
	rt.previousLoaded = false;
	return true;
}
