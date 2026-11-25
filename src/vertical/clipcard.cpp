#include "clipcard.hpp"

#include "../i18n.hpp"
#include "motion.hpp"
#include "theme.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

namespace {

enum class Kind { Saving, Saved, Failed, Note, NoteOk };

constexpr int SHADOW = 8;  // the soft shadow around the card, inside its window
constexpr int SLIDE = 8;   // how far it rises as it comes in
constexpr int GAP = 12;    // from the bottom of the preview
constexpr int SIDE = 14;   // from its sides

QPixmap canvas(int px)
{
	QPixmap pm(px * 3, px * 3);
	pm.setDevicePixelRatio(3);
	pm.fill(Qt::transparent);
	return pm;
}

QPen line(const QColor &c, qreal w = 1.6)
{
	return QPen(c, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

// The card's own small icons (12 px), in the docks' line style.
QIcon icon(const QString &name, const QColor &c)
{
	QPixmap pm = canvas(12);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(line(c));
	if (name == "play") {
		p.setBrush(c);
		p.drawPolygon(QPolygonF({QPointF(3.5, 2.2), QPointF(10, 6), QPointF(3.5, 9.8)}));
	} else if (name == "folder") {
		QPainterPath f;
		f.moveTo(1.5, 3);
		f.lineTo(4.6, 3);
		f.lineTo(5.8, 4.4);
		f.lineTo(10.5, 4.4);
		f.lineTo(10.5, 9.6);
		f.lineTo(1.5, 9.6);
		f.closeSubpath();
		p.drawPath(f);
	} else if (name == "close") {
		p.drawLine(QPointF(3, 3), QPointF(9, 9));
		p.drawLine(QPointF(9, 3), QPointF(3, 9));
	} else if (name == "tick") {
		p.setPen(line(c, 1.9));
		p.drawPolyline(QPolygonF({QPointF(2.2, 6.3), QPointF(4.9, 9), QPointF(9.8, 3.2)}));
	} else if (name == "alert" || name == "info") {
		// A ring with "!" (or "i"), in front of the title like the tick.
		p.setPen(line(c, 1.4));
		p.drawEllipse(QPointF(6, 6), 4.8, 4.8);
		p.setPen(line(c, 1.6));
		if (name == "alert") {
			p.drawLine(QPointF(6, 3.6), QPointF(6, 6.4));
			p.drawPoint(QPointF(6, 8.4));
		} else {
			p.drawPoint(QPointF(6, 3.8));
			p.drawLine(QPointF(6, 5.8), QPointF(6, 8.4));
		}
	}
	return QIcon(pm);
}

// The small spinner in the mark's place before "Saving clip…", where the tick lands.
QPixmap spinnerMark(qreal angle)
{
	QPixmap pm = canvas(12);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF ring(1.5, 1.5, 9, 9);
	p.setPen(line(QColor(255, 255, 255, 50), 1.6));
	p.drawEllipse(ring);
	p.setPen(line(vtheme::VIOLET, 1.6));
	p.drawArc(ring, int(-angle * 16), 100 * 16);
	return pm;
}

QString lengthWords(int s)
{
	if (s >= 60 && s % 60 == 0)
		return mt(s >= 120 ? "Vertical.MinutesOther" : "Vertical.MinutesOne").arg(s / 60);
	return mt("Vertical.Seconds").arg(s);
}

// The picture on the left: the clip's last frame with its length (a stack of
// them with a count once there are a few), dimmed while it saves, or a
// tile with what went wrong.
class Art : public QWidget {
public:
	Kind kind = Kind::Saved;
	QImage still;
	int seconds = 0;
	int count = 0;

	QSize sizeHint() const override
	{
		if (kind == Kind::Failed || kind == Kind::Note || kind == Kind::NoteOk)
			return {32, 32};
		// Room for the stack behind it always, so nothing beside it moves when the second clip comes.
		return QSize(44, 68);
	}
	QSize minimumSizeHint() const override { return sizeHint(); }
	void changed()
	{
		setFixedSize(sizeHint());
		update();
	}

protected:
	bool stacked() const { return count > 1; }

	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		if (kind == Kind::Failed || kind == Kind::Note || kind == Kind::NoteOk) {
			const QColor c = kind == Kind::Failed ? vtheme::ROSE_TEXT : kind == Kind::NoteOk ? vtheme::GREEN : vtheme::VIOLET;
			QColor tile = kind == Kind::Failed ? vtheme::ROSE : c;
			tile.setAlpha(36);
			QPainterPath t;
			t.addRoundedRect(QRectF(0, 0, 32, 32), 9, 9);
			p.fillPath(t, tile);
			p.setPen(line(c, 1.8));
			if (kind == Kind::NoteOk) {
				p.drawPolyline(QPolygonF({QPointF(10, 16.5), QPointF(14.2, 20.5), QPointF(22, 11.5)}));
			} else if (kind == Kind::Failed) {
				p.drawEllipse(QPointF(16, 16), 7.5, 7.5);
				p.drawLine(QPointF(16, 12), QPointF(16, 16.6));
				p.drawPoint(QPointF(16, 19.8));
			} else {
				p.drawEllipse(QPointF(16, 16), 7.5, 7.5);
				p.drawPoint(QPointF(16, 12.4));
				p.drawLine(QPointF(16, 15.4), QPointF(16, 19.8));
			}
			return;
		}
		const QRectF front(0, 4, 36, 64);
		if (stacked()) {
			// The clips before it, behind.
			QPainterPath back;
			back.addRoundedRect(QRectF(7, 0.5, 36, 63), 6, 6);
			p.fillPath(back, QColor(0x2a, 0x29, 0x36));
			p.setPen(QPen(QColor(255, 255, 255, 30), 1));
			p.drawPath(back);
		}
		QPainterPath shape;
		shape.addRoundedRect(front, 6, 6);
		p.save();
		p.setClipPath(shape);
		p.fillRect(front, QColor(0x1d, 0x1c, 0x27));
		if (!still.isNull()) {
			const QSizeF fit = QSizeF(still.size()).scaled(front.size(), Qt::KeepAspectRatioByExpanding);
			p.drawImage(QRectF(front.center().x() - fit.width() / 2, front.center().y() - fit.height() / 2, fit.width(), fit.height()), still);
		}
		if (kind == Kind::Saving)
			p.fillRect(front, QColor(14, 14, 19, 150));
		p.restore();
		p.setPen(QPen(QColor(255, 255, 255, 34), 1));
		p.setBrush(Qt::NoBrush);
		p.drawPath(shape);
		if (kind != Kind::Saving && seconds > 0) {
			// Its length, bottom left, like a video's.
			QFont f = font();
			f.setPixelSize(9);
			f.setBold(true);
			p.setFont(f);
			const QString len = QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
			const int w = p.fontMetrics().horizontalAdvance(len) + 8;
			const QRectF badge(front.left() + 3, front.bottom() - 17, w, 14);
			QPainterPath b;
			b.addRoundedRect(badge, 4, 4);
			p.fillPath(b, QColor(0, 0, 0, 170));
			p.setPen(Qt::white);
			p.drawText(badge, Qt::AlignCenter, len);
		}
		if (stacked()) {
			QFont f = font();
			f.setPixelSize(10);
			f.setWeight(QFont::Black);
			p.setFont(f);
			const QString n = count > 99 ? QStringLiteral("99+") : QString::number(count);
			const qreal w = std::max<qreal>(16, p.fontMetrics().horizontalAdvance(n) + 8);
			const QRectF badge(width() - w, 0, w, 16);
			QPainterPath b;
			b.addRoundedRect(badge, 8, 8);
			p.fillPath(b, vtheme::VIOLET);
			p.setPen(QColor(0x14, 0x11, 0x2a));
			p.drawText(badge, Qt::AlignCenter, n);
		}
	}
};

QToolButton *button(const QString &text, const char *pill, const QIcon &ic = QIcon())
{
	auto *b = new QToolButton();
	b->setText(text);
	b->setProperty("pill", pill);
	b->setCursor(Qt::PointingHandCursor);
	b->setFocusPolicy(Qt::NoFocus);
	b->setAutoRaise(true);
	if (!ic.isNull()) {
		b->setIcon(ic);
		b->setIconSize(QSize(12, 12));
		b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	}
	return b;
}

class Card : public QWidget {
public:
	QPointer<QWidget> over;
	QPointer<QWidget> watchedWindow;
	clipcard::Actions actions;
	Kind kind = Kind::Note;
	QString file;
	bool wanted = false; // it has something to say (and shows whenever the preview does)
	Art *art = nullptr;
	QLabel *tick = nullptr, *title = nullptr, *text = nullptr;
	QWidget *acts = nullptr;
	QToolButton *watch = nullptr, *folder = nullptr, *fix = nullptr, *x = nullptr;
	QVariantAnimation life, fade, spin;
	QTimer follow;
	QPoint base;

	static constexpr Qt::WindowFlags FLAGS = Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
						  Qt::NoDropShadowWindowHint;

	Card() : QWidget(nullptr, FLAGS)
	{
		setObjectName("multistreamClipCard");
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_ShowWithoutActivating);
		setStyleSheet(vtheme::style() + QStringLiteral(R"(
QLabel#clipTitle { color: #e8e7f2; background: transparent; font-size: 12px; font-weight: 700; }
QLabel#clipTitle[clipTone="bad"] { color: #ff9aab; }
QLabel#clipText { color: #b3b2c6; background: transparent; font-size: 11px; }
QToolButton[pill="primary"] { background: #7a68f0; border: 1px solid #a99cff; color: #ffffff; border-radius: 11px; padding: 0 10px; min-height: 22px; max-height: 22px; font-size: 11px; font-weight: 700; }
QToolButton[pill="primary"]:hover { background: #8877f5; }
QToolButton[pill="soft"]:disabled, QToolButton[pill="ghost"]:disabled { color: rgba(201,200,220,90); }
QToolButton[pill="soft"]:disabled { background: rgba(169,156,255,18); }
QToolButton[compact="true"] { padding: 0; min-width: 30px; max-width: 30px; }
QToolButton[pill="x"] { background: transparent; border: 0; border-radius: 13px; min-width: 26px; max-width: 26px; min-height: 26px; max-height: 26px; padding: 0; }
QToolButton[pill="x"]:hover { background: rgba(255,255,255,20); }
)"));
		auto *row = new QHBoxLayout(this);
		row->setContentsMargins(SHADOW + 10, SHADOW + 10, SHADOW + 6, SHADOW + 11);
		row->setSpacing(10);
		art = new Art();
		row->addWidget(art, 0, Qt::AlignTop);
		auto *col = new QVBoxLayout();
		col->setContentsMargins(0, 1, 0, 0);
		col->setSpacing(3);
		auto *head = new QHBoxLayout();
		head->setSpacing(5);
		tick = new QLabel();
		tick->setPixmap(icon("tick", vtheme::GREEN).pixmap(12, 12));
		tick->setContentsMargins(0, 3, 0, 0);
		head->addWidget(tick, 0, Qt::AlignTop);
		title = new QLabel();
		title->setObjectName("clipTitle");
		title->setTextFormat(Qt::PlainText);
		title->setWordWrap(true);
		head->addWidget(title, 1);
		col->addLayout(head);
		text = new QLabel();
		text->setObjectName("clipText");
		text->setTextFormat(Qt::PlainText);
		text->setWordWrap(true);
		col->addWidget(text);
		acts = new QWidget();
		auto *al = new QHBoxLayout(acts);
		al->setContentsMargins(0, 5, 0, 0);
		al->setSpacing(6);
		watch = button(mt("Vertical.ClipWatch"), "soft", icon("play", QColor(0xd6, 0xd0, 0xff)));
		folder = button(mt("Vertical.ClipShowFolder"), "ghost", icon("folder", vtheme::SOFT));
		watch->setToolTip(watch->text());
		folder->setToolTip(folder->text());
		fix = button(QString(), "primary");
		fix->setToolButtonStyle(Qt::ToolButtonTextOnly);
		al->addWidget(watch);
		al->addWidget(folder);
		al->addWidget(fix);
		al->addStretch(1);
		col->addWidget(acts);
		row->addLayout(col, 1);
		x = button(QString(), "x", icon("close", vtheme::MUTED));
		x->setToolTip(mt("Vertical.ClipClose"));
		row->addWidget(x, 0, Qt::AlignTop);

		QObject::connect(watch, &QToolButton::clicked, this, [this]() {
			if (!file.isEmpty())
				QDesktopServices::openUrl(QUrl::fromLocalFile(file));
		});
		QObject::connect(folder, &QToolButton::clicked, this, [this]() { showInFolder(file); });
		QObject::connect(fix, &QToolButton::clicked, this, [this]() {
			const auto act = kind == Kind::Failed && fix->property("does").toString() == "replay" ? actions.turnOnReplay
													      : actions.saveAgain;
			if (act)
				act();
		});
		QObject::connect(x, &QToolButton::clicked, this, [this]() { dismiss(); });

		// The line along its bottom runs down while it's up; the pointer on it holds it.
		life.setStartValue(1.0);
		life.setEndValue(0.0);
		QObject::connect(&life, &QVariantAnimation::valueChanged, this, [this]() { update(); });
		QObject::connect(&life, &QVariantAnimation::finished, this, [this]() { dismiss(); });
		fade.setEasingCurve(QEasingCurve::OutCubic);
		QObject::connect(&fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { setShown(v.toReal()); });
		QObject::connect(&fade, &QVariantAnimation::finished, this, [this]() {
			if (fade.endValue().toReal() == 0.0)
				hide();
		});
		spin.setStartValue(0.0);
		spin.setEndValue(360.0);
		spin.setDuration(900);
		spin.setLoopCount(-1);
		QObject::connect(&spin, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
			if (kind == Kind::Saving)
				tick->setPixmap(spinnerMark(v.toReal()));
		});
		// Docks move with splitters and floating windows: keep it on the preview.
		follow.setInterval(200);
		QObject::connect(&follow, &QTimer::timeout, this, [this]() { sync(); });
	}

	static void showInFolder(const QString &file)
	{
		if (file.isEmpty())
			return;
#ifdef _WIN32
		QProcess p;
		p.setProgram(QStringLiteral("explorer.exe"));
		p.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(file)));
		if (p.startDetached())
			return;
#elif defined(__APPLE__)
		if (QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), file}))
			return;
#endif
		QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(file).absolutePath()));
	}

	void setShown(qreal v)
	{
		setWindowOpacity(v);
		move(base + QPoint(0, int(std::lround((1.0 - v) * SLIDE))));
	}

	void animate(qreal to)
	{
		const qreal from = isVisible() ? windowOpacity() : 0.0;
		fade.stop();
		if (!vmotion::enabled() || std::abs(from - to) < 0.01) {
			setShown(to);
			if (to == 0.0)
				hide();
			return;
		}
		fade.setDuration(to > from ? 200 : 160);
		fade.setStartValue(from);
		fade.setEndValue(to);
		fade.start();
	}

	// Where the preview is, and whether it's on screen at all.
	void sync()
	{
		QWidget *win = over ? over->window() : nullptr;
		if (win != watchedWindow) {
			if (watchedWindow)
				watchedWindow->removeEventFilter(this);
			watchedWindow = win;
			if (win)
				win->installEventFilter(this);
		}
		const bool can = wanted && over && win && over->isVisible() && !win->isMinimized() && over->width() >= 140 &&
				 over->height() >= 70;
		if (!can) {
			// The preview went (a tab, a closed dock): so does the card, at once.
			// (Leaving on its own, it's fading out already.)
			if (wanted && isVisible()) {
				fade.stop();
				hide();
			}
			return;
		}
		if (parentWidget() != win)
			setParent(win, FLAGS);
		place();
		if (!isVisible()) {
			setWindowOpacity(0.0);
			show();
			raise();
			animate(1.0);
		} else if (fade.state() != QAbstractAnimation::Running) {
			setShown(windowOpacity());
		}
	}

	void place()
	{
		const int avail = over->width() - 2 * SIDE;
		const int w = std::min(avail, 440) + 2 * SHADOW;
		// A narrow dock: Watch and Show in folder as icons (their names on hover),
		// so neither gets cut to "...".
		const Qt::ToolButtonStyle style = w - 2 * SHADOW < 330 ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon;
		for (QToolButton *b : {watch, folder}) {
			if (b->toolButtonStyle() != style) {
				b->setToolButtonStyle(style);
				b->setProperty("compact", style == Qt::ToolButtonIconOnly);
				b->style()->unpolish(b);
				b->style()->polish(b);
			}
		}
		setFixedWidth(w);
		layout()->activate();
		const int h = layout()->hasHeightForWidth() ? layout()->heightForWidth(w) : sizeHint().height();
		setFixedHeight(h);
		const QPoint tl = over->mapToGlobal(QPoint(0, 0));
		const int y = std::max(tl.y() - SHADOW, tl.y() + over->height() - GAP - (h - SHADOW));
		base = QPoint(tl.x() + (over->width() - (w - 2 * SHADOW)) / 2 - SHADOW, y);
	}

	void present(Kind k, const QString &t, const QString &body, int ms)
	{
		const bool fresh = !wanted;
		const bool same = kind == k && title->text() == t;
		kind = k;
		art->kind = k;
		art->changed();
		title->setText(t);
		vtheme::setProp(title, "clipTone", k == Kind::Failed ? "bad" : "");
		text->setText(body);
		text->setVisible(!body.isEmpty());
		const bool clip = k == Kind::Saving || k == Kind::Saved;
		// Only a clip has a picture; the rest say it with a small mark before the
		// title, so the words get the full width and every card is about one height.
		art->setVisible(clip);
		// While saving the mark's place stays (empty), so the title doesn't slide when the tick comes.
		tick->setPixmap(k == Kind::Saving ? spinnerMark(0)
						  : icon(k == Kind::Failed ? "alert" : k == Kind::Note ? "info" : "tick",
							 k == Kind::Failed ? vtheme::ROSE_TEXT
							 : k == Kind::Note ? vtheme::VIOLET
									   : vtheme::GREEN)
								.pixmap(12, 12));
		watch->setVisible(clip);
		folder->setVisible(clip);
		watch->setEnabled(k == Kind::Saved);
		folder->setEnabled(k == Kind::Saved);
		// Nothing to watch yet while it's writing: clearly greyed, then they come up.
		for (QToolButton *b : {watch, folder}) {
			auto *dim = qobject_cast<QGraphicsOpacityEffect *>(b->graphicsEffect());
			if (!dim) {
				dim = new QGraphicsOpacityEffect(b);
				b->setGraphicsEffect(dim);
			}
			dim->setOpacity(k == Kind::Saving ? 0.4 : 1.0);
		}
		fix->setVisible(k == Kind::Failed);
		acts->setVisible(clip || k == Kind::Failed);
		if (k == Kind::Saving)
			spin.start();
		else
			spin.stop();
		if (!fresh && !same && isVisible())
			vmotion::dip(title);
		life.stop();
		if (ms > 0) {
			life.setDuration(ms);
			life.start();
			if (underMouse())
				life.pause();
		}
		update();
		wanted = true;
		follow.start();
		sync();
	}

	void dismiss()
	{
		wanted = false;
		life.stop();
		spin.stop();
		follow.stop();
		if (isVisible())
			animate(0.0);
	}

protected:
	bool eventFilter(QObject *o, QEvent *e) override
	{
		switch (e->type()) {
		case QEvent::Move:
		case QEvent::Resize:
		case QEvent::Show:
		case QEvent::Hide:
		case QEvent::WindowStateChange:
			if (wanted)
				QTimer::singleShot(0, this, [this]() { sync(); });
			break;
		default:
			break;
		}
		return QWidget::eventFilter(o, e);
	}

	void enterEvent(QEnterEvent *e) override
	{
		if (life.state() == QAbstractAnimation::Running)
			life.pause();
		QWidget::enterEvent(e);
	}

	void leaveEvent(QEvent *e) override
	{
		if (life.state() == QAbstractAnimation::Paused)
			life.resume();
		QWidget::leaveEvent(e);
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QRectF r = QRectF(rect()).adjusted(SHADOW + 0.5, SHADOW + 0.5, -SHADOW - 0.5, -SHADOW - 0.5);
		// A soft shadow, so it lifts off whatever the picture shows.
		for (int i = SHADOW; i > 0; i--) {
			QPainterPath s;
			s.addRoundedRect(r.adjusted(-i, -i + 3, i, i + 3), 12 + i, 12 + i);
			p.fillPath(s, QColor(0, 0, 0, 7));
		}
		QPainterPath card;
		card.addRoundedRect(r, 12, 12);
		p.fillPath(card, QColor(23, 22, 31, 248));
		p.setPen(QPen(kind == Kind::Failed ? QColor(255, 77, 109, 100) : QColor(255, 255, 255, 26), 1));
		p.drawPath(card);
		if (life.state() != QAbstractAnimation::Stopped) {
			p.setClipPath(card);
			// A faint track, so the line reads as time running out, not a stray
			// stroke; inside the corners, with round ends.
			const qreal y = r.bottom() - 4, x0 = r.left() + 12, len = r.width() - 24;
			p.setPen(QPen(QColor(255, 255, 255, 26), 2, Qt::SolidLine, Qt::RoundCap));
			p.drawLine(QPointF(x0, y), QPointF(x0 + len, y));
			const qreal left = len * life.currentValue().toReal();
			if (left > 0.5) {
				p.setPen(QPen(kind == Kind::Failed ? vtheme::ROSE_TEXT : vtheme::VIOLET, 2, Qt::SolidLine, Qt::RoundCap));
				p.drawLine(QPointF(x0, y), QPointF(x0 + left, y));
			}
		}
	}
};

QPointer<Card> card;

} // namespace

namespace clipcard {

void attach(QWidget *over, Actions actions)
{
	if (!card)
		card = new Card();
	card->over = over;
	card->actions = std::move(actions);
	over->installEventFilter(card);
}

void show(const QJsonObject &clip, const QImage &still)
{
	if (!card)
		return;
	const QString phase = clip.value("phase").toString();
	const int seconds = clip.value("seconds").toInt();
	const int count = clip.value("count").toInt();
	const QString at =
		QLocale().toString(QDateTime::fromMSecsSinceEpoch(qint64(clip.value("at").toDouble())).time(), QLocale::ShortFormat);
	// One short line: the picture's badge carries the length; past the first clip
	// the words say what the stack's number counts.
	QString meta;
	if (phase == "saving")
		meta = mt("Vertical.ClipLastShort").arg(lengthWords(seconds));
	else if (count > 1)
		meta = mt("Vertical.ClipSavedAtCount").arg(at).arg(count);
	else
		meta = at; // the title already says saved
	card->art->still = still;
	card->art->seconds = seconds;
	card->art->count = phase == "saved" ? count : count + 1; // the one saving now counts
	card->file = clip.value("file").toString();
	if (phase == "saving") {
		card->present(Kind::Saving, mt("Vertical.ClipSaving"), meta, 0);
	} else if (phase == "saved") {
		card->present(Kind::Saved, mt("Vertical.ClipSaved"), meta, 8000);
	} else if (phase == "failed") {
		const bool off = clip.value("reason").toString() == "off";
		card->fix->setText(mt(off ? "Vertical.ClipTurnOnReplay" : "Vertical.ClipSaveAgain"));
		card->fix->setProperty("does", off ? "replay" : "again");
		card->fix->setToolTip(off ? mt("Vertical.ClipOffTip").arg(lengthWords(seconds)) : QString());
		// It stays until they close it or fix it: a streamer talking to chat mustn't miss it.
		card->present(Kind::Failed, mt("Vertical.ClipNotSaved"), mt(off ? "Vertical.ClipOffWhy" : "Vertical.ClipWriteWhy"),
			      0);
	}
}

void note(const QString &title, const QString &text, bool ok)
{
	if (card)
		card->present(ok ? Kind::NoteOk : Kind::Note, title, text, 8000);
}

QJsonObject report()
{
	QJsonObject o;
	if (!card)
		return o;
	static const char *names[] = {"saving", "saved", "failed", "note", "note"};
	o["visible"] = card->isVisible() && card->wanted;
	o["wanted"] = card->wanted;
	o["phase"] = names[int(card->kind)];
	o["count"] = card->art->count; // the stack's badge
	o["title"] = card->title->text();
	o["text"] = card->text->isHidden() ? QString() : card->text->text();
	QJsonArray buttons;
	for (QToolButton *b : {card->watch, card->folder, card->fix})
		if (!b->isHidden())
			buttons.append(QJsonObject{{"text", b->text()},
						   {"enabled", b->isEnabled()},
						   {"iconOnly", b->toolButtonStyle() == Qt::ToolButtonIconOnly}});
	o["buttons"] = buttons;
	if (card->isVisible()) {
		const QRect g = card->geometry();
		o["rect"] = QJsonObject{{"x", g.x()}, {"y", g.y()}, {"width", g.width()}, {"height", g.height()}};
	}
	return o;
}

void close()
{
	delete card.data();
}

} // namespace clipcard
