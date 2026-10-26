#pragma once

#include <obs.h>

#include <QtGlobal>

#include <functional>

class QMenu;
class QWidget;
class VerticalCanvas;

// A vertical scene item's right-click menu, the same in the Vertical Preview
// and the Vertical Sources list, laid out like OBS's: Properties, Filters,
// Rename, Interact | Copy, Duplicate | Order | Transform (edit, copy, paste,
// reset, rotate, flip, fit, fill, center) | Hide, Lock | Remove. It acts on
// the scene on air; dialogs and errors open over `parent`.
namespace vitem {

void addItemMenu(QMenu &menu, VerticalCanvas *canvas, qint64 id, QWidget *parent,
		 std::function<void()> removed = nullptr);
// "Paste" and "Paste (duplicate)" for empty space, when a source was copied.
void addPasteActions(QMenu &menu, VerticalCanvas *canvas, QWidget *parent);
// Asks for a new name (the source is shared with the main canvas); undoable.
void renameSource(QWidget *parent, VerticalCanvas *canvas, obs_source_t *src);
// Asks first; Edit -> Undo puts it back. True if it was removed.
bool askRemove(QWidget *parent, VerticalCanvas *canvas, qint64 id);
// OBS's Edit Transform window for a vertical item: exact position, size,
// rotation, alignment, bounding box and crop. Changes show as they're typed;
// the whole visit is one Edit -> Undo step.
void editTransform(QWidget *parent, VerticalCanvas *canvas, qint64 id);
// Edit -> Undo / Redo of OBS's main window, for docks that float in their own
// window (OBS's own Ctrl+Z only works in the main window).
void undo(bool redo);

} // namespace vitem
