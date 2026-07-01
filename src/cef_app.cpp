// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  CefApp implementation for the CEF OSR weave host (#625, Step A).
 */

#include "cef_app.h"

void
CefHostApp::OnBeforeCommandLineProcessing(const CefString &process_type, CefRefPtr<CefCommandLine> command_line)
{
	// Accelerated (GPU shared-texture) offscreen rendering requires GPU
	// compositing to stay enabled in the browser process. Leave Chromium's
	// defaults otherwise — we want a Chromium-faithful engine.
	if (process_type.empty()) {
		// Browser process. Nothing forced off; the GPU path is on by default.
		// (If a machine falls back to software GL, OnAcceleratedPaint never
		// fires and we'd see OnPaint instead — diagnosed at runtime.)
	}
}

void
CefHostApp::OnWebKitInitialized()
{
	// Renderer-side router config must match the browser side (default cefQuery /
	// cefQueryCancel function names — see CefHostClient).
	CefMessageRouterConfig config;
	render_router_ = CefMessageRouterRendererSide::Create(config);
}

void
CefHostApp::OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
	if (render_router_) {
		render_router_->OnContextCreated(browser, frame, context);
	}
}

void
CefHostApp::OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
	if (render_router_) {
		render_router_->OnContextReleased(browser, frame, context);
	}
}

bool
CefHostApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame> frame,
                                     CefProcessId source_process,
                                     CefRefPtr<CefProcessMessage> message)
{
	if (render_router_ &&
	    render_router_->OnProcessMessageReceived(browser, frame, source_process, message)) {
		return true;
	}
	return false;
}
