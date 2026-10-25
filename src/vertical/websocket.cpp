#include "websocket.hpp"

#include "canvas.hpp"
#include "docks.hpp"
#include "record.hpp"
#include "../relay-output.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <obs-websocket-api.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QThread>

#include <atomic>
#include <functional>

namespace {

VerticalCanvas *canvas = nullptr;
RelayOutput *output = nullptr;
std::atomic<bool> alive{false};

// Requests arrive on obs-websocket's thread; the canvas lives on the UI
// thread.
void onUi(const std::function<void()> &fn)
{
	QCoreApplication *app = QCoreApplication::instance();
	if (!app)
		return;
	if (QThread::currentThread() == app->thread())
		fn();
	else
		QMetaObject::invokeMethod(app, fn, Qt::BlockingQueuedConnection);
}

QJsonObject toJson(obs_data_t *data)
{
	const char *json = data ? obs_data_get_json(data) : nullptr;
	return json ? QJsonDocument::fromJson(QByteArray(json)).object() : QJsonObject();
}

void reply(obs_data_t *response, QJsonObject o)
{
	if (!o.contains("ok"))
		o["ok"] = true;
	const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Compact);
	if (obs_data_t *d = obs_data_create_from_json(json.constData())) {
		obs_data_apply(response, d);
		obs_data_release(d);
	}
}

QJsonObject fail(const QString &error)
{
	QJsonObject o;
	o["ok"] = false;
	o["error"] = error;
	return o;
}

using Handler = QJsonObject (*)(const QJsonObject &in);

// The scene a request names, or the one on air.
QString sceneOf(const QJsonObject &in)
{
	const QString s = in.value("scene").toString();
	return s.isEmpty() ? canvas->currentScene() : s;
}

// The item a request names: {id} or {source} (its source's name, topmost).
qint64 itemOf(const QJsonObject &in, QString &error)
{
	if (in.value("id").toDouble() > 0)
		return qint64(in.value("id").toDouble());
	const QString source = in.value("source").toString();
	for (const QJsonValue v : canvas->sceneItems(sceneOf(in)))
		if (v.toObject().value("name").toString() == source)
			return qint64(v.toObject().value("id").toDouble());
	error = QString("No source called \"%1\" in vertical scene \"%2\".").arg(source, sceneOf(in));
	return 0;
}

struct Request {
	const char *name;
	Handler handler;
};

const Request REQUESTS[] = {
	{"state",
	 [](const QJsonObject &) {
		 QJsonObject o = canvas->state();
		 o["output"] = output->state();
		 return o;
	 }},
	{"docks", [](const QJsonObject &) { return vertical_docks::report(); }},
	{"get_scenes",
	 [](const QJsonObject &) {
		 QJsonObject o;
		 o["scenes"] = canvas->sceneList();
		 o["current"] = canvas->currentScene();
		 return o;
	 }},
	{"switch_scene",
	 [](const QJsonObject &in) {
		 QString error;
		 return canvas->setScene(in.value("scene").toString(), error) ? QJsonObject() : fail(error);
	 }},
	{"create_scene",
	 [](const QJsonObject &in) {
		 QString error;
		 return canvas->createScene(in.value("scene").toString(), error) ? QJsonObject() : fail(error);
	 }},
	{"rename_scene",
	 [](const QJsonObject &in) {
		 QString error;
		 return canvas->renameScene(in.value("scene").toString(), in.value("to").toString(), error)
				? QJsonObject()
				: fail(error);
	 }},
	{"duplicate_scene",
	 [](const QJsonObject &in) {
		 QString error;
		 return canvas->duplicateScene(in.value("scene").toString(), in.value("to").toString(), error)
				? QJsonObject()
				: fail(error);
	 }},
	{"remove_scene",
	 [](const QJsonObject &in) {
		 QString error;
		 return canvas->removeScene(in.value("scene").toString(), error) ? QJsonObject() : fail(error);
	 }},
	{"move_scene",
	 [](const QJsonObject &in) {
		 QString error;
		 return canvas->moveScene(in.value("scene").toString(), in.value("delta").toInt(), error)
				? QJsonObject()
				: fail(error);
	 }},
	// {main, scene}: show vertical `scene` when main scene `main` is on air
	// (scene empty: unlink).
	{"link_scene",
	 [](const QJsonObject &in) {
		 QString error;
		 return canvas->linkScene(in.value("main").toString(), in.value("scene").toString(), error)
				? QJsonObject()
				: fail(error);
	 }},
	{"set_follow",
	 [](const QJsonObject &in) {
		 canvas->setFollowMain(in.value("on").toBool(true));
		 return QJsonObject();
	 }},
	{"add_source",
	 [](const QJsonObject &in) {
		 QString error;
		 return canvas->addSource(in.value("scene").toString(), in.value("source").toString(), error)
				? QJsonObject()
				: fail(error);
	 }},
	// {scene?}: the items in a vertical scene (default: the one on air).
	{"get_scene_items",
	 [](const QJsonObject &in) {
		 QJsonObject o;
		 o["scene"] = sceneOf(in);
		 o["items"] = canvas->sceneItems(sceneOf(in));
		 return o;
	 }},
	// {scene?, id | source, visible}
	{"set_item_visible",
	 [](const QJsonObject &in) {
		 QString error;
		 const qint64 id = itemOf(in, error);
		 return id && canvas->setItemVisible(sceneOf(in), id, in.value("visible").toBool(true), error)
				? QJsonObject()
				: fail(error);
	 }},
	{"set_item_locked",
	 [](const QJsonObject &in) {
		 QString error;
		 const qint64 id = itemOf(in, error);
		 return id && canvas->setItemLocked(sceneOf(in), id, in.value("locked").toBool(true), error)
				? QJsonObject()
				: fail(error);
	 }},
	{"get_item_transform",
	 [](const QJsonObject &in) {
		 QString error;
		 const qint64 id = itemOf(in, error);
		 if (!id)
			 return fail(error);
		 QJsonObject o;
		 o["transform"] = canvas->itemTransform(sceneOf(in), id);
		 return o;
	 }},
	// {scene?, id | source, transform: {x, y, scaleX, ...}}: only the keys given change.
	{"set_item_transform",
	 [](const QJsonObject &in) {
		 QString error;
		 const qint64 id = itemOf(in, error);
		 if (!id || !canvas->setItemTransform(sceneOf(in), id, in.value("transform").toObject(), error))
			 return fail(error);
		 QJsonObject o;
		 o["transform"] = canvas->itemTransform(sceneOf(in), id);
		 return o;
	 }},
	{"remove_item",
	 [](const QJsonObject &in) {
		 QString error;
		 const qint64 id = itemOf(in, error);
		 return id && canvas->removeItem(sceneOf(in), id, error) ? QJsonObject() : fail(error);
	 }},
	// {main, scene}: a new vertical scene laid out from main scene `main`.
	{"create_from_main",
	 [](const QJsonObject &in) {
		 QString error;
		 return canvas->createFromMain(in.value("main").toString(), in.value("scene").toString(), error)
				? QJsonObject()
				: fail(error);
	 }},
	{"get_audio", [](const QJsonObject &) { return canvas->audioState(); }},
	{"set_audio",
	 [](const QJsonObject &in) {
		 QString error;
		 return canvas->setAudio(in.value("source").toString(), in, error) ? canvas->audioState()
										    : fail(error);
	 }},
	{"set_output",
	 [](const QJsonObject &in) {
		 output->configure(in.value("enabled").toBool(), in.value("server").toString(),
				   in.value("key").toString(), in.value("bitrate").toInt(6000));
		 return output->state();
	 }},
	{"start_output",
	 [](const QJsonObject &) {
		 if (!output->startNow())
			 return fail(output->state().value("error").toString());
		 return output->state();
	 }},
	{"stop_output",
	 [](const QJsonObject &) {
		 output->stopNow();
		 return output->state();
	 }},
	// ---- recording and replay buffer ----
	{"record_state", [](const QJsonObject &) { return canvas->recorder->state(); }},
	// Any of {followMain, format: "mp4" | "mkv", replaySeconds, bitrate}.
	{"set_record",
	 [](const QJsonObject &in) {
		 canvas->recorder->configure(in);
		 return canvas->recorder->state();
	 }},
	{"start_record",
	 [](const QJsonObject &) {
		 QString error;
		 return canvas->recorder->startRecording(error) ? canvas->recorder->state() : fail(error);
	 }},
	{"stop_record",
	 [](const QJsonObject &) {
		 canvas->recorder->stopRecording();
		 return canvas->recorder->state();
	 }},
	{"start_replay",
	 [](const QJsonObject &) {
		 QString error;
		 return canvas->recorder->startReplay(error) ? canvas->recorder->state() : fail(error);
	 }},
	{"stop_replay",
	 [](const QJsonObject &) {
		 canvas->recorder->stopReplay();
		 return canvas->recorder->state();
	 }},
	{"save_replay",
	 [](const QJsonObject &) {
		 QString error;
		 return canvas->recorder->saveReplay(error) ? QJsonObject() : fail(error);
	 }},
	// ---- moving in from Aitum Vertical / the Vertical tool; Dual Format ----
	{"import_state", [](const QJsonObject &) { return canvas->importState(); }},
	{"import_aitum",
	 [](const QJsonObject &) {
		 QString error;
		 const int n = canvas->importAitum(error);
		 if (n < 0)
			 return fail(error);
		 QJsonObject o;
		 o["imported"] = n;
		 return o;
	 }},
	// {framing?: "fill" | "fit", position?: 0..100}: default, the Vertical tool's own.
	{"import_lua",
	 [](const QJsonObject &in) {
		 const QJsonObject st = canvas->importState();
		 QString error;
		 const int n = canvas->importLuaTool(in.value("framing").toString(st.value("luaFraming").toString("fill")),
						     in.contains("position") ? in.value("position").toInt()
									     : st.value("luaPosition").toInt(50),
						     error);
		 if (n < 0)
			 return fail(error);
		 QJsonObject o;
		 o["imported"] = n;
		 if (!error.isEmpty())
			 o["note"] = error;
		 return o;
	 }},
	{"dual_format", [](const QJsonObject &) { return canvas->dualFormatState(); }},
	{"set_dual_format",
	 [](const QJsonObject &in) {
		 QString error;
		 return canvas->setDualFormat(in.value("on").toBool(), error) ? canvas->dualFormatState() : fail(error);
	 }},
};

void onRequest(obs_data_t *request, obs_data_t *response, void *priv)
{
	const Request *r = static_cast<const Request *>(priv);
	if (!alive.load()) {
		reply(response, fail("OBS is closing."));
		return;
	}
	const QJsonObject in = toJson(request);
	QJsonObject out;
	onUi([&]() { out = alive.load() ? r->handler(in) : fail("OBS is closing."); });
	reply(response, out);
}

} // namespace

namespace vertical_ws {

void registerVendor(VerticalCanvas *c, RelayOutput *o)
{
	canvas = c;
	output = o;
	obs_websocket_vendor vendor = obs_websocket_register_vendor("multistream-vertical");
	if (!vendor) {
		obs_log(LOG_INFO, "vertical: obs-websocket isn't loaded; no vendor requests");
		return;
	}
	for (const Request &r : REQUESTS)
		obs_websocket_vendor_register_request(vendor, r.name, onRequest, const_cast<Request *>(&r));
	// VendorEvent "scene_changed" {scene, previous}: for Stream Deck and the
	// Multistream app's "OBS: Scene changed" trigger.
	canvas->onSceneChanged = [vendor](const QString &scene, const QString &previous) {
		if (!alive.load())
			return;
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "scene", scene.toUtf8().constData());
		obs_data_set_string(d, "previous", previous.toUtf8().constData());
		obs_websocket_vendor_emit_event(vendor, "scene_changed", d);
		obs_data_release(d);
	};
	alive.store(true);
}

void shutdown()
{
	alive.store(false);
	if (canvas)
		canvas->onSceneChanged = nullptr;
}

} // namespace vertical_ws
