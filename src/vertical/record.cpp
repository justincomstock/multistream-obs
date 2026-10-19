#include "record.hpp"

#include "canvas.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QMetaObject>

namespace {

const char *CONFIG_FILE = "vertical-record.json";

struct Enc {
	const char *id;
	const char *label;
};
// Hardware first; x264 (CPU) last. Same order as the stream's.
const Enc ENCODERS[] = {
	{"obs_nvenc_h264_tex", "NVIDIA NVENC"},
	{"jim_nvenc", "NVIDIA NVENC"},
	{"h264_texture_amf", "AMD"},
	{"obs_qsv11_v2", "Intel Quick Sync"},
	{"obs_qsv11", "Intel Quick Sync"},
	{"com.apple.videotoolbox.videoencoder.ave.avc", "Apple VideoToolbox"},
	{"obs_x264", "x264 (CPU)"},
};

template<typename F> void onUi(F fn)
{
	if (QCoreApplication *app = QCoreApplication::instance())
		QMetaObject::invokeMethod(app, fn, Qt::QueuedConnection);
}

bool active(obs_output_t *o)
{
	return o && obs_output_active(o);
}

void releaseOutput(obs_output_t *&o)
{
	if (!o)
		return;
	if (obs_output_active(o))
		obs_output_force_stop(o);
	obs_output_release(o);
	o = nullptr;
}

} // namespace

void VerticalRecorder::load()
{
	char *file = obs_module_config_path(CONFIG_FILE);
	obs_data_t *d = obs_data_create_from_json_file_safe(file, "bak");
	bfree(file);
	if (d) {
		obs_data_set_default_bool(d, "followMain", true);
		followMain_ = obs_data_get_bool(d, "followMain");
		const QString f = QString::fromUtf8(obs_data_get_string(d, "format"));
		if (f == "mkv" || f == "mp4")
			format_ = f;
		if (const int s = int(obs_data_get_int(d, "replaySeconds")); s >= 5 && s <= 600)
			replaySeconds_ = s;
		if (const int b = int(obs_data_get_int(d, "bitrate")); b >= 1000 && b <= 100000)
			bitrate_ = b;
	}
	registerHotkeys();
	if (d) {
		auto loadKey = [&](obs_hotkey_id id, const char *key) {
			obs_data_array_t *a = obs_data_get_array(d, key);
			if (a)
				obs_hotkey_load(id, a);
			obs_data_array_release(a);
		};
		loadKey(hkRecord_, "hotkeyRecord");
		loadKey(hkReplay_, "hotkeyReplay");
		loadKey(hkSave_, "hotkeySave");
		obs_data_release(d);
	}
}

void VerticalRecorder::registerHotkeys()
{
	hkRecord_ = obs_hotkey_register_frontend(
		"multistream_vertical_record", "Multistream: Start / stop vertical recording",
		[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (!pressed)
				return;
			auto *self = static_cast<VerticalRecorder *>(data);
			onUi([self]() {
				QString error;
				if (self->recording())
					self->stopRecording();
				else
					self->startRecording(error);
			});
		},
		this);
	hkReplay_ = obs_hotkey_register_frontend(
		"multistream_vertical_replay", "Multistream: Start / stop vertical replay buffer",
		[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (!pressed)
				return;
			auto *self = static_cast<VerticalRecorder *>(data);
			onUi([self]() {
				QString error;
				if (self->replayActive())
					self->stopReplay();
				else
					self->startReplay(error);
			});
		},
		this);
	hkSave_ = obs_hotkey_register_frontend(
		"multistream_vertical_save", "Multistream: Save vertical replay",
		[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (!pressed)
				return;
			auto *self = static_cast<VerticalRecorder *>(data);
			onUi([self]() {
				QString error;
				self->saveReplay(error);
			});
		},
		this);
}

void VerticalRecorder::save() const
{
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "followMain", followMain_);
	obs_data_set_string(d, "format", format_.toUtf8().constData());
	obs_data_set_int(d, "replaySeconds", replaySeconds_);
	obs_data_set_int(d, "bitrate", bitrate_);
	auto saveKey = [&](obs_hotkey_id id, const char *key) {
		if (id == OBS_INVALID_HOTKEY_ID)
			return;
		obs_data_array_t *a = obs_hotkey_save(id);
		obs_data_set_array(d, key, a);
		obs_data_array_release(a);
	};
	saveKey(hkRecord_, "hotkeyRecord");
	saveKey(hkReplay_, "hotkeyReplay");
	saveKey(hkSave_, "hotkeySave");
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	char *file = obs_module_config_path(CONFIG_FILE);
	obs_data_save_json_safe(d, file, "tmp", "bak");
	bfree(file);
	obs_data_release(d);
}

QString VerticalRecorder::folder() const
{
	char *p = obs_frontend_get_current_record_output_path();
	QString out = p ? QString::fromUtf8(p) : QString();
	bfree(p);
	if (out.isEmpty() || !QDir(out).exists())
		out = QDir::homePath() + "/Videos";
	return out;
}

bool VerticalRecorder::encoders(QString &error)
{
	if (!canvas_->ready() || !canvas_->audio()) {
		error = "The vertical canvas isn't ready yet.";
		return false;
	}
	// Rebuilt only while nothing records (a new picture size, or a new canvas).
	const QString sig = canvas_->sig() + QString::number(quintptr(canvas_->video())) + QString::number(bitrate_);
	if (venc_ && aenc_ && (sig == encSig_ || active(record_) || active(replay_)))
		return true;
	releaseOutput(record_);
	releaseOutput(replay_);
	obs_encoder_release(venc_);
	obs_encoder_release(aenc_);
	venc_ = aenc_ = nullptr;

	obs_data_t *vs = obs_data_create();
	obs_data_set_string(vs, "rate_control", "CBR");
	obs_data_set_int(vs, "bitrate", bitrate_);
	obs_data_set_int(vs, "keyint_sec", 2);
	obs_data_set_string(vs, "profile", "high");
	obs_data_set_string(vs, "preset", "veryfast");
	obs_data_set_string(vs, "preset2", "p5");
	for (const Enc &e : ENCODERS) {
		venc_ = obs_video_encoder_create(e.id, "Multistream vertical recording video", vs, nullptr);
		if (venc_)
			break;
	}
	obs_data_release(vs);
	obs_data_t *as = obs_data_create();
	obs_data_set_int(as, "bitrate", 192);
	aenc_ = obs_audio_encoder_create("ffmpeg_aac", "Multistream vertical recording audio", as, 0, nullptr);
	if (!aenc_)
		aenc_ = obs_audio_encoder_create("CoreAudio_AAC", "Multistream vertical recording audio", as, 0, nullptr);
	obs_data_release(as);
	if (!venc_ || !aenc_) {
		error = "No encoder could be made for the vertical recording.";
		obs_encoder_release(venc_);
		obs_encoder_release(aenc_);
		venc_ = aenc_ = nullptr;
		return false;
	}
	obs_encoder_set_video(venc_, canvas_->video());
	obs_encoder_set_audio(aenc_, canvas_->audio());
	encSig_ = sig;
	return true;
}

void VerticalRecorder::releaseEncodersIfIdle()
{
	if (active(record_) || active(replay_))
		return;
	releaseOutput(record_);
	releaseOutput(replay_);
	obs_encoder_release(venc_);
	obs_encoder_release(aenc_);
	venc_ = aenc_ = nullptr;
	encSig_.clear();
}

bool VerticalRecorder::recording() const
{
	return active(record_);
}

bool VerticalRecorder::replayActive() const
{
	return active(replay_);
}

void VerticalRecorder::onRecordStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<VerticalRecorder *>(data);
	const long long code = calldata_int(cd, "code");
	auto *out = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	const char *last = out ? obs_output_get_last_error(out) : nullptr;
	const QString err = code == 0 ? QString()
				      : (last && *last ? QString::fromUtf8(last)
						       : QString("The vertical recording stopped (code %1).").arg(code));
	onUi([self, err]() {
		self->error_ = err;
		if (!err.isEmpty())
			obs_log(LOG_WARNING, "vertical: recording stopped: %s", err.toUtf8().constData());
		self->releaseEncodersIfIdle();
	});
}

void VerticalRecorder::onReplayStop(void *data, calldata_t *cd)
{
	onRecordStop(data, cd);
}

void VerticalRecorder::onReplaySaved(void *data, calldata_t *)
{
	auto *self = static_cast<VerticalRecorder *>(data);
	onUi([self]() {
		if (!self->replay_)
			return;
		calldata_t cd = {};
		proc_handler_t *ph = obs_output_get_proc_handler(self->replay_);
		if (proc_handler_call(ph, "get_last_replay", &cd)) {
			const char *p = calldata_string(&cd, "path");
			self->lastReplay_ = p ? QString::fromUtf8(p) : QString();
			obs_log(LOG_INFO, "vertical: replay saved to %s", p ? p : "?");
		}
		calldata_free(&cd);
	});
}

bool VerticalRecorder::startRecording(QString &error)
{
	if (recording())
		return true;
	if (!encoders(error)) {
		error_ = error;
		return false;
	}
	releaseOutput(record_);
	const QString ext = format_ == "mkv" ? "mkv" : "mp4";
	const QString path = folder() + "/Vertical " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh-mm-ss") + "." + ext;
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "path", path.toUtf8().constData());
	// OBS's hybrid MP4 survives a crash; plain ffmpeg MP4 wouldn't.
	if (ext == "mp4")
		record_ = obs_output_create("mp4_output", "Multistream vertical recording", s, nullptr);
	if (!record_)
		record_ = obs_output_create("ffmpeg_muxer", "Multistream vertical recording", s, nullptr);
	obs_data_release(s);
	if (!record_) {
		error = error_ = "OBS couldn't make the vertical recording.";
		return false;
	}
	obs_output_set_video_encoder(record_, venc_);
	obs_output_set_audio_encoder(record_, aenc_, 0);
	signal_handler_connect(obs_output_get_signal_handler(record_), "stop", onRecordStop, this);
	if (!obs_output_start(record_)) {
		const char *last = obs_output_get_last_error(record_);
		error = error_ = last && *last ? QString::fromUtf8(last) : "The vertical recording didn't start.";
		releaseEncodersIfIdle();
		return false;
	}
	lastFile_ = path;
	error_.clear();
	obs_log(LOG_INFO, "vertical: recording to %s", path.toUtf8().constData());
	return true;
}

void VerticalRecorder::stopRecording()
{
	if (active(record_))
		obs_output_stop(record_);
}

bool VerticalRecorder::startReplay(QString &error)
{
	if (replayActive())
		return true;
	if (!encoders(error)) {
		error_ = error;
		return false;
	}
	releaseOutput(replay_);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "directory", folder().toUtf8().constData());
	obs_data_set_string(s, "format", "Vertical Replay %CCYY-%MM-%DD %hh-%mm-%ss");
	obs_data_set_string(s, "extension", format_ == "mkv" ? "mkv" : "mp4");
	obs_data_set_bool(s, "allow_spaces", true);
	obs_data_set_int(s, "max_time_sec", replaySeconds_);
	obs_data_set_int(s, "max_size_mb", 0);
	replay_ = obs_output_create("replay_buffer", "Multistream vertical replay buffer", s, nullptr);
	obs_data_release(s);
	if (!replay_) {
		error = error_ = "OBS couldn't make the vertical replay buffer.";
		return false;
	}
	obs_output_set_video_encoder(replay_, venc_);
	obs_output_set_audio_encoder(replay_, aenc_, 0);
	signal_handler_t *sh = obs_output_get_signal_handler(replay_);
	signal_handler_connect(sh, "stop", onReplayStop, this);
	signal_handler_connect(sh, "saved", onReplaySaved, this);
	if (!obs_output_start(replay_)) {
		const char *last = obs_output_get_last_error(replay_);
		error = error_ = last && *last ? QString::fromUtf8(last) : "The vertical replay buffer didn't start.";
		releaseEncodersIfIdle();
		return false;
	}
	error_.clear();
	return true;
}

void VerticalRecorder::stopReplay()
{
	if (active(replay_))
		obs_output_stop(replay_);
}

bool VerticalRecorder::saveReplay(QString &error)
{
	if (!replayActive()) {
		error = "The vertical replay buffer isn't running.";
		return false;
	}
	calldata_t cd = {};
	proc_handler_call(obs_output_get_proc_handler(replay_), "save", &cd);
	calldata_free(&cd);
	return true;
}

void VerticalRecorder::onFrontendEvent(int event)
{
	QString ignored;
	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		if (followMain_)
			startRecording(ignored);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
		if (followMain_)
			stopRecording();
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
		if (followMain_)
			startReplay(ignored);
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPING:
		if (followMain_)
			stopReplay();
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
		if (followMain_ && replayActive())
			saveReplay(ignored);
		break;
	// The canvas goes away with the collection: let go of its video first.
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		releaseOutput(record_);
		releaseOutput(replay_);
		releaseEncodersIfIdle();
		break;
	default:
		break;
	}
}

void VerticalRecorder::shutdown()
{
	if (hkRecord_ != OBS_INVALID_HOTKEY_ID)
		save();
	releaseOutput(record_);
	releaseOutput(replay_);
	releaseEncodersIfIdle();
	for (obs_hotkey_id *id : {&hkRecord_, &hkReplay_, &hkSave_}) {
		if (*id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(*id);
		*id = OBS_INVALID_HOTKEY_ID;
	}
}

QJsonObject VerticalRecorder::state() const
{
	QJsonObject o;
	o["recording"] = recording();
	o["replay"] = replayActive();
	o["followMain"] = followMain_;
	o["format"] = format_;
	o["replaySeconds"] = replaySeconds_;
	o["bitrate"] = bitrate_;
	o["folder"] = folder();
	o["lastFile"] = lastFile_;
	o["lastReplay"] = lastReplay_;
	o["error"] = error_;
	return o;
}

void VerticalRecorder::configure(const QJsonObject &in)
{
	if (in.contains("followMain"))
		followMain_ = in.value("followMain").toBool();
	if (in.value("format").toString() == "mp4" || in.value("format").toString() == "mkv")
		format_ = in.value("format").toString();
	if (in.contains("replaySeconds"))
		replaySeconds_ = qBound(5, in.value("replaySeconds").toInt(), 600);
	if (in.contains("bitrate"))
		bitrate_ = qBound(1000, in.value("bitrate").toInt(), 100000);
	save();
}
