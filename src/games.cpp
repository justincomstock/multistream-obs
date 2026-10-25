#include "games.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>

#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include <unistd.h>
#endif

namespace {

const int MAX_WINDOWS = 80;
// OBS's window capture: match the window by its program first (the title
// of a game often changes as it plays), window-helpers' WINDOW_PRIORITY_EXE.
const int PRIORITY_EXE = 2;

struct Win {
	QString exe;   // as Windows names it: "eldenring.exe" (macOS: the program file, "Celeste")
	QString path;  // the program's full path
	QString title;
	QString cls;
	QString bundle; // macOS: the app's bundle id, what OBS's Mac capture follows
	QString name;  // what to call the game
	bool gameLike = false;
	bool foreground = false;
	bool minimized = false;
	bool fullscreen = false;
	qint64 started = 0; // when the program started, ms since 1970
	qint64 area = 0;
};

std::vector<Win> windows;
QStringList followSources;
QString followExe;

// Where games get installed: a program under one of these is a game (its
// folder is the game's name).
const char *LIBRARIES[] = {
	"\\steamapps\\common\\", "\\epic games\\",      "\\riot games\\",
	"\\xboxgames\\",         "\\gog galaxy\\games\\", "\\gog games\\",
	"\\ubisoft game launcher\\games\\", "\\ea games\\", "\\origin games\\",
	"\\rockstar games\\",    "\\itch\\apps\\",      "\\amazon games\\library\\",
	"\\battle.net\\games\\",
};
// Programs that are never the game, even in full screen or under a game's folder.
const QSet<QString> NOT_GAMES = {
	"explorer.exe", "chrome.exe", "msedge.exe", "firefox.exe", "opera.exe", "opera_gx.exe", "brave.exe",
	"vivaldi.exe", "discord.exe", "obs64.exe", "obs32.exe", "vlc.exe", "mpc-hc64.exe", "mpc-be64.exe",
	"potplayermini64.exe", "spotify.exe", "code.exe", "devenv.exe", "applicationframehost.exe",
	"textinputhost.exe", "searchhost.exe", "shellexperiencehost.exe", "startmenuexperiencehost.exe",
	"lockapp.exe", "systemsettings.exe", "msedgewebview2.exe", "electron.exe", "multistream.exe",
	"powerpnt.exe", "wmplayer.exe", "microsoft.photos.exe", "steam.exe", "steamwebhelper.exe",
	"epicgameslauncher.exe", "eadesktop.exe", "battle.net.exe", "riotclientux.exe", "riotclientservices.exe",
	"galaxyclient.exe", "upc.exe", "ubisoftconnect.exe", "playnite.desktopapp.exe", "medal.exe",
	"nvidia app.exe", "nvcontainer.exe", "taskmgr.exe", "notepad.exe", "cmd.exe", "windowsterminal.exe",
	"powershell.exe", "pwsh.exe", "claude.exe", "zoom.exe", "teams.exe", "ms-teams.exe", "slack.exe",
	// macOS programs (no .exe there).
	"finder", "safari", "google chrome", "firefox", "microsoft edge", "brave browser", "arc", "opera", "discord",
	"obs", "spotify", "music", "vlc", "iina", "quicktime player", "code", "electron", "multistream", "steam",
	"steam helper", "epicgameslauncher", "battle.net", "terminal", "iterm2", "claude", "zoom.us", "slack",
	"microsoft teams", "messages", "mail", "notes", "preview", "system settings", "activity monitor", "dock",
	"windowserver", "controlcenter", "notificationcenter", "loginwindow", "screencaptureui",
};
// Parts of a program's name that mean a launcher, an installer or a helper.
const char *HELPERS[] = {"launcher", "crashhandler", "crashreport", "setup", "installer", "updater",
			 "unins", "redist", "easyanticheat", "battleye", "helper", "bootstrap"};

QString str(const char *s)
{
	return QString::fromUtf8(s ? s : "");
}

bool isHelper(const QString &exe)
{
	for (const char *h : HELPERS)
		if (exe.contains(QLatin1String(h)))
			return true;
	return false;
}

// "D:\Steam\steamapps\common\ELDEN RING\Game\eldenring.exe" -> "ELDEN RING".
// macOS paths ("/Users/x/Library/Application Support/Steam/steamapps/common/
// Celeste/...") are read with "\" for "/" so the same list works.
QString libraryFolder(const QString &original)
{
	QString path = original;
	path.replace('/', '\\');
	const QString lower = path.toLower();
	for (const char *lib : LIBRARIES) {
		const int at = lower.indexOf(QLatin1String(lib));
		if (at < 0)
			continue;
		const int from = at + int(strlen(lib));
		const int end = path.indexOf('\\', from);
		if (end > from)
			return path.mid(from, end - from);
	}
	return QString();
}

// OBS's window setting: "title:class:exe", each part with ":" as "#3A"
// (libobs util/windows/window-helpers.c encode_dstr).
QString obsWindowValue(const Win &w)
{
	auto enc = [](QString s) { return s.replace("#", "#22").replace(":", "#3A"); };
	return enc(w.title) + ":" + enc(w.cls) + ":" + enc(w.exe);
}

#ifdef _WIN32
QString processPath(DWORD pid, qint64 *started)
{
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h)
		return QString();
	wchar_t buf[MAX_PATH * 2];
	DWORD size = DWORD(sizeof(buf) / sizeof(buf[0]));
	QString path;
	if (QueryFullProcessImageNameW(h, 0, buf, &size))
		path = QString::fromWCharArray(buf, int(size));
	FILETIME created, exited, kernel, user;
	if (started && GetProcessTimes(h, &created, &exited, &kernel, &user)) {
		ULARGE_INTEGER t;
		t.LowPart = created.dwLowDateTime;
		t.HighPart = created.dwHighDateTime;
		// 100 ns ticks since 1601 -> ms since 1970.
		*started = qint64(t.QuadPart / 10000ULL) - 11644473600000LL;
	}
	CloseHandle(h);
	return path;
}

BOOL CALLBACK onWindow(HWND hwnd, LPARAM param)
{
	auto *found = reinterpret_cast<QMap<QString, Win> *>(param);
	if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
		return TRUE;
	const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if (ex & WS_EX_TOOLWINDOW)
		return TRUE;
	wchar_t title[512];
	const int len = GetWindowTextW(hwnd, title, 512);
	if (len <= 0)
		return TRUE;
	wchar_t cls[256];
	if (!GetClassNameW(hwnd, cls, 256))
		return TRUE;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (!pid || pid == GetCurrentProcessId())
		return TRUE;
	Win w;
	w.path = processPath(pid, &w.started);
	if (w.path.isEmpty())
		return TRUE;
	w.exe = QFileInfo(w.path).fileName();
	const QString key = w.exe.toLower();
	w.title = QString::fromWCharArray(title, len).trimmed();
	w.cls = QString::fromWCharArray(cls);
	w.minimized = IsIconic(hwnd);
	w.foreground = GetForegroundWindow() == hwnd;
	RECT r;
	if (!w.minimized && GetWindowRect(hwnd, &r)) {
		w.area = qint64(r.right - r.left) * qint64(r.bottom - r.top);
		MONITORINFO mi = {};
		mi.cbSize = sizeof(mi);
		if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
			w.fullscreen = r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
				       r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
	}
	const QString folder = libraryFolder(w.path);
	const bool minecraft = (key == "javaw.exe" || key == "java.exe") && w.title.startsWith("Minecraft");
	if (!NOT_GAMES.contains(key) && !isHelper(key))
		w.gameLike = !folder.isEmpty() || w.fullscreen || minecraft;
	w.name = minecraft ? QStringLiteral("Minecraft") : !folder.isEmpty() ? folder : w.title.left(60);
	// One entry per program: its main window (on screen, in front, biggest).
	auto it = found->find(key);
	if (it != found->end()) {
		const Win &o = it.value();
		const bool better = (o.minimized && !w.minimized) || (!o.foreground && w.foreground) ||
				    (o.minimized == w.minimized && o.foreground == w.foreground && w.area > o.area);
		if (!better) {
			it.value().gameLike = o.gameLike || w.gameLike;
			return TRUE;
		}
		w.gameLike = w.gameLike || o.gameLike;
	}
	(*found)[key] = w;
	return found->size() < MAX_WINDOWS;
}
#elif defined(__APPLE__)
QString cfString(CFStringRef s)
{
	if (!s)
		return QString();
	const CFIndex len = CFStringGetLength(s);
	std::vector<UniChar> buf(static_cast<size_t>(len));
	CFStringGetCharacters(s, CFRangeMake(0, len), buf.data());
	return QString(reinterpret_cast<const QChar *>(buf.data()), int(len));
}

QString dictString(CFDictionaryRef d, CFStringRef key)
{
	const void *v = CFDictionaryGetValue(d, key);
	return v && CFGetTypeID(v) == CFStringGetTypeID() ? cfString(static_cast<CFStringRef>(v)) : QString();
}

qint64 dictInt(CFDictionaryRef d, CFStringRef key)
{
	const void *v = CFDictionaryGetValue(d, key);
	qint64 n = 0;
	if (v && CFGetTypeID(v) == CFNumberGetTypeID())
		CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt64Type, &n);
	return n;
}

// "/Applications/Celeste.app/Contents/MacOS/Celeste" -> the bundle id in
// Celeste.app's Info.plist, which OBS's Mac capture follows an app by.
QString bundleIdOf(const QString &path)
{
	const int at = path.indexOf(".app/");
	if (at < 0)
		return QString();
	const QByteArray app = path.left(at + 4).toUtf8();
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8 *>(app.constData()),
								CFIndex(app.size()), true);
	if (!url)
		return QString();
	QString id;
	if (CFBundleRef b = CFBundleCreate(nullptr, url)) {
		id = cfString(CFBundleGetIdentifier(b));
		CFRelease(b);
	}
	CFRelease(url);
	return id;
}

// The windows on screen, front to back (CGWindowList's order): the first
// normal window is the one in front. Minimized windows aren't on screen here.
void scanMac(QMap<QString, Win> &found)
{
	CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
						     kCGNullWindowID);
	if (!list)
		return;
	const pid_t self = getpid();
	bool front = true;
	for (CFIndex i = 0; i < CFArrayGetCount(list) && found.size() < MAX_WINDOWS; i++) {
		auto d = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
		if (dictInt(d, kCGWindowLayer) != 0)
			continue; // menus, the Dock, overlays
		const pid_t pid = pid_t(dictInt(d, kCGWindowOwnerPID));
		if (pid <= 0 || pid == self)
			continue;
		CGRect r = CGRectZero;
		const void *bounds = CFDictionaryGetValue(d, kCGWindowBounds);
		if (!bounds || CFGetTypeID(bounds) != CFDictionaryGetTypeID() ||
		    !CGRectMakeWithDictionaryRepresentation(static_cast<CFDictionaryRef>(bounds), &r) || r.size.width < 100 ||
		    r.size.height < 100)
			continue;
		char buf[PROC_PIDPATHINFO_MAXSIZE];
		if (proc_pidpath(pid, buf, sizeof(buf)) <= 0)
			continue;
		Win w;
		w.path = QString::fromUtf8(buf);
		w.exe = QFileInfo(w.path).fileName();
		const QString key = w.exe.toLower();
		w.title = dictString(d, kCGWindowName).trimmed(); // empty without Screen Recording access
		const QString owner = dictString(d, kCGWindowOwnerName);
		if (w.title.isEmpty())
			w.title = owner;
		w.cls = owner;
		w.bundle = bundleIdOf(w.path);
		w.foreground = front;
		front = false;
		w.area = qint64(r.size.width) * qint64(r.size.height);
		CGDirectDisplayID shown = 0;
		uint32_t count = 0;
		if (CGGetDisplaysWithRect(r, 1, &shown, &count) == kCGErrorSuccess && count) {
			const CGRect s = CGDisplayBounds(shown);
			w.fullscreen = r.origin.x <= s.origin.x && r.origin.y <= s.origin.y &&
				       r.origin.x + r.size.width >= s.origin.x + s.size.width &&
				       r.origin.y + r.size.height >= s.origin.y + s.size.height;
		}
		struct proc_bsdinfo info = {};
		if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == int(sizeof(info)))
			w.started = qint64(info.pbi_start_tvsec) * 1000 + qint64(info.pbi_start_tvusec) / 1000;
		const QString folder = libraryFolder(w.path);
		const bool minecraft = key == "java" && w.title.startsWith("Minecraft");
		if (!NOT_GAMES.contains(key) && !isHelper(key) && !w.path.startsWith("/System/"))
			w.gameLike = !folder.isEmpty() || w.fullscreen || minecraft;
		w.name = minecraft ? QStringLiteral("Minecraft") : !folder.isEmpty() ? folder : w.title.left(60);
		// One entry per program: the first (front-most) of its windows.
		auto it = found.find(key);
		if (it != found.end()) {
			it.value().gameLike = it.value().gameLike || w.gameLike;
			continue;
		}
		found[key] = w;
	}
	CFRelease(list);
}
#endif

void scan()
{
	windows.clear();
	QMap<QString, Win> found;
#ifdef _WIN32
	EnumWindows(onWindow, reinterpret_cast<LPARAM>(&found));
#elif defined(__APPLE__)
	scanMac(found);
#endif
	for (const Win &w : found)
		windows.push_back(w);
}

// The sources that can follow a game: Windows' window and game capture;
// macOS's screen capture (ScreenCaptureKit), which follows an app.
bool isCapture(obs_source_t *s)
{
	const char *id = obs_source_get_unversioned_id(s);
	return id && (strcmp(id, "window_capture") == 0 || strcmp(id, "game_capture") == 0 ||
		      strcmp(id, "screen_capture") == 0);
}

#ifdef __APPLE__
// macOS screen capture's "type": capture one application (mac-sck-common.h).
const long long SCK_APPLICATION = 2;
#endif

// Points the following sources at the game's window (only when it changed,
// and never at a minimized window: the source keeps the game it had).
void keepFollowing()
{
	if (followExe.isEmpty() || followSources.isEmpty())
		return;
	const Win *w = nullptr;
	for (const Win &x : windows)
		if (x.exe.compare(followExe, Qt::CaseInsensitive) == 0)
			w = &x;
	if (!w || w->minimized)
		return;
	const QString value = obsWindowValue(*w);
	for (const QString &name : followSources) {
		obs_source_t *s = obs_get_source_by_name(name.toUtf8().constData());
		if (!s)
			continue;
#ifdef __APPLE__
		// The app, not one of its windows: a game's window changes as it loads.
		if (isCapture(s) && strcmp(obs_source_get_unversioned_id(s), "screen_capture") == 0 && !w->bundle.isEmpty()) {
			obs_data_t *d = obs_source_get_settings(s);
			const bool same = obs_data_get_int(d, "type") == SCK_APPLICATION &&
					  str(obs_data_get_string(d, "application")) == w->bundle;
			if (!same) {
				obs_data_set_int(d, "type", SCK_APPLICATION);
				obs_data_set_string(d, "application", w->bundle.toUtf8().constData());
				obs_source_update(s, d);
				obs_log(LOG_INFO, "games: '%s' now captures %s", name.toUtf8().constData(),
					w->bundle.toUtf8().constData());
			}
			obs_data_release(d);
			obs_source_release(s);
			continue;
		}
#endif
		if (isCapture(s) && strcmp(obs_source_get_unversioned_id(s), "screen_capture") != 0) {
			obs_data_t *d = obs_source_get_settings(s);
			const bool game = strcmp(obs_source_get_unversioned_id(s), "game_capture") == 0;
			const bool same = str(obs_data_get_string(d, "window")) == value &&
					  obs_data_get_int(d, "priority") == PRIORITY_EXE &&
					  (!game || str(obs_data_get_string(d, "capture_mode")) == "window");
			if (!same) {
				obs_data_set_string(d, "window", value.toUtf8().constData());
				obs_data_set_int(d, "priority", PRIORITY_EXE);
				if (game)
					obs_data_set_string(d, "capture_mode", "window");
				obs_source_update(s, d);
				obs_log(LOG_INFO, "games: '%s' now captures %s", name.toUtf8().constData(),
					w->exe.toUtf8().constData());
			}
			obs_data_release(d);
		}
		obs_source_release(s);
	}
}

QString configFile()
{
	char *f = obs_module_config_path("games.json");
	QString p = str(f);
	bfree(f);
	return p;
}

void save()
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	QJsonArray a;
	for (const QString &n : followSources)
		a.append(n);
	QJsonObject o;
	o["follow"] = a;
	QFile f(configFile());
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write(QJsonDocument(o).toJson());
}

// The older stand-alone plugin (Game Window Switcher): its games, to bring
// over, and whether it's still loaded (the two would take turns). Only the
// names, programs, titles and categories: never its tokens.
QJsonObject legacy()
{
	QJsonObject out;
	char *dir = obs_module_config_path("");
	const QString file = QDir(str(dir)).absoluteFilePath("../obs-game-window-switcher/config.json");
	bfree(dir);
	QFile f(file);
	QJsonArray list;
	if (f.open(QIODevice::ReadOnly)) {
		const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
		for (const QJsonValue &v : root.value("profiles").toArray()) {
			const QJsonObject p = v.toObject();
			QJsonArray exes;
			for (const QJsonValue &e : p.value("executables").toArray()) {
				const QString n = e.isObject() ? e.toObject().value("name").toString() : e.toString();
				if (n.endsWith(".exe", Qt::CaseInsensitive))
					exes.append(n);
			}
			QJsonObject g;
			g["name"] = p.value("name").toString().left(60);
			g["exes"] = exes;
			g["title"] = p.value("twitch_title").toString().left(140);
			g["twitchCategory"] = p.value("twitch_category").toString().left(100);
			g["twitchCategoryId"] = p.value("twitch_category_id").toString().left(20);
			g["kickCategory"] = p.value("kick_category").toString().left(100);
			g["kickCategoryId"] = p.value("kick_category_id").toString().left(20);
			list.append(g);
		}
		out["found"] = true;
	} else {
		out["found"] = false;
	}
	out["games"] = list;
	out["loaded"] = obs_get_module("obs-game-window-switcher") != nullptr;
	return out;
}

} // namespace

void games::load()
{
	QFile f(configFile());
	if (!f.open(QIODevice::ReadOnly))
		return;
	followSources.clear();
	for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).object().value("follow").toArray())
		if (!v.toString().isEmpty())
			followSources.append(v.toString());
}

void games::tick()
{
	scan();
	keepFollowing();
}

QJsonObject games::state()
{
	QJsonArray ws;
	for (const Win &w : windows) {
		QJsonObject o;
		o["exe"] = w.exe;
		o["name"] = w.name;
		o["gameLike"] = w.gameLike;
		o["foreground"] = w.foreground;
		o["minimized"] = w.minimized;
		o["started"] = double(w.started);
		ws.append(o);
	}
	QJsonArray sources;
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			if (isCapture(s)) {
				QJsonObject o;
				o["name"] = str(obs_source_get_name(s));
				o["kind"] = str(obs_source_get_unversioned_id(s));
				static_cast<QJsonArray *>(param)->append(o);
			}
			return true;
		},
		&sources);
	QJsonArray follow;
	for (const QString &n : followSources)
		follow.append(n);
	QJsonObject o;
	o["windows"] = ws;
	o["sources"] = sources;
	o["follow"] = follow;
	o["following"] = followExe;
	o["legacy"] = legacy();
	return o;
}

void games::follow(const QString &exe)
{
	followExe = exe.trimmed();
	keepFollowing();
}

void games::setFollowSources(const QStringList &names)
{
	followSources.clear();
	for (const QString &n : names)
		if (!n.trimmed().isEmpty() && !followSources.contains(n))
			followSources.append(n);
	save();
	keepFollowing();
}
