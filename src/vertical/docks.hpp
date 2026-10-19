#pragma once

class VerticalCanvas;

// The vertical canvas's docks, next to OBS's own Scenes dock and preview:
//   Vertical Scenes   the vertical scene list: click to put one on air;
//                     +, -, up/down; right-click to rename, duplicate,
//                     remove, and pick which main scenes it shows with
//   Vertical Preview  what the vertical stream shows, letterboxed
// They redraw when VerticalCanvas::version() changes. UI thread only.
namespace vertical_docks {

void create(VerticalCanvas *canvas); // obs_module_load
void showOnFirstRun();                // FINISHED_LOADING
void close();                         // EXIT

} // namespace vertical_docks
