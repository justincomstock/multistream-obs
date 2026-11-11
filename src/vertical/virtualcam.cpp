#include "virtualcam.hpp"

#include "canvas.hpp"
#include "../i18n.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QMetaObject>

namespace {

const char *CONFIG_FILE = "vertical-virtualcam.json";

template<typename F> void onUi(F fn)
{
	if (QCoreApplication *app = QCoreApplication::instance())
		QMetaObject::invokeMethod(app, fn, Qt::QueuedConnection);
}

// OBS's virtual camera output (new reference), or null when this computer
// has none (Linux without v4l2loopback) or OBS hasn't made it yet.
obs_output_t *obsCamera()
{
	return obs_frontend_get_virtualcam_output();
}

} // namespace

void VerticalVirtualCam::load()
{
	registerHotkey();
	char *file = obs_module_config_path(CONFIG_FILE);
	obs_data_t *d = file ? obs_data_create_from_json_file_safe(file, "bak") : nullptr;
	bfree(file);
	if (d) {
		obs_data_array_t *a = obs_data_get_array(d, "hotkey");
		if (a)
			obs_hotkey_load(hk_, a);
		obs_data_array_release(a);
		obs_data_release(d);
	}
}

void VerticalVirtualCam::registerHotkey()
{
	hk_ = obs_hotkey_register_frontend(
		"multistream_vertical_virtualcam", obs_module_text("Vertical.HotkeyCam"),
		[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (!pressed)
				return;
			auto *self = static_cast<VerticalVirtualCam *>(data);
			onUi([self]() {
				QString error;
				if (self->active())
					self->stop();
				// A key press means "vertical now": take over from OBS's own.
				else
					self->start(error, true);
			});
		},
		this);
}

bool VerticalVirtualCam::active() const
{
	return out_ && obs_output_active(out_);
}

bool VerticalVirtualCam::mainActive() const
{
	return !active() && obs_frontend_virtualcam_active();
}

bool VerticalVirtualCam::available() const
{
	obs_output_t *o = obsCamera();
	const bool ok = o != nullptr;
	obs_output_release(o);
	return ok;
}

bool VerticalVirtualCam::start(QString &error, bool switchFromMain)
{
	if (active())
		return true;
	if (!canvas_->ready()) {
		error = error_ = mt(canvas_->waitsForOutputs() ? "Vertical.CamWaits" : "Vertical.NotReady");
		return false;
	}
	if (obs_frontend_virtualcam_active()) {
		if (!switchFromMain) {
			error = mt("Vertical.CamBusy");
			return false;
		}
		// Ours starts when OBS says its own has stopped (VIRTUALCAM_STOPPED).
		switching_ = true;
		obs_frontend_stop_virtualcam();
		obs_log(LOG_INFO, "vertical: switching OBS's virtual camera to the vertical picture");
		return true;
	}
	return startNow(error);
}

bool VerticalVirtualCam::startNow(QString &error)
{
	switching_ = false;
	obs_output_t *o = obsCamera();
	if (!o) {
#if defined(__linux__)
		error = error_ = mt("Vertical.CamMissingLinux");
#else
		error = error_ = mt("Vertical.CamMissing");
#endif
		return false;
	}
	if (obs_output_active(o)) {
		obs_output_release(o);
		error = mt("Vertical.CamBusy");
		return false;
	}
	video_t *video = canvas_->video();
	if (!video) {
		obs_output_release(o);
		error = error_ = mt("Vertical.NotReady");
		return false;
	}
	// Same as Aitum Vertical: OBS's camera, fed the vertical canvas.
	obs_output_set_media(o, video, obs_get_audio());
	signal_handler_connect(obs_output_get_signal_handler(o), "stop", onStop, this);
	out_ = o;
	if (!obs_output_start(o)) {
		const char *last = obs_output_get_last_error(o);
		signal_handler_disconnect(obs_output_get_signal_handler(o), "stop", onStop, this);
		restoreMain();
#ifdef __APPLE__
		// macOS installs the camera the first time OBS's own button starts it.
		error = error_ = mt("Vertical.CamMacInstall");
#else
		error = error_ = last && *last ? QString::fromUtf8(last) : mt("Vertical.CamNoStart");
#endif
		obs_log(LOG_WARNING, "vertical: virtual camera didn't start: %s", last ? last : "?");
		return false;
	}
	error_.clear();
	obs_log(LOG_INFO, "vertical: virtual camera on (%dx%d)", canvas_->width(), canvas_->height());
	return true;
}

void VerticalVirtualCam::stop()
{
	switching_ = false;
	if (active())
		obs_output_stop(out_);
	else
		restoreMain();
}

// OBS's camera goes back to the normal picture, ready for OBS's own button.
void VerticalVirtualCam::restoreMain()
{
	if (!out_)
		return;
	if (!obs_output_active(out_))
		obs_output_set_media(out_, obs_get_video(), obs_get_audio());
	obs_output_release(out_);
	out_ = nullptr;
}

void VerticalVirtualCam::onStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<VerticalVirtualCam *>(data);
	const long long code = calldata_int(cd, "code");
	onUi([self, code]() {
		if (!self->out_ || obs_output_active(self->out_))
			return;
		signal_handler_disconnect(obs_output_get_signal_handler(self->out_), "stop", onStop, self);
		self->restoreMain();
		if (code != 0) {
			self->error_ = mt("Vertical.CamStopped").arg(code);
			obs_log(LOG_WARNING, "vertical: virtual camera stopped (code %lld)", code);
		} else {
			obs_log(LOG_INFO, "vertical: virtual camera off");
		}
	});
}

void VerticalVirtualCam::onFrontendEvent(int event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
		// OBS's own camera has let go: ours takes over (after OBS's handler).
		if (switching_)
			onUi([this]() {
				if (!switching_)
					return;
				QString error;
				startNow(error);
			});
		break;
	// The canvas goes away with the collection: let go of its picture first,
	// then come back on once the new collection's canvas is up.
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		resume_ = active();
		switching_ = false;
		if (out_) {
			signal_handler_disconnect(obs_output_get_signal_handler(out_), "stop", onStop, this);
			if (obs_output_active(out_))
				obs_output_force_stop(out_);
			restoreMain();
		}
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		if (resume_) {
			resume_ = false;
			// The new canvas gets its picture a moment later.
			onUi([this]() {
				QString error;
				if (!start(error, true))
					error_ = error;
			});
		}
		break;
	default:
		break;
	}
}

void VerticalVirtualCam::shutdown()
{
	switching_ = resume_ = false;
	if (out_) {
		signal_handler_disconnect(obs_output_get_signal_handler(out_), "stop", onStop, this);
		if (obs_output_active(out_))
			obs_output_force_stop(out_);
		restoreMain();
	}
	if (hk_ != OBS_INVALID_HOTKEY_ID) {
		obs_data_t *d = obs_data_create();
		obs_data_array_t *a = obs_hotkey_save(hk_);
		obs_data_set_array(d, "hotkey", a);
		obs_data_array_release(a);
		char *dir = obs_module_config_path("");
		os_mkdirs(dir);
		bfree(dir);
		char *file = obs_module_config_path(CONFIG_FILE);
		obs_data_save_json_safe(d, file, "tmp", "bak");
		bfree(file);
		obs_data_release(d);
		obs_hotkey_unregister(hk_);
		hk_ = OBS_INVALID_HOTKEY_ID;
	}
}

QJsonObject VerticalVirtualCam::state() const
{
	QJsonObject o;
	o["active"] = active();
	o["mainActive"] = mainActive();
	o["available"] = available();
	o["switching"] = switching_;
	o["error"] = error_;
	if (active()) {
		o["width"] = canvas_->width();
		o["height"] = canvas_->height();
	}
	return o;
}
