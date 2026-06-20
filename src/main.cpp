// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CEF OSR weave host (#625, Step A) — a browser stand-in that drives
 *         the REAL display-processor weave through the shipped XR_EXT_weave RPC.
 *
 * A present-owner, modelled on the runtime's weave_rpc_probe harness, with a
 * real Chromium engine (CEF, offscreen-render) as the content source instead of
 * a synthetic side-by-side painter:
 *
 *   1. Own OS window + a transparent DirectComposition swap chain.
 *   2. Forced-IPC OpenXR session bound to that window (XR_EXT_win32_window_binding,
 *      transparent, no shared texture). xrWeaveBindWindowEXT(window) once.
 *   3. CEF renders the page offscreen; OnAcceleratedPaint hands a shared D3D11
 *      texture of the composited page (zero-copy, public CEF API).
 *   4. Per frame: extract the page's 3D-element sub-rect (its pre-weave SBS pair),
 *      xrWeaveSubmitEXT(sbs, windowRelativeRect) -> weaved texture + fence + the
 *      DP's tracked eyes; composite the weaved sub-rect back over the element rect
 *      and present. The eyes flow back to the page so it re-renders off-axis.
 *
 * The host NEVER weaves — the DP does, inside the runtime (ADR-007 / ADR-019).
 *
 * Run forced-IPC: set XRT_FORCE_MODE=ipc process-level, start displayxr-service,
 * then launch this exe (at the same integrity as the non-elevated service).
 * Autonomous capture: touch %TEMP%\weave_host_trigger -> %TEMP%\weave_host_output.bmp.
 */

#include "xr_session.h"
#include "weave_compositor.h"
#include "host_bridge.h"
#include "cef_app.h"
#include "cef_client.h"
#include "logging.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"

#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;

// ---- Layout constants -------------------------------------------------------
static const uint32_t kWinW = 1280;
static const uint32_t kWinH = 800;

// ---- Global window / D3D state ----------------------------------------------
static HWND g_hwnd = nullptr;
static bool g_quit = false;
static bool g_resized = false;
static uint32_t g_clientW = kWinW, g_clientH = kWinH;

static ComPtr<ID3D11Device5> g_device;
static ComPtr<ID3D11DeviceContext4> g_context;
static ComPtr<IDCompositionDevice> g_dcompDevice;
static ComPtr<IDCompositionTarget> g_dcompTarget;
static ComPtr<IDCompositionVisual> g_dcompVisual;
static ComPtr<IDXGISwapChain1> g_swapChain;

// ---- Win32 window -----------------------------------------------------------
static LRESULT CALLBACK
WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_CLOSE: g_quit = true; return 0;
	case WM_DESTROY: PostQuitMessage(0); return 0;
	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED) {
			g_resized = true;
		}
		return 0;
	case WM_KEYDOWN:
		if (wParam == VK_ESCAPE) {
			g_quit = true;
		}
		return 0;
	default: return DefWindowProc(hwnd, msg, wParam, lParam);
	}
}

static bool
CreateAppWindow(HINSTANCE hInst)
{
	WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = L"DXRCefWeaveHost";
	RegisterClassExW(&wc);

	RECT r = {0, 0, (LONG)kWinW, (LONG)kWinH};
	AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_NOREDIRECTIONBITMAP);
	// WS_EX_NOREDIRECTIONBITMAP: required for a DComp-presented (alpha) window.
	g_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"DisplayXR CEF Weave Host (#625)",
	                         WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
	                         nullptr, nullptr, hInst, nullptr);
	if (!g_hwnd) {
		LOG_ERROR("CreateWindowEx failed: %lu", GetLastError());
		return false;
	}
	RECT rc = {};
	GetClientRect(g_hwnd, &rc);
	g_clientW = (uint32_t)(rc.right - rc.left);
	g_clientH = (uint32_t)(rc.bottom - rc.top);
	ShowWindow(g_hwnd, SW_SHOW);
	return true;
}

// ---- D3D11 device on the OpenXR-required adapter -----------------------------
static bool
CreateDeviceOnAdapter(LUID luid)
{
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		LOG_ERROR("CreateDXGIFactory1 failed");
		return false;
	}
	ComPtr<IDXGIAdapter1> adapter, chosen;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 d = {};
		adapter->GetDesc1(&d);
		if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
			chosen = adapter;
			break;
		}
	}
	ComPtr<ID3D11Device> dev;
	ComPtr<ID3D11DeviceContext> ctx;
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // DComp needs BGRA support
	HRESULT hr = D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
	                               flags, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
	if (FAILED(hr)) {
		LOG_ERROR("D3D11CreateDevice failed: 0x%08lx", hr);
		return false;
	}
	if (FAILED(dev.As(&g_device)) || FAILED(ctx.As(&g_context))) {
		LOG_ERROR("ID3D11Device5/Context4 not available");
		return false;
	}
	return true;
}

// ---- DirectComposition transparent swap chain (caller-owned present) ---------
static bool
CreateCompositionSwapChain(uint32_t w, uint32_t h)
{
	ComPtr<IDXGIDevice> dxgiDevice;
	g_device.As(&dxgiDevice);
	ComPtr<IDXGIAdapter> adapter;
	dxgiDevice->GetAdapter(&adapter);
	ComPtr<IDXGIFactory2> factory;
	adapter->GetParent(IID_PPV_ARGS(&factory));

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = w;
	sd.Height = h;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // match CEF's composited-page format
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
	HRESULT hr = factory->CreateSwapChainForComposition(g_device.Get(), &sd, nullptr, &g_swapChain);
	if (FAILED(hr)) {
		LOG_ERROR("CreateSwapChainForComposition failed: 0x%08lx", hr);
		return false;
	}
	if (!g_dcompDevice) {
		hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&g_dcompDevice));
		if (FAILED(hr)) {
			LOG_ERROR("DCompositionCreateDevice failed: 0x%08lx", hr);
			return false;
		}
		hr = g_dcompDevice->CreateTargetForHwnd(g_hwnd, TRUE, &g_dcompTarget);
		if (FAILED(hr)) {
			LOG_ERROR("CreateTargetForHwnd failed: 0x%08lx", hr);
			return false;
		}
		g_dcompDevice->CreateVisual(&g_dcompVisual);
	}
	g_dcompVisual->SetContent(g_swapChain.Get());
	g_dcompTarget->SetRoot(g_dcompVisual.Get());
	g_dcompDevice->Commit();
	return true;
}

// ---- Autonomous capture: dump the composited back buffer on file trigger -----
static void
MaybeDumpComposite()
{
	char trig[MAX_PATH], outp[MAX_PATH];
	const char *tmp = getenv("TEMP");
	if (!tmp) {
		tmp = "C:\\Temp";
	}
	snprintf(trig, sizeof(trig), "%s\\weave_host_trigger", tmp);
	if (GetFileAttributesA(trig) == INVALID_FILE_ATTRIBUTES || !g_swapChain) {
		return;
	}
	DeleteFileA(trig);
	snprintf(outp, sizeof(outp), "%s\\weave_host_output.bmp", tmp);

	ComPtr<ID3D11Texture2D> back;
	if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
		return;
	}
	D3D11_TEXTURE2D_DESC td = {};
	back->GetDesc(&td);
	D3D11_TEXTURE2D_DESC sd = td;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.MiscFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Texture2D> staging;
	if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &staging))) {
		return;
	}
	g_context->CopyResource(staging.Get(), back.Get());
	g_context->Flush();
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
		return;
	}
	const uint32_t w = td.Width, h = td.Height;
	const uint32_t rowBytes = w * 3;
	const uint32_t padded = (rowBytes + 3) & ~3u;
	const uint32_t imgSize = padded * h;
#pragma pack(push, 1)
	struct {
		uint16_t bfType;
		uint32_t bfSize;
		uint16_t r1, r2;
		uint32_t bfOff;
		uint32_t biSize;
		int32_t biW, biH;
		uint16_t biPlanes, biBpp;
		uint32_t biComp, biImg;
		int32_t biXppm, biYppm;
		uint32_t biClr, biImp;
	} hdr = {};
#pragma pack(pop)
	hdr.bfType = 0x4D42;
	hdr.bfOff = sizeof(hdr);
	hdr.bfSize = sizeof(hdr) + imgSize;
	hdr.biSize = 40;
	hdr.biW = (int32_t)w;
	hdr.biH = (int32_t)h; // bottom-up
	hdr.biPlanes = 1;
	hdr.biBpp = 24;
	hdr.biImg = imgSize;
	FILE *f = fopen(outp, "wb");
	if (f) {
		fwrite(&hdr, sizeof(hdr), 1, f);
		std::vector<uint8_t> row(padded, 0);
		for (int32_t y = (int32_t)h - 1; y >= 0; y--) {
			const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
			for (uint32_t x = 0; x < w; x++) {
				// back buffer is BGRA8 -> BGR bmp (already in order)
				row[x * 3 + 0] = src[x * 4 + 0];
				row[x * 3 + 1] = src[x * 4 + 1];
				row[x * 3 + 2] = src[x * 4 + 2];
			}
			fwrite(row.data(), padded, 1, f);
		}
		fclose(f);
		LOG_INFO("Dumped composited output to %s (%ux%u)", outp, w, h);
	}
	g_context->Unmap(staging.Get(), 0);
}

// ---- file:// URL of the demo page next to the exe ---------------------------
static std::string
DemoPageUrl()
{
	char exe[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	std::string p(exe);
	size_t slash = p.find_last_of("\\/");
	std::string dir = (slash == std::string::npos) ? "." : p.substr(0, slash);
	std::string path = dir + "\\web\\index.html";
	for (auto &c : path) {
		if (c == '\\') {
			c = '/';
		}
	}
	return std::string("file:///") + path;
}

// -----------------------------------------------------------------------------
int WINAPI
wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
	// CEF multi-process: this same exe is re-launched for render/GPU/etc. The
	// app must exist in every process so the render process gets the renderer
	// side of the message router.
	CefMainArgs main_args(hInst);
	CefRefPtr<CefHostApp> app(new CefHostApp);
	int exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
	if (exit_code >= 0) {
		return exit_code; // sub-process completed
	}

	InitializeLogging("cef_weave_host");
	LOG_INFO("=== CEF OSR weave host (#625) starting ===");

	CefSettings settings;
	settings.no_sandbox = true;
	settings.windowless_rendering_enabled = true;
	settings.multi_threaded_message_loop = false; // we pump CefDoMessageLoopWork inline
	settings.log_severity = LOGSEVERITY_WARNING;
	if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
		LOG_ERROR("CefInitialize failed: %d", CefGetExitCode());
		return CefGetExitCode();
	}

	// OpenXR + D3D bring-up (probe order: instance/system -> adapter -> window ->
	// device on that adapter -> transparent DComp present -> forced-IPC session).
	XrSessionManager xr;
	WeaveCompositor compositor;
	HostBridge bridge;
	CefRefPtr<CefHostClient> client;
	bool ok = false;
	do {
		if (!InitializeOpenXR(xr)) {
			break;
		}
		LUID luid = {};
		if (!GetD3D11GraphicsRequirements(xr, &luid)) {
			break;
		}
		if (!CreateAppWindow(hInst)) {
			break;
		}
		if (!CreateDeviceOnAdapter(luid)) {
			break;
		}
		if (!CreateCompositionSwapChain(g_clientW, g_clientH)) {
			break;
		}
		if (!CreateSession(xr, g_device.Get(), g_hwnd)) {
			break;
		}

		LOG_INFO("Waiting for session to start...");
		for (int i = 0; i < 2000 && !xr.sessionRunning && !g_quit; i++) {
			MSG msg;
			while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			PollEvents(xr);
			Sleep(2);
		}
		if (!xr.sessionRunning) {
			LOG_ERROR("Session never reached running state");
			break;
		}

		XrResult br = g_pfnWeaveBindWindow(xr.session, (void *)g_hwnd);
		LogXrResult("xrWeaveBindWindowEXT", br);
		if (XR_FAILED(br)) {
			break;
		}

		if (!compositor.Init(g_device.Get(), g_context.Get())) {
			break;
		}
		bridge.device = g_device;
		bridge.context = g_context;

		// Create the CEF browser windowless, accelerated (shared texture), bound
		// to our window for input/DPI; it renders the demo page offscreen.
		client = new CefHostClient(&bridge);
		client->SetViewSize(g_clientW, g_clientH);

		CefWindowInfo wi;
		wi.SetAsWindowless(g_hwnd);
		wi.shared_texture_enabled = true; // OnAcceleratedPaint (zero-copy GPU texture)
		wi.external_begin_frame_enabled = false;

		CefBrowserSettings bs;
		bs.windowless_frame_rate = 60;

		std::string url = DemoPageUrl();
		LOG_INFO("Loading demo page: %s", url.c_str());
		if (!CefBrowserHost::CreateBrowser(wi, client, url, bs, nullptr, nullptr)) {
			LOG_ERROR("CreateBrowser failed");
			break;
		}
		ok = true;
	} while (false);

	if (!ok) {
		CefShutdown();
		ShutdownLogging();
		return 1;
	}

	LOG_INFO("Entering weave loop (ESC / close to quit)...");
	double latSum = 0.0;
	uint32_t latCount = 0;
	uint64_t frame = 0;
	while (!g_quit && !xr.exitRequested && !bridge.browserClosed) {
		MSG msg;
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		PollEvents(xr);
		CefDoMessageLoopWork(); // drives CEF; OnAcceleratedPaint / OnQuery fire here

		if (g_resized) {
			g_resized = false;
			RECT rc = {};
			GetClientRect(g_hwnd, &rc);
			uint32_t cw = (uint32_t)(rc.right - rc.left), ch = (uint32_t)(rc.bottom - rc.top);
			if (cw > 0 && ch > 0 && (cw != g_clientW || ch != g_clientH)) {
				g_clientW = cw;
				g_clientH = ch;
				g_swapChain->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
				compositor.OnResize();
				client->SetViewSize(cw, ch);
				if (client->GetBrowser()) {
					client->GetBrowser()->GetHost()->WasResized();
				}
			}
		}

		double ms = 0.0;
		if (compositor.Frame(xr.session, g_swapChain.Get(), g_clientW, g_clientH, bridge, &ms)) {
			MaybeDumpComposite();
			g_swapChain->Present(1, 0);
			if (g_dcompDevice) {
				g_dcompDevice->Commit();
			}
			DeliverEyesToPage(bridge); // page renders next frame off-axis
			latSum += ms;
			latCount++;
			if ((frame % 120) == 0 && latCount > 0) {
				LOG_INFO("weave round-trip: last=%.3f ms avg=%.3f ms (%u) rect=%d,%d %ux%u "
				         "eyes(valid=%d track=%d n=%u L.x=%.4f R.x=%.4f) pageFrame=%llu",
				         ms, latSum / latCount, latCount, bridge.rectX, bridge.rectY, bridge.rectW, bridge.rectH,
				         bridge.eyesValid, bridge.eyesTracking, bridge.eyeCount, bridge.eyes[0],
				         bridge.eyeCount >= 2 ? bridge.eyes[3] : 0.0f, (unsigned long long)bridge.pageFrame);
			}
			frame++;
		} else {
			Sleep(2); // no page texture / rect yet — don't busy-spin
		}
	}

	// Teardown: close the browser, pump until it's gone, then shut CEF + XR down.
	if (client && client->GetBrowser()) {
		client->GetBrowser()->GetHost()->CloseBrowser(true);
		for (int i = 0; i < 200 && !bridge.browserClosed; i++) {
			CefDoMessageLoopWork();
			Sleep(5);
		}
	}
	if (latCount > 0) {
		LOG_INFO("=== weave host done: %u frames, avg round-trip %.3f ms ===", latCount, latSum / latCount);
	}
	CleanupOpenXR(xr);
	CefShutdown();
	ShutdownLogging();
	return 0;
}
