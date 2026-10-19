#pragma once

#include <QString>

// The "Multistream" dock. Made when the module loads (so OBS puts it back
// where the streamer left it); its browser comes once OBS has finished
// loading, since obs-browser loads after this plugin.
namespace dock {
void create();
void loadPage(const QString &url);
void showOnFirstRun();
void close();
} // namespace dock
