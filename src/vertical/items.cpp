// The items in a vertical scene: listing, show/hide, lock, order, new
// sources, transforms, and a 9:16 layout made from a main scene.
#include "canvas.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

obs_sceneitem_t *itemIn(obs_scene_t *scene, qint64 id)
{
	return scene ? obs_scene_find_sceneitem_by_id(scene, id) : nullptr;
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

QJsonArray VerticalCanvas::sceneItems(const QString &scene) const
{
	QJsonArray out;
	obs_scene_t *s = findScene(scene);
	if (!s)
		return out;
	std::vector<QJsonObject> items;
	obs_scene_enum_items(
		s,
		[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
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
			static_cast<std::vector<QJsonObject> *>(p)->push_back(o);
			return true;
		},
		&items);
	obs_scene_release(s);
	// OBS lists bottom first; the dock shows the top first, like OBS's.
	for (auto it = items.rbegin(); it != items.rend(); ++it)
		out.append(*it);
	return out;
}

#define WITH_ITEM(scene, id, error)                                                    \
	obs_scene_t *s_ = findScene(scene);                                            \
	obs_sceneitem_t *item = itemIn(s_, id);                                        \
	if (!item) {                                                                   \
		obs_scene_release(s_);                                                 \
		error = "That source isn't in the vertical scene \"" + scene + "\"."; \
		return false;                                                          \
	}

bool VerticalCanvas::setItemVisible(const QString &scene, qint64 id, bool on, QString &error)
{
	WITH_ITEM(scene, id, error)
	obs_sceneitem_set_visible(item, on);
	obs_scene_release(s_);
	refreshAudio();
	return true;
}

bool VerticalCanvas::setItemLocked(const QString &scene, qint64 id, bool on, QString &error)
{
	WITH_ITEM(scene, id, error)
	obs_sceneitem_set_locked(item, on);
	obs_scene_release(s_);
	return true;
}

bool VerticalCanvas::moveItem(const QString &scene, qint64 id, int movement, QString &error)
{
	WITH_ITEM(scene, id, error)
	obs_sceneitem_set_order(item, static_cast<enum obs_order_movement>(movement));
	obs_scene_release(s_);
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
	if (obs_scene_t *scene = obs_scene_from_source(sceneSrc))
		if (obs_sceneitem_t *item = obs_scene_find_source(scene, obs_data_get_string(d, "name")))
			obs_sceneitem_remove(item);
	obs_source_release(sceneSrc);
	obs_data_release(d);
}

} // namespace

bool VerticalCanvas::removeItem(const QString &scene, qint64 id, QString &error)
{
	WITH_ITEM(scene, id, error)
	// Saved first, so OBS's Edit -> Undo can bring it back.
	obs_source_t *src = obs_sceneitem_get_source(item);
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	struct obs_sceneitem_crop c;
	obs_sceneitem_get_crop(item, &c);
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "scene", obs_source_get_uuid(obs_scene_get_source(s_)));
	obs_data_set_string(d, "name", obs_source_get_name(src));
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
	const std::string json = obs_data_get_json(d);
	const std::string what = std::string("Remove ") + obs_source_get_name(src) + " (vertical)";
	obs_data_release(d);
	obs_sceneitem_remove(item);
	obs_scene_release(s_);
	obs_frontend_add_undo_redo_action(what.c_str(), unremove, reremove, json.c_str(), json.c_str(), false);
	refreshAudio();
	return true;
}

bool VerticalCanvas::addNewSource(const QString &scene, const QString &typeId, const QString &name, QString &error,
				  obs_source_t **created)
{
	const QString n = name.trimmed();
	if (n.isEmpty()) {
		error = "Give the source a name.";
		return false;
	}
	if (obs_source_t *have = obs_get_source_by_name(n.toUtf8().constData())) {
		obs_source_release(have);
		error = "There's already a source called \"" + n + "\". Add it with \"Add existing\".";
		return false;
	}
	obs_scene_t *s = findScene(scene);
	if (!s) {
		error = "There's no vertical scene called \"" + scene + "\".";
		return false;
	}
	// A color takes its size from the main canvas (wide); here it starts phone-sized.
	obs_data_t *settings = nullptr;
	if (typeId.startsWith("color_source")) {
		settings = obs_data_create();
		obs_data_set_int(settings, "width", width_);
		obs_data_set_int(settings, "height", height_);
	}
	obs_source_t *src = obs_source_create(typeId.toUtf8().constData(), n.toUtf8().constData(), settings, nullptr);
	obs_data_release(settings);
	if (!src) {
		obs_scene_release(s);
		error = "OBS couldn't make that kind of source.";
		return false;
	}
	obs_sceneitem_t *item = obs_scene_add(s, src);
	// Fits the frame until it's moved; a new source has no size yet.
	if (item && (obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO))
		placeIn(item, 0.0f, 0.0f, float(width_), float(height_), false);
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
		error = "There's no main scene called \"" + main + "\".";
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
