/*
BGH LIVE Front
Copyright (C) 2026 Barrington Gospel Hall

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/
// The slideshow on the projector.
//   PowerPoint: controlled directly (Next / Previous), so slides change even while an OBS projector covers the
//               slideshow. The phone gets the real slide count and pictures of this slide and the next one.
//   Anything else full screen on the OPTOMA (Google Slides or Keynote in a browser, a PDF...): the Left and Right
//               arrow keys are sent to that window, and the phone gets a picture of the projector screen.

#include <obs-module.h>
#include <plugin-support.h>

#include <QBuffer>
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QScreen>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <ole2.h>
#include <oleauto.h>
#include <cwctype>
#endif

#include "front.h"

namespace front {

namespace {

std::mutex g_m;
std::string g_app; // "PowerPoint", "screen" or ""
long g_n = 0, g_total = 0;
std::string g_ratio = "4:3";
int g_version = 0;
std::vector<unsigned char> g_cur, g_next;
size_t g_screenHash = 0;

struct Cmd {
	int delta;
	std::shared_ptr<std::promise<std::string>> done;
};
std::mutex g_qm;
std::condition_variable g_qcv;
std::deque<Cmd> g_queue;
std::atomic<bool> g_run{false};
std::thread g_worker;

std::string ratioFor(double w, double h)
{
	return (h > 0 && w / h > 1.55) ? "16:9" : "4:3";
}

#ifdef _WIN32
/* -------------------------------------------------------------- the OPTOMA monitor, by its EDID name */
std::wstring upperW(std::wstring s)
{
	for (auto &c : s)
		c = (wchar_t)towupper(c);
	return s;
}

struct MonFind {
	std::wstring device;
	HMONITOR found = nullptr;
};

BOOL CALLBACK monEnum(HMONITOR m, HDC, LPRECT, LPARAM p)
{
	auto *f = (MonFind *)p;
	MONITORINFOEXW mi{};
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoW(m, &mi) && f->device == mi.szDevice) {
		f->found = m;
		return FALSE;
	}
	return TRUE;
}

std::wstring widen(const std::string &s)
{
	if (s.empty())
		return L"";
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

HMONITOR projectorMonitor()
{
	// A screen chosen in the dock is matched by its exact name; otherwise any screen named OPTOMA
	const std::wstring chosen = widen(projectorChoice());
	const std::wstring want = upperW(widen(kProjectorName));
	UINT32 np = 0, nm = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS)
		return nullptr;
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS)
		return nullptr;
	MonFind f;
	for (UINT32 i = 0; i < np; i++) {
		DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
		tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		tn.header.size = sizeof(tn);
		tn.header.adapterId = paths[i].targetInfo.adapterId;
		tn.header.id = paths[i].targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS)
			continue;
		DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
		sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		sn.header.size = sizeof(sn);
		sn.header.adapterId = paths[i].sourceInfo.adapterId;
		sn.header.id = paths[i].sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS)
			continue;
		const std::wstring friendly = tn.monitorFriendlyDeviceName, gdi = sn.viewGdiDeviceName;
		const bool match = chosen.empty() ? upperW(friendly).find(want) != std::wstring::npos
						  : (friendly == chosen || gdi == chosen);
		if (match) {
			f.device = gdi;
			break;
		}
	}
	if (f.device.empty())
		return nullptr;
	EnumDisplayMonitors(nullptr, nullptr, monEnum, (LPARAM)&f);
	return f.found;
}

/* -------------------------------------------------------------- the window showing on the OPTOMA */
struct WinFind {
	HMONITOR mon;
	RECT monRect;
	DWORD self;
	HWND found = nullptr;
	bool coveredByObs = false; // an OBS window (the projector) is on top of it
};

std::wstring className(HWND w)
{
	wchar_t c[128] = {};
	GetClassNameW(w, c, 127);
	return c;
}

// Windows that are "visible" but aren't anything a person sees: overlays (NVIDIA, Discord, Xbox Game Bar),
// suspended Windows apps, the desktop and taskbar, tooltips
bool invisibleKind(HWND w)
{
	const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
	if (ex & (WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
		return true;
	BOOL cloaked = FALSE;
	if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
		return true;
	if (className(w) == L"screenClass")
		return false; // PowerPoint's slide show: it belongs to PowerPoint's main window, but it's what people see
	if (GetWindow(w, GW_OWNER) && !(ex & WS_EX_APPWINDOW))
		return true; // owned pop-ups (menus, tooltips)
	const std::wstring c = className(w);
	return c == L"Progman" || c == L"WorkerW" || c == L"Shell_TrayWnd" || c == L"Shell_SecondaryTrayWnd" ||
	       c == L"CEF-OSC-WIDGET" || c == L"Windows.UI.Core.CoreWindow" || c == L"ApplicationFrameWindow" && cloaked;
}

BOOL CALLBACK winEnum(HWND w, LPARAM p)
{
	auto *f = (WinFind *)p;
	if (!IsWindowVisible(w) || IsIconic(w) || invisibleKind(w))
		return TRUE;
	if (MonitorFromWindow(w, MONITOR_DEFAULTTONULL) != f->mon)
		return TRUE;
	RECT r;
	if (!GetWindowRect(w, &r))
		return TRUE;
	RECT x;
	if (!IntersectRect(&x, &r, &f->monRect))
		return TRUE;
	const long area = (x.right - x.left) * (x.bottom - x.top);
	const long monArea = (f->monRect.right - f->monRect.left) * (f->monRect.bottom - f->monRect.top);
	if (monArea <= 0 || area * 2 < monArea)
		return TRUE; // only windows that fill most of the projector
	DWORD pid = 0;
	GetWindowThreadProcessId(w, &pid);
	if (pid == f->self) {
		f->coveredByObs = true; // OBS's own projector: look underneath it
		return TRUE;
	}
	f->found = w;
	return FALSE;
}

BOOL CALLBACK findRender(HWND w, LPARAM p)
{
	if (className(w) == L"Chrome_RenderWidgetHostHWND" && IsWindowVisible(w)) {
		*(HWND *)p = w;
		return FALSE;
	}
	return TRUE;
}

// Brings a window to the front. Windows only lets the program the person last used do that, so first try
// the usual way, then once more after a do-nothing mouse nudge from this program (which counts as "last used").
bool bringToFront(HWND target)
{
	if (GetForegroundWindow() == target)
		return true;
	const DWORD me = GetCurrentThreadId();
	for (int attempt = 0; attempt < 2; attempt++) {
		if (attempt == 1) {
			INPUT nudge = {};
			nudge.type = INPUT_MOUSE;
			nudge.mi.dwFlags = MOUSEEVENTF_MOVE; // moves 0 pixels
			SendInput(1, &nudge, sizeof(INPUT));
		}
		HWND fg = GetForegroundWindow();
		const DWORD fgT = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
		const DWORD tgT = GetWindowThreadProcessId(target, nullptr);
		if (fgT && fgT != me)
			AttachThreadInput(me, fgT, TRUE);
		if (tgT && tgT != me && tgT != fgT)
			AttachThreadInput(me, tgT, TRUE);
		BringWindowToTop(target);
		SetForegroundWindow(target);
		if (tgT && tgT != me && tgT != fgT)
			AttachThreadInput(me, tgT, FALSE);
		if (fgT && fgT != me)
			AttachThreadInput(me, fgT, FALSE);
		for (int i = 0; i < 10 && GetForegroundWindow() != target; i++)
			Sleep(10);
		if (GetForegroundWindow() == target)
			return true;
	}
	return false;
}

HWND g_lastTarget = nullptr;

void postKey(HWND target, WORD vk)
{
	HWND to = nullptr;
	EnumChildWindows(target, findRender, (LPARAM)&to); // Chrome and Edge take keys in this inner window
	if (!to)
		to = target;
	const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	const LPARAM down = (LPARAM)1 | ((LPARAM)scan << 16) | ((LPARAM)1 << 24);
	const LPARAM up = down | ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
	PostMessageW(to, WM_KEYDOWN, vk, down);
	PostMessageW(to, WM_KEYUP, vk, up);
}

// Presses a key in the slideshow window. Returns false if it could only be posted in the background.
bool sendKeyTo(HWND target, WORD vk)
{
	HWND before = GetForegroundWindow();
	DWORD beforePid = 0;
	if (before)
		GetWindowThreadProcessId(before, &beforePid);
	if (!bringToFront(target)) {
		postKey(target, vk);
		return false;
	}
	INPUT in[2] = {};
	in[0].type = INPUT_KEYBOARD;
	in[0].ki.wVk = vk;
	in[0].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
	in[1] = in[0];
	in[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
	SendInput(2, in, sizeof(INPUT));
	Sleep(120); // let the browser read the key before the focus goes back
	// Give the focus back to OBS if it had it, so whoever is at the laptop carries on where they were
	if (before && before != target && beforePid == GetCurrentProcessId())
		bringToFront(before);
	return true;
}

// delta: +1 Right, -1 Left, 0 Escape (ends the slideshow, or leaves full screen in a browser)
std::string sendArrow(int delta)
{
	HMONITOR mon = projectorMonitor();
	if (!mon)
		return "Can't find the " + projectorLabel() + " projector. Check its HDMI cable.";
	MONITORINFO mi{};
	mi.cbSize = sizeof(mi);
	GetMonitorInfoW(mon, &mi);
	WinFind f{mon, mi.rcMonitor, GetCurrentProcessId()};
	EnumWindows(winEnum, (LPARAM)&f);
	if (!f.found)
		return "There's no slideshow showing on the projector. Put the presentation full screen on the projector.";
	if (f.found != g_lastTarget) {
		// The log says which window the arrow keys go to, in case it's the wrong one
		g_lastTarget = f.found;
		wchar_t title[256] = {};
		GetWindowTextW(f.found, title, 255);
		char t[512] = {}, c[256] = {};
		WideCharToMultiByte(CP_UTF8, 0, title, -1, t, sizeof(t) - 1, nullptr, nullptr);
		WideCharToMultiByte(CP_UTF8, 0, className(f.found).c_str(), -1, c, sizeof(c) - 1, nullptr, nullptr);
		obs_log(LOG_INFO, "slideshow window on the projector: \"%s\" (%s)", t, c);
	}
	if (!sendKeyTo(f.found, delta > 0 ? VK_RIGHT : delta < 0 ? VK_LEFT : VK_ESCAPE))
		obs_log(LOG_INFO, "Windows didn't let the slideshow come to the front, so the key was sent in the background");
	return "";
}

/* -------------------------------------------------------------- which window stays in front */
// Windows this plug-in made "always on top", so only those are ever put back
std::vector<HWND> g_madeTop;

bool isTopmost(HWND w)
{
	return (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

void makeTop(HWND w, bool top)
{
	if (!w || !IsWindow(w))
		return;
	if (top) {
		if (!isTopmost(w))
			SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		if (std::find(g_madeTop.begin(), g_madeTop.end(), w) == g_madeTop.end())
			g_madeTop.push_back(w);
	} else {
		auto it = std::find(g_madeTop.begin(), g_madeTop.end(), w);
		if (it == g_madeTop.end())
			return; // never touch a window this plug-in didn't raise
		g_madeTop.erase(it);
		if (isTopmost(w))
			SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}

/* -------------------------------------------------------------- PowerPoint, through COM */
HRESULT dispCall(IDispatch *d, const wchar_t *name, WORD flags, VARIANT *result, UINT argc, VARIANT *args)
{
	DISPID id;
	LPOLESTR n = (LPOLESTR)name;
	HRESULT hr = d->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id);
	if (FAILED(hr))
		return hr;
	DISPPARAMS dp{args, nullptr, argc, 0};
	if (result)
		VariantInit(result);
	return d->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &dp, result, nullptr, nullptr);
}

IDispatch *getObj(IDispatch *d, const wchar_t *name)
{
	if (!d)
		return nullptr;
	VARIANT v;
	if (FAILED(dispCall(d, name, DISPATCH_PROPERTYGET, &v, 0, nullptr)))
		return nullptr;
	if (v.vt != VT_DISPATCH || !v.pdispVal) {
		VariantClear(&v);
		return nullptr;
	}
	return v.pdispVal; // the caller releases it
}

bool getNum(IDispatch *d, const wchar_t *name, double &out)
{
	if (!d)
		return false;
	VARIANT v;
	if (FAILED(dispCall(d, name, DISPATCH_PROPERTYGET, &v, 0, nullptr)))
		return false;
	VARIANT r;
	VariantInit(&r);
	const bool ok = SUCCEEDED(VariantChangeType(&r, &v, 0, VT_R8));
	if (ok)
		out = r.dblVal;
	VariantClear(&v);
	VariantClear(&r);
	return ok;
}

std::wstring getStr(IDispatch *d, const wchar_t *name)
{
	if (!d)
		return L"";
	VARIANT v;
	if (FAILED(dispCall(d, name, DISPATCH_PROPERTYGET, &v, 0, nullptr)))
		return L"";
	std::wstring out = (v.vt == VT_BSTR && v.bstrVal) ? v.bstrVal : L"";
	VariantClear(&v);
	return out;
}

IDispatch *item(IDispatch *coll, long i)
{
	if (!coll)
		return nullptr;
	VARIANT a;
	VariantInit(&a);
	a.vt = VT_I4;
	a.lVal = i;
	VARIANT r;
	if (FAILED(dispCall(coll, L"Item", DISPATCH_METHOD | DISPATCH_PROPERTYGET, &r, 1, &a)))
		return nullptr;
	if (r.vt != VT_DISPATCH || !r.pdispVal) {
		VariantClear(&r);
		return nullptr;
	}
	return r.pdispVal;
}

void rel(IDispatch *&d)
{
	if (d) {
		d->Release();
		d = nullptr;
	}
}

IDispatch *g_ppt = nullptr;

IDispatch *powerPoint()
{
	if (g_ppt)
		return g_ppt;
	CLSID clsid;
	if (FAILED(CLSIDFromProgID(L"PowerPoint.Application", &clsid)))
		return nullptr;
	IUnknown *unk = nullptr;
	if (FAILED(GetActiveObject(clsid, nullptr, &unk)) || !unk)
		return nullptr;
	unk->QueryInterface(IID_IDispatch, (void **)&g_ppt);
	unk->Release();
	return g_ppt;
}

struct Show {
	IDispatch *view = nullptr, *pres = nullptr, *slides = nullptr;
	long n = 0, total = 0;
	double w = 4, h = 3;
	std::wstring file;
	bool ok = false;
	void release()
	{
		rel(view);
		rel(pres);
		rel(slides);
	}
};

// The running slide show, if PowerPoint has one
Show readShow()
{
	Show s;
	IDispatch *app = powerPoint();
	if (!app)
		return s;
	IDispatch *windows = getObj(app, L"SlideShowWindows");
	if (!windows) {
		rel(g_ppt); // PowerPoint closed: look for it again next time
		return s;
	}
	double count = 0;
	getNum(windows, L"Count", count);
	if (count >= 1) {
		IDispatch *win = item(windows, 1);
		s.view = getObj(win, L"View");
		s.pres = getObj(win, L"Presentation");
		rel(win);
		s.slides = getObj(s.pres, L"Slides");
		double n = 0, total = 0;
		getNum(s.view, L"CurrentShowPosition", n);
		getNum(s.slides, L"Count", total);
		IDispatch *setup = getObj(s.pres, L"PageSetup");
		getNum(setup, L"SlideWidth", s.w);
		getNum(setup, L"SlideHeight", s.h);
		rel(setup);
		s.file = getStr(s.pres, L"FullName");
		s.n = (long)n;
		s.total = (long)total;
		s.ok = s.view && s.slides && s.total > 0;
	}
	rel(windows);
	if (!s.ok)
		s.release();
	return s;
}

std::vector<unsigned char> exportSlide(IDispatch *slides, long index, double w, double h, const wchar_t *name)
{
	std::vector<unsigned char> out;
	IDispatch *slide = item(slides, index);
	if (!slide)
		return out;
	wchar_t dir[MAX_PATH];
	GetTempPathW(MAX_PATH, dir);
	std::wstring path = std::wstring(dir) + name;
	const long width = 960, height = (long)(960.0 * h / (w > 0 ? w : 4));
	VARIANT args[4];
	for (auto &a : args)
		VariantInit(&a);
	// Export(FileName, FilterName, ScaleWidth, ScaleHeight): arguments go in reverse order
	args[3].vt = VT_BSTR;
	args[3].bstrVal = SysAllocString(path.c_str());
	args[2].vt = VT_BSTR;
	args[2].bstrVal = SysAllocString(L"PNG");
	args[1].vt = VT_I4;
	args[1].lVal = width;
	args[0].vt = VT_I4;
	args[0].lVal = height;
	if (SUCCEEDED(dispCall(slide, L"Export", DISPATCH_METHOD, nullptr, 4, args))) {
		std::ifstream f(path, std::ios::binary);
		out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		f.close();
		DeleteFileW(path.c_str());
	}
	SysFreeString(args[3].bstrVal);
	SysFreeString(args[2].bstrVal);
	rel(slide);
	return out;
}

void worker()
{
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	long lastN = -1, lastTotal = -1;
	std::wstring lastFile;
	while (g_run) {
		std::deque<Cmd> cmds;
		{
			std::unique_lock<std::mutex> lock(g_qm);
			g_qcv.wait_for(lock, std::chrono::milliseconds(700), []() { return !g_queue.empty() || !g_run; });
			cmds.swap(g_queue);
		}
		if (!g_run) {
			for (auto &c : cmds)
				c.done->set_value("OBS is closing.");
			break;
		}
		Show s = readShow();
		std::vector<std::pair<std::shared_ptr<std::promise<std::string>>, std::string>> results;
		for (auto &c : cmds) {
			std::string err;
			if (s.ok && c.delta == 0) {
				if (FAILED(dispCall(s.view, L"Exit", DISPATCH_METHOD, nullptr, 0, nullptr)))
					err = "PowerPoint didn't end the slide show.";
			} else if (s.ok) {
				if (FAILED(dispCall(s.view, c.delta > 0 ? L"Next" : L"Previous", DISPATCH_METHOD, nullptr, 0,
						    nullptr)))
					err = "PowerPoint didn't change the slide.";
			} else {
				err = sendArrow(c.delta);
			}
			results.push_back({c.done, err});
		}
		// The phone is answered once the new slide's picture is ready, so it shows it straight away
		bool answered = false;
		auto answer = [&]() {
			if (answered)
				return;
			answered = true;
			for (auto &r : results)
				r.first->set_value(r.second);
		};
		if (!cmds.empty() && s.ok) {
			s.release();
			s = readShow();
		}
		if (s.ok) {
			const long n = std::max(1L, std::min(s.n, s.total));
			if (n != lastN || s.total != lastTotal || s.file != lastFile) {
				auto cur = exportSlide(s.slides, n, s.w, s.h, L"bgh-live-front-cur.png");
				{
					std::lock_guard<std::mutex> lock(g_m);
					g_cur.swap(cur);
					g_next.clear();
					g_app = "PowerPoint";
					g_n = s.n > s.total ? s.total : s.n;
					g_total = s.total;
					g_ratio = ratioFor(s.w, s.h);
					g_version++;
				}
				answer();
				auto nxt = n < s.total ? exportSlide(s.slides, n + 1, s.w, s.h, L"bgh-live-front-next.png")
						       : std::vector<unsigned char>();
				lastN = n;
				lastTotal = s.total;
				lastFile = s.file;
				std::lock_guard<std::mutex> lock(g_m);
				g_next.swap(nxt);
				g_version++;
			}
			std::lock_guard<std::mutex> lock(g_m);
			g_app = "PowerPoint";
			g_n = s.n > s.total ? s.total : s.n;
			g_total = s.total;
			g_ratio = ratioFor(s.w, s.h);
		} else {
			lastN = lastTotal = -1;
			lastFile.clear();
			std::lock_guard<std::mutex> lock(g_m);
			if (g_app == "PowerPoint") {
				g_app = "";
				g_cur.clear();
				g_next.clear();
				g_version++;
			}
		}
		answer();
		s.release();
	}
	rel(g_ppt);
	CoUninitialize();
}
#else
std::string sendArrow(int)
{
	return "Changing slides works on Windows only.";
}
void worker()
{
	while (g_run) {
		std::deque<Cmd> cmds;
		{
			std::unique_lock<std::mutex> lock(g_qm);
			g_qcv.wait_for(lock, std::chrono::milliseconds(700), []() { return !g_queue.empty() || !g_run; });
			cmds.swap(g_queue);
		}
		for (auto &c : cmds)
			c.done->set_value(sendArrow(c.delta));
	}
}
#endif

} // namespace

void slidesStart()
{
	if (g_run)
		return;
	g_run = true;
	g_worker = std::thread(worker);
}

void slidesStop()
{
	if (!g_run)
		return;
	g_run = false;
	g_qcv.notify_all();
	if (g_worker.joinable())
		g_worker.join();
}

std::string slidesStep(int delta)
{
	if (!g_run)
		return "OBS is closing.";
	auto done = std::make_shared<std::promise<std::string>>();
	auto fut = done->get_future();
	{
		std::lock_guard<std::mutex> lock(g_qm);
		g_queue.push_back({delta, done});
	}
	g_qcv.notify_all();
	if (fut.wait_for(std::chrono::seconds(4)) != std::future_status::ready)
		return "The slideshow didn't answer. Check the laptop.";
	return fut.get();
}

std::string slidesJson()
{
	std::lock_guard<std::mutex> lock(g_m);
	const bool pp = g_app == "PowerPoint";
	return std::string("{\"app\":\"") + g_app + "\",\"n\":" + std::to_string(pp ? g_n : 0) +
	       ",\"total\":" + std::to_string(pp ? g_total : 0) + ",\"ratio\":\"" + g_ratio + "\",\"v\":" +
	       std::to_string(g_version) + ",\"img\":" + (g_cur.empty() ? "false" : "true") +
	       ",\"next\":" + (g_next.empty() ? "false" : "true") + "}";
}

std::vector<unsigned char> slideImage(const std::string &which)
{
	std::lock_guard<std::mutex> lock(g_m);
	return which == "next" ? g_next : g_cur;
}

// UI thread, once a second. When PowerPoint isn't running the show and a Presenter phone is watching,
// grab the projector screen so the phone shows what the audience sees.
#ifdef _WIN32
// A presentation, as opposed to an ordinary window: PowerPoint's slide show, or anything in true full screen
// (Keynote or Google Slides presenting in a browser), which covers the whole screen with no title bar.
// A maximized browser window still has its title bar, so it isn't one.
bool isPresentation(HWND w, const RECT &mon)
{
	if (!w)
		return false;
	if (className(w) == L"screenClass")
		return true; // PowerPoint's slide show
	const LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
	if ((style & WS_CAPTION) == WS_CAPTION)
		return false;
	RECT r;
	return GetWindowRect(w, &r) && r.left <= mon.left && r.top <= mon.top && r.right >= mon.right &&
	       r.bottom >= mon.bottom;
}

// True if a is in front of b
bool inFront(HWND a, HWND b)
{
	for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
		if (w == a)
			return true;
		if (w == b)
			return false;
	}
	return false;
}
#endif

std::atomic<bool> g_presInFront{false};

bool presentationInFront()
{
	return g_presInFront;
}

void applyOnTop(bool restore)
{
#ifdef _WIN32
	g_madeTop.erase(std::remove_if(g_madeTop.begin(), g_madeTop.end(), [](HWND w) { return !IsWindow(w); }),
			g_madeTop.end());
	if (restore) {
		for (HWND w : std::vector<HWND>(g_madeTop))
			makeTop(w, false);
		return;
	}
	HWND pres = nullptr;
	RECT monRect{};
	if (HMONITOR mon = projectorMonitor()) {
		MONITORINFO mi{};
		mi.cbSize = sizeof(mi);
		GetMonitorInfoW(mon, &mi);
		monRect = mi.rcMonitor;
		WinFind f{mon, mi.rcMonitor, GetCurrentProcessId()};
		EnumWindows(winEnum, (LPARAM)&f);
		pres = f.found;
	}
	std::vector<HWND> obs;
	for (void *h : obsProjectorHandles())
		obs.push_back((HWND)h);
	const std::string mode = onTop();
	// auto:         presentations on top, OBS's projector second, everything else underneath
	// presentation: whatever is on the projector stays in front of OBS
	// obs:          OBS's projector covers everything
	const bool presOnTop = !obs.empty() && pres &&
			       (mode == "presentation" || (mode == "auto" && isPresentation(pres, monRect)));
	const bool obsOnTop = mode != "presentation";
	// Anything raised before that isn't wanted in front any more goes back to normal
	for (HWND w : std::vector<HWND>(g_madeTop)) {
		const bool isObs = std::find(obs.begin(), obs.end(), w) != obs.end();
		const bool keep = (isObs && obsOnTop) || (presOnTop && w == pres);
		if (!keep)
			makeTop(w, false);
	}
	if (obsOnTop)
		for (HWND w : obs)
			makeTop(w, true);
	if (presOnTop) {
		makeTop(pres, true);
		// Both stay above ordinary windows; the presentation goes above OBS's projector
		for (HWND w : obs)
			if (inFront(w, pres)) {
				SetWindowPos(pres, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
				break;
			}
	} else if (obsOnTop && pres) {
		// OBS's projector goes above the presentation. PowerPoint keeps its slide show "always on top" by itself,
		// so being topmost isn't enough: OBS's projector is put back above it whenever it isn't
		for (HWND w : obs)
			if (inFront(pres, w))
				SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	// What phones are told is what's really in front, not just what was asked for
	bool presReallyFront = false;
	for (HWND w : obs)
		if (pres && inFront(pres, w))
			presReallyFront = true;
	g_presInFront = presReallyFront;
#else
	(void)restore;
#endif
}

std::string slidesApp()
{
	std::lock_guard<std::mutex> lock(g_m);
	return g_app;
}

void slidesTick()
{
	slidesCapture(false);
}

// Grabs the projector screen (UI thread). force: straight after Next or Back, even if no Presenter phone is
// counted yet, so the phone sees the new slide at once instead of on the next once-a-second check.
void slidesCapture(bool force)
{
	{
		std::lock_guard<std::mutex> lock(g_m);
		if (g_app == "PowerPoint")
			return;
	}
	QScreen *pj = projectorScreen();
	if (!pj) {
		std::lock_guard<std::mutex> lock(g_m);
		if (!g_app.empty() || !g_cur.empty()) {
			g_app.clear();
			g_cur.clear();
			g_next.clear();
			g_screenHash = 0;
			g_version++;
		}
		return;
	}
	if (!force && presenterViewers() == 0) {
		std::lock_guard<std::mutex> lock(g_m);
		g_app = "screen";
		return;
	}
	QImage img = pj->grabWindow(0).toImage();
	if (img.isNull())
		return;
	img = img.convertToFormat(QImage::Format_RGB32);
	// A 16:9 slideshow on the 4:3 projector has black bars above and below: leave them out
	auto darkRow = [&](int y) {
		const QRgb *line = (const QRgb *)img.constScanLine(y);
		for (int x = 0; x < img.width(); x += 8)
			if (qGray(line[x]) > 18)
				return false;
		return true;
	};
	int top = 0, bottom = img.height() - 1;
	while (top < bottom && darkRow(top))
		top++;
	while (bottom > top && darkRow(bottom))
		bottom--;
	if (bottom - top > img.height() / 3 && (top > 4 || bottom < img.height() - 5))
		img = img.copy(0, top, img.width(), bottom - top + 1);
	const std::string ratio = ratioFor(img.width(), img.height());
	img = img.scaledToWidth(720, Qt::SmoothTransformation);
	QByteArray bytes;
	QBuffer buf(&bytes);
	buf.open(QIODevice::WriteOnly);
	img.save(&buf, "PNG");
	const size_t hash = qHash(bytes);
	std::lock_guard<std::mutex> lock(g_m);
	g_app = "screen";
	g_ratio = ratio;
	if (hash != g_screenHash) {
		g_screenHash = hash;
		g_cur.assign(bytes.begin(), bytes.end());
		g_next.clear();
		g_version++;
	}
}

} // namespace front
