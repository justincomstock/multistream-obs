#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QString>
#include <QStyledItemDelegate>
#include <QWidget>

// The vertical docks in the Multistream dock's look (cloud/web/obs-dock,
// "Signal strip"): near-black ground, violet for what's on, rose for live,
// pill buttons, small spaced capitals over each list. The owner picked one
// design per dock: Preview and Sources as the dock itself, Scenes in cards,
// the Mixer as console strips. Everything is a stylesheet on each dock's own
// widget (OBS's dialogs and menus keep OBS's look) plus a few painted
// controls. No Q_OBJECT here: the plugin builds without moc.
namespace vtheme {

const QColor BG(0x0e, 0x0e, 0x13);
const QColor INK(0xe8, 0xe7, 0xf2);
const QColor SOFT(0xc9, 0xc8, 0xdc);
const QColor MUTED(0x8a, 0x89, 0xa0);
const QColor OFF(0x55, 0x54, 0x6a);
const QColor VIOLET(0xa9, 0x9c, 0xff);
const QColor ROSE(0xff, 0x4d, 0x6d);
const QColor ROSE_TEXT(0xff, 0x7a, 0x92);
const QColor GREEN(0x4f, 0xe0, 0xa0);
const QColor AMBER(0xff, 0xb5, 0x47);

// The stylesheet each dock's root widget gets.
QString style();
// Cascadia Mono (Consolas when missing), in pixels; spacing in pixels too.
QFont mono(int px, bool bold = false, qreal spacing = 0);
// The Signal strip's spaced capitals over a list.
QFont head();
// A source's icon by its type id: a glyph on a rounded tile.
QIcon sourceIcon(const QString &type, bool group);
// The small buttons' icons, drawn (text arrows come out as colour emoji):
// "minus", "up", "down", "gear", "more", "replay", "save".
QIcon lineIcon(const QString &name);
// A dot (or a square, for Stop) for the record button.
QIcon dotIcon(const QColor &c, bool square = false);
// Restyle after a dynamic property the stylesheet reads ("tone", "pill").
void setProp(QWidget *w, const char *name, const QString &value);

// The violet on/off switch (30x18).
class Switch : public QAbstractButton {
public:
	explicit Switch(QWidget *parent = nullptr);
	QSize sizeHint() const override { return {30, 18}; }

protected:
	void paintEvent(QPaintEvent *e) override;
};

// A small square toggle with a painted glyph: the lock (amber when locked)
// or the speaker (rose with a cross when muted).
class IconToggle : public QAbstractButton {
public:
	enum class Kind { Lock, Speaker };
	explicit IconToggle(Kind kind, QWidget *parent = nullptr);
	QSize sizeHint() const override { return {22, 22}; }

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	Kind kind_;
};

// A segmented level meter: 0..100 on the faders' dB scale. Tall in a mixer
// strip; wider than tall, it fills left to right (a mixer row).
class Meter : public QWidget {
public:
	explicit Meter(QWidget *parent = nullptr);
	void setLevel(int level);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	int level_ = 0;
};

// Vertical Scenes rows: a dot, the name, and "ON AIR" or the main scenes it
// shows with. The item's text is its name; the roles below carry the rest.
class SceneDelegate : public QStyledItemDelegate {
public:
	static constexpr int OnAirRole = Qt::UserRole + 1;
	static constexpr int MainsRole = Qt::UserRole + 2;
	using QStyledItemDelegate::QStyledItemDelegate;
	void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
	QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
};

} // namespace vtheme
