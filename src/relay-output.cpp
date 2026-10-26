#include "relay-output.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QMetaObject>

namespace {

const char *CLIENT = "multistream-obs-plugin";
const qint64 RETRY_MS = 5000;

struct Enc {
	const char *id;
	const char *label;
};
// Hardware first; x264 (CPU) last. Names differ across OBS versions.
const Enc ENCODERS[] = {
	{"obs_nvenc_h264_tex", "NVIDIA NVENC"},
	{"jim_nvenc", "NVIDIA NVENC"},
	{"h264_texture_amf", "AMD"},
	{"obs_qsv11_v2", "Intel Quick Sync"},
	{"obs_qsv11", "Intel Quick Sync"},
	{"com.apple.videotoolbox.videoencoder.ave.avc", "Apple VideoToolbox"},
	{"obs_x264", "x264 (CPU)"},
};

QString stopReason(long long code)
{
	switch (code) {
	case -1:
		return "The Multistream address is wrong.";
	case -2:
		return "Couldn't reach Multistream.";
	case -3:
		return "Multistream turned the stream away.";
	case -4:
		return "The Multistream output hit an error.";
	case -5:
		return "Lost the connection to Multistream.";
	case -6:
		return "This encoder can't be used for the Multistream output.";
	case -8:
		return "The encoder failed.";
	default:
		return QString("The Multistream output stopped (code %1).").arg(code);
	}
}

qint64 now()
{
	return QDateTime::currentMSecsSinceEpoch();
}

// Runs `fn` on the UI thread.
template<typename F> void onUi(F fn)
{
	if (QCoreApplication *app = QCoreApplication::instance())
		QMetaObject::invokeMethod(app, fn, Qt::QueuedConnection);
}

} // namespace

RelayOutput::RelayOutput(const char *file, const char *name, const char *feed)
	: file_(file),
	  name_(name),
	  feed_(feed),
	  video_([]() { return obs_get_video(); }),
	  audio_([]() { return obs_get_audio(); })
{
}

void RelayOutput::setMedia(std::function<video_t *()> video, std::function<audio_t *()> audio,
			   std::function<QString()> sig)
{
	video_ = std::move(video);
	audio_ = std::move(audio);
	mediaSig_ = std::move(sig);
}

void RelayOutput::load()
{
	char *file = obs_module_config_path(file_.toUtf8().constData());
	if (obs_data_t *d = obs_data_create_from_json_file_safe(file, "bak")) {
		obs_data_set_default_int(d, "bitrate", 6000);
		enabled_ = obs_data_get_bool(d, "enabled");
		server_ = QString::fromUtf8(obs_data_get_string(d, "server"));
		key_ = QString::fromUtf8(obs_data_get_string(d, "key"));
		bitrate_ = int(obs_data_get_int(d, "bitrate"));
		obs_data_release(d);
	}
	bfree(file);
}

void RelayOutput::save() const
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "enabled", enabled_);
	obs_data_set_string(d, "server", server_.toUtf8().constData());
	obs_data_set_string(d, "key", key_.toUtf8().constData());
	obs_data_set_int(d, "bitrate", bitrate_);
	char *file = obs_module_config_path(file_.toUtf8().constData());
	obs_data_save_json_safe(d, file, "tmp", "bak");
	bfree(file);
	obs_data_release(d);
}

void RelayOutput::configure(bool enabled, const QString &server, const QString &key, int bitrate)
{
	const bool was = enabled_;
	enabled_ = enabled && !server.isEmpty() && !key.isEmpty();
	server_ = server;
	key_ = key;
	bitrate_ = bitrate >= 500 && bitrate <= 50000 ? bitrate : 6000;
	save();
	if (!enabled_ && was)
		stopOutput();
	// Turned on mid-stream: join in.
	else if (enabled_ && obs_frontend_streaming_active()) {
		wanted_ = true;
		startOutput();
	}
}

QString RelayOutput::sig() const
{
	return server_ + "|" + key_ + "|" + QString::number(bitrate_) + "|" + (mediaSig_ ? mediaSig_() : QString());
}

void RelayOutput::releaseAll()
{
	if (output_) {
		signal_handler_t *sh = obs_output_get_signal_handler(output_);
		signal_handler_disconnect(sh, "start", onStart, this);
		signal_handler_disconnect(sh, "stop", onStop, this);
		signal_handler_disconnect(sh, "reconnect", onReconnect, this);
		signal_handler_disconnect(sh, "reconnect_success", onReconnectSuccess, this);
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (service_) {
		obs_service_release(service_);
		service_ = nullptr;
	}
	if (venc_) {
		obs_encoder_release(venc_);
		venc_ = nullptr;
	}
	if (aenc_) {
		obs_encoder_release(aenc_);
		aenc_ = nullptr;
	}
	builtSig_.clear();
}

bool RelayOutput::build()
{
	video_t *video = video_();
	audio_t *audio = audio_();
	if (!video || !audio) {
		error_ = feed_ == "vertical" ? "The vertical canvas isn't ready yet." : "OBS's video isn't ready yet.";
		return false;
	}
	const QByteArray name = name_.toUtf8();
	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", server_.toUtf8().constData());
	// The query tells Multistream this stream comes from the plugin.
	QString key = key_ + "?client=" + CLIENT + "&v=" + PLUGIN_VERSION;
	if (feed_ != "main")
		key += "&feed=" + feed_;
	obs_data_set_string(ss, "key", key.toUtf8().constData());
	service_ = obs_service_create("rtmp_custom", (name + " service").constData(), ss, nullptr);
	obs_data_release(ss);

	obs_data_t *vs = obs_data_create();
	obs_data_set_string(vs, "rate_control", "CBR");
	obs_data_set_int(vs, "bitrate", bitrate_);
	obs_data_set_int(vs, "keyint_sec", 2);
	obs_data_set_string(vs, "profile", "high");
	obs_data_set_string(vs, "preset", "veryfast"); // x264
	obs_data_set_string(vs, "preset2", "p5");      // NVENC
	for (const Enc &e : ENCODERS) {
		// OBS hands back a placeholder for an id it doesn't have (NVENC on a
		// Mac), so only try encoders this OBS actually registered.
		if (!obs_get_encoder_codec(e.id))
			continue;
		venc_ = obs_video_encoder_create(e.id, (name + " video").constData(), vs, nullptr);
		if (venc_) {
			encoder_ = e.label;
			break;
		}
	}
	obs_data_release(vs);
	if (!venc_ || !service_) {
		error_ = "No video encoder could be made for the Multistream output.";
		releaseAll();
		return false;
	}
	obs_encoder_set_video(venc_, video);

	obs_data_t *as = obs_data_create();
	obs_data_set_int(as, "bitrate", 160);
	const char *aac = obs_get_encoder_codec("ffmpeg_aac") ? "ffmpeg_aac" : "CoreAudio_AAC";
	aenc_ = obs_audio_encoder_create(aac, (name + " audio").constData(), as, 0, nullptr);
	obs_data_release(as);
	if (!aenc_) {
		error_ = "No AAC audio encoder could be made.";
		releaseAll();
		return false;
	}
	obs_encoder_set_audio(aenc_, audio);

	output_ = obs_output_create("rtmp_output", name.constData(), nullptr, nullptr);
	if (!output_) {
		error_ = "OBS couldn't make the Multistream output.";
		releaseAll();
		return false;
	}
	obs_output_set_service(output_, service_);
	obs_output_set_video_encoder(output_, venc_);
	obs_output_set_audio_encoder(output_, aenc_, 0);
	obs_output_set_reconnect_settings(output_, 60, 2);
	signal_handler_t *sh = obs_output_get_signal_handler(output_);
	signal_handler_connect(sh, "start", onStart, this);
	signal_handler_connect(sh, "stop", onStop, this);
	signal_handler_connect(sh, "reconnect", onReconnect, this);
	signal_handler_connect(sh, "reconnect_success", onReconnectSuccess, this);
	builtSig_ = sig();
	return true;
}

bool RelayOutput::startOutput()
{
	if (!enabled_)
		return false;
	if (attempting_ || (output_ && obs_output_active(output_)))
		return true;
	lastTry_ = now();
	if (!output_ || builtSig_ != sig()) {
		releaseAll();
		if (!build())
			return false;
	}
	attempting_ = true;
	if (!obs_output_start(output_)) {
		attempting_ = false;
		const char *err = obs_output_get_last_error(output_);
		error_ = err && *err ? QString::fromUtf8(err) : QString("OBS couldn't start the Multistream output.");
		obs_log(LOG_WARNING, "Multistream output didn't start: %s", error_.toUtf8().constData());
		return false;
	}
	startedAt_ = now();
	obs_log(LOG_INFO, "Multistream output: sending with %s", encoder_.toUtf8().constData());
	return true;
}

bool RelayOutput::startNow()
{
	if (!enabled_) {
		error_ = "Add the Multistream server and stream key first.";
		return false;
	}
	manual_ = true;
	wanted_ = true;
	return startOutput();
}

void RelayOutput::stopNow()
{
	stopOutput();
}

void RelayOutput::stopOutput()
{
	wanted_ = false;
	manual_ = false;
	if (output_)
		obs_output_stop(output_);
	sending_ = false;
	reconnecting_ = false;
}

void RelayOutput::onFrontendEvent(int event)
{
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED && enabled_) {
		wanted_ = true;
		startOutput();
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPING && !manual_) {
		stopOutput();
	}
}

void RelayOutput::tick()
{
	if (!wanted_ || attempting_ || (output_ && obs_output_active(output_)) || now() - lastTry_ < RETRY_MS)
		return;
	if (!enabled_ || (!manual_ && !obs_frontend_streaming_active()))
		wanted_ = false;
	else
		startOutput();
}

void RelayOutput::shutdown()
{
	wanted_ = false;
	if (output_ && obs_output_active(output_))
		obs_output_stop(output_);
	releaseAll();
}

QJsonObject RelayOutput::state() const
{
	QJsonObject o;
	o["feed"] = feed_;
	o["enabled"] = enabled_;
	o["manual"] = manual_;
	o["server"] = server_;
	o["bitrate"] = bitrate_;
	o["sending"] = sending_;
	o["reconnecting"] = reconnecting_;
	o["connecting"] = attempting_;
	o["encoder"] = encoder_;
	o["error"] = error_;
	o["startedAt"] = double(sending_ ? startedAt_ : 0);
	return o;
}

void RelayOutput::onStart(void *data, calldata_t *)
{
	auto *self = static_cast<RelayOutput *>(data);
	onUi([self]() {
		self->attempting_ = false;
		self->sending_ = true;
		self->reconnecting_ = false;
		self->error_.clear();
	});
}

void RelayOutput::onStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<RelayOutput *>(data);
	const long long code = calldata_int(cd, "code");
	obs_output_t *out = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	const char *err = out ? obs_output_get_last_error(out) : nullptr;
	QString detail = err && *err ? QString::fromUtf8(err) : QString();
	onUi([self, code, detail]() {
		self->attempting_ = false;
		self->sending_ = false;
		self->reconnecting_ = false;
		if (code != 0) {
			self->error_ = stopReason(code);
			if (!detail.isEmpty())
				self->error_ += " " + detail;
			if (self->wanted_)
				self->error_ += " Trying again every 5 seconds.";
		}
	});
}

void RelayOutput::onReconnect(void *data, calldata_t *)
{
	auto *self = static_cast<RelayOutput *>(data);
	onUi([self]() { self->reconnecting_ = true; });
}

void RelayOutput::onReconnectSuccess(void *data, calldata_t *)
{
	auto *self = static_cast<RelayOutput *>(data);
	onUi([self]() { self->reconnecting_ = false; });
}
