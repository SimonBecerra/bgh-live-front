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

BOOL CALLBACK winEnum(HWND w, LPARAM p)
{
	auto *f = (WinFind *)p;
	if (!IsWindowVisible(w) || IsIconic(w))
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

void sendKeyTo(HWND target, WORD vk, bool background)
{
	if (background) {
		// Without bringing it to the front (an OBS projector stays on top): post the key to the window
		HWND to = FindWindowExW(target, nullptr, L"Chrome_RenderWidgetHostHWND", nullptr);
		if (!to) {
			HWND child = FindWindowExW(target, nullptr, L"Chrome_WidgetWin_0", nullptr);
			if (child)
				to = FindWindowExW(child, nullptr, L"Chrome_RenderWidgetHostHWND", nullptr);
		}
		if (!to)
			to = target;
		const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
		const LPARAM down = (LPARAM)1 | ((LPARAM)scan << 16) | ((LPARAM)1 << 24);
		const LPARAM up = down | ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
		PostMessageW(to, WM_KEYDOWN, vk, down);
		PostMessageW(to, WM_KEYUP, vk, up);
		return;
	}
	// Bring the slideshow window to the front for a moment, press the key, then give the focus back
	HWND before = GetForegroundWindow();
	const DWORD me = GetCurrentThreadId();
	const DWORD fg = before ? GetWindowThreadProcessId(before, nullptr) : 0;
	if (fg && fg != me)
		AttachThreadInput(me, fg, TRUE);
	SetForegroundWindow(target);
	if (fg && fg != me)
		AttachThreadInput(me, fg, FALSE);
	Sleep(40);
	INPUT in[2] = {};
	in[0].type = INPUT_KEYBOARD;
	in[0].ki.wVk = vk;
	in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
	in[1] = in[0];
	in[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
	SendInput(2, in, sizeof(INPUT));
	Sleep(40);
	if (before && before != target)
		SetForegroundWindow(before);
}

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
		return "There's no slideshow showing on the projector.";
	sendKeyTo(f.found, delta > 0 ? VK_RIGHT : VK_LEFT, f.coveredByObs);
	return "";
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
		for (auto &c : cmds) {
			std::string err;
			if (s.ok) {
				if (FAILED(dispCall(s.view, c.delta > 0 ? L"Next" : L"Previous", DISPATCH_METHOD, nullptr, 0,
						  nullptr)))
					err = "PowerPoint didn't change the slide.";
			} else {
				err = sendArrow(c.delta);
			}
			c.done->set_value(err);
		}
		if (!cmds.empty() && s.ok) {
			s.release();
			s = readShow();
		}
		if (s.ok) {
			const long n = std::max(1L, std::min(s.n, s.total));
			if (n != lastN || s.total != lastTotal || s.file != lastFile) {
				auto cur = exportSlide(s.slides, n, s.w, s.h, L"bgh-live-front-cur.png");
				auto nxt = n < s.total ? exportSlide(s.slides, n + 1, s.w, s.h, L"bgh-live-front-next.png")
						       : std::vector<unsigned char>();
				lastN = n;
				lastTotal = s.total;
				lastFile = s.file;
				std::lock_guard<std::mutex> lock(g_m);
				g_cur.swap(cur);
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
void slidesTick()
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
	if (presenterViewers() == 0) {
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
