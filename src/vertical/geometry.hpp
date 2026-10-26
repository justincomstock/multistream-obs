#pragma once

// Where a vertical scene's item sits on the canvas, shared by the Vertical
// Preview (handles, clicks) and the item actions (centre, fit, flip).
#include <obs.h>
#include <graphics/matrix4.h>
#include <graphics/vec3.h>

#include <algorithm>
#include <cmath>

namespace vgeom {

struct Rect {
	float x, y, w, h;
};

// The item's box on the canvas, from its box transform (unrotated items).
inline Rect boxOf(obs_sceneitem_t *item)
{
	struct matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);
	struct vec3 a, b;
	vec3_set(&a, 0.0f, 0.0f, 0.0f);
	vec3_set(&b, 1.0f, 1.0f, 0.0f);
	vec3_transform(&a, &a, &m);
	vec3_transform(&b, &b, &m);
	return {std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x), std::fabs(b.y - a.y)};
}

// What the item actually shows on the canvas (unrotated items). A "fit" item's box is
// its bounds, which can be much bigger than the picture inside it.
inline Rect visibleOf(obs_sceneitem_t *item)
{
	const Rect box = boxOf(item);
	if (obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE)
		return box;
	obs_source_t *src = obs_sceneitem_get_source(item);
	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	const float cw = float(int(obs_source_get_width(src)) - crop.left - crop.right);
	const float ch = float(int(obs_source_get_height(src)) - crop.top - crop.bottom);
	if (cw <= 0.0f || ch <= 0.0f)
		return box;
	struct matrix4 m;
	obs_sceneitem_get_draw_transform(item, &m);
	struct vec3 a, b;
	vec3_set(&a, 0.0f, 0.0f, 0.0f);
	vec3_set(&b, cw, ch, 0.0f);
	vec3_transform(&a, &a, &m);
	vec3_transform(&b, &b, &m);
	Rect r{std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x), std::fabs(b.y - a.y)};
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	if (t.crop_to_bounds) {
		const float x0 = std::max(r.x, box.x), y0 = std::max(r.y, box.y);
		const float x1 = std::min(r.x + r.w, box.x + box.w), y1 = std::min(r.y + r.h, box.y + box.h);
		r = {x0, y0, std::max(x1 - x0, 0.0f), std::max(y1 - y0, 0.0f)};
	}
	return r.w > 0.0f && r.h > 0.0f ? r : box;
}

// The upright rectangle around what the item shows, rotated or not (what "Center"
// centres).
inline Rect shownOf(obs_sceneitem_t *item)
{
	if (obs_sceneitem_get_rot(item) == 0.0f)
		return visibleOf(item);
	struct matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);
	float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
	for (int i = 0; i < 4; i++) {
		struct vec3 c;
		vec3_set(&c, float(i & 1), float(i >> 1), 0.0f);
		vec3_transform(&c, &c, &m);
		x0 = std::min(x0, c.x);
		y0 = std::min(y0, c.y);
		x1 = std::max(x1, c.x);
		y1 = std::max(y1, c.y);
	}
	return {x0, y0, x1 - x0, y1 - y0};
}

} // namespace vgeom
