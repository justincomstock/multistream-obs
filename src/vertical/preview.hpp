#pragma once

#include <obs.h>

#include <QWidget>

#include <atomic>
#include <functional>
#include <mutex>

class VerticalCanvas;

// The Vertical Preview: an OBS display drawing the vertical canvas's
// finished frame, letterboxed, where items can be edited like in OBS's own
// preview:
//   click        select the top item under the mouse (locked ones skipped)
//   drag         move it (snaps to the frame's edges and centre; Shift: no snap)
//   corner drag  resize, keeping its shape (Shift: free); Alt: crop instead
//   arrow keys   nudge 1 px (Shift: 10 px); Delete removes the item
//   Ctrl+wheel   zoom around the mouse; middle-drag or Space+drag pans;
//                Ctrl+0 back to fit
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
	void setSafeAreas(bool on) { safeAreas_.store(on); }
	bool safeAreas() const { return safeAreas_.load(); }
	// Called when a click in the preview selects an item.
	std::function<void(qint64)> onSelect;
	// A projector window of the vertical picture: no editing or guides,
	// fullscreen on any screen (right-click), Esc leaves fullscreen.
	static void openProjector(VerticalCanvas *canvas);

protected:
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	bool event(QEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void keyReleaseEvent(QKeyEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	struct View {
		float scale = 1.0f; // display pixels per canvas pixel
		float ox = 0.0f;     // display pixel of canvas x = 0
		float oy = 0.0f;
	};
	View view(uint32_t cx, uint32_t cy) const;
	QPointF toCanvas(const QPointF &widgetPos) const;
	obs_scene_t *sceneRef() const; // the scene on air, new reference
	obs_sceneitem_t *itemAt(obs_scene_t *scene, const QPointF &canvasPos) const;
	int cornerAt(obs_scene_t *scene, const QPointF &canvasPos) const; // -1: none, 0..3: TL TR BR BL
	void beginUndo();
	void endUndo(const char *what);
	void nudge(float dx, float dy);

	void createDisplay();
	void destroyDisplay();
	static void draw(void *param, uint32_t cx, uint32_t cy);

	VerticalCanvas *canvas_;
	obs_display_t *display_ = nullptr;
	std::mutex m_;
	obs_weak_canvas_t *weakCanvas_ = nullptr;
	obs_weak_source_t *weakScene_ = nullptr;
	std::atomic<qint64> selected_{0};
	std::atomic<bool> safeAreas_{false};
	std::atomic<bool> projector_{false};
	std::atomic<int> lastState_{0};
	// zoom/pan, shared with the graphics thread
	std::atomic<float> zoom_{1.0f};
	std::atomic<float> panX_{0.0f};
	std::atomic<float> panY_{0.0f};
	std::atomic<uint32_t> canvasW_{1080};
	std::atomic<uint32_t> canvasH_{1920};

	enum class Drag { None, Move, Resize, Crop, Pan };
	Drag drag_ = Drag::None;
	int corner_ = -1;
	QPointF pressCanvas_;
	QPointF pressWidget_;
	float startPanX_ = 0, startPanY_ = 0;
	QString undoBefore_;
	bool space_ = false;
	// at press: the item's box and transform
	float boxX_ = 0, boxY_ = 0, boxW_ = 0, boxH_ = 0;
	struct obs_transform_info startInfo_ = {};
	struct obs_sceneitem_crop startCrop_ = {};
	uint32_t srcW_ = 0, srcH_ = 0;
};
