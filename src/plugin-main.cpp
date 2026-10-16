/*
BGH LIVE Front
Copyright (C) 2026 Barrington Gospel Hall

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/
// The plug-in starts here. It runs the phone remote for the laptop at the front of the auditorium:
// Presenter phones change slides, Controller phones switch scenes, project OBS on the OPTOMA projector and record.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QApplication>
#include <QTimer>

#include "front.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "BGH LIVE Front: phone remote for slides, scenes, the projector and recording";
}

static QTimer *g_timer = nullptr;
static bool g_started = false;

static void startAll()
{
	if (g_started)
		return;
	g_started = true;
	front::stateInit();
	front::dockCreate();
	front::slidesStart();
	if (!front::serverStart())
		obs_log(LOG_WARNING, "the phone remote couldn't start on port %d", front::kPort);

	// Once a second: what OBS is doing, the projector, and the slideshow
	g_timer = new QTimer();
	g_timer->setInterval(1000);
	QObject::connect(g_timer, &QTimer::timeout, []() {
		front::refreshState();
		front::slidesTick();
		front::applyOnTop();
		front::dockRefresh();
	});
	g_timer->start();
	front::refreshState();
	front::dockRefresh();
}

static void stopAll()
{
	if (!g_started)
		return;
	g_started = false;
	if (g_timer) {
		g_timer->stop();
		delete g_timer;
		g_timer = nullptr;
	}
	front::applyOnTop(true); // give the projector's windows their normal order back
	front::stateShutdown(); // first, so nothing waits on OBS's UI thread while it closes
	front::serverStop();
	front::slidesStop();
}

static void onFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		startAll();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		if (g_started)
			front::refreshState();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		stopAll();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	stopAll();
	obs_log(LOG_INFO, "unloaded");
}
