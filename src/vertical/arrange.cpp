// Arranging a vertical scene's items: the right-click menu's Transform
// choices, and where a newly added source lands.
#include "canvas.hpp"
#include "geometry.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>

#include <cmath>
#include <functional>
#include <string>

namespace {

void loadStates(const char *data)
{
	obs_scene_load_transform_states(data);
}

std::string statesOf(obs_scene_t *scene)
{
	obs_data_t *d = obs_scene_save_transform_states(scene, true);
	std::string json = obs_data_get_json(d);
	obs_data_release(d);
	return json;
}

// The centre of the item's box on the canvas.
struct vec3 centreOf(obs_sceneitem_t *item)
{
	struct matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);
	struct vec3 c;
	vec3_set(&c, 0.5f, 0.5f, 0.0f);
	vec3_transform(&c, &c, &m);
	return c;
}

// Changes the transform, then moves it back so its centre stays where it was
// (rotate and flip turn in place, as OBS's do).
void inPlace(obs_sceneitem_t *item, const std::function<void(struct obs_transform_info &)> &change)
{
	const struct vec3 before = centreOf(item);
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	change(t);
	obs_sceneitem_set_info2(item, &t);
	const struct vec3 after = centreOf(item);
	t.pos.x += before.x - after.x;
	t.pos.y += before.y - after.y;
	obs_sceneitem_set_info2(item, &t);
}

// Its own size, centred in a W x H frame, scaled down to the frame's width if
// wider. `clear` also drops crops and flips (Reset).
void ownSize(obs_sceneitem_t *item, float W, float H, bool clear)
{
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	if (clear) {
		struct obs_sceneitem_crop none = {0, 0, 0, 0};
		obs_sceneitem_set_crop(item, &none);
	}
	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	const float w = float(int(obs_source_get_width(obs_sceneitem_get_source(item))) - crop.left - crop.right);
	const float s = w > W ? W / w : 1.0f;
	t.rot = 0.0f;
	t.alignment = OBS_ALIGN_CENTER;
	t.bounds_type = OBS_BOUNDS_NONE;
	t.crop_to_bounds = false;
	vec2_set(&t.pos, W / 2.0f, H / 2.0f);
	vec2_set(&t.scale, clear || t.scale.x >= 0 ? s : -s, clear || t.scale.y >= 0 ? s : -s);
	obs_sceneitem_set_info2(item, &t);
}

bool atOwnSize(obs_sceneitem_t *item, float W, float H)
{
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	return t.rot == 0.0f && t.bounds_type == OBS_BOUNDS_NONE && t.scale.x == 1.0f && t.scale.y == 1.0f &&
	       t.alignment == OBS_ALIGN_CENTER && t.pos.x == W / 2.0f && t.pos.y == H / 2.0f;
}

} // namespace

void VerticalCanvas::placeNew(obs_scene_t *scene, obs_sceneitem_t *item)
{
	if (!(obs_source_get_output_flags(obs_sceneitem_get_source(item)) & OBS_SOURCE_VIDEO))
		return;
	ownSize(item, float(width_), float(height_), true);
	if (obs_source_get_width(obs_sceneitem_get_source(item)) == 0)
		pending_.append({QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(scene))),
				 obs_sceneitem_get_id(item)});
}

void VerticalCanvas::placePending()
{
	for (int i = pending_.size() - 1; i >= 0; i--) {
		obs_source_t *sceneSrc = findSceneByUuid(pending_[i].first);
		obs_scene_t *s = obs_scene_from_source(sceneSrc);
		obs_sceneitem_t *item = s ? obs_scene_find_sceneitem_by_id(s, pending_[i].second) : nullptr;
		const bool sized = item && obs_source_get_width(obs_sceneitem_get_source(item)) > 0;
		// Scaled only if nobody has moved it since.
		if (sized && atOwnSize(item, float(width_), float(height_)))
			ownSize(item, float(width_), float(height_), false);
		if (!item || sized)
			pending_.removeAt(i);
		obs_source_release(sceneSrc);
	}
}

bool VerticalCanvas::arrangeItem(const QString &scene, qint64 id, const QString &action, QString &error)
{
	obs_scene_t *s = findScene(scene);
	obs_sceneitem_t *item = s ? obs_scene_find_sceneitem_by_id(s, id) : nullptr;
	if (!item) {
		obs_scene_release(s);
		error = "That source isn't in the vertical scene \"" + scene + "\".";
		return false;
	}
	const float W = float(width_), H = float(height_);
	const std::string before = statesOf(s);
	QString what;
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	if (action == "center" || action == "centerH" || action == "centerV") {
		const vgeom::Rect r = vgeom::shownOf(item);
		if (action != "centerV")
			t.pos.x += W / 2.0f - (r.x + r.w / 2.0f);
		if (action != "centerH")
			t.pos.y += H / 2.0f - (r.y + r.h / 2.0f);
		obs_sceneitem_set_info2(item, &t);
		what = "Center";
	} else if (action == "fit" || action == "fill") {
		// Flips and crops stay; "Fill" trims what spills past the frame.
		const bool fill = action == "fill";
		t.rot = 0.0f;
		vec2_set(&t.pos, 0.0f, 0.0f);
		vec2_set(&t.scale, t.scale.x < 0 ? -1.0f : 1.0f, t.scale.y < 0 ? -1.0f : 1.0f);
		t.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
		t.bounds_type = fill ? OBS_BOUNDS_SCALE_OUTER : OBS_BOUNDS_SCALE_INNER;
		t.bounds_alignment = OBS_ALIGN_CENTER;
		vec2_set(&t.bounds, W, H);
		t.crop_to_bounds = fill;
		obs_sceneitem_set_info2(item, &t);
		what = fill ? "Fill frame" : "Fit to frame";
	} else if (action == "fitWidth") {
		struct obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);
		const float w = float(int(obs_source_get_width(obs_sceneitem_get_source(item))) - crop.left - crop.right);
		if (w <= 0.0f) {
			obs_scene_release(s);
			error = "It has no picture yet, so it has no width to fit. Try again once it shows something.";
			return false;
		}
		const float k = W / w;
		t.rot = 0.0f;
		t.alignment = OBS_ALIGN_CENTER;
		t.bounds_type = OBS_BOUNDS_NONE;
		t.crop_to_bounds = false;
		vec2_set(&t.pos, W / 2.0f, H / 2.0f);
		vec2_set(&t.scale, t.scale.x < 0 ? -k : k, t.scale.y < 0 ? -k : k);
		obs_sceneitem_set_info2(item, &t);
		what = "Fit to frame width";
	} else if (action == "resetQuiet") {
		// The Edit transform window's Reset: its visit is one undo step of its own.
		ownSize(item, W, H, true);
		obs_scene_release(s);
		return true;
	} else if (action == "reset") {
		ownSize(item, W, H, true);
		what = "Reset size and position";
	} else if (action == "rotateRight" || action == "rotateLeft" || action == "rotate180") {
		const float by = action == "rotateRight" ? 90.0f : action == "rotateLeft" ? -90.0f : 180.0f;
		inPlace(item, [by](struct obs_transform_info &i) { i.rot = std::fmod(i.rot + by + 360.0f, 360.0f); });
		what = "Rotate";
	} else if (action == "flipH" || action == "flipV") {
		const bool h = action == "flipH";
		inPlace(item, [h](struct obs_transform_info &i) {
			if (h)
				i.scale.x = -i.scale.x;
			else
				i.scale.y = -i.scale.y;
		});
		what = h ? "Flip horizontal" : "Flip vertical";
	} else if (action == "copy") {
		copiedInfo_ = t;
		obs_sceneitem_get_crop(item, &copiedCrop_);
		copied_ = true;
		obs_scene_release(s);
		return true;
	} else if (action == "paste") {
		if (!copied_) {
			obs_scene_release(s);
			error = "Copy a source's position and size first (right-click it, Transform, Copy).";
			return false;
		}
		obs_sceneitem_set_info2(item, &copiedInfo_);
		obs_sceneitem_set_crop(item, &copiedCrop_);
		what = "Paste position and size";
	} else {
		obs_scene_release(s);
		error = "Unknown action \"" + action + "\".";
		return false;
	}
	const std::string after = statesOf(s);
	const std::string label = what.toStdString() + " (vertical)";
	if (after != before)
		obs_frontend_add_undo_redo_action(label.c_str(), loadStates, loadStates, before.c_str(), after.c_str(),
						  false);
	obs_scene_release(s);
	return true;
}
