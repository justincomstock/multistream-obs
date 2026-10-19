#pragma once

#include <QJsonObject>
#include <QString>

// What the dock needs to know about OBS, and the few OBS settings it may
// change. Everything here runs on OBS's UI thread.
namespace obsstate {

// The stream, the stream service, the canvas and the encoder settings.
QJsonObject snapshot();

// Every second: works out the upload speed from the output's byte count.
void tick();

// OBS's frontend events (enum obs_frontend_event).
void onEvent(int event);

// Points OBS's own stream at `server` + `key` (a Custom service). The
// service it replaces is kept in the plugin's config (previous-service.json).
bool setService(const QString &server, const QString &key, QString &error);

// The stream encoder's bitrate (kbps) and keyframe interval (seconds); 0
// leaves one as it is. Simple output mode always sends a keyframe every 2 s.
bool setEncoder(int bitrate, int keyintSec, QString &error);

} // namespace obsstate
