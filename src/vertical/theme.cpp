#include "theme.hpp"
#include "../i18n.hpp"

#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyle>

#include <cmath>

namespace vtheme {

QString style()
{
	return QStringLiteral(R"(
QWidget#multistreamVerticalPreview, QWidget#multistreamVerticalSources, QWidget#multistreamVerticalMixer { background: #0e0e13; }
QWidget#multistreamVerticalScenes { background: #0b0b0f; }

QLabel[role="head"] { color: #8a89a0; background: transparent; }
QLabel[role="title"] { color: #e8e7f2; background: transparent; font-size: 12px; font-weight: 600; }
QLabel[role="muted"] { color: #8a89a0; background: transparent; font-size: 11px; }
QLabel[role="note"] { color: #a3a2b8; background: transparent; font-size: 12px; }
QLabel[role="strip"], QLabel[role="db"], QLabel[role="mono"] { color: #8a89a0; background: transparent; }
QLabel[role="head"], QLabel[role="strip"] { font-family: "Cascadia Mono", Consolas; font-size: 10px; }
QLabel[role="db"], QLabel[role="mono"] { font-family: "Cascadia Mono", Consolas; font-size: 11px; }
QLabel[role="warn"] { color: #ffb547; background: transparent; font-size: 14px; }
QLabel[tone="live"], QLabel[tone="busy"], QLabel[tone="off"], QLabel[tone="bad"] { border-radius: 9px; padding: 2px 8px; font-size: 10px; font-weight: 700; }
QLabel[tone="live"] { background: rgba(255,77,109,41); color: #ff7a92; }
QLabel[tone="busy"] { background: rgba(169,156,255,31); color: #a99cff; }
QLabel[tone="off"] { background: rgba(255,255,255,15); color: #8a89a0; }
QLabel[tone="bad"] { background: rgba(255,181,71,31); color: #ffb547; }
QFrame[role="line"] { background: rgba(255,255,255,15); border: 0; min-height: 1px; max-height: 1px; }

QToolButton[pill="ghost"], QToolButton[pill="rec"], QToolButton[pill="soft"], QPushButton[pill="soft"], QToolButton[pill="round"], QToolButton[pill="square"], QToolButton[pill="key"] { border: 0; color: #c9c8dc; font-size: 11px; font-weight: 700; }
QToolButton[pill="ghost"]:disabled, QToolButton[pill="rec"]:disabled, QToolButton[pill="soft"]:disabled, QPushButton[pill="soft"]:disabled, QToolButton[pill="round"]:disabled, QToolButton[pill="square"]:disabled, QToolButton[pill="key"]:disabled { color: rgba(201,200,220,100); }
QToolButton[pill="ghost"], QToolButton[pill="rec"] { background: transparent; border: 1px solid rgba(255,255,255,36); border-radius: 11px; padding: 0 8px; min-height: 22px; max-height: 22px; font-weight: 600; }
QToolButton[pill="ghost"]:hover, QToolButton[pill="rec"]:hover { background: rgba(255,255,255,12); }
QToolButton[pill="ghost"]:disabled { border-color: rgba(255,255,255,18); }
QToolButton[pill="ghost"]:checked { background: rgba(169,156,255,36); border-color: rgba(169,156,255,90); color: #d6d0ff; }
QToolButton[pill="rec"]:checked { background: rgba(255,77,109,26); border-color: rgba(255,77,109,100); color: #ff9aab; }
QToolButton[pill="soft"], QPushButton[pill="soft"] { background: rgba(169,156,255,36); color: #d6d0ff; border-radius: 11px; padding: 0 10px; min-height: 22px; max-height: 22px; }
QToolButton[pill="soft"]:hover, QPushButton[pill="soft"]:hover { background: rgba(169,156,255,56); }
QToolButton[pill="round"] { background: rgba(255,255,255,13); border-radius: 12px; min-width: 24px; max-width: 24px; min-height: 24px; max-height: 24px; padding: 0; font-size: 13px; }
QToolButton[pill="square"] { background: rgba(255,255,255,15); border-radius: 7px; min-width: 24px; max-width: 24px; min-height: 24px; max-height: 24px; padding: 0; font-size: 13px; }
QToolButton[pill="round"]:hover, QToolButton[pill="square"]:hover { background: rgba(255,255,255,30); }
QToolButton[pill="key"] { background: #1a1a23; border-radius: 5px; padding: 0 4px; min-height: 20px; max-height: 20px; font-family: "Cascadia Mono", Consolas; font-size: 10px; font-weight: 600; }
QToolButton[pill="key"]:hover { background: #22222d; }
QToolButton[pill="key"]:checked { background: rgba(169,156,255,46); color: #d6d0ff; }

QTreeWidget#vItems { background: transparent; border: 0; outline: 0; color: #e8e7f2; font-size: 12px; font-weight: 600; }
QTreeWidget#vItems::item { padding: 3px 2px; border-top: 1px solid rgba(255,255,255,13); }
QTreeWidget#vItems::item:hover { background: rgba(255,255,255,6); }
QTreeWidget#vItems::item:selected { background: rgba(169,156,255,22); color: #e8e7f2; }
QListWidget#vScenes { background: transparent; border: 0; outline: 0; padding: 4px; }

QScrollArea#vMixScroll, QScrollArea#vScroll { background: transparent; border: 0; }
QWidget#vMixView, QWidget#vMixRows, QWidget#vScrollView, QWidget#vInner { background: transparent; }
QFrame#vStrip { background: #14141b; border: 1px solid transparent; border-radius: 7px; }
QFrame#vStrip[own="true"] { border: 1px solid rgba(169,156,255,90); }
QSlider#vFader { background: transparent; }
QSlider#vFader::groove:vertical { background: rgba(255,255,255,30); width: 4px; border-radius: 2px; }
QSlider#vFader::add-page:vertical, QSlider#vFader::sub-page:vertical { background: transparent; }
QSlider#vFader::handle:vertical { background: #e8e7f2; height: 8px; margin: 0 -6px; border-radius: 3px; }
QSlider#vFader::handle:vertical:disabled { background: #55546a; }
QSlider#vFader::groove:horizontal { background: rgba(255,255,255,30); height: 4px; border-radius: 2px; }
QSlider#vFader::add-page:horizontal, QSlider#vFader::sub-page:horizontal { background: transparent; }
QSlider#vFader::handle:horizontal { background: #e8e7f2; width: 10px; margin: -6px 0; border-radius: 3px; }
QSlider#vFader::handle:horizontal:disabled { background: #55546a; }
)");
}

QFont mono(int px, bool bold, qreal spacing)
{
	QFont f;
	f.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas")});
	f.setStyleHint(QFont::Monospace);
	f.setPixelSize(px);
	f.setWeight(bold ? QFont::DemiBold : QFont::Normal);
	if (spacing != 0)
		f.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
	return f;
}

QFont head()
{
	return mono(10, false, 1.6);
}

void setProp(QWidget *w, const char *name, const QString &value)
{
	if (w->property(name).toString() == value)
		return;
	w->setProperty(name, value);
	w->style()->unpolish(w);
	w->style()->polish(w);
	w->update();
}

namespace {

QPixmap canvasFor(int size)
{
	QPixmap pm(size * 2, size * 2);
	pm.setDevicePixelRatio(2.0);
	pm.fill(Qt::transparent);
	return pm;
}

QPen line(const QColor &c, qreal w = 1.6)
{
	QPen pen(c, w);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	return pen;
}

} // namespace

QIcon sourceIcon(const QString &type, bool group)
{
	QPixmap pm = canvasFor(28);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 255, 255, 18));
	p.drawRoundedRect(QRectF(0, 0, 28, 28), 9, 9);
	p.setPen(line(SOFT));
	p.setBrush(Qt::NoBrush);
	const QString t = type;
	if (group || t == "scene") {
		p.drawRoundedRect(QRectF(8, 10, 10, 9), 1.5, 1.5);
		p.drawPolyline(QPolygonF({QPointF(11, 7.5), QPointF(20.5, 7.5), QPointF(20.5, 16)}));
	} else if (t.contains("dshow") || t.contains("av_capture") || t.contains("v4l2")) {
		p.drawRoundedRect(QRectF(6.5, 9, 11, 10), 2, 2);
		p.drawPolyline(QPolygonF({QPointF(17.5, 12.5), QPointF(21.5, 10), QPointF(21.5, 18), QPointF(17.5, 15.5)}));
	} else if (t.contains("game_capture") || t.contains("monitor_capture") || t.contains("window_capture") ||
		   t.contains("display_capture") || t.contains("screen_capture") || t.contains("xcomposite") ||
		   t.contains("pipewire")) {
		p.drawRoundedRect(QRectF(6, 7.5, 16, 11), 2, 2);
		p.drawLine(QPointF(11, 21.5), QPointF(17, 21.5));
		p.drawLine(QPointF(14, 18.5), QPointF(14, 21.5));
	} else if (t.contains("browser")) {
		QPainterPath b;
		b.moveTo(7, 8);
		b.lineTo(21, 8);
		b.lineTo(21, 18);
		b.lineTo(12, 18);
		b.lineTo(8, 21);
		b.lineTo(8, 18);
		b.lineTo(7, 18);
		b.closeSubpath();
		p.drawPath(b);
	} else if (t.contains("image") || t.contains("slideshow")) {
		p.drawRoundedRect(QRectF(6.5, 7.5, 15, 13), 2, 2);
		p.drawPolyline(QPolygonF({QPointF(8.5, 18), QPointF(12.5, 13.5), QPointF(15, 16), QPointF(17, 14), QPointF(19.5, 17)}));
	} else if (t.contains("text")) {
		p.drawLine(QPointF(8, 8.5), QPointF(20, 8.5));
		p.drawLine(QPointF(14, 8.5), QPointF(14, 20));
	} else if (t.contains("ffmpeg") || t.contains("vlc") || t.contains("media")) {
		QPainterPath play;
		play.moveTo(10.5, 8);
		play.lineTo(19.5, 14);
		play.lineTo(10.5, 20);
		play.closeSubpath();
		p.drawPath(play);
	} else if (t.contains("wasapi") || t.contains("audio") || t.contains("pulse") || t.contains("coreaudio")) {
		QPainterPath s;
		s.moveTo(7, 12);
		s.lineTo(10, 12);
		s.lineTo(14, 8.5);
		s.lineTo(14, 19.5);
		s.lineTo(10, 16);
		s.lineTo(7, 16);
		s.closeSubpath();
		p.drawPath(s);
		p.drawArc(QRectF(13, 10, 7, 8), -50 * 16, 100 * 16);
	} else if (t.contains("color")) {
		p.setPen(Qt::NoPen);
		p.setBrush(SOFT);
		p.drawEllipse(QRectF(9, 9, 10, 10));
	} else {
		p.drawRoundedRect(QRectF(8, 8, 12, 12), 2.5, 2.5);
	}
	p.end();
	return QIcon(pm);
}

QIcon lineIcon(const QString &name)
{
	QPixmap pm = canvasFor(14);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(line(SOFT, 1.7));
	p.setBrush(Qt::NoBrush);
	if (name == "minus") {
		p.drawLine(QPointF(3, 7), QPointF(11, 7));
	} else if (name == "up") {
		p.drawPolyline(QPolygonF({QPointF(3.5, 9), QPointF(7, 5.5), QPointF(10.5, 9)}));
	} else if (name == "down") {
		p.drawPolyline(QPolygonF({QPointF(3.5, 5), QPointF(7, 8.5), QPointF(10.5, 5)}));
	} else if (name == "gear") {
		p.drawEllipse(QPointF(7, 7), 2.2, 2.2);
		for (int i = 0; i < 8; i++) {
			const double a = i * 3.14159265 / 4.0;
			p.drawLine(QPointF(7 + 4.0 * std::cos(a), 7 + 4.0 * std::sin(a)),
				   QPointF(7 + 5.6 * std::cos(a), 7 + 5.6 * std::sin(a)));
		}
	} else if (name == "replay") {
		p.drawArc(QRectF(2.5, 2.5, 9, 9), 60 * 16, 290 * 16);
		p.drawPolyline(QPolygonF({QPointF(10.2, 1.8), QPointF(10.6, 4.4), QPointF(8, 4.8)}));
	} else if (name == "save") {
		p.drawLine(QPointF(7, 2.5), QPointF(7, 9));
		p.drawPolyline(QPolygonF({QPointF(4, 6.5), QPointF(7, 9.5), QPointF(10, 6.5)}));
		p.drawLine(QPointF(3, 11.8), QPointF(11, 11.8));
	} else if (name == "more") {
		p.setPen(Qt::NoPen);
		p.setBrush(SOFT);
		for (double x : {3.0, 7.0, 11.0})
			p.drawEllipse(QPointF(x, 7), 1.3, 1.3);
	}
	p.end();
	return QIcon(pm);
}

QIcon dotIcon(const QColor &c, bool square)
{
	QPixmap pm = canvasFor(10);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(c);
	if (square)
		p.drawRoundedRect(QRectF(1, 1, 8, 8), 1.5, 1.5);
	else
		p.drawEllipse(QRectF(1.5, 1.5, 7, 7));
	p.end();
	return QIcon(pm);
}

// ---- Switch ------------------------------------------------------------------

Switch::Switch(QWidget *parent) : QAbstractButton(parent)
{
	setCheckable(true);
	setCursor(Qt::PointingHandCursor);
	setFocusPolicy(Qt::TabFocus);
	setFixedSize(30, 18);
}

void Switch::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	if (!isEnabled())
		p.setOpacity(0.45);
	const bool on = isChecked();
	const QRectF r(0, 0, 30, 18);
	p.setPen(hasFocus() ? QPen(INK, 1.5) : Qt::NoPen);
	p.setBrush(on ? VIOLET : QColor(255, 255, 255, 26));
	p.drawRoundedRect(r.adjusted(0.75, 0.75, -0.75, -0.75), 8.25, 8.25);
	p.setPen(Qt::NoPen);
	p.setBrush(on ? BG : MUTED);
	p.drawEllipse(QRectF(on ? 14 : 2, 2, 14, 14));
}

// ---- IconToggle ----------------------------------------------------------------

IconToggle::IconToggle(Kind kind, QWidget *parent) : QAbstractButton(parent), kind_(kind)
{
	setCheckable(true);
	setCursor(Qt::PointingHandCursor);
	setFocusPolicy(Qt::TabFocus);
	setFixedSize(22, 22);
}

void IconToggle::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	if (!isEnabled())
		p.setOpacity(0.4);
	const bool on = isChecked();
	const QColor tone = kind_ == Kind::Lock ? AMBER : ROSE_TEXT;
	QColor bg = on ? tone : QColor(255, 255, 255, underMouse() ? 30 : 15);
	if (on)
		bg.setAlpha(36);
	p.scale(22.0 / 26.0, 22.0 / 26.0); // drawn on a 26 px grid
	p.setPen(hasFocus() ? QPen(INK, 1.2) : Qt::NoPen);
	p.setBrush(bg);
	p.drawRoundedRect(QRectF(0.5, 0.5, 25, 25), 7, 7);
	p.setPen(line(on ? tone : SOFT, 1.5));
	p.setBrush(Qt::NoBrush);
	if (kind_ == Kind::Lock) {
		p.drawRoundedRect(QRectF(8, 12, 10, 8), 1.5, 1.5);
		QPainterPath s;
		s.moveTo(10, 12);
		s.lineTo(10, 9.5);
		s.arcTo(QRectF(10, 6.5, 6, 6), 180, -180);
		if (on)
			s.lineTo(16, 12);
		p.drawPath(s);
	} else {
		QPainterPath s;
		s.moveTo(6.5, 11);
		s.lineTo(9.5, 11);
		s.lineTo(13, 8);
		s.lineTo(13, 18);
		s.lineTo(9.5, 15);
		s.lineTo(6.5, 15);
		s.closeSubpath();
		p.drawPath(s);
		if (on) {
			p.drawLine(QPointF(16, 10.5), QPointF(20, 15.5));
			p.drawLine(QPointF(20, 10.5), QPointF(16, 15.5));
		} else {
			p.drawArc(QRectF(11, 9, 8, 8), -50 * 16, 100 * 16);
		}
	}
}

// ---- Meter ---------------------------------------------------------------------

Meter::Meter(QWidget *parent) : QWidget(parent)
{
	setFixedSize(8, 102);
	setAttribute(Qt::WA_TransparentForMouseEvents);
}

void Meter::setLevel(int level)
{
	// Peak hold, like OBS's meters: the loudest level stays lit for 1.5 seconds.
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	int hold = hold_;
	if (level >= hold || now - holdAt_ > 1500) {
		hold = level;
		holdAt_ = now;
	}
	if (level == level_ && hold == hold_)
		return;
	level_ = level;
	hold_ = hold;
	update();
}

void Meter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const bool across = width() > height(); // a mixer row: left to right
	const int n = (across ? width() : height()) / 6;
	const int lit = int(std::lround(double(n) * double(level_) / 100.0));
	const int held = int(std::lround(double(n) * double(hold_) / 100.0)) - 1;
	for (int i = 0; i < n; i++) {
		QColor c(255, 255, 255, 20);
		if (i < lit || (i == held && held > 0))
			c = i >= n * 85 / 100 ? ROSE : i >= n * 70 / 100 ? AMBER : GREEN;
		p.fillRect(across ? QRect(i * 6, 0, 4, height()) : QRect(0, height() - (i + 1) * 6 + 2, width(), 4), c);
	}
}

// ---- SceneDelegate -------------------------------------------------------------

QString SceneDelegate::tag(bool shown, const QString &mains)
{
	if (shown)
		return live ? mt("Vertical.OnAir") : mt("Vertical.Showing");
	return mains.isEmpty() ? QString() : QStringLiteral("← ") + mains;
}

void SceneDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const
{
	p->save();
	p->setRenderHint(QPainter::Antialiasing);
	const QRectF r = QRectF(opt.rect).adjusted(0, 1, 0, -1);
	// Red "ON AIR" only while a stream is out: before that it's what the
	// vertical picture shows, not what viewers see.
	const bool shown = idx.data(OnAirRole).toBool();
	const bool onAir = shown && live;
	const bool sel = opt.state.testFlag(QStyle::State_Selected);
	const bool hover = opt.state.testFlag(QStyle::State_MouseOver);
	QColor bg = Qt::transparent;
	if (onAir)
		bg = QColor(255, 77, 109, sel ? 40 : 26);
	else if (shown || sel)
		bg = QColor(169, 156, 255, sel ? 34 : 22);
	else if (hover)
		bg = QColor(255, 255, 255, 10);
	p->setPen(Qt::NoPen);
	p->setBrush(bg);
	p->drawRoundedRect(r, 9, 9);
	p->setBrush(onAir ? ROSE : shown ? VIOLET : OFF);
	p->drawEllipse(QPointF(r.left() + 14, r.center().y()), 4, 4);

	const QString mains = idx.data(MainsRole).toString();
	const QString tag = SceneDelegate::tag(shown, mains);
	QFont tf = opt.font;
	tf.setPixelSize(10);
	tf.setWeight(shown ? QFont::Bold : QFont::Normal);
	const QFontMetrics tm(tf);
	const QRectF body = r.adjusted(28, 0, -10, 0);
	const QString tagText = tm.elidedText(tag, Qt::ElideRight, int(body.width() * 0.45));
	const qreal tagW = tag.isEmpty() ? 0 : tm.horizontalAdvance(tagText) + 8;
	if (!tag.isEmpty()) {
		p->setFont(tf);
		p->setPen(onAir ? ROSE_TEXT : shown ? VIOLET : MUTED);
		p->drawText(body, Qt::AlignRight | Qt::AlignVCenter, tagText);
	}
	QFont nf = opt.font;
	nf.setPixelSize(12);
	nf.setWeight(QFont::DemiBold);
	p->setFont(nf);
	p->setPen(INK);
	const QRectF nameRect = body.adjusted(0, 0, -tagW, 0);
	p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
		    QFontMetrics(nf).elidedText(idx.data(Qt::DisplayRole).toString(), Qt::ElideRight, int(nameRect.width())));
	p->restore();
}

QSize SceneDelegate::sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &) const
{
	return {0, 30}; // rows fill the list's width
}

} // namespace vtheme
