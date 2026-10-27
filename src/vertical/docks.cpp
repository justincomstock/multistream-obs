#include "docks.hpp"

#include "canvas.hpp"
#include "../i18n.hpp"
#include "item-menu.hpp"
#include "preview.hpp"
#include "record.hpp"
#include "theme.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QDesktopServices>
#include <QFileInfo>
#include <QFontMetrics>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
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
QPointer<QToolButton> pauseButton;
QPointer<QLabel> noticeLabel; // "Clip saved · Open folder", for a moment
int shownNotice = 0;
QString shownRecError; // the recording problem last told about
QPointer<QWidget> importBox;
QPointer<QLabel> importText;
QPointer<QPushButton> importAitumButton;
QPointer<QPushButton> importLuaButton;
QTimer *importTimer = nullptr;
PreviewWidget *preview = nullptr;
QPointer<QStackedWidget> previewStack; // the preview, or "the preview is off"
int zoomPercent = 100;
QTimer *watcher = nullptr;
QTimer *meters = nullptr;
quint64 shownVersion = 0;
QString shownItems;
QString shownMixer;

void warn(const QString &error)
{
	QMessageBox::warning(scenesDock, QStringLiteral("Multistream Vertical"), error);
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

// Keys for a widget: on(e, run) says whether it takes the key, and with run does
// it. Keys it takes don't reach OBS's main-window shortcuts (OBS's Ctrl+C or
// Delete are for the main scenes' sources).
class Keys : public QObject {
public:
	Keys(QWidget *watched, std::function<bool(QKeyEvent *, bool)> on) : QObject(watched), on_(std::move(on))
	{
		watched->installEventFilter(this);
	}
	bool eventFilter(QObject *, QEvent *e) override
	{
		if (e->type() == QEvent::ShortcutOverride && on_(static_cast<QKeyEvent *>(e), false)) {
			e->accept();
			return true;
		}
		if (e->type() == QEvent::KeyPress)
			return on_(static_cast<QKeyEvent *>(e), true);
		return false;
	}

private:
	std::function<bool(QKeyEvent *, bool)> on_;
};

bool isDeleteKey(QKeyEvent *e)
{
#ifdef __APPLE__
	if (e->key() == Qt::Key_Backspace) // a Mac keyboard's Delete key
		return true;
#endif
	return e->key() == Qt::Key_Delete;
}

// Ctrl+Z / Ctrl+Y in a dock floating in its own window, where OBS's own don't reach.
void undoKeys(QWidget *root)
{
	new Keys(root, [](QKeyEvent *e, bool run) {
		const bool undo = e->matches(QKeySequence::Undo), redo = e->matches(QKeySequence::Redo);
		if (!run || (!undo && !redo))
			return false; // OBS's Edit -> Undo goes first, where it can
		vitem::undo(redo);
		return true;
	});
}

// An action of OBS's main window by its object name (null if OBS renamed it).
QAction *obsAction(const char *name)
{
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	return main ? main->findChild<QAction *>(name) : nullptr;
}

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
	add->setText(width >= 320 ? full : width >= 250 ? mt("Vertical.AddShort") : QString("+"));
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
		it->setToolTip(mains.isEmpty() ? mt("Vertical.SceneTipNone").arg(name)
					       : mt("Vertical.SceneTipMains").arg(mains.join(", ")));
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
	if (followSub)
		// A click here would otherwise seem to be undone when the main scene changes.
		followSub->setText(canvas->followMain() ? mt("Vertical.FollowOnSub") : mt("Vertical.FollowOffSub"));
}

void addEmptyScene()
{
	bool ok = false;
	const QString name = QInputDialog::getText(scenesDock, mt("Vertical.AddSceneTitle"), mt("Vertical.NameLabel"), QLineEdit::Normal,
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
	const QString name = QInputDialog::getText(scenesDock, mt("Vertical.NewFromMainTitle").arg(main), mt("Vertical.NameLabel"),
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
	menu.addAction(mt("Vertical.EmptyScene"), []() { addEmptyScene(); });
	QMenu *from = menu.addMenu(mt("Vertical.FromMain"));
	const QStringList mains = mainScenes();
	if (mains.isEmpty())
		from->addAction(mt("Vertical.NoMainScenes"))->setEnabled(false);
	for (const QString &m : mains)
		from->addAction(m, [m]() { addFromMain(m); });
	menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

void renameScene(const QString &name)
{
	bool ok = false;
	const QString to = QInputDialog::getText(scenesDock, mt("Vertical.RenameTitle"), mt("Vertical.NameLabel"), QLineEdit::Normal, name, &ok);
	QString error;
	if (ok && !canvas->renameScene(name, to, error))
		warn(error);
}

void duplicateScene(const QString &name)
{
	bool ok = false;
	const QString to = QInputDialog::getText(scenesDock, mt("Vertical.DuplicateTitle"), mt("Vertical.CopyName"),
						 QLineEdit::Normal, freeName(name, canvas->sceneNames()), &ok);
	QString error;
	if (ok && !canvas->duplicateScene(name, to, error))
		warn(error);
}

void removeScene(const QString &name)
{
	if (QMessageBox::question(scenesDock, mt("Vertical.RemoveTitle"), mt("Vertical.AskRemoveScene").arg(name)) !=
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

// The vertical canvas's own scene transition: OBS's, a cut or a fade, and how long.
void transitionMenu(QMenu &menu)
{
	QMenu *tr = menu.addMenu(mt("Vertical.TransitionMenu"));
	const QString type = canvas->transitionChoice();
	const int ms = canvas->transitionMs();
	const QPair<const char *, const char *> kinds[] = {
		{"Vertical.TransSameAsObs", ""}, {"Vertical.TransCut", "cut_transition"}, {"Vertical.TransFade", "fade_transition"}};
	for (const auto &k : kinds) {
		const QString id = QString::fromLatin1(k.second);
		QAction *a = tr->addAction(mt(k.first), [id]() { canvas->setTransition(id, canvas->transitionMs()); });
		a->setCheckable(true);
		a->setChecked(type == id);
	}
	tr->addSeparator();
	const QPair<const char *, int> lengths[] = {
		{"Vertical.TransLenSame", 0}, {"Vertical.TransSec03", 300}, {"Vertical.TransSec05", 500}, {"Vertical.TransSec1", 1000}};
	for (const auto &l : lengths) {
		const int v = l.second;
		QAction *a = tr->addAction(mt(l.first), [v]() { canvas->setTransition(canvas->transitionChoice(), v); });
		a->setCheckable(true);
		a->setChecked(ms == v);
		a->setEnabled(type != "cut_transition"); // a cut takes no time
	}
}

void scenesMenu(const QPoint &pos)
{
	QListWidgetItem *it = list->itemAt(pos);
	QMenu menu(list);
	if (!it) {
		menu.addAction(mt("Vertical.AddSceneMenu"), []() { addEmptyScene(); });
		menu.addSeparator();
		transitionMenu(menu);
		menu.exec(list->viewport()->mapToGlobal(pos));
		return;
	}
	const QString name = it->data(Qt::UserRole).toString();
	menu.addAction(mt("Vertical.RenameF2"), [name]() { renameScene(name); });
	menu.addAction(mt("Vertical.Duplicate"), [name]() { duplicateScene(name); });
	menu.addAction(mt("Vertical.RemoveDelete"), [name]() { removeScene(name); });
	menu.addSeparator();
	QMenu *order = menu.addMenu(mt("Vertical.Order"));
	order->addAction(mt("Vertical.MoveUp"), [name]() { moveScene(name, -1); });
	order->addAction(mt("Vertical.MoveDown"), [name]() { moveScene(name, 1); });
	order->addAction(mt("Vertical.MoveToTop"), [name]() { moveScene(name, -100000); });
	order->addAction(mt("Vertical.MoveToBottom"), [name]() { moveScene(name, 100000); });
	menu.addAction(mt("Vertical.FiltersPlain"), [name]() {
		if (obs_scene_t *s = canvas->scene(name)) {
			obs_frontend_open_source_filters(obs_scene_get_source(s));
			obs_scene_release(s);
		}
	});
	menu.addAction(mt("Vertical.ScreenshotScene"), [name]() {
		if (obs_scene_t *s = canvas->scene(name)) {
			obs_frontend_take_source_screenshot(obs_scene_get_source(s));
			obs_scene_release(s);
		}
	});
	if (QAction *settings = obsAction("action_Settings")) {
		QAction *keys = menu.addAction(mt("Vertical.HotkeyMenu"), [settings]() { settings->trigger(); });
		keys->setToolTip(mt("Vertical.HotkeyMenuTip"));
	}
	transitionMenu(menu);
	menu.addSeparator();
	// Which main scenes put this one on air. A main scene shows one
	// vertical scene, so ticking it here moves it from another.
	QMenu *with = menu.addMenu(mt("Vertical.ShowWithMain"));
	const QStringList linked = canvas->linkedMains(name);
	const QStringList mains = mainScenes();
	if (mains.isEmpty())
		with->addAction(mt("Vertical.NoMainScenes"))->setEnabled(false);
	for (const QString &m : mains) {
		const QString other = canvas->linkFor(m);
		QAction *a = with->addAction(other.isEmpty() || other == name || linked.contains(m)
						     ? m
						     : mt("Vertical.MainNow").arg(m, other));
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

// Drag a scene up or down to reorder; the canvas keeps the order.
class SceneList : public QListWidget {
protected:
	void dropEvent(QDropEvent *e) override
	{
		QListWidgetItem *moving = currentItem();
		if (e->source() != this || !moving) {
			e->ignore();
			return;
		}
		const int from = row(moving);
		const QPoint pos = e->position().toPoint();
		QListWidgetItem *over = itemAt(pos);
		int to = over ? row(over) + (pos.y() > visualItemRect(over).center().y() ? 1 : 0) : count();
		if (to > from)
			to--;
		// The list is rebuilt from the canvas's order: Qt mustn't move or drop the row.
		e->setDropAction(Qt::IgnoreAction);
		e->accept();
		if (to != from)
			moveScene(moving->data(Qt::UserRole).toString(), to - from);
	}
};

QWidget *makeScenesDock()
{
	// Design 3 ("Cards"): each scene a rounded card row; no boxes around
	// them, so a small dock spends its room on the scenes.
	QWidget *w = dockRoot("multistreamVerticalScenes", 160, 70);
	auto *layout = new QVBoxLayout(scrolled(w));
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	list = new SceneList();
	list->setObjectName("vScenes");
	list->setDragDropMode(QAbstractItemView::InternalMove);
	list->setDefaultDropAction(Qt::MoveAction);
	list->setItemDelegate(new vtheme::SceneDelegate(list));
	list->setMouseTracking(true);
	list->viewport()->setAttribute(Qt::WA_Hover);
	list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	list->setMinimumHeight(30); // one row; it grows with the dock
	list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	list->setContextMenuPolicy(Qt::CustomContextMenu);
	list->setToolTip(mt("Vertical.ScenesTip"));
	QObject::connect(list, &QListWidget::itemClicked, list, [](QListWidgetItem *it) {
		QString error;
		if (!canvas->setScene(it->data(Qt::UserRole).toString(), error))
			warn(error);
	});
	new Keys(list, [](QKeyEvent *e, bool run) {
		const QString name = selectedScene();
		const bool f2 = e->key() == Qt::Key_F2, del = isDeleteKey(e);
		if (name.isEmpty() || (!f2 && !del))
			return false;
		if (run && f2)
			renameScene(name);
		else if (run)
			removeScene(name);
		return true;
	});
	QObject::connect(list, &QListWidget::customContextMenuRequested, list, &scenesMenu);
	layout->addWidget(list, 1);
	layout->addWidget(hline());

	auto *bar = new QHBoxLayout();
	bar->setContentsMargins(8, 5, 8, 5);
	bar->setSpacing(6);
	QToolButton *add = toolButton(mt("Vertical.AddScene"), mt("Vertical.AddSceneTip"), "ghost");
	QToolButton *remove = iconButton("minus", mt("Vertical.RemoveSceneTip"));
	QToolButton *up = iconButton("up", mt("Vertical.MoveUp"));
	QToolButton *down = iconButton("down", mt("Vertical.MoveDown"));
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
	QLabel *followTitle = roleLabel(mt("Vertical.FollowTitle"), "title");
	followTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	ft->addWidget(followTitle);
	followSub = roleLabel(mt("Vertical.FollowSub"), "muted");
	followSub->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	ft->addWidget(followSub);
	fl->addLayout(ft, 1);
	follow = new vtheme::Switch();
	follow->setToolTip(mt("Vertical.FollowTip"));
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
	importAitumButton = new QPushButton(mt("Vertical.ImportAitum"));
	importAitumButton->setProperty("pill", "soft");
	importAitumButton->setToolTip(mt("Vertical.ImportAitumTip"));
	QObject::connect(importAitumButton, &QPushButton::clicked, importAitumButton, []() {
		QString error;
		const int n = canvas->importAitum(error);
		if (n < 0)
			warn(error);
		else
			QMessageBox::information(scenesDock, QStringLiteral("Multistream Vertical"),
						 mt(n == 1 ? "Vertical.ImportedOne" : "Vertical.ImportedOther").arg(n));
	});
	importLuaButton = new QPushButton(mt("Vertical.MakeScenes"));
	importLuaButton->setProperty("pill", "soft");
	importLuaButton->setToolTip(mt("Vertical.MakeScenesTip"));
	QObject::connect(importLuaButton, &QPushButton::clicked, importLuaButton, []() {
		const QJsonObject st = canvas->importState();
		QString error;
		const int n = canvas->importLuaTool(st.value("luaFraming").toString("fill"), st.value("luaPosition").toInt(50), error);
		if (n < 0 || (!n && !error.isEmpty()))
			warn(error);
		else
			QMessageBox::information(scenesDock, QStringLiteral("Multistream Vertical"),
						 mt(n == 1 ? "Vertical.MadeOne" : "Vertical.MadeOther").arg(n));
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
		followTitle->setText(w->width() >= 220 ? mt("Vertical.FollowTitle") : mt("Vertical.FollowShort"));
		fitAdd(add, w->width(), mt("Vertical.AddScene"));
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
		lines << mt("Vertical.LuaRunning");
	if (aitum)
		lines << mt("Vertical.AitumFound");
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

QList<qint64> selectedIds()
{
	QList<qint64> ids;
	if (items)
		for (QTreeWidgetItem *it : items->selectedItems())
			ids << qint64(it->data(0, Qt::UserRole).toDouble());
	return ids;
}

void selectItem(qint64 id)
{
	if (!items)
		return;
	items->blockSignals(true);
	QTreeWidgetItem *found = nullptr;
	for (QTreeWidgetItemIterator i(items); id && *i; ++i)
		if (qint64((*i)->data(0, Qt::UserRole).toDouble()) == id) {
			found = *i;
			break;
		}
	items->setCurrentItem(found);
	if (!found)
		items->clearSelection();
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
		itemsTitle->setText(scene.isEmpty() ? mt("Vertical.NoVerticalScene") : mt("Vertical.InScene").arg(scene.toUpper()));
	const qint64 keep = preview ? preview->selected() : selectedItem();
	const int scroll = items->verticalScrollBar()->value();
	items->blockSignals(true);
	items->clear();
	QMap<qint64, QTreeWidgetItem *> groups;
	for (const QJsonValue v : arr) {
		const QJsonObject o = v.toObject();
		auto *it = new QTreeWidgetItem();
		const bool group = o.value("group").toBool(), visible = o.value("visible").toBool();
		const QString type = o.value("type").toString();
		const qint64 id = qint64(o.value("id").toDouble());
		const qint64 parent = qint64(o.value("parent").toDouble());
		it->setText(0, group ? mt("Vertical.GroupName").arg(o.value("name").toString()) : o.value("name").toString());
		it->setIcon(0, vtheme::sourceIcon(type, group));
		it->setData(0, Qt::UserRole, o.value("id").toDouble());
		it->setToolTip(0, QString::fromUtf8(obs_source_get_display_name(type.toUtf8().constData())));
		if (!visible)
			it->setForeground(0, vtheme::MUTED);
		// Rows are dragged between rows, never into one; a group's sources stay in it.
		it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | (parent ? Qt::NoItemFlags : Qt::ItemIsDragEnabled));
		if (QTreeWidgetItem *g = parent ? groups.value(parent) : nullptr)
			g->addChild(it); // indented under its group
		else
			items->addTopLevelItem(it);
		if (group)
			groups[id] = it;
		auto *lock = new vtheme::IconToggle(vtheme::IconToggle::Kind::Lock);
		lock->setChecked(o.value("locked").toBool());
		lock->setToolTip(mt("Vertical.LockTip"));
		QObject::connect(lock, &QAbstractButton::toggled, lock, [id](bool on) {
			QString error;
			if (!canvas->setItemLocked(canvas->currentScene(), id, on, error))
				warn(error);
		});
		auto *show = new vtheme::Switch();
		show->setChecked(visible);
		show->setToolTip(mt("Vertical.ShowTip"));
		QObject::connect(show, &QAbstractButton::toggled, show, [id](bool on) {
			QString error;
			if (!canvas->setItemVisible(canvas->currentScene(), id, on, error))
				warn(error);
		});
		items->setItemWidget(it, 1, cell(lock));
		items->setItemWidget(it, 2, cell(show));
	}
	items->expandAll();
	items->blockSignals(false);
	selectItem(keep);
	items->verticalScrollBar()->setValue(scroll); // a change doesn't jump the list to the top
}

obs_source_t *itemSource(qint64 id)
{
	obs_scene_t *s = canvas->scene(canvas->currentScene());
	obs_sceneitem_t *item = s ? VerticalCanvas::itemById(s, id) : nullptr;
	obs_source_t *src = item ? obs_source_get_ref(obs_sceneitem_get_source(item)) : nullptr;
	obs_scene_release(s);
	return src;
}

void addExisting(const QString &name, bool vertical = false)
{
	QString error;
	if (!canvas->addSource(canvas->currentScene(), name, error, vertical))
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
	const QString name = QInputDialog::getText(sourcesDock, mt("Vertical.NewSourceTitle").arg(display), mt("Vertical.NameLabel"), QLineEdit::Normal,
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

// Opens below `anchor`, or at screen point `at`.
void addSourceMenu(QWidget *anchor, const QPoint *at = nullptr)
{
	if (canvas->currentScene().isEmpty()) {
		warn(mt("Vertical.AddSceneFirst"));
		return;
	}
	QMenu menu(anchor);
	// Sources OBS already has (shared: same camera, same alerts).
	QMenu *existing = menu.addMenu(mt("Vertical.AddExisting"));
	using Named = QList<QPair<QString, QString>>; // (name, type id)
	struct Collect {
		QMap<QString, Named> byKind; // under the kind's name, as OBS's own menu shows them
	} c;
	obs_enum_sources(
		[](void *p, obs_source_t *src) {
			auto *col = static_cast<Collect *>(p);
			if (obs_source_is_group(src))
				return true;
			const char *type = obs_source_get_id(src);
			const QString kind = QString::fromUtf8(obs_source_get_display_name(type));
			col->byKind[kind.isEmpty() ? mt("Vertical.KindOther") : kind].append(
				{QString::fromUtf8(obs_source_get_name(src)), QString::fromUtf8(type)});
			return true;
		},
		&c);
	auto addKind = [existing](const QString &heading, Named list, bool vertical) {
		if (list.isEmpty())
			return;
		std::sort(list.begin(), list.end(),
			  [](const auto &a, const auto &b) { return a.first.compare(b.first, Qt::CaseInsensitive) < 0; });
		existing->addSection(heading);
		for (const auto &n : list) {
			const QString name = n.first;
			existing->addAction(vtheme::sourceIcon(n.second, false), name,
					    [name, vertical]() { addExisting(name, vertical); });
		}
	};
	// Scenes can be nested too: the main ones, and the other vertical ones.
	Named mains, verticals;
	for (const QString &m : mainScenes())
		mains.append({m, "scene"});
	for (const QString &v : canvas->sceneNames())
		if (v != canvas->currentScene())
			verticals.append({v, "scene"});
	addKind(mt("Vertical.MainScenes"), mains, false);
	addKind(mt("Vertical.VerticalScenes"), verticals, true);
	for (auto it = c.byKind.cbegin(); it != c.byKind.cend(); ++it)
		addKind(it.key(), it.value(), false);
	if (existing->isEmpty())
		existing->addAction(mt("Vertical.NoneYet"))->setEnabled(false);

	QMenu *fresh = menu.addMenu(mt("Vertical.NewSource"));
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
	menu.exec(at ? *at : anchor->mapToGlobal(QPoint(0, anchor->height())));
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
		const QList<qint64> ids = selectedIds();
		if (ids.size() < 2) {
			vitem::askRemove(sourcesDock, canvas, id);
		} else if (QMessageBox::question(sourcesDock, mt("Vertical.RemoveSourcesTitle"),
						 mt("Vertical.AskRemoveSources").arg(ids.size())) == QMessageBox::Yes) {
			for (qint64 one : ids) {
				QString e;
				if (!canvas->removeItem(scene, one, e) && error.isEmpty())
					error = e;
			}
			// A source inside a group that went first is gone already: not a problem.
			ok = true;
			error.clear();
		}
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
		else if (src)
			vitem::renameSource(sourcesDock, canvas, src);
		obs_source_release(src);
	}
	if (!ok)
		warn(error);
	refreshItems(true);
}

// Shows or locks several sources at once (what: "visible" or "locked").
void setMany(const QList<qint64> &ids, const char *what, bool on)
{
	const QString scene = canvas->currentScene();
	const bool visible = QString::fromLatin1(what) == "visible";
	QString error;
	for (qint64 id : ids) {
		QString e;
		if (!(visible ? canvas->setItemVisible(scene, id, on, e) : canvas->setItemLocked(scene, id, on, e)) &&
		    error.isEmpty())
			error = e;
	}
	if (!error.isEmpty())
		warn(error);
	refreshItems(true);
}

bool hasFiles(const QMimeData *m)
{
	if (m && m->hasUrls())
		for (const QUrl &url : m->urls())
			if (url.isLocalFile())
				return true;
	return false;
}

// The sources list: drag rows to reorder them; drop files from Explorer or Finder
// to add them. The list is rebuilt from the scene, so Qt never moves rows itself.
class ItemTree : public QTreeWidget {
protected:
	void dragEnterEvent(QDragEnterEvent *e) override
	{
		if (hasFiles(e->mimeData()))
			e->acceptProposedAction();
		else
			QTreeWidget::dragEnterEvent(e);
	}
	void dragMoveEvent(QDragMoveEvent *e) override
	{
		if (hasFiles(e->mimeData()))
			e->acceptProposedAction();
		else
			QTreeWidget::dragMoveEvent(e);
	}
	void dropEvent(QDropEvent *e) override
	{
		const QString scene = canvas->currentScene();
		QString error;
		if (hasFiles(e->mimeData())) {
			QStringList paths;
			for (const QUrl &url : e->mimeData()->urls())
				if (url.isLocalFile())
					paths << url.toLocalFile();
			e->acceptProposedAction();
			canvas->addFiles(scene, paths, error);
			if (!error.isEmpty())
				warn(error);
			return;
		}
		if (e->source() != this) {
			e->ignore();
			return;
		}
		// The rows, top first, with the dragged ones taken out and put back at the
		// drop point: before the row under the mouse, or after it in its lower half.
		const QPoint pos = e->position().toPoint();
		QTreeWidgetItem *over = itemAt(pos);
		while (over && over->parent())
			over = over->parent();
		const bool after = over && pos.y() > visualItemRect(over).center().y();
		QList<qint64> order, moving;
		int at = -1;
		for (int i = 0; i < topLevelItemCount(); i++) {
			QTreeWidgetItem *t = topLevelItem(i);
			if (t == over && !after && at < 0)
				at = int(order.size());
			(t->isSelected() ? moving : order) << qint64(t->data(0, Qt::UserRole).toDouble());
			if (t == over && after && at < 0)
				at = int(order.size());
		}
		e->setDropAction(Qt::IgnoreAction);
		e->accept();
		if (moving.isEmpty())
			return;
		if (at < 0)
			at = int(order.size());
		for (int i = 0; i < moving.size(); i++)
			order.insert(at + i, moving[i]);
		if (!canvas->reorderItems(scene, order, error))
			warn(error);
		QTimer::singleShot(0, []() { refreshItems(true); });
	}
};

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
	lockShowHead = headLabel(mt("Vertical.LockShow"));
	lockShowHead->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	headRow->addWidget(lockShowHead);
	layout->addLayout(headRow);

	items = new ItemTree();
	items->setObjectName("vItems");
	items->setSelectionMode(QAbstractItemView::ExtendedSelection);
	items->setDragDropMode(QAbstractItemView::InternalMove);
	items->setDefaultDropAction(Qt::MoveAction);
	items->setColumnCount(3);
	items->setHeaderLabels({mt("Vertical.ColSource"), mt("Vertical.ColLock"), mt("Vertical.ColShow")});
	items->setHeaderHidden(true);
	items->setIconSize(QSize(22, 22));
	items->setIndentation(14); // a group's sources sit under it
	items->setUniformRowHeights(true);
	items->setRootIsDecorated(false);
	items->setMinimumHeight(34); // one row; it grows with the dock
	items->header()->setStretchLastSection(false);
	items->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	items->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	items->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	items->setContextMenuPolicy(Qt::CustomContextMenu);
	items->setToolTip(mt("Vertical.ItemsTip"));
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
		QTreeWidgetItem *it = items->itemAt(pos);
		const QList<qint64> ids = selectedIds();
		if (it && it->isSelected() && ids.size() > 1) {
			// Several selected (Ctrl or Shift click): what works on all of them.
			const int n = int(ids.size());
			menu.addAction(mt("Vertical.ShowAllN").arg(n), [ids]() { setMany(ids, "visible", true); });
			menu.addAction(mt("Vertical.HideAllN").arg(n), [ids]() { setMany(ids, "visible", false); });
			menu.addAction(mt("Vertical.LockAllN").arg(n), [ids]() { setMany(ids, "locked", true); });
			menu.addAction(mt("Vertical.UnlockAllN").arg(n), [ids]() { setMany(ids, "locked", false); });
			menu.addSeparator();
			menu.addAction(mt("Vertical.RemoveNSources").arg(n), []() { itemAction("remove"); });
		} else if (it) {
			// The same menu as the Vertical Preview's.
			vitem::addItemMenu(menu, canvas, qint64(it->data(0, Qt::UserRole).toDouble()), sourcesDock);
		} else {
			menu.addAction(mt("Vertical.AddSourceMenu"), []() { addSourceMenu(items); });
			vitem::addPasteActions(menu, canvas, sourcesDock);
		}
		menu.exec(items->viewport()->mapToGlobal(pos));
	});
	// Ctrl+C / Ctrl+V copy and paste a source, Delete removes, F2 renames.
	new Keys(items, [](QKeyEvent *e, bool run) {
		const qint64 id = selectedItem();
		const QString scene = canvas->currentScene();
		QString error;
		bool ok = true;
		if (e->matches(QKeySequence::Copy) && id) {
			if (run)
				ok = canvas->copySource(scene, id, error);
		} else if (e->matches(QKeySequence::Paste) && canvas->canPasteSource()) {
			if (run)
				ok = canvas->pasteSource(scene, false, error);
		} else if (isDeleteKey(e) && id) {
			if (run)
				itemAction("remove");
		} else if (e->key() == Qt::Key_F2 && id) {
			if (run)
				itemAction("rename");
		} else {
			return false;
		}
		if (!ok)
			warn(error);
		return true;
	});
	layout->addWidget(items, 1);
	layout->addWidget(hline());

	auto *bar = new QHBoxLayout();
	bar->setContentsMargins(10, 6, 10, 8);
	bar->setSpacing(6);
	QToolButton *add = toolButton(mt("Vertical.AddSource"), mt("Vertical.AddSourceTip"), "soft");
	QToolButton *remove = iconButton("minus", mt("Vertical.RemoveSourceTip"), "round");
	QToolButton *props = iconButton("gear", mt("Vertical.PropertiesPlain"), "round");
	QToolButton *up = iconButton("up", mt("Vertical.MoveUp"), "round");
	QToolButton *down = iconButton("down", mt("Vertical.MoveDown"), "round");
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
		fitAdd(add, w->width(), mt("Vertical.AddSource"));
		props->setVisible(w->width() >= 210); // double-click opens it too
	});
	return w;
}

// ---- Vertical Mixer -----------------------------------------------------------

// The fader: 0..110 on a dB scale (-60 dB .. +6 dB), 0 = silent.
const int FADER_MAX = 110;

float faderToMul(int v)
{
	return v <= 0 ? 0.0f : std::pow(10.0f, (-60.0f + 0.6f * float(v)) / 20.0f);
}

int mulToFader(double mul)
{
	if (mul <= 0.001)
		return 0;
	return std::clamp(int(std::lround((20.0 * std::log10(mul) + 60.0) / 0.6)), 0, FADER_MAX);
}

// The fader's level, as its strip shows it.
QString faderText(int v)
{
	return v <= 0 ? QString("−∞ dB") : QString::number(-60.0 + 0.6 * v, 'f', 1) + " dB";
}

void setAudio(const QString &uuid, const QJsonObject &change);

// A source's own OBS source, by its id (new reference, or null).
obs_source_t *audioSource(const QString &uuid)
{
	return obs_get_source_by_uuid(uuid.toUtf8().constData());
}

// Right-click on a strip: what OBS's mixer offers.
void stripMenu(QWidget *strip, const QString &uuid, const QPoint &at)
{
	QMenu menu(strip);
	menu.addAction(mt("Vertical.Rename"), [uuid]() {
		if (obs_source_t *src = audioSource(uuid)) {
			vitem::renameSource(mixerDock, canvas, src);
			obs_source_release(src);
		}
	});
	menu.addAction(mt("Vertical.FiltersPlain"), [uuid]() {
		if (obs_source_t *src = audioSource(uuid)) {
			obs_frontend_open_source_filters(src);
			obs_source_release(src);
		}
	});
	menu.addAction(mt("Vertical.PropertiesPlain"), [uuid]() {
		if (obs_source_t *src = audioSource(uuid)) {
			obs_frontend_open_source_properties(src);
			obs_source_release(src);
		}
	});
	if (QAction *adv = obsAction("actionAdvAudioProperties")) {
		QAction *a = menu.addAction(mt("Vertical.AdvAudio"), [adv]() { adv->trigger(); });
		a->setToolTip(mt("Vertical.AdvAudioTip"));
	}
	menu.addSeparator();
	menu.addAction(mt("Vertical.HideFromMixer"), [uuid]() { setAudio(uuid, {{"hidden", true}}); });
	menu.exec(at);
}

QList<QPair<QString, QString>> hiddenStrips; // (uuid, name)

void showHiddenMenu(QWidget *anchor, const QPoint &at)
{
	QMenu menu(anchor);
	if (hiddenStrips.isEmpty())
		menu.addAction(mt("Vertical.NoHiddenSources"))->setEnabled(false);
	for (const auto &h : hiddenStrips) {
		const QString uuid = h.first;
		menu.addAction(mt("Vertical.ShowNamed").arg(h.second), [uuid]() { setAudio(uuid, {{"hidden", false}}); });
	}
	if (hiddenStrips.size() > 1) {
		menu.addSeparator();
		menu.addAction(mt("Vertical.ShowAll"), []() {
			for (const auto &h : QList<QPair<QString, QString>>(hiddenStrips))
				setAudio(h.first, {{"hidden", false}});
		});
	}
	menu.exec(at);
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
		       QString::number(o.value("volume").toDouble()) + (o.value("muted").toBool() ? "m" : "") +
		       (o.value("hidden").toBool() ? "h" : "") + (o.value("offAir").toBool() ? "a" : "") +
		       (o.value("monitorOnly").toBool() ? "o" : "") + "|";
	}
	auto *strips = static_cast<QHBoxLayout *>(mixerRows->layout());
	if (force || sig != shownMixer) {
		shownMixer = sig;
		while (QLayoutItem *li = strips->takeAt(0)) {
			delete li->widget();
			delete li;
		}
		hiddenStrips.clear();
		for (const QJsonValue v : arr)
			if (v.toObject().value("hidden").toBool())
				hiddenStrips.append({v.toObject().value("uuid").toString(), v.toObject().value("name").toString()});
		if (arr.size() == hiddenStrips.size()) {
			QLabel *none = roleLabel(arr.isEmpty() ? mt("Vertical.MixerEmpty")
							       : mt("Vertical.MixerAllHidden"),
						 "muted");
			none->setWordWrap(true);
			strips->addWidget(none, 1, Qt::AlignTop);
		}
		// Design 4 ("Console"): a channel strip per source, its name, level
		// meter beside its fader, then MAIN and mute. Kept tight: the fader
		// takes whatever height the dock has (fitStrips).
		for (const QJsonValue v : arr) {
			const QJsonObject o = v.toObject();
			if (o.value("hidden").toBool())
				continue;
			const QString uuid = o.value("uuid").toString();
			const QString name = o.value("name").toString();
			const bool own = o.value("mode").toString() == "own";
			const bool offAir = o.value("offAir").toBool(), monitorOnly = o.value("monitorOnly").toBool();
			auto *strip = new QFrame();
			strip->setObjectName("vStrip");
			strip->setProperty("own", own ? "true" : "false");
			strip->setFixedWidth(70);
			strip->setContextMenuPolicy(Qt::CustomContextMenu);
			QObject::connect(strip, &QWidget::customContextMenuRequested, strip, [strip, uuid](const QPoint &pos) {
				stripMenu(strip, uuid, strip->mapToGlobal(pos));
			});
			auto *sl = new QVBoxLayout(strip);
			sl->setContentsMargins(3, 4, 3, 4);
			sl->setSpacing(3);
			QLabel *label = roleLabel(QFontMetrics(vtheme::head()).elidedText(name.toUpper(), Qt::ElideRight, 62), "strip");
			label->setFont(vtheme::head());
			label->setAlignment(Qt::AlignCenter);
			label->setToolTip(offAir ? mt("Vertical.StripOffAirTip").arg(name)
					: monitorOnly ? mt("Vertical.StripMonitorTip").arg(name)
						      : name);
			if (offAir || monitorOnly)
				label->setStyleSheet("color: " + vtheme::MUTED.name());
			auto *meter = new vtheme::Meter();
			meter->setObjectName("meter:" + uuid);
			meter->setToolTip(mt("Vertical.MeterTip"));
			meter->setFixedSize(6, stripHeight);
			auto *fader = new QSlider(Qt::Vertical);
			fader->setObjectName("vFader");
			fader->setFixedSize(18, stripHeight);
			fader->setRange(0, FADER_MAX);
			fader->setValue(mulToFader(own ? o.value("volume").toDouble() : o.value("gain").toDouble()));
			fader->setEnabled(own);
			fader->setCursor(own ? Qt::PointingHandCursor : Qt::ArrowCursor);
			fader->setToolTip(mt(own ? "Vertical.VolumeTip" : "Vertical.FollowsFaderTip").arg(faderText(fader->value())));
			// Under the fader: its level, or why it's silent on the vertical stream.
			QLabel *db = roleLabel(offAir ? mt("Vertical.NotOnAir") : monitorOnly ? mt("Vertical.MonitorOnly") : faderText(fader->value()),
					       "muted");
			db->setAlignment(Qt::AlignCenter);
			QObject::connect(fader, &QSlider::valueChanged, fader, [fader, db, uuid, offAir, monitorOnly](int value) {
				fader->setToolTip(mt("Vertical.VolumeTip").arg(faderText(value)));
				if (!offAir && !monitorOnly)
					db->setText(faderText(value));
				// A click on the groove, the wheel or the arrow keys: applied now.
				// A drag is applied on release (applying it rebuilds the strip).
				if (!fader->isSliderDown())
					setAudio(uuid, {{"volume", double(faderToMul(value))}});
			});
			QObject::connect(fader, &QSlider::sliderReleased, fader, [fader, uuid]() {
				setAudio(uuid, {{"volume", double(faderToMul(fader->value()))}});
			});
			QToolButton *same = toolButton(mt("Vertical.SameAsMain"), mt("Vertical.MainTip"),
						       "key");
			same->setCheckable(true);
			same->setChecked(!own);
			QObject::connect(same, &QToolButton::toggled, same,
					 [uuid](bool on) { setAudio(uuid, {{"mode", on ? "same" : "own"}}); });
			auto *mute = new vtheme::IconToggle(vtheme::IconToggle::Kind::Speaker);
			mute->setChecked(own && o.value("muted").toBool());
			mute->setEnabled(own);
			mute->setToolTip(mt("Vertical.MuteTip"));
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
			sl->addWidget(db);
			sl->addLayout(keys);
			strips->addWidget(strip, 0, Qt::AlignTop);
		}
		if (!hiddenStrips.isEmpty() && arr.size() != hiddenStrips.size()) {
			QToolButton *more = toolButton(mt("Vertical.HiddenCount").arg(hiddenStrips.size()), mt("Vertical.HiddenTip"),
						       "soft");
			QObject::connect(more, &QToolButton::clicked, more,
					 [more]() { showHiddenMenu(more, more->mapToGlobal(QPoint(0, more->height()))); });
			strips->addWidget(more, 0, Qt::AlignTop);
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
	scroll->setToolTip(mt("Vertical.MixerTip"));
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
	mixerRows->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(mixerRows, &QWidget::customContextMenuRequested, mixerRows,
			 [](const QPoint &pos) { showHiddenMenu(mixerRows, mixerRows->mapToGlobal(pos)); });
	new Fit(scroll->viewport(), []() { fitStrips(); });
	return w;
}

// ---- Vertical Preview -----------------------------------------------------------

void refreshRecord();

// "1080×1920", and the zoom when the preview isn't at "fit".
void showSize()
{
	if (!sizeLabel)
		return;
	QString text = canvas->ready() ? QString("%1×%2").arg(canvas->width()).arg(canvas->height()) : QString();
	if (!text.isEmpty() && zoomPercent != 100)
		text += QString(" · %1%").arg(zoomPercent);
	sizeLabel->setText(text);
	sizeLabel->setToolTip(mt(zoomPercent != 100 ? "Vertical.SizeZoomed" : "Vertical.SizeTip"));
}

void setPreviewOn(bool on)
{
	if (previewStack)
		previewStack->setCurrentIndex(on ? 0 : 1);
	canvas->setUiInt("previewOff", on ? 0 : 1);
}

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
	preview->hint = mt("Vertical.PreviewTip");
	preview->setToolTip(preview->hint);
	preview->onSelect = [](qint64 id) { selectItem(id); };
	preview->onAddSource = [](QWidget *anchor, const QPoint &at) { addSourceMenu(anchor, &at); };
	preview->onZoom = [](int percent) {
		zoomPercent = percent;
		showSize();
	};
	preview->onTurnOff = []() { setPreviewOn(false); };
	// Turned off from its right-click menu: saves a little computer power.
	auto *off = new QWidget();
	auto *ol = new QVBoxLayout(off);
	ol->addStretch(1);
	QLabel *offText = roleLabel(mt("Vertical.PreviewOff"), "muted");
	offText->setWordWrap(true);
	offText->setAlignment(Qt::AlignCenter);
	ol->addWidget(offText);
	auto *turnOn = new QPushButton(mt("Vertical.TurnOnPreview"));
	turnOn->setProperty("pill", "soft");
	QObject::connect(turnOn, &QPushButton::clicked, turnOn, []() { setPreviewOn(true); });
	ol->addWidget(turnOn, 0, Qt::AlignCenter);
	ol->addStretch(1);
	previewStack = new QStackedWidget();
	previewStack->setMinimumSize(60, 40);
	previewStack->addWidget(preview);
	previewStack->addWidget(off);
	previewStack->setCurrentIndex(canvas->uiInt("previewOff", 0) ? 1 : 0);
	layout->addWidget(previewStack, 1);

	// Recording and replay buffer of the vertical picture.
	auto *bar = new QHBoxLayout();
	bar->setSpacing(6);
	recButton = toolButton(mt("Vertical.Rec"), QString(), "rec");
	recButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	recButton->setIconSize(QSize(8, 8));
	recButton->setCheckable(true);
	QObject::connect(recButton, &QToolButton::clicked, recButton, [](bool on) {
		QString error;
		if (!on)
			canvas->recorder->stopRecording();
		else if (!canvas->recorder->startRecording(error)) {
			shownRecError = error;
			warn(error);
		}
	});
	replayButton = toolButton(mt("Vertical.Replay"), QString(), "ghost");
	replayButton->setCheckable(true);
	QObject::connect(replayButton, &QToolButton::clicked, replayButton, [](bool on) {
		QString error;
		if (!on)
			canvas->recorder->stopReplay();
		else if (!canvas->recorder->startReplay(error)) {
			shownRecError = error;
			warn(error);
		}
	});
	pauseButton = toolButton(mt("Vertical.Pause"), QString(), "ghost");
	pauseButton->setVisible(false); // while recording
	QObject::connect(pauseButton, &QToolButton::clicked, pauseButton, []() {
		QString error;
		if (!canvas->recorder->setPaused(!canvas->recorder->paused(), error))
			warn(error);
		refreshRecord();
	});
	saveButton = toolButton(mt("Vertical.SaveClip"), mt("Vertical.SaveClipTip"), "ghost");
	QObject::connect(saveButton, &QToolButton::clicked, saveButton, []() {
		QString error;
		if (!canvas->recorder->saveReplay(error))
			warn(error);
	});
	QToolButton *more = iconButton("more", mt("Vertical.MoreTip"), "round");
	QObject::connect(more, &QToolButton::clicked, more, [more]() {
		VerticalRecorder *r = canvas->recorder;
		const QJsonObject st = r->state();
		QMenu menu(more);
		QAction *followMain = menu.addAction(mt("Vertical.FollowObsButtons"));
		followMain->setCheckable(true);
		followMain->setChecked(st.value("followMain").toBool());
		QObject::connect(followMain, &QAction::toggled, [r](bool on) { r->configure({{"followMain", on}}); });
		QMenu *fmt = menu.addMenu(mt("Vertical.FileType"));
		for (const char *f : {"mp4", "mkv"}) {
			const QString v = QString::fromLatin1(f);
			QAction *a = fmt->addAction(v.toUpper());
			a->setCheckable(true);
			a->setChecked(st.value("format").toString() == v);
			QObject::connect(a, &QAction::triggered, [r, v]() { r->configure({{"format", v}}); });
		}
		// The picture quality: a bitrate, in plain words.
		QMenu *quality = menu.addMenu(mt(r->recording() || r->replayActive() ? "Vertical.QualityBusy" : "Vertical.Quality"));
		const QPair<const char *, int> levels[] = {{"Vertical.QualityStandard", 6000},
							   {"Vertical.QualityHigh", 12000},
							   {"Vertical.QualityMain", 0}};
		for (const auto &l : levels) {
			const int kbps = l.second;
			QAction *a = quality->addAction(mt(l.first));
			a->setCheckable(true);
			a->setChecked(st.value("bitrate").toInt() == kbps);
			a->setEnabled(!r->recording() && !r->replayActive());
			QObject::connect(a, &QAction::triggered, [r, kbps]() { r->configure({{"bitrate", kbps}}); });
		}
		QMenu *len = menu.addMenu(mt(r->replayActive() ? "Vertical.ClipLengthBusy" : "Vertical.ClipLength"));
		for (int s : {15, 30, 60, 120, 300}) {
			QAction *a = len->addAction(s < 60 ? mt("Vertical.Seconds").arg(s)
							    : mt(s >= 120 ? "Vertical.MinutesOther" : "Vertical.MinutesOne").arg(s / 60));
			a->setCheckable(true);
			a->setChecked(st.value("replaySeconds").toInt() == s);
			a->setEnabled(!r->replayActive());
			QObject::connect(a, &QAction::triggered, [r, s]() { r->configure({{"replaySeconds", s}}); });
		}
		const QString dir = st.value("folder").toString();
		menu.addAction(mt("Vertical.OpenRecordings"), [dir]() { QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); });
		menu.addSeparator();
		const QJsonObject df = canvas->dualFormatState();
		QAction *dual = menu.addAction(mt("Vertical.SendDual"));
		dual->setCheckable(true);
		dual->setChecked(df.value("on").toBool());
		QObject::connect(dual, &QAction::toggled, [](bool on) {
			QString error;
			if (!canvas->setDualFormat(on, error))
				warn(error);
			else if (on && !canvas->dualFormatState().value("enhanced").toBool())
				QMessageBox::information(scenesDock, QStringLiteral("Multistream Vertical"), mt("Vertical.EnhancedNeeded"));
		});
		menu.exec(more->mapToGlobal(QPoint(0, more->height())));
	});
	// Never squeezed until the words go: a narrow dock gets shorter words.
	for (QToolButton *b : {recButton.data(), replayButton.data(), saveButton.data()})
		b->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	bar->addWidget(recButton);
	bar->addWidget(pauseButton);
	bar->addWidget(replayButton);
	bar->addWidget(saveButton);
	bar->addStretch(1);
	recLabel = roleLabel(QString(), "warn");
	bar->addWidget(recLabel);
	noticeLabel = roleLabel(QString(), "muted");
	noticeLabel->setTextFormat(Qt::RichText);
	noticeLabel->setVisible(false);
	QObject::connect(noticeLabel, &QLabel::linkActivated, noticeLabel, [](const QString &dir) {
		QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
	});
	bar->addWidget(noticeLabel);
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
	recButton->setText(rec ? mt("Vertical.Stop") : mt("Vertical.Rec"));
	if (recButton->property("recIcon").toBool() != rec || recButton->icon().isNull()) {
		recButton->setIcon(vtheme::dotIcon(rec ? vtheme::ROSE_TEXT : vtheme::ROSE, rec));
		recButton->setProperty("recIcon", rec);
	}
	recButton->setToolTip(rec ? mt("Vertical.RecordingTo").arg(st.value("lastFile").toString())
				  : mt("Vertical.RecordInto").arg(st.value("folder").toString()));
	replayButton->setChecked(rep);
	replayButton->setText(rep && recordWords == 2 ? mt("Vertical.ReplayOn") : mt("Vertical.Replay"));
	saveButton->setText(recordWords == 2 ? mt("Vertical.SaveClip") : mt("Vertical.Save"));
	replayButton->setToolTip(rep ? mt("Vertical.ReplayRunningTip") : mt("Vertical.ReplayStartTip"));
	saveButton->setEnabled(rep);
	const bool paused = st.value("paused").toBool();
	pauseButton->setVisible(rec);
	pauseButton->setText(recordWords == 2 ? mt(paused ? "Vertical.Resume" : "Vertical.Pause") : (paused ? "\u25b6" : "\u275a\u275a"));
	pauseButton->setToolTip(mt(paused ? "Vertical.PausedTip" : "Vertical.PauseTip"));
	if (paused)
		recButton->setToolTip(mt("Vertical.PausedFile").arg(st.value("lastFile").toString()));
	// Something just happened: say so for a few seconds (a clip saved, a hotkey
	// that didn't work, a collection switch).
	if (const int seq = st.value("noticeSeq").toInt(); seq != shownNotice) {
		shownNotice = seq;
		const QString file = st.value("noticeFile").toString();
		QString text = st.value("notice").toString().toHtmlEscaped();
		if (!file.isEmpty())
			text += QString(" · <a href=\"%1\">%2</a>").arg(QFileInfo(file).absolutePath().toHtmlEscaped(), mt("Vertical.OpenFolder").toHtmlEscaped());
		noticeLabel->setText(text);
		noticeLabel->setToolTip(file.isEmpty() ? st.value("notice").toString() : file);
		noticeLabel->setVisible(true);
		QTimer::singleShot(file.isEmpty() ? 12000 : 8000, noticeLabel, [seq]() {
			if (noticeLabel && shownNotice == seq)
				noticeLabel->setVisible(false);
		});
	}
	const QString err = st.value("error").toString();
	recLabel->setText(err.isEmpty() || rec || rep ? QString() : mt("Vertical.NotRecording"));
	recLabel->setToolTip(err);
	if (err.isEmpty())
		shownRecError.clear();
	else if (err != shownRecError) {
		// Started from OBS's own Record button: say so, or it looks like it worked.
		shownRecError = err;
		auto *box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Multistream Vertical"),
					    mt("Vertical.RecCouldnt").arg(err), QMessageBox::Ok, scenesDock);
		box->setAttribute(Qt::WA_DeleteOnClose);
		box->setModal(false);
		box->show();
	}
}

// The pill over the preview: is the vertical stream going to Multistream?
void refreshLive()
{
	if (!liveLabel || !liveState)
		return;
	const QJsonObject st = liveState();
	const QString error = st.value("error").toString();
	QString text = mt("Vertical.NotLive"), tone = "off", tip = mt("Vertical.NotLiveTip");
	if (st.value("reconnecting").toBool()) {
		text = mt("Vertical.Reconnecting");
		tone = "busy";
		tip = mt("Vertical.ReconnectingTip");
	} else if (st.value("sending").toBool()) {
		text = mt("Vertical.Live");
		tone = "live";
		tip = mt("Vertical.LiveTip");
	} else if (st.value("connecting").toBool()) {
		text = mt("Vertical.Connecting");
		tone = "busy";
		tip = mt("Vertical.ConnectingTip");
	} else if (canvas->dualFormatState().value("on").toBool()) {
		// Twitch Dual Format sends the vertical picture with OBS's own stream.
		const bool streaming = obs_frontend_streaming_active();
		text = streaming ? mt("Vertical.TwitchLive") : mt("Vertical.TwitchDual");
		tone = streaming ? "live" : "off";
		tip = streaming ? mt("Vertical.TwitchLiveTip") : mt("Vertical.TwitchDualTip");
	} else if (!error.isEmpty()) {
		// The reason in the pill itself, cut short; all of it in the tooltip.
		text = QFontMetrics(liveLabel->font()).elidedText(mt("Vertical.ProblemText").arg(error), Qt::ElideRight, 260);
		tone = "bad";
		tip = error;
	} else if (!st.value("enabled").toBool()) {
		text = mt("Vertical.Off");
		tip = mt("Vertical.OffTip");
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
			sceneLabel->setText(canvas->ready()             ? canvas->currentScene()
					    : canvas->waitsForOutputs() ? mt("Vertical.WaitsForOutputs")
									: mt("Vertical.Starting"));
			sceneLabel->setToolTip(canvas->ready() ? mt("Vertical.ShowingScene").arg(canvas->currentScene())
					       : canvas->waitsForOutputs() ? mt("Vertical.WaitsTip") : QString());
			showSize();
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
	addDock("multistream-vertical-scenes", obs_module_text("Vertical.DockScenes"), scenesDock);
	sourcesDock = makeSourcesDock();
	addDock("multistream-vertical-sources", obs_module_text("Vertical.DockSources"), sourcesDock);
	previewDock = makePreviewDock();
	addDock("multistream-vertical-preview", obs_module_text("Vertical.DockPreview"), previewDock);
	mixerDock = makeMixerDock();
	addDock("multistream-vertical-mixer", obs_module_text("Vertical.DockMixer"), mixerDock);
	for (QWidget *root : {scenesDock.data(), sourcesDock.data(), previewDock.data(), mixerDock.data()})
		undoKeys(root);

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
// put them on the right of OBS's window, where a new streamer sees them,
// as tabs (Preview, Scenes, Sources, Mixer) under the Multistream dock
// rather than a column of five squeezed panels. Each dock has its own
// flag, so docks added in later versions show too, and docks someone has
// already placed are never moved.
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
	bool saved = false, previewPlaced = false;
	QDockWidget *previewDw = previewDock ? qobject_cast<QDockWidget *>(previewDock->parentWidget()) : nullptr;
	for (QWidget *content : {previewDock.data(), scenesDock.data(), sourcesDock.data(), mixerDock.data()}) {
		auto *dw = content ? qobject_cast<QDockWidget *>(content->parentWidget()) : nullptr;
		const QByteArray key = content ? content->objectName().toUtf8() : QByteArray();
		if (!dw || !main || obs_data_get_bool(d, key.constData()))
			continue;
		dw->setFloating(false);
		if (dw != previewDw && previewDw && !previewDw->isFloating() && !previewDw->isHidden()) {
			// Wherever the preview is, a new dock joins it as a tab.
			main->tabifyDockWidget(previewDw, dw);
		} else {
			main->addDockWidget(Qt::RightDockWidgetArea, dw);
		}
		dw->show();
		if (dw == previewDw)
			previewPlaced = true;
		obs_data_set_bool(d, key.constData(), true);
		saved = true;
	}
	// A first run: the picture is the tab in front, the tabs get 60% of the
	// column (OBS would give the Multistream dock above most of it), and
	// the column widens to 500 (at most 2/5 of the window), so the four
	// tab names read in full rather than "Scenes ...", which looks like
	// OBS's own Scenes. resizeDocks does nothing in OBS's window, so once it
	// is on screen the preview briefly asks for that size as its minimum
	// and the docks around it give way; then it lets go, keeping the size.
	if (previewPlaced && previewDw && main) {
		QTimer::singleShot(0, previewDw, [previewDw]() { previewDw->raise(); });
		QPointer<QDockWidget> pv = previewDw;
		QPointer<QWidget> content = previewDock.data();
		QTimer::singleShot(1500, main, [main, pv, content]() {
			QPointer<QDockWidget> ms = main->findChild<QDockWidget *>("multistream");
			if (!pv || !content || !ms || ms->isFloating() || !ms->isVisible() || pv->isFloating() ||
			    !pv->isVisible() || main->dockWidgetArea(ms) != main->dockWidgetArea(pv))
				return;
			const int h = ms->height() + pv->height();
			if (h < 300)
				return;
			const QSize before = content->minimumSize();
			const int chrome = pv->height() - content->height();
			content->setMinimumHeight(std::max(before.height(), h * 3 / 5 - chrome));
			const int w = std::min(500, main->width() * 2 / 5);
			if (w > pv->width())
				content->setMinimumWidth(std::max(before.width(), w - (pv->width() - content->width())));
			QTimer::singleShot(800, content, [content, before, pv, ms]() {
				if (content)
					content->setMinimumSize(before);
				obs_log(LOG_INFO, "Vertical docks: Multistream %d, tabs %dx%d", ms ? ms->height() : -1,
					pv ? pv->width() : -1, pv ? pv->height() : -1);
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
	PreviewWidget::closeProjectors();
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
