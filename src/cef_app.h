// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  CefApp for the CEF OSR weave host (#625, Step A).
 *
 * One CefApp instance is used for every process (browser + the render/GPU/etc.
 * sub-processes that share this exe via CefExecuteProcess). It provides:
 *   - the browser-process handler (command-line tweaks needed for accelerated
 *     OSR shared textures), and
 *   - the render-process handler that owns the RENDERER side of the
 *     CefMessageRouter (the JS `window.cefQuery` <-> native bridge).
 * The BROWSER side of the router lives in CefHostClient (cef_client.h).
 */

#pragma once

#include "include/cef_app.h"
#include "include/wrapper/cef_message_router.h"

class CefHostApp : public CefApp,
                   public CefBrowserProcessHandler,
                   public CefRenderProcessHandler {
 public:
	CefHostApp() = default;

	// CefApp:
	CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
	CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }
	void OnBeforeCommandLineProcessing(const CefString &process_type,
	                                   CefRefPtr<CefCommandLine> command_line) override;

	// CefRenderProcessHandler (drives the renderer-side message router):
	void OnWebKitInitialized() override;
	void OnContextCreated(CefRefPtr<CefBrowser> browser,
	                      CefRefPtr<CefFrame> frame,
	                      CefRefPtr<CefV8Context> context) override;
	void OnContextReleased(CefRefPtr<CefBrowser> browser,
	                       CefRefPtr<CefFrame> frame,
	                       CefRefPtr<CefV8Context> context) override;
	bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
	                              CefRefPtr<CefFrame> frame,
	                              CefProcessId source_process,
	                              CefRefPtr<CefProcessMessage> message) override;

 private:
	CefRefPtr<CefMessageRouterRendererSide> render_router_;

	IMPLEMENT_REFCOUNTING(CefHostApp);
	DISALLOW_COPY_AND_ASSIGN(CefHostApp);
};
