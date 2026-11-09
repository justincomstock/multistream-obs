#include "motion.hpp"

#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QPointer>
#include <QVariantAnimation>
#include <QWidget>

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // windows.h's min and max macros break std::min and std::max
#endif
#include <windows.h>
#endif

namespace vmotion {

namespace {

const char *kAnim = "vm-anim";
const char *kEffect = "vm-fade";

QVariantAnimation *animOf(QWidget *w)
{
	return w->findChild<QVariantAnimation *>(QString::fromLatin1(kAnim), Qt::FindDirectChildrenOnly);
}

bool ourEffect(QWidget *w)
{
	QGraphicsEffect *e = w->graphicsEffect();
	return e && e->objectName() == QLatin1String(kEffect);
}

// Stops what is moving without finishing it (the caller sets the end state).
void dropAnim(QWidget *w)
{
	if (QVariantAnimation *a = animOf(w)) {
		a->disconnect();
		a->stop();
		a->setObjectName(QString());
		a->deleteLater();
	}
}

void clearEffect(QWidget *w)
{
	if (ourEffect(w))
		w->setGraphicsEffect(nullptr); // the widget deletes it
}

void freeSize(QWidget *w)
{
	w->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
}

// Is there a reason to skip the motion: animations off, or nobody would see it.
bool instant(QWidget *w)
{
	if (!enabled())
		return true;
	QWidget *parent = w->parentWidget();
	if (!parent || !parent->isVisible() || !w->window() || !w->window()->isVisible())
		return true;
	// Someone else's effect on this widget: leave it alone.
	QGraphicsEffect *e = w->graphicsEffect();
	if (e && e->objectName() != QLatin1String(kEffect))
		return true;
	return false;
}

void settle(QWidget *w, bool show)
{
	dropAnim(w);
	clearEffect(w);
	freeSize(w);
	if (w->isHidden() == show)
		w->setVisible(show);
}

// How big the widget wants to be when it's all the way open.
int naturalSize(QWidget *w, bool horizontal)
{
	QSize hint = w->sizeHint().expandedTo(w->minimumSizeHint());
	int full = horizontal ? hint.width() : hint.height();
	if (!horizontal && w->hasHeightForWidth()) {
		int width = w->width() > 1 ? w->width() : (w->parentWidget() ? w->parentWidget()->width() : hint.width());
		full = std::max(full, w->heightForWidth(width));
	}
	return std::max(full, 1);
}

// One animation drives both: v goes 0 (gone) to 1 (all the way shown).
void run(QWidget *w, bool show, bool fold, bool horizontal, int ms)
{
	if (!w)
		return;
	QVariantAnimation *cur = animOf(w);
	if (cur) {
		if (cur->property("vmTarget").toBool() == show)
			return; // already on its way there
	} else if (!w->isHidden() == show) {
		return; // already there
	}
	if (instant(w)) {
		settle(w, show);
		return;
	}

	const bool shown = !w->isHidden();
	const qreal from = cur ? cur->currentValue().toReal() : (shown ? 1.0 : 0.0);
	int full = 1;
	if (fold) {
		freeSize(w);
		full = naturalSize(w, horizontal);
		if (shown && !cur)
			full = std::max(full, horizontal ? w->width() : w->height());
	}
	dropAnim(w);
	clearEffect(w);

	auto *effect = new QGraphicsOpacityEffect(w);
	effect->setObjectName(QString::fromLatin1(kEffect));
	QPointer<QWidget> pw(w);
	QPointer<QGraphicsOpacityEffect> pe(effect);
	auto apply = [pw, pe, fold, horizontal, full](qreal v) {
		if (!pw || !pe)
			return;
		v = std::clamp(v, 0.0, 1.0);
		if (fold) {
			const int size = int(std::lround(full * v));
			if (horizontal)
				pw->setMaximumWidth(size);
			else
				pw->setMaximumHeight(size);
		}
		pe->setOpacity(fold ? std::min(1.0, v * 1.5) : v);
	};
	apply(from);
	w->setGraphicsEffect(effect);
	if (!shown)
		w->show();

	auto *a = new QVariantAnimation(w);
	a->setObjectName(QString::fromLatin1(kAnim));
	a->setProperty("vmTarget", show);
	a->setStartValue(from);
	a->setEndValue(show ? 1.0 : 0.0);
	a->setDuration(std::max(1, int(std::lround(ms * std::abs((show ? 1.0 : 0.0) - from)))));
	a->setEasingCurve(fold ? QEasingCurve::InOutCubic : QEasingCurve::OutCubic);
	QPointer<QWidget> guard(w);
	QObject::connect(a, &QVariantAnimation::valueChanged, a, [apply](const QVariant &v) { apply(v.toReal()); });
	QObject::connect(a, &QVariantAnimation::finished, a, [guard, show]() {
		if (guard)
			settle(guard, show);
	});
	a->start();
}

} // namespace

bool enabled()
{
#ifdef _WIN32
	BOOL on = TRUE;
	if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0))
		return on != FALSE;
#endif
	return true;
}

void fadeTo(QWidget *w, bool show, int ms)
{
	run(w, show, false, false, ms);
}

void foldTo(QWidget *w, bool show, bool horizontal, int ms)
{
	run(w, show, true, horizontal, ms);
}

void dip(QWidget *w, int ms)
{
	if (!w || animOf(w) || !w->isVisible() || instant(w))
		return;
	if (w->graphicsEffect())
		return;
	auto *effect = new QGraphicsOpacityEffect(w);
	effect->setObjectName(QString::fromLatin1(kEffect));
	effect->setOpacity(0.35);
	w->setGraphicsEffect(effect);
	auto *a = new QVariantAnimation(w);
	a->setObjectName(QString::fromLatin1(kAnim));
	a->setProperty("vmTarget", true);
	a->setStartValue(0.35);
	a->setEndValue(1.0);
	a->setDuration(ms);
	a->setEasingCurve(QEasingCurve::OutCubic);
	QPointer<QGraphicsOpacityEffect> e(effect);
	QPointer<QWidget> guard(w);
	QObject::connect(a, &QVariantAnimation::valueChanged, a, [e](const QVariant &v) {
		if (e)
			e->setOpacity(v.toReal());
	});
	QObject::connect(a, &QVariantAnimation::finished, a, [guard]() {
		if (guard)
			settle(guard, true);
	});
	a->start();
}

} // namespace vmotion
