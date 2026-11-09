#pragma once

#include <QJsonObject>
#include <QString>

struct obs_scene; // obs_scene_t (obs.h)

// What the dock needs to know about OBS, and the few OBS settings it may
// change. Everything here runs on OBS's UI thread.
namespace obsstate {

// The stream, the stream service, the canvas and the encoder settings.
QJsonObject snapshot();

// How many things a scene shows a picture of: visible items with a size,
// through groups and nested scenes. 0 means viewers would see a black
// screen (nothing added, all hidden, or a window capture with no window).
int shownSources(obs_scene *scene);

// Every second: works out the upload speed from the output's byte count.
void tick();

// OBS's frontend events (enum obs_frontend_event).
void onEvent(int event);

// Points OBS's own stream at `server` + `key` (a Custom service). The
// service it replaces is kept in the plugin's config (previous-service.json).
bool setService(const QString &server, const QString &key, QString &error);

// Points OBS at Twitch with this stream key, as Settings > Stream would
// (OBS's own Twitch service, so Twitch's quality options keep working).
bool setTwitchService(const QString &key, QString &error);

// Puts back the service setService replaced (previous-service.json), then
// forgets it.
bool restoreService(QString &error);

// The stream encoder's bitrate (kbps) and keyframe interval (seconds); 0
// leaves one as it is. Simple output mode always sends a keyframe every 2 s.
bool setEncoder(int bitrate, int keyintSec, QString &error);

} // namespace obsstate
