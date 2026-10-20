/*
BGH LIVE Front
Copyright (C) 2026 Barrington Gospel Hall

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/
// The Baptism Cam: an OBSBOT Tiny 2 on USB, moved from Controller phones.
//
// The plug-in doesn't talk to the camera directly. OBSBOT Center (OBSBOT's own app) runs on the laptop, minimized,
// and listens for OSC messages over UDP (its OSC setting, port 16284 by default). The plug-in sends it:
//   /OBSBOT/WebCam/General/SetGimbalLeft|Right|Up|Down <speed 0..100>   the joystick (0 stops that direction)
//   /OBSBOT/WebCam/General/SetZoom <0..100>                             the zoom slider
//   /OBSBOT/WebCam/General/SetGimMotorDegree <speed> <pan> <tilt>       Go to shot
//   /OBSBOT/WebCam/General/GetGimbalPosInfo 1, GetZoomInfo 1            where it's pointing (for Set, and "on the shot")
//   /OBSBOT/WebCam/General/WakeSleep 1                                  wake it when a phone opens Camera / Baptism Cam goes live
//   /OBSBOT/WebCam/Tiny/SetAiMode 0, ToggleAILock 0                     AI tracking off, so nothing fights the joystick
//   /OBSBOT/WebCam/General/Connected 1                                  every 2 s, to know OBSBOT Center is answering
// Replies come back to the port this plug-in sends from (16285 by default; set the same in OBSBOT Center if it asks
// where to send replies). Every reply address is written to OBS's log once, which helps if OBSBOT changes its messages.
//
// The saved shot lives only in this plug-in's memory, shared by every Controller phone. It's forgotten when the camera
// is unplugged or OBS closes, so each service starts fresh. The phone's preview is a still of the camera's OBS source,
// about 4 a second, only while a phone has Camera open.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <QBuffer>
#include <QImage>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

#include "front.h"

namespace front {

namespace {

const char *G = "/OBSBOT/WebCam/General/";
const char *kScene = "Baptism Cam"; // the scene that shows this camera: while it's in Program, moves show on stream
const int kShotSpeed = 30;           // how fast Go to shot turns the camera (0..90): gentle enough to look smooth

/* ------------------------------------------------------------------ settings (camera.json) */
struct Settings {
	bool autoCenter = true, keepAwake = true, upside = false;
	int topSpeed = 100, oscPort = 16284, replyPort = 16285;
	std::string centerDir = "C:\\Program Files\\OBSBOT Center";
};
std::mutex g_sm;
Settings g_set;

std::string settingsPath()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path("camera.json");
	std::string out = p ? p : "";
	bfree(p);
	return out;
}

void loadSettings()
{
	const std::string f = settingsPath();
	obs_data_t *d = f.empty() ? nullptr : obs_data_create_from_json_file_safe(f.c_str(), "bak");
	if (!d)
		return;
	std::lock_guard<std::mutex> lock(g_sm);
	obs_data_set_default_bool(d, "autoCenter", true);
	obs_data_set_default_bool(d, "keepAwake", true);
	obs_data_set_default_int(d, "topSpeed", 100);
	obs_data_set_default_int(d, "oscPort", 16284);
	obs_data_set_default_int(d, "replyPort", 16285);
	obs_data_set_default_string(d, "centerDir", g_set.centerDir.c_str());
	g_set.autoCenter = obs_data_get_bool(d, "autoCenter");
	g_set.keepAwake = obs_data_get_bool(d, "keepAwake");
	g_set.upside = obs_data_get_bool(d, "upside");
	g_set.topSpeed = std::clamp((int)obs_data_get_int(d, "topSpeed"), 30, 100);
	g_set.oscPort = (int)obs_data_get_int(d, "oscPort");
	g_set.replyPort = (int)obs_data_get_int(d, "replyPort");
	g_set.centerDir = obs_data_get_string(d, "centerDir");
	obs_data_release(d);
}

void saveSettings()
{
	obs_data_t *d = obs_data_create();
	{
		std::lock_guard<std::mutex> lock(g_sm);
		obs_data_set_bool(d, "autoCenter", g_set.autoCenter);
		obs_data_set_bool(d, "keepAwake", g_set.keepAwake);
		obs_data_set_bool(d, "upside", g_set.upside);
		obs_data_set_int(d, "topSpeed", g_set.topSpeed);
		obs_data_set_int(d, "oscPort", g_set.oscPort);
		obs_data_set_int(d, "replyPort", g_set.replyPort);
		obs_data_set_string(d, "centerDir", g_set.centerDir.c_str());
	}
	const std::string f = settingsPath();
	if (!f.empty())
		obs_data_save_json_safe(d, f.c_str(), "tmp", "bak");
	obs_data_release(d);
}

Settings settings()
{
	std::lock_guard<std::mutex> lock(g_sm);
	return g_set;
}

/* ------------------------------------------------------------------ OSC over UDP */
struct OscArg {
	char type = 'i';
	int32_t i = 0;
	float f = 0;
	std::string s;
	double num() const { return type == 'f' ? f : type == 'i' ? i : std::atof(s.c_str()); }
};

void pad(std::string &b)
{
	do
		b.push_back('\0');
	while (b.size() % 4);
}

void be32(std::string &b, uint32_t v)
{
	b.push_back((char)(v >> 24));
	b.push_back((char)(v >> 16));
	b.push_back((char)(v >> 8));
	b.push_back((char)v);
}

std::string encode(const std::string &addr, const std::vector<int> &args)
{
	std::string b = addr;
	pad(b);
	std::string tags = ",";
	for (size_t n = 0; n < args.size(); n++)
		tags += 'i';
	b += tags;
	pad(b);
	for (int v : args)
		be32(b, (uint32_t)v);
	return b;
}

bool decode(const char *p, size_t n, std::string &addr, std::vector<OscArg> &args)
{
	auto str = [&](size_t &at, std::string &out) {
		const void *z = memchr(p + at, 0, n - at);
		if (!z)
			return false;
		const size_t len = (const char *)z - (p + at);
		out.assign(p + at, len);
		at += (len + 4) & ~(size_t)3;
		return at <= n;
	};
	size_t at = 0;
	std::string tags;
	if (!str(at, addr) || addr.empty() || addr[0] != '/')
		return false;
	if (at >= n)
		return true;
	if (!str(at, tags) || tags.empty() || tags[0] != ',')
		return false;
	for (size_t t = 1; t < tags.size(); t++) {
		OscArg a;
		a.type = tags[t];
		if (a.type == 'i' || a.type == 'f') {
			if (at + 4 > n)
				return false;
			const uint32_t v = ((uint32_t)(uint8_t)p[at] << 24) | ((uint32_t)(uint8_t)p[at + 1] << 16) |
					   ((uint32_t)(uint8_t)p[at + 2] << 8) | (uint32_t)(uint8_t)p[at + 3];
			at += 4;
			if (a.type == 'i')
				a.i = (int32_t)v;
			else
				memcpy(&a.f, &v, 4);
		} else if (a.type == 's') {
			if (!str(at, a.s))
				return false;
		} else {
			continue; // T, F, N...: no data
		}
		args.push_back(a);
	}
	return true;
}

SOCKET g_sock = INVALID_SOCKET;
std::mutex g_sendm;
int g_sentTo = 0;

void openSocket()
{
	const Settings s = settings();
	if (g_sock != INVALID_SOCKET)
		closesocket(g_sock);
	g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (g_sock == INVALID_SOCKET)
		return;
	sockaddr_in me{};
	me.sin_family = AF_INET;
	me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	me.sin_port = htons((unsigned short)s.replyPort);
	if (bind(g_sock, (sockaddr *)&me, sizeof(me)) != 0) {
		// The reply port is taken: send from any free port instead (OBSBOT Center can still reply to it)
		me.sin_port = 0;
		bind(g_sock, (sockaddr *)&me, sizeof(me));
		obs_log(LOG_WARNING, "camera: port %d is in use, replies from OBSBOT Center come to another port", s.replyPort);
	}
#ifdef _WIN32
	DWORD t = 200;
	setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
#else
	timeval t{0, 200000};
	setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
#endif
	g_sentTo = s.oscPort;
}

void osc(const std::string &addr, const std::vector<int> &args = {1})
{
	std::lock_guard<std::mutex> lock(g_sendm);
	if (g_sock == INVALID_SOCKET)
		return;
	const std::string b = encode(addr, args);
	sockaddr_in to{};
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	to.sin_port = htons((unsigned short)g_sentTo);
	sendto(g_sock, b.data(), (int)b.size(), 0, (sockaddr *)&to, sizeof(to));
}

/* ------------------------------------------------------------------ what's known about the camera */
std::mutex g_m;
std::atomic<bool> g_run{false};
std::thread g_recv, g_loop;

bool g_centerRunning = false; // OBSBOT Center's process is running
int64_t g_lastReply = 0;      // the last time OBSBOT Center answered
bool g_prepped = false;
int64_t g_startingUntil = 0; // the plug-in started OBSBOT Center and is waiting for it
bool g_usb = true;           // the camera is plugged in (as far as OBS's camera source can tell)
bool g_busy = false;         // OBS isn't getting the camera's picture (OBSBOT Center's preview holds it)
std::string g_program;

double g_pan = 0, g_tilt = 0, g_zoom = 0;
bool g_havePos = false, g_haveZoom = false;
int g_posSeq = 0, g_zoomSeq = 0;
int g_zoomTo = 0, g_zSent = -100;

struct Shot {
	double pan = 0, tilt = 0, zoom = 0;
	int v = 0;
	std::vector<unsigned char> thumb;
};
bool g_haveShot = false;
Shot g_shot;
int g_shotV = 0;

// The joystick: signed speeds -100..100 (x: right +, y: up +), who is pushing it and when they last said so
int g_jx = 0, g_jy = 0;
std::string g_joyOwner, g_joyOwnerName;
int64_t g_joyAt = 0;
int64_t g_joyRepAt = 0; // when the held joystick speeds were last sent again

int64_t g_watchUntil = 0; // a phone has Camera open (refreshed by its preview requests)
std::set<std::string> g_seenAddrs;
std::string g_lastCamEvt;

bool centerAnswering(int64_t now)
{
	return g_lastReply && now - g_lastReply < 6000;
}

bool atShotLocked()
{
	return g_haveShot && g_havePos && std::fabs(g_pan - g_shot.pan) < 0.6 && std::fabs(g_tilt - g_shot.tilt) < 0.6 &&
	       (!g_haveZoom || std::fabs(g_zoom - g_shot.zoom) < 1.5);
}

std::string errLocked(int64_t now)
{
	if (!g_usb)
		return "The camera isn't plugged into the front laptop. Check its USB cable.";
	if (g_startingUntil > now && !centerAnswering(now))
		return "OBSBOT Center is starting on the front laptop. The camera controls come back in a moment.";
	if (!g_centerRunning)
		return "OBSBOT Center isn't open on the front laptop.";
	return "";
}

// One axis of the joystick. Changing direction sends 0 to the old direction first, so the camera never holds two
// opposite commands at once.
void axis(int prev, int next, const char *neg, const char *pos)
{
	if (prev == next)
		return;
	if (prev && (prev < 0) != (next < 0))
		osc(std::string(G) + (prev < 0 ? neg : pos), {0});
	if (next)
		osc(std::string(G) + (next < 0 ? neg : pos), {std::abs(next)});
	else if (prev && (prev < 0) == (next < 0))
		osc(std::string(G) + (prev < 0 ? neg : pos), {0});
}

void stopJoyLocked()
{
	axis(g_jx, 0, "SetGimbalLeft", "SetGimbalRight");
	axis(g_jy, 0, "SetGimbalDown", "SetGimbalUp");
	g_jx = g_jy = 0;
}

void prep()
{
	// What state it's in, then AI tracking off so nothing fights the joystick. (Gesture and voice control have no
	// OSC command: turn those off once in OBSBOT Center.)
	osc(std::string(G) + "GetDeviceInfo");
	osc("/OBSBOT/WebCam/Tiny/SetAiMode", {0});
	osc("/OBSBOT/WebCam/Tiny/ToggleAILock", {0});
}

/* ------------------------------------------------------------------ OBSBOT Center, the app */
#ifdef _WIN32
std::wstring widen(const std::string &s)
{
	if (s.empty())
		return L"";
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

std::wstring lowerW(std::wstring s)
{
	for (auto &c : s)
		c = (wchar_t)towlower(c);
	return s;
}

bool centerProcess()
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return false;
	PROCESSENTRY32W pe{};
	pe.dwSize = sizeof(pe);
	bool found = false;
	for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
		const std::wstring n = lowerW(pe.szExeFile);
		found = n.find(L"obsbot") != std::wstring::npos && n.find(L"uninst") == std::wstring::npos &&
			n.find(L"update") == std::wstring::npos;
	}
	CloseHandle(snap);
	return found;
}

// OBSBOT Center's program in its folder (searched a couple of levels down)
std::wstring findCenterExe(const std::wstring &dir, int depth = 0)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return L"";
	std::wstring found;
	std::vector<std::wstring> subdirs;
	do {
		const std::wstring name = fd.cFileName;
		if (name == L"." || name == L"..")
			continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			subdirs.push_back(dir + L"\\" + name);
			continue;
		}
		const std::wstring n = lowerW(name);
		if (n.size() > 4 && n.substr(n.size() - 4) == L".exe" && n.find(L"obsbot") != std::wstring::npos &&
		    n.find(L"uninst") == std::wstring::npos && n.find(L"update") == std::wstring::npos &&
		    n.find(L"crash") == std::wstring::npos)
			found = dir + L"\\" + name;
	} while (found.empty() && FindNextFileW(h, &fd));
	FindClose(h);
	if (found.empty() && depth < 2)
		for (auto &d : subdirs) {
			found = findCenterExe(d, depth + 1);
			if (!found.empty())
				break;
		}
	return found;
}

std::string launchCenter()
{
	const std::wstring exe = findCenterExe(widen(settings().centerDir));
	if (exe.empty())
		return "Can't find OBSBOT Center on the front laptop. Check its folder in OBS's BGH LIVE Front panel.";
	const std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
	// Minimized, without taking the focus, so it stays out of the way
	const auto r = (INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWMINNOACTIVE);
	if (r <= 32)
		return "OBSBOT Center didn't start.";
	return "";
}
#else
bool centerProcess()
{
	return false;
}
std::string launchCenter()
{
	return "OBSBOT Center can only be started on Windows.";
}
#endif

/* ------------------------------------------------------------------ OBS's camera source, for the preview */
obs_weak_source_t *g_camSrc = nullptr; // the OBSBOT video capture device, or the Baptism Cam scene if there's none
bool g_srcIsDevice = false;
bool g_showing = false;

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

obs_source_t *camSource()
{
	return g_camSrc ? obs_weak_source_get_source(g_camSrc) : nullptr;
}

// Finds the OBSBOT capture device, makes sure it keeps running when Baptism Cam isn't showing (so the phone's preview
// works before it goes live), and checks it's still plugged in. UI thread.
void checkSource()
{
	obs_source_t *dev = nullptr;
	obs_enum_sources(
		[](void *p, obs_source_t *s) {
			const char *id = obs_source_get_unversioned_id(s);
			if (!id || strcmp(id, "dshow_input") != 0)
				return true;
			obs_data_t *st = obs_source_get_settings(s);
			const std::string dev = lower(obs_data_get_string(st, "video_device_id"));
			obs_data_release(st);
			if (dev.find("obsbot") == std::string::npos)
				return true;
			*(obs_source_t **)p = obs_source_get_ref(s);
			return false;
		},
		&dev);
	bool usb = true;
	if (dev) {
		obs_data_t *st = obs_source_get_settings(dev);
		const std::string want = obs_data_get_string(st, "video_device_id");
		if (obs_data_get_bool(st, "deactivate_when_not_showing")) {
			obs_data_t *ch = obs_data_create();
			obs_data_set_bool(ch, "deactivate_when_not_showing", false);
			obs_source_update(dev, ch);
			obs_data_release(ch);
			obs_log(LOG_INFO, "camera: Baptism Cam's camera now stays on when the scene isn't showing");
		}
		obs_data_release(st);
		// Is the device still in Windows' list of cameras?
		obs_properties_t *props = obs_source_properties(dev);
		obs_property_t *list = props ? obs_properties_get(props, "video_device_id") : nullptr;
		if (list) {
			usb = false;
			const size_t n = obs_property_list_item_count(list);
			for (size_t i = 0; i < n; i++)
				if (want == obs_property_list_item_string(list, i))
					usb = true;
		}
		obs_properties_destroy(props);
	}
	obs_source_t *use = dev;
	if (!use)
		use = obs_get_source_by_name(kScene);
	obs_weak_source_release(g_camSrc);
	g_camSrc = use ? obs_source_get_weak_source(use) : nullptr;
	g_srcIsDevice = dev != nullptr;
	obs_source_release(use);

	std::lock_guard<std::mutex> lock(g_m);
	if (g_usb && !usb && g_haveShot) {
		g_haveShot = false; // the camera was unplugged: the saved shot no longer means anything
		g_shotV++;
		obs_log(LOG_INFO, "camera: unplugged, so the saved shot was forgotten");
	}
	if (!usb)
		stopJoyLocked();
	g_usb = usb;
}

std::mutex g_grabm;
gs_texrender_t *g_tex = nullptr;
gs_stagesurf_t *g_stage = nullptr;
uint32_t g_cx = 0, g_cy = 0;
std::vector<unsigned char> g_live;
int64_t g_liveAt = 0;

std::vector<unsigned char> grab(int width)
{
	std::vector<unsigned char> out;
	obs_source_t *src = camSource();
	if (!src)
		return out;
	const uint32_t sw = obs_source_get_width(src), sh = obs_source_get_height(src);
	if (!sw || !sh) {
		obs_source_release(src);
		return out;
	}
	const uint32_t cx = (uint32_t)width, cy = std::max<uint32_t>(16, (uint32_t)std::lround((double)width * sh / sw));
	QImage img;
	obs_enter_graphics();
	if (g_cx != cx || g_cy != cy || !g_tex) {
		gs_texrender_destroy(g_tex);
		gs_stagesurface_destroy(g_stage);
		g_tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		g_stage = gs_stagesurface_create(cx, cy, GS_RGBA);
		g_cx = cx;
		g_cy = cy;
	}
	gs_texrender_reset(g_tex);
	if (g_tex && g_stage && gs_texrender_begin(g_tex, cx, cy)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(src);
		gs_blend_state_pop();
		gs_texrender_end(g_tex);
		gs_stage_texture(g_stage, gs_texrender_get_texture(g_tex));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(g_stage, &data, &linesize)) {
			img = QImage(data, (int)cx, (int)cy, (int)linesize, QImage::Format_RGBA8888).convertToFormat(QImage::Format_RGB888);
			gs_stagesurface_unmap(g_stage);
		}
	}
	obs_leave_graphics();
	obs_source_release(src);
	if (img.isNull())
		return out;
	QByteArray bytes;
	QBuffer buf(&bytes);
	buf.open(QIODevice::WriteOnly);
	if (!img.save(&buf, "JPG", 72)) {
		buf.close();
		bytes.clear();
		buf.open(QIODevice::WriteOnly);
		img.save(&buf, "PNG");
	}
	out.assign(bytes.begin(), bytes.end());
	return out;
}

/* ------------------------------------------------------------------ the two threads */
void handleReply(const std::string &addr, const std::vector<OscArg> &args)
{
	const int64_t now = nowMs();
	std::lock_guard<std::mutex> lock(g_m);
	const bool wasAnswering = centerAnswering(now);
	g_lastReply = now;
	if (g_seenAddrs.insert(addr).second) {
		std::string a;
		for (auto &x : args)
			a += " " + (x.type == 's' ? x.s : std::to_string(x.num()));
		obs_log(LOG_INFO, "camera: OBSBOT Center sent %s%s", addr.c_str(), a.c_str());
	}
	if (!wasAnswering)
		g_prepped = false; // it (re)connected: get the camera ready again
	const auto ends = [&](const char *s) {
		const size_t n = strlen(s);
		return addr.size() >= n && addr.compare(addr.size() - n, n, s) == 0;
	};
	if ((ends("GimbalPosInfoResp") || ends("GimbalPosInfo")) && args.size() >= 2) {
		g_tilt = args[args.size() - 2].num(); // (roll,) pitch, yaw
		g_pan = args[args.size() - 1].num();
		g_havePos = true;
		g_posSeq++;
	} else if ((ends("ZoomInfo") || ends("ZoomInfoResp")) && !args.empty()) {
		g_zoom = args[0].num();
		g_haveZoom = true;
		g_zoomSeq++;
	}
}

void recvLoop()
{
	char buf[2048];
	while (g_run) {
		if (g_sock == INVALID_SOCKET) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			continue;
		}
		sockaddr_in from{};
		socklen_t len = sizeof(from);
		const int n = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr *)&from, &len);
		if (n <= 0)
			continue;
		// An OSC bundle holds messages: open it up
		if (n >= 16 && memcmp(buf, "#bundle", 8) == 0) {
			size_t at = 16;
			while (at + 4 <= (size_t)n) {
				const uint32_t sz = ((uint32_t)(uint8_t)buf[at] << 24) | ((uint32_t)(uint8_t)buf[at + 1] << 16) |
						    ((uint32_t)(uint8_t)buf[at + 2] << 8) | (uint32_t)(uint8_t)buf[at + 3];
				at += 4;
				if (at + sz > (size_t)n)
					break;
				std::string addr;
				std::vector<OscArg> args;
				if (decode(buf + at, sz, addr, args))
					handleReply(addr, args);
				at += sz;
			}
			continue;
		}
		std::string addr;
		std::vector<OscArg> args;
		if (decode(buf, (size_t)n, addr, args))
			handleReply(addr, args);
	}
}

void mainLoop()
{
	int64_t lastHello = 0, lastPoll = 0, lastProc = 0;
	while (g_run) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		const int64_t now = nowMs();
		if (now - lastProc >= 1000) {
			lastProc = now;
			const bool running = centerProcess();
			std::lock_guard<std::mutex> lock(g_m);
			g_centerRunning = running;
		}
		bool doPrep = false, poll = false;
		{
			std::lock_guard<std::mutex> lock(g_m);
			// The joystick: if a phone stops saying "still pushing" (it dropped off the Wi-Fi mid-push), stop the camera
			if ((g_jx || g_jy) && now - g_joyAt > 600) {
				stopJoyLocked();
				obs_log(LOG_INFO, "camera: the joystick went quiet, so the camera was stopped");
			}
			if (!g_centerRunning)
				g_prepped = false;
			if (g_centerRunning && !g_prepped && (centerAnswering(now) || now - lastHello > 4000)) {
				g_prepped = true;
				doPrep = true;
			}
			poll = g_watchUntil > now && g_centerRunning;
		}
		if (now - lastHello >= 2000) {
			lastHello = now;
			if (centerProcess())
				osc(std::string(G) + "Connected");
		}
		if (doPrep)
			prep();
		// While a phone has Camera open: where it's pointing and the zoom, a few times a second
		if (poll && now - lastPoll >= 300) {
			lastPoll = now;
			osc(std::string(G) + "GetGimbalPosInfo");
			osc(std::string(G) + "GetZoomInfo");
		}
		std::string evt;
		{
			std::lock_guard<std::mutex> lock(g_m);
			if (poll)
				evt = std::string("{\"zoom\":") + std::to_string((int)std::lround(g_haveZoom ? g_zoom : g_zoomTo)) +
				      ",\"at\":" + (atShotLocked() ? "true" : "false") + "}";
			if (evt == g_lastCamEvt)
				evt.clear();
			else
				g_lastCamEvt = evt;
		}
		if (!evt.empty())
			serverBroadcast("cam", evt);
	}
}

#ifdef _WIN32
void keepAwake(bool on)
{
	SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
}
#else
void keepAwake(bool) {}
#endif

void wake()
{
	osc(std::string(G) + "WakeSleep", {1});
}

} // namespace

/* ------------------------------------------------------------------ the rest of the plug-in */
void camStart()
{
	if (g_run)
		return;
	loadSettings();
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	openSocket();
	g_run = true;
	g_recv = std::thread(recvLoop);
	g_loop = std::thread(mainLoop);
	const Settings s = settings();
	keepAwake(s.keepAwake);
	if (s.autoCenter && !centerProcess()) {
		const std::string err = launchCenter();
		if (err.empty()) {
			std::lock_guard<std::mutex> lock(g_m);
			g_startingUntil = nowMs() + 30000;
			obs_log(LOG_INFO, "camera: OBS opened, so OBSBOT Center was started (minimized)");
		} else {
			obs_log(LOG_WARNING, "camera: %s", err.c_str());
		}
	}
}

void camStop()
{
	if (!g_run)
		return;
	{
		std::lock_guard<std::mutex> lock(g_m);
		stopJoyLocked();
		g_haveShot = false; // each service starts fresh
	}
	g_run = false;
	{
		std::lock_guard<std::mutex> lock(g_sendm);
		closesocket(g_sock);
		g_sock = INVALID_SOCKET;
	}
	if (g_recv.joinable())
		g_recv.join();
	if (g_loop.joinable())
		g_loop.join();
	keepAwake(false);
	if (g_showing) {
		if (obs_source_t *s = camSource()) {
			obs_source_dec_showing(s);
			obs_source_release(s);
		}
		g_showing = false;
	}
	obs_weak_source_release(g_camSrc);
	g_camSrc = nullptr;
	obs_enter_graphics();
	gs_texrender_destroy(g_tex);
	gs_stagesurface_destroy(g_stage);
	g_tex = nullptr;
	g_stage = nullptr;
	obs_leave_graphics();
}

// UI thread, once a second
void camTick()
{
	static int n = 0;
	if (n++ % 3 == 0)
		checkSource();
	// The camera source has to be "showing" for OBS to keep its picture coming while a phone watches it
	bool watched;
	{
		std::lock_guard<std::mutex> lock(g_m);
		watched = g_watchUntil > nowMs();
	}
	obs_source_t *s = camSource();
	if (s && watched != g_showing) {
		if (watched)
			obs_source_inc_showing(s);
		else
			obs_source_dec_showing(s);
		g_showing = watched;
	}
	if (s) {
		const bool noPicture = watched && g_srcIsDevice && obs_source_get_width(s) == 0;
		std::lock_guard<std::mutex> lock(g_m);
		g_busy = noPicture && g_usb && g_centerRunning;
	}
	obs_source_release(s);
}

void camProgram(const std::string &program)
{
	bool wakeIt = false;
	{
		std::lock_guard<std::mutex> lock(g_m);
		wakeIt = program == kScene && g_program != kScene;
		g_program = program;
	}
	if (wakeIt)
		wake(); // Baptism Cam went live: make sure the camera is awake
}

std::string camJson()
{
	const int64_t now = nowMs();
	std::lock_guard<std::mutex> lock(g_m);
	const std::string err = errLocked(now);
	const bool starting = g_startingUntil > now && !centerAnswering(now);
	std::string shot = "null";
	if (g_haveShot)
		shot = "{\"zoom\":" + std::to_string((int)std::lround(g_shot.zoom)) + ",\"v\":" + std::to_string(g_shotV) + "}";
	return std::string("{\"ok\":") + (err.empty() ? "true" : "false") + ",\"err\":\"" + jsonEscape(err) + "\"" +
	       ",\"live\":" + (g_program == kScene ? "true" : "false") +
	       ",\"zoom\":" + std::to_string((int)std::lround(g_haveZoom ? g_zoom : g_zoomTo)) +
	       ",\"zoomTo\":" + std::to_string(g_zoomTo) + ",\"shot\":" + shot + ",\"at\":" + (atShotLocked() ? "true" : "false") +
	       ",\"usb\":" + (g_usb ? "true" : "false") + ",\"starting\":" + (starting ? "true" : "false") +
	       ",\"waking\":false,\"busy\":" + (g_busy ? "true" : "false") +
	       ",\"canStart\":" + (g_usb && !g_centerRunning && !starting ? "true" : "false") + "}";
}

std::string camCommand(const std::string &cmd, obs_data_t *args, const std::string &phoneId, const std::string &phoneName)
{
	const int64_t now = nowMs();
	if (cmd == "camStartCenter") {
		if (centerProcess())
			return "";
		const std::string err = launchCenter();
		if (err.empty()) {
			std::lock_guard<std::mutex> lock(g_m);
			g_startingUntil = now + 30000;
		}
		return err;
	}
	if (cmd == "camOpen") {
		{
			std::lock_guard<std::mutex> lock(g_m);
			g_watchUntil = now + 10000;
		}
		wake(); // a sleeping camera is ready by the time a thumb reaches the joystick
		return "";
	}
	const Settings s = settings();
	std::unique_lock<std::mutex> lock(g_m);
	const std::string err = errLocked(now);
	if (!err.empty())
		return err;
	if (cmd == "camJoy") {
		// One phone at a time: while someone is pushing the stick, other phones are told who has it
		if (!g_joyOwner.empty() && g_joyOwner != phoneId && (g_jx || g_jy) && now - g_joyAt < 1000)
			return (g_joyOwnerName.empty() ? std::string("Another phone") : g_joyOwnerName) +
			       " is moving the camera right now.";
		const double k = (s.upside ? -1.0 : 1.0) * s.topSpeed / 100.0;
		auto m = [&](double v) {
			const int n = (int)std::lround(std::clamp(v, -100.0, 100.0) * k);
			return std::abs(n) < 2 ? 0 : n;
		};
		const int x = m(obs_data_get_double(args, "x")), y = m(obs_data_get_double(args, "y"));
		g_joyAt = now;
		// Each time a pan starts or changes direction, so the log shows what the camera was asked to do
		if (x && (!g_jx || (x < 0) != (g_jx < 0)))
			obs_log(LOG_INFO, "camera: joystick pan %s (speed %d)", x < 0 ? "left" : "right", std::abs(x));
		axis(g_jx, x, "SetGimbalLeft", "SetGimbalRight");
		axis(g_jy, y, "SetGimbalDown", "SetGimbalUp");
		// Held still, the phone repeats itself every 250 ms. Pass that on, so OBSBOT Center keeps moving the camera
		// even if it lets a single command lapse.
		if (now - g_joyRepAt >= 240) {
			g_joyRepAt = now;
			if (x && x == g_jx)
				osc(std::string(G) + (x < 0 ? "SetGimbalLeft" : "SetGimbalRight"), {std::abs(x)});
			if (y && y == g_jy)
				osc(std::string(G) + (y < 0 ? "SetGimbalDown" : "SetGimbalUp"), {std::abs(y)});
		}
		g_jx = x;
		g_jy = y;
		if (x || y) {
			g_joyOwner = phoneId;
			g_joyOwnerName = phoneName;
		}
		return "";
	}
	if (cmd == "camZoom") {
		// The slider says where it is and the camera eases there: steps of 4 or more, and the last value
		g_zoomTo = std::clamp((int)obs_data_get_int(args, "z"), 0, 100);
		if (obs_data_get_bool(args, "final") || std::abs(g_zoomTo - g_zSent) >= 4) {
			g_zSent = g_zoomTo;
			osc(std::string(G) + "SetZoom", {g_zoomTo});
		}
		return "";
	}
	if (cmd == "camSet") {
		// Ask the camera where it's pointing and how far it's zoomed, then remember that
		const int pos0 = g_posSeq, zoom0 = g_zoomSeq;
		lock.unlock();
		osc(std::string(G) + "GetGimbalPosInfo");
		osc(std::string(G) + "GetZoomInfo");
		for (int i = 0; i < 20; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			std::lock_guard<std::mutex> l2(g_m);
			if (g_posSeq != pos0 && g_zoomSeq != zoom0)
				break;
		}
		std::vector<unsigned char> thumb = grab(320);
		lock.lock();
		if (!g_havePos)
			return "OBSBOT Center didn't say where the camera is pointing, so the shot couldn't be saved. Check its OSC setting.";
		g_shot.pan = g_pan;
		g_shot.tilt = g_tilt;
		g_shot.zoom = g_haveZoom ? g_zoom : g_zoomTo;
		g_shot.thumb.swap(thumb);
		g_haveShot = true;
		g_shotV++;
		obs_log(LOG_INFO, "camera: shot saved (pan %.1f, tilt %.1f, zoom %.0f)", g_shot.pan, g_shot.tilt, g_shot.zoom);
		return "";
	}
	if (cmd == "camGo") {
		if (!g_haveShot)
			return "No shot saved yet. Aim the camera, then press Set.";
		stopJoyLocked();
		osc(std::string(G) + "SetGimMotorDegree",
		    {kShotSpeed, (int)std::lround(g_shot.pan), (int)std::lround(g_shot.tilt)});
		g_zoomTo = g_zSent = (int)std::lround(g_shot.zoom);
		osc(std::string(G) + "SetZoom", {g_zoomTo});
		return "";
	}
	return "That isn't something the remote can do.";
}

// "live": the camera now (at most 5 a second); "shot": the picture taken when the shot was saved
std::vector<unsigned char> camImage(const std::string &which)
{
	const int64_t now = nowMs();
	if (which == "shot") {
		std::lock_guard<std::mutex> lock(g_m);
		return g_haveShot ? g_shot.thumb : std::vector<unsigned char>();
	}
	{
		std::lock_guard<std::mutex> lock(g_m);
		g_watchUntil = now + 3000; // keeps the camera's picture coming and the numbers updating
	}
	std::lock_guard<std::mutex> lock(g_grabm);
	if (now - g_liveAt >= 200 || g_live.empty()) {
		std::vector<unsigned char> img = grab(480);
		if (!img.empty())
			g_live.swap(img);
		g_liveAt = now;
	}
	return g_live;
}

/* ------------------------------------------------------------------ for the OBS panel */
CamPanel camPanel()
{
	const int64_t now = nowMs();
	CamPanel p;
	const Settings s = settings();
	p.autoCenter = s.autoCenter;
	p.keepAwake = s.keepAwake;
	p.upside = s.upside;
	p.topSpeed = s.topSpeed;
	p.oscPort = s.oscPort;
	p.replyPort = s.replyPort;
	p.centerDir = s.centerDir;
	std::lock_guard<std::mutex> lock(g_m);
	p.err = errLocked(now);
	p.usb = g_usb;
	p.running = g_centerRunning;
	p.answering = centerAnswering(now);
	p.starting = g_startingUntil > now && !p.answering;
	p.busy = g_busy;
	p.hasSource = g_srcIsDevice;
	return p;
}

void camSetPanel(const CamPanel &p)
{
	bool reopen;
	{
		std::lock_guard<std::mutex> lock(g_sm);
		reopen = p.oscPort != g_set.oscPort || p.replyPort != g_set.replyPort;
		g_set.autoCenter = p.autoCenter;
		g_set.keepAwake = p.keepAwake;
		g_set.upside = p.upside;
		g_set.topSpeed = std::clamp(p.topSpeed, 30, 100);
		g_set.oscPort = p.oscPort;
		g_set.replyPort = p.replyPort;
		g_set.centerDir = p.centerDir;
	}
	saveSettings();
	keepAwake(p.keepAwake);
	if (reopen) {
		std::lock_guard<std::mutex> lock(g_sendm);
		openSocket();
	}
}

std::string camStartCenterNow()
{
	return camCommand("camStartCenter", nullptr, "", "");
}

// Check directions: a short nudge left, then back, so whoever set it up can see the joystick and the camera agree
std::string camCheckDirections()
{
	{
		std::lock_guard<std::mutex> lock(g_m);
		const std::string err = errLocked(nowMs());
		if (!err.empty())
			return err;
	}
	wake();
	std::thread([]() {
		const int sp = settings().upside ? 30 : -30; // negative: left, as the joystick would send it
		auto step = [](int x, int ms) {
			{
				std::lock_guard<std::mutex> lock(g_m);
				axis(g_jx, x, "SetGimbalLeft", "SetGimbalRight");
				g_jx = x;
				g_joyAt = nowMs() + ms; // the watchdog leaves it alone for the step
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		};
		step(sp, 700);
		step(-sp, 700);
		step(0, 10);
	}).detach();
	return "";
}

} // namespace front
