// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Shared state between the CEF callbacks and the weave loop (#625).
 *
 * Everything here is touched on a SINGLE thread: the host's main thread, which
 * is also the CEF "UI" thread because the host pumps CefDoMessageLoopWork()
 * inline from its own loop (multi_threaded_message_loop is OFF). So CEF
 * callbacks (OnAcceleratedPaint, OnQuery) and the weave loop never race — no
 * locks are needed. If that threading model ever changes, this struct needs a
 * mutex.
 */

#pragma once

#include <d3d11_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include "include/wrapper/cef_message_router.h"

//! Mirrors XR_WEAVE_MAX_EYES_DXR (kept local so this header has no OpenXR dep).
static const uint32_t kHostMaxEyes = 8;

//! One inline-3D element's committed device-pixel rect within the window client
//! area (y-down). The DOM drives the SET of 3D regions (#625 multi-element):
//! one of these per `.element3d` canvas, refreshed every frame from the page.
struct Element3D {
	int32_t x = 0, y = 0;
	uint32_t w = 0, h = 0;
};

//! The single shared bridge between CEF (content + page messages) and the weave
//! loop. Created once in main and handed to the CEF client.
struct HostBridge {
	// --- Host D3D11 device (set by main BEFORE the browser is created) -------
	Microsoft::WRL::ComPtr<ID3D11Device5> device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;

	// --- Composited page texture (written by OnAcceleratedPaint) ------------
	// A host-owned copy (CEF's accelerated-paint texture cannot be cached: it
	// is pool-allocated and reclaimed when the callback returns). BGRA, full
	// window client size.
	Microsoft::WRL::ComPtr<ID3D11Texture2D> pageTex;
	uint32_t pageW = 0;
	uint32_t pageH = 0;
	uint64_t pageFrame = 0; //!< increments on every accelerated paint

	// --- 3D element rects reported by the page (written by OnQuery) ----------
	// The page posts the full list every frame ("rects <n> x0 y0 w0 h0 ...");
	// OnQuery replaces this wholesale, so removing a `.element3d` from the DOM
	// simply drops it from the next list (no stale-element bookkeeping).
	std::vector<Element3D> elements;
	uint64_t rectSeq = 0; //!< increments on every rect-list update

	// --- Eyes-down channel ---------------------------------------------------
	// The persistent cefQuery callback the page registered to receive eyes.
	CefRefPtr<CefMessageRouterBrowserSide::Callback> eyesCallback;

	// Latest eyes returned by xrWeaveSubmitDXR (written by main, delivered to
	// the page via eyesCallback). Display-space metres, [eye*3 + {0:x,1:y,2:z}].
	float eyes[kHostMaxEyes * 3] = {};
	uint32_t eyeCount = 0;
	bool eyesValid = false;
	bool eyesTracking = false;

	// --- Lifecycle -----------------------------------------------------------
	bool browserClosed = false;
};
