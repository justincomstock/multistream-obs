#pragma once

#include <QString>

// Declared only, so outputs can note events without pulling in Qt's JSON
// (the offline tests build relay-output.cpp against small Qt stand-ins).
// bridge.cpp and events.cpp include <QJsonArray> themselves.
class QJsonArray;

// Problems worth the owner seeing (the stream stopped with an error, OBS
// kept reconnecting, Multistream couldn't be reached, the dock page fell
// back to its offline copy). Each one is written to the OBS log, and the
// last few wait here until the dock page collects them (GET /bridge/events)
// and sends them to multistream.gg's Server health page.
//
// Never put a stream key, token or chat text in `detail`. Safe from any
// thread (OBS output signals arrive on their own threads).
namespace msevents {

// `kind` is a short slug ("stream-stopped", "cant-reach", ...). The same
// kind is kept at most once every 30 s; repeats are counted on the last one.
void note(const char *kind, const QString &detail, bool warning = true);

// Everything waiting, oldest first, then cleared.
QJsonArray take();

// One-line OS name, for the startup log line and the events' sender.
QString osName();

} // namespace msevents
