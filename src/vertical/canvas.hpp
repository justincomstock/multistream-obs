#pragma once

#include "audio-mix.hpp"

class VerticalRecorder;

#include <obs.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>

inline constexpr const char *FIRST_SCENE_NAME = "Vertical scene";
// Aitum Vertical's canvas and module (file) name, for importing its scenes.
inline constexpr const char *AITUM_CANVAS = "Aitum Vertical";
inline constexpr const char *AITUM_MODULE = "vertical-canvas";

// The vertical (9:16) canvas: an OBS 31.1 canvas added through the
// frontend, so OBS saves its scenes in the scene collection like the main
// canvas's. It isn't flagged MIX_AUDIO: its sound goes only to the
// vertical stream, through VerticalAudio.
//
// Everything here runs on OBS's UI thread.
class VerticalCanvas {
public:
	void load();       // the plugin's config (size, per-collection choices)
	void attach();     // finds or adds the canvas for the current collection
	void detach();     // before the collection goes away, and at exit
	// At unload, after the vertical output is released (its encoder reads
	// our audio).
	void shutdown();
	void tick();       // every second: audio sources, video retry
	void onFrontendEvent(int event);

	bool ready() const { return canvas_ && obs_canvas_has_video(canvas_); }
	// Not ready because OBS was streaming or recording when it had to start.
	bool waitsForOutputs() const { return videoWaits_ && !ready(); }
	video_t *video() const { return ready() ? obs_canvas_get_video(canvas_) : nullptr; }
	audio_t *audio() const { return audio_.audio(); }
	// Changes when the picture's size does.
	QString sig() const { return QString("%1x%2").arg(width_).arg(height_); }

	// In the streamer's order (the dock's up/down), new scenes last.
	QStringList sceneNames() const;
	// [{name, current, mains}]: objects, since obs-websocket's vendor
	// replies go through obs_data, which keeps only arrays of objects.
	QJsonArray sceneList() const;
	QString currentScene() const { return scene_; }
	// A vertical scene by name (new reference, or null).
	obs_scene_t *scene(const QString &name) const { return findScene(name); }
	bool setScene(const QString &name, QString &error);
	bool createScene(const QString &name, QString &error);
	bool renameScene(const QString &name, const QString &to, QString &error);
	// A copy whose items share the original's sources.
	bool duplicateScene(const QString &name, const QString &to, QString &error);
	bool removeScene(const QString &name, QString &error);
	bool moveScene(const QString &name, int delta, QString &error);
	// Adds an existing source to a vertical scene, scaled to fill it.
	bool addSource(const QString &scene, const QString &source, QString &error);

	// ---- the items in a vertical scene (items.cpp) ----
	// [{id, name, type, visible, locked, audio, group}], top first.
	QJsonArray sceneItems(const QString &scene) const;
	bool setItemVisible(const QString &scene, qint64 id, bool on, QString &error);
	bool setItemLocked(const QString &scene, qint64 id, bool on, QString &error);
	// movement: OBS_ORDER_MOVE_UP / _DOWN / _TOP / _BOTTOM
	bool moveItem(const QString &scene, qint64 id, int movement, QString &error);
	bool removeItem(const QString &scene, qint64 id, QString &error);
	// A new source of type `typeId` (shared with the main canvas, like any
	// source), added to the scene; `created` gets a new reference.
	bool addNewSource(const QString &scene, const QString &typeId, const QString &name, QString &error,
			  obs_source_t **created = nullptr);
	// {x, y, scaleX, scaleY, rot, boundsType, boundsW, boundsH, align,
	//  cropLeft, cropTop, cropRight, cropBottom}
	QJsonObject itemTransform(const QString &scene, qint64 id) const;
	bool setItemTransform(const QString &scene, qint64 id, const QJsonObject &t, QString &error);
	// A new vertical scene laid out for 9:16 from a main scene's items
	// (sharing their sources): the game or screen fills, a camera takes the
	// bottom, overlays and text stack where they fit. Editable afterwards.
	bool createFromMain(const QString &main, const QString &name, QString &error);

	// Follow the main canvas: when main scene `main` goes on air, show
	// vertical scene `scene` (empty: unlink). A main scene with no link
	// shows the vertical scene of the same name, if there is one. Links are
	// kept by UUID, so renaming either side keeps them.
	bool linkScene(const QString &main, const QString &scene, QString &error);
	QStringList linkedMains(const QString &scene) const;
	QString linkFor(const QString &main) const; // the vertical scene's name, or empty
	bool followMain() const { return followMain_; }
	void setFollowMain(bool on);

	// ---- moving in, Dual Format (migrate.cpp) ----
	// {aitumScenes: [{name}], aitumLoaded, luaRunning, luaSending, luaFraming, luaPosition}
	QJsonObject importState() const;
	// Copies Aitum Vertical's scenes onto this canvas. Returns how many, or -1.
	int importAitum(QString &error);
	// One vertical scene per main scene, framed like the Vertical tool
	// ("fill" with a 0..100 crop position, or "fit"). Returns how many, or -1.
	int importLuaTool(const QString &framing, int position, QString &error);
	// {enhanced, on}: this canvas as Twitch Enhanced Broadcasting's extra canvas.
	QJsonObject dualFormatState() const;
	bool setDualFormat(bool on, QString &error);

	QJsonObject audioState() const;
	bool setAudio(const QString &source, const QJsonObject &in, QString &error);

	QJsonObject state() const;

	// The canvas for the preview (new reference, or null). Its weak
	// reference is safe to keep on the graphics thread.
	obs_canvas_t *canvasRef() const { return canvas_ ? obs_canvas_get_ref(canvas_) : nullptr; }
	int width() const { return width_; }
	int height() const { return height_; }
	// Goes up whenever the scene list, the scene on air or a link changes,
	// so the docks know to redraw.
	quint64 version() const { return version_; }
	// Its recording and replay buffer (record.hpp), set by the plugin.
	VerticalRecorder *recorder = nullptr;
	// Called when the scene on air changes: (scene, previous).
	std::function<void(const QString &, const QString &)> onSceneChanged;

private:
	static void onCanvasCreate(void *data, calldata_t *cd);
	obs_scene_t *findScene(const QString &name) const; // new reference
	obs_source_t *findSceneByUuid(const QString &uuid) const; // new reference
	void showScene(obs_source_t *scene);
	void followMainScene();
	void resetVideo();
	void refreshAudio();
	void removeIfEmpty(const char *name);
	void ensureTransition();
	void releaseTransition();
	void registerSceneHotkeys();
	static void onSceneHotkey(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed);
	void save() const;
	void changed() { version_++; }
	QString collection() const;

	obs_canvas_t *canvas_ = nullptr; // our reference
	// The canvas shows this (private, same type as OBS's current
	// transition), which shows the scene: scene switches animate like OBS's.
	obs_source_t *transition_ = nullptr;
	QString transitionType_;
	// "Switch to vertical scene" hotkeys, per scene (saved with the scene).
	QMap<obs_hotkey_id, QString> sceneHotkeys_; // -> scene UUID
	VerticalAudio audio_;
	QString scene_;
	QStringList order_;               // vertical scene UUIDs
	QMap<QString, QString> links_;    // main scene UUID -> vertical scene UUID
	int width_ = 1080;
	int height_ = 1920;
	bool followMain_ = true;
	quint64 version_ = 1;
	bool videoWaits_ = false;
	// vertical.json: {width, height, followMain,
	//   collections: {name: {scene, order: [{uuid}], links: [{main, vertical}], audio}}}
	obs_data_t *config_ = nullptr;
};
