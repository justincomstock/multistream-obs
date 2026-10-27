// The items in a vertical scene: listing, show/hide, lock, order, new
// sources, transforms, and a 9:16 layout made from a main scene.
#include "canvas.hpp"
#include "../i18n.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

// An item by id, at the top or inside one of the scene's groups.
obs_sceneitem_t *itemIn(obs_scene_t *scene, qint64 id)
{
	if (!scene)
		return nullptr;
	if (obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, id))
		return item;
	struct Find {
		qint64 id;
		obs_sceneitem_t *found;
	} f{id, nullptr};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
			auto *f = static_cast<Find *>(p);
			if (obs_sceneitem_is_group(item))
				f->found = obs_scene_find_sceneitem_by_id(obs_sceneitem_group_get_scene(item), f->id);
			return f->found == nullptr;
		},
		&f);
	return f.found;
}

// Whether the item sits inside a group (its position is in the group's pixels).
bool inGroup(obs_scene_t *scene, obs_sceneitem_t *item)
{
	return item && obs_scene_find_sceneitem_by_id(scene, obs_sceneitem_get_id(item)) != item;
}

bool isCamera(const char *id)
{
	static const char *const CAMERAS[] = {"dshow_input", "av_capture_input", "av_capture_input_v2", "v4l2_input",
					      "macos-avcapture", "macos-avcapture-fast"};
	return id && std::any_of(std::begin(CAMERAS), std::end(CAMERAS), [&](const char *c) { return strcmp(c, id) == 0; });
}

// An item's box on its canvas (rotation ignored): top-left and size.
struct Box {
	float x = 0, y = 0, w = 0, h = 0;
};

Box boxOf(obs_sceneitem_t *item, const struct matrix4 *parent = nullptr)
{
	struct matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);
	struct vec3 a, b;
	vec3_set(&a, 0.0f, 0.0f, 0.0f);
	vec3_set(&b, 1.0f, 1.0f, 0.0f);
	vec3_transform(&a, &a, &m);
	vec3_transform(&b, &b, &m);
	if (parent) { // an item in a group: its box is in the group's own pixels
		vec3_transform(&a, &a, parent);
		vec3_transform(&b, &b, parent);
	}
	Box box;
	box.x = std::min(a.x, b.x);
	box.y = std::min(a.y, b.y);
	box.w = std::fabs(b.x - a.x);
	box.h = std::fabs(b.y - a.y);
	return box;
}

// Puts an item in a rectangle of the vertical canvas: `fill` covers it
// (cropped to it), otherwise it fits inside, centred.
void placeIn(obs_sceneitem_t *item, float x, float y, float w, float h, bool fill)
{
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	t.rot = 0.0f;
	vec2_set(&t.pos, x, y);
	vec2_set(&t.scale, 1.0f, 1.0f);
	t.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	t.bounds_type = fill ? OBS_BOUNDS_SCALE_OUTER : OBS_BOUNDS_SCALE_INNER;
	t.bounds_alignment = OBS_ALIGN_CENTER;
	vec2_set(&t.bounds, std::max(w, 1.0f), std::max(h, 1.0f));
	t.crop_to_bounds = fill;
	obs_sceneitem_set_info2(item, &t);
	struct obs_sceneitem_crop none = {0, 0, 0, 0};
	obs_sceneitem_set_crop(item, &none);
}

} // namespace

namespace {

// sceneItems' rows, bottom first; a group's sources come right before it, so
// they follow it once the list is turned top first.
struct ItemCollect {
	std::vector<QJsonObject> items;
	qint64 parent = 0;
};

bool collectItem(obs_scene_t *, obs_sceneitem_t *item, void *p)
{
	auto *c = static_cast<ItemCollect *>(p);
	obs_source_t *src = obs_sceneitem_get_source(item);
	QJsonObject o;
	o["id"] = double(obs_sceneitem_get_id(item));
	o["name"] = QString::fromUtf8(obs_source_get_name(src));
	o["type"] = QString::fromUtf8(obs_source_get_id(src));
	o["visible"] = obs_sceneitem_visible(item);
	o["locked"] = obs_sceneitem_locked(item);
	o["audio"] = (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) != 0;
	o["video"] = (obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO) != 0;
	o["group"] = obs_sceneitem_is_group(item);
	o["parent"] = double(c->parent);
	if (obs_sceneitem_is_group(item)) {
		ItemCollect inner;
		inner.parent = obs_sceneitem_get_id(item);
		obs_sceneitem_group_enum_items(item, collectItem, &inner);
		c->items.insert(c->items.end(), inner.items.begin(), inner.items.end());
	}
	c->items.push_back(o);
	return true;
}

} // namespace

obs_sceneitem_t *VerticalCanvas::itemById(obs_scene_t *scene, qint64 id)
{
	return itemIn(scene, id);
}

bool VerticalCanvas::isInGroup(obs_scene_t *scene, obs_sceneitem_t *item)
{
	return inGroup(scene, item);
}

QJsonArray VerticalCanvas::sceneItems(const QString &scene) const
{
	QJsonArray out;
	obs_scene_t *s = findScene(scene);
	if (!s)
		return out;
	ItemCollect col;
	obs_scene_enum_items(s, collectItem, &col);
	obs_scene_release(s);
	// OBS lists bottom first; the dock shows the top first, like OBS's.
	for (auto it = col.items.rbegin(); it != col.items.rend(); ++it)
		out.append(*it);
	return out;
}

#define WITH_ITEM(scene, id, error)                                                    \
	obs_scene_t *s_ = findScene(scene);                                            \
	obs_sceneitem_t *item = itemIn(s_, id);                                        \
	if (!item) {                                                                   \
		obs_scene_release(s_);                                                 \
		error = mt("Vertical.SourceNotInScene").arg(scene);                       \
		return false;                                                          \
	}

namespace {

// Undo of show/hide and lock: {scene, id, what: "visible" | "locked", on}.
void setFlag(const char *data)
{
	obs_data_t *d = obs_data_create_from_json(data);
	obs_source_t *sceneSrc = obs_get_source_by_uuid(obs_data_get_string(d, "scene"));
	if (obs_sceneitem_t *item = itemIn(obs_scene_from_source(sceneSrc), qint64(obs_data_get_int(d, "id")))) {
		if (strcmp(obs_data_get_string(d, "what"), "visible") == 0)
			obs_sceneitem_set_visible(item, obs_data_get_bool(d, "on"));
		else
			obs_sceneitem_set_locked(item, obs_data_get_bool(d, "on"));
	}
	obs_source_release(sceneSrc);
	obs_data_release(d);
}

std::string flagData(obs_scene_t *scene, qint64 id, const char *what, bool on)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "scene", obs_source_get_uuid(obs_scene_get_source(scene)));
	obs_data_set_int(d, "id", id);
	obs_data_set_string(d, "what", what);
	obs_data_set_bool(d, "on", on);
	std::string json = obs_data_get_json(d);
	obs_data_release(d);
	return json;
}

void addFlagUndo(obs_scene_t *scene, obs_sceneitem_t *item, const char *what, bool on)
{
	const qint64 id = obs_sceneitem_get_id(item);
	const std::string undo = flagData(scene, id, what, !on), redo = flagData(scene, id, what, on);
	const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
	const std::string label = strcmp(what, "visible") == 0 ? std::string(on ? "Show " : "Hide ") + name + " (vertical)"
							      : std::string(on ? "Lock " : "Unlock ") + name + " (vertical)";
	obs_frontend_add_undo_redo_action(label.c_str(), setFlag, setFlag, undo.c_str(), redo.c_str(), false);
}

// The scene's top-level items, bottom first: {scene, ids: [{id}]}.
std::string orderData(obs_scene_t *scene)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "scene", obs_source_get_uuid(obs_scene_get_source(scene)));
	obs_data_array_t *ids = obs_data_array_create();
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
			obs_data_t *o = obs_data_create();
			obs_data_set_int(o, "id", obs_sceneitem_get_id(item));
			obs_data_array_push_back(static_cast<obs_data_array_t *>(p), o);
			obs_data_release(o);
			return true;
		},
		ids);
	obs_data_set_array(d, "ids", ids);
	obs_data_array_release(ids);
	std::string json = obs_data_get_json(d);
	obs_data_release(d);
	return json;
}

void loadOrder(const char *data)
{
	obs_data_t *d = obs_data_create_from_json(data);
	obs_source_t *sceneSrc = obs_get_source_by_uuid(obs_data_get_string(d, "scene"));
	if (obs_scene_t *scene = obs_scene_from_source(sceneSrc)) {
		obs_data_array_t *ids = obs_data_get_array(d, "ids");
		int at = 0;
		for (size_t i = 0; i < obs_data_array_count(ids); i++) {
			obs_data_t *o = obs_data_array_item(ids, i);
			if (obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, obs_data_get_int(o, "id")))
				obs_sceneitem_set_order_position(item, at++);
			obs_data_release(o);
		}
		obs_data_array_release(ids);
	}
	obs_source_release(sceneSrc);
	obs_data_release(d);
}

// Undo of a rename: {uuid, name}.
void loadName(const char *data)
{
	obs_data_t *d = obs_data_create_from_json(data);
	if (obs_source_t *src = obs_get_source_by_uuid(obs_data_get_string(d, "uuid"))) {
		obs_source_set_name(src, obs_data_get_string(d, "name"));
		obs_source_release(src);
	}
	obs_data_release(d);
}

std::string nameData(obs_source_t *src, const QString &name)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "uuid", obs_source_get_uuid(src));
	obs_data_set_string(d, "name", name.toUtf8().constData());
	std::string json = obs_data_get_json(d);
	obs_data_release(d);
	return json;
}

// A source name nobody has yet: "Base", "Base 2", ...
QString freeSourceName(const QString &base)
{
	for (int i = 1;; i++) {
		const QString n = i == 1 ? base : QString("%1 %2").arg(base).arg(i);
		obs_source_t *have = obs_get_source_by_name(n.toUtf8().constData());
		if (!have)
			return n;
		obs_source_release(have);
	}
}

} // namespace

bool VerticalCanvas::setItemVisible(const QString &scene, qint64 id, bool on, QString &error)
{
	WITH_ITEM(scene, id, error)
	if (obs_sceneitem_visible(item) != on) {
		obs_sceneitem_set_visible(item, on);
		addFlagUndo(s_, item, "visible", on);
	}
	obs_scene_release(s_);
	refreshAudio();
	return true;
}

bool VerticalCanvas::setItemLocked(const QString &scene, qint64 id, bool on, QString &error)
{
	WITH_ITEM(scene, id, error)
	if (obs_sceneitem_locked(item) != on) {
		obs_sceneitem_set_locked(item, on);
		addFlagUndo(s_, item, "locked", on);
	}
	obs_scene_release(s_);
	return true;
}

bool VerticalCanvas::moveItem(const QString &scene, qint64 id, int movement, QString &error)
{
	WITH_ITEM(scene, id, error)
	const bool top = !inGroup(s_, item); // a group's own order isn't in the undo
	const std::string before = top ? orderData(s_) : std::string();
	obs_sceneitem_set_order(item, static_cast<enum obs_order_movement>(movement));
	if (top) {
		const std::string after = orderData(s_);
		if (after != before)
			obs_frontend_add_undo_redo_action(mt("Vertical.UndoReorder").toUtf8().constData(), loadOrder, loadOrder, before.c_str(),
							  after.c_str(), false);
	}
	obs_scene_release(s_);
	return true;
}

bool VerticalCanvas::reorderItems(const QString &scene, const QList<qint64> &topFirst, QString &error)
{
	obs_scene_t *s = findScene(scene);
	if (!s) {
		error = mt("Vertical.NoScene").arg(scene);
		return false;
	}
	std::vector<obs_sceneitem_t *> order; // bottom first
	for (auto it = topFirst.crbegin(); it != topFirst.crend(); ++it)
		if (obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(s, *it))
			order.push_back(item);
	const std::string before = orderData(s);
	for (size_t i = 0; i < order.size(); i++)
		obs_sceneitem_set_order_position(order[i], int(i));
	const std::string after = orderData(s);
	if (after != before)
		obs_frontend_add_undo_redo_action(mt("Vertical.UndoReorder").toUtf8().constData(), loadOrder, loadOrder, before.c_str(),
						  after.c_str(), false);
	obs_scene_release(s);
	return true;
}

bool VerticalCanvas::renameSource(obs_source_t *src, const QString &to, QString &error)
{
	const QString name = to.trimmed();
	const QString old = QString::fromUtf8(obs_source_get_name(src));
	if (name.isEmpty()) {
		error = mt("Vertical.NeedSourceName");
		return false;
	}
	if (name == old)
		return true;
	if (obs_source_t *have = obs_get_source_by_name(name.toUtf8().constData())) {
		obs_source_release(have);
		error = mt("Vertical.SourceNameTaken").arg(name);
		return false;
	}
	const std::string undo = nameData(src, old), redo = nameData(src, name);
	obs_source_set_name(src, name.toUtf8().constData());
	const std::string label = mt("Vertical.UndoRename").arg(old).toStdString();
	obs_frontend_add_undo_redo_action(label.c_str(), loadName, loadName, undo.c_str(), redo.c_str(), false);
	changed();
	return true;
}

namespace {

// Undo of a removal: puts the source back in its scene, where and how it was.
// The source is recreated from its saved copy if nothing else held it.
void unremove(const char *data)
{
	obs_data_t *d = obs_data_create_from_json(data);
	obs_source_t *sceneSrc = obs_get_source_by_uuid(obs_data_get_string(d, "scene"));
	obs_scene_t *scene = obs_scene_from_source(sceneSrc);
	obs_source_t *src = obs_get_source_by_name(obs_data_get_string(d, "name"));
	if (!src) {
		obs_data_t *saved = obs_data_get_obj(d, "source");
		src = obs_load_source(saved);
		obs_data_release(saved);
	}
	if (scene && src) {
		obs_sceneitem_t *item = obs_scene_add(scene, src);
		if (item) {
			struct obs_transform_info t;
			obs_sceneitem_get_info2(item, &t);
			obs_data_get_vec2(d, "pos", &t.pos);
			obs_data_get_vec2(d, "scale", &t.scale);
			obs_data_get_vec2(d, "bounds", &t.bounds);
			t.rot = float(obs_data_get_double(d, "rot"));
			t.alignment = uint32_t(obs_data_get_int(d, "align"));
			t.bounds_type = obs_bounds_type(obs_data_get_int(d, "boundsType"));
			t.bounds_alignment = uint32_t(obs_data_get_int(d, "boundsAlign"));
			t.crop_to_bounds = obs_data_get_bool(d, "cropToBounds");
			obs_sceneitem_set_info2(item, &t);
			struct obs_sceneitem_crop c;
			c.left = int(obs_data_get_int(d, "cropLeft"));
			c.top = int(obs_data_get_int(d, "cropTop"));
			c.right = int(obs_data_get_int(d, "cropRight"));
			c.bottom = int(obs_data_get_int(d, "cropBottom"));
			obs_sceneitem_set_crop(item, &c);
			obs_sceneitem_set_visible(item, obs_data_get_bool(d, "visible"));
			obs_sceneitem_set_locked(item, obs_data_get_bool(d, "locked"));
			obs_sceneitem_set_order_position(item, int(obs_data_get_int(d, "index")));
		}
	}
	obs_source_release(src);
	obs_source_release(sceneSrc);
	obs_data_release(d);
}

void reremove(const char *data)
{
	obs_data_t *d = obs_data_create_from_json(data);
	obs_source_t *sceneSrc = obs_get_source_by_uuid(obs_data_get_string(d, "scene"));
	if (obs_scene_t *scene = obs_scene_from_source(sceneSrc)) {
		// The same item if it's still there (a source can be in a scene twice).
		obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, obs_data_get_int(d, "id"));
		obs_source_t *src = item ? obs_sceneitem_get_source(item) : nullptr;
		if (!src || strcmp(obs_source_get_name(src), obs_data_get_string(d, "name")) != 0)
			item = obs_scene_find_source(scene, obs_data_get_string(d, "name"));
		if (item)
			obs_sceneitem_remove(item);
	}
	obs_source_release(sceneSrc);
	obs_data_release(d);
}

} // namespace

namespace {

// Everything unremove() needs to put the item back as it is now.
std::string snapshotOf(obs_scene_t *scene, obs_sceneitem_t *item)
{
	obs_source_t *src = obs_sceneitem_get_source(item);
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	struct obs_sceneitem_crop c;
	obs_sceneitem_get_crop(item, &c);
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "scene", obs_source_get_uuid(obs_scene_get_source(scene)));
	obs_data_set_string(d, "name", obs_source_get_name(src));
	obs_data_set_int(d, "id", obs_sceneitem_get_id(item));
	obs_data_t *saved = obs_save_source(src);
	obs_data_set_obj(d, "source", saved);
	obs_data_release(saved);
	obs_data_set_vec2(d, "pos", &t.pos);
	obs_data_set_vec2(d, "scale", &t.scale);
	obs_data_set_vec2(d, "bounds", &t.bounds);
	obs_data_set_double(d, "rot", t.rot);
	obs_data_set_int(d, "align", t.alignment);
	obs_data_set_int(d, "boundsType", t.bounds_type);
	obs_data_set_int(d, "boundsAlign", t.bounds_alignment);
	obs_data_set_bool(d, "cropToBounds", t.crop_to_bounds);
	obs_data_set_int(d, "cropLeft", c.left);
	obs_data_set_int(d, "cropTop", c.top);
	obs_data_set_int(d, "cropRight", c.right);
	obs_data_set_int(d, "cropBottom", c.bottom);
	obs_data_set_bool(d, "visible", obs_sceneitem_visible(item));
	obs_data_set_bool(d, "locked", obs_sceneitem_locked(item));
	obs_data_set_int(d, "index", obs_sceneitem_get_order_position(item));
	std::string json = obs_data_get_json(d);
	obs_data_release(d);
	return json;
}

} // namespace

bool VerticalCanvas::removeItem(const QString &scene, qint64 id, QString &error)
{
	WITH_ITEM(scene, id, error)
	// Saved first, so OBS's Edit -> Undo can bring it back.
	const std::string json = snapshotOf(s_, item);
	const std::string what =
		mt("Vertical.UndoRemove").arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)))).toStdString();
	obs_sceneitem_remove(item);
	obs_scene_release(s_);
	obs_frontend_add_undo_redo_action(what.c_str(), unremove, reremove, json.c_str(), json.c_str(), false);
	refreshAudio();
	return true;
}

void VerticalCanvas::undoableAdd(obs_scene_t *scene, obs_sceneitem_t *item)
{
	const std::string json = snapshotOf(scene, item);
	const std::string what =
		mt("Vertical.UndoAdd").arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)))).toStdString();
	obs_frontend_add_undo_redo_action(what.c_str(), reremove, unremove, json.c_str(), json.c_str(), false);
}

int VerticalCanvas::addFiles(const QString &scene, const QStringList &paths, QString &error)
{
	obs_scene_t *s = findScene(scene);
	if (!s) {
		error = mt("Vertical.AddSceneFirst");
		return -1;
	}
	static const QStringList PICTURES = {"png", "jpg", "jpeg", "gif", "bmp", "tga", "webp", "psd", "jxr"};
	static const QStringList MEDIA = {"mp4", "mkv", "mov", "webm", "flv", "avi", "ts", "m4v", "mpg",
					  "mp3", "wav", "ogg", "flac", "aac", "m4a", "opus"};
	int added = 0;
	QStringList skipped;
	for (const QString &path : paths) {
		const QFileInfo f(path);
		const QString ext = f.suffix().toLower();
		const char *type = PICTURES.contains(ext) ? "image_source"
				   : MEDIA.contains(ext)  ? "ffmpeg_source"
				   : ext == "html" || ext == "htm" ? "browser_source"
								  : nullptr;
		if (!f.isFile() || !type) {
			skipped << f.fileName();
			continue;
		}
		const QByteArray file = QDir::toNativeSeparators(f.absoluteFilePath()).toUtf8();
		obs_data_t *settings = obs_data_create();
		if (strcmp(type, "image_source") == 0) {
			obs_data_set_string(settings, "file", file.constData());
		} else if (strcmp(type, "ffmpeg_source") == 0) {
			obs_data_set_bool(settings, "is_local_file", true);
			obs_data_set_string(settings, "local_file", file.constData());
		} else {
			obs_data_set_bool(settings, "is_local_file", true);
			obs_data_set_string(settings, "local_file", file.constData());
			obs_data_set_int(settings, "width", width_);
			obs_data_set_int(settings, "height", height_);
		}
		const QString name = freeSourceName(f.completeBaseName());
		obs_source_t *src = obs_source_create(type, name.toUtf8().constData(), settings, nullptr);
		obs_data_release(settings);
		obs_sceneitem_t *item = src ? obs_scene_add(s, src) : nullptr;
		if (item) {
			placeNew(s, item);
			undoableAdd(s, item);
			added++;
		} else {
			skipped << f.fileName();
		}
		obs_source_release(src);
	}
	obs_scene_release(s);
	if (!skipped.isEmpty())
		error = mt("Vertical.CantAddFiles").arg(skipped.join(", "));
	refreshAudio();
	return added;
}

bool VerticalCanvas::copySource(const QString &scene, qint64 id, QString &error)
{
	WITH_ITEM(scene, id, error)
	copiedSource_ = QString::fromUtf8(obs_source_get_uuid(obs_sceneitem_get_source(item)));
	obs_sceneitem_get_info2(item, &copiedSourceInfo_);
	obs_sceneitem_get_crop(item, &copiedSourceCrop_);
	obs_scene_release(s_);
	return true;
}

bool VerticalCanvas::pasteSource(const QString &scene, bool duplicate, QString &error)
{
	obs_source_t *src = copiedSource_.isEmpty() ? nullptr : obs_get_source_by_uuid(copiedSource_.toUtf8().constData());
	if (!src) {
		error = copiedSource_.isEmpty() ? mt("Vertical.CopyFirst") : mt("Vertical.CopiedGone");
		return false;
	}
	obs_scene_t *s = findScene(scene);
	if (!s) {
		obs_source_release(src);
		error = mt("Vertical.AddSceneFirst");
		return false;
	}
	if (duplicate && !obs_scene_from_source(src)) {
		obs_source_t *copy =
			obs_source_duplicate(src, freeSourceName(QString::fromUtf8(obs_source_get_name(src))).toUtf8().constData(), false);
		obs_source_release(src);
		src = copy;
	}
	obs_sceneitem_t *item = src ? obs_scene_add(s, src) : nullptr;
	if (item) {
		obs_sceneitem_set_info2(item, &copiedSourceInfo_);
		obs_sceneitem_set_crop(item, &copiedSourceCrop_);
		undoableAdd(s, item);
	}
	const bool isScene = src && obs_scene_from_source(src);
	obs_source_release(src);
	obs_scene_release(s);
	if (!item) {
		error = isScene ? mt("Vertical.PasteSceneLoop") : mt("Vertical.CantPaste");
		return false;
	}
	refreshAudio();
	return true;
}

bool VerticalCanvas::addNewSource(const QString &scene, const QString &typeId, const QString &name, QString &error,
				  obs_source_t **created)
{
	const QString n = name.trimmed();
	if (n.isEmpty()) {
		error = mt("Vertical.NeedSourceName");
		return false;
	}
	if (obs_source_t *have = obs_get_source_by_name(n.toUtf8().constData())) {
		obs_source_release(have);
		error = mt("Vertical.SourceExists").arg(n);
		return false;
	}
	obs_scene_t *s = findScene(scene);
	if (!s) {
		error = mt("Vertical.NoScene").arg(scene);
		return false;
	}
	// A color or a browser page takes its size from the main canvas (wide), or is
	// 800x600; here it starts phone-sized.
	obs_data_t *settings = nullptr;
	if (typeId.startsWith("color_source") || typeId == "browser_source") {
		settings = obs_data_create();
		obs_data_set_int(settings, "width", width_);
		obs_data_set_int(settings, "height", height_);
	}
	obs_source_t *src = obs_source_create(typeId.toUtf8().constData(), n.toUtf8().constData(), settings, nullptr);
	obs_data_release(settings);
	if (!src) {
		obs_scene_release(s);
		error = mt("Vertical.CantMakeSource");
		return false;
	}
	obs_sceneitem_t *item = obs_scene_add(s, src);
	// Its own size, centred (scaled once a capture has a size).
	if (item) {
		placeNew(s, item);
		undoableAdd(s, item);
	}
	obs_scene_release(s);
	if (created)
		*created = obs_source_get_ref(src);
	obs_source_release(src);
	refreshAudio();
	return true;
}

QJsonObject VerticalCanvas::itemTransform(const QString &scene, qint64 id) const
{
	QJsonObject o;
	obs_scene_t *s = findScene(scene);
	obs_sceneitem_t *item = itemIn(s, id);
	if (item) {
		struct obs_transform_info t;
		obs_sceneitem_get_info2(item, &t);
		struct obs_sceneitem_crop c;
		obs_sceneitem_get_crop(item, &c);
		o["x"] = t.pos.x;
		o["y"] = t.pos.y;
		o["scaleX"] = t.scale.x;
		o["scaleY"] = t.scale.y;
		o["rot"] = t.rot;
		o["align"] = int(t.alignment);
		o["boundsType"] = int(t.bounds_type);
		o["boundsW"] = t.bounds.x;
		o["boundsH"] = t.bounds.y;
		o["cropToBounds"] = t.crop_to_bounds;
		o["cropLeft"] = c.left;
		o["cropTop"] = c.top;
		o["cropRight"] = c.right;
		o["cropBottom"] = c.bottom;
		const Box b = boxOf(item);
		o["boxX"] = b.x;
		o["boxY"] = b.y;
		o["boxW"] = b.w;
		o["boxH"] = b.h;
	}
	obs_scene_release(s);
	return o;
}

bool VerticalCanvas::setItemTransform(const QString &scene, qint64 id, const QJsonObject &in, QString &error)
{
	WITH_ITEM(scene, id, error)
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	auto num = [&](const char *k, float v) { return in.contains(k) ? float(in.value(k).toDouble()) : v; };
	t.pos.x = num("x", t.pos.x);
	t.pos.y = num("y", t.pos.y);
	t.scale.x = num("scaleX", t.scale.x);
	t.scale.y = num("scaleY", t.scale.y);
	t.rot = num("rot", t.rot);
	if (in.contains("align"))
		t.alignment = uint32_t(in.value("align").toInt());
	if (in.contains("boundsType"))
		t.bounds_type = static_cast<enum obs_bounds_type>(in.value("boundsType").toInt());
	t.bounds.x = num("boundsW", t.bounds.x);
	t.bounds.y = num("boundsH", t.bounds.y);
	if (in.contains("cropToBounds"))
		t.crop_to_bounds = in.value("cropToBounds").toBool();
	obs_sceneitem_set_info2(item, &t);
	struct obs_sceneitem_crop c;
	obs_sceneitem_get_crop(item, &c);
	c.left = in.contains("cropLeft") ? in.value("cropLeft").toInt() : c.left;
	c.top = in.contains("cropTop") ? in.value("cropTop").toInt() : c.top;
	c.right = in.contains("cropRight") ? in.value("cropRight").toInt() : c.right;
	c.bottom = in.contains("cropBottom") ? in.value("cropBottom").toInt() : c.bottom;
	obs_sceneitem_set_crop(item, &c);
	obs_scene_release(s_);
	return true;
}

bool VerticalCanvas::createFromMain(const QString &main, const QString &name, QString &error)
{
	obs_source_t *mainSrc = obs_get_source_by_name(main.toUtf8().constData());
	obs_scene_t *mainScene = mainSrc ? obs_scene_from_source(mainSrc) : nullptr;
	if (!mainScene) {
		obs_source_release(mainSrc);
		error = mt("Vertical.NoMainScene").arg(main);
		return false;
	}
	if (!createScene(name, error)) {
		obs_source_release(mainSrc);
		return false;
	}
	obs_scene_t *v = findScene(name.trimmed());

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	const float MW = float(ovi.base_width ? ovi.base_width : 1920);
	const float MH = float(ovi.base_height ? ovi.base_height : 1080);
	const float W = float(width_), H = float(height_);

	enum Kind { Background, Camera, Overlay, FullOverlay, Sound, Skip };
	struct Entry {
		obs_sceneitem_t *item;
		Kind kind;
		Box box;
		bool visible;
	};
	// A group can't be in two scenes, so the sources inside it come over one by
	// one, in place of the group.
	struct Walk {
		std::vector<Entry> entries;
		const struct matrix4 *parent = nullptr;
		bool parentVisible = true;
	} walk;
	struct Enum {
		static bool add(obs_scene_t *, obs_sceneitem_t *item, void *p)
		{
			auto *w = static_cast<Walk *>(p);
			const bool visible = w->parentVisible && obs_sceneitem_visible(item);
			if (obs_sceneitem_is_group(item)) {
				if (w->parent)
					return true; // groups don't nest in OBS
				struct matrix4 m;
				obs_sceneitem_get_draw_transform(item, &m);
				w->parent = &m;
				w->parentVisible = visible;
				obs_sceneitem_group_enum_items(item, add, p);
				w->parent = nullptr;
				w->parentVisible = true;
				return true;
			}
			Entry e{item, Skip, Box(), visible};
			obs_source_t *src = obs_sceneitem_get_source(item);
			const uint32_t flags = obs_source_get_output_flags(src);
			if (!(flags & OBS_SOURCE_VIDEO)) {
				e.kind = Sound;
			} else {
				e.box = boxOf(item, w->parent);
				e.kind = Overlay;
			}
			w->entries.push_back(e);
			return true;
		}
	};
	obs_scene_enum_items(mainScene, Enum::add, &walk);
	std::vector<Entry> &entries = walk.entries;
	bool haveBackground = false, haveCamera = false;
	for (Entry &e : entries) {
		if (e.kind != Overlay)
			continue;
		const char *id = obs_source_get_id(obs_sceneitem_get_source(e.item));
		const float area = (e.box.w * e.box.h) / (MW * MH);
		if (isCamera(id))
			e.kind = Camera;
		else if (area >= 0.6f && id && strcmp(id, "browser_source") == 0)
			e.kind = FullOverlay; // full-screen alerts or chat: see-through, on top
		else if (area >= 0.6f)
			e.kind = Background;
		haveBackground |= e.kind == Background && e.visible;
		haveCamera |= e.kind == Camera && e.visible;
	}

	// The game/screen on top, the camera underneath (TikTok-style split)
	// when both show; either alone fills the frame.
	const float split = haveBackground && haveCamera ? 0.62f : 1.0f;
	int skipped = 0;
	for (const Entry &e : entries) {
		if (e.kind == Skip) {
			skipped++;
			continue;
		}
		obs_sceneitem_t *item = obs_scene_add(v, obs_sceneitem_get_source(e.item));
		if (!item)
			continue;
		obs_sceneitem_set_visible(item, e.visible);
		switch (e.kind) {
		case Background:
			placeIn(item, 0.0f, 0.0f, W, H * split, true);
			break;
		case Camera:
			if (split < 1.0f)
				placeIn(item, 0.0f, H * split, W, H * (1.0f - split), true);
			else
				placeIn(item, 0.0f, 0.0f, W, H, true);
			break;
		case Overlay: {
			// Twice as wide (relative to the frame) as on the main canvas,
			// at most 90 %; centred where it was, kept inside the frame.
			const float relW = e.box.w / MW;
			const float w = std::clamp(relW * 2.0f, 0.25f, 0.9f) * W;
			const float h = e.box.w > 0 ? w * (e.box.h / e.box.w) : w;
			const float cx = std::clamp((e.box.x + e.box.w / 2) / MW * W, w / 2, W - w / 2);
			const float cy = std::clamp((e.box.y + e.box.h / 2) / MH * H, h / 2, H - h / 2);
			placeIn(item, cx - w / 2, cy - h / 2, w, h, false);
			break;
		}
		case FullOverlay:
			placeIn(item, 0.0f, 0.0f, W, H, false);
			break;
		default:
			break;
		}
		// The crop from the main scene (a trimmed webcam stays trimmed).
		struct obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(e.item, &crop);
		obs_sceneitem_set_crop(item, &crop);
	}
	if (skipped)
		obs_log(LOG_INFO, "vertical: %d source(s) in \"%s\" left out", skipped, main.toUtf8().constData());
	obs_scene_release(v);
	obs_source_release(mainSrc);
	refreshAudio();
	changed();
	return true;
}
