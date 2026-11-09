#include "events.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSysInfo>

#include <deque>
#include <mutex>

namespace {

struct Event {
	QString kind;
	QString detail;
	qint64 at = 0;
	int count = 1;
};

const size_t MAX_WAITING = 50;
const qint64 SAME_KIND_MS = 30 * 1000;

std::mutex lock;
std::deque<Event> waiting;

} // namespace

namespace msevents {

void note(const char *kind, const QString &detail, bool warning)
{
	const QString k = QString::fromUtf8(kind ? kind : "event");
	const QString d = detail.left(300);
	obs_log(warning ? LOG_WARNING : LOG_INFO, "[%s] %s", k.toUtf8().constData(), d.toUtf8().constData());
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	std::lock_guard<std::mutex> g(lock);
	for (auto it = waiting.rbegin(); it != waiting.rend(); ++it) {
		if (it->kind == k && now - it->at < SAME_KIND_MS) {
			it->count++;
			it->detail = d;
			return;
		}
	}
	waiting.push_back({k, d, now, 1});
	while (waiting.size() > MAX_WAITING)
		waiting.pop_front();
}

QJsonArray take()
{
	QJsonArray out;
	std::lock_guard<std::mutex> g(lock);
	for (const Event &e : waiting) {
		QJsonObject o;
		o["kind"] = e.kind;
		o["detail"] = e.detail;
		o["at"] = double(e.at);
		if (e.count > 1)
			o["count"] = e.count;
		out.append(o);
	}
	waiting.clear();
	return out;
}

QString osName()
{
	return QSysInfo::prettyProductName() + " " + QSysInfo::currentCpuArchitecture();
}

} // namespace msevents
