// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  OpenXR session management for the CEF OSR weave host (#625, Step A).
 *
 * Forked from test_apps/weave_rpc_probe_d3d11_win/xr_session.{h,cpp} in the
 * runtime repo. The host is a present-owner: it owns its OS window, runs
 * forced-IPC, binds its window for DP phase-snap, and drives the shipped
 * XR_DXR_weave service (xrWeaveBindWindowDXR + xrWeaveSubmitDXR). The content
 * source is a real Chromium engine via CEF offscreen rendering instead of the
 * probe's synthetic side-by-side painter — but the OpenXR/weave plumbing is
 * identical. The session is created with XR_DXR_win32_window_binding (real HWND
 * + transparentBackgroundEnabled, NO shared texture) so the per-client display
 * processor binds to the host's window and the service never tries to present
 * the cross-process window. There is no frame loop / projection submission —
 * the weave service is synchronous and independent of xrEndFrame.
 */

#pragma once

#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"
#include <openxr/XR_DXR_weave.h>

// XR_DXR_weave available + enabled on the instance, and the resolved entry points.
extern bool g_hasWeaveExt;

//! The XR_DXR_weave SPEC version the RUNTIME reported at extension enumeration
//! (0 = extension absent). The host negotiates DOWN to it, so a newer vendored
//! header never forces a newer runtime:
//!   >= 3  batched submit (chained XrWeaveSubmitRectsDXR) — one call per frame.
//!   >= 5  XrWeaveSubmitInfoDXR::firstChunk — the runtime clears its woven
//!         output to transparent, so the host can draw it back WHOLE-WINDOW.
//! Anything lower takes the legacy one-submit-per-element path.
//!
//! Set DXR_WEAVE_MAX_SPEC=<n> to cap this (never raise it) — one build then
//! reproduces every submit shape the extension has shipped against a current
//! runtime, which is the point of keeping this host around.
extern uint32_t g_weaveSpecVersion;
extern PFN_xrWeaveBindWindowDXR g_pfnWeaveBindWindow;
extern PFN_xrWeaveSubmitDXR g_pfnWeaveSubmit;
extern PFN_xrWeaveSnapWindowRectDXR g_pfnWeaveSnapWindowRect; // #625 window-drag phase lock (may be NULL on an older runtime)

/*!
 * @brief The active rendering mode's atlas geometry — what spec-v6 needs.
 *
 * Queried once via xrEnumerateDisplayRenderingModesDXR. @c atlasWidth /
 * @c atlasHeight are the WORST CASE over every enumerated mode, derived from
 * the DISPLAY (not the window), so the v6 input atlas is allocated once and
 * survives resize / fullscreen (ADR-010). Note the worst case is NOT bounded by
 * the display size — a mode with viewScaleX 1.0 and tileColumns 2 needs 2W.
 */
struct WeaveModeInfo
{
	uint32_t viewCount = 0;
	uint32_t tileColumns = 0;
	uint32_t tileRows = 0;
	float viewScaleX = 0.0f;
	float viewScaleY = 0.0f;
	uint32_t atlasWidth = 0;  //!< worst case over all modes
	uint32_t atlasHeight = 0; //!< worst case over all modes
	bool valid = false;
};

// Initialize OpenXR instance + system; detect/enable D3D11 + win32_window_binding
// + display_info + weave.
bool InitializeOpenXR(XrSessionManager& xr);

// Query the active rendering mode + the worst-case atlas size for the spec-v6
// submit path. Returns false if XR_DXR_display_info is absent or enumeration
// fails, in which case the caller falls back to the v3 batch layout.
bool QueryWeaveModeInfo(XrSessionManager& xr, WeaveModeInfo* out);

// Get the D3D11 graphics requirements (adapter LUID) so the host's device is on
// the adapter the runtime requires (shared handles only work same-adapter).
bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create the forced-IPC session: D3D11 binding + win32 window binding (real HWND,
// transparentBackgroundEnabled, NO shared texture). Resolves the weave PFNs.
bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device, HWND appHwnd);
