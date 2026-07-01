// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Weave compositing for the CEF OSR host (#625, Step A).
 *
 * Per frame this:
 *   1. Extracts the page's 3D-element sub-rect (the pre-weave side-by-side pair)
 *      from the cached composited-page texture into a keyed-mutex shared input
 *      texture.
 *   2. Drives the shipped weave RPC: xrWeaveSubmitEXT(inputSBS, windowRelRect)
 *      -> weaved shared texture + fence + the DP's current tracked eyes. The
 *      host NEVER weaves; the DP does, inside the runtime (ADR-007 / ADR-019).
 *   3. GPU-waits the fence, then composites the page (base) + the weaved sub-rect
 *      (over the element rect) into the swap-chain back buffer with a shader blit
 *      (robust to BGRA/RGBA format differences between CEF, the weaved output,
 *      and the back buffer).
 *
 * The returned eyes are written back into HostBridge for delivery to the page
 * (off-axis / look-around). Presentation is left to the caller.
 */

#pragma once

#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#define XR_USE_GRAPHICS_API_D3D11
#include "xr_session_common.h"
#include <openxr/XR_EXT_weave.h>

#include "host_bridge.h"

class WeaveCompositor {
 public:
	bool Init(ID3D11Device5 *dev, ID3D11DeviceContext4 *ctx);

	// Extract -> weave RPC -> composite into the swap-chain back buffer. winW/winH
	// are the window client size in device px (== bound-window / weaved size).
	// Writes the returned eyes into `bridge`. Returns true if it composited a
	// frame (caller then presents). outMs receives the weave round-trip latency.
	bool Frame(XrSession session,
	           IDXGISwapChain1 *swapchain,
	           uint32_t winW,
	           uint32_t winH,
	           HostBridge &bridge,
	           double *outMs);

	// Force the weaved handback + fence to be re-opened on the next Frame (call
	// on window resize, since the weaved output dims change).
	void OnResize() { have_handback_ = false; }

 private:
	bool EnsureBlitPipeline();
	bool EnsureSbsInput(uint32_t w, uint32_t h);
	bool OpenHandback(const XrWeaveOutputEXT &out);
	void BlitQuad(ID3D11ShaderResourceView *srv,
	              float dstX,
	              float dstY,
	              float dstW,
	              float dstH, // [0,1] window coords
	              float srcU,
	              float srcV,
	              float srcW,
	              float srcH); // [0,1] uv

	Microsoft::WRL::ComPtr<ID3D11Device5> device_;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;

	// Blit pipeline.
	Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
	Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> samp_;
	Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;

	// Pre-weave SBS input (keyed-mutex shared, sized to the element rect).
	Microsoft::WRL::ComPtr<ID3D11Texture2D> sbs_;
	Microsoft::WRL::ComPtr<IDXGIKeyedMutex> sbs_mutex_;
	HANDLE sbs_handle_ = nullptr;
	uint32_t sbs_w_ = 0, sbs_h_ = 0;

	// Weaved handback (opened from the runtime's exported handles).
	Microsoft::WRL::ComPtr<ID3D11Texture2D> weaved_;        //!< service-allocated, opened
	Microsoft::WRL::ComPtr<ID3D11Texture2D> weaved_srv_tex_; //!< host copy w/ SRV bind
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> weaved_srv_;
	Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
	uint32_t weaved_w_ = 0, weaved_h_ = 0;
	bool have_handback_ = false;
};
