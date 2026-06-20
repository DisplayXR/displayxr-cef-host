// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR session management for the CEF OSR weave host (#625, Step A).
 *
 * Forked from test_apps/weave_rpc_probe_d3d11_win/xr_session.{h,cpp} in the
 * runtime repo. The host is a present-owner: it owns its OS window, runs
 * forced-IPC, binds its window for DP phase-snap, and drives the shipped
 * XR_EXT_weave service (xrWeaveBindWindowEXT + xrWeaveSubmitEXT). The content
 * source is a real Chromium engine via CEF offscreen rendering instead of the
 * probe's synthetic side-by-side painter — but the OpenXR/weave plumbing is
 * identical. The session is created with XR_EXT_win32_window_binding (real HWND
 * + transparentBackgroundEnabled, NO shared texture) so the per-client display
 * processor binds to the host's window and the service never tries to present
 * the cross-process window. There is no frame loop / projection submission —
 * the weave service is synchronous and independent of xrEndFrame.
 */

#pragma once

#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"
#include <openxr/XR_EXT_weave.h>

// XR_EXT_weave available + enabled on the instance, and the resolved entry points.
extern bool g_hasWeaveExt;
extern PFN_xrWeaveBindWindowEXT g_pfnWeaveBindWindow;
extern PFN_xrWeaveSubmitEXT g_pfnWeaveSubmit;

// Initialize OpenXR instance + system; detect/enable D3D11 + win32_window_binding
// + display_info + weave.
bool InitializeOpenXR(XrSessionManager& xr);

// Get the D3D11 graphics requirements (adapter LUID) so the host's device is on
// the adapter the runtime requires (shared handles only work same-adapter).
bool GetD3D11GraphicsRequirements(XrSessionManager& xr, LUID* outAdapterLuid);

// Create the forced-IPC session: D3D11 binding + win32 window binding (real HWND,
// transparentBackgroundEnabled, NO shared texture). Resolves the weave PFNs.
bool CreateSession(XrSessionManager& xr, ID3D11Device* d3d11Device, HWND appHwnd);
