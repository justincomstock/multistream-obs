#include "docks.hpp"

#include "canvas.hpp"
#include "preview.hpp"
#include "record.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QCheckBox>
#include <QDesktopServices>
#include <QDockWidget>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
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
QPointer<QCheckBox> follow;
QPointer<QLabel> previewLabel;
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

QToolButton *toolButton(const QString &text, const QString &tip)
{
	auto *b = new QToolButton();
	b->setText(text);
	b->setToolTip(tip);
	b->setAutoRaise(true);
	return b;
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
		auto *it = new QListWidgetItem(mains.isEmpty() ? name : QString("%1   ← %2").arg(name, mains.join(", ")));
		it->setData(Qt::UserRole, name);
		it->setToolTip(mains.isEmpty() ? QString("Shows when the main scene called \"%1\" is on air, if there is one.").arg(name)
					       : "Shows when these main scenes are on air: " + mains.join(", "));
		if (s.value("current").toBool()) {
			QFont f = it->font();
			f.setBold(true);
			it->setFont(f);
		}
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
	auto *w = new QWidget();
	w->setObjectName("multistreamVerticalScenes");
	w->setMinimumSize(180, 160);
	auto *layout = new QVBoxLayout(w);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	list = new QListWidget();
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

	auto *bar = new QHBoxLayout();
	bar->setSpacing(2);
	QToolButton *add = toolButton("+", "Add a vertical scene: empty, or laid out from a main scene");
	QToolButton *remove = toolButton("−", "Remove the selected vertical scene");
	QToolButton *up = toolButton("↑", "Move up");
	QToolButton *down = toolButton("↓", "Move down");
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
	for (QToolButton *b : {add, remove, up, down})
		bar->addWidget(b);
	bar->addStretch(1);
	layout->addLayout(bar);

	follow = new QCheckBox("Follow the main scenes");
	follow->setToolTip("When a main scene goes on air, show the vertical scene it's linked to "
			   "(right-click a vertical scene) or the one with the same name.");
	QObject::connect(follow, &QCheckBox::toggled, follow, [](bool on) { canvas->setFollowMain(on); });
	layout->addWidget(follow);

	// Moving in from Aitum Vertical or the Vertical tool (Lua script).
	importBox = new QWidget();
	auto *ib = new QVBoxLayout(importBox);
	ib->setContentsMargins(0, 4, 0, 0);
	importText = new QLabel();
	importText->setWordWrap(true);
	ib->addWidget(importText);
	auto *ibar = new QHBoxLayout();
	importAitumButton = new QPushButton("Import Aitum's scenes");
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
		itemsTitle->setText(scene.isEmpty() ? QString("No vertical scene") : "In \"" + scene + "\"");
	const qint64 keep = preview ? preview->selected() : selectedItem();
	items->blockSignals(true);
	items->clear();
	for (const QJsonValue v : arr) {
		const QJsonObject o = v.toObject();
		auto *it = new QTreeWidgetItem();
		const QString kind = o.value("group").toBool() ? " (group)" : QString();
		it->setText(0, o.value("name").toString() + kind);
		it->setData(0, Qt::UserRole, o.value("id").toDouble());
		it->setToolTip(0, QString::fromUtf8(obs_source_get_display_name(o.value("type").toString().toUtf8().constData())));
		it->setCheckState(1, o.value("visible").toBool() ? Qt::Checked : Qt::Unchecked);
		it->setToolTip(1, "Show / hide in this vertical scene");
		it->setCheckState(2, o.value("locked").toBool() ? Qt::Checked : Qt::Unchecked);
		it->setToolTip(2, "Lock (can't be moved in the preview)");
		items->addTopLevelItem(it);
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
	auto *w = new QWidget();
	w->setObjectName("multistreamVerticalSources");
	w->setMinimumSize(200, 160);
	auto *layout = new QVBoxLayout(w);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);
	itemsTitle = new QLabel();
	layout->addWidget(itemsTitle);

	items = new QTreeWidget();
	items->setColumnCount(3);
	items->setHeaderLabels({"Source", "Show", "Lock"});
	items->setRootIsDecorated(false);
	items->header()->setStretchLastSection(false);
	items->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	items->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	items->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	items->setContextMenuPolicy(Qt::CustomContextMenu);
	items->setToolTip("The sources in the vertical scene on air, top first. Tick to show; lock to keep in place.");
	QObject::connect(items, &QTreeWidget::currentItemChanged, items, [](QTreeWidgetItem *it, QTreeWidgetItem *) {
		if (preview)
			preview->select(it ? qint64(it->data(0, Qt::UserRole).toDouble()) : 0);
	});
	QObject::connect(items, &QTreeWidget::itemChanged, items, [](QTreeWidgetItem *it, int column) {
		const qint64 id = qint64(it->data(0, Qt::UserRole).toDouble());
		const bool on = it->checkState(column) == Qt::Checked;
		QString error;
		bool ok = true;
		if (column == 1)
			ok = canvas->setItemVisible(canvas->currentScene(), id, on, error);
		else if (column == 2)
			ok = canvas->setItemLocked(canvas->currentScene(), id, on, error);
		if (!ok)
			warn(error);
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

	auto *bar = new QHBoxLayout();
	bar->setSpacing(2);
	QToolButton *add = toolButton("+", "Add a source: one OBS already has, or a new one");
	QToolButton *remove = toolButton("−", "Remove the selected source from this vertical scene");
	QToolButton *props = toolButton("⚙", "Properties");
	QToolButton *up = toolButton("↑", "Move up");
	QToolButton *down = toolButton("↓", "Move down");
	QObject::connect(add, &QToolButton::clicked, add, [add]() { addSourceMenu(add); });
	QObject::connect(remove, &QToolButton::clicked, remove, []() { itemAction("remove"); });
	QObject::connect(props, &QToolButton::clicked, props, []() { itemAction("properties"); });
	QObject::connect(up, &QToolButton::clicked, up, []() { itemAction("up"); });
	QObject::connect(down, &QToolButton::clicked, down, []() { itemAction("down"); });
	for (QToolButton *b : {add, remove, props, up, down})
		bar->addWidget(b);
	bar->addStretch(1);
	layout->addLayout(bar);
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
	auto *grid = static_cast<QGridLayout *>(mixerRows->layout());
	if (force || sig != shownMixer) {
		shownMixer = sig;
		while (QLayoutItem *li = grid->takeAt(0)) {
			delete li->widget();
			delete li;
		}
		if (arr.isEmpty()) {
			auto *none = new QLabel("No sound in the vertical scene on air.");
			none->setWordWrap(true);
			grid->addWidget(none, 0, 0, 1, 5);
		}
		int row = 0;
		for (const QJsonValue v : arr) {
			const QJsonObject o = v.toObject();
			const QString uuid = o.value("uuid").toString();
			const bool own = o.value("mode").toString() == "own";
			auto *name = new QLabel(o.value("name").toString());
			name->setToolTip(o.value("name").toString());
			auto *meter = new QProgressBar();
			meter->setObjectName("meter:" + uuid);
			meter->setRange(0, 100);
			meter->setTextVisible(false);
			meter->setMaximumHeight(8);
			meter->setToolTip("What the vertical stream hears from it");
			auto *fader = new QSlider(Qt::Horizontal);
			fader->setRange(0, 100);
			fader->setValue(mulToFader(own ? o.value("volume").toDouble() : o.value("gain").toDouble()));
			fader->setEnabled(own);
			fader->setToolTip(own ? "Vertical volume" : "Follows OBS's own fader (untick \"Same as main\")");
			QObject::connect(fader, &QSlider::sliderReleased, fader, [fader, uuid]() {
				setAudio(uuid, {{"volume", double(faderToMul(fader->value()))}});
			});
			auto *mute = new QToolButton();
			mute->setCheckable(true);
			mute->setChecked(own && o.value("muted").toBool());
			mute->setEnabled(own);
			mute->setText(mute->isChecked() ? "\U0001F507" : "\U0001F50A");
			mute->setToolTip("Mute on the vertical stream only");
			QObject::connect(mute, &QToolButton::toggled, mute,
					 [uuid](bool on) { setAudio(uuid, {{"muted", on}}); });
			auto *same = new QCheckBox("Same as main");
			same->setChecked(!own);
			same->setToolTip("Ticked: the vertical stream uses OBS's own fader and mute for it.\n"
					 "Unticked: its own vertical volume and mute.");
			QObject::connect(same, &QCheckBox::toggled, same,
					 [uuid](bool on) { setAudio(uuid, {{"mode", on ? "same" : "own"}}); });
			name->setMinimumWidth(0);
			grid->addWidget(name, row, 0);
			grid->addWidget(same, row, 1);
			grid->addWidget(mute, row, 2);
			grid->addWidget(fader, row + 1, 0, 1, 3);
			grid->addWidget(meter, row + 2, 0, 1, 3);
			// A gap before the next source.
			grid->setRowMinimumHeight(row + 3, 8);
			row += 4;
		}
		grid->setRowStretch(row, 1);
	}
	// The meters: peak of the last block, on the same dB scale.
	for (const QJsonValue v : arr) {
		const QJsonObject o = v.toObject();
		auto *meter = mixerRows->findChild<QProgressBar *>("meter:" + o.value("uuid").toString());
		const double peak = o.value("peak").toDouble();
		if (meter)
			meter->setValue(peak <= 0.001 ? 0 : std::clamp(int((20.0 * std::log10(peak) + 60.0) / 0.6), 0, 100));
	}
}

QWidget *makeMixerDock()
{
	auto *w = new QWidget();
	w->setObjectName("multistreamVerticalMixer");
	w->setMinimumSize(220, 120);
	auto *layout = new QVBoxLayout(w);
	layout->setContentsMargins(4, 4, 4, 4);
	auto *hint = new QLabel("What the vertical stream hears.");
	hint->setToolTip("Only sources in the vertical scene on air play here, plus Desktop Audio and Mic.\n"
			 "Untick \"Same as main\" to give one its own vertical volume and mute.");
	hint->setWordWrap(true);
	layout->addWidget(hint);
	auto *scroll = new QScrollArea();
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	mixerRows = new QWidget();
	auto *grid = new QGridLayout(mixerRows);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setVerticalSpacing(2);
	grid->setColumnStretch(0, 1);
	scroll->setWidget(mixerRows);
	layout->addWidget(scroll, 1);
	return w;
}

// ---- Vertical Preview -----------------------------------------------------------

QWidget *makePreviewDock()
{
	auto *w = new QWidget();
	w->setObjectName("multistreamVerticalPreview");
	w->setMinimumSize(160, 240);
	auto *layout = new QVBoxLayout(w);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);
	preview = new PreviewWidget(canvas);
	preview->setToolTip("Drag to move, corners to resize (Alt: crop), Ctrl+wheel to zoom, right-click for more.");
	preview->onSelect = [](qint64 id) { selectItem(id); };
	layout->addWidget(preview, 1);
	previewLabel = new QLabel();
	previewLabel->setAlignment(Qt::AlignCenter);
	layout->addWidget(previewLabel);

	// Recording and replay buffer of the vertical picture.
	auto *bar = new QHBoxLayout();
	bar->setSpacing(4);
	recButton = new QToolButton();
	recButton->setCheckable(true);
	QObject::connect(recButton, &QToolButton::clicked, recButton, [](bool on) {
		QString error;
		if (!on)
			canvas->recorder->stopRecording();
		else if (!canvas->recorder->startRecording(error))
			warn(error);
	});
	replayButton = new QToolButton();
	replayButton->setCheckable(true);
	QObject::connect(replayButton, &QToolButton::clicked, replayButton, [](bool on) {
		QString error;
		if (!on)
			canvas->recorder->stopReplay();
		else if (!canvas->recorder->startReplay(error))
			warn(error);
	});
	saveButton = toolButton("Save", "Save a clip of the last seconds of the vertical picture");
	QObject::connect(saveButton, &QToolButton::clicked, saveButton, []() {
		QString error;
		if (!canvas->recorder->saveReplay(error))
			warn(error);
	});
	QToolButton *more = toolButton("\u22ef", "Recording options, Twitch Dual Format");
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
	for (QToolButton *b : {recButton.data(), replayButton.data(), saveButton.data()})
		b->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	bar->addWidget(recButton);
	bar->addWidget(replayButton);
	bar->addWidget(saveButton);
	bar->addStretch(1);
	recLabel = new QLabel();
	bar->addWidget(recLabel);
	bar->addWidget(more);
	layout->addLayout(bar);
	return w;
}

void refreshRecord()
{
	if (!recButton || !canvas->recorder)
		return;
	const QJsonObject st = canvas->recorder->state();
	const bool rec = st.value("recording").toBool(), rep = st.value("replay").toBool();
	recButton->setChecked(rec);
	recButton->setText(rec ? "\u25a0 Stop" : "\u25cf Rec");
	recButton->setToolTip(rec ? "Recording to " + st.value("lastFile").toString()
				  : "Record the vertical picture into " + st.value("folder").toString());
	replayButton->setChecked(rep);
	replayButton->setText(rep ? "Replay \u2713" : "Replay");
	replayButton->setToolTip(rep ? "The vertical replay buffer is running: Save clip keeps the last seconds."
				     : "Start the vertical replay buffer");
	saveButton->setEnabled(rep);
	const QString err = st.value("error").toString();
	recLabel->setText(err.isEmpty() ? QString() : "\u26a0");
	recLabel->setToolTip(err);
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
		if (previewLabel)
			previewLabel->setText(canvas->ready() ? QString("On air: %1  ·  %2×%3")
									.arg(canvas->currentScene())
									.arg(canvas->width())
									.arg(canvas->height())
							      : QString("The vertical canvas is starting…"));
	}
	refreshItems(changed);
	refreshRecord();
}

void addDock(const char *id, const char *title, QWidget *w)
{
	if (!obs_frontend_add_dock_by_id(id, title, w))
		obs_log(LOG_WARNING, "vertical: OBS already has a dock called %s", id);
}

} // namespace

namespace vertical_docks {

void create(VerticalCanvas *c)
{
	canvas = c;
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

} // namespace vertical_docks
