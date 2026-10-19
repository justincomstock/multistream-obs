#pragma once

#include <obs.h>

#include <QJsonObject>
#include <QString>

#include <functional>

// A second stream output to Multistream, for when OBS's own stream goes to
// Twitch (Enhanced Broadcasting keeps working): it starts and stops with
// OBS's Start/Stop Streaming. The C++ version of obs/multistream-relay.lua.
// The vertical canvas uses another one (feed "vertical") with its own
// picture and sound: see setMedia().
//
// Output signals arrive on OBS's output threads; their handlers only queue
// work onto the UI thread. Nothing releases the output while it's
// connecting (that waits for the connect thread): one attempt at a time,
// and the output is reused between attempts.
class RelayOutput {
public:
	RelayOutput(const char *file = "relay.json", const char *name = "Multistream output",
		    const char *feed = "main");
	// Where the picture and sound come from (default: OBS's main mix). `sig`
	// changes when the picture's size does, so the encoder is rebuilt. A null
	// video or audio means not ready yet: tried again on the next retry.
	void setMedia(std::function<video_t *()> video, std::function<audio_t *()> audio,
		      std::function<QString()> sig);
	void load();
	// From the dock. Saved in the plugin's config, so it also works before
	// the dock has loaded.
	void configure(bool enabled, const QString &server, const QString &key, int bitrate);
	// Starts now, without OBS's own stream (and keeps going when it stops),
	// until stopNow().
	bool startNow();
	void stopNow();
	void onFrontendEvent(int event);
	void tick();
	void shutdown();
	QJsonObject state() const;

private:
	bool build();
	void releaseAll();
	bool startOutput();
	void stopOutput();
	QString sig() const;
	void save() const;

	static void onStart(void *data, calldata_t *cd);
	static void onStop(void *data, calldata_t *cd);
	static void onReconnect(void *data, calldata_t *cd);
	static void onReconnectSuccess(void *data, calldata_t *cd);

	QString file_;
	QString name_;
	QString feed_;
	std::function<video_t *()> video_;
	std::function<audio_t *()> audio_;
	std::function<QString()> mediaSig_;

	bool enabled_ = false;
	QString server_;
	QString key_;
	int bitrate_ = 6000;

	obs_output_t *output_ = nullptr;
	obs_service_t *service_ = nullptr;
	obs_encoder_t *venc_ = nullptr;
	obs_encoder_t *aenc_ = nullptr;
	QString builtSig_;

	bool wanted_ = false;
	bool manual_ = false;
	bool attempting_ = false;
	bool sending_ = false;
	bool reconnecting_ = false;
	QString error_;
	QString encoder_;
	qint64 lastTry_ = 0;
	qint64 startedAt_ = 0;
};
