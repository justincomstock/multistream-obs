#include "item-menu.hpp"

#include "canvas.hpp"
#include "../i18n.hpp"

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
	const QString to = QInputDialog::getText(parent, mt("Vertical.RenameSourceTitle"), mt("Vertical.RenameSourceLabel"),
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
	if (QMessageBox::question(parent, TITLE, mt("Vertical.AskRemove").arg(name)) != QMessageBox::Yes)
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
	menu.addAction(mt("Vertical.PasteLinked"), paste(false))->setToolTip(mt("Vertical.PasteLinkedTip"));
	menu.addAction(mt("Vertical.PasteCopy"), paste(true))->setToolTip(mt("Vertical.PasteCopyTip"));
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

	menu.addAction(mt("Vertical.Properties"), withSource(obs_frontend_open_source_properties));
	menu.addAction(mt("Vertical.Filters"), withSource(obs_frontend_open_source_filters));
	menu.addAction(mt("Vertical.Rename"), [canvas, id, parent]() {
		if (obs_source_t *src = sourceOf(canvas, id)) {
			renameSource(parent, canvas, src);
			obs_source_release(src);
		}
	});
	if (flags & OBS_SOURCE_INTERACTION)
		menu.addAction(mt("Vertical.Interact"), withSource(obs_frontend_open_source_interaction));
	menu.addSeparator();

	if (!grouped) {
		menu.addAction(mt("Vertical.CopyCtrl"), [canvas, scene, id, parent]() {
			QString error;
			if (!canvas->copySource(scene, id, error))
				QMessageBox::warning(parent, TITLE, error);
		});
		menu.addAction(mt("Vertical.DuplicateItem"), [canvas, scene, id, parent]() {
			QString error;
			if (!canvas->copySource(scene, id, error) || !canvas->pasteSource(scene, true, error))
				QMessageBox::warning(parent, TITLE, error);
		});
		addPasteActions(menu, canvas, parent);
		menu.addSeparator();
	}

	QMenu *o = menu.addMenu(mt("Vertical.Order"));
	o->addAction(mt("Vertical.MoveUp"), order(OBS_ORDER_MOVE_UP));
	o->addAction(mt("Vertical.MoveDown"), order(OBS_ORDER_MOVE_DOWN));
	o->addAction(mt("Vertical.MoveToTop"), order(OBS_ORDER_MOVE_TOP));
	o->addAction(mt("Vertical.MoveToBottom"), order(OBS_ORDER_MOVE_BOTTOM));

	if ((flags & OBS_SOURCE_VIDEO) && !grouped) {
		QMenu *t = menu.addMenu(mt("Vertical.Transform"));
		t->addAction(mt("Vertical.TrEdit"), [canvas, id, parent]() { editTransform(parent, canvas, id); });
		t->addSeparator();
		t->addAction(mt("Vertical.TrCopy"), arrange("copy"));
		t->addAction(mt("Vertical.TrPaste"), arrange("paste"))->setEnabled(canvas->canPasteTransform());
		t->addAction(mt("Vertical.TrReset"), arrange("reset"));
		t->addSeparator();
		t->addAction(mt("Vertical.RotateRight"), arrange("rotateRight"));
		t->addAction(mt("Vertical.RotateLeft"), arrange("rotateLeft"));
		t->addAction(mt("Vertical.TrRotate180"), arrange("rotate180"));
		t->addSeparator();
		t->addAction(mt("Vertical.TrFlipH"), arrange("flipH"));
		t->addAction(mt("Vertical.TrFlipV"), arrange("flipV"));
		t->addSeparator();
		t->addAction(mt("Vertical.TrFit"), arrange("fit"));
		t->addAction(mt("Vertical.TrFill"), arrange("fill"));
		t->addAction(mt("Vertical.TrFitWidth"), arrange("fitWidth"));
		t->addSeparator();
		t->addAction(mt("Vertical.TrCenter"), arrange("center"));
		t->addAction(mt("Vertical.TrCenterH"), arrange("centerH"));
		t->addAction(mt("Vertical.TrCenterV"), arrange("centerV"));
	}
	menu.addSeparator();

	menu.addAction(visible ? mt("Vertical.HideItem") : mt("Vertical.ColShow"), [canvas, scene, id, parent, visible]() {
		QString error;
		if (!canvas->setItemVisible(scene, id, !visible, error))
			QMessageBox::warning(parent, TITLE, error);
	});
	menu.addAction(locked ? mt("Vertical.UnlockItem") : mt("Vertical.ColLock"), [canvas, scene, id, parent, locked]() {
		QString error;
		if (!canvas->setItemLocked(scene, id, !locked, error))
			QMessageBox::warning(parent, TITLE, error);
	});
	menu.addSeparator();
	menu.addAction(mt("Vertical.RemoveFromScene"), [canvas, id, parent, removed]() {
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
	const char *key; // its locale key
	uint32_t value;
} ALIGNS[] = {{"Vertical.AlignTopLeft", OBS_ALIGN_TOP | OBS_ALIGN_LEFT},
	      {"Vertical.AlignTop", OBS_ALIGN_TOP},
	      {"Vertical.AlignTopRight", OBS_ALIGN_TOP | OBS_ALIGN_RIGHT},
	      {"Vertical.AlignLeft", OBS_ALIGN_LEFT},
	      {"Vertical.AlignCenter", OBS_ALIGN_CENTER},
	      {"Vertical.AlignRight", OBS_ALIGN_RIGHT},
	      {"Vertical.AlignBottomLeft", OBS_ALIGN_BOTTOM | OBS_ALIGN_LEFT},
	      {"Vertical.AlignBottom", OBS_ALIGN_BOTTOM},
	      {"Vertical.AlignBottomRight", OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT}};

QComboBox *alignBox()
{
	auto *c = new QComboBox();
	for (const auto &a : ALIGNS)
		c->addItem(mt(a.key), uint(a.value));
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
	d->dlg->setWindowTitle(mt("Vertical.EditTransformTitle").arg(name));
	auto *form = new QFormLayout(d->dlg);
	d->x = numberBox(-100000, 100000, " px");
	d->y = numberBox(-100000, 100000, " px");
	d->rot = numberBox(-360, 360, "°");
	d->w = numberBox(0, 100000, " px");
	d->h = numberBox(0, 100000, " px");
	d->align = alignBox();
	d->boundsType = new QComboBox();
	for (const char *b : {"Vertical.BoundsNone", "Vertical.BoundsStretch", "Vertical.BoundsInner", "Vertical.BoundsOuter",
			      "Vertical.BoundsWidth", "Vertical.BoundsHeight", "Vertical.BoundsMax"})
		d->boundsType->addItem(mt(b));
	d->boundsAlign = alignBox();
	d->bw = numberBox(0, 100000, " px");
	d->bh = numberBox(0, 100000, " px");
	d->cropToBounds = new QCheckBox(mt("Vertical.CropToBox"));
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
	form->addRow(mt("Vertical.FldPosition"), pair(d->x, d->y));
	form->addRow(mt("Vertical.FldRotation"), d->rot);
	form->addRow(mt("Vertical.FldSize"), pair(d->w, d->h));
	form->addRow(mt("Vertical.FldAlign"), d->align);
	form->addRow(mt("Vertical.FldBoundsType"), d->boundsType);
	form->addRow(mt("Vertical.FldBoundsAlign"), d->boundsAlign);
	form->addRow(mt("Vertical.FldBoundsSize"), pair(d->bw, d->bh));
	form->addRow(QString(), d->cropToBounds);
	form->addRow(mt("Vertical.FldCropLR"), pair(d->cl, d->cr));
	form->addRow(mt("Vertical.FldCropTB"), pair(d->ct, d->cb));
	auto *note = new QLabel(mt("Vertical.TransformNote"));
	note->setWordWrap(true);
	form->addRow(note);
	auto *buttons = new QDialogButtonBox();
	QPushButton *reset = buttons->addButton(mt("Vertical.Reset"), QDialogButtonBox::ResetRole);
	reset->setToolTip(mt("Vertical.ResetTip"));
	QPushButton *close = buttons->addButton(mt("Vertical.Close"), QDialogButtonBox::AcceptRole);
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
			obs_frontend_add_undo_redo_action(mt("Vertical.UndoEditTransform").toUtf8().constData(), loadStates, loadStates,
							  d->before.c_str(), after.c_str(), false);
	});
	d->dlg->show();
}

} // namespace vitem
