// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  OpenXR session management for the CEF OSR weave host (#625, Step A).
 *
 * Forked from the weave_rpc_probe harness in the runtime repo — identical
 * forced-IPC present-owner session + XR_DXR_weave entry-point resolution.
 */

#include "xr_session.h"
#include "logging.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

bool g_hasWeaveExt = false;
uint32_t g_weaveSpecVersion = 0;
PFN_xrWeaveBindWindowDXR g_pfnWeaveBindWindow = nullptr;
PFN_xrWeaveSubmitDXR g_pfnWeaveSubmit = nullptr;
PFN_xrWeaveSnapWindowRectDXR g_pfnWeaveSnapWindowRect = nullptr;

#define XR_CHECK(call)                                                                                                  \
	do {                                                                                                               \
		XrResult result = (call);                                                                                    \
		if (XR_FAILED(result)) {                                                                                     \
			LogXrResult(#call, result);                                                                          \
			return false;                                                                                        \
		}                                                                                                            \
	} while (0)

#define XR_CHECK_LOG(call)                                                                                             \
	do {                                                                                                               \
		XrResult result = (call);                                                                                    \
		LogXrResult(#call, result);                                                                                  \
		if (XR_FAILED(result)) {                                                                                     \
			return false;                                                                                        \
		}                                                                                                            \
	} while (0)

bool
GetD3D11GraphicsRequirements(XrSessionManager &xr, LUID *outAdapterLuid)
{
	PFN_xrGetD3D11GraphicsRequirementsKHR pfn = nullptr;
	XrResult result = xrGetInstanceProcAddr(xr.instance, "xrGetD3D11GraphicsRequirementsKHR",
	                                        (PFN_xrVoidFunction *)&pfn);
	if (XR_FAILED(result) || !pfn) {
		LOG_ERROR("Failed to get xrGetD3D11GraphicsRequirementsKHR");
		return false;
	}
	XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
	result = pfn(xr.instance, xr.systemId, &req);
	if (XR_FAILED(result)) {
		LogXrResult("xrGetD3D11GraphicsRequirementsKHR", result);
		return false;
	}
	LOG_INFO("D3D11 adapter LUID: 0x%08X%08X", req.adapterLuid.HighPart, req.adapterLuid.LowPart);
	*outAdapterLuid = req.adapterLuid;
	return true;
}

bool
InitializeOpenXR(XrSessionManager &xr)
{
	uint32_t extensionCount = 0;
	XR_CHECK_LOG(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
	std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
	XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

	bool hasD3D11 = false;
	xr.hasWin32WindowBindingExt = false;
	xr.hasDisplayInfoExt = false;
	g_hasWeaveExt = false;
	g_weaveSpecVersion = 0;
	for (const auto &ext : extensions) {
		if (strcmp(ext.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) {
			hasD3D11 = true;
		}
		if (strcmp(ext.extensionName, XR_DXR_WIN32_WINDOW_BINDING_EXTENSION_NAME) == 0) {
			xr.hasWin32WindowBindingExt = true;
		}
		if (strcmp(ext.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0) {
			xr.hasDisplayInfoExt = true;
		}
		if (strcmp(ext.extensionName, XR_DXR_WEAVE_EXTENSION_NAME) == 0) {
			g_hasWeaveExt = true;
			// The RUNTIME's spec version, not our vendored header's — the
			// submit path is chosen from this (batch >= 3, firstChunk >= 5).
			g_weaveSpecVersion = ext.extensionVersion;
		}
	}

	LOG_INFO("XR_KHR_D3D11_enable:         %s", hasD3D11 ? "AVAILABLE" : "NOT FOUND");
	LOG_INFO("XR_DXR_win32_window_binding: %s", xr.hasWin32WindowBindingExt ? "AVAILABLE" : "NOT FOUND");
	LOG_INFO("XR_DXR_display_info:         %s", xr.hasDisplayInfoExt ? "AVAILABLE" : "NOT FOUND");
	// Reproducer knob: cap the negotiated spec version so one build can exercise
	// every submit shape the extension has shipped (DXR_WEAVE_MAX_SPEC=2 forces
	// the legacy per-element path, =4 the batch without the v5 firstChunk clear).
	// This is what makes the host useful for bisecting "is this bug batch-only?"
	// against a current runtime — it never raises the version, only lowers it.
	if (const char *cap = getenv("DXR_WEAVE_MAX_SPEC")) {
		uint32_t capped = (uint32_t)atoi(cap);
		if (capped < g_weaveSpecVersion) {
			LOG_INFO("DXR_WEAVE_MAX_SPEC=%s: capping weave spec v%u -> v%u", cap, g_weaveSpecVersion,
			         capped);
			g_weaveSpecVersion = capped;
		}
	}

	LOG_INFO("XR_DXR_weave:                %s (runtime spec v%u, header v%u) -> %s submit",
	         g_hasWeaveExt ? "AVAILABLE" : "NOT FOUND", g_weaveSpecVersion,
	         (uint32_t)XR_DXR_weave_SPEC_VERSION,
	         g_weaveSpecVersion >= 3 ? (g_weaveSpecVersion >= 5 ? "BATCHED + firstChunk" : "BATCHED")
	                                 : "LEGACY per-element");

	if (!hasD3D11) {
		LOG_ERROR("XR_KHR_D3D11_enable not available - cannot continue");
		return false;
	}
	if (!xr.hasWin32WindowBindingExt) {
		LOG_ERROR("XR_DXR_win32_window_binding not available - cannot bind the present-owner window");
		return false;
	}
	if (!g_hasWeaveExt) {
		LOG_ERROR("XR_DXR_weave not advertised - the weave service is unavailable on this runtime");
		return false;
	}

	std::vector<const char *> enabled;
	enabled.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
	enabled.push_back(XR_DXR_WIN32_WINDOW_BINDING_EXTENSION_NAME);
	if (xr.hasDisplayInfoExt) {
		enabled.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
	}
	enabled.push_back(XR_DXR_WEAVE_EXTENSION_NAME);

	XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy_s(createInfo.applicationInfo.applicationName, "DXRCefWeaveHost");
	createInfo.applicationInfo.applicationVersion = 1;
	strcpy_s(createInfo.applicationInfo.engineName, "None");
	createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	createInfo.enabledExtensionCount = (uint32_t)enabled.size();
	createInfo.enabledExtensionNames = enabled.data();
	XR_CHECK_LOG(xrCreateInstance(&createInfo, &xr.instance));

	XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK_LOG(xrGetSystem(xr.instance, &systemInfo, &xr.systemId));

	{
		XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
		if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
			memcpy(xr.systemName, sysProps.systemName, sizeof(xr.systemName));
			LOG_INFO("System name: %s", xr.systemName);
		}
	}
	if (xr.hasDisplayInfoExt) {
		XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
		XrDisplayInfoDXR di = {(XrStructureType)XR_TYPE_DISPLAY_INFO_DXR};
		sysProps.next = &di;
		if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
			xr.displayPixelWidth = di.displayPixelWidth;
			xr.displayPixelHeight = di.displayPixelHeight;
			LOG_INFO("Display pixels: %ux%u", xr.displayPixelWidth, xr.displayPixelHeight);
		}
	}

	// Enumerate view config (required before xrBeginSession / valid session).
	uint32_t viewCount = 0;
	XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));
	xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount,
	                                           xr.configViews.data()));
	LOG_INFO("OpenXR init complete (%u views)", viewCount);
	return true;
}

bool
QueryWeaveModeInfo(XrSessionManager &xr, WeaveModeInfo *out)
{
	*out = WeaveModeInfo{};
	if (!xr.hasDisplayInfoExt || xr.session == XR_NULL_HANDLE) {
		return false;
	}
	PFN_xrEnumerateDisplayRenderingModesDXR pfn = nullptr;
	if (XR_FAILED(xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesDXR",
	                                    (PFN_xrVoidFunction *)&pfn)) ||
	    pfn == nullptr) {
		LOG_INFO("xrEnumerateDisplayRenderingModesDXR unavailable — no v6 atlas layout");
		return false;
	}

	uint32_t count = 0;
	if (XR_FAILED(pfn(xr.session, 0, &count, nullptr)) || count == 0) {
		return false;
	}
	std::vector<XrDisplayRenderingModeInfoDXR> modes(count,
	                                                 {(XrStructureType)XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR});
	if (XR_FAILED(pfn(xr.session, count, &count, modes.data()))) {
		return false;
	}

	// Worst case over EVERY mode, sized from the display (ADR-010): the atlas is
	// allocated once and never reallocated on window resize.
	const float dw = (float)(xr.displayPixelWidth ? xr.displayPixelWidth : 1);
	const float dh = (float)(xr.displayPixelHeight ? xr.displayPixelHeight : 1);
	uint32_t worstW = 0, worstH = 0;
	const XrDisplayRenderingModeInfoDXR *active = nullptr;
	for (const auto &m : modes) {
		if (m.tileColumns == 0 || m.tileRows == 0 || m.viewScaleX <= 0.0f || m.viewScaleY <= 0.0f) {
			continue;
		}
		worstW = std::max(worstW, (uint32_t)(m.tileColumns * m.viewScaleX * dw + 0.5f));
		worstH = std::max(worstH, (uint32_t)(m.tileRows * m.viewScaleY * dh + 0.5f));
		if (m.isActive == XR_TRUE) {
			active = &m;
		}
	}
	if (active == nullptr || worstW == 0 || worstH == 0) {
		return false;
	}

	out->viewCount = active->viewCount;
	out->tileColumns = active->tileColumns;
	out->tileRows = active->tileRows;
	out->viewScaleX = active->viewScaleX;
	out->viewScaleY = active->viewScaleY;
	out->atlasWidth = worstW;
	out->atlasHeight = worstH;
	out->valid = true;
	LOG_INFO("active rendering mode '%s': views=%u grid=%ux%u scale=%.3fx%.3f -> worst-case atlas %ux%u",
	         active->modeName, out->viewCount, out->tileColumns, out->tileRows, out->viewScaleX,
	         out->viewScaleY, out->atlasWidth, out->atlasHeight);
	return true;
}

bool
CreateSession(XrSessionManager &xr, ID3D11Device *d3d11Device, HWND appHwnd)
{
	LOG_INFO("Creating forced-IPC session (present-owner: HWND=%p, transparent, no shared texture)",
	         (void *)appHwnd);
	xr.windowHandle = appHwnd;

	XrGraphicsBindingD3D11KHR d3d11Binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
	d3d11Binding.device = d3d11Device;

	// Present-owner binding: real HWND + transparentBackgroundEnabled, NO shared
	// texture. The non-null HWND makes the per-client DP bind to this window
	// (correct phase reference); transparent avoids the cross-process swap-chain
	// path (the service can't present our window). We never submit projection
	// layers — the weave service is driven directly via xrWeaveSubmitDXR.
	XrWin32WindowBindingCreateInfoDXR bind = {XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR};
	bind.windowHandle = (void *)appHwnd;
	bind.sharedTextureHandle = nullptr;
	bind.transparentBackgroundEnabled = XR_TRUE;
	d3d11Binding.next = &bind;

	XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
	sessionInfo.next = &d3d11Binding;
	sessionInfo.systemId = xr.systemId;
	XR_CHECK_LOG(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
	LOG_INFO("Session created: %p", (void *)xr.session);

	// Resolve the weave entry points.
	xrGetInstanceProcAddr(xr.instance, "xrWeaveBindWindowDXR", (PFN_xrVoidFunction *)&g_pfnWeaveBindWindow);
	xrGetInstanceProcAddr(xr.instance, "xrWeaveSubmitDXR", (PFN_xrVoidFunction *)&g_pfnWeaveSubmit);
	// #625 window-drag phase lock — optional: an older runtime (spec v1) won't
	// resolve it, in which case the host simply skips the snap and drags as before.
	xrGetInstanceProcAddr(xr.instance, "xrWeaveSnapWindowRectDXR",
	                      (PFN_xrVoidFunction *)&g_pfnWeaveSnapWindowRect);
	LOG_INFO("xrWeaveBindWindowDXR: %s, xrWeaveSubmitDXR: %s, xrWeaveSnapWindowRectDXR: %s",
	         g_pfnWeaveBindWindow ? "resolved" : "NULL", g_pfnWeaveSubmit ? "resolved" : "NULL",
	         g_pfnWeaveSnapWindowRect ? "resolved" : "NULL");
	if (!g_pfnWeaveBindWindow || !g_pfnWeaveSubmit) {
		LOG_ERROR("Failed to resolve weave entry points");
		return false;
	}
	return true;
}
