/*
BGH LIVE Front
Copyright (C) 2026 Barrington Gospel Hall

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/
// What OBS is doing, the paired phones, and the actions phones can take.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <random>

#include "front.h"

namespace front {

const std::vector<std::string> &allowedScenes()
{
	static const std::vector<std::string> list = {"Spanish Translator", "Korean Translator", "English Translator",
						      "Baptism Cam", "Black"};
	return list;
}

/* ------------------------------------------------------------------ helpers */
int64_t nowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string randomHex(int bytes)
{
	static std::mutex m;
	static std::mt19937_64 rng{std::random_device{}() ^ (uint64_t)nowMs()};
	std::lock_guard<std::mutex> lock(m);
	static const char *hex = "0123456789abcdef";
	std::string out;
	for (int i = 0; i < bytes; i++) {
		unsigned v = (unsigned)(rng() & 0xff);
		out += hex[v >> 4];
		out += hex[v & 15];
	}
	return out;
}

std::string jsonEscape(const std::string &s)
{
	std::string o;
	o.reserve(s.size() + 8);
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			o += "\\\"";
			break;
		case '\\':
			o += "\\\\";
			break;
		case '\n':
			o += "\\n";
			break;
		case '\r':
			o += "\\r";
			break;
		case '\t':
			o += "\\t";
			break;
		default:
			if (c < 0x20) {
				char b[8];
				snprintf(b, sizeof(b), "\\u%04x", c);
				o += b;
			} else {
				o += (char)c;
			}
		}
	}
	return o;
}

static std::atomic<bool> g_closing{false};

bool runOnUi(const std::function<void()> &fn, int timeoutMs)
{
	if (!qApp)
		return false;
	if (QThread::currentThread() == qApp->thread()) {
		fn();
		return true;
	}
	if (g_closing)
		return false;
	auto done = std::make_shared<std::promise<void>>();
	auto fut = done->get_future();
	std::function<void()> copy = fn;
	QMetaObject::invokeMethod(
		qApp,
		[copy, done]() {
			if (!g_closing)
				copy();
			done->set_value();
		},
		Qt::QueuedConnection);
	return fut.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready;
}

/* ------------------------------------------------------------------ the projector screen */
static std::mutex g_cfgm;
static std::string g_choice;

static std::string settingsFile()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path("settings.json");
	std::string out = p ? p : "";
	bfree(p);
	return out;
}

static std::string g_onTop = "auto";

static void loadSettings()
{
	const std::string f = settingsFile();
	obs_data_t *d = f.empty() ? nullptr : obs_data_create_from_json_file_safe(f.c_str(), "bak");
	std::lock_guard<std::mutex> lock(g_cfgm);
	g_choice = d ? obs_data_get_string(d, "projectorScreen") : "";
	const std::string saved = d ? obs_data_get_string(d, "onTop") : "";
	g_onTop = saved == "presentation" || saved == "obs" ? saved : "auto";
	if (d)
		obs_data_release(d);
}

static void saveSettings()
{
	obs_data_t *d = obs_data_create();
	{
		std::lock_guard<std::mutex> lock(g_cfgm);
		obs_data_set_string(d, "projectorScreen", g_choice.c_str());
		obs_data_set_string(d, "onTop", g_onTop.c_str());
	}
	const std::string f = settingsFile();
	if (!f.empty())
		obs_data_save_json_safe(d, f.c_str(), "tmp", "bak");
	obs_data_release(d);
}

std::string onTop()
{
	std::lock_guard<std::mutex> lock(g_cfgm);
	return g_onTop;
}

void setOnTop(const std::string &which)
{
	{
		std::lock_guard<std::mutex> lock(g_cfgm);
		g_onTop = which == "presentation" || which == "obs" ? which : "auto";
	}
	saveSettings();
	obs_log(LOG_INFO, "on top of the projector: %s", which.c_str());
}

std::string projectorChoice()
{
	std::lock_guard<std::mutex> lock(g_cfgm);
	return g_choice;
}

std::string projectorLabel()
{
	const std::string c = projectorChoice();
	return c.empty() ? std::string(kProjectorName) : c;
}

void setProjectorChoice(const std::string &name)
{
	{
		std::lock_guard<std::mutex> lock(g_cfgm);
		g_choice = name;
	}
	saveSettings();
	obs_log(LOG_INFO, "projector screen: %s", name.empty() ? "any screen named OPTOMA" : name.c_str());
}

QScreen *projectorScreen()
{
	// A screen chosen in the dock: that screen only, never another one
	const std::string choice = projectorChoice();
	if (!choice.empty()) {
		for (QScreen *s : QGuiApplication::screens())
			if (s->name().toStdString() == choice)
				return s;
		return nullptr;
	}
	const QString want = QString::fromUtf8(kProjectorName);
	for (QScreen *s : QGuiApplication::screens()) {
		const QString text = s->name() + " " + s->manufacturer() + " " + s->model();
		if (text.contains(want, Qt::CaseInsensitive))
			return s;
	}
	return nullptr;
}

std::string screensDescription()
{
	std::string out;
	int i = 1;
	for (QScreen *s : QGuiApplication::screens()) {
		QString label = s->name();
		const QString extra = (s->manufacturer() + " " + s->model()).trimmed();
		if (!extra.isEmpty() && !label.contains(extra, Qt::CaseInsensitive))
			label += " (" + extra + ")";
		const QSize size = s->size() * s->devicePixelRatio();
		if (!out.empty())
			out += "\n";
		out += std::to_string(i++) + ". " + label.toStdString() + " · " + std::to_string(size.width()) + "×" +
		       std::to_string(size.height());
	}
	return out;
}

// OBS's full-screen projectors are its own windows of the class OBSProjector
static std::vector<QWidget *> projectorWindows(QScreen *screen)
{
	std::vector<QWidget *> out;
	for (QWidget *w : QApplication::topLevelWidgets()) {
		if (!w->isVisible())
			continue;
		if (qstrcmp(w->metaObject()->className(), "OBSProjector") != 0)
			continue;
		if (screen && w->screen() != screen)
			continue;
		out.push_back(w);
	}
	return out;
}

std::vector<void *> obsProjectorHandles()
{
	std::vector<void *> out;
	QScreen *pj = projectorScreen();
	if (!pj)
		return out;
	for (QWidget *w : projectorWindows(pj))
		out.push_back((void *)w->winId());
	return out;
}

bool isProjecting()
{
	QScreen *pj = projectorScreen();
	return pj && !projectorWindows(pj).empty();
}

/* ------------------------------------------------------------------ actions */
std::string switchScene(const std::string &name)
{
	bool allowed = false;
	for (auto &s : allowedScenes())
		if (s == name)
			allowed = true;
	if (!allowed)
		return "That scene isn't one the remote can switch to.";
	obs_source_t *src = obs_get_source_by_name(name.c_str());
	if (!src || !obs_source_is_scene(src)) {
		obs_source_release(src);
		return "There's no scene called \"" + name + "\" in OBS.";
	}
	if (obs_frontend_preview_program_mode_active()) {
		obs_frontend_set_current_preview_scene(src);
		obs_frontend_preview_program_trigger_transition();
	} else {
		obs_frontend_set_current_scene(src);
	}
	obs_source_release(src);
	return "";
}

std::string setRecording(bool on)
{
	if (on == obs_frontend_recording_active())
		return "";
	if (on)
		obs_frontend_recording_start();
	else
		obs_frontend_recording_stop();
	return "";
}

std::string setProjecting(bool on)
{
	QScreen *screen = projectorScreen();
	if (!on) {
		for (QWidget *w : projectorWindows(screen))
			w->close();
		return "";
	}
	if (!screen)
		return "Can't find the " + projectorLabel() +
		       " screen. Check the projector's HDMI cable is plugged into the laptop.";
	const int index = (int)QGuiApplication::screens().indexOf(screen);
	obs_frontend_open_projector("Program", index, "", "");
	// Straight away, so a presentation chosen to stay in front covers the new projector without a flash
	applyOnTop();
	// OBS opens the projector window a moment later: keep putting it in its place (and telling phones) while it
	// appears, so it ends up above a presentation (PowerPoint keeps itself on top) without waiting a second
	for (int ms : {150, 400, 800, 1500})
		QTimer::singleShot(ms, []() {
			applyOnTop();
			refreshState();
		});
	return "";
}

/* ------------------------------------------------------------------ phones and pairing */
static std::mutex g_pm;
static std::vector<Phone> g_phones;
static std::string g_code, g_key;
static int g_wrong = 0;
static int64_t g_lockedUntil = 0;

static std::string phonesFile()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path("phones.json");
	std::string out = p ? p : "";
	bfree(p);
	return out;
}

static void newCode()
{
	char b[8];
	unsigned v = 0;
	const std::string h = randomHex(2);
	v = (unsigned)std::stoul(h, nullptr, 16) % 1000;
	snprintf(b, sizeof(b), "%03u", v);
	g_code = b;
	g_key = randomHex(10);
}

static void savePhonesLocked()
{
	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	for (auto &p : g_phones) {
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "id", p.id.c_str());
		obs_data_set_string(d, "name", p.name.c_str());
		obs_data_set_string(d, "token", p.token.c_str());
		obs_data_set_string(d, "mode", p.mode.c_str());
		obs_data_set_int(d, "pairedAt", p.pairedAt);
		obs_data_set_int(d, "lastSeen", p.lastSeen);
		obs_data_array_push_back(arr, d);
		obs_data_release(d);
	}
	obs_data_set_array(root, "phones", arr);
	obs_data_array_release(arr);
	const std::string f = phonesFile();
	if (!f.empty())
		obs_data_save_json_safe(root, f.c_str(), "tmp", "bak");
	obs_data_release(root);
}

static void loadPhones()
{
	std::lock_guard<std::mutex> lock(g_pm);
	g_phones.clear();
	const std::string f = phonesFile();
	obs_data_t *root = f.empty() ? nullptr : obs_data_create_from_json_file_safe(f.c_str(), "bak");
	if (!root)
		return;
	obs_data_array_t *arr = obs_data_get_array(root, "phones");
	const size_t n = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *d = obs_data_array_item(arr, i);
		Phone p;
		p.id = obs_data_get_string(d, "id");
		p.name = obs_data_get_string(d, "name");
		p.token = obs_data_get_string(d, "token");
		p.mode = obs_data_get_string(d, "mode");
		p.pairedAt = obs_data_get_int(d, "pairedAt");
		p.lastSeen = obs_data_get_int(d, "lastSeen");
		if (!p.id.empty() && p.token.size() >= 16)
			g_phones.push_back(p);
		obs_data_release(d);
	}
	obs_data_array_release(arr);
	obs_data_release(root);
}

std::string pairCode()
{
	std::lock_guard<std::mutex> lock(g_pm);
	return g_code;
}

std::string pairKey()
{
	std::lock_guard<std::mutex> lock(g_pm);
	return g_key;
}

static std::string nameFromAgent(const std::string &ua)
{
	if (ua.find("iPhone") != std::string::npos)
		return "iPhone";
	if (ua.find("iPad") != std::string::npos)
		return "iPad";
	if (ua.find("Android") != std::string::npos)
		return "Android phone";
	if (ua.find("Macintosh") != std::string::npos)
		return "Mac";
	if (ua.find("Windows") != std::string::npos)
		return "Windows computer";
	return "Phone";
}

std::string pairPhone(const std::string &code, const std::string &key, const std::string &userAgent, std::string &err)
{
	std::lock_guard<std::mutex> lock(g_pm);
	const int64_t now = nowMs();
	if (now < g_lockedUntil) {
		err = "LOCKED:" + std::to_string((g_lockedUntil - now) / 1000 + 1);
		return "";
	}
	const bool ok = (!key.empty() && key == g_key) || (!code.empty() && code == g_code);
	if (!ok) {
		if (++g_wrong >= 5) {
			g_wrong = 0;
			g_lockedUntil = now + 5 * 60 * 1000;
			err = "LOCKED:300";
		} else {
			err = "WRONG:" + std::to_string(5 - g_wrong);
		}
		return "";
	}
	g_wrong = 0;
	Phone p;
	p.id = randomHex(4);
	p.token = randomHex(16);
	std::string base = nameFromAgent(userAgent), name = base;
	for (int n = 2;; n++) {
		bool taken = false;
		for (auto &q : g_phones)
			if (q.name == name)
				taken = true;
		if (!taken)
			break;
		name = base + " " + std::to_string(n);
	}
	p.name = name;
	p.pairedAt = p.lastSeen = now;
	g_phones.push_back(p);
	savePhonesLocked();
	newCode(); // a new code and QR code for the next phone
	obs_log(LOG_INFO, "paired a phone (%s)", p.name.c_str());
	return p.token;
}

std::string phoneForToken(const std::string &token)
{
	if (token.size() < 16)
		return "";
	std::lock_guard<std::mutex> lock(g_pm);
	for (auto &p : g_phones)
		if (p.token == token)
			return p.id;
	return "";
}

std::vector<Phone> phones()
{
	std::lock_guard<std::mutex> lock(g_pm);
	return g_phones;
}

void removePhone(const std::string &id)
{
	{
		std::lock_guard<std::mutex> lock(g_pm);
		for (size_t i = 0; i < g_phones.size(); i++)
			if (g_phones[i].id == id) {
				g_phones.erase(g_phones.begin() + (long)i);
				break;
			}
		savePhonesLocked();
	}
	serverDisconnectPhone(id);
}

void setPhoneMode(const std::string &id, const std::string &mode)
{
	std::lock_guard<std::mutex> lock(g_pm);
	for (auto &p : g_phones)
		if (p.id == id && p.mode != mode) {
			p.mode = mode;
			savePhonesLocked();
		}
}

void phoneConnected(const std::string &id, bool connected)
{
	std::lock_guard<std::mutex> lock(g_pm);
	for (auto &p : g_phones)
		if (p.id == id) {
			p.online += connected ? 1 : -1;
			if (p.online < 0)
				p.online = 0;
			p.lastSeen = nowMs();
		}
}

void touchPhone(const std::string &id)
{
	std::lock_guard<std::mutex> lock(g_pm);
	for (auto &p : g_phones)
		if (p.id == id)
			p.lastSeen = nowMs();
}

/* ------------------------------------------------------------------ what OBS is doing */
static std::mutex g_sm;
static std::string g_state;
static int64_t g_recStart = 0;

std::string stateJson()
{
	std::lock_guard<std::mutex> lock(g_sm);
	return g_state;
}

void refreshState()
{
	std::string program;
	if (obs_source_t *cur = obs_frontend_get_current_scene()) {
		program = obs_source_get_name(cur);
		obs_source_release(cur);
	}
	std::vector<std::string> present;
	struct obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; i++) {
		const char *n = obs_source_get_name(list.sources.array[i]);
		if (n)
			present.push_back(n);
	}
	obs_frontend_source_list_free(&list);

	const bool rec = obs_frontend_recording_active();
	if (rec && !g_recStart)
		g_recStart = nowMs();
	if (!rec)
		g_recStart = 0;

	QScreen *pj = projectorScreen();
	const bool projecting = pj && !projectorWindows(pj).empty();

	std::string scenes = "[";
	bool first = true;
	for (auto &s : allowedScenes()) {
		bool here = false;
		for (auto &p : present)
			if (p == s)
				here = true;
		scenes += std::string(first ? "" : ",") + "{\"label\":\"" + jsonEscape(s) + "\",\"here\":" +
			  (here ? "true" : "false") + "}";
		first = false;
	}
	scenes += "]";

	std::string json = std::string("{\"obs\":true") + ",\"program\":\"" + jsonEscape(program) + "\"" +
			   ",\"scenes\":" + scenes + ",\"rec\":" + (rec ? "true" : "false") +
			   ",\"recT0\":" + std::to_string(g_recStart) + ",\"proj\":" + (projecting ? "true" : "false") +
			   ",\"pjFound\":" + (pj ? "true" : "false") + ",\"onTop\":\"" + onTop() + "\"" +
			   ",\"presFront\":" + (presentationInFront() || !projecting ? "true" : "false") + ",\"pjName\":\"" + jsonEscape(projectorLabel()) +
			   "\"" + ",\"slides\":" + slidesJson() + "}";

	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(g_sm);
		if (json != g_state) {
			g_state = json;
			changed = true;
		}
	}
	if (changed)
		serverBroadcast("state", json);
}

void stateInit()
{
	g_closing = false;
	loadSettings();
	loadPhones();
	std::lock_guard<std::mutex> lock(g_pm);
	newCode();
	for (QScreen *s : QGuiApplication::screens())
		obs_log(LOG_INFO, "screen: %s | %s | %s", s->name().toUtf8().constData(),
			s->manufacturer().toUtf8().constData(), s->model().toUtf8().constData());
}

void stateShutdown()
{
	g_closing = true;
}

} // namespace front
