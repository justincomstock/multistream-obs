// Moving in from other vertical tools, and Twitch Dual Format.
//   Aitum Vertical: its canvas lives in the scene collection; its scenes are
//     copied onto ours (sharing their sources), Aitum's own are left alone.
//   The Multistream Vertical tool (Lua script): its picture becomes one
//     vertical scene per main scene, framed the same way, followed
//     automatically by name.
#include "canvas.hpp"
#include "../i18n.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <graphics/vec2.h>
#include <util/config-file.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

obs_canvas_t *aitumCanvas() // new reference, or null
{
	obs_canvas_t *c = obs_get_canvas_by_name(AITUM_CANVAS);
	if (c && (obs_canvas_removed(c) || !obs_canvas_has_video(c))) {
		obs_canvas_release(c);
		return nullptr;
	}
	return c;
}

bool collect(void *p, obs_source_t *src)
{
	if (obs_source_is_scene(src))
		static_cast<std::vector<obs_source_t *> *>(p)->push_back(obs_source_get_ref(src));
	return true;
}

QString luaStatusFile()
{
	QString base = qEnvironmentVariable("APPDATA");
	if (!base.isEmpty())
		return base + "/Multistream/relay/obs-vertical.json";
	return QDir::homePath() + "/Library/Application Support/Multistream/relay/obs-vertical.json";
}

} // namespace

// The first-run scene, once other scenes replace it, if nothing was put in it.
void VerticalCanvas::removeIfEmpty(const char *name)
{
	obs_scene_t *s = findScene(QString::fromUtf8(name));
	if (!s)
		return;
	int items = 0;
	obs_scene_enum_items(
		s,
		[](obs_scene_t *, obs_sceneitem_t *, void *p) {
			++*static_cast<int *>(p);
			return false;
		},
		&items);
	obs_scene_release(s);
	QString ignored;
	if (!items && sceneNames().size() > 1)
		removeScene(QString::fromUtf8(name), ignored); // off air first, if it was on
}

QJsonObject VerticalCanvas::importState() const
{
	QJsonObject o;
	QStringList aitum;
	if (obs_canvas_t *c = aitumCanvas()) {
		std::vector<obs_source_t *> scenes;
		obs_canvas_enum_scenes(c, collect, &scenes);
		for (obs_source_t *s : scenes) {
			aitum << QString::fromUtf8(obs_source_get_name(s));
			obs_source_release(s);
		}
		obs_canvas_release(c);
	}
	QJsonArray a;
	for (const QString &n : aitum)
		a.append(QJsonObject{{"name", n}});
	o["aitumScenes"] = a;
	o["aitumLoaded"] = obs_get_module(AITUM_MODULE) != nullptr;

	// The Lua tool writes its status every second while OBS runs it.
	QFile f(luaStatusFile());
	QJsonObject lua;
	if (f.open(QIODevice::ReadOnly))
		lua = QJsonDocument::fromJson(f.readAll()).object();
	const qint64 at = qint64(lua.value("at").toDouble());
	const bool running = lua.value("client").toString() == "multistream-vertical-script" &&
			     QDateTime::currentSecsSinceEpoch() - at < 15;
	o["luaRunning"] = running;
	o["luaSending"] = running && lua.value("sending").toBool();
	o["luaFraming"] = lua.value("framing").toString("fill");
	o["luaPosition"] = lua.contains("position") ? lua.value("position").toInt() : 50;
	return o;
}

int VerticalCanvas::importAitum(QString &error)
{
	if (!canvas_) {
		error = mt("Vertical.NotReady");
		return -1;
	}
	obs_canvas_t *c = aitumCanvas();
	if (!c) {
		error = mt("Vertical.NoAitumScenes");
		return -1;
	}
	std::vector<obs_source_t *> scenes;
	obs_canvas_enum_scenes(c, collect, &scenes);
	QStringList taken = sceneNames();
	int count = 0;
	for (obs_source_t *src : scenes) {
		const QString base = QString::fromUtf8(obs_source_get_name(src));
		QString n = base;
		for (int i = 2; taken.contains(n); i++)
			n = QString("%1 %2").arg(base).arg(i);
		// The copy starts on Aitum's canvas under a name free there too,
		// then moves to ours (with its groups) and takes the real name.
		const QByteArray tmp = (base + " (importing " + QString::number(QDateTime::currentMSecsSinceEpoch()) + ")").toUtf8();
		obs_scene_t *copy = obs_scene_duplicate(obs_scene_from_source(src), tmp.constData(), OBS_SCENE_DUP_REFS);
		if (copy) {
			obs_canvas_move_scene(copy, canvas_);
			obs_source_set_name(obs_scene_get_source(copy), n.toUtf8().constData());
			order_ << QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(copy)));
			taken << n;
			obs_scene_release(copy);
			count++;
		}
		obs_source_release(src);
	}
	obs_canvas_release(c);
	if (!count) {
		error = mt("Vertical.AitumEmpty");
		return -1;
	}
	removeIfEmpty(FIRST_SCENE_NAME);
	if (obs_scene_t *cur = findScene(scene_))
		obs_scene_release(cur);
	else {
		QString ignored;
		setScene(sceneNames().value(0), ignored);
	}
	obs_log(LOG_INFO, "vertical: imported %d scenes from Aitum Vertical", count);
	save();
	changed();
	return count;
}

int VerticalCanvas::importLuaTool(const QString &framing, int position, QString &error)
{
	if (!canvas_) {
		error = mt("Vertical.NotReady");
		return -1;
	}
	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	const float sw = float(ovi.base_width), sh = float(ovi.base_height);
	if (sw <= 0 || sh <= 0) {
		error = mt("Relay.VideoNotReady");
		return -1;
	}
	const float W = float(width_), H = float(height_);
	const bool fit = framing == "fit";
	const float scale = fit ? std::min(W / sw, H / sh) : std::max(W / sw, H / sh);
	const float pos = float(std::clamp(position, 0, 100)) / 100.0f;
	const float x = fit ? (W - sw * scale) / 2 : -(sw * scale - W) * pos;
	const float y = (H - sh * scale) / 2;

	// One vertical scene per main scene, named the same so it follows it.
	struct obs_frontend_source_list mains = {};
	obs_frontend_get_scenes(&mains);
	const QStringList have = sceneNames();
	int count = 0;
	for (size_t i = 0; i < mains.sources.num; i++) {
		obs_source_t *main = mains.sources.array[i];
		const QString name = QString::fromUtf8(obs_source_get_name(main));
		if (have.contains(name))
			continue;
		QString ignored;
		if (!createScene(name, ignored))
			continue;
		obs_scene_t *s = findScene(name);
		obs_sceneitem_t *item = s ? obs_scene_add(s, main) : nullptr;
		if (item) {
			obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
			struct vec2 v;
			vec2_set(&v, scale, scale);
			obs_sceneitem_set_scale(item, &v);
			vec2_set(&v, x, y);
			obs_sceneitem_set_pos(item, &v);
			count++;
		}
		obs_scene_release(s);
	}
	obs_frontend_source_list_free(&mains);
	setFollowMain(true);
	// The first-run "Vertical scene" is empty: the imported ones replace it.
	if (count) {
		removeIfEmpty(FIRST_SCENE_NAME);
		followMainScene();
	}
	if (!count && !have.isEmpty())
		error = mt("Vertical.AllHaveScenes");
	obs_log(LOG_INFO, "vertical: made %d scenes from the Vertical tool's framing (%s, %d)", count,
		framing.toUtf8().constData(), position);
	save();
	changed();
	return count;
}

// ---- Twitch Dual Format ----------------------------------------------------------
// OBS's Enhanced Broadcasting sends one extra canvas to Twitch, chosen in its
// profile (Settings -> Stream). Ours can be that canvas.

QJsonObject VerticalCanvas::dualFormatState() const
{
	QJsonObject o;
	config_t *cfg = obs_frontend_get_profile_config();
	const char *extra = cfg ? config_get_string(cfg, "Stream1", "MultitrackExtraCanvas") : nullptr;
	o["enhanced"] = cfg && config_get_bool(cfg, "Stream1", "EnableMultitrackVideo");
	o["on"] = canvas_ && extra && strcmp(extra, obs_canvas_get_uuid(canvas_)) == 0;
	return o;
}

bool VerticalCanvas::setDualFormat(bool on, QString &error)
{
	if (!canvas_) {
		error = mt("Vertical.NotReady");
		return false;
	}
	if (obs_frontend_streaming_active()) {
		error = mt("Vertical.ChangeAfterStream");
		return false;
	}
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg) {
		error = mt("Vertical.NoProfile");
		return false;
	}
	config_set_string(cfg, "Stream1", "MultitrackExtraCanvas", on ? obs_canvas_get_uuid(canvas_) : "");
	config_save_safe(cfg, "tmp", nullptr);
	changed();
	return true;
}
