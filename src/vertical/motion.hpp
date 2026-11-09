#pragma once
// Small fades and folds for the Vertical docks, so a part that comes or goes
// with a state doesn't pop (docs/design-rules.md section 7: 160ms for fades,
// 240ms for height and width, easing like the panel's). Practical only: the
// docks are native Qt, so just the parts that appear and disappear.
//
// Rules, all kept here so callers can ignore them:
//  - Asking for where a widget already is (or is already going) does nothing,
//    so a status poll can call these every second without restarting anything.
//  - The opacity effect exists only while a change is moving; at rest the
//    widget has none (an effect left on makes a widget draw to a pixmap).
//  - Never use them on the preview or on a browser (CEF) widget: a native
//    window can't sit under an opacity effect.
//  - When Windows' "animation effects" setting is off, or the widget isn't on
//    screen, the change is instant.

class QWidget;

namespace vmotion {

// False when the person turned animations off in Windows (always true elsewhere).
bool enabled();

// Show or hide with a fade (the widget's place in the layout appears or goes at once).
void fadeTo(QWidget *w, bool show, int ms = 160);

// Show or hide by growing or shrinking (height, or width when horizontal) while it fades.
void foldTo(QWidget *w, bool show, bool horizontal = false, int ms = 240);

// Words that changed on a label or button: a short dip and return, so the new
// words fade in. Does nothing while the widget is already fading or folding.
void dip(QWidget *w, int ms = 160);

} // namespace vmotion
