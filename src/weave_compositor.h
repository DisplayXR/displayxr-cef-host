// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Weave compositing for the CEF OSR host (#625).
 *
 * Per frame this drives the shipped XR_DXR_weave RPC and composites the result.
 * The host NEVER weaves; the DP does, inside the runtime (ADR-007 / ADR-019).
 *
 * Two submit paths, chosen from the SPEC VERSION THE RUNTIME REPORTS
 * (g_weaveSpecVersion) — never from the vendored header's, so a newer header
 * never forces a newer runtime:
 *
 *  - **Batched (runtime spec >= 3, the path you should copy).** ONE
 *    xrWeaveSubmitDXR per frame carrying every visible element rect via a
 *    chained XrWeaveSubmitRectsDXR. The chain switches the input-layout
 *    contract: @c inputTexture is sized to the bound window's client area and
 *    each rect's pre-weave SBS content sits AT THAT RECT'S OWN WINDOW POSITION.
 *    That is exactly how CEF already composites the page — each `.element3d`
 *    canvas renders squeezed SBS at its own layout position — so the batch
 *    input is a straight copy of the page texture, no per-element repack. One
 *    submit, one fence wait, one copy of the woven output, regardless of N.
 *    On runtime spec >= 5 the submit also sets @c firstChunk, which clears the
 *    woven output to premultiplied-transparent between the tiles so the host
 *    draws it back WHOLE-WINDOW with one premultiplied "over" blit.
 *
 *  - **Legacy per-element (runtime spec < 3), kept as a documented fallback.**
 *    One submit per element, each with its own element-sized SBS input, each
 *    result copied out before the next submit can overwrite the shared woven
 *    output. Costs the IPC round-trip + shared-texture open + keyed-mutex +
 *    fence N times per frame; that per-call fixed cost is exactly what the
 *    batch amortizes.
 *
 * Deliberately NOT implemented (see README "Spec coverage"): the v4
 * DP-composited overlay atlas and the v6 N-view worst-case atlas.
 *
 * The eyes returned by the submit are written back into HostBridge for delivery
 * to the page (off-axis / look-around). Presentation is left to the caller.
 */

#pragma once

#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <vector>

#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"
#include <openxr/XR_DXR_weave.h>

#include "host_bridge.h"
#include "xr_session.h" // WeaveModeInfo, g_weaveSpecVersion

class WeaveCompositor {
 public:
	bool Init(ID3D11Device5 *dev, ID3D11DeviceContext4 *ctx);

	// Weave RPC -> composite into the swap-chain back buffer. winW/winH are the
	// window client size in device px (== bound-window / weaved size). Writes the
	// returned eyes into `bridge`. Returns true if it composited a frame (caller
	// then presents). outMs receives the weave round-trip latency (submit only).
	bool Frame(XrSession session,
	           IDXGISwapChain1 *swapchain,
	           uint32_t winW,
	           uint32_t winH,
	           HostBridge &bridge,
	           double *outMs);

	// Force the weaved handback + fence to be re-opened on the next Frame (call
	// on window resize, since the weaved output dims change).
	void OnResize() { have_handback_ = false; }

	// Hand over the active rendering mode's atlas geometry (from
	// QueryWeaveModeInfo) to enable the spec-v6 N-view submit path. Without it
	// the compositor takes the v3 batch.
	void SetModeInfo(const WeaveModeInfo &m)
	{
		mode_ = m;
		mode_queried_ = true;
	}

 private:
	//! One element rect clamped to the page texture and the window (device px).
	struct ClampedRect
	{
		int32_t x, y, w, h;
	};

	bool EnsureBlitPipeline();
	//! Keyed-mutex shared input texture. The batch path sizes it to the WINDOW;
	//! the legacy path sizes it to the element rect.
	bool EnsureSharedInput(uint32_t w, uint32_t h);
	bool OpenHandback(const XrWeaveOutputDXR &out);
	void BlitQuad(ID3D11ShaderResourceView *srv,
	              float dstX,
	              float dstY,
	              float dstW,
	              float dstH, // [0,1] window coords
	              float srcU,
	              float srcV,
	              float srcW,
	              float srcH,                    // [0,1] uv
	              ID3D11PixelShader *ps); // nullptr = the opaque-overwrite PS
	//! Apply the returned eyes to the bridge (identical for every element/rect).
	static void StoreEyes(const XrWeaveOutputDXR &out, HostBridge &bridge);

	//! Worst-case N-view atlas for the spec-v6 path: keyed-mutex shared AND a
	//! render target, because the host packs the tiles itself.
	bool EnsureAtlas(uint32_t w, uint32_t h);

	// The three submit + composite paths. All assume Frame() has already bound
	// the back buffer, blitted the page base, and set the viewport.
	uint32_t CompositeNView(XrSession session,
	                        const std::vector<ClampedRect> &rects,
	                        uint32_t winW,
	                        uint32_t winH,
	                        HostBridge &bridge,
	                        double *outMs);
	uint32_t CompositeBatched(XrSession session,
	                          const std::vector<ClampedRect> &rects,
	                          uint32_t winW,
	                          uint32_t winH,
	                          HostBridge &bridge,
	                          double *outMs);
	uint32_t CompositeLegacy(XrSession session,
	                         const std::vector<ClampedRect> &rects,
	                         uint32_t winW,
	                         uint32_t winH,
	                         HostBridge &bridge,
	                         double *outMs);

	Microsoft::WRL::ComPtr<ID3D11Device5> device_;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;
	//! This frame's back-buffer RTV — the v6 path re-binds it after packing the
	//! atlas (which retargets the context at the atlas).
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> back_rtv_;

	// Blit pipeline. `blend_` overwrites (page base, per-tile draw-back);
	// `blend_over_` is premultiplied "over" for the whole-window draw-back.
	Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_over_;
	Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> samp_;
	Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
	Microsoft::WRL::ComPtr<ID3D11BlendState> blend_over_;

	// Pre-weave SBS input (keyed-mutex shared; window-sized under the batch).
	Microsoft::WRL::ComPtr<ID3D11Texture2D> sbs_;
	Microsoft::WRL::ComPtr<IDXGIKeyedMutex> sbs_mutex_;
	HANDLE sbs_handle_ = nullptr;
	uint32_t sbs_w_ = 0, sbs_h_ = 0;

	// Spec-v6 N-view atlas: the host packs the tiles, so this is both a shared
	// keyed-mutex input AND a render target. Allocated once at the worst-case
	// size (mode_.atlasWidth/Height) — window resize never reallocates it.
	Microsoft::WRL::ComPtr<ID3D11Texture2D> atlas_;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> atlas_rtv_;
	Microsoft::WRL::ComPtr<IDXGIKeyedMutex> atlas_mutex_;
	HANDLE atlas_handle_ = nullptr;
	uint32_t atlas_w_ = 0, atlas_h_ = 0;
	//! Active-mode geometry, queried once (see the note in CompositeNView).
	WeaveModeInfo mode_;
	bool mode_queried_ = false;

	// Weaved handback (opened from the runtime's exported handles).
	Microsoft::WRL::ComPtr<ID3D11Texture2D> weaved_;         //!< service-allocated, opened
	Microsoft::WRL::ComPtr<ID3D11Texture2D> weaved_srv_tex_; //!< host copy w/ SRV bind
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> weaved_srv_;
	Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
	uint32_t weaved_w_ = 0, weaved_h_ = 0;
	bool have_handback_ = false;
};
