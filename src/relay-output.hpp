#pragma once

#include <obs.h>

#include <QJsonObject>
#include <QString>

// A second stream output to Multistream, for when OBS's own stream goes to
// Twitch (Enhanced Broadcasting keeps working): it starts and stops with
// OBS's Start/Stop Streaming. The C++ version of obs/multistream-relay.lua.
//
// Output signals arrive on OBS's output threads; their handlers only queue
// work onto the UI thread. Nothing releases the output while it's
// connecting (that waits for the connect thread): one attempt at a time,
// and the output is reused between attempts.
class RelayOutput {
public:
	void load();
	// From the dock. Saved in the plugin's config, so it also works before
	// the dock has loaded.
	void configure(bool enabled, const QString &server, const QString &key, int bitrate);
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
	bool attempting_ = false;
	bool sending_ = false;
	bool reconnecting_ = false;
	QString error_;
	QString encoder_;
	qint64 lastTry_ = 0;
	qint64 startedAt_ = 0;
};
