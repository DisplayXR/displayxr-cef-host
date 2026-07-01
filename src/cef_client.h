// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  CefClient for the CEF OSR weave host (#625, Step A).
 *
 * One client wires four things together:
 *   - CefLifeSpanHandler  — owns the browser handle, tracks close.
 *   - CefRenderHandler    — OSR: GetViewRect + OnAcceleratedPaint (the zero-copy
 *                            shared-texture handoff of the composited page).
 *   - CefMessageRouterBrowserSide + Handler — the BROWSER side of the page<->host
 *     bridge (rect-up, eyes-subscription) over `window.cefQuery`.
 *
 * All callbacks run on the host's single thread (it pumps CefDoMessageLoopWork
 * inline), so the shared HostBridge needs no locking.
 */

#pragma once

#include "include/cef_client.h"
#include "include/wrapper/cef_message_router.h"
#include "host_bridge.h"

class CefHostClient : public CefClient,
                      public CefLifeSpanHandler,
                      public CefRenderHandler,
                      public CefMessageRouterBrowserSide::Handler {
 public:
	explicit CefHostClient(HostBridge *bridge);

	// Window client size in device px (used by GetViewRect). Set by main on
	// create + resize; main also calls browser->GetHost()->WasResized().
	void SetViewSize(uint32_t w, uint32_t h) { view_w_ = w; view_h_ = h; }

	CefRefPtr<CefBrowser> GetBrowser() const { return browser_; }

	// CefClient:
	CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
	CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
	bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
	                              CefRefPtr<CefFrame> frame,
	                              CefProcessId source_process,
	                              CefRefPtr<CefProcessMessage> message) override;

	// CefLifeSpanHandler:
	void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
	void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

	// CefRenderHandler:
	void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect) override;
	void OnPaint(CefRefPtr<CefBrowser> browser,
	             PaintElementType type,
	             const RectList &dirtyRects,
	             const void *buffer,
	             int width,
	             int height) override;
	void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser,
	                        PaintElementType type,
	                        const RectList &dirtyRects,
	                        const CefAcceleratedPaintInfo &info) override;

	// CefMessageRouterBrowserSide::Handler:
	bool OnQuery(CefRefPtr<CefBrowser> browser,
	             CefRefPtr<CefFrame> frame,
	             int64_t query_id,
	             const CefString &request,
	             bool persistent,
	             CefRefPtr<Callback> callback) override;

 private:
	HostBridge *bridge_; //!< not owned; outlives the client
	CefRefPtr<CefBrowser> browser_;
	CefRefPtr<CefMessageRouterBrowserSide> browser_router_;
	uint32_t view_w_ = 1280;
	uint32_t view_h_ = 720;
	bool warned_no_accel_ = false;

	IMPLEMENT_REFCOUNTING(CefHostClient);
	DISALLOW_COPY_AND_ASSIGN(CefHostClient);
};

//! Deliver the latest weave-returned eyes (in `bridge`) to the page through the
//! persistent eyes-subscription callback, if the page has registered one. Safe
//! to call every frame; a no-op until the page subscribes. Call only on the
//! host/CEF thread.
void DeliverEyesToPage(HostBridge &bridge);
