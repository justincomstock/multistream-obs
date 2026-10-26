#include "audio-mix.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>

namespace {
// Two blocks (~43 ms at 48 kHz) are queued before a source is mixed, so a
// source that delivers a little late doesn't click; more than half a second
// queued means it has drifted ahead, and the oldest sound is dropped.
const size_t PRIME_FRAMES = 2 * AUDIO_OUTPUT_FRAMES;
} // namespace

VerticalAudio::~VerticalAudio()
{
	close();
}

bool VerticalAudio::open()
{
	if (audio_)
		return true;
	struct obs_audio_info oai = {};
	if (!obs_get_audio_info(&oai))
		return false;
	rate_ = oai.samples_per_sec;
	channels_ = get_audio_channels(oai.speakers);

	struct audio_output_info info = {};
	info.name = "Multistream Vertical audio";
	info.samples_per_sec = oai.samples_per_sec;
	info.format = AUDIO_FORMAT_FLOAT_PLANAR;
	info.speakers = oai.speakers;
	info.input_callback = onMix;
	info.input_param = this;
	if (audio_output_open(&audio_, &info) != AUDIO_OUTPUT_SUCCESS) {
		audio_ = nullptr;
		obs_log(LOG_WARNING, "vertical: couldn't open the vertical audio mix");
		return false;
	}
	return true;
}

void VerticalAudio::close()
{
	setSources({});
	if (audio_) {
		audio_output_close(audio_);
		audio_ = nullptr;
	}
}

void VerticalAudio::detach(const std::shared_ptr<Input> &in)
{
	if (obs_source_t *src = obs_weak_source_get_source(in->weak)) {
		// Returns once OBS isn't inside our callback (it holds the source's
		// callback lock while calling it).
		obs_source_remove_audio_capture_callback(src, onCapture, in.get());
		obs_source_release(src);
	}
	obs_weak_source_release(in->weak);
	in->weak = nullptr;
}

void VerticalAudio::setSources(const std::vector<obs_source_t *> &sources)
{
	std::vector<std::shared_ptr<Input>> keep, drop;
	{
		std::lock_guard<std::mutex> lock(inputsMutex_);
		for (const auto &in : inputs_) {
			const bool wanted = std::any_of(sources.begin(), sources.end(), [&](obs_source_t *s) {
				return obs_weak_source_references_source(in->weak, s);
			});
			(wanted ? keep : drop).push_back(in);
		}
	}
	std::vector<std::shared_ptr<Input>> added;
	for (obs_source_t *s : sources) {
		const bool have = std::any_of(keep.begin(), keep.end(), [&](const std::shared_ptr<Input> &in) {
			return obs_weak_source_references_source(in->weak, s);
		});
		if (have)
			continue;
		auto in = std::make_shared<Input>();
		in->owner = this;
		in->weak = obs_source_get_weak_source(s);
		in->uuid = obs_source_get_uuid(s);
		in->name = obs_source_get_name(s);
		added.push_back(in);
	}
	// Removed sources stop calling back before they leave the list (the mix
	// callback only reads the list under its lock).
	for (const auto &in : drop)
		detach(in);
	{
		std::lock_guard<std::mutex> lock(inputsMutex_);
		inputs_ = keep;
		inputs_.insert(inputs_.end(), added.begin(), added.end());
	}
	for (const auto &in : added) {
		if (obs_source_t *src = obs_weak_source_get_source(in->weak)) {
			obs_source_add_audio_capture_callback(src, onCapture, in.get());
			obs_source_release(src);
		}
	}
}

// OBS 33's monitoring switch, looked up at run time: the plugin builds
// against OBS 31.1, and on 33 the old type getter logs a deprecation warning
// on every call (this runs for every block of sound).
using MonitoringEnabledFn = bool (*)(const obs_source_t *);
static MonitoringEnabledFn monitoringEnabled()
{
#ifdef _WIN32
	static const auto fn = reinterpret_cast<MonitoringEnabledFn>(
		GetProcAddress(GetModuleHandleW(L"obs.dll"), "obs_source_get_monitoring_enabled"));
#else
	static const auto fn =
		reinterpret_cast<MonitoringEnabledFn>(dlsym(RTLD_DEFAULT, "obs_source_get_monitoring_enabled"));
#endif
	return fn;
}

bool isMonitorOnly(obs_source_t *source)
{
	if (MonitoringEnabledFn enabled = monitoringEnabled())
		return enabled(source) && obs_source_muted(source);
	return obs_source_get_monitoring_type(source) == OBS_MONITORING_TYPE_MONITOR_ONLY;
}

float VerticalAudio::gainFor(obs_source_t *source, const Input &in, bool muted) const
{
	// "Monitor only": the streamer hears it, the stream doesn't (music they can't
	// stream, for example). OBS's main mix leaves it out too.
	if (isMonitorOnly(source))
		return 0.0f;
	std::lock_guard<std::mutex> lock(settingsMutex_);
	auto it = settings_.find(in.uuid);
	if (it != settings_.end() && it->second.own)
		return it->second.muted ? 0.0f : it->second.volume;
	return muted ? 0.0f : obs_source_get_volume(source);
}

void VerticalAudio::onCapture(void *param, obs_source_t *source, const struct audio_data *data, bool muted)
{
	auto *in = static_cast<Input *>(param);
	if (!data || !data->frames)
		return;
	const float gain = in->owner->gainFor(source, *in, muted);
	in->gain.store(gain);
	in->offsetNs.store(obs_source_get_sync_offset(source));
	float peak = 0.0f;
	std::lock_guard<std::mutex> lock(in->m);
	// The sound arrives in OBS's own format: float planar, OBS's channels.
	if (!data->data[0])
		return;
	for (size_t ch = 0; ch < in->owner->channels_; ch++) {
		const float *src = reinterpret_cast<const float *>(data->data[ch] ? data->data[ch] : data->data[0]);
		auto &q = in->buf[ch];
		for (uint32_t i = 0; i < data->frames; i++) {
			const float v = src[i] * gain;
			q.push_back(v);
			peak = std::max(peak, std::fabs(v));
		}
	}
	in->peak.store(peak);
}

bool VerticalAudio::onMix(void *param, uint64_t start_ts, uint64_t, uint64_t *new_ts, uint32_t,
			  struct audio_output_data *mixes)
{
	auto *self = static_cast<VerticalAudio *>(param);
	*new_ts = start_ts;
	const size_t maxFrames = self->rate_ / 2;
	std::lock_guard<std::mutex> lock(self->inputsMutex_);
	for (const auto &in : self->inputs_) {
		std::lock_guard<std::mutex> inLock(in->m);
		// A positive sync offset (Advanced Audio Properties) holds it back by
		// that much silence, up to 20 seconds; a negative one can't be done here.
		const int64_t off = std::clamp<int64_t>(in->offsetNs.load(), 0, 20000000000LL);
		const size_t want = size_t(off * int64_t(self->rate_) / 1000000000LL);
		if (want != in->delayed) {
			for (size_t ch = 0; ch < self->channels_; ch++) {
				auto &q = in->buf[ch];
				if (want > in->delayed)
					q.insert(q.begin(), want - in->delayed, 0.0f);
				else
					q.erase(q.begin(), q.begin() + std::min(q.size(), in->delayed - want));
			}
			in->delayed = want;
		}
		size_t have = in->buf[0].size();
		if (have > maxFrames + in->delayed) {
			for (size_t ch = 0; ch < self->channels_; ch++) {
				auto &q = in->buf[ch];
				q.erase(q.begin(), q.begin() + std::min(q.size(), have - PRIME_FRAMES - in->delayed));
			}
			have = in->buf[0].size();
		}
		if (!in->primed) {
			if (have < PRIME_FRAMES)
				continue;
			in->primed = true;
		}
		const size_t n = std::min<size_t>(have, AUDIO_OUTPUT_FRAMES);
		for (size_t ch = 0; ch < self->channels_; ch++) {
			auto &q = in->buf[ch];
			float *out = mixes[0].data[ch];
			const size_t take = std::min(n, q.size());
			if (out)
				for (size_t i = 0; i < take; i++)
					out[i] += q[i];
			q.erase(q.begin(), q.begin() + take);
		}
		// Ran dry: wait for two blocks again rather than stutter, and hold the
		// next sound back by its offset again.
		if (n < AUDIO_OUTPUT_FRAMES) {
			in->primed = false;
			in->delayed = 0;
		}
	}
	return true;
}

void VerticalAudio::setSetting(const std::string &uuid, const Setting &s)
{
	std::lock_guard<std::mutex> lock(settingsMutex_);
	settings_[uuid] = s;
}

VerticalAudio::Setting VerticalAudio::setting(const std::string &uuid) const
{
	std::lock_guard<std::mutex> lock(settingsMutex_);
	auto it = settings_.find(uuid);
	return it == settings_.end() ? Setting() : it->second;
}

void VerticalAudio::loadSettings(obs_data_t *data)
{
	std::lock_guard<std::mutex> lock(settingsMutex_);
	settings_.clear();
	if (!data)
		return;
	for (obs_data_item_t *item = obs_data_first(data); item; obs_data_item_next(&item)) {
		obs_data_t *o = obs_data_item_get_obj(item);
		if (!o)
			continue;
		obs_data_set_default_double(o, "volume", 1.0);
		Setting s;
		s.own = obs_data_get_bool(o, "own");
		s.volume = float(std::clamp(obs_data_get_double(o, "volume"), 0.0, 2.0));
		s.muted = obs_data_get_bool(o, "muted");
		s.hidden = obs_data_get_bool(o, "hidden");
		settings_[obs_data_item_get_name(item)] = s;
		obs_data_release(o);
	}
}

obs_data_t *VerticalAudio::saveSettings() const
{
	std::lock_guard<std::mutex> lock(settingsMutex_);
	obs_data_t *d = obs_data_create();
	for (const auto &[uuid, s] : settings_) {
		obs_data_t *o = obs_data_create();
		obs_data_set_bool(o, "own", s.own);
		obs_data_set_double(o, "volume", s.volume);
		obs_data_set_bool(o, "muted", s.muted);
		obs_data_set_bool(o, "hidden", s.hidden);
		obs_data_set_obj(d, uuid.c_str(), o);
		obs_data_release(o);
	}
	return d;
}

QJsonArray VerticalAudio::state() const
{
	QJsonArray out;
	std::lock_guard<std::mutex> lock(inputsMutex_);
	for (const auto &in : inputs_) {
		const Setting s = setting(in->uuid);
		QJsonObject o;
		o["uuid"] = QString::fromStdString(in->uuid);
		// Its name now (it may have been renamed since it started playing).
		QString name = QString::fromStdString(in->name);
		bool monitorOnly = false;
		if (obs_source_t *src = obs_weak_source_get_source(in->weak)) {
			name = QString::fromUtf8(obs_source_get_name(src));
			monitorOnly = isMonitorOnly(src);
			obs_source_release(src);
		}
		o["name"] = name;
		o["monitorOnly"] = monitorOnly;
		o["mode"] = s.own ? "own" : "same";
		o["volume"] = s.volume;
		o["muted"] = s.muted;
		o["hidden"] = s.hidden;
		o["gain"] = in->gain.load();
		o["peak"] = in->peak.load();
		out.append(o);
	}
	return out;
}
