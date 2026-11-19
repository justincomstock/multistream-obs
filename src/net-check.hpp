#pragma once

#include <QString>

#include <functional>

// Why a stream address can't be reached, found out by trying it: run when
// the plugin's own output can't connect (OBS only says "couldn't connect").
// The answer is a word the dock turns into what to do:
//   "app-closed"   the address is the Multistream app on this PC and nothing listens
//   "dns"          the host name can't be looked up
//   "refused"      the host answered and turned the connection down
//   "port-blocked" the website answers, the stream port doesn't (antivirus, firewall, VPN, network)
//   "blocked"      neither answers
//   "reached"      the stream port answers: the trouble is after connecting
// Call on the UI thread; `done` runs there too, once, within about 12 s.
namespace netcheck {

void diagnose(const QString &server, std::function<void(const QString &why)> done);

} // namespace netcheck
