#pragma once

#include <obs.h>

#include <QJsonObject>
#include <QString>

class VerticalCanvas;

// Sends the vertical picture out through OBS's own virtual camera ("OBS
// Virtual Camera"), so TikTok Live Studio, Discord or Zoom can use it as a
// webcam. Windows and macOS have only one OBS virtual camera, so it borrows
// OBS's: while ours runs, OBS's Start Virtual Camera button shows it as on,
// and when it stops the camera goes back to OBS's normal picture.
//
// UI thread only. Output signals queue their work onto it.
class VerticalVirtualCam {
public:
	explicit VerticalVirtualCam(VerticalCanvas *canvas) : canvas_(canvas) {}
	void load();
	void onFrontendEvent(int event);
	void shutdown();

	// Starts it. Fails (with the reason) while OBS's own camera shows the
	// normal picture, unless `switchFromMain`: then OBS's is stopped first
	// and ours starts once it has (true means it's on its way).
	bool start(QString &error, bool switchFromMain = false);
	void stop();
	bool active() const; // ours is on
	// OBS's own virtual camera is on with the normal picture.
	bool mainActive() const;
	// There's an OBS virtual camera on this computer to borrow.
	bool available() const;

	// {active, mainActive, available, switching, error}
	QJsonObject state() const;

private:
	bool startNow(QString &error);
	void restoreMain();
	void registerHotkey();
	static void onStop(void *data, calldata_t *cd);

	VerticalCanvas *canvas_;
	obs_output_t *out_ = nullptr; // OBS's camera, while ours is on it
	bool switching_ = false;      // waiting for OBS's own to stop
	bool resume_ = false;         // on through a scene collection switch
	QString error_;
	obs_hotkey_id hk_ = OBS_INVALID_HOTKEY_ID;
};
