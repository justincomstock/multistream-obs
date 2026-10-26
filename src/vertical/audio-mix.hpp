#pragma once

#include <obs.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The vertical canvas's own sound, separate from OBS's main mix (Aitum
// Vertical sums both canvases into OBS's tracks, so whatever plays in one
// goes out on the other).
//
// Each audio source in the vertical scene (and OBS's global Desktop Audio /
// Mic sources) gets an audio capture callback: OBS hands it the source's
// sound after its filters and before its volume fader. The callback scales
// it by the vertical volume and queues it; the mix callback, on our own
// audio_t's thread, adds one block of every queue together. The vertical
// encoder reads that audio_t, so OBS's main mix never hears vertical-only
// sources and the vertical stream never hears main-only ones.
//
// A source follows its OBS fader and mute ("same", the default) until it
// is given its own vertical volume ("own"). Like OBS's own mix, it leaves
// out sources set to "Monitor only" (Advanced Audio Properties), and holds a
// source back by its positive sync offset.
class VerticalAudio {
public:
	~VerticalAudio();
	bool open();
	void close();
	audio_t *audio() const { return audio_; }

	// UI thread: makes the captured sources exactly `sources` (strong refs,
	// borrowed for the call).
	void setSources(const std::vector<obs_source_t *> &sources);

	// Per-source vertical settings, keyed by source UUID.
	struct Setting {
		bool own = false;
		float volume = 1.0f; // 0..2 multiplier (up to +6 dB), when own
		bool muted = false;  // when own
		bool hidden = false; // no strip in the Vertical Mixer (it still plays)
	};
	void setSetting(const std::string &uuid, const Setting &s);
	Setting setting(const std::string &uuid) const;
	void loadSettings(obs_data_t *data);
	obs_data_t *saveSettings() const; // new reference

	// For the vendor API / dock: each captured source, its settings and
	// level (peak of the last block, 0..1).
	QJsonArray state() const;

private:
	struct Input {
		VerticalAudio *owner = nullptr; // outlives every Input it hands to OBS
		obs_weak_source_t *weak = nullptr;
		std::string uuid;
		std::string name;
		std::atomic<float> gain{1.0f};
		std::atomic<float> peak{0.0f};
		std::atomic<int64_t> offsetNs{0}; // its sync offset
		std::mutex m;
		std::deque<float> buf[MAX_AUDIO_CHANNELS];
		bool primed = false;
		size_t delayed = 0; // frames of silence queued for its sync offset
	};

	static void onCapture(void *param, obs_source_t *source, const struct audio_data *data, bool muted);
	static bool onMix(void *param, uint64_t start_ts, uint64_t end_ts, uint64_t *new_ts, uint32_t mixers,
			  struct audio_output_data *mixes);
	void detach(const std::shared_ptr<Input> &in);
	float gainFor(obs_source_t *source, const Input &in, bool muted) const;

	audio_t *audio_ = nullptr;
	size_t channels_ = 2;
	uint32_t rate_ = 48000;

	mutable std::mutex inputsMutex_;
	std::vector<std::shared_ptr<Input>> inputs_;

	mutable std::mutex settingsMutex_;
	std::map<std::string, Setting> settings_;
};
