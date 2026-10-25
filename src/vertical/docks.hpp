#pragma once

#include <QJsonObject>

#include <functional>

class VerticalCanvas;

// The vertical canvas's docks, next to OBS's own Scenes dock and preview:
//   Vertical Scenes   the vertical scene list: click to put one on air;
//                     +, -, up/down; right-click to rename, duplicate,
//                     remove, and pick which main scenes it shows with
//   Vertical Preview  what the vertical stream shows, letterboxed
// They redraw when VerticalCanvas::version() changes. UI thread only.
// Their look is the Multistream dock's (theme.hpp).
namespace vertical_docks {

// obs_module_load. `live`: the vertical output's state (RelayOutput::state),
// for the pill over the preview.
void create(VerticalCanvas *canvas, std::function<QJsonObject()> live);
void showOnFirstRun();                // FINISHED_LOADING
void close();                         // EXIT
// For the tests (websocket.cpp "docks"): every dock in OBS's window, its
// size and minimum, so they can tell the dividers can still move; and the
// Vertical Scenes rows' tags ("SHOWING", "ON AIR").
QJsonObject report();

} // namespace vertical_docks
