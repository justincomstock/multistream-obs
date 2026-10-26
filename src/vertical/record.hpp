#pragma once

#include <obs.h>

#include <QJsonObject>
#include <QString>

class VerticalCanvas;

// Records the vertical canvas, and keeps a replay buffer of it, into OBS's
// own recording folder ("Vertical 2026-10-19 14-03-11.mp4"). By default both
// follow OBS's own: Start Recording records both pictures, and Save Replay
// saves both clips. Each can also be driven alone, from the Vertical Preview
// dock, its hotkeys, or obs-websocket.
//
// UI thread only. Output signals queue their work onto it.
class VerticalRecorder {
public:
	explicit VerticalRecorder(VerticalCanvas *canvas) : canvas_(canvas) {}
	void load();
	void onFrontendEvent(int event);
	void shutdown();

	bool startRecording(QString &error);
	void stopRecording();
	bool recording() const;
	bool paused() const;
	bool setPaused(bool on, QString &error);
	bool startReplay(QString &error);
	void stopReplay();
	bool replayActive() const;
	bool saveReplay(QString &error);

	// {recording, paused, replay, followMain, format, replaySeconds, bitrate
	//  (0: same as the main stream's), lastFile, lastReplay, error, notice,
	//  noticeSeq, noticeFile}
	QJsonObject state() const;
	// Any of {followMain, format: "mp4"|"mkv", replaySeconds, bitrate}.
	void configure(const QJsonObject &in);

private:
	bool encoders(QString &error);
	void releaseEncodersIfIdle();
	QString folder() const;
	int bitrate() const; // what the recording uses now
	void save() const;
	// Something to tell the streamer in the Vertical Preview for a moment.
	void notify(const QString &text, const QString &file = QString());
	void registerHotkeys();
	static void onRecordStop(void *data, calldata_t *cd);
	static void onReplayStop(void *data, calldata_t *cd);
	static void onReplaySaved(void *data, calldata_t *cd);

	VerticalCanvas *canvas_;
	obs_encoder_t *venc_ = nullptr;
	obs_encoder_t *aenc_ = nullptr;
	obs_output_t *record_ = nullptr;
	obs_output_t *replay_ = nullptr;
	QString encSig_;

	bool followMain_ = true;
	QString format_ = "mp4";
	int replaySeconds_ = 30;
	int bitrate_ = 12000; // 0: same as the main stream's
	QString lastFile_;
	QString lastReplay_;
	QString error_;
	QString notice_;
	QString noticeFile_;
	int noticeSeq_ = 0;
	// Recording through a scene collection switch (its canvas goes away).
	bool resumeRecord_ = false;
	bool resumeReplay_ = false;

	obs_hotkey_id hkRecord_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkReplay_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkSave_ = OBS_INVALID_HOTKEY_ID;
};
