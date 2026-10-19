#include "preview.hpp"

#include "canvas.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

const float HANDLE_PX = 8.0f; // handle size and grab distance, display pixels
const float SNAP_PX = 8.0f;

struct Rect {
	float x, y, w, h;
};

// The item's box on the canvas, from its box transform (unrotated items).
Rect boxOf(obs_sceneitem_t *item)
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

// Where an item's position sits on its box, for its alignment.
void alignOffset(uint32_t align, float w, float h, float &x, float &y)
{
	x = (align & OBS_ALIGN_LEFT) ? 0.0f : (align & OBS_ALIGN_RIGHT) ? w : w / 2.0f;
	y = (align & OBS_ALIGN_TOP) ? 0.0f : (align & OBS_ALIGN_BOTTOM) ? h : h / 2.0f;
}

// A filled rectangle in the current units (fractions allowed).
void drawRect(float x, float y, float w, float h)
{
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(w, h, 1.0f);
	gs_draw_sprite(nullptr, 0, 1, 1);
	gs_matrix_pop();
}

void drawOutline(float x, float y, float w, float h, float t)
{
	drawRect(x, y, w, t);
	drawRect(x, y + h - t, w, t);
	drawRect(x, y, t, h);
	drawRect(x + w - t, y, t, h);
}

void setColor(gs_effect_t *solid, uint32_t argb)
{
	struct vec4 c;
	vec4_from_rgba(&c, ((argb >> 16) & 0xff) | (argb & 0xff00) | ((argb & 0xff) << 16) | (argb & 0xff000000));
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &c);
}

void loadStates(const char *data)
{
	obs_scene_load_transform_states(data);
}

} // namespace

PreviewWidget::PreviewWidget(VerticalCanvas *canvas, QWidget *parent) : QWidget(parent), canvas_(canvas)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setFocusPolicy(Qt::ClickFocus);
	setMouseTracking(true);
	setMinimumSize(90, 160);
}

PreviewWidget::~PreviewWidget()
{
	destroyDisplay();
	std::lock_guard<std::mutex> lock(m_);
	obs_weak_canvas_release(weakCanvas_);
	obs_weak_source_release(weakScene_);
	weakCanvas_ = nullptr;
	weakScene_ = nullptr;
}

void PreviewWidget::sync()
{
	obs_canvas_t *c = canvas_->canvasRef();
	obs_scene_t *s = c ? canvas_->scene(canvas_->currentScene()) : nullptr;
	{
		std::lock_guard<std::mutex> lock(m_);
		obs_weak_canvas_release(weakCanvas_);
		obs_weak_source_release(weakScene_);
		weakCanvas_ = c ? obs_canvas_get_weak_canvas(c) : nullptr;
		weakScene_ = s ? obs_source_get_weak_source(obs_scene_get_source(s)) : nullptr;
	}
	canvasW_.store(uint32_t(std::max(canvas_->width(), 1)));
	canvasH_.store(uint32_t(std::max(canvas_->height(), 1)));
	if (s && selected_.load() && !obs_scene_find_sceneitem_by_id(s, selected_.load()))
		selected_.store(0);
	obs_scene_release(s);
	obs_canvas_release(c);
}

void PreviewWidget::openProjector(VerticalCanvas *canvas)
{
	auto *p = new PreviewWidget(canvas);
	p->projector_.store(true);
	p->setAttribute(Qt::WA_DeleteOnClose);
	p->setWindowTitle("Vertical projector");
	p->setToolTip(QString());
	const int h = 720;
	p->resize(h * std::max(canvas->width(), 1) / std::max(canvas->height(), 1), h);
	p->sync();
	// Follows the canvas through scene collection switches.
	auto *t = new QTimer(p);
	QObject::connect(t, &QTimer::timeout, p, [p]() { p->sync(); });
	t->start(1000);
	p->show();
}

void PreviewWidget::select(qint64 id)
{
	selected_.store(id);
}

obs_scene_t *PreviewWidget::sceneRef() const
{
	return canvas_->scene(canvas_->currentScene());
}

// ---- the display ------------------------------------------------------------

void PreviewWidget::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	createDisplay();
	if (display_)
		obs_display_set_enabled(display_, true);
}

void PreviewWidget::hideEvent(QHideEvent *e)
{
	QWidget::hideEvent(e);
	if (display_)
		obs_display_set_enabled(display_, false);
}

void PreviewWidget::paintEvent(QPaintEvent *)
{
	createDisplay();
}

bool PreviewWidget::event(QEvent *e)
{
	// Docking or floating can give the widget a new native window.
	if (e->type() == QEvent::WinIdChange && display_) {
		destroyDisplay();
		createDisplay();
	}
	return QWidget::event(e);
}

void PreviewWidget::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	if (display_) {
		const qreal dpr = devicePixelRatioF();
		obs_display_resize(display_, uint32_t(width() * dpr), uint32_t(height() * dpr));
	}
}

void PreviewWidget::createDisplay()
{
	if (display_ || !isVisible())
		return;
	const qreal dpr = devicePixelRatioF();
	struct gs_init_data info = {};
	info.cx = uint32_t(std::max(int(width() * dpr), 1));
	info.cy = uint32_t(std::max(int(height() * dpr), 1));
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
#ifdef _WIN32
	info.window.hwnd = reinterpret_cast<void *>(winId());
#else
	return; // macOS and Linux need their own window handles; the plugin ships for Windows.
#endif
	display_ = obs_display_create(&info, 0x14100e);
	if (display_)
		obs_display_add_draw_callback(display_, draw, this);
	obs_log(LOG_INFO, "vertical: preview display %s (%ux%u)", display_ ? "made" : "failed", info.cx, info.cy);
}

void PreviewWidget::destroyDisplay()
{
	if (!display_)
		return;
	obs_display_remove_draw_callback(display_, draw, this);
	obs_display_destroy(display_);
	display_ = nullptr;
}

PreviewWidget::View PreviewWidget::view(uint32_t cx, uint32_t cy) const
{
	const float W = float(canvasW_.load()), H = float(canvasH_.load());
	View v;
	const float fit = std::min(float(cx) / W, float(cy) / H);
	v.scale = fit * zoom_.load();
	v.ox = (float(cx) - W * v.scale) / 2.0f + panX_.load();
	v.oy = (float(cy) - H * v.scale) / 2.0f + panY_.load();
	return v;
}

QPointF PreviewWidget::toCanvas(const QPointF &p) const
{
	const qreal dpr = devicePixelRatioF();
	const View v = view(uint32_t(width() * dpr), uint32_t(height() * dpr));
	return QPointF((p.x() * dpr - v.ox) / v.scale, (p.y() * dpr - v.oy) / v.scale);
}

void PreviewWidget::draw(void *param, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<PreviewWidget *>(param);
	obs_canvas_t *c = nullptr;
	obs_source_t *sceneSrc = nullptr;
	{
		std::lock_guard<std::mutex> lock(self->m_);
		c = self->weakCanvas_ ? obs_weak_canvas_get_canvas(self->weakCanvas_) : nullptr;
		sceneSrc = self->weakScene_ ? obs_weak_source_get_source(self->weakScene_) : nullptr;
	}
	struct obs_video_info ovi = {};
	const bool info = c && obs_canvas_get_video_info(c, &ovi);
	const int state = !c ? 1 : !info ? 2 : 4;
	if (self->lastState_.exchange(state) != state)
		obs_log(LOG_INFO, "vertical: preview %s (%ux%u)",
			state == 1 ? "has no canvas" : state == 2 ? "has no canvas video" : "draws", cx, cy);
	if (info && cx && cy) {
		const View v = self->view(cx, cy);
		const float W = float(ovi.base_width), H = float(ovi.base_height);
		gs_viewport_push();
		gs_projection_push();
		// The whole display, in canvas units.
		gs_ortho(-v.ox / v.scale, (float(cx) - v.ox) / v.scale, -v.oy / v.scale, (float(cy) - v.oy) / v.scale,
			 -100.0f, 100.0f);
		gs_set_viewport(0, 0, int(cx), int(cy));

		// The canvas's finished frame: exactly what the vertical stream sends.
		obs_render_canvas_texture(c);
		if (self->projector_.load()) {
			gs_projection_pop();
			gs_viewport_pop();
			obs_source_release(sceneSrc);
			obs_canvas_release(c);
			return;
		}

		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		const float px = 1.0f / v.scale; // one display pixel, in canvas units
		while (gs_effect_loop(solid, "Solid")) {
			// The frame's edge.
			setColor(solid, 0x50ffffff);
			drawOutline(-px, -px, W + 2 * px, H + 2 * px, px);
			// Where TikTok / Shorts / Reels put their own buttons and captions.
			if (self->safeAreas_.load()) {
				setColor(solid, 0x30ff4060);
				drawRect(0, 0, W, H * 0.08f);          // top bar
				drawRect(0, H * 0.80f, W, H * 0.20f);  // caption and username
				drawRect(W * 0.86f, H * 0.35f, W * 0.14f, H * 0.45f); // like/share column
			}
			// The selected item, with its corner handles.
			obs_scene_t *scene = sceneSrc ? obs_scene_from_source(sceneSrc) : nullptr;
			obs_sceneitem_t *item = scene && self->selected_.load()
							? obs_scene_find_sceneitem_by_id(scene, self->selected_.load())
							: nullptr;
			if (item) {
				obs_sceneitem_addref(item);
				struct matrix4 m;
				obs_sceneitem_get_box_transform(item, &m);
				setColor(solid, 0xff7c6cff);
				gs_matrix_push();
				gs_matrix_mul(&m);
				// The box is the unit square here: its outline, 2 px wide.
				const float sx = std::max(std::sqrt(m.x.x * m.x.x + m.x.y * m.x.y), 0.0001f);
				const float sy = std::max(std::sqrt(m.y.x * m.y.x + m.y.y * m.y.y), 0.0001f);
				drawRect(0, 0, 1, 2 * px / sy);
				drawRect(0, 1 - 2 * px / sy, 1, 2 * px / sy);
				drawRect(0, 0, 2 * px / sx, 1);
				drawRect(1 - 2 * px / sx, 0, 2 * px / sx, 1);
				gs_matrix_pop();
				if (obs_sceneitem_get_rot(item) == 0.0f && !obs_sceneitem_locked(item)) {
					const Rect b = boxOf(item);
					const float h = HANDLE_PX * px;
					const float hx[4] = {b.x, b.x + b.w, b.x + b.w, b.x};
					const float hy[4] = {b.y, b.y, b.y + b.h, b.y + b.h};
					for (int i = 0; i < 4; i++)
						drawRect(hx[i] - h / 2, hy[i] - h / 2, h, h);
				}
				obs_sceneitem_release(item);
			}
		}
		gs_projection_pop();
		gs_viewport_pop();
	}
	obs_source_release(sceneSrc);
	obs_canvas_release(c);
}

// ---- editing --------------------------------------------------------------------

obs_sceneitem_t *PreviewWidget::itemAt(obs_scene_t *scene, const QPointF &p) const
{
	struct Hit {
		QPointF p;
		obs_sceneitem_t *item;
	} hit{p, nullptr};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *h = static_cast<Hit *>(param);
			obs_source_t *src = obs_sceneitem_get_source(item);
			if (!obs_sceneitem_visible(item) || obs_sceneitem_locked(item) ||
			    !(obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO))
				return true;
			struct matrix4 m, inv;
			obs_sceneitem_get_box_transform(item, &m);
			matrix4_inv(&inv, &m);
			struct vec3 v;
			vec3_set(&v, float(h->p.x()), float(h->p.y()), 0.0f);
			vec3_transform(&v, &v, &inv);
			if (v.x >= 0.0f && v.x <= 1.0f && v.y >= 0.0f && v.y <= 1.0f)
				h->item = item; // later = higher: the top one wins
			return true;
		},
		&hit);
	return hit.item;
}

int PreviewWidget::cornerAt(obs_scene_t *scene, const QPointF &p) const
{
	obs_sceneitem_t *item = selected_.load() ? obs_scene_find_sceneitem_by_id(scene, selected_.load()) : nullptr;
	if (!item || obs_sceneitem_locked(item) || obs_sceneitem_get_rot(item) != 0.0f)
		return -1;
	const qreal dpr = devicePixelRatioF();
	const float reach = HANDLE_PX / view(uint32_t(width() * dpr), uint32_t(height() * dpr)).scale;
	const Rect b = boxOf(item);
	const QPointF corners[4] = {{b.x, b.y}, {b.x + b.w, b.y}, {b.x + b.w, b.y + b.h}, {b.x, b.y + b.h}};
	for (int i = 0; i < 4; i++)
		if (std::fabs(p.x() - corners[i].x()) <= reach && std::fabs(p.y() - corners[i].y()) <= reach)
			return i;
	return -1;
}

void PreviewWidget::beginUndo()
{
	undoBefore_.clear();
	if (obs_scene_t *s = sceneRef()) {
		obs_data_t *d = obs_scene_save_transform_states(s, true);
		undoBefore_ = QString::fromUtf8(obs_data_get_json(d));
		obs_data_release(d);
		obs_scene_release(s);
	}
}

void PreviewWidget::endUndo(const char *what)
{
	obs_scene_t *s = sceneRef();
	if (!s || undoBefore_.isEmpty()) {
		obs_scene_release(s);
		return;
	}
	obs_data_t *d = obs_scene_save_transform_states(s, true);
	const QString after = QString::fromUtf8(obs_data_get_json(d));
	obs_data_release(d);
	obs_scene_release(s);
	if (after != undoBefore_)
		obs_frontend_add_undo_redo_action(what, loadStates, loadStates, undoBefore_.toUtf8().constData(),
						  after.toUtf8().constData(), false);
	undoBefore_.clear();
}

void PreviewWidget::mousePressEvent(QMouseEvent *e)
{
	if (projector_.load())
		return;
	setFocus();
	pressWidget_ = e->position();
	pressCanvas_ = toCanvas(e->position());
	if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && space_)) {
		drag_ = Drag::Pan;
		startPanX_ = panX_.load();
		startPanY_ = panY_.load();
		return;
	}
	if (e->button() != Qt::LeftButton)
		return;
	obs_scene_t *s = sceneRef();
	if (!s)
		return;
	corner_ = cornerAt(s, pressCanvas_);
	obs_sceneitem_t *item = nullptr;
	if (corner_ >= 0) {
		item = obs_scene_find_sceneitem_by_id(s, selected_.load());
		const bool canCrop = obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE;
		drag_ = (e->modifiers() & Qt::AltModifier) && canCrop ? Drag::Crop : Drag::Resize;
	} else {
		item = itemAt(s, pressCanvas_);
		selected_.store(item ? obs_sceneitem_get_id(item) : 0);
		if (onSelect)
			onSelect(selected_.load());
		drag_ = item ? Drag::Move : Drag::None;
	}
	if (item) {
		const Rect b = boxOf(item);
		boxX_ = b.x;
		boxY_ = b.y;
		boxW_ = b.w;
		boxH_ = b.h;
		obs_sceneitem_get_info2(item, &startInfo_);
		obs_sceneitem_get_crop(item, &startCrop_);
		obs_source_t *src = obs_sceneitem_get_source(item);
		srcW_ = obs_source_get_width(src);
		srcH_ = obs_source_get_height(src);
		beginUndo();
	}
	obs_scene_release(s);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent *e)
{
	if (drag_ == Drag::Pan) {
		const qreal dpr = devicePixelRatioF();
		panX_.store(startPanX_ + float((e->position().x() - pressWidget_.x()) * dpr));
		panY_.store(startPanY_ + float((e->position().y() - pressWidget_.y()) * dpr));
		return;
	}
	if (drag_ == Drag::None) {
		obs_scene_t *s = sceneRef();
		const int c = s ? cornerAt(s, toCanvas(e->position())) : -1;
		obs_scene_release(s);
		setCursor(c == 0 || c == 2 ? Qt::SizeFDiagCursor : c == 1 || c == 3 ? Qt::SizeBDiagCursor : Qt::ArrowCursor);
		return;
	}
	obs_scene_t *s = sceneRef();
	obs_sceneitem_t *item = s && selected_.load() ? obs_scene_find_sceneitem_by_id(s, selected_.load()) : nullptr;
	if (!item) {
		obs_scene_release(s);
		return;
	}
	const QPointF p = toCanvas(e->position());
	const float W = float(canvas_->width()), H = float(canvas_->height());
	const qreal dpr = devicePixelRatioF();
	const float snap = SNAP_PX / view(uint32_t(width() * dpr), uint32_t(height() * dpr)).scale;
	const bool noSnap = (e->modifiers() & Qt::ShiftModifier) != 0;
	struct obs_transform_info t = startInfo_;

	if (drag_ == Drag::Move) {
		float dx = float(p.x() - pressCanvas_.x()), dy = float(p.y() - pressCanvas_.y());
		if (!noSnap) {
			// Snap the box's edges and centre to the frame's.
			auto snapAxis = [&](float start, float size, float total, float &d) {
				const float edges[3] = {start + d, start + d + size / 2, start + d + size};
				const float targets[3] = {0.0f, total / 2, total};
				float best = snap + 1;
				float adjust = 0;
				for (float edge : edges)
					for (float target : targets)
						if (std::fabs(target - edge) < best) {
							best = std::fabs(target - edge);
							adjust = target - edge;
						}
				if (best <= snap)
					d += adjust;
			};
			snapAxis(boxX_, boxW_, W, dx);
			snapAxis(boxY_, boxH_, H, dy);
		}
		t.pos.x = startInfo_.pos.x + dx;
		t.pos.y = startInfo_.pos.y + dy;
		obs_sceneitem_set_info2(item, &t);
	} else if (drag_ == Drag::Resize && boxW_ > 0 && boxH_ > 0) {
		// The opposite corner stays put.
		const bool right = corner_ == 1 || corner_ == 2, bottom = corner_ == 2 || corner_ == 3;
		const float ax = right ? boxX_ : boxX_ + boxW_, ay = bottom ? boxY_ : boxY_ + boxH_;
		float w = std::max(std::fabs(float(p.x()) - ax), 4.0f), h = std::max(std::fabs(float(p.y()) - ay), 4.0f);
		if (!noSnap) {
			const float k = std::max(w / boxW_, h / boxH_);
			w = boxW_ * k;
			h = boxH_ * k;
		}
		const float left = right ? ax : ax - w, top = bottom ? ay : ay - h;
		if (t.bounds_type != OBS_BOUNDS_NONE) {
			t.bounds.x = w / std::max(std::fabs(t.scale.x), 0.0001f);
			t.bounds.y = h / std::max(std::fabs(t.scale.y), 0.0001f);
		} else {
			t.scale.x = startInfo_.scale.x * (w / boxW_);
			t.scale.y = startInfo_.scale.y * (h / boxH_);
		}
		float offX, offY;
		alignOffset(t.alignment, w, h, offX, offY);
		t.pos.x = left + offX;
		t.pos.y = top + offY;
		obs_sceneitem_set_info2(item, &t);
	} else if (drag_ == Drag::Crop && srcW_ && srcH_) {
		const float sx = std::max(std::fabs(startInfo_.scale.x), 0.0001f);
		const float sy = std::max(std::fabs(startInfo_.scale.y), 0.0001f);
		const float dx = float(p.x() - pressCanvas_.x()), dy = float(p.y() - pressCanvas_.y());
		struct obs_sceneitem_crop c = startCrop_;
		const bool right = corner_ == 1 || corner_ == 2, bottom = corner_ == 2 || corner_ == 3;
		const int maxW = int(srcW_) - 4, maxH = int(srcH_) - 4;
		if (right)
			c.right = std::clamp(int(std::lround(startCrop_.right - dx / sx)), 0, maxW - c.left);
		else
			c.left = std::clamp(int(std::lround(startCrop_.left + dx / sx)), 0, maxW - c.right);
		if (bottom)
			c.bottom = std::clamp(int(std::lround(startCrop_.bottom - dy / sy)), 0, maxH - c.top);
		else
			c.top = std::clamp(int(std::lround(startCrop_.top + dy / sy)), 0, maxH - c.bottom);
		obs_sceneitem_set_crop(item, &c);
		// The uncropped edges stay where they were.
		const float left = boxX_ + float(c.left - startCrop_.left) * sx;
		const float top = boxY_ + float(c.top - startCrop_.top) * sy;
		const float w = float(int(srcW_) - c.left - c.right) * sx, h = float(int(srcH_) - c.top - c.bottom) * sy;
		float offX, offY;
		alignOffset(t.alignment, w, h, offX, offY);
		t.pos.x = left + offX;
		t.pos.y = top + offY;
		obs_sceneitem_set_info2(item, &t);
	}
	obs_scene_release(s);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent *)
{
	if (drag_ == Drag::Move)
		endUndo("Move vertical source");
	else if (drag_ == Drag::Resize)
		endUndo("Resize vertical source");
	else if (drag_ == Drag::Crop)
		endUndo("Crop vertical source");
	drag_ = Drag::None;
	corner_ = -1;
}

void PreviewWidget::wheelEvent(QWheelEvent *e)
{
	if (!(e->modifiers() & Qt::ControlModifier) || projector_.load()) {
		QWidget::wheelEvent(e);
		return;
	}
	// Zoom around the mouse: the canvas point under it stays under it.
	const qreal dpr = devicePixelRatioF();
	const uint32_t cx = uint32_t(width() * dpr), cy = uint32_t(height() * dpr);
	const QPointF before = toCanvas(e->position());
	const float z = std::clamp(zoom_.load() * std::pow(1.15f, float(e->angleDelta().y()) / 120.0f), 0.5f, 8.0f);
	zoom_.store(z);
	const View v = view(cx, cy);
	const float mx = float(e->position().x() * dpr), my = float(e->position().y() * dpr);
	panX_.store(panX_.load() + (mx - (v.ox + float(before.x()) * v.scale)));
	panY_.store(panY_.load() + (my - (v.oy + float(before.y()) * v.scale)));
	e->accept();
}

void PreviewWidget::nudge(float dx, float dy)
{
	obs_scene_t *s = sceneRef();
	obs_sceneitem_t *item = s && selected_.load() ? obs_scene_find_sceneitem_by_id(s, selected_.load()) : nullptr;
	if (item && !obs_sceneitem_locked(item)) {
		beginUndo();
		struct vec2 pos;
		obs_sceneitem_get_pos(item, &pos);
		pos.x += dx;
		pos.y += dy;
		obs_sceneitem_set_pos(item, &pos);
		endUndo("Nudge vertical source");
	}
	obs_scene_release(s);
}

void PreviewWidget::keyPressEvent(QKeyEvent *e)
{
	if (projector_.load()) {
		if (e->key() == Qt::Key_Escape && isFullScreen())
			showNormal();
		return;
	}
	const float step = (e->modifiers() & Qt::ShiftModifier) ? 10.0f : 1.0f;
	switch (e->key()) {
	case Qt::Key_Space:
		if (!e->isAutoRepeat())
			space_ = true;
		setCursor(Qt::OpenHandCursor);
		return;
	case Qt::Key_Left:
		nudge(-step, 0);
		return;
	case Qt::Key_Right:
		nudge(step, 0);
		return;
	case Qt::Key_Up:
		nudge(0, -step);
		return;
	case Qt::Key_Down:
		nudge(0, step);
		return;
	case Qt::Key_0:
		if (e->modifiers() & Qt::ControlModifier) {
			zoom_.store(1.0f);
			panX_.store(0);
			panY_.store(0);
			return;
		}
		break;
	case Qt::Key_Delete: {
		const qint64 id = selected_.load();
		if (!id)
			break;
		if (QMessageBox::question(this, "Vertical Preview", "Remove the selected source from this vertical scene?") ==
		    QMessageBox::Yes) {
			QString error;
			canvas_->removeItem(canvas_->currentScene(), id, error);
			selected_.store(0);
		}
		return;
	}
	default:
		break;
	}
	QWidget::keyPressEvent(e);
}

void PreviewWidget::keyReleaseEvent(QKeyEvent *e)
{
	if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
		space_ = false;
		setCursor(Qt::ArrowCursor);
		return;
	}
	QWidget::keyReleaseEvent(e);
}

void PreviewWidget::contextMenuEvent(QContextMenuEvent *e)
{
	if (projector_.load()) {
		QMenu menu(this);
		QMenu *full = menu.addMenu("Fullscreen on");
		const QList<QScreen *> screens = QGuiApplication::screens();
		for (int i = 0; i < screens.size(); i++) {
			QScreen *sc = screens[i];
			full->addAction(QString("Screen %1 (%2\u00d7%3)").arg(i + 1).arg(sc->size().width()).arg(sc->size().height()),
					[this, sc]() {
						showNormal();
						setGeometry(sc->geometry());
						showFullScreen();
					});
		}
		if (isFullScreen())
			menu.addAction("Leave fullscreen (Esc)", [this]() { showNormal(); });
		menu.addAction("Close", [this]() { close(); });
		menu.exec(e->globalPos());
		return;
	}
	obs_scene_t *s = sceneRef();
	if (!s)
		return;
	obs_sceneitem_t *item = itemAt(s, toCanvas(e->pos()));
	if (item) {
		selected_.store(obs_sceneitem_get_id(item));
		if (onSelect)
			onSelect(selected_.load());
	}
	QMenu menu(this);
	if (item) {
		obs_source_t *src = obs_sceneitem_get_source(item);
		const qint64 id = obs_sceneitem_get_id(item);
		const QString scene = canvas_->currentScene();
		const float W = float(canvas_->width()), H = float(canvas_->height());
		auto place = [this, scene, id, W, H](int boundsType) {
			QString error;
			beginUndo();
			QJsonObject t{{"x", 0.0}, {"y", 0.0}, {"scaleX", 1.0}, {"scaleY", 1.0}, {"rot", 0.0},
				      {"align", int(OBS_ALIGN_LEFT | OBS_ALIGN_TOP)}, {"boundsType", boundsType},
				      {"boundsW", W}, {"boundsH", H}, {"cropLeft", 0}, {"cropTop", 0}, {"cropRight", 0},
				      {"cropBottom", 0}};
			canvas_->setItemTransform(scene, id, t, error);
			endUndo("Fit vertical source");
		};
		menu.addAction("Properties…", [src]() { obs_frontend_open_source_properties(src); });
		menu.addAction("Filters…", [src]() { obs_frontend_open_source_filters(src); });
		menu.addSeparator();
		menu.addAction("Fill the frame", [place]() { place(int(OBS_BOUNDS_SCALE_OUTER)); });
		menu.addAction("Fit in the frame", [place]() { place(int(OBS_BOUNDS_SCALE_INNER)); });
		menu.addAction("Rotate 90° right", [this, scene, id]() {
			QString error;
			beginUndo();
			const QJsonObject cur = canvas_->itemTransform(scene, id);
			canvas_->setItemTransform(scene, id, {{"rot", std::fmod(cur.value("rot").toDouble() + 90.0, 360.0)}},
						  error);
			endUndo("Rotate vertical source");
		});
		menu.addAction("Rotate 90° left", [this, scene, id]() {
			QString error;
			beginUndo();
			const QJsonObject cur = canvas_->itemTransform(scene, id);
			canvas_->setItemTransform(scene, id,
						  {{"rot", std::fmod(cur.value("rot").toDouble() + 270.0, 360.0)}}, error);
			endUndo("Rotate vertical source");
		});
		menu.addSeparator();
		menu.addAction("Remove from this scene", [this, scene, id]() {
			QString error;
			canvas_->removeItem(scene, id, error);
			selected_.store(0);
		});
		menu.addSeparator();
	}
	QAction *safe = menu.addAction("Show TikTok / Shorts safe areas", [this](bool on) { setSafeAreas(on); });
	safe->setCheckable(true);
	safe->setChecked(safeAreas());
	menu.addAction("Zoom to fit (Ctrl+0)", [this]() {
		zoom_.store(1.0f);
		panX_.store(0);
		panY_.store(0);
	});
	VerticalCanvas *c = canvas_;
	menu.addAction("Open vertical projector", [c]() { openProjector(c); });
	obs_scene_release(s);
	menu.exec(e->globalPos());
}
