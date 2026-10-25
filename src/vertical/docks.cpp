#include "docks.hpp"

#include "canvas.hpp"
#include "preview.hpp"
#include "record.hpp"
#include "theme.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QDesktopServices>
#include <QDockWidget>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

VerticalCanvas *canvas = nullptr;

QPointer<QWidget> scenesDock;
QPointer<QWidget> sourcesDock;
QPointer<QWidget> previewDock;
QPointer<QWidget> mixerDock;
QPointer<QListWidget> list;
QPointer<vtheme::Switch> follow;
QPointer<QLabel> liveLabel;
QPointer<QLabel> sceneLabel;
QPointer<QLabel> sizeLabel;
std::function<QJsonObject()> liveState;
QPointer<QScrollArea> mixerScroll;
int stripHeight = 80; // the mixer's faders and meters: fitStrips() fits them to the dock
QPointer<QLabel> followSub;
QPointer<QLabel> lockShowHead;
int recordWords = 2;        // the preview dock's width: 2 full words, 1 short, 0 icons only
QPointer<QTreeWidget> items;
QPointer<QLabel> itemsTitle;
QPointer<QWidget> mixerRows;
QPointer<QToolButton> recButton;
QPointer<QToolButton> replayButton;
QPointer<QToolButton> saveButton;
QPointer<QLabel> recLabel;
QPointer<QWidget> importBox;
QPointer<QLabel> importText;
QPointer<QPushButton> importAitumButton;
QPointer<QPushButton> importLuaButton;
QTimer *importTimer = nullptr;
PreviewWidget *preview = nullptr;
QTimer *watcher = nullptr;
QTimer *meters = nullptr;
quint64 shownVersion = 0;
QString shownItems;
QString shownMixer;

void warn(const QString &error)
{
	QMessageBox::warning(scenesDock, "Multistream Vertical", error);
}

// pill: the button's look in theme.cpp (ghost, rec, soft, round, square, key).
QToolButton *toolButton(const QString &text, const QString &tip, const char *pill = "square")
{
	auto *b = new QToolButton();
	b->setText(text);
	b->setToolTip(tip);
	b->setAutoRaise(true);
	b->setProperty("pill", pill);
	b->setCursor(Qt::PointingHandCursor);
	return b;
}

// Calls fit() whenever the watched widget is resized: docks squeeze or drop
// what doesn't fit (no moc needed for an event filter).
class Fit : public QObject {
public:
	Fit(QWidget *watched, std::function<void()> fit) : QObject(watched), fit_(std::move(fit))
	{
		watched->installEventFilter(this);
	}
	bool eventFilter(QObject *, QEvent *e) override
	{
		if (e->type() == QEvent::Resize)
			fit_();
		return false;
	}

private:
	std::function<void()> fit_;
};

// An icon-only button: icon is a vtheme::lineIcon name.
QToolButton *iconButton(const char *icon, const QString &tip, const char *pill = "square")
{
	QToolButton *b = toolButton(QString(), tip, pill);
	b->setIcon(vtheme::lineIcon(QString::fromLatin1(icon)));
	b->setIconSize(QSize(14, 14));
	b->setAccessibleName(tip);
	return b;
}

// A dock's root: the Multistream dock's look for it and everything in it.
// Minimums stay small (contents scroll): five docks share OBS's right
// column, and their minimums together must leave room to drag the dividers
// in a 1030x870 window (scripts/test-obs-dock-sizes.cjs).
QWidget *dockRoot(const char *name, int minW, int minH)
{
	auto *w = new QWidget();
	w->setObjectName(name);
	w->setMinimumSize(minW, minH);
	w->setAttribute(Qt::WA_StyledBackground);
	w->setStyleSheet(vtheme::style());
	return w;
}

// The dock's contents, in a scroll area: when OBS makes the dock smaller
// than they need (it does, in a full column), they scroll instead of being
// squeezed on top of each other. Returns the widget to lay them out in.
QWidget *scrolled(QWidget *root)
{
	auto *outer = new QVBoxLayout(root);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *scroll = new QScrollArea();
	scroll->setObjectName("vScroll");
	scroll->viewport()->setObjectName("vScrollView");
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	auto *inner = new QWidget();
	inner->setObjectName("vInner");
	scroll->setWidget(inner);
	outer->addWidget(scroll);
	return inner;
}

// "+ Add scene", "+ Add", "+": as much as the dock's width allows.
void fitAdd(QToolButton *add, int width, const QString &full)
{
	add->setText(width >= 320 ? full : width >= 250 ? QString("+ Add") : QString("+"));
}

// The spaced capitals over a list ("IN “GAMEPLAY”").
QLabel *headLabel(const QString &text)
{
	auto *l = new QLabel(text);
	l->setProperty("role", "head");
	l->setFont(vtheme::head());
	l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	return l;
}

QLabel *roleLabel(const QString &text, const char *role)
{
	auto *l = new QLabel(text);
	l->setProperty("role", role);
	return l;
}

QFrame *hline()
{
	auto *f = new QFrame();
	f->setProperty("role", "line");
	return f;
}

// A control centred in a list cell; clicks beside it still select the row.
QWidget *cell(QWidget *control)
{
	auto *w = new QWidget();
	auto *l = new QHBoxLayout(w);
	l->setContentsMargins(4, 0, 4, 0);
	l->addWidget(control, 0, Qt::AlignCenter);
	return w;
}

QStringList mainScenes()
{
	QStringList out;
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++)
		out << QString::fromUtf8(obs_source_get_name(scenes.sources.array[i]));
	obs_frontend_source_list_free(&scenes);
	return out;
}

// A name not taken yet: "Base", "Base 2", "Base 3"...
QString freeName(const QString &base, const QStringList &taken)
{
	if (!taken.contains(base))
		return base;
	for (int i = 2;; i++) {
		const QString n = QString("%1 %2").arg(base).arg(i);
		if (!taken.contains(n))
			return n;
	}
}

// ---- Vertical Scenes ---------------------------------------------------------

QString selectedScene()
{
	QListWidgetItem *it = list ? list->currentItem() : nullptr;
	return it ? it->data(Qt::UserRole).toString() : QString();
}

void refreshScenes()
{
	const QString keep = selectedScene();
	list->blockSignals(true);
	list->clear();
	for (const QJsonValue v : canvas->sceneList()) {
		const QJsonObject s = v.toObject();
		const QString name = s.value("name").toString();
		QStringList mains;
		for (const QJsonValue m : s.value("mains").toArray())
			mains << m.toObject().value("name").toString();
		auto *it = new QListWidgetItem(name);
		it->setData(Qt::UserRole, name);
		it->setData(vtheme::SceneDelegate::OnAirRole, s.value("current").toBool());
		it->setData(vtheme::SceneDelegate::MainsRole, mains.join(", "));
		it->setToolTip(mains.isEmpty() ? QString("Shows when the main scene called \"%1\" is showing, if there is one.").arg(name)
					       : "Shows when one of these main scenes is showing: " + mains.join(", "));
		list->addItem(it);
		if (name == (keep.isEmpty() ? canvas->currentScene() : keep))
			list->setCurrentItem(it);
	}
	list->blockSignals(false);
	if (follow) {
		follow->blockSignals(true);
		follow->setChecked(canvas->followMain());
		follow->blockSignals(false);
	}
}

void addEmptyScene()
{
	bool ok = false;
	const QString name = QInputDialog::getText(scenesDock, "Add vertical scene", "Name:", QLineEdit::Normal,
						   freeName("Vertical scene", canvas->sceneNames()), &ok);
	QString error;
	if (!ok)
		return;
	if (!canvas->createScene(name, error) || !canvas->setScene(name.trimmed(), error))
		warn(error);
}

void addFromMain(const QString &main)
{
	bool ok = false;
	const QString name = QInputDialog::getText(scenesDock, "New vertical scene from \"" + main + "\"", "Name:",
						   QLineEdit::Normal, freeName(main, canvas->sceneNames()), &ok);
	QString error;
	if (!ok)
		return;
	if (!canvas->createFromMain(main, name, error) || !canvas->setScene(name.trimmed(), error)) {
		warn(error);
		return;
	}
	// Shown with its main scene from now on.
	canvas->linkScene(main, name.trimmed(), error);
}

void addMenu(QWidget *anchor)
{
	QMenu menu(anchor);
	menu.addAction("Empty scene…", []() { addEmptyScene(); });
	QMenu *from = menu.addMenu("From a main scene (laid out for 9:16)");
	const QStringList mains = mainScenes();
	if (mains.isEmpty())
		from->addAction("(no main scenes)")->setEnabled(false);
	for (const QString &m : mains)
		from->addAction(m, [m]() { addFromMain(m); });
	menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

void renameScene(const QString &name)
{
	bool ok = false;
	const QString to = QInputDialog::getText(scenesDock, "Rename vertical scene", "Name:", QLineEdit::Normal, name, &ok);
	QString error;
	if (ok && !canvas->renameScene(name, to, error))
		warn(error);
}

void duplicateScene(const QString &name)
{
	bool ok = false;
	const QString to = QInputDialog::getText(scenesDock, "Duplicate vertical scene", "Name of the copy:",
						 QLineEdit::Normal, freeName(name, canvas->sceneNames()), &ok);
	QString error;
	if (ok && !canvas->duplicateScene(name, to, error))
		warn(error);
}

void removeScene(const QString &name)
{
	if (QMessageBox::question(scenesDock, "Remove vertical scene",
				  QString("Remove the vertical scene \"%1\"? Its sources stay in OBS.").arg(name)) !=
	    QMessageBox::Yes)
		return;
	QString error;
	if (!canvas->removeScene(name, error))
		warn(error);
}

void moveScene(const QString &name, int delta)
{
	QString error;
	if (!canvas->moveScene(name, delta, error))
		warn(error);
}

void scenesMenu(const QPoint &pos)
{
	QListWidgetItem *it = list->itemAt(pos);
	QMenu menu(list);
	if (!it) {
		menu.addAction("Add vertical scene…", []() { addEmptyScene(); });
		menu.exec(list->viewport()->mapToGlobal(pos));
		return;
	}
	const QString name = it->data(Qt::UserRole).toString();
	menu.addAction("Rename…", [name]() { renameScene(name); });
	menu.addAction("Duplicate…", [name]() { duplicateScene(name); });
	menu.addAction("Remove", [name]() { removeScene(name); });
	menu.addSeparator();
	menu.addAction("Move up", [name]() { moveScene(name, -1); });
	menu.addAction("Move down", [name]() { moveScene(name, 1); });
	menu.addSeparator();
	// Which main scenes put this one on air. A main scene shows one
	// vertical scene, so ticking it here moves it from another.
	QMenu *with = menu.addMenu("Show with main scene");
	const QStringList linked = canvas->linkedMains(name);
	const QStringList mains = mainScenes();
	if (mains.isEmpty())
		with->addAction("(no main scenes)")->setEnabled(false);
	for (const QString &m : mains) {
		const QString other = canvas->linkFor(m);
		QAction *a = with->addAction(other.isEmpty() || other == name || linked.contains(m)
						     ? m
						     : QString("%1  (now: %2)").arg(m, other));
		a->setCheckable(true);
		a->setChecked(linked.contains(m));
		QObject::connect(a, &QAction::toggled, [m, name](bool on) {
			QString error;
			if (!canvas->linkScene(m, on ? name : QString(), error))
				warn(error);
		});
	}
	menu.exec(list->viewport()->mapToGlobal(pos));
}

QWidget *makeScenesDock()
{
	// Design 3 ("Cards"): each scene a rounded card row; no boxes around
	// them, so a small dock spends its room on the scenes.
	QWidget *w = dockRoot("multistreamVerticalScenes", 160, 70);
	auto *layout = new QVBoxLayout(scrolled(w));
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	list = new QListWidget();
	list->setObjectName("vScenes");
	list->setItemDelegate(new vtheme::SceneDelegate(list));
	list->setMouseTracking(true);
	list->viewport()->setAttribute(Qt::WA_Hover);
	list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	list->setMinimumHeight(30); // one row; it grows with the dock
	list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	list->setContextMenuPolicy(Qt::CustomContextMenu);
	list->setToolTip("Click a scene to put it on the vertical stream. Right-click for more.");
	QObject::connect(list, &QListWidget::itemClicked, list, [](QListWidgetItem *it) {
		QString error;
		if (!canvas->setScene(it->data(Qt::UserRole).toString(), error))
			warn(error);
	});
	QObject::connect(list, &QListWidget::itemDoubleClicked, list,
			 [](QListWidgetItem *it) { renameScene(it->data(Qt::UserRole).toString()); });
	QObject::connect(list, &QListWidget::customContextMenuRequested, list, &scenesMenu);
	layout->addWidget(list, 1);
	layout->addWidget(hline());

	auto *bar = new QHBoxLayout();
	bar->setContentsMargins(8, 5, 8, 5);
	bar->setSpacing(6);
	QToolButton *add = toolButton("+ Add scene", "Add a vertical scene: empty, or laid out from a main scene", "ghost");
	QToolButton *remove = iconButton("minus", "Remove the selected vertical scene");
	QToolButton *up = iconButton("up", "Move up");
	QToolButton *down = iconButton("down", "Move down");
	QObject::connect(add, &QToolButton::clicked, add, [add]() { addMenu(add); });
	QObject::connect(remove, &QToolButton::clicked, remove, []() {
		if (!selectedScene().isEmpty())
			removeScene(selectedScene());
	});
	QObject::connect(up, &QToolButton::clicked, up, []() {
		if (!selectedScene().isEmpty())
			moveScene(selectedScene(), -1);
	});
	QObject::connect(down, &QToolButton::clicked, down, []() {
		if (!selectedScene().isEmpty())
			moveScene(selectedScene(), 1);
	});
	add->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	bar->addWidget(add);
	bar->addStretch(1);
	for (QToolButton *b : {remove, up, down})
		bar->addWidget(b);
	layout->addLayout(bar);
	layout->addWidget(hline());

	auto *followBox = new QWidget();
	auto *fl = new QHBoxLayout(followBox);
	fl->setContentsMargins(10, 5, 8, 6);
	fl->setSpacing(10);
	auto *ft = new QVBoxLayout();
	ft->setSpacing(1);
	QLabel *followTitle = roleLabel("Follow the main scenes", "title");
	followTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	ft->addWidget(followTitle);
	followSub = roleLabel("Switches with the main scene that's showing", "muted");
	followSub->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	ft->addWidget(followSub);
	fl->addLayout(ft, 1);
	follow = new vtheme::Switch();
	follow->setToolTip("When a main scene is shown, show the vertical scene it's linked to "
			   "(right-click a vertical scene) or the one with the same name.");
	followBox->setToolTip(follow->toolTip());
	QObject::connect(follow, &QAbstractButton::toggled, follow, [](bool on) { canvas->setFollowMain(on); });
	fl->addWidget(follow, 0, Qt::AlignVCenter);
	layout->addWidget(followBox);

	// Moving in from Aitum Vertical or the Vertical tool (Lua script).
	importBox = new QWidget();
	auto *ib = new QVBoxLayout(importBox);
	ib->setContentsMargins(8, 4, 8, 6);
	importText = roleLabel(QString(), "note");
	importText->setWordWrap(true);
	ib->addWidget(importText);
	auto *ibar = new QHBoxLayout();
	importAitumButton = new QPushButton("Import Aitum's scenes");
	importAitumButton->setProperty("pill", "soft");
	importAitumButton->setToolTip("Copies Aitum Vertical's scenes here. Their sources are shared; Aitum's own scenes stay as they are.");
	QObject::connect(importAitumButton, &QPushButton::clicked, importAitumButton, []() {
		QString error;
		const int n = canvas->importAitum(error);
		if (n < 0)
			warn(error);
		else
			QMessageBox::information(scenesDock, "Multistream Vertical",
						 QString("Imported %1 scene(s) from Aitum Vertical. You can remove Aitum Vertical now.").arg(n));
	});
	importLuaButton = new QPushButton("Make scenes like it");
	importLuaButton->setProperty("pill", "soft");
	importLuaButton->setToolTip("One vertical scene per main scene, framed like the Vertical tool's picture. "
				    "They follow your main scenes by name.");
	QObject::connect(importLuaButton, &QPushButton::clicked, importLuaButton, []() {
		const QJsonObject st = canvas->importState();
		QString error;
		const int n = canvas->importLuaTool(st.value("luaFraming").toString("fill"), st.value("luaPosition").toInt(50), error);
		if (n < 0 || (!n && !error.isEmpty()))
			warn(error);
		else
			QMessageBox::information(scenesDock, "Multistream Vertical",
						 QString("Made %1 vertical scene(s). Now remove the Vertical tool: Tools \u2192 Scripts, "
							 "select multistream-vertical.lua, then \u2212.").arg(n));
	});
	ibar->addWidget(importAitumButton);
	ibar->addWidget(importLuaButton);
	ibar->addStretch(1);
	ib->addLayout(ibar);
	importBox->hide();
	layout->addWidget(importBox);
	// A short dock keeps the list: the Follow card drops its second line.
	new Fit(w, [w, add, followTitle]() {
		if (followSub)
			followSub->setVisible(w->height() >= 280 && w->width() >= 240);
		followTitle->setText(w->width() >= 220 ? "Follow the main scenes" : "Follow main");
		fitAdd(add, w->width(), "+ Add scene");
	});
	return w;
}

void refreshImport()
{
	if (!importBox || !canvas->ready())
		return;
	const QJsonObject st = canvas->importState();
	const QJsonArray aitumScenes = st.value("aitumScenes").toArray();
	const bool lua = st.value("luaRunning").toBool();
	// Aitum's scenes are offered until some of them are here.
	bool imported = false;
	const QStringList ours = canvas->sceneNames();
	for (const QJsonValue v : aitumScenes)
		imported = imported || ours.contains(v.toObject().value("name").toString());
	const bool aitum = !aitumScenes.isEmpty() && !imported;
	QStringList lines;
	if (lua)
		lines << "<b>The Vertical tool (Lua script) is also running.</b> Both would send your vertical stream: "
			 "keep one. To move to these scenes, make them like its picture, then remove the script "
			 "(Tools \u2192 Scripts).";
	if (aitum)
		lines << "<b>Aitum Vertical scenes found.</b> Import them to use them here.";
	importText->setText(lines.join("<br><br>"));
	importAitumButton->setVisible(aitum);
	importLuaButton->setVisible(lua);
	importBox->setVisible(!lines.isEmpty());
}

// ---- Vertical Sources --------------------------------------------------------

qint64 selectedItem()
{
	QTreeWidgetItem *it = items ? items->currentItem() : nullptr;
	return it ? qint64(it->data(0, Qt::UserRole).toDouble()) : 0;
}

void selectItem(qint64 id)
{
	if (!items)
		return;
	items->blockSignals(true);
	for (int i = 0; i < items->topLevelItemCount(); i++) {
		QTreeWidgetItem *it = items->topLevelItem(i);
		if (qint64(it->data(0, Qt::UserRole).toDouble()) == id) {
			items->setCurrentItem(it);
			break;
		}
	}
	if (!id)
		items->setCurrentItem(nullptr);
	items->blockSignals(false);
}

void refreshItems(bool force)
{
	if (!items)
		return;
	const QString scene = canvas->currentScene();
	const QJsonArray arr = canvas->sceneItems(scene);
	const QString sig = scene + QJsonDocument(arr).toJson(QJsonDocument::Compact);
	if (!force && sig == shownItems)
		return;
	shownItems = sig;
	if (itemsTitle)
		itemsTitle->setText(scene.isEmpty() ? QString("NO VERTICAL SCENE") : "IN “" + scene.toUpper() + "”");
	const qint64 keep = preview ? preview->selected() : selectedItem();
	items->blockSignals(true);
	items->clear();
	for (const QJsonValue v : arr) {
		const QJsonObject o = v.toObject();
		auto *it = new QTreeWidgetItem();
		const bool group = o.value("group").toBool(), visible = o.value("visible").toBool();
		const QString type = o.value("type").toString();
		const qint64 id = qint64(o.value("id").toDouble());
		it->setText(0, o.value("name").toString() + (group ? " (group)" : QString()));
		it->setIcon(0, vtheme::sourceIcon(type, group));
		it->setData(0, Qt::UserRole, o.value("id").toDouble());
		it->setToolTip(0, QString::fromUtf8(obs_source_get_display_name(type.toUtf8().constData())));
		if (!visible)
			it->setForeground(0, vtheme::MUTED);
		items->addTopLevelItem(it);
		auto *lock = new vtheme::IconToggle(vtheme::IconToggle::Kind::Lock);
		lock->setChecked(o.value("locked").toBool());
		lock->setToolTip("Lock (can't be moved in the preview)");
		QObject::connect(lock, &QAbstractButton::toggled, lock, [id](bool on) {
			QString error;
			if (!canvas->setItemLocked(canvas->currentScene(), id, on, error))
				warn(error);
		});
		auto *show = new vtheme::Switch();
		show->setChecked(visible);
		show->setToolTip("Show / hide in this vertical scene");
		QObject::connect(show, &QAbstractButton::toggled, show, [id](bool on) {
			QString error;
			if (!canvas->setItemVisible(canvas->currentScene(), id, on, error))
				warn(error);
		});
		items->setItemWidget(it, 1, cell(lock));
		items->setItemWidget(it, 2, cell(show));
	}
	items->blockSignals(false);
	selectItem(keep);
}

obs_source_t *itemSource(qint64 id)
{
	obs_scene_t *s = canvas->scene(canvas->currentScene());
	obs_sceneitem_t *item = s ? obs_scene_find_sceneitem_by_id(s, id) : nullptr;
	obs_source_t *src = item ? obs_source_get_ref(obs_sceneitem_get_source(item)) : nullptr;
	obs_scene_release(s);
	return src;
}

void addExisting(const QString &name)
{
	QString error;
	if (!canvas->addSource(canvas->currentScene(), name, error))
		warn(error);
}

void addNew(const QString &type)
{
	const QString display = QString::fromUtf8(obs_source_get_display_name(type.toUtf8().constData()));
	QStringList taken;
	obs_enum_sources(
		[](void *p, obs_source_t *src) {
			static_cast<QStringList *>(p)->append(QString::fromUtf8(obs_source_get_name(src)));
			return true;
		},
		&taken);
	bool ok = false;
	const QString name = QInputDialog::getText(sourcesDock, "New " + display, "Name:", QLineEdit::Normal,
						   freeName(display, taken), &ok);
	if (!ok)
		return;
	QString error;
	obs_source_t *src = nullptr;
	if (!canvas->addNewSource(canvas->currentScene(), type, name, error, &src)) {
		warn(error);
		return;
	}
	obs_frontend_open_source_properties(src);
	obs_source_release(src);
}

void addSourceMenu(QWidget *anchor)
{
	if (canvas->currentScene().isEmpty()) {
		warn("Add a vertical scene first.");
		return;
	}
	QMenu menu(anchor);
	// Sources OBS already has (shared: same camera, same alerts).
	QMenu *existing = menu.addMenu("Add existing source");
	struct Collect {
		QStringList names;
	} c;
	obs_enum_sources(
		[](void *p, obs_source_t *src) {
			auto *col = static_cast<Collect *>(p);
			if (!obs_source_is_group(src))
				col->names.append(QString::fromUtf8(obs_source_get_name(src)));
			return true;
		},
		&c);
	// Main scenes can be nested too.
	for (const QString &m : mainScenes())
		c.names.append(m);
	c.names.sort(Qt::CaseInsensitive);
	c.names.removeDuplicates();
	if (c.names.isEmpty())
		existing->addAction("(none yet)")->setEnabled(false);
	for (const QString &n : c.names)
		existing->addAction(n, [n]() { addExisting(n); });

	QMenu *fresh = menu.addMenu("New source");
	const char *id = nullptr;
	const char *unversioned = nullptr;
	QList<QPair<QString, QString>> types;
	for (size_t i = 0; obs_enum_input_types2(i, &id, &unversioned); i++) {
		const uint32_t caps = obs_get_source_output_flags(id);
		if (caps & (OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_DEPRECATED | OBS_SOURCE_CAP_OBSOLETE))
			continue;
		const char *display = obs_source_get_display_name(id);
		if (display && *display)
			types.append({QString::fromUtf8(display), QString::fromUtf8(id)});
	}
	std::sort(types.begin(), types.end(),
		  [](const auto &a, const auto &b) { return a.first.compare(b.first, Qt::CaseInsensitive) < 0; });
	for (const auto &t : types) {
		const QString type = t.second;
		fresh->addAction(t.first, [type]() { addNew(type); });
	}
	menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

void itemAction(const char *what)
{
	const qint64 id = selectedItem();
	if (!id)
		return;
	const QString scene = canvas->currentScene();
	QString error;
	const QString w = what;
	bool ok = true;
	if (w == "remove") {
		if (QMessageBox::question(sourcesDock, "Remove source", "Remove it from this vertical scene? It stays in OBS.") !=
		    QMessageBox::Yes)
			return;
		ok = canvas->removeItem(scene, id, error);
	} else if (w == "up") {
		ok = canvas->moveItem(scene, id, OBS_ORDER_MOVE_UP, error);
	} else if (w == "down") {
		ok = canvas->moveItem(scene, id, OBS_ORDER_MOVE_DOWN, error);
	} else if (w == "top") {
		ok = canvas->moveItem(scene, id, OBS_ORDER_MOVE_TOP, error);
	} else if (w == "bottom") {
		ok = canvas->moveItem(scene, id, OBS_ORDER_MOVE_BOTTOM, error);
	} else if (w == "properties" || w == "filters" || w == "rename") {
		obs_source_t *src = itemSource(id);
		if (src && w == "properties")
			obs_frontend_open_source_properties(src);
		else if (src && w == "filters")
			obs_frontend_open_source_filters(src);
		else if (src) {
			bool given = false;
			const QString old = QString::fromUtf8(obs_source_get_name(src));
			const QString to = QInputDialog::getText(sourcesDock, "Rename source",
								 "Name (it's the same source on the main canvas):",
								 QLineEdit::Normal, old, &given)
						   .trimmed();
			if (given && !to.isEmpty() && to != old) {
				if (obs_source_t *have = obs_get_source_by_name(to.toUtf8().constData())) {
					obs_source_release(have);
					error = "There's already a source called \"" + to + "\".";
					ok = false;
				} else {
					obs_source_set_name(src, to.toUtf8().constData());
				}
			}
		}
		obs_source_release(src);
	}
	if (!ok)
		warn(error);
	refreshItems(true);
}

QWidget *makeSourcesDock()
{
	// Design 1 ("Signal strip"), like the Multistream dock's platform list.
	QWidget *w = dockRoot("multistreamVerticalSources", 160, 70);
	auto *layout = new QVBoxLayout(scrolled(w));
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	auto *headRow = new QHBoxLayout();
	headRow->setContentsMargins(12, 8, 12, 4);
	itemsTitle = headLabel(QString());
	headRow->addWidget(itemsTitle, 1);
	lockShowHead = headLabel("LOCK · SHOW");
	lockShowHead->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	headRow->addWidget(lockShowHead);
	layout->addLayout(headRow);

	items = new QTreeWidget();
	items->setObjectName("vItems");
	items->setColumnCount(3);
	items->setHeaderLabels({"Source", "Lock", "Show"});
	items->setHeaderHidden(true);
	items->setIconSize(QSize(22, 22));
	items->setIndentation(0);
	items->setUniformRowHeights(true);
	items->setRootIsDecorated(false);
	items->setMinimumHeight(34); // one row; it grows with the dock
	items->header()->setStretchLastSection(false);
	items->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	items->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	items->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	items->setContextMenuPolicy(Qt::CustomContextMenu);
	items->setToolTip("The sources in the vertical scene that's showing, top first. Switch on to show; lock to keep in place.");
	QObject::connect(items, &QTreeWidget::currentItemChanged, items, [](QTreeWidgetItem *it, QTreeWidgetItem *) {
		if (preview)
			preview->select(it ? qint64(it->data(0, Qt::UserRole).toDouble()) : 0);
	});
	QObject::connect(items, &QTreeWidget::itemDoubleClicked, items, [](QTreeWidgetItem *, int column) {
		if (column == 0)
			itemAction("properties");
	});
	QObject::connect(items, &QTreeWidget::customContextMenuRequested, items, [](const QPoint &pos) {
		QMenu menu(items);
		if (items->itemAt(pos)) {
			menu.addAction("Properties…", []() { itemAction("properties"); });
			menu.addAction("Filters…", []() { itemAction("filters"); });
			menu.addAction("Rename…", []() { itemAction("rename"); });
			menu.addSeparator();
			menu.addAction("Move to top", []() { itemAction("top"); });
			menu.addAction("Move up", []() { itemAction("up"); });
			menu.addAction("Move down", []() { itemAction("down"); });
			menu.addAction("Move to bottom", []() { itemAction("bottom"); });
			menu.addSeparator();
			menu.addAction("Remove from this scene", []() { itemAction("remove"); });
		} else {
			menu.addAction("Add a source…", []() { addSourceMenu(items); });
		}
		menu.exec(items->viewport()->mapToGlobal(pos));
	});
	layout->addWidget(items, 1);
	layout->addWidget(hline());

	auto *bar = new QHBoxLayout();
	bar->setContentsMargins(10, 6, 10, 8);
	bar->setSpacing(6);
	QToolButton *add = toolButton("+ Add source", "Add a source: one OBS already has, or a new one", "soft");
	QToolButton *remove = iconButton("minus", "Remove the selected source from this vertical scene", "round");
	QToolButton *props = iconButton("gear", "Properties", "round");
	QToolButton *up = iconButton("up", "Move up", "round");
	QToolButton *down = iconButton("down", "Move down", "round");
	QObject::connect(add, &QToolButton::clicked, add, [add]() { addSourceMenu(add); });
	QObject::connect(remove, &QToolButton::clicked, remove, []() { itemAction("remove"); });
	QObject::connect(props, &QToolButton::clicked, props, []() { itemAction("properties"); });
	QObject::connect(up, &QToolButton::clicked, up, []() { itemAction("up"); });
	QObject::connect(down, &QToolButton::clicked, down, []() { itemAction("down"); });
	add->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	bar->addWidget(add);
	bar->addStretch(1);
	for (QToolButton *b : {remove, props, up, down})
		bar->addWidget(b);
	layout->addLayout(bar);
	new Fit(w, [w, add, props]() {
		if (lockShowHead)
			lockShowHead->setVisible(w->width() >= 250);
		fitAdd(add, w->width(), "+ Add source");
		props->setVisible(w->width() >= 210); // double-click opens it too
	});
	return w;
}

// ---- Vertical Mixer -----------------------------------------------------------

// The fader: 0..100 on a dB scale (-60 dB .. 0 dB), 0 = silent.
float faderToMul(int v)
{
	return v <= 0 ? 0.0f : std::pow(10.0f, (-60.0f + 0.6f * float(v)) / 20.0f);
}

int mulToFader(double mul)
{
	if (mul <= 0.001)
		return 0;
	return std::clamp(int(std::lround((20.0 * std::log10(mul) + 60.0) / 0.6)), 0, 100);
}

// The fader's level, as its strip shows it.
QString faderText(int v)
{
	return v <= 0 ? QString("−∞ dB") : QString::number(-60.0 + 0.6 * v, 'f', 1) + " dB";
}

void setAudio(const QString &uuid, const QJsonObject &change)
{
	QString error;
	if (!canvas->setAudio(uuid, change, error))
		warn(error);
}

void refreshMixer(bool force)
{
	if (!mixerRows)
		return;
	const QJsonArray arr = canvas->audioState().value("sources").toArray();
	// Rebuilt when the sources or their settings change; meters update in place.
	QString sig;
	for (const QJsonValue v : arr) {
		const QJsonObject o = v.toObject();
		sig += o.value("uuid").toString() + o.value("name").toString() + o.value("mode").toString() +
		       QString::number(o.value("volume").toDouble()) + (o.value("muted").toBool() ? "m" : "") + "|";
	}
	auto *strips = static_cast<QHBoxLayout *>(mixerRows->layout());
	if (force || sig != shownMixer) {
		shownMixer = sig;
		while (QLayoutItem *li = strips->takeAt(0)) {
			delete li->widget();
			delete li;
		}
		if (arr.isEmpty()) {
			QLabel *none = roleLabel("No sound in the vertical scene that's showing.", "muted");
			none->setWordWrap(true);
			strips->addWidget(none, 1, Qt::AlignTop);
		}
		// Design 4 ("Console"): a channel strip per source, its name, level
		// meter beside its fader, then MAIN and mute. Kept tight: the fader
		// takes whatever height the dock has (fitStrips).
		for (const QJsonValue v : arr) {
			const QJsonObject o = v.toObject();
			const QString uuid = o.value("uuid").toString();
			const QString name = o.value("name").toString();
			const bool own = o.value("mode").toString() == "own";
			auto *strip = new QFrame();
			strip->setObjectName("vStrip");
			strip->setProperty("own", own ? "true" : "false");
			strip->setFixedWidth(70);
			auto *sl = new QVBoxLayout(strip);
			sl->setContentsMargins(3, 4, 3, 4);
			sl->setSpacing(3);
			QLabel *label = roleLabel(QFontMetrics(vtheme::head()).elidedText(name.toUpper(), Qt::ElideRight, 62), "strip");
			label->setFont(vtheme::head());
			label->setAlignment(Qt::AlignCenter);
			label->setToolTip(name);
			auto *meter = new vtheme::Meter();
			meter->setObjectName("meter:" + uuid);
			meter->setToolTip("What the vertical stream hears from it");
			meter->setFixedSize(6, stripHeight);
			auto *fader = new QSlider(Qt::Vertical);
			fader->setObjectName("vFader");
			fader->setFixedSize(18, stripHeight);
			fader->setRange(0, 100);
			fader->setValue(mulToFader(own ? o.value("volume").toDouble() : o.value("gain").toDouble()));
			fader->setEnabled(own);
			fader->setCursor(own ? Qt::PointingHandCursor : Qt::ArrowCursor);
			fader->setToolTip((own ? "Vertical volume: " : "Follows OBS's own fader (turn off MAIN): ") +
					  faderText(fader->value()));
			QObject::connect(fader, &QSlider::valueChanged, fader, [fader](int value) {
				fader->setToolTip("Vertical volume: " + faderText(value));
			});
			QObject::connect(fader, &QSlider::sliderReleased, fader, [fader, uuid]() {
				setAudio(uuid, {{"volume", double(faderToMul(fader->value()))}});
			});
			QToolButton *same = toolButton("MAIN", "Lit: the vertical stream uses OBS's own fader and mute for it.\n"
							       "Off: its own vertical volume and mute.", "key");
			same->setCheckable(true);
			same->setChecked(!own);
			QObject::connect(same, &QToolButton::toggled, same,
					 [uuid](bool on) { setAudio(uuid, {{"mode", on ? "same" : "own"}}); });
			auto *mute = new vtheme::IconToggle(vtheme::IconToggle::Kind::Speaker);
			mute->setChecked(own && o.value("muted").toBool());
			mute->setEnabled(own);
			mute->setToolTip("Mute on the vertical stream only");
			QObject::connect(mute, &QAbstractButton::toggled, mute,
					 [uuid](bool on) { setAudio(uuid, {{"muted", on}}); });
			auto *mid = new QHBoxLayout();
			mid->setSpacing(5);
			mid->addStretch(1);
			mid->addWidget(meter);
			mid->addWidget(fader);
			mid->addStretch(1);
			auto *keys = new QHBoxLayout();
			keys->setSpacing(2);
			keys->addStretch(1);
			keys->addWidget(same);
			keys->addWidget(mute);
			keys->addStretch(1);
			sl->addWidget(label);
			sl->addLayout(mid, 1);
			sl->addLayout(keys);
			strips->addWidget(strip, 0, Qt::AlignTop);
		}
		strips->addStretch(1);
	}
	// The meters: peak of the last block, on the same dB scale.
	for (const QJsonValue v : arr) {
		const QJsonObject o = v.toObject();
		auto *meter = static_cast<vtheme::Meter *>(mixerRows->findChild<QWidget *>("meter:" + o.value("uuid").toString()));
		const double peak = o.value("peak").toDouble();
		if (meter)
			meter->setLevel(peak <= 0.001 ? 0 : std::clamp(int((20.0 * std::log10(peak) + 60.0) / 0.6), 0, 100));
	}
}

// The faders take the dock's height, so a strip never runs off the bottom.
// What a strip needs besides its fader (its name, MAIN and mute, gaps,
// margins, border, the list's margins) is measured from a strip; this is
// for when there is none yet.
const int STRIP_CHROME = 66;

void fitStrips()
{
	if (!mixerScroll || !mixerRows)
		return;
	const QMargins m = mixerRows->layout()->contentsMargins();
	QWidget *strip = mixerRows->findChild<QWidget *>("vStrip");
	const int chrome = strip ? strip->sizeHint().height() - stripHeight + m.top() + m.bottom() : STRIP_CHROME;
	const int h = std::max(16, mixerScroll->viewport()->height() - chrome);
	if (h == stripHeight)
		return;
	stripHeight = h;
	for (QWidget *c : mixerRows->findChildren<QWidget *>())
		if (c->objectName() == "vFader" || c->objectName().startsWith("meter:"))
			c->setFixedHeight(h);
}

QWidget *makeMixerDock()
{
	QWidget *w = dockRoot("multistreamVerticalMixer", 160, 70);
	auto *layout = new QVBoxLayout(w);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	// No heading: the strips get the room. What it said is the tooltip.
	auto *scroll = new QScrollArea();
	scroll->setToolTip("Only sources in the vertical scene that's showing play here, plus Desktop Audio and Mic.\n"
			   "Turn off MAIN on a source to give it its own vertical volume and mute.");
	scroll->setObjectName("vMixScroll");
	scroll->viewport()->setObjectName("vMixView");
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	mixerRows = new QWidget();
	mixerRows->setObjectName("vMixRows");
	auto *strips = new QHBoxLayout(mixerRows);
	strips->setContentsMargins(6, 6, 6, 6);
	strips->setSpacing(4);
	scroll->setWidget(mixerRows);
	layout->addWidget(scroll, 1);
	mixerScroll = scroll;
	new Fit(scroll->viewport(), []() { fitStrips(); });
	return w;
}

// ---- Vertical Preview -----------------------------------------------------------

void refreshRecord();

QWidget *makePreviewDock()
{
	// Design 1 ("Signal strip"): the live pill and scene over the picture.
	// Its minimum stays small: OBS squeezes a full column below the docks'
	// minimums, and the picture (a native window) would then cover the buttons.
	QWidget *w = dockRoot("multistreamVerticalPreview", 150, 110);
	auto *layout = new QVBoxLayout(w);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(6);
	auto *top = new QHBoxLayout();
	top->setSpacing(8);
	liveLabel = new QLabel();
	liveLabel->setProperty("tone", "off");
	top->addWidget(liveLabel);
	sceneLabel = roleLabel(QString(), "title");
	sceneLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	top->addWidget(sceneLabel, 1);
	sizeLabel = roleLabel(QString(), "mono");
	sizeLabel->setFont(vtheme::mono(11));
	top->addWidget(sizeLabel);
	layout->addLayout(top);
	preview = new PreviewWidget(canvas);
	preview->setMinimumSize(60, 40); // the dock decides; the picture letterboxes
	preview->setToolTip("Drag to move, corners to resize (Alt: crop), Ctrl+wheel to zoom, right-click for more.");
	preview->onSelect = [](qint64 id) { selectItem(id); };
	layout->addWidget(preview, 1);

	// Recording and replay buffer of the vertical picture.
	auto *bar = new QHBoxLayout();
	bar->setSpacing(6);
	recButton = toolButton("Rec", QString(), "rec");
	recButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	recButton->setIconSize(QSize(8, 8));
	recButton->setCheckable(true);
	QObject::connect(recButton, &QToolButton::clicked, recButton, [](bool on) {
		QString error;
		if (!on)
			canvas->recorder->stopRecording();
		else if (!canvas->recorder->startRecording(error))
			warn(error);
	});
	replayButton = toolButton("Replay", QString(), "ghost");
	replayButton->setCheckable(true);
	QObject::connect(replayButton, &QToolButton::clicked, replayButton, [](bool on) {
		QString error;
		if (!on)
			canvas->recorder->stopReplay();
		else if (!canvas->recorder->startReplay(error))
			warn(error);
	});
	saveButton = toolButton("Save clip", "Save a clip of the last seconds of the vertical picture", "ghost");
	QObject::connect(saveButton, &QToolButton::clicked, saveButton, []() {
		QString error;
		if (!canvas->recorder->saveReplay(error))
			warn(error);
	});
	QToolButton *more = iconButton("more", "Recording options, Twitch Dual Format", "round");
	QObject::connect(more, &QToolButton::clicked, more, [more]() {
		VerticalRecorder *r = canvas->recorder;
		const QJsonObject st = r->state();
		QMenu menu(more);
		QAction *followMain = menu.addAction("Record and replay with OBS's own buttons");
		followMain->setCheckable(true);
		followMain->setChecked(st.value("followMain").toBool());
		QObject::connect(followMain, &QAction::toggled, [r](bool on) { r->configure({{"followMain", on}}); });
		QMenu *fmt = menu.addMenu("File type");
		for (const char *f : {"mp4", "mkv"}) {
			const QString v = QString::fromLatin1(f);
			QAction *a = fmt->addAction(v.toUpper());
			a->setCheckable(true);
			a->setChecked(st.value("format").toString() == v);
			QObject::connect(a, &QAction::triggered, [r, v]() { r->configure({{"format", v}}); });
		}
		QMenu *len = menu.addMenu("Clip length");
		for (int s : {15, 30, 60, 120, 300}) {
			QAction *a = len->addAction(s < 60 ? QString("%1 seconds").arg(s)
							    : QString("%1 minute%2").arg(s / 60).arg(s >= 120 ? "s" : ""));
			a->setCheckable(true);
			a->setChecked(st.value("replaySeconds").toInt() == s);
			a->setEnabled(!r->replayActive());
			QObject::connect(a, &QAction::triggered, [r, s]() { r->configure({{"replaySeconds", s}}); });
		}
		const QString dir = st.value("folder").toString();
		menu.addAction("Open the recordings folder", [dir]() { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
		menu.addSeparator();
		const QJsonObject df = canvas->dualFormatState();
		QAction *dual = menu.addAction("Send to Twitch as Dual Format");
		dual->setCheckable(true);
		dual->setChecked(df.value("on").toBool());
		QObject::connect(dual, &QAction::toggled, [](bool on) {
			QString error;
			if (!canvas->setDualFormat(on, error))
				warn(error);
			else if (on && !canvas->dualFormatState().value("enhanced").toBool())
				QMessageBox::information(scenesDock, "Multistream Vertical",
							 "Now turn on Enhanced Broadcasting in OBS Settings \u2192 Stream (Twitch), "
							 "so Twitch gets the vertical picture too.");
		});
		menu.exec(more->mapToGlobal(QPoint(0, more->height())));
	});
	// Never squeezed until the words go: a narrow dock gets shorter words.
	for (QToolButton *b : {recButton.data(), replayButton.data(), saveButton.data()})
		b->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	bar->addWidget(recButton);
	bar->addWidget(replayButton);
	bar->addWidget(saveButton);
	bar->addStretch(1);
	recLabel = roleLabel(QString(), "warn");
	bar->addWidget(recLabel);
	bar->addWidget(more);
	layout->addLayout(bar);
	// A narrow dock: shorter button words, then icons; no size next to the scene.
	new Fit(w, [w]() {
		const int words = w->width() >= 350 ? 2 : w->width() >= 290 ? 1 : 0;
		if (sizeLabel)
			sizeLabel->setVisible(w->width() >= 250);
		if (words != recordWords) {
			recordWords = words;
			for (QToolButton *b : {recButton.data(), replayButton.data(), saveButton.data()})
				b->setToolButtonStyle(words ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
			replayButton->setIcon(words ? QIcon() : vtheme::lineIcon("replay"));
			saveButton->setIcon(words ? QIcon() : vtheme::lineIcon("save"));
			refreshRecord();
		}
	});
	return w;
}

void refreshRecord()
{
	if (!recButton || !canvas->recorder)
		return;
	const QJsonObject st = canvas->recorder->state();
	const bool rec = st.value("recording").toBool(), rep = st.value("replay").toBool();
	recButton->setChecked(rec);
	recButton->setText(rec ? "Stop" : "Rec");
	if (recButton->property("recIcon").toBool() != rec || recButton->icon().isNull()) {
		recButton->setIcon(vtheme::dotIcon(rec ? vtheme::ROSE_TEXT : vtheme::ROSE, rec));
		recButton->setProperty("recIcon", rec);
	}
	recButton->setToolTip(rec ? "Recording to " + st.value("lastFile").toString()
				  : "Record the vertical picture into " + st.value("folder").toString());
	replayButton->setChecked(rep);
	replayButton->setText(rep && recordWords == 2 ? "Replay \u2713" : "Replay");
	saveButton->setText(recordWords == 2 ? "Save clip" : "Save");
	replayButton->setToolTip(rep ? "The vertical replay buffer is running: Save clip keeps the last seconds."
				     : "Start the vertical replay buffer");
	saveButton->setEnabled(rep);
	const QString err = st.value("error").toString();
	recLabel->setText(err.isEmpty() ? QString() : "\u26a0");
	recLabel->setToolTip(err);
}

// The pill over the preview: is the vertical stream going to Multistream?
void refreshLive()
{
	if (!liveLabel || !liveState)
		return;
	const QJsonObject st = liveState();
	const QString error = st.value("error").toString();
	QString text = "Not live", tone = "off", tip = "Goes live on Multistream with Start Streaming.";
	if (st.value("reconnecting").toBool()) {
		text = "Reconnecting";
		tone = "busy";
		tip = "The vertical stream lost its connection and is trying again.";
	} else if (st.value("sending").toBool()) {
		text = "LIVE";
		tone = "live";
		tip = "The vertical stream is going to Multistream.";
	} else if (st.value("connecting").toBool()) {
		text = "Connecting";
		tone = "busy";
		tip = "Starting the vertical stream.";
	} else if (!error.isEmpty()) {
		text = "Problem";
		tone = "bad";
		tip = error;
	} else if (!st.value("enabled").toBool()) {
		text = "Off";
		tip = "The vertical stream to Multistream is turned off.";
	}
	liveLabel->setText("● " + text);
	liveLabel->setToolTip(tip);
	vtheme::setProp(liveLabel, "tone", tone);
	// Vertical Scenes says ON AIR only while the vertical stream is out.
	const bool out = st.value("sending").toBool() || st.value("reconnecting").toBool();
	if (out != vtheme::SceneDelegate::live) {
		vtheme::SceneDelegate::live = out;
		if (list)
			list->viewport()->update();
	}
}

void refresh(bool force = false)
{
	if (!canvas)
		return;
	const bool changed = force || canvas->version() != shownVersion;
	if (changed) {
		shownVersion = canvas->version();
		if (list)
			refreshScenes();
		if (preview)
			preview->sync();
		if (sceneLabel) {
			sceneLabel->setText(canvas->ready() ? canvas->currentScene() : QString("The vertical canvas is starting…"));
			sceneLabel->setToolTip(canvas->ready() ? "Showing: " + canvas->currentScene() : QString());
			sizeLabel->setText(canvas->ready() ? QString("%1×%2").arg(canvas->width()).arg(canvas->height())
							   : QString());
		}
	}
	refreshItems(changed);
	refreshRecord();
	refreshLive();
}

void addDock(const char *id, const char *title, QWidget *w)
{
	if (!obs_frontend_add_dock_by_id(id, title, w))
		obs_log(LOG_WARNING, "vertical: OBS already has a dock called %s", id);
}

} // namespace

namespace vertical_docks {

void create(VerticalCanvas *c, std::function<QJsonObject()> live)
{
	canvas = c;
	liveState = std::move(live);
	scenesDock = makeScenesDock();
	addDock("multistream-vertical-scenes", "Vertical Scenes", scenesDock);
	sourcesDock = makeSourcesDock();
	addDock("multistream-vertical-sources", "Vertical Sources", sourcesDock);
	previewDock = makePreviewDock();
	addDock("multistream-vertical-preview", "Vertical Preview", previewDock);
	mixerDock = makeMixerDock();
	addDock("multistream-vertical-mixer", "Vertical Mixer", mixerDock);

	watcher = new QTimer();
	QObject::connect(watcher, &QTimer::timeout, []() { refresh(); });
	watcher->start(250);
	meters = new QTimer();
	QObject::connect(meters, &QTimer::timeout, []() { refreshMixer(false); });
	meters->start(100);
	importTimer = new QTimer();
	QObject::connect(importTimer, &QTimer::timeout, []() { refreshImport(); });
	importTimer->start(3000);
}

// The first time, OBS would keep the new docks hidden in the Docks menu:
// put them on the right of OBS's window, where a new streamer sees them.
// Each dock has its own flag, so docks added in later versions show too.
void showOnFirstRun()
{
	refresh(true);
	refreshMixer(true);
	refreshImport();
	char *file = obs_module_config_path("vertical-docks.json");
	obs_data_t *d = obs_data_create_from_json_file_safe(file, "bak");
	if (!d)
		d = obs_data_create();
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	// Before per-dock flags: "shown" meant Scenes and Preview.
	if (obs_data_get_bool(d, "shown")) {
		obs_data_set_bool(d, "multistreamVerticalScenes", true);
		obs_data_set_bool(d, "multistreamVerticalPreview", true);
	}
	bool saved = false;
	QDockWidget *previewDw = previewDock ? qobject_cast<QDockWidget *>(previewDock->parentWidget()) : nullptr;
	for (QWidget *content : {previewDock.data(), scenesDock.data(), sourcesDock.data(), mixerDock.data()}) {
		auto *dw = content ? qobject_cast<QDockWidget *>(content->parentWidget()) : nullptr;
		const QByteArray key = content ? content->objectName().toUtf8() : QByteArray();
		if (!dw || !main || obs_data_get_bool(d, key.constData()))
			continue;
		dw->setFloating(false);
		main->addDockWidget(Qt::RightDockWidgetArea, dw);
		dw->show();
		if (dw != previewDw && previewDw && !previewDw->isFloating())
			main->splitDockWidget(previewDw, dw, Qt::Vertical);
		obs_data_set_bool(d, key.constData(), true);
		saved = true;
	}
	// All four placed now (a first run): OBS would split the column evenly,
	// leaving the picture small. Most of it goes to the preview.
	QList<QDockWidget *> column;
	for (QWidget *content : {previewDock.data(), mixerDock.data(), sourcesDock.data(), scenesDock.data()}) {
		auto *dw = content ? qobject_cast<QDockWidget *>(content->parentWidget()) : nullptr;
		if (dw && main && !dw->isFloating() && !dw->isHidden() && main->dockWidgetArea(dw) == Qt::RightDockWidgetArea)
			column << dw;
	}
	if (saved && main && column.size() == 4) {
		// resizeDocks does nothing in OBS's window, so once it is on screen
		// the preview (38% of the column) and the mixer (24%) briefly ask
		// for it as their minimum and the lists give way; then they let go,
		// keeping the size.
		QList<QPointer<QDockWidget>> held;
		for (QDockWidget *dw : column)
			held << dw;
		QList<QPointer<QWidget>> grow{previewDock.data(), mixerDock.data()};
		QTimer::singleShot(1500, main, [held, grow]() {
			int h = 0;
			for (const QPointer<QDockWidget> &dw : held) {
				if (!dw || dw->isFloating() || dw->isHidden() || !dw->isVisible())
					return;
				h += dw->height();
			}
			if (h < 300 || !grow[0] || !grow[1])
				return;
			QList<QSize> before;
			for (int i = 0; i < 2; ++i) {
				before << grow[i]->minimumSize();
				const int title = held[i]->height() - grow[i]->height();
				grow[i]->setMinimumHeight(std::max(before[i].height(), h * (i ? 24 : 38) / 100 - title));
			}
			QTimer::singleShot(800, grow[0], [held, grow, before]() {
				for (int i = 0; i < 2; ++i)
					if (grow[i])
						grow[i]->setMinimumSize(before[i]);
				obs_log(LOG_INFO, "Vertical docks: preview %d, mixer %d", held[0] ? held[0]->height() : -1,
					held[1] ? held[1]->height() : -1);
			});
		});
	}
	if (saved) {
		char *dir = obs_module_config_path("");
		os_mkdirs(dir);
		bfree(dir);
		obs_data_save_json_safe(d, file, "tmp", "bak");
	}
	obs_data_release(d);
	bfree(file);
}

void close()
{
	for (QTimer **t : {&watcher, &meters, &importTimer}) {
		if (*t) {
			(*t)->stop();
			delete *t;
			*t = nullptr;
		}
	}
}

QJsonObject report()
{
	QJsonObject o;
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (main) {
		o["window"] = QJsonObject{{"width", main->width()}, {"height", main->height()}};
		QJsonArray docks;
		for (QDockWidget *dw : main->findChildren<QDockWidget *>()) {
			const Qt::DockWidgetArea area = main->dockWidgetArea(dw);
			QWidget *content = dw->widget();
			docks.append(QJsonObject{
				{"id", dw->objectName()},
				{"title", dw->windowTitle()},
				{"visible", dw->isVisible()},
				{"floating", dw->isFloating()},
				{"area", area == Qt::RightDockWidgetArea ? "right"
					 : area == Qt::LeftDockWidgetArea ? "left"
					 : area == Qt::BottomDockWidgetArea ? "bottom"
					 : area == Qt::TopDockWidgetArea ? "top"
									  : "none"},
				{"x", dw->x()},
				{"y", dw->y()},
				{"width", dw->width()},
				{"height", dw->height()},
				// What the dock can't shrink below: its own minimum and its
				// title bar on top of its contents'.
				{"minWidth", dw->minimumSizeHint().width()},
				{"minHeight", std::max(dw->minimumHeight(), dw->minimumSizeHint().height())},
				{"contentMinHeight", content ? std::max(content->minimumHeight(), content->minimumSizeHint().height()) : 0},
			});
		}
		o["docks"] = docks;
	}
	QJsonArray rows;
	if (list)
		for (int i = 0; i < list->count(); ++i) {
			QListWidgetItem *it = list->item(i);
			rows.append(QJsonObject{
				{"name", it->text()},
				{"tag", vtheme::SceneDelegate::tag(it->data(vtheme::SceneDelegate::OnAirRole).toBool(),
								   it->data(vtheme::SceneDelegate::MainsRole).toString())},
			});
		}
	o["sceneRows"] = rows;
	return o;
}

} // namespace vertical_docks
