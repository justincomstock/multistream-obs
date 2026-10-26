#include "item-menu.hpp"

#include "canvas.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>

#include <cmath>
#include <memory>
#include <string>

namespace vitem {

namespace {

const char *const TITLE = "Multistream Vertical";

// The item's source in the scene on air (new reference, or null).
obs_source_t *sourceOf(VerticalCanvas *canvas, qint64 id)
{
	obs_scene_t *s = canvas->scene(canvas->currentScene());
	obs_sceneitem_t *item = s ? VerticalCanvas::itemById(s, id) : nullptr;
	obs_source_t *src = item ? obs_source_get_ref(obs_sceneitem_get_source(item)) : nullptr;
	obs_scene_release(s);
	return src;
}

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

} // namespace

void undo(bool redo)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QAction *a = main ? main->findChild<QAction *>(redo ? "actionMainRedo" : "actionMainUndo") : nullptr;
	if (!a)
		obs_log(LOG_WARNING, "vertical: OBS's Edit -> %s isn't where it used to be", redo ? "Redo" : "Undo");
	else if (a->isEnabled())
		a->trigger();
}

void renameSource(QWidget *parent, VerticalCanvas *canvas, obs_source_t *src)
{
	bool given = false;
	const QString old = QString::fromUtf8(obs_source_get_name(src));
	const QString to = QInputDialog::getText(parent, "Rename source", "Name (it's the same source on the main canvas):",
						 QLineEdit::Normal, old, &given)
				   .trimmed();
	if (!given || to.isEmpty() || to == old)
		return;
	QString error;
	if (!canvas->renameSource(src, to, error))
		QMessageBox::warning(parent, TITLE, error);
}

bool askRemove(QWidget *parent, VerticalCanvas *canvas, qint64 id)
{
	obs_source_t *src = sourceOf(canvas, id);
	if (!src)
		return false;
	const QString name = QString::fromUtf8(obs_source_get_name(src));
	obs_source_release(src);
	if (QMessageBox::question(parent, TITLE,
				  "Remove \"" + name + "\" from this vertical scene? It stays in OBS.\n\nEdit → Undo puts it back.") !=
	    QMessageBox::Yes)
		return false;
	QString error;
	if (!canvas->removeItem(canvas->currentScene(), id, error)) {
		QMessageBox::warning(parent, TITLE, error);
		return false;
	}
	return true;
}

void addPasteActions(QMenu &menu, VerticalCanvas *canvas, QWidget *parent)
{
	if (!canvas->canPasteSource())
		return;
	auto paste = [canvas, parent](bool duplicate) {
		return [canvas, parent, duplicate]() {
			QString error;
			if (!canvas->pasteSource(canvas->currentScene(), duplicate, error))
				QMessageBox::warning(parent, TITLE, error);
		};
	};
	menu.addAction("Paste (Ctrl+V)", paste(false))->setToolTip("The same source again: changing one changes both.");
	menu.addAction("Paste (duplicate)", paste(true))->setToolTip("A separate copy you can change on its own.");
}

void addItemMenu(QMenu &menu, VerticalCanvas *canvas, qint64 id, QWidget *parent, std::function<void()> removed)
{
	const QString scene = canvas->currentScene();
	obs_scene_t *s = canvas->scene(scene);
	obs_sceneitem_t *item = s ? VerticalCanvas::itemById(s, id) : nullptr;
	if (!item) {
		obs_scene_release(s);
		return;
	}
	obs_source_t *src = obs_sceneitem_get_source(item);
	const uint32_t flags = obs_source_get_output_flags(src);
	const bool visible = obs_sceneitem_visible(item), locked = obs_sceneitem_locked(item);
	// A group's sources are placed in the group's own pixels: the frame
	// arrangements don't apply to them.
	const bool grouped = VerticalCanvas::isInGroup(s, item);
	obs_scene_release(s);

	// Each action finds the source again: it may be gone by the time it's picked.
	auto withSource = [canvas, id](void (*fn)(obs_source_t *)) {
		return [canvas, id, fn]() {
			if (obs_source_t *src = sourceOf(canvas, id)) {
				fn(src);
				obs_source_release(src);
			}
		};
	};
	auto arrange = [canvas, scene, id, parent](const char *action) {
		return [canvas, scene, id, parent, action]() {
			QString error;
			if (!canvas->arrangeItem(scene, id, action, error))
				QMessageBox::warning(parent, TITLE, error);
		};
	};
	auto order = [canvas, scene, id, parent](int movement) {
		return [canvas, scene, id, parent, movement]() {
			QString error;
			if (!canvas->moveItem(scene, id, movement, error))
				QMessageBox::warning(parent, TITLE, error);
		};
	};

	menu.addAction("Properties…", withSource(obs_frontend_open_source_properties));
	menu.addAction("Filters…", withSource(obs_frontend_open_source_filters));
	menu.addAction("Rename…", [canvas, id, parent]() {
		if (obs_source_t *src = sourceOf(canvas, id)) {
			renameSource(parent, canvas, src);
			obs_source_release(src);
		}
	});
	if (flags & OBS_SOURCE_INTERACTION)
		menu.addAction("Interact…", withSource(obs_frontend_open_source_interaction));
	menu.addSeparator();

	if (!grouped) {
		menu.addAction("Copy (Ctrl+C)", [canvas, scene, id, parent]() {
			QString error;
			if (!canvas->copySource(scene, id, error))
				QMessageBox::warning(parent, TITLE, error);
		});
		menu.addAction("Duplicate", [canvas, scene, id, parent]() {
			QString error;
			if (!canvas->copySource(scene, id, error) || !canvas->pasteSource(scene, true, error))
				QMessageBox::warning(parent, TITLE, error);
		});
		addPasteActions(menu, canvas, parent);
		menu.addSeparator();
	}

	QMenu *o = menu.addMenu("Order");
	o->addAction("Move up", order(OBS_ORDER_MOVE_UP));
	o->addAction("Move down", order(OBS_ORDER_MOVE_DOWN));
	o->addAction("Move to top", order(OBS_ORDER_MOVE_TOP));
	o->addAction("Move to bottom", order(OBS_ORDER_MOVE_BOTTOM));

	if ((flags & OBS_SOURCE_VIDEO) && !grouped) {
		QMenu *t = menu.addMenu("Transform");
		t->addAction("Edit transform…", [canvas, id, parent]() { editTransform(parent, canvas, id); });
		t->addSeparator();
		t->addAction("Copy position and size", arrange("copy"));
		t->addAction("Paste position and size", arrange("paste"))->setEnabled(canvas->canPasteTransform());
		t->addAction("Reset size and position", arrange("reset"));
		t->addSeparator();
		t->addAction("Rotate 90° right", arrange("rotateRight"));
		t->addAction("Rotate 90° left", arrange("rotateLeft"));
		t->addAction("Rotate 180°", arrange("rotate180"));
		t->addSeparator();
		t->addAction("Flip horizontal", arrange("flipH"));
		t->addAction("Flip vertical", arrange("flipV"));
		t->addSeparator();
		t->addAction("Fit to frame", arrange("fit"));
		t->addAction("Fill frame", arrange("fill"));
		t->addAction("Fit to frame width", arrange("fitWidth"));
		t->addSeparator();
		t->addAction("Center", arrange("center"));
		t->addAction("Center horizontally", arrange("centerH"));
		t->addAction("Center vertically", arrange("centerV"));
	}
	menu.addSeparator();

	menu.addAction(visible ? "Hide" : "Show", [canvas, scene, id, parent, visible]() {
		QString error;
		if (!canvas->setItemVisible(scene, id, !visible, error))
			QMessageBox::warning(parent, TITLE, error);
	});
	menu.addAction(locked ? "Unlock" : "Lock", [canvas, scene, id, parent, locked]() {
		QString error;
		if (!canvas->setItemLocked(scene, id, !locked, error))
			QMessageBox::warning(parent, TITLE, error);
	});
	menu.addSeparator();
	menu.addAction("Remove from this scene", [canvas, id, parent, removed]() {
		if (askRemove(parent, canvas, id) && removed)
			removed();
	});
}

// ---- Edit transform -------------------------------------------------------------

namespace {

struct TransformDialog {
	VerticalCanvas *canvas = nullptr;
	QString scene;
	qint64 id = 0;
	std::string before;
	bool loading = false;
	QDoubleSpinBox *x, *y, *rot, *w, *h, *bw, *bh;
	QComboBox *align, *boundsType, *boundsAlign;
	QSpinBox *cl, *ct, *cr, *cb;
	QCheckBox *cropToBounds;
	QDialog *dlg;
};

// OBS's alignment flags, in the order OBS's dialog lists them.
const struct {
	const char *name;
	uint32_t value;
} ALIGNS[] = {{"Top left", OBS_ALIGN_TOP | OBS_ALIGN_LEFT},
	      {"Top", OBS_ALIGN_TOP},
	      {"Top right", OBS_ALIGN_TOP | OBS_ALIGN_RIGHT},
	      {"Left", OBS_ALIGN_LEFT},
	      {"Center", OBS_ALIGN_CENTER},
	      {"Right", OBS_ALIGN_RIGHT},
	      {"Bottom left", OBS_ALIGN_BOTTOM | OBS_ALIGN_LEFT},
	      {"Bottom", OBS_ALIGN_BOTTOM},
	      {"Bottom right", OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT}};

QComboBox *alignBox()
{
	auto *c = new QComboBox();
	for (const auto &a : ALIGNS)
		c->addItem(a.name, uint(a.value));
	return c;
}

QDoubleSpinBox *numberBox(double lo, double hi, const QString &suffix)
{
	auto *b = new QDoubleSpinBox();
	b->setRange(lo, hi);
	b->setDecimals(1);
	b->setSuffix(suffix);
	b->setAccelerated(true);
	return b;
}

QSpinBox *cropBox()
{
	auto *b = new QSpinBox();
	b->setRange(0, 100000);
	b->setSuffix(" px");
	return b;
}

// The item while the dialog is open (no reference), or null if it's gone.
obs_sceneitem_t *dialogItem(const TransformDialog &d, obs_scene_t **scene)
{
	*scene = d.canvas->scene(d.scene);
	return *scene ? obs_scene_find_sceneitem_by_id(*scene, d.id) : nullptr;
}

void loadDialog(TransformDialog &d)
{
	obs_scene_t *s = nullptr;
	obs_sceneitem_t *item = dialogItem(d, &s);
	if (!item) {
		obs_scene_release(s);
		d.dlg->close();
		return;
	}
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	struct obs_sceneitem_crop c;
	obs_sceneitem_get_crop(item, &c);
	obs_source_t *src = obs_sceneitem_get_source(item);
	const int sw = int(obs_source_get_width(src)) - c.left - c.right;
	const int sh = int(obs_source_get_height(src)) - c.top - c.bottom;
	obs_scene_release(s);
	d.loading = true;
	d.x->setValue(t.pos.x);
	d.y->setValue(t.pos.y);
	d.rot->setValue(t.rot);
	d.w->setValue(std::fabs(t.scale.x) * std::max(sw, 0));
	d.h->setValue(std::fabs(t.scale.y) * std::max(sh, 0));
	// Size is the picture's: a bounding box decides it instead, as in OBS.
	const bool sized = t.bounds_type == OBS_BOUNDS_NONE && sw > 0 && sh > 0;
	d.w->setEnabled(sized);
	d.h->setEnabled(sized);
	d.align->setCurrentIndex(std::max(0, d.align->findData(uint(t.alignment))));
	d.boundsType->setCurrentIndex(int(t.bounds_type));
	d.boundsAlign->setCurrentIndex(std::max(0, d.boundsAlign->findData(uint(t.bounds_alignment))));
	d.bw->setValue(t.bounds.x);
	d.bh->setValue(t.bounds.y);
	const bool bounded = t.bounds_type != OBS_BOUNDS_NONE;
	for (QWidget *b : {static_cast<QWidget *>(d.boundsAlign), static_cast<QWidget *>(d.bw), static_cast<QWidget *>(d.bh),
			   static_cast<QWidget *>(d.cropToBounds)})
		b->setEnabled(bounded);
	d.cropToBounds->setChecked(t.crop_to_bounds);
	d.cl->setValue(c.left);
	d.ct->setValue(c.top);
	d.cr->setValue(c.right);
	d.cb->setValue(c.bottom);
	d.loading = false;
}

void applyDialog(TransformDialog &d)
{
	if (d.loading)
		return;
	obs_scene_t *s = nullptr;
	obs_sceneitem_t *item = dialogItem(d, &s);
	if (!item) {
		obs_scene_release(s);
		d.dlg->close();
		return;
	}
	struct obs_transform_info t;
	obs_sceneitem_get_info2(item, &t);
	struct obs_sceneitem_crop c;
	c.left = d.cl->value();
	c.top = d.ct->value();
	c.right = d.cr->value();
	c.bottom = d.cb->value();
	obs_sceneitem_set_crop(item, &c);
	obs_source_t *src = obs_sceneitem_get_source(item);
	const int sw = int(obs_source_get_width(src)) - c.left - c.right;
	const int sh = int(obs_source_get_height(src)) - c.top - c.bottom;
	t.pos.x = float(d.x->value());
	t.pos.y = float(d.y->value());
	t.rot = float(d.rot->value());
	if (d.w->isEnabled() && sw > 0 && sh > 0) {
		// The size boxes set the scale; a flip keeps its sign.
		t.scale.x = std::copysign(float(d.w->value()) / float(sw), t.scale.x);
		t.scale.y = std::copysign(float(d.h->value()) / float(sh), t.scale.y);
	}
	t.alignment = d.align->currentData().toUInt();
	t.bounds_type = static_cast<enum obs_bounds_type>(d.boundsType->currentIndex());
	t.bounds_alignment = d.boundsAlign->currentData().toUInt();
	t.bounds.x = float(d.bw->value());
	t.bounds.y = float(d.bh->value());
	if (t.bounds_type != OBS_BOUNDS_NONE && (t.bounds.x < 1.0f || t.bounds.y < 1.0f)) {
		// Turning a bounding box on: start it at what the item shows now.
		t.bounds.x = std::max(1.0f, std::fabs(t.scale.x) * float(std::max(sw, 1)));
		t.bounds.y = std::max(1.0f, std::fabs(t.scale.y) * float(std::max(sh, 1)));
	}
	t.crop_to_bounds = d.cropToBounds->isChecked();
	obs_sceneitem_set_info2(item, &t);
	obs_scene_release(s);
	loadDialog(d); // what follows from the change (size enabled, bounds filled in)
}

} // namespace

void editTransform(QWidget *parent, VerticalCanvas *canvas, qint64 id)
{
	const QString scene = canvas->currentScene();
	obs_scene_t *s = canvas->scene(scene);
	obs_sceneitem_t *item = s ? obs_scene_find_sceneitem_by_id(s, id) : nullptr;
	if (!item) {
		obs_scene_release(s);
		return;
	}
	auto d = std::make_shared<TransformDialog>();
	d->canvas = canvas;
	d->scene = scene;
	d->id = id;
	d->before = statesOf(s);
	const QString name = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)));
	obs_scene_release(s);

	d->dlg = new QDialog(parent);
	d->dlg->setAttribute(Qt::WA_DeleteOnClose);
	d->dlg->setWindowTitle("Edit transform: " + name);
	auto *form = new QFormLayout(d->dlg);
	d->x = numberBox(-100000, 100000, " px");
	d->y = numberBox(-100000, 100000, " px");
	d->rot = numberBox(-360, 360, "°");
	d->w = numberBox(0, 100000, " px");
	d->h = numberBox(0, 100000, " px");
	d->align = alignBox();
	d->boundsType = new QComboBox();
	for (const char *b : {"None", "Stretch to bounds", "Scale to inner bounds (fit)", "Scale to outer bounds (fill)",
			      "Scale to width of bounds", "Scale to height of bounds", "Maximum size only"})
		d->boundsType->addItem(b);
	d->boundsAlign = alignBox();
	d->bw = numberBox(0, 100000, " px");
	d->bh = numberBox(0, 100000, " px");
	d->cropToBounds = new QCheckBox("Crop to bounding box");
	d->cl = cropBox();
	d->ct = cropBox();
	d->cr = cropBox();
	d->cb = cropBox();
	auto pair = [](QWidget *a, QWidget *b) {
		auto *row = new QWidget();
		auto *l = new QHBoxLayout(row);
		l->setContentsMargins(0, 0, 0, 0);
		l->addWidget(a);
		l->addWidget(b);
		return row;
	};
	form->addRow("Position (x, y)", pair(d->x, d->y));
	form->addRow("Rotation", d->rot);
	form->addRow("Size (width, height)", pair(d->w, d->h));
	form->addRow("Positional alignment", d->align);
	form->addRow("Bounding box type", d->boundsType);
	form->addRow("Alignment in bounding box", d->boundsAlign);
	form->addRow("Bounding box size", pair(d->bw, d->bh));
	form->addRow(QString(), d->cropToBounds);
	form->addRow("Crop left, right", pair(d->cl, d->cr));
	form->addRow("Crop top, bottom", pair(d->ct, d->cb));
	auto *note = new QLabel("Changes show right away. Edit → Undo takes back everything changed here at once.");
	note->setWordWrap(true);
	form->addRow(note);
	auto *buttons = new QDialogButtonBox();
	QPushButton *reset = buttons->addButton("Reset", QDialogButtonBox::ResetRole);
	reset->setToolTip("Its own size, centred in the frame, no crop or rotation");
	QPushButton *close = buttons->addButton("Close", QDialogButtonBox::AcceptRole);
	form->addRow(buttons);

	loadDialog(*d);
	auto apply = [d]() { applyDialog(*d); };
	for (QDoubleSpinBox *b : {d->x, d->y, d->rot, d->w, d->h, d->bw, d->bh})
		QObject::connect(b, &QDoubleSpinBox::valueChanged, d->dlg, apply);
	for (QSpinBox *b : {d->cl, d->ct, d->cr, d->cb})
		QObject::connect(b, &QSpinBox::valueChanged, d->dlg, apply);
	for (QComboBox *b : {d->align, d->boundsType, d->boundsAlign})
		QObject::connect(b, &QComboBox::currentIndexChanged, d->dlg, apply);
	QObject::connect(d->cropToBounds, &QCheckBox::toggled, d->dlg, apply);
	QObject::connect(reset, &QPushButton::clicked, d->dlg, [d]() {
		QString error;
		// Its own undo step comes in the dialog's single one below.
		d->canvas->arrangeItem(d->scene, d->id, "resetQuiet", error);
		loadDialog(*d);
	});
	QObject::connect(close, &QPushButton::clicked, d->dlg, &QDialog::close);
	QObject::connect(d->dlg, &QDialog::finished, d->dlg, [d]() {
		obs_scene_t *s = d->canvas->scene(d->scene);
		if (!s)
			return;
		const std::string after = statesOf(s);
		obs_scene_release(s);
		if (after != d->before)
			obs_frontend_add_undo_redo_action("Edit transform (vertical)", loadStates, loadStates,
							  d->before.c_str(), after.c_str(), false);
	});
	d->dlg->show();
}

} // namespace vitem
