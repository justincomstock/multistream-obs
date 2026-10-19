#include "canvas.hpp"
#include "record.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <graphics/vec2.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QMetaObject>

#include <algorithm>
#include <climits>
#include <cstring>
#include <set>
#include <vector>

namespace {

const char *CANVAS_NAME = "Multistream Vertical";
const char *FIRST_SCENE = FIRST_SCENE_NAME;
const char *CONFIG_FILE = "vertical.json";

struct SceneList {
	std::vector<obs_source_t *> scenes; // borrowed, valid during the call
};

bool collectScene(void *param, obs_source_t *src)
{
	static_cast<SceneList *>(param)->scenes.push_back(src);
	return true;
}

// Audio sources a scene plays: visible items, through groups and nested
// scenes (a few levels deep at most).
void collectAudio(obs_scene_t *scene, std::set<obs_source_t *> &out, int depth)
{
	if (!scene || depth > 8)
		return;
	struct Ctx {
		std::set<obs_source_t *> *out;
		int depth;
	} ctx{&out, depth};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
			auto *c = static_cast<Ctx *>(p);
			if (!obs_sceneitem_visible(item))
				return true;
			if (obs_sceneitem_is_group(item)) {
				collectAudio(obs_sceneitem_group_get_scene(item), *c->out, c->depth + 1);
				return true;
			}
			obs_source_t *src = obs_sceneitem_get_source(item);
			if (obs_scene_t *nested = obs_scene_from_source(src)) {
				collectAudio(nested, *c->out, c->depth + 1);
				return true;
			}
			if (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO)
				c->out->insert(src);
			return true;
		},
		&ctx);
}

} // namespace

// OBS loads a saved canvas with no picture size, then its scenes; an item
// saved in pixels is turned into canvas-relative units as it loads, so
// with no size yet it lands nowhere (the scene draws nothing). The size is
// given here, as OBS makes the canvas, before its scenes load.
void VerticalCanvas::onCanvasCreate(void *data, calldata_t *cd)
{
	auto *self = static_cast<VerticalCanvas *>(data);
	auto *c = static_cast<obs_canvas_t *>(calldata_ptr(cd, "canvas"));
	const char *name = c ? obs_canvas_get_name(c) : nullptr;
	// Aitum Vertical's canvas stays in the scene collection after Aitum is
	// removed, with no size: sized like ours, its scenes can be imported.
	const bool orphanAitum = name && strcmp(name, AITUM_CANVAS) == 0 && !obs_get_module(AITUM_MODULE);
	if (!name || (strcmp(name, CANVAS_NAME) != 0 && !orphanAitum) || obs_canvas_has_video(c))
		return;
	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
		return;
	ovi.base_width = ovi.output_width = uint32_t(self->width_);
	ovi.base_height = ovi.output_height = uint32_t(self->height_);
	if (!obs_canvas_reset_video(c, &ovi))
		obs_log(LOG_WARNING, "vertical: couldn't size the canvas while it loads (an output is running)");
}

void VerticalCanvas::shutdown()
{
	signal_handler_disconnect(obs_get_signal_handler(), "canvas_create", onCanvasCreate, this);
	detach();
	audio_.close();
}

void VerticalCanvas::load()
{
	signal_handler_connect(obs_get_signal_handler(), "canvas_create", onCanvasCreate, this);
	char *file = obs_module_config_path(CONFIG_FILE);
	config_ = obs_data_create_from_json_file_safe(file, "bak");
	bfree(file);
	if (!config_)
		config_ = obs_data_create();
	obs_data_set_default_int(config_, "width", 1080);
	obs_data_set_default_int(config_, "height", 1920);
	obs_data_set_default_bool(config_, "followMain", true);
	width_ = int(obs_data_get_int(config_, "width"));
	height_ = int(obs_data_get_int(config_, "height"));
	followMain_ = obs_data_get_bool(config_, "followMain");
	if (width_ < 64 || height_ < 64 || width_ > 8192 || height_ > 8192) {
		width_ = 1080;
		height_ = 1920;
	}
}

QString VerticalCanvas::collection() const
{
	char *c = obs_frontend_get_current_scene_collection();
	QString name = QString::fromUtf8(c ? c : "");
	bfree(c);
	return name;
}

void VerticalCanvas::save() const
{
	if (!config_)
		return;
	obs_data_set_int(config_, "width", width_);
	obs_data_set_int(config_, "height", height_);
	obs_data_set_bool(config_, "followMain", followMain_);
	if (canvas_) {
		obs_data_t *all = obs_data_get_obj(config_, "collections");
		if (!all) {
			all = obs_data_create();
			obs_data_set_obj(config_, "collections", all);
		}
		obs_data_t *mine = obs_data_create();
		obs_data_set_string(mine, "scene", scene_.toUtf8().constData());
		obs_data_array_t *order = obs_data_array_create();
		for (const QString &uuid : order_) {
			obs_data_t *o = obs_data_create();
			obs_data_set_string(o, "uuid", uuid.toUtf8().constData());
			obs_data_array_push_back(order, o);
			obs_data_release(o);
		}
		obs_data_set_array(mine, "order", order);
		obs_data_array_release(order);
		obs_data_array_t *links = obs_data_array_create();
		for (auto it = links_.cbegin(); it != links_.cend(); ++it) {
			obs_data_t *o = obs_data_create();
			obs_data_set_string(o, "main", it.key().toUtf8().constData());
			obs_data_set_string(o, "vertical", it.value().toUtf8().constData());
			obs_data_array_push_back(links, o);
			obs_data_release(o);
		}
		obs_data_set_array(mine, "links", links);
		obs_data_array_release(links);
		obs_data_t *audio = audio_.saveSettings();
		obs_data_set_obj(mine, "audio", audio);
		obs_data_release(audio);
		obs_data_set_obj(all, collection().toUtf8().constData(), mine);
		obs_data_release(mine);
		obs_data_release(all);
	}
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	char *file = obs_module_config_path(CONFIG_FILE);
	obs_data_save_json_safe(config_, file, "tmp", "bak");
	bfree(file);
}

void VerticalCanvas::resetVideo()
{
	if (!canvas_ || obs_canvas_has_video(canvas_))
		return;
	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
		return;
	ovi.base_width = ovi.output_width = uint32_t(width_);
	ovi.base_height = ovi.output_height = uint32_t(height_);
	// Fails while any output is running (OBS can't resize mid-stream):
	// tick() tries again.
	if (!obs_canvas_reset_video(canvas_, &ovi))
		obs_log(LOG_INFO, "vertical: canvas video waits until outputs stop");
}

void VerticalCanvas::attach()
{
	if (canvas_)
		detach();

	struct obs_frontend_canvas_list list = {};
	obs_frontend_get_canvases(&list);
	for (size_t i = 0; i < list.canvases.num; i++) {
		obs_canvas_t *c = list.canvases.array[i];
		const char *name = obs_canvas_get_name(c);
		if (!canvas_ && name && strcmp(name, CANVAS_NAME) == 0)
			canvas_ = obs_canvas_get_ref(c);
	}
	obs_frontend_canvas_list_free(&list);

	if (!canvas_) {
		struct obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		ovi.base_width = ovi.output_width = uint32_t(width_);
		ovi.base_height = ovi.output_height = uint32_t(height_);
		// No MIX_AUDIO: the vertical sound is ours (VerticalAudio).
		canvas_ = obs_frontend_add_canvas(CANVAS_NAME, &ovi, ACTIVATE | SCENE_REF);
		if (!canvas_) {
			obs_log(LOG_WARNING, "vertical: OBS couldn't add the vertical canvas");
			return;
		}
		obs_log(LOG_INFO, "vertical: added the vertical canvas (%dx%d)", width_, height_);
	}
	// A canvas loaded from the scene collection has no video yet.
	resetVideo();
	ensureTransition();

	SceneList scenes;
	obs_canvas_enum_scenes(canvas_, collectScene, &scenes);
	if (scenes.scenes.empty()) {
		obs_scene_t *s = obs_canvas_scene_create(canvas_, FIRST_SCENE);
		obs_scene_release(s);
	}

	QString wanted;
	obs_data_t *mine = nullptr;
	if (obs_data_t *all = obs_data_get_obj(config_, "collections")) {
		mine = obs_data_get_obj(all, collection().toUtf8().constData());
		obs_data_release(all);
	}
	order_.clear();
	links_.clear();
	if (mine) {
		wanted = QString::fromUtf8(obs_data_get_string(mine, "scene"));
		obs_data_array_t *order = obs_data_get_array(mine, "order");
		for (size_t i = 0, n = obs_data_array_count(order); i < n; i++) {
			obs_data_t *o = obs_data_array_item(order, i);
			order_ << QString::fromUtf8(obs_data_get_string(o, "uuid"));
			obs_data_release(o);
		}
		obs_data_array_release(order);
		obs_data_array_t *links = obs_data_get_array(mine, "links");
		for (size_t i = 0, n = obs_data_array_count(links); i < n; i++) {
			obs_data_t *o = obs_data_array_item(links, i);
			links_[QString::fromUtf8(obs_data_get_string(o, "main"))] =
				QString::fromUtf8(obs_data_get_string(o, "vertical"));
			obs_data_release(o);
		}
		obs_data_array_release(links);
	}
	obs_data_t *audioSettings = mine ? obs_data_get_obj(mine, "audio") : nullptr;
	audio_.loadSettings(audioSettings);
	obs_data_release(audioSettings);
	obs_data_release(mine);

	obs_scene_t *scene = wanted.isEmpty() ? nullptr : findScene(wanted);
	if (!scene) {
		const QStringList names = sceneNames();
		if (!names.isEmpty())
			scene = findScene(names.front());
	}
	if (scene) {
		showScene(obs_scene_get_source(scene));
		obs_scene_release(scene);
	}
	if (followMain_)
		followMainScene();

	audio_.open();
	refreshAudio();
	registerSceneHotkeys();
	changed();
}

void VerticalCanvas::detach()
{
	audio_.setSources({});
	if (!canvas_)
		return;
	save();
	if (!obs_canvas_removed(canvas_))
		obs_canvas_set_channel(canvas_, 0, nullptr);
	releaseTransition();
	// The scenes go with the collection, and their hotkeys with them.
	sceneHotkeys_.clear();
	obs_canvas_release(canvas_);
	canvas_ = nullptr;
	scene_.clear();
	order_.clear();
	links_.clear();
	changed();
}

void VerticalCanvas::tick()
{
	if (canvas_ && obs_canvas_removed(canvas_))
		detach();
	if (!canvas_)
		return;
	resetVideo();
	refreshAudio();
	registerSceneHotkeys(); // scenes made since
}

void VerticalCanvas::onFrontendEvent(int event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		attach();
		break;
	// CHANGING comes before OBS clears the old collection's canvases;
	// CLEANUP after (a fallback).
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		detach();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		if (followMain_)
			followMainScene();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED:
		ensureTransition();
		break;
	// The main scene list changed (renamed, removed): the dock's link menu
	// redraws.
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		changed();
		break;
	default:
		break;
	}
}

void VerticalCanvas::followMainScene()
{
	if (!canvas_)
		return;
	// The main scene on air (the program scene in Studio Mode).
	obs_source_t *main = obs_frontend_get_current_scene();
	if (!main)
		return;
	const QString name = linkFor(QString::fromUtf8(obs_source_get_name(main)));
	obs_source_release(main);
	if (name.isEmpty())
		return;
	if (obs_scene_t *v = findScene(name)) {
		showScene(obs_scene_get_source(v));
		obs_scene_release(v);
	}
}

obs_source_t *VerticalCanvas::findSceneByUuid(const QString &uuid) const
{
	if (!canvas_ || uuid.isEmpty())
		return nullptr;
	SceneList scenes;
	obs_canvas_enum_scenes(canvas_, collectScene, &scenes);
	const QByteArray u = uuid.toUtf8();
	for (obs_source_t *src : scenes.scenes) {
		const char *su = obs_source_get_uuid(src);
		if (su && u == su)
			return obs_source_get_ref(src);
	}
	return nullptr;
}

QString VerticalCanvas::linkFor(const QString &mainName) const
{
	obs_source_t *main = obs_get_source_by_name(mainName.toUtf8().constData());
	if (!main)
		return QString();
	const QString mainUuid = QString::fromUtf8(obs_source_get_uuid(main));
	obs_source_release(main);
	if (links_.contains(mainUuid)) {
		obs_source_t *v = findSceneByUuid(links_.value(mainUuid));
		if (v) {
			const QString name = QString::fromUtf8(obs_source_get_name(v));
			obs_source_release(v);
			return name;
		}
	}
	// No link: the vertical scene with the same name, if any.
	return sceneNames().contains(mainName) ? mainName : QString();
}

QStringList VerticalCanvas::linkedMains(const QString &scene) const
{
	QStringList out;
	obs_scene_t *s = findScene(scene);
	if (!s)
		return out;
	const QString uuid = QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(s)));
	obs_scene_release(s);
	for (auto it = links_.cbegin(); it != links_.cend(); ++it) {
		if (it.value() != uuid)
			continue;
		if (obs_source_t *main = obs_get_source_by_uuid(it.key().toUtf8().constData())) {
			out << QString::fromUtf8(obs_source_get_name(main));
			obs_source_release(main);
		}
	}
	return out;
}

bool VerticalCanvas::linkScene(const QString &main, const QString &scene, QString &error)
{
	obs_source_t *m = obs_get_source_by_name(main.toUtf8().constData());
	if (!m || !obs_source_is_scene(m)) {
		obs_source_release(m);
		error = "There's no main scene called \"" + main + "\".";
		return false;
	}
	const QString mainUuid = QString::fromUtf8(obs_source_get_uuid(m));
	obs_source_release(m);
	if (scene.isEmpty()) {
		links_.remove(mainUuid);
	} else {
		obs_scene_t *s = findScene(scene);
		if (!s) {
			error = "There's no vertical scene called \"" + scene + "\".";
			return false;
		}
		links_[mainUuid] = QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(s)));
		obs_scene_release(s);
	}
	save();
	changed();
	if (followMain_)
		followMainScene();
	return true;
}

void VerticalCanvas::setFollowMain(bool on)
{
	followMain_ = on;
	save();
	changed();
	if (on)
		followMainScene();
}

obs_scene_t *VerticalCanvas::findScene(const QString &name) const
{
	if (!canvas_)
		return nullptr;
	SceneList scenes;
	obs_canvas_enum_scenes(canvas_, collectScene, &scenes);
	const QByteArray n = name.toUtf8();
	for (obs_source_t *src : scenes.scenes) {
		const char *sn = obs_source_get_name(src);
		if (sn && n == sn)
			return obs_scene_get_ref(obs_scene_from_source(src));
	}
	return nullptr;
}

// The transition OBS uses now, copied: the vertical canvas switches scenes
// the same way (Fade, Stinger, Cut...). Rebuilt when OBS's changes type.
void VerticalCanvas::ensureTransition()
{
	if (!canvas_)
		return;
	obs_source_t *main = obs_frontend_get_current_transition();
	const QString type = main ? QString::fromUtf8(obs_source_get_id(main)) : QString("cut_transition");
	if (transition_ && type == transitionType_) {
		if (main) {
			// Same type, maybe new settings (a different stinger file).
			obs_data_t *s = obs_source_get_settings(main);
			obs_source_update(transition_, s);
			obs_data_release(s);
		}
		obs_source_release(main);
		return;
	}
	obs_data_t *settings = main ? obs_source_get_settings(main) : nullptr;
	obs_source_t *t = obs_source_create_private(type.toUtf8().constData(), "Multistream vertical transition", settings);
	obs_data_release(settings);
	obs_source_release(main);
	if (!t)
		t = obs_source_create_private("cut_transition", "Multistream vertical transition", nullptr);
	if (!t)
		return;
	obs_transition_set_size(t, uint32_t(width_), uint32_t(height_));
	obs_transition_set_alignment(t, OBS_ALIGN_CENTER);
	obs_transition_set_scale_type(t, OBS_TRANSITION_SCALE_ASPECT);
	// Picks up what's on air now, with no animation.
	obs_source_t *showing = nullptr;
	if (transition_)
		showing = obs_transition_get_active_source(transition_);
	else if (obs_scene_t *s = findScene(scene_)) {
		showing = obs_source_get_ref(obs_scene_get_source(s));
		obs_scene_release(s);
	}
	obs_transition_set(t, showing);
	obs_source_release(showing);
	obs_canvas_set_channel(canvas_, 0, t);
	releaseTransition();
	transition_ = t;
	transitionType_ = type;
}

void VerticalCanvas::releaseTransition()
{
	if (!transition_)
		return;
	obs_transition_clear(transition_);
	obs_source_release(transition_);
	transition_ = nullptr;
	transitionType_.clear();
}

void VerticalCanvas::onSceneHotkey(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *self = static_cast<VerticalCanvas *>(data);
	QCoreApplication *app = QCoreApplication::instance();
	if (!app)
		return;
	// Hotkeys arrive on OBS's hotkey thread.
	QMetaObject::invokeMethod(app, [self, id]() {
		const QString uuid = self->sceneHotkeys_.value(id);
		obs_source_t *src = uuid.isEmpty() ? nullptr : self->findSceneByUuid(uuid);
		if (!src)
			return;
		QString error;
		self->setScene(QString::fromUtf8(obs_source_get_name(src)), error);
		obs_source_release(src);
	}, Qt::QueuedConnection);
}

// Each vertical scene gets a "Switch to vertical scene" hotkey (OBS
// Settings -> Hotkeys, under the scene's name). It's registered on the scene
// itself, so OBS saves its keys with the scene collection.
void VerticalCanvas::registerSceneHotkeys()
{
	if (!canvas_)
		return;
	QStringList have = sceneHotkeys_.values();
	SceneList scenes;
	obs_canvas_enum_scenes(canvas_, collectScene, &scenes);
	for (obs_source_t *src : scenes.scenes) {
		const QString uuid = QString::fromUtf8(obs_source_get_uuid(src));
		if (have.contains(uuid))
			continue;
		const obs_hotkey_id id = obs_hotkey_register_source(src, "MultistreamVertical.SwitchTo",
								    "Switch to vertical scene", onSceneHotkey, this);
		if (id != OBS_INVALID_HOTKEY_ID)
			sceneHotkeys_[id] = uuid;
	}
}

void VerticalCanvas::showScene(obs_source_t *scene)
{
	if (!canvas_ || !scene)
		return;
	if (!transition_)
		ensureTransition();
	if (transition_) {
		obs_source_t *now = obs_transition_get_active_source(transition_);
		const uint32_t ms = uint32_t(std::max(0, obs_frontend_get_transition_duration()));
		// Mid-transition, or the first picture: straight there.
		if (!now || now == scene || !obs_transition_start(transition_, OBS_TRANSITION_MODE_AUTO, ms, scene))
			obs_transition_set(transition_, scene);
		obs_source_release(now);
	} else {
		obs_canvas_set_channel(canvas_, 0, scene);
	}
	const QString name = QString::fromUtf8(obs_source_get_name(scene));
	if (name != scene_) {
		const QString previous = scene_;
		scene_ = name;
		changed();
		if (onSceneChanged)
			onSceneChanged(name, previous);
	}
	refreshAudio();
	save();
}

QStringList VerticalCanvas::sceneNames() const
{
	QStringList out;
	if (!canvas_)
		return out;
	SceneList scenes;
	obs_canvas_enum_scenes(canvas_, collectScene, &scenes);
	// The saved order first, then scenes it doesn't know (new ones).
	std::vector<std::pair<int, QString>> sorted;
	for (obs_source_t *src : scenes.scenes) {
		const int at = order_.indexOf(QString::fromUtf8(obs_source_get_uuid(src)));
		sorted.push_back({at < 0 ? INT_MAX : at, QString::fromUtf8(obs_source_get_name(src))});
	}
	std::stable_sort(sorted.begin(), sorted.end(),
			 [](const auto &a, const auto &b) { return a.first < b.first; });
	for (const auto &s : sorted)
		out << s.second;
	return out;
}

QJsonArray VerticalCanvas::sceneList() const
{
	QJsonArray out;
	for (const QString &name : sceneNames()) {
		QJsonObject s;
		s["name"] = name;
		s["current"] = name == scene_;
		QJsonArray mains;
		for (const QString &m : linkedMains(name)) {
			QJsonObject o;
			o["name"] = m;
			mains.append(o);
		}
		s["mains"] = mains;
		out.append(s);
	}
	return out;
}

bool VerticalCanvas::setScene(const QString &name, QString &error)
{
	obs_scene_t *s = findScene(name);
	if (!s) {
		error = "There's no vertical scene called \"" + name + "\".";
		return false;
	}
	showScene(obs_scene_get_source(s));
	obs_scene_release(s);
	return true;
}

namespace {
bool checkName(const QString &n, QString &error)
{
	if (n.isEmpty()) {
		error = "Give the scene a name.";
		return false;
	}
	return true;
}
} // namespace

bool VerticalCanvas::createScene(const QString &name, QString &error)
{
	if (!canvas_) {
		error = "The vertical canvas isn't ready.";
		return false;
	}
	const QString n = name.trimmed();
	if (!checkName(n, error))
		return false;
	if (obs_scene_t *have = findScene(n)) {
		obs_scene_release(have);
		error = "There's already a vertical scene called \"" + n + "\".";
		return false;
	}
	obs_scene_t *s = obs_canvas_scene_create(canvas_, n.toUtf8().constData());
	if (!s) {
		error = "OBS couldn't make the scene.";
		return false;
	}
	order_ = [&]() {
		QStringList o;
		for (const QString &existing : sceneNames()) {
			if (obs_scene_t *e = findScene(existing)) {
				o << QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(e)));
				obs_scene_release(e);
			}
		}
		return o;
	}();
	obs_scene_release(s);
	save();
	changed();
	return true;
}

bool VerticalCanvas::renameScene(const QString &name, const QString &to, QString &error)
{
	const QString n = to.trimmed();
	if (!checkName(n, error))
		return false;
	obs_scene_t *s = findScene(name);
	if (!s) {
		error = "There's no vertical scene called \"" + name + "\".";
		return false;
	}
	if (n != name) {
		if (obs_scene_t *have = findScene(n)) {
			obs_scene_release(have);
			obs_scene_release(s);
			error = "There's already a vertical scene called \"" + n + "\".";
			return false;
		}
		obs_source_set_name(obs_scene_get_source(s), n.toUtf8().constData());
		if (scene_ == name)
			scene_ = n;
	}
	obs_scene_release(s);
	save();
	changed();
	return true;
}

bool VerticalCanvas::duplicateScene(const QString &name, const QString &to, QString &error)
{
	const QString n = to.trimmed();
	if (!checkName(n, error))
		return false;
	obs_scene_t *s = findScene(name);
	if (!s) {
		error = "There's no vertical scene called \"" + name + "\".";
		return false;
	}
	if (obs_scene_t *have = findScene(n)) {
		obs_scene_release(have);
		obs_scene_release(s);
		error = "There's already a vertical scene called \"" + n + "\".";
		return false;
	}
	// Stays on the original's canvas; the items share the sources.
	obs_scene_t *copy = obs_scene_duplicate(s, n.toUtf8().constData(), OBS_SCENE_DUP_REFS);
	obs_scene_release(s);
	if (!copy) {
		error = "OBS couldn't copy the scene.";
		return false;
	}
	// Right after the original.
	const QString uuid = QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(copy)));
	obs_scene_release(copy);
	QStringList o;
	for (const QString &existing : sceneNames()) {
		if (existing == n)
			continue;
		if (obs_scene_t *e = findScene(existing)) {
			o << QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(e)));
			obs_scene_release(e);
		}
		if (existing == name)
			o << uuid;
	}
	order_ = o;
	save();
	changed();
	return true;
}

bool VerticalCanvas::removeScene(const QString &name, QString &error)
{
	const QStringList names = sceneNames();
	if (names.size() <= 1) {
		error = "The vertical canvas needs at least one scene.";
		return false;
	}
	obs_scene_t *s = findScene(name);
	if (!s) {
		error = "There's no vertical scene called \"" + name + "\".";
		return false;
	}
	obs_source_t *src = obs_scene_get_source(s);
	const QString uuid = QString::fromUtf8(obs_source_get_uuid(src));
	// Off air first: the next scene in the list takes its place.
	if (scene_ == name) {
		const int at = names.indexOf(name);
		const QString next = names.value(at + 1 < names.size() ? at + 1 : at - 1);
		if (obs_scene_t *n = findScene(next)) {
			showScene(obs_scene_get_source(n));
			obs_scene_release(n);
		}
	}
	obs_source_remove(src);
	obs_scene_release(s);
	order_.removeAll(uuid);
	for (auto it = links_.begin(); it != links_.end();)
		it = it.value() == uuid ? links_.erase(it) : std::next(it);
	save();
	changed();
	return true;
}

bool VerticalCanvas::moveScene(const QString &name, int delta, QString &error)
{
	QStringList names = sceneNames();
	const int at = names.indexOf(name);
	if (at < 0) {
		error = "There's no vertical scene called \"" + name + "\".";
		return false;
	}
	const int to = qBound(0, at + delta, int(names.size()) - 1);
	names.move(at, to);
	QStringList o;
	for (const QString &n : names) {
		if (obs_scene_t *e = findScene(n)) {
			o << QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(e)));
			obs_scene_release(e);
		}
	}
	order_ = o;
	save();
	changed();
	return true;
}

bool VerticalCanvas::addSource(const QString &scene, const QString &source, QString &error)
{
	obs_scene_t *s = findScene(scene);
	if (!s) {
		error = "There's no vertical scene called \"" + scene + "\".";
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(source.toUtf8().constData());
	if (!src) {
		obs_scene_release(s);
		error = "There's no source called \"" + source + "\".";
		return false;
	}
	obs_sceneitem_t *item = obs_scene_add(s, src);
	if (item) {
		// Fill the 9:16 frame, centred (the Vertical tool's "Fill").
		struct vec2 bounds, pos;
		vec2_set(&bounds, float(width_), float(height_));
		vec2_set(&pos, float(width_) / 2.0f, float(height_) / 2.0f);
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_OUTER);
		obs_sceneitem_set_bounds(item, &bounds);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
		obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
		obs_sceneitem_set_pos(item, &pos);
	}
	obs_source_release(src);
	obs_scene_release(s);
	if (!item) {
		error = "OBS couldn't add the source (a scene can't contain itself).";
		return false;
	}
	refreshAudio();
	return true;
}

void VerticalCanvas::refreshAudio()
{
	std::set<obs_source_t *> found;
	if (canvas_) {
		if (obs_source_t *ch = obs_canvas_get_channel(canvas_, 0)) {
			if (obs_source_get_type(ch) == OBS_SOURCE_TYPE_TRANSITION) {
				// Both scenes while it switches, then the new one.
				for (enum obs_transition_target t : {OBS_TRANSITION_SOURCE_A, OBS_TRANSITION_SOURCE_B}) {
					obs_source_t *s = obs_transition_get_source(ch, t);
					if (s)
						collectAudio(obs_scene_from_source(s), found, 0);
					obs_source_release(s);
				}
			} else {
				collectAudio(obs_scene_from_source(ch), found, 0);
			}
			obs_source_release(ch);
		}
	}
	// OBS's global Desktop Audio and Mic/Aux sources (Settings -> Audio).
	std::vector<obs_source_t *> globals;
	for (uint32_t ch = 1; ch <= 6; ch++) {
		if (obs_source_t *g = obs_get_output_source(ch)) {
			if (obs_source_get_output_flags(g) & OBS_SOURCE_AUDIO)
				found.insert(g);
			globals.push_back(g);
		}
	}
	audio_.setSources(std::vector<obs_source_t *>(found.begin(), found.end()));
	for (obs_source_t *g : globals)
		obs_source_release(g);
}

QJsonObject VerticalCanvas::audioState() const
{
	QJsonObject o;
	o["sources"] = audio_.state();
	return o;
}

bool VerticalCanvas::setAudio(const QString &source, const QJsonObject &in, QString &error)
{
	QString uuid;
	for (const QJsonValue v : audio_.state()) {
		const QJsonObject s = v.toObject();
		if (s.value("uuid").toString() == source || s.value("name").toString() == source)
			uuid = s.value("uuid").toString();
	}
	if (uuid.isEmpty()) {
		error = "\"" + source + "\" isn't playing in the vertical scene.";
		return false;
	}
	VerticalAudio::Setting s = audio_.setting(uuid.toStdString());
	if (in.contains("mode"))
		s.own = in.value("mode").toString() == "own";
	if (in.contains("volume")) {
		s.volume = float(qBound(0.0, in.value("volume").toDouble(), 1.0));
		s.own = true;
	}
	if (in.contains("muted")) {
		s.muted = in.value("muted").toBool();
		s.own = true;
	}
	audio_.setSetting(uuid.toStdString(), s);
	save();
	return true;
}

QJsonObject VerticalCanvas::state() const
{
	QJsonObject o;
	o["ready"] = ready();
	o["width"] = width_;
	o["height"] = height_;
	o["scene"] = scene_;
	o["scenes"] = sceneList();
	o["followMain"] = followMain_;
	o["audio"] = audio_.state();
	if (recorder)
		o["record"] = recorder->state();
	o["dualFormat"] = dualFormatState();
	return o;
}
