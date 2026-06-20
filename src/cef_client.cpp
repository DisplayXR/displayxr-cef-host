// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CefClient implementation for the CEF OSR weave host (#625, Step A).
 */

#include "cef_client.h"
#include "logging.h"

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

CefHostClient::CefHostClient(HostBridge *bridge) : bridge_(bridge)
{
	CefMessageRouterConfig config; // default cefQuery / cefQueryCancel
	browser_router_ = CefMessageRouterBrowserSide::Create(config);
	browser_router_->AddHandler(this, /*first*/ true);
}

// ---- CefClient --------------------------------------------------------------
bool
CefHostClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        CefProcessId source_process,
                                        CefRefPtr<CefProcessMessage> message)
{
	return browser_router_->OnProcessMessageReceived(browser, frame, source_process, message);
}

// ---- CefLifeSpanHandler -----------------------------------------------------
void
CefHostClient::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
	browser_ = browser;
	LOG_INFO("CEF browser created (windowless OSR)");
}

void
CefHostClient::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
	browser_router_->OnBeforeClose(browser);
	bridge_->eyesCallback = nullptr;
	bridge_->browserClosed = true;
	browser_ = nullptr;
	LOG_INFO("CEF browser closed");
}

// ---- CefRenderHandler -------------------------------------------------------
void
CefHostClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect)
{
	rect.x = 0;
	rect.y = 0;
	rect.width = (int)(view_w_ > 0 ? view_w_ : 1);
	rect.height = (int)(view_h_ > 0 ? view_h_ : 1);
}

void
CefHostClient::OnPaint(CefRefPtr<CefBrowser> browser,
                       PaintElementType type,
                       const RectList &dirtyRects,
                       const void *buffer,
                       int width,
                       int height)
{
	// CPU paint path. We only reach here if accelerated (GPU) OSR is unavailable
	// on this machine — which means no zero-copy shared texture. Warn once; the
	// weave path needs the accelerated path.
	if (!warned_no_accel_) {
		warned_no_accel_ = true;
		LOG_ERROR("OnPaint (CPU) fired — accelerated OSR shared texture unavailable. "
		          "GPU compositing may be disabled; the weave round-trip needs OnAcceleratedPaint.");
	}
}

void
CefHostClient::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser,
                                  PaintElementType type,
                                  const RectList &dirtyRects,
                                  const CefAcceleratedPaintInfo &info)
{
	if (type != PET_VIEW || !bridge_->device || !info.shared_texture_handle) {
		return;
	}
	HANDLE h = (HANDLE)info.shared_texture_handle;

	// CEF's accelerated-paint texture is a shared D3D11 texture WITHOUT a keyed
	// mutex; the handle is pool-allocated and reclaimed when this callback
	// returns, so it must be reopened + copied here every frame. Chromium shares
	// via NT handles → OpenSharedResource1; fall back to the legacy global path.
	ComPtr<ID3D11Texture2D> src;
	HRESULT hr = bridge_->device->OpenSharedResource1(h, IID_PPV_ARGS(&src));
	if (FAILED(hr)) {
		hr = bridge_->device->OpenSharedResource(h, IID_PPV_ARGS(&src));
		if (FAILED(hr)) {
			if (!warned_no_accel_) {
				warned_no_accel_ = true;
				LOG_ERROR("OpenSharedResource(accelerated paint) failed: 0x%08lx", hr);
			}
			return;
		}
	}

	D3D11_TEXTURE2D_DESC sd = {};
	src->GetDesc(&sd);

	// (Re)create the host-owned page copy when size/format changes. It is a plain
	// shader-resource texture on our device (the composite samples it).
	if (!bridge_->pageTex || bridge_->pageW != sd.Width || bridge_->pageH != sd.Height) {
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = sd.Width;
		td.Height = sd.Height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = sd.Format; // CEF gives BGRA8 (DXGI_FORMAT_B8G8R8A8_UNORM)
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ComPtr<ID3D11Texture2D> tex;
		if (FAILED(bridge_->device->CreateTexture2D(&td, nullptr, &tex))) {
			LOG_ERROR("page copy CreateTexture2D failed (%ux%u)", sd.Width, sd.Height);
			return;
		}
		bridge_->pageTex = tex;
		bridge_->pageW = sd.Width;
		bridge_->pageH = sd.Height;
	}

	bridge_->context->CopyResource(bridge_->pageTex.Get(), src.Get());
	bridge_->context->Flush(); // ensure the copy is submitted before we read it
	bridge_->pageFrame++;
}

// ---- CefMessageRouterBrowserSide::Handler -----------------------------------
bool
CefHostClient::OnQuery(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       int64_t query_id,
                       const CefString &request,
                       bool persistent,
                       CefRefPtr<Callback> callback)
{
	const std::string req = request.ToString();

	// Eyes-down subscription: a single persistent query the page registers once.
	// We keep the callback and push eyes to it every weave frame (DeliverEyesToPage).
	if (req == "subscribe-eyes") {
		bridge_->eyesCallback = callback;
		LOG_INFO("page subscribed to eyes");
		return true; // keep the persistent query open; do NOT Success() now
	}

	// Rect-up: "rect <x> <y> <w> <h>" in device px, window-relative, y-down.
	if (req.rfind("rect ", 0) == 0) {
		int x = 0, y = 0, w = 0, hgt = 0;
		if (sscanf_s(req.c_str(), "rect %d %d %d %d", &x, &y, &w, &hgt) == 4 && w > 0 && hgt > 0) {
			bridge_->rectX = x;
			bridge_->rectY = y;
			bridge_->rectW = (uint32_t)w;
			bridge_->rectH = (uint32_t)hgt;
			bridge_->haveRect = true;
			bridge_->rectSeq++;
		}
		callback->Success("ok");
		return true;
	}

	return false; // not handled
}

// ---- eyes-down delivery -----------------------------------------------------
void
DeliverEyesToPage(HostBridge &bridge)
{
	if (!bridge.eyesCallback) {
		return;
	}
	// Compact JSON the page parses with JSON.parse: eyes in display-space metres.
	char buf[512];
	int n = snprintf(buf, sizeof(buf), "{\"valid\":%s,\"tracking\":%s,\"count\":%u,\"eyes\":[",
	                 bridge.eyesValid ? "true" : "false", bridge.eyesTracking ? "true" : "false",
	                 bridge.eyeCount);
	uint32_t count = bridge.eyeCount > kHostMaxEyes ? kHostMaxEyes : bridge.eyeCount;
	for (uint32_t i = 0; i < count && n > 0 && n < (int)sizeof(buf); i++) {
		n += snprintf(buf + n, sizeof(buf) - n, "%s{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f}", i ? "," : "",
		              bridge.eyes[i * 3 + 0], bridge.eyes[i * 3 + 1], bridge.eyes[i * 3 + 2]);
	}
	if (n > 0 && n < (int)sizeof(buf)) {
		n += snprintf(buf + n, sizeof(buf) - n, "]}");
	}
	bridge.eyesCallback->Success(CefString(buf));
}
