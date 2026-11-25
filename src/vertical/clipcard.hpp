#pragma once

#include <QImage>
#include <QJsonObject>
#include <QString>

#include <functional>

class QWidget;

// The card that floats over the bottom of the Vertical Preview after Save clip
// (the owner's pick, design A): the clip's last frame and length, Watch and
// Show in folder, or why it wasn't saved and the button that fixes it. A
// small window of its own, because the preview is a native window that paints
// over anything placed on it. It follows the preview, hides with it, fades
// and slides in and out, and stays while the pointer is on it. UI thread.
namespace clipcard {

struct Actions {
	std::function<void()> turnOnReplay; // a clip asked for while Replay was off
	std::function<void()> saveAgain;    // the clip couldn't be written
};

// Pins the card over `over` (the preview's stack). Call once.
void attach(QWidget *over, Actions actions);
// The recorder's `clip` state (record.hpp) and the picture when it was asked for.
void show(const QJsonObject &clip, const QImage &still);
// A one-line note in the same card: `ok` gets a green tick, else a violet "i".
void note(const QString &title, const QString &text, bool ok);
// Where it is now, for the tests: {visible, phase, title, text, buttons: [..]}.
QJsonObject report();
void close();

} // namespace clipcard
