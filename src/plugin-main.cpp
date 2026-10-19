/*
Multistream for OBS
Copyright (C) 2026 Multistream <hello@multistream.gg>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "bridge.hpp"
#include "dock.hpp"
#include "obs-state.hpp"
#include "relay-output.hpp"
#include "vertical/canvas.hpp"
#include "vertical/docks.hpp"
#include "vertical/record.hpp"
#include "vertical/websocket.hpp"

#include <QTimer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {
RelayOutput relay;
VerticalCanvas vertical;
RelayOutput verticalRelay("vertical-relay.json", "Multistream vertical output", "vertical");
VerticalRecorder verticalRecord(&vertical);
Bridge *bridge = nullptr;
QTimer *ticker = nullptr;

void onEvent(enum obs_frontend_event event, void *)
{
	obsstate::onEvent(int(event));
	relay.onFrontendEvent(int(event));
	// The vertical output reads the canvas's picture: it lets go before the
	// canvas does (collection switch, exit).
	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING || event == OBS_FRONTEND_EVENT_EXIT) {
		if (event == OBS_FRONTEND_EVENT_EXIT)
			vertical_ws::shutdown();
		verticalRelay.shutdown();
		if (event == OBS_FRONTEND_EVENT_EXIT)
			verticalRecord.shutdown(); // saves its hotkeys too
	}
	verticalRecord.onFrontendEvent(int(event));
	verticalRelay.onFrontendEvent(int(event));
	vertical.onFrontendEvent(int(event));
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		bridge = new Bridge(&relay, &verticalRelay, &vertical);
		if (bridge->start())
			dock::loadPage(bridge->pageUrl());
		dock::showOnFirstRun();
		vertical_docks::showOnFirstRun();
		ticker = new QTimer();
		QObject::connect(ticker, &QTimer::timeout, []() {
			obsstate::tick();
			relay.tick();
			vertical.tick();
			verticalRelay.tick();
		});
		ticker->start(1000);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		dock::close();
		vertical_docks::close();
		if (ticker) {
			ticker->stop();
			delete ticker;
			ticker = nullptr;
		}
		relay.shutdown();
		delete bridge;
		bridge = nullptr;
		break;
	default:
		break;
	}
}
} // namespace

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Multistream: check everything, then go live on all your platforms from one dock.";
}

bool obs_module_load(void)
{
	relay.load();
	vertical.load();
	verticalRelay.setMedia([]() { return vertical.video(); }, []() { return vertical.audio(); },
			       []() { return vertical.sig(); });
	verticalRelay.load();
	verticalRecord.load();
	vertical.recorder = &verticalRecord;
	dock::create();
	vertical_docks::create(&vertical);
	obs_frontend_add_event_callback(onEvent, nullptr);
	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	vertical_ws::registerVendor(&vertical, &verticalRelay);
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onEvent, nullptr);
	vertical_ws::shutdown();
	relay.shutdown();
	verticalRelay.shutdown();
	verticalRecord.shutdown();
	vertical.recorder = nullptr;
	vertical.shutdown();
}
