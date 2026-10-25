#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

// Games: which programs are open on this PC, for the dock's game switching
// (cloud/web/obs-dock/games.mjs picks the game; the cloud sets the title and
// category, cloud/src/games.cjs). Capture sources the streamer picks follow
// the game's window. Everything here runs on OBS's UI thread.
namespace games {

// The capture sources that follow the game (plugin_config/multistream/games.json).
void load();

// Every 2 seconds: looks at the open windows again, and keeps the following
// sources on the game's window.
void tick();

// For /bridge/games: { windows: [{ exe, name, gameLike, foreground,
// minimized, started }], sources: [{ name, kind }], follow: [names],
// following, legacy: { found, loaded, games } }.
QJsonObject state();

// The program the dock says is being played ("" = leave the sources as they are).
void follow(const QString &exe);

// Which capture sources follow the game.
void setFollowSources(const QStringList &names);

} // namespace games
