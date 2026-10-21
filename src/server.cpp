/*
BGH LIVE Front
Copyright (C) 2026 Barrington Gospel Hall

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/
// The little web server phones on the church Wi-Fi talk to, on port 8650.
//   GET  /                 the phone remote (data/phone.html)
//   POST /api/pair         {code} or {key} from the QR code -> {token}
//   GET  /api/events       live updates (server-sent events): hello, state, unpaired
//   POST /api/cmd          {cmd, args} with "Authorization: Bearer <token>"
//   GET  /api/img          the current or next slide, as a PNG
//   GET  /hymns            the Soterio hymn book, opened from here so its Presenter Mode can reach OBS (data/hymns.html)
//   GET  /hymn-screen      the hymn verse on the projector, for an OBS Browser Source (data/hymn-screen.html)
//   GET  /new-believers.json  The New Believers Hymn Book for the remote's Hymn Sing mode (data/new-believers.json)
//   GET  /hymn-qr.svg      the QR code /hymn-screen shows between hymns: the hymn book on the internet
//   GET  /api/hymn-events  live updates for /hymn-screen (server-sent events): hymn. Needs no pairing: it only sends
//                          the words on the screen
// Each request gets its own short-lived thread. Live-update connections are kept in a list and written to by
// one sender thread, which also sends a keep-alive every 15 seconds.

#include <obs-module.h>
#include <util/platform.h>
#include <plugin-support.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "front.h"
#include "qrcodegen.hpp"

namespace front {

namespace {

struct Client {
	SOCKET s = INVALID_SOCKET;
	std::string phone, mode; // mode "screen": a /hymn-screen page, which only gets hymn events
};

struct Message {
	std::string event, data, phone; // phone: only to this phone ("" for everyone)
	bool close = false;             // close that phone's connections after sending
};

std::atomic<bool> g_run{false};
SOCKET g_listen = INVALID_SOCKET;
std::thread g_accept, g_sender;
std::mutex g_cm;
std::vector<std::shared_ptr<Client>> g_clients;
std::mutex g_qm;
std::condition_variable g_qcv;
std::deque<Message> g_queue;
std::string g_html, g_hymnsHtml, g_screenHtml, g_hymnQr, g_nbh;
// The hymn verse on the screen, as the JSON a phone sent ({} when the screen is clear)
std::mutex g_hm;
std::string g_hymn = "{}";

std::string currentHymn()
{
	std::lock_guard<std::mutex> lock(g_hm);
	return g_hymn;
}

const char *kIcon =
	"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"412 0 1096 1920\"><rect x=\"412\" y=\"0\" width=\"1096\" "
	"height=\"1920\" rx=\"182\" fill=\"#F5B700\"/><path d=\"M756 332h129v455h279v105H756z\" fill=\"#132932\"/>"
	"<circle cx=\"960\" cy=\"1392\" r=\"207\" fill=\"none\" stroke=\"#132932\" stroke-width=\"146\"/></svg>";

const char *kManifest =
	"{\"name\":\"BGH Front Remote\",\"short_name\":\"BGH Front Remote\",\"start_url\":\"/\",\"display\":\"standalone\","
	"\"background_color\":\"#0B1418\",\"theme_color\":\"#0B1418\",\"icons\":["
	"{\"src\":\"/icon-192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\"},"
	"{\"src\":\"/icon-512.png\",\"sizes\":\"512x512\",\"type\":\"image/png\"},"
	"{\"src\":\"/icon.svg\",\"sizes\":\"any\",\"type\":\"image/svg+xml\"}]}";

// The logo as PNG and ICO files from data/icons, for Safari bookmarks, the home screen and browser tabs
std::map<std::string, std::pair<std::string, std::string>> g_icons; // path -> (type, bytes)

bool sendAll(SOCKET s, const char *data, size_t len)
{
	while (len > 0) {
		const int n = send(s, data, (int)std::min<size_t>(len, 1 << 16), 0);
		if (n <= 0)
			return false;
		data += n;
		len -= (size_t)n;
	}
	return true;
}

bool sendStr(SOCKET s, const std::string &str)
{
	return sendAll(s, str.data(), str.size());
}

void setTimeouts(SOCKET s, int ms)
{
#ifdef _WIN32
	DWORD t = (DWORD)ms;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
#else
	timeval t{ms / 1000, (ms % 1000) * 1000};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
#endif
}

const char *statusText(int code)
{
	switch (code) {
	case 200:
		return "OK";
	case 204:
		return "No Content";
	case 400:
		return "Bad Request";
	case 401:
		return "Unauthorized";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	default:
		return "Error";
	}
}

void respond(SOCKET s, int code, const std::string &type, const std::string &body, bool cache = false)
{
	std::string head = "HTTP/1.1 " + std::to_string(code) + " " + statusText(code) +
			   "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
			   (cache ? "\r\nCache-Control: private, max-age=600" : "\r\nCache-Control: no-store") +
			   "\r\nConnection: close\r\n\r\n";
	sendStr(s, head);
	if (!body.empty())
		sendStr(s, body);
}

void respondJson(SOCKET s, int code, const std::string &json)
{
	respond(s, code, "application/json; charset=utf-8", json);
}

std::string urlDecode(const std::string &in)
{
	std::string out;
	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] == '+')
			out += ' ';
		else if (in[i] == '%' && i + 2 < in.size()) {
			out += (char)std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
			i += 2;
		} else
			out += in[i];
	}
	return out;
}

std::string queryParam(const std::string &query, const std::string &name)
{
	std::stringstream ss(query);
	std::string part;
	while (std::getline(ss, part, '&')) {
		const size_t eq = part.find('=');
		if (eq != std::string::npos && part.substr(0, eq) == name)
			return urlDecode(part.substr(eq + 1));
	}
	return "";
}

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

struct Request {
	std::string method, path, query, body, auth, agent;
};

bool readRequest(SOCKET s, Request &r)
{
	std::string buf;
	char tmp[4096];
	size_t headerEnd = std::string::npos;
	while (headerEnd == std::string::npos) {
		const int n = recv(s, tmp, sizeof(tmp), 0);
		if (n <= 0)
			return false;
		buf.append(tmp, (size_t)n);
		headerEnd = buf.find("\r\n\r\n");
		if (buf.size() > 32 * 1024)
			return false;
	}
	std::istringstream head(buf.substr(0, headerEnd));
	std::string line, target, version;
	std::getline(head, line);
	{
		std::istringstream first(line);
		first >> r.method >> target >> version;
	}
	const size_t q = target.find('?');
	r.path = q == std::string::npos ? target : target.substr(0, q);
	r.query = q == std::string::npos ? "" : target.substr(q + 1);
	size_t length = 0;
	while (std::getline(head, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t colon = line.find(':');
		if (colon == std::string::npos)
			continue;
		const std::string key = lower(line.substr(0, colon));
		std::string value = line.substr(colon + 1);
		value.erase(0, value.find_first_not_of(' '));
		if (key == "content-length")
			length = (size_t)std::strtoul(value.c_str(), nullptr, 10);
		else if (key == "authorization")
			r.auth = value;
		else if (key == "user-agent")
			r.agent = value;
	}
	if (length > 64 * 1024)
		return false;
	r.body = buf.substr(headerEnd + 4);
	while (r.body.size() < length) {
		const int n = recv(s, tmp, sizeof(tmp), 0);
		if (n <= 0)
			return false;
		r.body.append(tmp, (size_t)n);
	}
	r.body.resize(std::min(r.body.size(), length));
	return true;
}

std::string bearer(const Request &r)
{
	const std::string prefix = "Bearer ";
	return r.auth.rfind(prefix, 0) == 0 ? r.auth.substr(prefix.size()) : "";
}

std::string ok()
{
	return "{\"ok\":true}";
}

std::string fail(const std::string &msg)
{
	return "{\"ok\":false,\"error\":\"" + jsonEscape(msg) + "\"}";
}

// Runs an OBS action on the UI thread, then sends everyone the new state
std::string onUi(const std::function<std::string()> &fn)
{
	auto result = std::make_shared<std::string>("OBS is busy. Try again.");
	const bool ran = runOnUi([fn, result]() {
		*result = fn();
		refreshState();
	});
	return ran ? *result : std::string("OBS didn't answer. Check OBS is open and not frozen.");
}

void handleCommand(SOCKET s, const Request &r, const std::string &phone)
{
	obs_data_t *d = obs_data_create_from_json(r.body.c_str());
	if (!d)
		return respondJson(s, 400, fail("That request wasn't understood."));
	const std::string cmd = obs_data_get_string(d, "cmd");
	obs_data_t *args = obs_data_get_obj(d, "args");
	if (!args)
		args = obs_data_create();
	std::string err;
	touchPhone(phone);
	if (cmd.rfind("cam", 0) == 0) {
		// The camera answers straight away; the joystick and zoom don't send everyone a full update
		std::string name;
		for (auto &p : phones())
			if (p.id == phone)
				name = p.name;
		err = camCommand(cmd, args, phone, name);
		if (cmd != "camJoy" && cmd != "camZoom")
			runOnUi([]() { refreshState(); }, 1500);
	} else if (cmd == "ping") {
	} else if (cmd == "mode") {
		const std::string mode = obs_data_get_string(args, "mode");
		if (mode == "presenter" || mode == "controller" || mode == "hymns")
			setPhoneMode(phone, mode);
		std::lock_guard<std::mutex> lock(g_cm);
		for (auto &c : g_clients)
			if (c->phone == phone)
				c->mode = mode;
	} else if (cmd == "scene") {
		const std::string label = obs_data_get_string(args, "label");
		err = onUi([label]() { return switchScene(label); });
	} else if (cmd == "rec") {
		const bool on = obs_data_get_bool(args, "on");
		err = onUi([on]() { return setRecording(on); });
	} else if (cmd == "proj") {
		const bool on = obs_data_get_bool(args, "on");
		err = onUi([on]() { return setProjecting(on); });
	} else if (cmd == "swap") {
		// Swap which is in front on the projector: the presentation or OBS's projector
		err = onUi([]() {
			// With OBS not projecting, the presentation is what's in front
			const bool toProjection = presentationInFront() || !isProjecting();
			setOnTop(toProjection ? "obs" : "presentation");
			// Swapping to the projection when OBS isn't projecting yet: start it, on the Black scene
			if (toProjection && !isProjecting()) {
				std::string e = switchScene("Black");
				if (e.empty())
					e = setProjecting(true);
				if (!e.empty())
					return e;
			}
			applyOnTop();
			return std::string();
		});
	} else if (cmd == "endshow") {
		// Stop presenting: ends PowerPoint's slide show, or presses Escape on the slideshow on the projector
		err = slidesStep(0);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		runOnUi([]() { slidesCapture(true); refreshState(); }, 1500);
	} else if (cmd == "hymn") {
		// Shows a hymn verse on /hymn-screen: args {show:{key, num, label, text, chorus, ...}}, or {} to clear it
		std::string json = "{}";
		obs_data_t *show = obs_data_get_obj(args, "show");
		if (show) {
			const char *j = obs_data_get_json(show);
			if (j && obs_data_get_string(show, "key")[0])
				json = j;
			obs_data_release(show);
		}
		// One line, as server-sent events need. Newlines inside the words are already written as \n.
		json.erase(std::remove_if(json.begin(), json.end(), [](char c) { return c == '\n' || c == '\r'; }),
			   json.end());
		{
			std::lock_guard<std::mutex> lock(g_hm);
			g_hymn = json;
		}
		serverBroadcast("hymn", json);
		{
			int screens = 0;
			std::lock_guard<std::mutex> lock(g_cm);
			for (auto &c : g_clients)
				if (c->mode == "screen")
					screens++;
			obs_log(LOG_INFO, "hymn verses: %s (sent to %d hymn screen%s in OBS)",
				json == "{}" ? "cleared" : "a verse", screens, screens == 1 ? "" : "s");
		}
		// A verse: put its hymn book's scene on the projector, if it isn't already
		if (json != "{}") {
			obs_data_t *sh = obs_data_get_obj(args, "show");
			const std::string book = sh && std::string(obs_data_get_string(sh, "book")) == "nbh" ? "nbh" : "soterio";
			if (sh)
				obs_data_release(sh);
			err = onUi([book]() { return hymnSceneShow(book, false); });
		}
	} else if (cmd == "hymnbook") {
		// A hymn book chosen on the remote: its scene on the projector, projecting if OBS isn't
		const std::string book = std::string(obs_data_get_string(args, "book")) == "nbh" ? "nbh" : "soterio";
		err = onUi([book]() { return hymnSceneShow(book, true); });
	} else if (cmd == "pairpin") {
		// For the PIN the remote shows, to pair a phone using the Soterio hymn book: the current pairing code
		obs_data_release(args);
		obs_data_release(d);
		return respondJson(s, 200, "{\"ok\":true,\"code\":\"" + jsonEscape(pairCode()) + "\"}");
	} else if (cmd == "hymnback") {
		// Done with the hymn book: clear the verse and go back to the scene from before it
		{
			std::lock_guard<std::mutex> lock(g_hm);
			g_hymn = "{}";
		}
		serverBroadcast("hymn", "{}");
		err = onUi([]() { return hymnSceneBack(); });
	} else if (cmd == "slide") {
		const int delta = (int)obs_data_get_int(args, "d");
		err = slidesStep(delta < 0 ? -1 : 1);
		if (err.empty() && slidesApp() != "PowerPoint") {
			// A browser slideshow: give it a moment to draw the new slide, grab the projector, and send it.
			// Once more a little later, for slideshows with a transition.
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			runOnUi([]() { slidesCapture(true); refreshState(); }, 1500);
			std::thread([]() {
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				runOnUi([]() { slidesCapture(true); refreshState(); }, 1500);
			}).detach();
		} else {
			runOnUi([]() { refreshState(); }, 1500); // PowerPoint's new slide is already in
		}
	} else {
		err = "That isn't something the remote can do.";
	}
	obs_data_release(args);
	obs_data_release(d);
	respondJson(s, 200, err.empty() ? ok() : fail(err));
}

void handlePair(SOCKET s, const Request &r)
{
	obs_data_t *d = obs_data_create_from_json(r.body.c_str());
	const std::string code = d ? obs_data_get_string(d, "code") : "";
	const std::string key = d ? obs_data_get_string(d, "key") : "";
	if (d)
		obs_data_release(d);
	std::string err;
	const std::string token = pairPhone(code, key, r.agent, err);
	if (!token.empty())
		return respondJson(s, 200, "{\"ok\":true,\"token\":\"" + token + "\"}");
	const size_t colon = err.find(':');
	const std::string kind = err.substr(0, colon), n = colon == std::string::npos ? "0" : err.substr(colon + 1);
	if (kind == "LOCKED")
		return respondJson(s, 200, "{\"ok\":false,\"code\":\"LOCKED\",\"wait\":" + n + "}");
	respondJson(s, 200, "{\"ok\":false,\"code\":\"WRONG\",\"left\":" + n + "}");
}

// Keeps the connection open and hands it to the sender thread
void handleEvents(SOCKET s, const std::string &phone, const std::string &mode)
{
	setTimeouts(s, 5000);
	std::string head =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n"
		"X-Accel-Buffering: no\r\n\r\n";
	head += "retry: 2000\n\n";
	head += "event: hello\ndata: {\"now\":" + std::to_string(nowMs()) + ",\"id\":\"" + phone + "\"}\n\n";
	const std::string st = stateJson();
	if (!st.empty())
		head += "event: state\ndata: " + st + "\n\n";
	head += "event: hymn\ndata: " + currentHymn() + "\n\n";
	if (!sendStr(s, head)) {
		closesocket(s);
		return;
	}
	auto c = std::make_shared<Client>();
	c->s = s;
	c->phone = phone;
	c->mode = mode;
	if (mode == "presenter" || mode == "controller" || mode == "hymns")
		setPhoneMode(phone, mode);
	phoneConnected(phone, true);
	std::lock_guard<std::mutex> lock(g_cm);
	g_clients.push_back(c);
}

// A /hymn-screen page: keeps the connection open for hymn events only
void handleScreenEvents(SOCKET s)
{
	setTimeouts(s, 5000);
	std::string head =
		"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n"
		"X-Accel-Buffering: no\r\n\r\n";
	head += "retry: 2000\n\n";
	head += "event: hymn\ndata: " + currentHymn() + "\n\n";
	if (!sendStr(s, head)) {
		closesocket(s);
		return;
	}
	auto c = std::make_shared<Client>();
	c->s = s;
	c->mode = "screen";
	std::lock_guard<std::mutex> lock(g_cm);
	g_clients.push_back(c);
}

void handle(SOCKET s)
{
	setTimeouts(s, 10000);
	Request r;
	if (!readRequest(s, r)) {
		closesocket(s);
		return;
	}
	if (r.method == "GET" && (r.path == "/" || r.path == "/index.html")) {
		respond(s, 200, "text/html; charset=utf-8", g_html.empty() ? "BGH LIVE Front: phone.html is missing." : g_html);
	} else if (r.method == "GET" && (r.path == "/hymns" || r.path == "/hymns/")) {
		respond(s, 200, "text/html; charset=utf-8",
			g_hymnsHtml.empty() ? "BGH LIVE Front: hymns.html is missing." : g_hymnsHtml);
	} else if (r.method == "GET" && r.path == "/hymn-screen") {
		respond(s, 200, "text/html; charset=utf-8",
			g_screenHtml.empty() ? "BGH LIVE Front: hymn-screen.html is missing." : g_screenHtml);
	} else if (r.method == "GET" && r.path == "/new-believers.json") {
		if (g_nbh.empty())
			respond(s, 404, "text/plain; charset=utf-8", "new-believers.json is missing");
		else
			respond(s, 200, "application/json; charset=utf-8", g_nbh, true);
	} else if (r.method == "GET" && r.path == "/hymn-qr.svg") {
		respond(s, 200, "image/svg+xml", g_hymnQr, true);
	} else if (r.method == "GET" && r.path == "/api/hymn-events") {
		return handleScreenEvents(s); // keeps the socket
	} else if (r.method == "GET" && r.path == "/icon.svg") {
		respond(s, 200, "image/svg+xml", kIcon);
	} else if (r.method == "GET" && g_icons.count(r.path)) {
		const auto &icon = g_icons.at(r.path); // read-only once the server has started
		respond(s, 200, icon.first, icon.second, true);
	} else if (r.method == "GET" && r.path == "/manifest.json") {
		respond(s, 200, "application/manifest+json", kManifest);
	} else if (r.method == "POST" && r.path == "/api/pair") {
		handlePair(s, r);
	} else if (r.method == "GET" && r.path == "/api/events") {
		const std::string phone = phoneForToken(queryParam(r.query, "t"));
		if (phone.empty())
			respondJson(s, 401, fail("This phone isn't paired."));
		else
			return handleEvents(s, phone, queryParam(r.query, "mode")); // keeps the socket
	} else if (r.method == "POST" && r.path == "/api/cmd") {
		const std::string phone = phoneForToken(bearer(r));
		if (phone.empty())
			respondJson(s, 401, fail("This phone isn't paired."));
		else
			handleCommand(s, r, phone);
	} else if (r.method == "GET" && r.path == "/api/cam") {
		// The camera's preview ("live", a few a second while Camera is open) or the saved shot's picture ("shot")
		if (phoneForToken(queryParam(r.query, "t")).empty()) {
			respondJson(s, 401, fail("This phone isn't paired."));
		} else {
			const std::string k = queryParam(r.query, "k") == "shot" ? "shot" : "live";
			const std::vector<unsigned char> img = camImage(k);
			const bool png = img.size() > 4 && img[0] == 0x89 && img[1] == 'P';
			if (img.empty())
				respond(s, 404, "text/plain", "");
			else
				respond(s, 200, png ? "image/png" : "image/jpeg", std::string(img.begin(), img.end()), k == "shot");
		}
	} else if (r.method == "GET" && r.path == "/api/img") {
		if (phoneForToken(queryParam(r.query, "t")).empty()) {
			respondJson(s, 401, fail("This phone isn't paired."));
		} else {
			const std::vector<unsigned char> png = slideImage(queryParam(r.query, "k") == "next" ? "next" : "cur");
			if (png.empty())
				respond(s, 404, "text/plain", "");
			else
				respond(s, 200, "image/png", std::string(png.begin(), png.end()), true); // the URL changes with each slide
		}
	} else {
		respond(s, 404, "text/plain; charset=utf-8", "Not found");
	}
	closesocket(s);
}

void acceptLoop()
{
	while (g_run) {
		sockaddr_in addr{};
		socklen_t len = sizeof(addr);
		SOCKET c = accept(g_listen, (sockaddr *)&addr, &len);
		if (c == INVALID_SOCKET) {
			if (!g_run)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		std::thread(handle, c).detach();
	}
}

void dropClient(const std::shared_ptr<Client> &c)
{
	closesocket(c->s);
	if (!c->phone.empty())
		phoneConnected(c->phone, false);
}

void senderLoop()
{
	auto lastPing = std::chrono::steady_clock::now();
	while (g_run) {
		std::deque<Message> batch;
		{
			std::unique_lock<std::mutex> lock(g_qm);
			g_qcv.wait_for(lock, std::chrono::seconds(5), []() { return !g_queue.empty() || !g_run; });
			batch.swap(g_queue);
		}
		if (!g_run)
			break;
		const bool ping = std::chrono::steady_clock::now() - lastPing > std::chrono::seconds(15);
		if (ping)
			lastPing = std::chrono::steady_clock::now();
		if (batch.empty() && !ping)
			continue;

		std::vector<std::shared_ptr<Client>> clients;
		{
			std::lock_guard<std::mutex> lock(g_cm);
			clients = g_clients;
		}
		std::vector<std::shared_ptr<Client>> dead;
		for (auto &c : clients) {
			bool alive = true, closeIt = false;
			std::string out;
			for (auto &m : batch) {
				if (!m.phone.empty() && m.phone != c->phone)
					continue;
				if (c->mode == "screen" && m.event != "hymn")
					continue;
				out += "event: " + m.event + "\ndata: " + m.data + "\n\n";
				if (m.close)
					closeIt = true;
			}
			if (ping)
				out += ": ping\n\n";
			if (!out.empty())
				alive = sendStr(c->s, out);
			if (!alive || closeIt)
				dead.push_back(c);
		}
		if (!dead.empty()) {
			std::lock_guard<std::mutex> lock(g_cm);
			for (auto &c : dead) {
				g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), c), g_clients.end());
				dropClient(c);
			}
		}
	}
}

std::string loadFile(const char *name)
{
	char *path = obs_module_file(name);
	if (!path)
		return "";
	std::ifstream f(path, std::ios::binary);
	bfree(path);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

void loadIcons()
{
	const std::pair<const char *, const char *> files[] = {
		{"icons/apple-touch-icon.png", "image/png"}, {"icons/icon-192.png", "image/png"},
		{"icons/icon-512.png", "image/png"},         {"icons/favicon-32.png", "image/png"},
		{"icons/favicon.ico", "image/x-icon"}};
	for (auto &f : files) {
		const std::string bytes = loadFile(f.first);
		if (!bytes.empty())
			g_icons["/" + std::string(f.first).substr(6)] = {f.second, bytes};
	}
	// Safari also asks for these names by itself
	if (g_icons.count("/apple-touch-icon.png"))
		g_icons["/apple-touch-icon-precomposed.png"] = g_icons["/apple-touch-icon.png"];
}

// A QR code as an SVG: black squares on white, with the 4-square margin scanners need
std::string qrSvg(const char *text)
{
	using qrcodegen::QrCode;
	const QrCode qr = QrCode::encodeText(text, QrCode::Ecc::MEDIUM);
	const int n = qr.getSize(), m = 4, size = n + m * 2;
	std::string path;
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++)
			if (qr.getModule(x, y))
				path += "M" + std::to_string(x + m) + "," + std::to_string(y + m) + "h1v1h-1z";
	return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " + std::to_string(size) + " " +
	       std::to_string(size) + "\" shape-rendering=\"crispEdges\"><rect width=\"100%\" height=\"100%\" fill=\"#fff\"/>" +
	       "<path d=\"" + path + "\" fill=\"#000\"/></svg>";
}

std::string loadHtml()
{
	char *path = obs_module_file("phone.html");
	if (!path)
		return "";
	std::ifstream f(path, std::ios::binary);
	bfree(path);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

} // namespace

bool serverStart()
{
	if (g_run)
		return true;
	g_html = loadHtml();
	g_hymnsHtml = loadFile("hymns.html");
	g_screenHtml = loadFile("hymn-screen.html");
	g_hymnQr = qrSvg(kHymnBookUrl);
	g_nbh = loadFile("new-believers.json");
	loadIcons();
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;
#endif
	g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_listen == INVALID_SOCKET)
		return false;
#ifdef _WIN32
	BOOL excl = TRUE;
	setsockopt(g_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof(excl));
#else
	int yes = 1;
	setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((unsigned short)kPort);
	if (bind(g_listen, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR || listen(g_listen, 16) == SOCKET_ERROR) {
		closesocket(g_listen);
		g_listen = INVALID_SOCKET;
		return false;
	}
	g_run = true;
	g_accept = std::thread(acceptLoop);
	g_sender = std::thread(senderLoop);
	obs_log(LOG_INFO, "phone remote listening on port %d", kPort);
	return true;
}

void serverStop()
{
	if (!g_run)
		return;
	g_run = false;
	closesocket(g_listen);
	g_listen = INVALID_SOCKET;
	g_qcv.notify_all();
	if (g_accept.joinable())
		g_accept.join();
	if (g_sender.joinable())
		g_sender.join();
	std::lock_guard<std::mutex> lock(g_cm);
	for (auto &c : g_clients)
		closesocket(c->s);
	g_clients.clear();
	// Request threads still running finish by themselves: their sockets time out within 10 seconds
}

void serverBroadcast(const std::string &event, const std::string &data)
{
	if (!g_run)
		return;
	{
		std::lock_guard<std::mutex> lock(g_qm);
		g_queue.push_back({event, data, "", false});
	}
	g_qcv.notify_all();
}

void serverDisconnectPhone(const std::string &id)
{
	if (!g_run)
		return;
	{
		std::lock_guard<std::mutex> lock(g_qm);
		g_queue.push_back({"unpaired", "{}", id, true});
	}
	g_qcv.notify_all();
}

int presenterViewers()
{
	std::lock_guard<std::mutex> lock(g_cm);
	int n = 0;
	for (auto &c : g_clients)
		if (c->mode == "presenter")
			n++;
	return n;
}

std::vector<std::string> localAddresses()
{
	std::vector<std::string> out;
#ifdef _WIN32
	ULONG size = 16 * 1024;
	std::vector<unsigned char> buf(size);
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG res = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
	if (res == ERROR_BUFFER_OVERFLOW) {
		buf.resize(size);
		res = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
	}
	if (res != NO_ERROR)
		return out;
	for (auto *a = (IP_ADAPTER_ADDRESSES *)buf.data(); a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
		    a->IfType == IF_TYPE_TUNNEL)
			continue;
		for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
			auto *sin = (sockaddr_in *)u->Address.lpSockaddr;
			char ip[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
			out.push_back(ip);
		}
	}
#else
	ifaddrs *list = nullptr;
	if (getifaddrs(&list) == 0) {
		for (ifaddrs *i = list; i; i = i->ifa_next) {
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
				continue;
			char ip[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &((sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof(ip));
			out.push_back(ip);
		}
		freeifaddrs(list);
	}
#endif
	// Home and church networks first (192.168.x, then 10.x, then 172.16-31.x); never loopback or self-assigned
	auto rank = [](const std::string &ip) {
		if (ip.rfind("192.168.", 0) == 0)
			return 0;
		if (ip.rfind("10.", 0) == 0)
			return 1;
		if (ip.rfind("172.", 0) == 0)
			return 2;
		return 3;
	};
	out.erase(std::remove_if(out.begin(), out.end(),
				 [](const std::string &ip) {
					 return ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0;
				 }),
		  out.end());
	std::stable_sort(out.begin(), out.end(),
			 [&](const std::string &a, const std::string &b) { return rank(a) < rank(b); });
	return out;
}

} // namespace front
