/*
BGH LIVE Front
Copyright (C) 2026 Barrington Gospel Hall

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/
// Shared pieces of the plug-in.
//   state.cpp   what OBS is doing (scene, recording, projector), the paired phones, and the actions phones can take
//   server.cpp  the little web server phones on the church Wi-Fi talk to (port 8650)
//   slides.cpp  the slideshow on the projector: PowerPoint, or the Left/Right arrow keys for anything else
//   dock.cpp    the "BGH LIVE Front" panel in OBS: QR code, pairing code and paired phones
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class QScreen;

namespace front {

constexpr int kPort = 8650;
// Unless a screen is chosen in the dock, any connected screen with this in its name is the projector.
constexpr const char *kProjectorName = "OPTOMA";
// The screen chosen in the dock's "Projector screen" list, by name ("" = any screen named OPTOMA). Any thread.
std::string projectorChoice();
void setProjectorChoice(const std::string &name);
// How to name the projector to people: the chosen screen's name, or "OPTOMA"
std::string projectorLabel();
// The only OBS scenes the phone shows, in this order. Any other scenes in OBS are ignored.
const std::vector<std::string> &allowedScenes();

/* ---- state.cpp ---- */
struct Phone {
	std::string id, name, token, mode;
	int64_t pairedAt = 0, lastSeen = 0;
	int online = 0; // open live connections
};

void stateInit();
void stateShutdown();
// Recomputes what OBS is doing (UI thread). Sends it to every phone when something changed.
void refreshState();
// The latest state as JSON, safe from any thread
std::string stateJson();

// Runs fn on OBS's UI thread and waits for it (up to timeoutMs). False if it timed out or OBS is closing.
bool runOnUi(const std::function<void()> &fn, int timeoutMs = 4000);

// Actions phones can take. Run on the UI thread. Return "" when it worked, otherwise what went wrong.
std::string switchScene(const std::string &name);
std::string setRecording(bool on);
std::string setProjecting(bool on);

QScreen *projectorScreen();      // UI thread
std::string screensDescription(); // UI thread: every connected screen's name, for the dock

// Pairing (any thread)
std::string pairCode();
std::string pairKey();
// Pairs a phone by its 3-digit code or the QR code's key. Returns the phone's token, or "" with err set.
std::string pairPhone(const std::string &code, const std::string &key, const std::string &userAgent,
		      std::string &err);
// The phone's id for a token, or "" if it isn't paired
std::string phoneForToken(const std::string &token);
std::vector<Phone> phones();
void removePhone(const std::string &id);
void setPhoneMode(const std::string &id, const std::string &mode);
void phoneConnected(const std::string &id, bool connected);
void touchPhone(const std::string &id);

/* ---- server.cpp ---- */
bool serverStart();
void serverStop();
void serverBroadcast(const std::string &event, const std::string &data);
void serverDisconnectPhone(const std::string &id);
std::vector<std::string> localAddresses();
int presenterViewers();

/* ---- slides.cpp ---- */
void slidesStart();
void slidesStop();
// Next (+1) or back (-1). Returns "" or what went wrong.
std::string slidesStep(int delta);
// {"app":"PowerPoint","n":3,"total":8,"ratio":"16:9","v":12,"next":true} or with app "" when no slideshow is known
std::string slidesJson();
// PNG of the current ("cur") or next ("next") slide; empty if there isn't one
std::vector<unsigned char> slideImage(const std::string &which);
// Called from the UI thread every second: grabs the projector screen when PowerPoint isn't giving slides
void slidesTick();

/* ---- dock.cpp ---- */
void dockCreate();
void dockRefresh();

/* ---- helpers ---- */
std::string jsonEscape(const std::string &s);
int64_t nowMs();
std::string randomHex(int bytes);

} // namespace front
