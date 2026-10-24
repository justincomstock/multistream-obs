#include "obs-state.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QDateTime>

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
};
Runtime rt;

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

} // namespace

void obsstate::onEvent(int event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		rt.starting = true;
		rt.stopping = false;
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		rt.starting = false;
		rt.startedAt = now();
		rt.lastBytes = 0;
		rt.lastAt = 0;
		rt.kbps = 0;
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		rt.stopping = true;
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		rt.starting = false;
		rt.stopping = false;
		rt.stoppedAt = now();
		rt.kbps = 0;
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
		error = "End the stream first.";
		return false;
	}
	if (server.isEmpty() || key.isEmpty()) {
		error = "The server or stream key is missing.";
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
		error = "OBS couldn't make a Custom stream service.";
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
		error = "End the stream first.";
		return false;
	}
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg) {
		error = "OBS has no profile open.";
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
		error = "OBS's encoder settings couldn't be saved.";
	return ok;
}
