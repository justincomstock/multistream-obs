#pragma once

#include <obs.h>
#include <graphics/matrix4.h>

#include <QString>
#include <QWidget>

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

class QTimer;
class VerticalCanvas;

// The Vertical Preview: an OBS display drawing the vertical canvas's
// finished frame, letterboxed, where items can be edited like in OBS's own
// preview:
//   click        select the top item under the mouse (a locked one too: it
//                gets a dim outline and can't be moved)
//   hover        outlines what a click would grab; its name is the tooltip
//   drag         move it (snaps to the frame's and other sources' edges and
//                centres, with a guide line; Shift: no snap)
//   handles      8 of them, rotated items too: they keep the shape (Shift:
//                stretch freely); Alt: crop instead ("Fill" sources too)
//   arrow keys   nudge 1 px (Shift: 10 px), one undo step per burst
//   Delete       removes the item (Backspace too on a Mac)
//   Ctrl+C/V     copy and paste a source; Ctrl+E: Edit transform
//   double-click Properties; right-click: the item menu OBS has (item-menu.hpp)
//   Ctrl+wheel   zoom around the mouse; middle-drag or Space+drag pans
//                (the frame can't be lost off-screen); Ctrl+0 back to fit
//   drop files   pictures, videos, sounds and .html pages become sources
// Moves are undoable with OBS's Edit -> Undo. Drawing runs on OBS's graphics
// thread: it reaches the canvas and scene only through weak references.
class PreviewWidget : public QWidget {
public:
	explicit PreviewWidget(VerticalCanvas *canvas, QWidget *parent = nullptr);
	~PreviewWidget() override;

	QPaintEngine *paintEngine() const override { return nullptr; }

	// UI thread: the canvas changed (collection switch) or the scene on air.
	void sync();
	// The selected item (0: none) in the scene on air.
	qint64 selected() const { return selected_.load(); }
	void select(qint64 id);
	// Where TikTok, YouTube Shorts or Instagram Reels put their own buttons
	// and captions: 0 off, 1 TikTok, 2 Shorts, 3 Reels. Remembered.
	void setSafeArea(int which);
	int safeArea() const { return safeArea_.load(); }
	// The tooltip when the mouse isn't over a source.
	QString hint;
	// Called when a click in the preview selects an item.
	std::function<void(qint64)> onSelect;
	// Right-click on empty space: "Add a source…" opens this menu at a screen point.
	std::function<void(QWidget *, const QPoint &)> onAddSource;
	// The zoom changed: percent of "fit" (100 = fit).
	std::function<void(int)> onZoom;
	// "Turn off the preview" in the right-click menu.
	std::function<void()> onTurnOff;
	// A projector window of the vertical picture: no editing or guides,
	// fullscreen on any screen (right-click), Esc leaves fullscreen.
	static void openProjector(VerticalCanvas *canvas);
	// At OBS's exit: projectors have no parent, so OBS wouldn't close them.
	static void closeProjectors();

protected:
	void showEvent(QShowEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	bool event(QEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void keyReleaseEvent(QKeyEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dropEvent(QDropEvent *e) override;

private:
	struct View {
		float scale = 1.0f; // display pixels per canvas pixel
		float ox = 0.0f;     // display pixel of canvas x = 0
		float oy = 0.0f;
	};
	View view(uint32_t cx, uint32_t cy) const;
	View widgetView() const; // for the widget's current size
	QPointF toCanvas(const QPointF &widgetPos) const;
	obs_scene_t *sceneRef() const; // the scene on air, new reference
	obs_sceneitem_t *itemAt(obs_scene_t *scene, const QPointF &canvasPos) const;
	int handleAt(obs_scene_t *scene, const QPointF &canvasPos) const; // -1: none, 0..7 (see preview.cpp)
	void beginUndo();
	void endUndo(const char *what);
	void nudge(float dx, float dy);
	void flushNudge(); // the nudges so far become one undo step
	void askRemove(qint64 id);
	void clampPan();
	void zoomChanged();
	void setDpr();
	void collectSnapTargets(obs_scene_t *scene, obs_sceneitem_t *except);
	void showGuides(obs_sceneitem_t *item); // guide lines where it lines up
	QString tipAt(const QPointF &canvasPos, obs_sceneitem_t *item) const;

	void createDisplay();
	void destroyDisplay();
	static void draw(void *param, uint32_t cx, uint32_t cy);

	VerticalCanvas *canvas_;
	obs_display_t *display_ = nullptr;
	std::mutex m_;
	obs_weak_canvas_t *weakCanvas_ = nullptr;
	obs_weak_source_t *weakScene_ = nullptr;
	std::atomic<qint64> selected_{0};
	std::atomic<qint64> hover_{0};
	std::atomic<int> safeArea_{0};
	std::atomic<bool> projector_{false};
	std::atomic<int> lastState_{0};
	std::atomic<float> dpr_{1.0f};
	// Guide lines while dragging (canvas units; NaN: none).
	std::atomic<float> guideX_;
	std::atomic<float> guideY_;
	// zoom/pan, shared with the graphics thread
	std::atomic<float> zoom_{1.0f};
	std::atomic<float> panX_{0.0f};
	std::atomic<float> panY_{0.0f};
	std::atomic<uint32_t> canvasW_{1080};
	std::atomic<uint32_t> canvasH_{1920};

	enum class Drag { None, Move, Resize, Crop, Pan };
	Drag drag_ = Drag::None;
	int handle_ = -1;
	QPointF pressCanvas_;
	QPointF pressWidget_;
	float startPanX_ = 0, startPanY_ = 0;
	QString undoBefore_;
	bool space_ = false;
	QTimer *nudgeTimer_ = nullptr;
	bool nudging_ = false;
	QString shownScene_; // the scene sync() last saw on air (ids are per scene)
	QString pressScene_; // the scene a drag started in
	// at press: the item's box and transform
	float boxX_ = 0, boxY_ = 0, boxW_ = 0, boxH_ = 0;
	struct obs_transform_info startInfo_ = {};
	struct obs_sceneitem_crop startCrop_ = {};
	struct matrix4 startBox_ = {}; // its box transform (unit square -> canvas)
	struct matrix4 startBoxInv_ = {};
	float handleU_ = 0, handleV_ = 0; // the grabbed handle, in that unit square
	uint32_t srcW_ = 0, srcH_ = 0;
	// What a drag snaps to: the frame's and the other sources' edges and centres.
	std::vector<float> snapX_, snapY_;
};
