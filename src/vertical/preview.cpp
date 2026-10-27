#include "preview.hpp"

#include "canvas.hpp"
#include "../i18n.hpp"
#include "geometry.hpp"
#include "item-menu.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFocusEvent>
#include <QPointer>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QHideEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStringList>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

const float HANDLE_PX = 8.0f; // handle size and grab distance, screen pixels
const float SNAP_PX = 8.0f;
const char *TITLE = "Vertical";

using vgeom::boxOf;
using vgeom::Rect;
using vgeom::shownOf;
using vgeom::visibleOf;

// The 8 handles in the item's unit square: corners TL TR BR BL, then sides T R B L.
const float HANDLE_U[8] = {0, 1, 1, 0, 0.5f, 1, 0.5f, 0};
const float HANDLE_V[8] = {0, 0, 1, 1, 0, 0.5f, 1, 0.5f};

const float NO_GUIDE = std::numeric_limits<float>::quiet_NaN();

// Where the phone apps put their own buttons and captions, as parts of the frame.
struct Zone {
	float x, y, w, h;
	const char *what; // its locale key
};
struct SafeArea {
	const char *name;
	Zone zones[3];
};
const SafeArea SAFE_AREAS[3] = {
	{"TikTok",
	 {{0, 0, 1, 0.08f, "Vertical.ZoneTikTokTop"},
	  {0, 0.80f, 1, 0.20f, "Vertical.ZoneTikTokCaption"},
	  {0.86f, 0.35f, 0.14f, 0.45f, "Vertical.ZoneTikTokButtons"}}},
	{"YouTube Shorts",
	 {{0, 0, 1, 0.07f, "Vertical.ZoneYouTubeTop"},
	  {0, 0.79f, 1, 0.21f, "Vertical.ZoneYouTubeCaption"},
	  {0.85f, 0.45f, 0.15f, 0.34f, "Vertical.ZoneYouTubeButtons"}}},
	{"Instagram Reels",
	 {{0, 0, 1, 0.07f, "Vertical.ZoneInstagramTop"},
	  {0, 0.80f, 1, 0.20f, "Vertical.ZoneInstagramCaption"},
	  {0.86f, 0.50f, 0.14f, 0.30f, "Vertical.ZoneInstagramButtons"}}},
};

bool isVideo(obs_sceneitem_t *item)
{
	return obs_sceneitem_visible(item) &&
	       (obs_source_get_output_flags(obs_sceneitem_get_source(item)) & OBS_SOURCE_VIDEO);
}

// Whether a canvas point is on the item's picture (what a click there grabs).
bool hits(obs_sceneitem_t *item, const QPointF &p)
{
	if (!isVideo(item))
		return false;
	if (obs_sceneitem_get_rot(item) == 0.0f) {
		// Only what's visible: a "fit" item's empty bounds don't catch clicks.
		const Rect r = visibleOf(item);
		return p.x() >= r.x && p.x() <= r.x + r.w && p.y() >= r.y && p.y() <= r.y + r.h;
	}
	struct matrix4 m, inv;
	obs_sceneitem_get_box_transform(item, &m);
	matrix4_inv(&inv, &m);
	struct vec3 v;
	vec3_set(&v, float(p.x()), float(p.y()), 0.0f);
	vec3_transform(&v, &v, &inv);
	return v.x >= 0.0f && v.x <= 1.0f && v.y >= 0.0f && v.y <= 1.0f;
}

// The item with this id (at the top or in a group), with references taken under
// the scenes' locks, so the render thread can't race a removal on the UI thread.
// `group` gets its group, if it's in one. Release both after.
obs_sceneitem_t *itemRef(obs_scene_t *scene, qint64 id, obs_sceneitem_t **group)
{
	struct Find {
		qint64 id;
		obs_sceneitem_t *item;
		obs_sceneitem_t *group;
	} f{id, nullptr, nullptr};
	*group = nullptr;
	if (!scene || !id)
		return nullptr;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *f = static_cast<Find *>(param);
			if (obs_sceneitem_get_id(item) == f->id) {
				obs_sceneitem_addref(item);
				f->item = item;
				return false;
			}
			if (obs_sceneitem_is_group(item)) {
				f->group = item;
				obs_sceneitem_group_enum_items(
					item,
					[](obs_scene_t *, obs_sceneitem_t *child, void *param) {
						auto *f = static_cast<Find *>(param);
						if (obs_sceneitem_get_id(child) != f->id)
							return true;
						obs_sceneitem_addref(child);
						f->item = child;
						return false;
					},
					f);
				if (f->item) {
					obs_sceneitem_addref(item);
					return false;
				}
				f->group = nullptr;
			}
			return true;
		},
		&f);
	*group = f.group;
	return f.item;
}

// Sets the transform, then shifts it so its picture's top-left lands on (left, top).
// Works for any alignment and for flipped items (OBS updates the box right away).
void setAt(obs_sceneitem_t *item, struct obs_transform_info &t, float left, float top)
{
	obs_sceneitem_set_info2(item, &t);
	const Rect r = visibleOf(item);
	t.pos.x += left - r.x;
	t.pos.y += top - r.y;
	obs_sceneitem_set_info2(item, &t);
}

// Whether OBS trims the item to its box ("Crop to bounding box" on a Fill-type box).
bool trimsToBox(const struct obs_transform_info &t)
{
	return t.crop_to_bounds && (t.bounds_type == OBS_BOUNDS_SCALE_OUTER || t.bounds_type == OBS_BOUNDS_SCALE_TO_WIDTH ||
				    t.bounds_type == OBS_BOUNDS_SCALE_TO_HEIGHT);
}

// A "fit" or "fill" item without trimming becomes a plain scaled item showing the
// same picture, so handles and crops act on what's visible.
void bakeBounds(obs_sceneitem_t *item)
{
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	if (t.bounds_type == OBS_BOUNDS_NONE || trimsToBox(t))
		return;
	obs_source_t *src = obs_sceneitem_get_source(item);
	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	const float cw = float(int(obs_source_get_width(src)) - crop.left - crop.right);
	const float ch = float(int(obs_source_get_height(src)) - crop.top - crop.bottom);
	if (cw <= 0.0f || ch <= 0.0f)
		return;
	const Rect v = visibleOf(item);
	t.bounds_type = OBS_BOUNDS_NONE;
	t.crop_to_bounds = false;
	t.scale.x = std::copysign(v.w / cw, t.scale.x);
	t.scale.y = std::copysign(v.h / ch, t.scale.y);
	setAt(item, t, v.x, v.y);
}

// A "Fill" item trimmed to its box becomes a plain scaled item with that trim as
// its own crop, the same picture, so Alt-drag can crop it further.
void bakeTrim(obs_sceneitem_t *item)
{
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	if (!trimsToBox(t))
		return;
	obs_source_t *src = obs_sceneitem_get_source(item);
	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	const float cw = float(int(obs_source_get_width(src)) - crop.left - crop.right);
	const float ch = float(int(obs_source_get_height(src)) - crop.top - crop.bottom);
	const float bx = std::fabs(t.bounds.x), by = std::fabs(t.bounds.y);
	if (cw <= 0.0f || ch <= 0.0f || bx <= 0.0f || by <= 0.0f)
		return;
	// The scale OBS gives it (calculate_bounds_data in libobs).
	const float w = cw * std::fabs(t.scale.x), h = ch * std::fabs(t.scale.y);
	float mul = 1.0f;
	if (t.bounds_type == OBS_BOUNDS_SCALE_OUTER)
		mul = (bx / by < w / h) ? by / h : bx / w;
	else if (t.bounds_type == OBS_BOUNDS_SCALE_TO_WIDTH)
		mul = bx / w;
	else
		mul = by / h;
	const float sx = std::max(std::fabs(t.scale.x) * mul, 0.0001f), sy = std::max(std::fabs(t.scale.y) * mul, 0.0001f);
	// What sticks out of the box, in the source's pixels, split by the box's alignment.
	auto trim = [](float over, bool alignLow, bool alignHigh, bool flipped, int &low, int &high) {
		if (over <= 0.1f)
			return;
		const float first = alignLow ? 0.0f : alignHigh ? over : over / 2; // left or top on screen
		const int a = int(std::lround(first)), b = int(std::lround(over - first));
		if (flipped) {
			high += a;
			low += b;
		} else {
			low += a;
			high += b;
		}
	};
	const Rect v = visibleOf(item); // the box
	trim((cw * sx - bx) / sx, t.bounds_alignment & OBS_ALIGN_LEFT, t.bounds_alignment & OBS_ALIGN_RIGHT,
	     t.scale.x < 0, crop.left, crop.right);
	trim((ch * sy - by) / sy, t.bounds_alignment & OBS_ALIGN_TOP, t.bounds_alignment & OBS_ALIGN_BOTTOM,
	     t.scale.y < 0, crop.top, crop.bottom);
	obs_sceneitem_set_crop(item, &crop);
	t.bounds_type = OBS_BOUNDS_NONE;
	t.crop_to_bounds = false;
	t.scale.x = std::copysign(sx, t.scale.x);
	t.scale.y = std::copysign(sy, t.scale.y);
	setAt(item, t, v.x, v.y);
}

// The handles' canvas points: on what's visible for an upright item, on its box
// for a rotated one.
void handlePoints(obs_sceneitem_t *item, QPointF pts[8])
{
	if (obs_sceneitem_get_rot(item) == 0.0f) {
		const Rect b = visibleOf(item);
		for (int i = 0; i < 8; i++)
			pts[i] = QPointF(b.x + b.w * HANDLE_U[i], b.y + b.h * HANDLE_V[i]);
		return;
	}
	struct matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);
	for (int i = 0; i < 8; i++) {
		struct vec3 v;
		vec3_set(&v, HANDLE_U[i], HANDLE_V[i], 0.0f);
		vec3_transform(&v, &v, &m);
		pts[i] = QPointF(v.x, v.y);
	}
}

// A resize cursor pointing the way handle `i` pulls.
Qt::CursorShape handleCursor(obs_sceneitem_t *item, int i)
{
	float ax = 1, ay = 0, bx = 0, by = 1; // the box's sides, as directions
	if (obs_sceneitem_get_rot(item) != 0.0f) {
		struct matrix4 m;
		obs_sceneitem_get_box_transform(item, &m);
		const float lx = std::max(std::hypot(m.x.x, m.x.y), 0.0001f);
		const float ly = std::max(std::hypot(m.y.x, m.y.y), 0.0001f);
		ax = m.x.x / lx;
		ay = m.x.y / lx;
		bx = m.y.x / ly;
		by = m.y.y / ly;
	}
	const float u = HANDLE_U[i] - 0.5f, v = HANDLE_V[i] - 0.5f;
	const float dx = u * ax + v * bx, dy = u * ay + v * by;
	float deg = std::atan2(dy, dx) * 180.0f / 3.14159265f;
	deg = std::fmod(deg + 360.0f, 180.0f);
	if (deg < 22.5f || deg >= 157.5f)
		return Qt::SizeHorCursor;
	if (deg < 67.5f)
		return Qt::SizeFDiagCursor;
	if (deg < 112.5f)
		return Qt::SizeVerCursor;
	return Qt::SizeBDiagCursor;
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

// The item's outline, `t` canvas units wide: around what's visible for an upright
// item, around its box when rotated or inside a group (`group`).
void outlineItem(obs_sceneitem_t *item, obs_sceneitem_t *group, float t)
{
	if (!group && obs_sceneitem_get_rot(item) == 0.0f) {
		const Rect v = visibleOf(item);
		drawOutline(v.x, v.y, v.w, v.h, t);
		return;
	}
	struct matrix4 box, gm;
	obs_sceneitem_get_box_transform(item, &box);
	if (group)
		obs_sceneitem_get_draw_transform(group, &gm);
	auto at = [&](float x, float y) {
		struct vec3 v;
		vec3_set(&v, x, y, 0.0f);
		vec3_transform(&v, &v, &box);
		if (group)
			vec3_transform(&v, &v, &gm);
		return v;
	};
	const struct vec3 o = at(0, 0), ex = at(1, 0), ey = at(0, 1);
	const float sx = std::max(std::hypot(ex.x - o.x, ex.y - o.y), 0.0001f);
	const float sy = std::max(std::hypot(ey.x - o.x, ey.y - o.y), 0.0001f);
	// As OBS draws a group's items: the group's transform, then the item's box.
	gs_matrix_push();
	if (group)
		gs_matrix_mul(&gm);
	gs_matrix_mul(&box);
	drawRect(0, 0, 1, t / sy);
	drawRect(0, 1 - t / sy, 1, t / sy);
	drawRect(0, 0, t / sx, 1);
	drawRect(1 - t / sx, 0, t / sx, 1);
	gs_matrix_pop();
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

// Snaps `edges` (moved by `d`) to the nearest target within `reach`: adjusts `d`
// and returns the target lined up with, or NaN.
float snapAxis(const float *edges, int n, const std::vector<float> &targets, float reach, float &d)
{
	float best = reach;
	float adjust = 0, hit = NO_GUIDE;
	for (int i = 0; i < n; i++)
		for (float target : targets)
			if (std::fabs(target - (edges[i] + d)) <= best) {
				best = std::fabs(target - (edges[i] + d));
				adjust = target - (edges[i] + d);
				hit = target;
			}
	d += adjust;
	return hit;
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
	setAcceptDrops(true);
	setMinimumSize(90, 160);
	guideX_.store(NO_GUIDE);
	guideY_.store(NO_GUIDE);
	safeArea_.store(std::clamp(canvas_->uiInt("safeArea", 0), 0, 3));
	// A burst of arrow-key nudges is one undo step.
	nudgeTimer_ = new QTimer(this);
	nudgeTimer_->setSingleShot(true);
	nudgeTimer_->setInterval(700);
	QObject::connect(nudgeTimer_, &QTimer::timeout, this, [this]() { flushNudge(); });
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
	const uint32_t w = uint32_t(std::max(canvas_->width(), 1)), h = uint32_t(std::max(canvas_->height(), 1));
	const bool resized = canvasW_.exchange(w) != w;
	if (canvasH_.exchange(h) != h || resized)
		clampPan();
	// Item ids are per scene: another scene's item 3 isn't the one that was selected,
	// and a drag or a burst of nudges can't carry over into it.
	const QString shown = canvas_->currentScene();
	if (shown != shownScene_) {
		shownScene_ = shown;
		selected_.store(0);
		hover_.store(0);
		drag_ = Drag::None;
		nudging_ = false;
		nudgeTimer_->stop();
		undoBefore_.clear();
		guideX_.store(NO_GUIDE);
		guideY_.store(NO_GUIDE);
		if (onSelect)
			onSelect(0);
	}
	if (s && selected_.load() && !VerticalCanvas::itemById(s, selected_.load()))
		selected_.store(0);
	obs_scene_release(s);
	obs_canvas_release(c);
}

namespace {
QList<QPointer<PreviewWidget>> projectors;
}

void PreviewWidget::closeProjectors()
{
	for (const QPointer<PreviewWidget> &p : projectors)
		if (p)
			delete p.data();
	projectors.clear();
}

void PreviewWidget::focusOutEvent(QFocusEvent *e)
{
	// Space released while focus was elsewhere never arrives here.
	space_ = false;
	if (drag_ == Drag::Pan)
		drag_ = Drag::None;
	flushNudge();
	if (!projector_.load())
		setCursor(Qt::ArrowCursor);
	QWidget::focusOutEvent(e);
}

void PreviewWidget::openProjector(VerticalCanvas *canvas)
{
	auto *p = new PreviewWidget(canvas);
	projectors.append(p);
	p->projector_.store(true);
	p->setAcceptDrops(false);
	p->setAttribute(Qt::WA_DeleteOnClose);
	p->setWindowTitle(mt("Vertical.ProjectorTitle"));
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
	if (id != selected_.load())
		flushNudge();
	selected_.store(id);
}

void PreviewWidget::setSafeArea(int which)
{
	which = std::clamp(which, 0, 3);
	safeArea_.store(which);
	if (!projector_.load())
		canvas_->setUiInt("safeArea", which);
}

obs_scene_t *PreviewWidget::sceneRef() const
{
	return canvas_->scene(canvas_->currentScene());
}

// ---- the display ------------------------------------------------------------

void PreviewWidget::setDpr()
{
	dpr_.store(float(devicePixelRatioF()));
}

void PreviewWidget::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	setDpr();
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
	// Moved to a screen with other scaling: the display needs its new pixel size.
	if (e->type() == QEvent::DevicePixelRatioChange) {
		setDpr();
		if (display_)
			obs_display_resize(display_, uint32_t(width() * devicePixelRatioF()),
					   uint32_t(height() * devicePixelRatioF()));
	}
#endif
	if (e->type() == QEvent::ShortcutOverride && !projector_.load()) {
		// OBS's main window has Ctrl+C, Ctrl+V and Delete for its own sources: here
		// they're for the vertical ones. Undo stays OBS's, after the nudges so far.
		auto *k = static_cast<QKeyEvent *>(e);
		if (k->matches(QKeySequence::Undo) || k->matches(QKeySequence::Redo)) {
			flushNudge();
		} else if (k->matches(QKeySequence::Copy) || k->matches(QKeySequence::Paste) ||
			   ((k->modifiers() & Qt::ControlModifier) && k->key() == Qt::Key_E) || k->key() == Qt::Key_Delete ||
			   k->key() == Qt::Key_Backspace || k->key() == Qt::Key_Left || k->key() == Qt::Key_Right ||
			   k->key() == Qt::Key_Up || k->key() == Qt::Key_Down) {
			e->accept();
			return true;
		}
	}
	return QWidget::event(e);
}

void PreviewWidget::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	setDpr();
	if (display_) {
		const qreal dpr = devicePixelRatioF();
		obs_display_resize(display_, uint32_t(width() * dpr), uint32_t(height() * dpr));
	}
	clampPan();
}

void PreviewWidget::createDisplay()
{
	if (display_ || !isVisible())
		return;
	setDpr();
	const qreal dpr = devicePixelRatioF();
	struct gs_init_data info = {};
	info.cx = uint32_t(std::max(int(width() * dpr), 1));
	info.cy = uint32_t(std::max(int(height() * dpr), 1));
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
#ifdef _WIN32
	info.window.hwnd = reinterpret_cast<void *>(winId());
#elif defined(__APPLE__)
	// The widget's NSView, as OBS's own previews do (frontend qt-wrappers.cpp).
	info.window.view = (id)winId();
#else
	return; // Linux needs its display connection too; the plugin ships for Windows and macOS.
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

PreviewWidget::View PreviewWidget::widgetView() const
{
	const qreal dpr = devicePixelRatioF();
	return view(uint32_t(std::max(width() * dpr, 1.0)), uint32_t(std::max(height() * dpr, 1.0)));
}

QPointF PreviewWidget::toCanvas(const QPointF &p) const
{
	const qreal dpr = devicePixelRatioF();
	const View v = widgetView();
	return QPointF((p.x() * dpr - v.ox) / v.scale, (p.y() * dpr - v.oy) / v.scale);
}

// Panning can't lose the frame: some of it always stays in view.
void PreviewWidget::clampPan()
{
	const qreal dpr = devicePixelRatioF();
	const float cx = float(std::max(width() * dpr, 1.0)), cy = float(std::max(height() * dpr, 1.0));
	const float W = float(canvasW_.load()), H = float(canvasH_.load());
	const float s = std::min(cx / W, cy / H) * zoom_.load();
	const float keep = 48.0f * float(dpr);
	const float ox0 = (cx - W * s) / 2.0f, oy0 = (cy - H * s) / 2.0f;
	auto clamp = [](float v, float lo, float hi) { return lo > hi ? (lo + hi) / 2 : std::clamp(v, lo, hi); };
	panX_.store(clamp(panX_.load(), keep - W * s - ox0, cx - keep - ox0));
	panY_.store(clamp(panY_.load(), keep - H * s - oy0, cy - keep - oy0));
}

void PreviewWidget::zoomChanged()
{
	clampPan();
	if (onZoom)
		onZoom(int(std::lround(zoom_.load() * 100.0f)));
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
		const float dpr = self->dpr_.load();
		while (gs_effect_loop(solid, "Solid")) {
			// The frame's edge.
			setColor(solid, 0x50ffffff);
			drawOutline(-px, -px, W + 2 * px, H + 2 * px, px);
			// Where TikTok / Shorts / Reels put their own buttons and captions.
			const int area = self->safeArea_.load();
			if (area > 0) {
				setColor(solid, 0x30ff4060);
				for (const Zone &z : SAFE_AREAS[area - 1].zones)
					drawRect(W * z.x, H * z.y, W * z.w, H * z.h);
			}
			obs_scene_t *scene = sceneSrc ? obs_scene_from_source(sceneSrc) : nullptr;
			// What a click would grab.
			const qint64 hoverId = self->hover_.load();
			if (hoverId && hoverId != self->selected_.load()) {
				obs_sceneitem_t *group = nullptr;
				if (obs_sceneitem_t *item = itemRef(scene, hoverId, &group)) {
					setColor(solid, 0x90ffffff);
					outlineItem(item, group, px * dpr);
					obs_sceneitem_release(item);
				}
				obs_sceneitem_release(group);
			}
			// The selected item, with its handles (none when locked or in a group).
			obs_sceneitem_t *group = nullptr;
			obs_sceneitem_t *item = itemRef(scene, self->selected_.load(), &group);
			if (item) {
				const bool fixed = group || obs_sceneitem_locked(item);
				setColor(solid, fixed ? 0x907c6cff : 0xff7c6cff);
				outlineItem(item, group, 2 * px * dpr);
				if (!fixed) {
					QPointF pts[8];
					handlePoints(item, pts);
					const float h = HANDLE_PX * dpr * px;
					for (const QPointF &p : pts)
						drawRect(float(p.x()) - h / 2, float(p.y()) - h / 2, h, h);
				}
				obs_sceneitem_release(item);
			}
			obs_sceneitem_release(group);
			// Where a drag lines up with the frame or another source.
			const float gx = self->guideX_.load(), gy = self->guideY_.load();
			if (!std::isnan(gx) || !std::isnan(gy)) {
				setColor(solid, 0xffff5fa8);
				if (!std::isnan(gx))
					drawRect(gx - px * dpr / 2, 0, px * dpr, H);
				if (!std::isnan(gy))
					drawRect(0, gy - px * dpr / 2, W, px * dpr);
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
	// The top unlocked item under the mouse; a locked one only if there's nothing
	// else there, so a locked full-frame overlay doesn't catch every click.
	struct Hit {
		QPointF p;
		obs_sceneitem_t *item;
		obs_sceneitem_t *locked;
	} hit{p, nullptr, nullptr};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *h = static_cast<Hit *>(param);
			if (hits(item, h->p)) { // later = higher: the top one wins
				if (obs_sceneitem_locked(item))
					h->locked = item;
				else
					h->item = item;
			}
			return true;
		},
		&hit);
	return hit.item ? hit.item : hit.locked;
}

int PreviewWidget::handleAt(obs_scene_t *scene, const QPointF &p) const
{
	obs_sceneitem_t *item = selected_.load() ? VerticalCanvas::itemById(scene, selected_.load()) : nullptr;
	if (!item || obs_sceneitem_locked(item) || VerticalCanvas::isInGroup(scene, item) || !isVideo(item))
		return -1;
	const float reach = HANDLE_PX * dpr_.load() / widgetView().scale;
	QPointF pts[8];
	handlePoints(item, pts);
	for (int i = 0; i < 8; i++) // corners first: they win where handles overlap
		if (std::fabs(p.x() - pts[i].x()) <= reach && std::fabs(p.y() - pts[i].y()) <= reach)
			return i;
	return -1;
}

void PreviewWidget::collectSnapTargets(obs_scene_t *scene, obs_sceneitem_t *except)
{
	const float W = float(canvas_->width()), H = float(canvas_->height());
	snapX_ = {0.0f, W / 2, W};
	snapY_ = {0.0f, H / 2, H};
	struct Collect {
		PreviewWidget *self;
		obs_sceneitem_t *except;
	} c{this, except};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *c = static_cast<Collect *>(param);
			if (item == c->except || !isVideo(item))
				return true;
			const Rect r = shownOf(item);
			if (r.w <= 0 || r.h <= 0)
				return true;
			c->self->snapX_.insert(c->self->snapX_.end(), {r.x, r.x + r.w / 2, r.x + r.w});
			c->self->snapY_.insert(c->self->snapY_.end(), {r.y, r.y + r.h / 2, r.y + r.h});
			return true;
		},
		&c);
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
	flushNudge();
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
	pressScene_ = canvas_->currentScene();
	handle_ = handleAt(s, pressCanvas_);
	obs_sceneitem_t *item = nullptr;
	if (handle_ >= 0) {
		item = VerticalCanvas::itemById(s, selected_.load());
		drag_ = Drag::Resize;
	} else {
		// A click inside the selected item keeps it, as in OBS, so a source under a
		// full-frame overlay can still be dragged once picked in the list.
		obs_sceneitem_t *sel = VerticalCanvas::itemById(s, selected_.load());
		if (sel && VerticalCanvas::isInGroup(s, sel))
			sel = nullptr;
		item = sel && hits(sel, pressCanvas_) ? sel : itemAt(s, pressCanvas_);
		selected_.store(item ? obs_sceneitem_get_id(item) : 0);
		if (onSelect)
			onSelect(selected_.load());
		// A locked one is selected (its menu works) but stays put.
		drag_ = item && !obs_sceneitem_locked(item) ? Drag::Move : Drag::None;
	}
	if (item && drag_ != Drag::None) {
		beginUndo();
		const bool upright = obs_sceneitem_get_rot(item) == 0.0f;
		if (drag_ == Drag::Resize && upright) {
			if (e->modifiers() & Qt::AltModifier)
				bakeTrim(item); // a trimmed "Fill" source crops too
			// Handles sit on the picture, so a "fit" item becomes plain scaled first.
			bakeBounds(item);
			if ((e->modifiers() & Qt::AltModifier) && obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE)
				drag_ = Drag::Crop;
		}
		const Rect b = drag_ == Drag::Move ? shownOf(item) : upright ? visibleOf(item) : boxOf(item);
		boxX_ = b.x;
		boxY_ = b.y;
		boxW_ = b.w;
		boxH_ = b.h;
		obs_sceneitem_get_info2(item, &startInfo_);
		obs_sceneitem_get_crop(item, &startCrop_);
		obs_sceneitem_get_box_transform(item, &startBox_);
		matrix4_inv(&startBoxInv_, &startBox_);
		obs_source_t *src = obs_sceneitem_get_source(item);
		srcW_ = obs_source_get_width(src);
		srcH_ = obs_source_get_height(src);
		collectSnapTargets(s, item);
		if (drag_ == Drag::Resize) {
			// The grabbed handle in the box's unit square (a flipped item's top-left
			// handle is its picture's top-right).
			QPointF pts[8];
			handlePoints(item, pts);
			struct vec3 u;
			vec3_set(&u, float(pts[handle_].x()), float(pts[handle_].y()), 0.0f);
			vec3_transform(&u, &u, &startBoxInv_);
			handleU_ = std::round(u.x * 2.0f) / 2.0f;
			handleV_ = std::round(u.y * 2.0f) / 2.0f;
		}
	}
	obs_scene_release(s);
}

QString PreviewWidget::tipAt(const QPointF &p, obs_sceneitem_t *item) const
{
	QStringList lines;
	if (item) {
		QString name = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)));
		if (obs_sceneitem_locked(item))
			name = mt("Vertical.NameLocked").arg(name);
		lines << name;
	}
	const int area = safeArea_.load();
	const float W = float(canvas_->width()), H = float(canvas_->height());
	if (area > 0)
		for (const Zone &z : SAFE_AREAS[area - 1].zones)
			if (p.x() >= W * z.x && p.x() <= W * (z.x + z.w) && p.y() >= H * z.y && p.y() <= H * (z.y + z.h)) {
				lines << mt("Vertical.CoveredBy").arg(mt(z.what));
				break;
			}
	return lines.isEmpty() ? hint : lines.join("\n");
}

void PreviewWidget::mouseMoveEvent(QMouseEvent *e)
{
	if (projector_.load())
		return;
	if (drag_ == Drag::Pan) {
		const qreal dpr = devicePixelRatioF();
		panX_.store(startPanX_ + float((e->position().x() - pressWidget_.x()) * dpr));
		panY_.store(startPanY_ + float((e->position().y() - pressWidget_.y()) * dpr));
		clampPan();
		return;
	}
	if (drag_ == Drag::None) {
		const QPointF p = toCanvas(e->position());
		obs_scene_t *s = sceneRef();
		const int h = s ? handleAt(s, p) : -1;
		obs_sceneitem_t *under = s ? itemAt(s, p) : nullptr;
		hover_.store(under && h < 0 ? obs_sceneitem_get_id(under) : 0);
		obs_sceneitem_t *sel = h >= 0 ? VerticalCanvas::itemById(s, selected_.load()) : nullptr;
		setCursor(space_ ? Qt::OpenHandCursor : sel ? handleCursor(sel, h) : Qt::ArrowCursor);
		const QString tip = tipAt(p, under);
		if (tip != toolTip())
			setToolTip(tip);
		obs_scene_release(s);
		return;
	}
	if (canvas_->currentScene() != pressScene_) {
		// The scene on air changed mid-drag: its items aren't the ones grabbed.
		drag_ = Drag::None;
		undoBefore_.clear();
		guideX_.store(NO_GUIDE);
		guideY_.store(NO_GUIDE);
		return;
	}
	obs_scene_t *s = sceneRef();
	obs_sceneitem_t *item = s && selected_.load() ? VerticalCanvas::itemById(s, selected_.load()) : nullptr;
	if (!item) {
		obs_scene_release(s);
		return;
	}
	QPointF p = toCanvas(e->position());
	const float snap = SNAP_PX * dpr_.load() / widgetView().scale;
	const bool loose = (e->modifiers() & Qt::ShiftModifier) != 0; // no snap, no kept shape
	struct obs_transform_info t = startInfo_;
	float gx = NO_GUIDE, gy = NO_GUIDE;

	if (drag_ == Drag::Move) {
		float dx = float(p.x() - pressCanvas_.x()), dy = float(p.y() - pressCanvas_.y());
		if (!loose) {
			// The box's edges and centre to the frame's and the other sources'.
			const float ex[3] = {boxX_, boxX_ + boxW_ / 2, boxX_ + boxW_};
			const float ey[3] = {boxY_, boxY_ + boxH_ / 2, boxY_ + boxH_};
			gx = snapAxis(ex, 3, snapX_, snap, dx);
			gy = snapAxis(ey, 3, snapY_, snap, dy);
		}
		t.pos.x = startInfo_.pos.x + dx;
		t.pos.y = startInfo_.pos.y + dy;
		obs_sceneitem_set_info2(item, &t);
	} else if (drag_ == Drag::Resize) {
		const bool doX = handleU_ != 0.5f, doY = handleV_ != 0.5f;
		if (!loose && obs_sceneitem_get_rot(item) == 0.0f) {
			// The edge being dragged lines up with the frame and the other sources.
			const float px[1] = {float(p.x())}, py[1] = {float(p.y())};
			float dx = 0, dy = 0;
			if (doX)
				gx = snapAxis(px, 1, snapX_, snap, dx);
			if (doY)
				gy = snapAxis(py, 1, snapY_, snap, dy);
			p += QPointF(dx, dy);
		}
		// In the box's own unit square, so rotated items resize along their sides.
		struct vec3 u;
		vec3_set(&u, float(p.x()), float(p.y()), 0.0f);
		vec3_transform(&u, &u, &startBoxInv_);
		const float ax = doX ? 1.0f - handleU_ : 0.5f, ay = doY ? 1.0f - handleV_ : 0.5f;
		const float wpx = std::max(std::hypot(startBox_.x.x, startBox_.x.y), 0.0001f);
		const float hpx = std::max(std::hypot(startBox_.y.x, startBox_.y.y), 0.0001f);
		// The pointer can't pass the opposite side: the item never turns inside out.
		float fx = doX ? std::max((u.x - ax) * (handleU_ - ax), 4.0f / wpx) : 1.0f;
		float fy = doY ? std::max((u.y - ay) * (handleV_ - ay), 4.0f / hpx) : 1.0f;
		if (!loose) {
			const float k = doX && doY ? std::max(fx, fy) : doX ? fx : fy;
			fx = fy = k;
			if (doX && doY) {
				gx = gy = NO_GUIDE; // the shape decides one of them
			} else if (doX) {
				gy = NO_GUIDE;
			} else {
				gx = NO_GUIDE;
			}
		}
		if (t.bounds_type != OBS_BOUNDS_NONE) {
			t.bounds.x = startInfo_.bounds.x * fx;
			t.bounds.y = startInfo_.bounds.y * fy;
		} else {
			t.scale.x = startInfo_.scale.x * fx; // keeps a flip's sign
			t.scale.y = startInfo_.scale.y * fy;
		}
		obs_sceneitem_set_info2(item, &t);
		// The opposite corner or side stays where it was.
		struct vec3 before, now;
		struct matrix4 m;
		vec3_set(&before, ax, ay, 0.0f);
		vec3_transform(&before, &before, &startBox_);
		obs_sceneitem_get_box_transform(item, &m);
		vec3_set(&now, ax, ay, 0.0f);
		vec3_transform(&now, &now, &m);
		t.pos.x += before.x - now.x;
		t.pos.y += before.y - now.y;
		obs_sceneitem_set_info2(item, &t);
	} else if (drag_ == Drag::Crop && srcW_ && srcH_) {
		const float sx = std::max(std::fabs(startInfo_.scale.x), 0.0001f);
		const float sy = std::max(std::fabs(startInfo_.scale.y), 0.0001f);
		const float dx = float(p.x() - pressCanvas_.x()), dy = float(p.y() - pressCanvas_.y());
		// Which screen edges the handle moves: -1 left/top, 1 right/bottom, 0 neither.
		const int hx = HANDLE_U[handle_] == 0.5f ? 0 : HANDLE_U[handle_] > 0.5f ? 1 : -1;
		const int hy = HANDLE_V[handle_] == 0.5f ? 0 : HANDLE_V[handle_] > 0.5f ? 1 : -1;
		// The screen edge being dragged, as a crop side: a flipped item's screen
		// left is its picture's right.
		const bool flipX = startInfo_.scale.x < 0, flipY = startInfo_.scale.y < 0;
		struct obs_sceneitem_crop c = startCrop_;
		float left = boxX_, top = boxY_;
		if (hx) {
			const bool right = hx > 0;
			int &cx = (right != flipX) ? c.right : c.left;
			const int other = (right != flipX) ? c.left : c.right;
			const int start = cx;
			const float grow = right ? -dx : dx; // more crop
			cx = std::clamp(int(std::lround(start + grow / sx)), 0, std::max(0, int(srcW_) - 4 - other));
			if (!right)
				left = boxX_ + float(cx - start) * sx; // the edges not being cropped stay put
		}
		if (hy) {
			const bool bottom = hy > 0;
			int &cy = (bottom != flipY) ? c.bottom : c.top;
			const int other = (bottom != flipY) ? c.top : c.bottom;
			const int start = cy;
			const float grow = bottom ? -dy : dy;
			cy = std::clamp(int(std::lround(start + grow / sy)), 0, std::max(0, int(srcH_) - 4 - other));
			if (!bottom)
				top = boxY_ + float(cy - start) * sy;
		}
		obs_sceneitem_set_crop(item, &c);
		setAt(item, t, left, top);
	}
	guideX_.store(gx);
	guideY_.store(gy);
	obs_scene_release(s);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent *)
{
	if (drag_ == Drag::Move)
		endUndo(obs_module_text("Vertical.UndoMove"));
	else if (drag_ == Drag::Resize)
		endUndo(obs_module_text("Vertical.UndoResize"));
	else if (drag_ == Drag::Crop)
		endUndo(obs_module_text("Vertical.UndoCrop"));
	drag_ = Drag::None;
	handle_ = -1;
	guideX_.store(NO_GUIDE);
	guideY_.store(NO_GUIDE);
}

void PreviewWidget::leaveEvent(QEvent *e)
{
	hover_.store(0);
	QWidget::leaveEvent(e);
}

void PreviewWidget::wheelEvent(QWheelEvent *e)
{
	if (!(e->modifiers() & Qt::ControlModifier) || projector_.load()) {
		QWidget::wheelEvent(e);
		return;
	}
	// Zoom around the mouse: the canvas point under it stays under it.
	const qreal dpr = devicePixelRatioF();
	const QPointF before = toCanvas(e->position());
	const float z = std::clamp(zoom_.load() * std::pow(1.15f, float(e->angleDelta().y()) / 120.0f), 0.5f, 8.0f);
	zoom_.store(z);
	const View v = widgetView();
	const float mx = float(e->position().x() * dpr), my = float(e->position().y() * dpr);
	panX_.store(panX_.load() + (mx - (v.ox + float(before.x()) * v.scale)));
	panY_.store(panY_.load() + (my - (v.oy + float(before.y()) * v.scale)));
	zoomChanged();
	e->accept();
}

void PreviewWidget::nudge(float dx, float dy)
{
	obs_scene_t *s = sceneRef();
	obs_sceneitem_t *item = s && selected_.load() ? VerticalCanvas::itemById(s, selected_.load()) : nullptr;
	if (item && !obs_sceneitem_locked(item) && !VerticalCanvas::isInGroup(s, item)) {
		if (!nudging_) {
			beginUndo();
			nudging_ = true;
		}
		struct vec2 pos;
		obs_sceneitem_get_pos(item, &pos);
		pos.x += dx;
		pos.y += dy;
		obs_sceneitem_set_pos(item, &pos);
		nudgeTimer_->start();
	}
	obs_scene_release(s);
}

void PreviewWidget::flushNudge()
{
	nudgeTimer_->stop();
	if (!nudging_)
		return;
	nudging_ = false;
	endUndo(obs_module_text("Vertical.UndoNudge"));
}

void PreviewWidget::askRemove(qint64 id)
{
	flushNudge();
	if (vitem::askRemove(this, canvas_, id) && selected_.load() == id) {
		selected_.store(0);
		if (onSelect)
			onSelect(0);
	}
}

void PreviewWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
	if (projector_.load() || e->button() != Qt::LeftButton)
		return;
	// Double-click a source for its Properties, as in OBS.
	obs_scene_t *s = sceneRef();
	obs_sceneitem_t *item = s ? VerticalCanvas::itemById(s, selected_.load()) : nullptr;
	if (item && !VerticalCanvas::isInGroup(s, item) && hits(item, toCanvas(e->position())))
		obs_frontend_open_source_properties(obs_sceneitem_get_source(item));
	obs_scene_release(s);
}

void PreviewWidget::keyPressEvent(QKeyEvent *e)
{
	if (projector_.load()) {
		if (e->key() == Qt::Key_Escape && isFullScreen())
			showNormal();
		return;
	}
	const qint64 id = selected_.load();
	const QString scene = canvas_->currentScene();
	QString error;
	if (e->matches(QKeySequence::Undo) || e->matches(QKeySequence::Redo)) {
		// Only reaches here when the dock floats outside OBS's window.
		flushNudge();
		vitem::undo(e->matches(QKeySequence::Redo));
		return;
	}
	if (e->matches(QKeySequence::Copy)) {
		if (id && !canvas_->copySource(scene, id, error))
			QMessageBox::warning(this, TITLE, error);
		return;
	}
	if (e->matches(QKeySequence::Paste)) {
		flushNudge();
		if (!canvas_->canPasteSource())
			return;
		if (!canvas_->pasteSource(scene, false, error))
			QMessageBox::warning(this, TITLE, error);
		return;
	}
	if ((e->modifiers() & Qt::ControlModifier) && e->key() == Qt::Key_E) {
		flushNudge();
		if (id)
			vitem::editTransform(this, canvas_, id);
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
			zoomChanged();
			return;
		}
		break;
#ifdef __APPLE__
	case Qt::Key_Backspace: // a Mac keyboard's Delete key
#endif
	case Qt::Key_Delete:
		if (!id)
			break;
		askRemove(id);
		return;
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

void PreviewWidget::dragEnterEvent(QDragEnterEvent *e)
{
	if (projector_.load() || !e->mimeData()->hasUrls())
		return;
	for (const QUrl &url : e->mimeData()->urls())
		if (url.isLocalFile()) {
			e->acceptProposedAction();
			return;
		}
}

void PreviewWidget::dropEvent(QDropEvent *e)
{
	QStringList paths;
	for (const QUrl &url : e->mimeData()->urls())
		if (url.isLocalFile())
			paths << url.toLocalFile();
	if (paths.isEmpty())
		return;
	e->acceptProposedAction();
	flushNudge();
	QString error;
	canvas_->addFiles(canvas_->currentScene(), paths, error);
	if (!error.isEmpty())
		QMessageBox::warning(this, TITLE, error);
}

void PreviewWidget::contextMenuEvent(QContextMenuEvent *e)
{
	if (projector_.load()) {
		QMenu menu(this);
		QMenu *full = menu.addMenu(mt("Vertical.FullscreenOn"));
		const QList<QScreen *> screens = QGuiApplication::screens();
		for (int i = 0; i < screens.size(); i++) {
			QScreen *sc = screens[i];
			full->addAction(mt("Vertical.ScreenN").arg(i + 1).arg(sc->size().width()).arg(sc->size().height()),
					[this, sc]() {
						showNormal();
						setGeometry(sc->geometry());
						showFullScreen();
					});
		}
		if (isFullScreen())
			menu.addAction(mt("Vertical.LeaveFullscreen"), [this]() { showNormal(); });
		menu.addAction(mt("Vertical.Close"), [this]() { close(); });
		menu.exec(e->globalPos());
		return;
	}
	flushNudge();
	obs_scene_t *s = sceneRef();
	obs_sceneitem_t *item = s ? itemAt(s, toCanvas(e->pos())) : nullptr;
	// A right-click inside the selected item keeps it, like a left click.
	if (obs_sceneitem_t *sel = s ? VerticalCanvas::itemById(s, selected_.load()) : nullptr)
		if (!VerticalCanvas::isInGroup(s, sel) && hits(sel, toCanvas(e->pos())))
			item = sel;
	const qint64 id = item ? obs_sceneitem_get_id(item) : 0;
	const bool haveScene = s != nullptr;
	obs_scene_release(s);
	if (id) {
		selected_.store(id);
		if (onSelect)
			onSelect(id);
	}
	QMenu menu(this);
	if (id) {
		vitem::addItemMenu(menu, canvas_, id, this, [this, id]() {
			if (selected_.load() == id) {
				selected_.store(0);
				if (onSelect)
					onSelect(0);
			}
		});
		menu.addSeparator();
	} else if (haveScene) {
		if (onAddSource) {
			QPoint at = e->globalPos();
			menu.addAction(mt("Vertical.AddSourceMenu"), [this, at]() { onAddSource(this, at); });
		}
		vitem::addPasteActions(menu, canvas_, this);
		menu.addSeparator();
	}
	QMenu *safe = menu.addMenu(mt("Vertical.SafeAreasMenu"));
	const QString names[4] = {mt("Vertical.Off"), QString::fromUtf8(SAFE_AREAS[0].name),
				  QString::fromUtf8(SAFE_AREAS[1].name), QString::fromUtf8(SAFE_AREAS[2].name)}; // the apps are brand names
	for (int i = 0; i < 4; i++) {
		QAction *a = safe->addAction(names[i], [this, i]() { setSafeArea(i); });
		a->setCheckable(true);
		a->setChecked(safeArea() == i);
	}
	menu.addAction(mt("Vertical.ZoomFit"), [this]() {
		zoom_.store(1.0f);
		panX_.store(0);
		panY_.store(0);
		zoomChanged();
	});
	VerticalCanvas *c = canvas_;
	menu.addAction(mt("Vertical.OpenProjector"), [c]() { openProjector(c); });
	if (onTurnOff) {
		menu.addSeparator();
		menu.addAction(mt("Vertical.TurnOffPreview"), [this]() { onTurnOff(); });
	}
	menu.exec(e->globalPos());
}
