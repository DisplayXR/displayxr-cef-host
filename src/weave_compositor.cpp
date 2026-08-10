// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Weave compositing implementation for the CEF OSR host (#625).
 */

#include "weave_compositor.h"
#include "xr_session.h" // g_pfnWeaveSubmit, g_weaveSpecVersion
#include "logging.h"

#include <d3dcompiler.h>
#include <algorithm>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

// Per-blit constants: dst rect in [0,1] window coords, src rect in [0,1] uv.
struct BlitCB {
	float dst[4];
	float src[4];
};

static const char *kBlitHlsl = R"(
cbuffer Blit : register(b0) { float4 gDst; float4 gSrc; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint vid : SV_VertexID) {
    float2 c = float2((vid == 1 || vid == 3) ? 1.0 : 0.0, (vid >= 2) ? 1.0 : 0.0);
    float2 p = gDst.xy + c * gDst.zw;          // [0,1] window space
    VSOut o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0); // -> NDC (y down)
    o.uv = gSrc.xy + c * gSrc.zw;
    return o;
}
Texture2D gTex : register(t0);
SamplerState gSamp : register(s0);
// Opaque overwrite: the page base and the per-tile draw-back replace the target.
float4 PSMain(VSOut i) : SV_TARGET { return float4(gTex.Sample(gSamp, i.uv).rgb, 1.0); }
// Premultiplied "over": the woven output's alpha IS the 3D-vs-page mask under
// spec-v5 firstChunk (opaque tiles replace the page, cleared gaps show it
// through), so alpha must reach the blender unmodified.
float4 PSOver(VSOut i) : SV_TARGET { return gTex.Sample(gSamp, i.uv); }
)";

bool
WeaveCompositor::Init(ID3D11Device5 *dev, ID3D11DeviceContext4 *ctx)
{
	device_ = dev;
	context_ = ctx;
	return EnsureBlitPipeline();
}

//! Compile one entry point of kBlitHlsl into a pixel shader.
static bool
CompilePs(ID3D11Device5 *device, const char *entry, ID3D11PixelShader **out)
{
	ComPtr<ID3DBlob> blob, err;
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "blit", nullptr, nullptr, entry, "ps_5_0",
	                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err))) {
		LOG_ERROR("blit PS(%s) compile failed: %s", entry, err ? (const char *)err->GetBufferPointer() : "?");
		return false;
	}
	if (FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out))) {
		LOG_ERROR("blit PS(%s) create failed", entry);
		return false;
	}
	return true;
}

bool
WeaveCompositor::EnsureBlitPipeline()
{
	if (vs_ && ps_ && ps_over_) {
		return true;
	}
	ComPtr<ID3DBlob> vsb, err;
	UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "blit", nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &vsb,
	                      &err))) {
		LOG_ERROR("blit VS compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
		return false;
	}
	if (FAILED(device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_))) {
		LOG_ERROR("blit VS create failed");
		return false;
	}
	if (!CompilePs(device_.Get(), "PSMain", &ps_) || !CompilePs(device_.Get(), "PSOver", &ps_over_)) {
		return false;
	}

	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = sizeof(BlitCB);
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(device_->CreateBuffer(&bd, nullptr, &cb_))) {
		LOG_ERROR("blit CB create failed");
		return false;
	}

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // point: weave needs device-pixel-exact sampling
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(device_->CreateSamplerState(&sd, &samp_))) {
		LOG_ERROR("blit sampler create failed");
		return false;
	}

	D3D11_BLEND_DESC blend = {}; // opaque overwrite (PSMain already forces alpha = 1)
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(device_->CreateBlendState(&blend, &blend_))) {
		LOG_ERROR("blit blend state create failed");
		return false;
	}

	// Premultiplied "over" (dst = src + dst * (1 - src.a)) for the spec-v5
	// whole-window draw-back of the woven output.
	D3D11_BLEND_DESC over = {};
	over.RenderTarget[0].BlendEnable = TRUE;
	over.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	over.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	over.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	over.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	over.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	over.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	over.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(device_->CreateBlendState(&over, &blend_over_))) {
		LOG_ERROR("blit 'over' blend state create failed");
		return false;
	}
	return true;
}

bool
WeaveCompositor::EnsureSharedInput(uint32_t w, uint32_t h)
{
	if (sbs_ && sbs_w_ == w && sbs_h_ == h) {
		return true;
	}
	sbs_.Reset();
	sbs_mutex_.Reset();
	if (sbs_handle_) {
		CloseHandle(sbs_handle_);
		sbs_handle_ = nullptr;
	}

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // match CEF's composited-page format
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	if (FAILED(device_->CreateTexture2D(&td, nullptr, &sbs_))) {
		LOG_ERROR("SBS input CreateTexture2D failed (%ux%u)", w, h);
		return false;
	}
	if (FAILED(sbs_.As(&sbs_mutex_))) {
		LOG_ERROR("SBS input has no keyed mutex");
		return false;
	}
	ComPtr<IDXGIResource1> res1;
	if (FAILED(sbs_.As(&res1)) ||
	    FAILED(res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
	                                    &sbs_handle_))) {
		LOG_ERROR("SBS input CreateSharedHandle failed");
		return false;
	}
	sbs_w_ = w;
	sbs_h_ = h;
	LOG_INFO("SBS input (re)created %ux%u (NT handle=%p)", w, h, sbs_handle_);
	return true;
}

bool
WeaveCompositor::OpenHandback(const XrWeaveOutputDXR &out)
{
	if (out.weavedTexture != nullptr) {
		weaved_.Reset();
		HRESULT hr = device_->OpenSharedResource1((HANDLE)out.weavedTexture, IID_PPV_ARGS(&weaved_));
		CloseHandle((HANDLE)out.weavedTexture); // caller owns the duplicated handle
		if (FAILED(hr)) {
			LOG_ERROR("OpenSharedResource1(weaved) failed: 0x%08lx", hr);
			return false;
		}
		// Host-owned SRV-capable copy target (the service texture may lack the
		// SHADER_RESOURCE bind flag; CopyResource doesn't need it on the source).
		D3D11_TEXTURE2D_DESC wd = {};
		weaved_->GetDesc(&wd);
		D3D11_TEXTURE2D_DESC cd = wd;
		cd.Usage = D3D11_USAGE_DEFAULT;
		cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		cd.MiscFlags = 0;
		cd.CPUAccessFlags = 0;
		weaved_srv_tex_.Reset();
		weaved_srv_.Reset();
		if (FAILED(device_->CreateTexture2D(&cd, nullptr, &weaved_srv_tex_))) {
			LOG_ERROR("weaved SRV copy CreateTexture2D failed");
			return false;
		}
		if (FAILED(device_->CreateShaderResourceView(weaved_srv_tex_.Get(), nullptr, &weaved_srv_))) {
			LOG_ERROR("weaved SRV create failed");
			return false;
		}
	}
	if (out.fence != nullptr) {
		fence_.Reset();
		HRESULT hr = device_->OpenSharedFence((HANDLE)out.fence, IID_PPV_ARGS(&fence_));
		CloseHandle((HANDLE)out.fence);
		if (FAILED(hr)) {
			LOG_ERROR("OpenSharedFence failed: 0x%08lx", hr);
			return false;
		}
	}
	weaved_w_ = out.width;
	weaved_h_ = out.height;
	LOG_INFO("Opened weaved handback %ux%u (tex=%p fence=%p)", weaved_w_, weaved_h_, (void *)weaved_.Get(),
	         (void *)fence_.Get());
	return true;
}

void
WeaveCompositor::BlitQuad(ID3D11ShaderResourceView *srv,
                          float dstX,
                          float dstY,
                          float dstW,
                          float dstH,
                          float srcU,
                          float srcV,
                          float srcW,
                          float srcH,
                          ID3D11PixelShader *ps)
{
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(context_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		return;
	}
	BlitCB cb = {{dstX, dstY, dstW, dstH}, {srcU, srcV, srcW, srcH}};
	memcpy(m.pData, &cb, sizeof(cb));
	context_->Unmap(cb_.Get(), 0);

	context_->VSSetShader(vs_.Get(), nullptr, 0);
	context_->PSSetShader(ps ? ps : ps_.Get(), nullptr, 0);
	context_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
	ID3D11ShaderResourceView *srvs[1] = {srv};
	context_->PSSetShaderResources(0, 1, srvs);
	context_->PSSetSamplers(0, 1, samp_.GetAddressOf());
	context_->IASetInputLayout(nullptr);
	context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context_->Draw(4, 0);
}

void
WeaveCompositor::StoreEyes(const XrWeaveOutputDXR &out, HostBridge &bridge)
{
	// Returned eyes -> bridge (the VIEWER's, global: identical for every element,
	// delivered to the page for the next frame's off-axis render).
	if (out.eyesValid != XR_TRUE || out.eyeCount == 0) {
		return;
	}
	uint32_t n = out.eyeCount > kHostMaxEyes ? kHostMaxEyes : out.eyeCount;
	for (uint32_t i = 0; i < n; i++) {
		bridge.eyes[i * 3 + 0] = out.eyes[i].x;
		bridge.eyes[i * 3 + 1] = out.eyes[i].y;
		bridge.eyes[i * 3 + 2] = out.eyes[i].z;
	}
	bridge.eyeCount = n;
	bridge.eyesValid = true;
	bridge.eyesTracking = (out.eyesTracking == XR_TRUE);
}

bool
WeaveCompositor::EnsureAtlas(uint32_t w, uint32_t h)
{
	if (atlas_ && atlas_w_ == w && atlas_h_ == h) {
		return true;
	}
	atlas_.Reset();
	atlas_rtv_.Reset();
	atlas_mutex_.Reset();
	if (atlas_handle_) {
		CloseHandle(atlas_handle_);
		atlas_handle_ = nullptr;
	}

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // match CEF's composited-page format
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	// RENDER_TARGET as well: unlike the v3 path (where the runtime unpacks the
	// squeezed SBS), under v6 the HOST packs the tiles, so it draws into this.
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	if (FAILED(device_->CreateTexture2D(&td, nullptr, &atlas_))) {
		LOG_ERROR("v6 atlas CreateTexture2D failed (%ux%u)", w, h);
		return false;
	}
	if (FAILED(device_->CreateRenderTargetView(atlas_.Get(), nullptr, &atlas_rtv_)) ||
	    FAILED(atlas_.As(&atlas_mutex_))) {
		LOG_ERROR("v6 atlas RTV / keyed mutex unavailable");
		return false;
	}
	ComPtr<IDXGIResource1> res1;
	if (FAILED(atlas_.As(&res1)) ||
	    FAILED(res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
	                                    &atlas_handle_))) {
		LOG_ERROR("v6 atlas CreateSharedHandle failed");
		return false;
	}
	atlas_w_ = w;
	atlas_h_ = h;
	LOG_INFO("v6 worst-case atlas created %ux%u (NT handle=%p)", w, h, atlas_handle_);
	return true;
}

// ---------------------------------------------------------------------------
// N-view worst-case atlas submit — XR_DXR_weave spec v6 (#774). The preferred
// path: the host becomes an ordinary N-view DisplayXR client.
// ---------------------------------------------------------------------------
uint32_t
WeaveCompositor::CompositeNView(XrSession session,
                                const std::vector<ClampedRect> &rects,
                                uint32_t winW,
                                uint32_t winH,
                                HostBridge &bridge,
                                double *outMs)
{
	if (!EnsureAtlas(mode_.atlasWidth, mode_.atlasHeight)) {
		return 0;
	}
	// Per-tile content size = the window's client size scaled by the ACTIVE
	// mode's view scale. Tiles pack contiguously from the top-left at this
	// stride — NOT atlasWidth / tileColumns (the rest is dead space the runtime
	// never reads).
	const uint32_t cvw = (uint32_t)(winW * mode_.viewScaleX + 0.5f);
	const uint32_t cvh = (uint32_t)(winH * mode_.viewScaleY + 0.5f);
	if (cvw == 0 || cvh == 0 || mode_.tileColumns * cvw > atlas_w_ || mode_.tileRows * cvh > atlas_h_) {
		LOG_ERROR("v6: packed %ux%u exceeds atlas %ux%u — falling back", mode_.tileColumns * cvw,
		          mode_.tileRows * cvh, atlas_w_, atlas_h_);
		return 0;
	}

	ComPtr<ID3D11ShaderResourceView> pageSrv;
	if (FAILED(device_->CreateShaderResourceView(bridge.pageTex.Get(), nullptr, &pageSrv))) {
		return 0;
	}

	// --- Host-packed atlas -------------------------------------------------
	// Clear to premultiplied transparent, then draw each element's view v into
	// tile v at the element's own window position, scaled. The alpha the host
	// writes here IS the 3D-vs-page mask: the DP is alpha-native, so the gaps
	// come out transparent in the woven output with no runtime-side clear (this
	// is why v6 does not need the v5 firstChunk).
	if (atlas_mutex_->AcquireSync(0, 1000) != S_OK) {
		return 0;
	}
	ID3D11RenderTargetView *atlasRtvs[1] = {atlas_rtv_.Get()};
	context_->OMSetRenderTargets(1, atlasRtvs, nullptr);
	const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	context_->ClearRenderTargetView(atlas_rtv_.Get(), transparent);
	float bf[4] = {1, 1, 1, 1};
	context_->OMSetBlendState(blend_.Get(), bf, 0xffffffff); // overwrite
	D3D11_VIEWPORT avp = {0.0f, 0.0f, (float)atlas_w_, (float)atlas_h_, 0.0f, 1.0f};
	context_->RSSetViewports(1, &avp);

	const float pw = (float)bridge.pageW, ph = (float)bridge.pageH;
	const float aw = (float)atlas_w_, ah = (float)atlas_h_;
	for (const ClampedRect &r : rects) {
		// The page still renders each element as squeezed SBS inside its own
		// rect, so view v is the v-th horizontal slice of the rect. Un-squeezing
		// it into the tile is the work the runtime used to do for us under v3.
		const float sliceW = (float)r.w / (float)mode_.viewCount;
		for (uint32_t v = 0; v < mode_.viewCount; v++) {
			const float tileX = (float)((v % mode_.tileColumns) * cvw);
			const float tileY = (float)((v / mode_.tileColumns) * cvh);
			const float dx = tileX + r.x * mode_.viewScaleX;
			const float dy = tileY + r.y * mode_.viewScaleY;
			const float dw = r.w * mode_.viewScaleX;
			const float dh = r.h * mode_.viewScaleY;
			BlitQuad(pageSrv.Get(), dx / aw, dy / ah, dw / aw, dh / ah, (r.x + v * sliceW) / pw,
			         (float)r.y / ph, sliceW / pw, (float)r.h / ph, nullptr);
		}
	}
	ID3D11ShaderResourceView *nullSrv[1] = {nullptr};
	context_->PSSetShaderResources(0, 1, nullSrv);
	context_->Flush();
	atlas_mutex_->ReleaseSync(0);

	// Retarget the back buffer IMMEDIATELY — before anything that can fail and
	// return. Every path out of here (including the fallback to the v3 batch)
	// must leave the context pointed at the back buffer, or the next blit would
	// land in the atlas instead of on screen.
	ID3D11RenderTargetView *backRtvs[1] = {back_rtv_.Get()};
	context_->OMSetRenderTargets(1, backRtvs, nullptr);
	D3D11_VIEWPORT winVp = {0.0f, 0.0f, (float)winW, (float)winH, 0.0f, 1.0f};
	context_->RSSetViewports(1, &winVp);

	// --- One submit --------------------------------------------------------
	// The layout chain replaces the squeezed-SBS contract; the rects ride along
	// only as a scope hint (zone publication + our own draw-back region), so
	// XR_WEAVE_SUBMIT_MAX_RECTS_DXR no longer bounds the element count.
	XrRect2Di xrRects[XR_WEAVE_SUBMIT_MAX_RECTS_DXR];
	const uint32_t n = (uint32_t)std::min(rects.size(), (size_t)XR_WEAVE_SUBMIT_MAX_RECTS_DXR);
	for (uint32_t i = 0; i < n; i++) {
		xrRects[i].offset.x = rects[i].x;
		xrRects[i].offset.y = rects[i].y;
		xrRects[i].extent.width = rects[i].w;
		xrRects[i].extent.height = rects[i].h;
	}
	XrWeaveSubmitRectsDXR batch = {XR_TYPE_WEAVE_SUBMIT_RECTS_DXR};
	batch.rectCount = n;
	batch.rects = xrRects;

	XrWeaveSubmitLayoutDXR layout = {XR_TYPE_WEAVE_SUBMIT_LAYOUT_DXR};
	layout.next = &batch;
	layout.viewCount = mode_.viewCount;
	layout.tileColumns = mode_.tileColumns;
	layout.tileRows = mode_.tileRows;
	layout.contentViewWidth = cvw;
	layout.contentViewHeight = cvh;

	XrWeaveSubmitInfoDXR in = {XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
	in.next = &layout;
	in.inputTexture = (void *)atlas_handle_;
	in.inputIsDxgi = XR_FALSE;
	// No firstChunk: the transparency between tiles is carried by OUR atlas
	// alpha, which reaches the DP untouched — there is no runtime-side scratch
	// to clear on this path.

	XrWeaveOutputDXR out = {XR_TYPE_WEAVE_OUTPUT_DXR};
	LARGE_INTEGER f, t0, t1;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	XrResult sr = g_pfnWeaveSubmit(session, &in, &out);
	QueryPerformanceCounter(&t1);
	if (XR_FAILED(sr)) {
		LogXrResult("xrWeaveSubmitDXR(v6 atlas)", sr);
		return 0;
	}
	if (outMs) {
		*outMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
	}
	StoreEyes(out, bridge);
	if (!have_handback_ || out.weavedTexture != nullptr) {
		if (!OpenHandback(out)) {
			return 0;
		}
		have_handback_ = (weaved_ != nullptr);
	}
	if (!weaved_ || !weaved_srv_) {
		return 0;
	}

	// --- Whole-window draw-back -------------------------------------------
	if (fence_) {
		context_->Wait(fence_.Get(), out.fenceValue);
	}
	context_->CopyResource(weaved_srv_tex_.Get(), weaved_.Get());

	const float wsw = weaved_w_ ? (float)weaved_w_ : (float)winW;
	const float wsh = weaved_h_ ? (float)weaved_h_ : (float)winH;
	context_->OMSetBlendState(blend_over_.Get(), bf, 0xffffffff);
	BlitQuad(weaved_srv_.Get(), 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, (float)winW / wsw, (float)winH / wsh,
	         ps_over_.Get());
	context_->OMSetBlendState(blend_.Get(), bf, 0xffffffff);
	return (uint32_t)rects.size();
}

// ---------------------------------------------------------------------------
// Batched submit — XR_DXR_weave spec v3+ (fallback for pre-v6 runtimes).
// ---------------------------------------------------------------------------
uint32_t
WeaveCompositor::CompositeBatched(XrSession session,
                                  const std::vector<ClampedRect> &rects,
                                  uint32_t winW,
                                  uint32_t winH,
                                  HostBridge &bridge,
                                  double *outMs)
{
	// The batch input is WINDOW-sized and each rect's pre-weave SBS content must
	// sit at that rect's own window position. CEF's composited page already
	// satisfies that (every `.element3d` canvas renders squeezed SBS at its own
	// layout position), so the whole page copies straight across — the identity
	// mapping the spec describes, with no per-element repack.
	if (!EnsureSharedInput(winW, winH)) {
		return 0;
	}
	// key 0 = "caller done writing, runtime may read" — release BEFORE the submit
	// or the service's same-key acquire would block.
	if (sbs_mutex_->AcquireSync(0, 1000) != S_OK) {
		return 0;
	}
	D3D11_BOX box = {};
	box.right = std::min(bridge.pageW, winW);
	box.bottom = std::min(bridge.pageH, winH);
	box.back = 1;
	if (box.right > 0 && box.bottom > 0) {
		context_->CopySubresourceRegion(sbs_.Get(), 0, 0, 0, 0, bridge.pageTex.Get(), 0, &box);
	}
	context_->Flush();
	sbs_mutex_->ReleaseSync(0);

	// ONE submit per chunk of up to XR_WEAVE_SUBMIT_MAX_RECTS_DXR rects. All
	// chunks accumulate into the same window-sized woven output on the runtime's
	// single monotonic weave fence, so waiting the LAST chunk's value covers all.
	const bool useFirstChunk = g_weaveSpecVersion >= 5;
	XrRect2Di xrRects[XR_WEAVE_SUBMIT_MAX_RECTS_DXR];
	size_t done = 0;
	uint64_t fenceValue = 0;
	double totalMs = 0.0;
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);

	while (done < rects.size()) {
		const size_t n = std::min(rects.size() - done, (size_t)XR_WEAVE_SUBMIT_MAX_RECTS_DXR);
		for (size_t i = 0; i < n; i++) {
			const ClampedRect &r = rects[done + i];
			xrRects[i].offset.x = r.x;
			xrRects[i].offset.y = r.y;
			xrRects[i].extent.width = r.w;
			xrRects[i].extent.height = r.h;
		}

		XrWeaveSubmitRectsDXR batch = {XR_TYPE_WEAVE_SUBMIT_RECTS_DXR};
		batch.rectCount = (uint32_t)n;
		batch.rects = xrRects;

		XrWeaveSubmitInfoDXR in = {XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
		in.next = &batch; // the chain switches the input-layout contract
		in.inputTexture = (void *)sbs_handle_;
		in.inputIsDxgi = XR_FALSE;
		// in.rect is ignored under the batch chain; leave it zeroed.
		// firstChunk (v5): clear the woven output to premultiplied transparent on
		// the FIRST submit of the frame only — later chunks accumulate into it.
		in.firstChunk = (useFirstChunk && done == 0) ? XR_TRUE : XR_FALSE;

		XrWeaveOutputDXR out = {XR_TYPE_WEAVE_OUTPUT_DXR};
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		XrResult sr = g_pfnWeaveSubmit(session, &in, &out);
		QueryPerformanceCounter(&t1);
		if (XR_FAILED(sr)) {
			LogXrResult("xrWeaveSubmitDXR(batch)", sr);
			return 0;
		}
		totalMs += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;

		StoreEyes(out, bridge);
		// Handles arrive on the first submit and on re-allocation (resize).
		if (!have_handback_ || out.weavedTexture != nullptr) {
			if (!OpenHandback(out)) {
				return 0;
			}
			have_handback_ = (weaved_ != nullptr);
		}
		fenceValue = out.fenceValue;
		done += n;
	}
	if (outMs) {
		*outMs = totalMs;
	}
	if (!weaved_ || !weaved_srv_) {
		return 0;
	}

	// GPU-wait the whole frame's weave once, then capture one SRV-able copy.
	if (fence_) {
		context_->Wait(fence_.Get(), fenceValue);
	}
	ID3D11ShaderResourceView *nullSrv[1] = {nullptr};
	context_->PSSetShaderResources(0, 1, nullSrv); // unbind: copy writes the SRV's resource
	context_->CopyResource(weaved_srv_tex_.Get(), weaved_.Get());

	const float wsw = weaved_w_ ? (float)weaved_w_ : (float)winW;
	const float wsh = weaved_h_ ? (float)weaved_h_ : (float)winH;

	if (useFirstChunk) {
		// v5: the gaps between the woven tiles were cleared to premultiplied
		// transparent, so ONE "over" blit of the whole woven output draws every
		// tile at once and lets the page show through everywhere else.
		float bf[4] = {1, 1, 1, 1};
		context_->OMSetBlendState(blend_over_.Get(), bf, 0xffffffff);
		BlitQuad(weaved_srv_.Get(), 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, (float)winW / wsw, (float)winH / wsh,
		         ps_over_.Get());
		context_->OMSetBlendState(blend_.Get(), bf, 0xffffffff);
	} else {
		// Spec v3/v4: no clear, so only the tiles themselves are meaningful —
		// draw each one back opaquely over its own rect.
		for (const ClampedRect &r : rects) {
			BlitQuad(weaved_srv_.Get(), (float)r.x / (float)winW, (float)r.y / (float)winH,
			         (float)r.w / (float)winW, (float)r.h / (float)winH, (float)r.x / wsw, (float)r.y / wsh,
			         (float)r.w / wsw, (float)r.h / wsh, nullptr);
		}
	}
	return (uint32_t)rects.size();
}

// ---------------------------------------------------------------------------
// Legacy per-element submit — XR_DXR_weave spec < 3 fallback.
// ---------------------------------------------------------------------------
uint32_t
WeaveCompositor::CompositeLegacy(XrSession session,
                                 const std::vector<ClampedRect> &rects,
                                 uint32_t winW,
                                 uint32_t winH,
                                 HostBridge &bridge,
                                 double *outMs)
{
	// Serialized per element (submit -> wait -> copy -> blit) so it is robust
	// whether or not the DP preserves prior sub-rects across submits into the
	// same window-sized output texture — each element's woven sub-rect is
	// captured before the next submit can overwrite the shared output.
	double totalMs = 0.0;
	uint32_t wove = 0;
	ID3D11ShaderResourceView *nullSrv[1] = {nullptr};
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);

	for (const ClampedRect &r : rects) {
		// Here the input is ELEMENT-sized: the whole texture is one 2x1 SBS
		// atlas for the single rect (the pre-v3 contract).
		if (!EnsureSharedInput((uint32_t)r.w, (uint32_t)r.h)) {
			continue;
		}
		if (sbs_mutex_->AcquireSync(0, 1000) != S_OK) {
			continue;
		}
		D3D11_BOX box = {};
		box.left = (UINT)r.x;
		box.top = (UINT)r.y;
		box.right = (UINT)(r.x + r.w);
		box.bottom = (UINT)(r.y + r.h);
		box.back = 1;
		context_->CopySubresourceRegion(sbs_.Get(), 0, 0, 0, 0, bridge.pageTex.Get(), 0, &box);
		context_->Flush();
		sbs_mutex_->ReleaseSync(0);

		XrWeaveSubmitInfoDXR in = {XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
		in.inputTexture = (void *)sbs_handle_;
		in.inputIsDxgi = XR_FALSE;
		in.rect.offset.x = r.x;
		in.rect.offset.y = r.y;
		in.rect.extent.width = r.w;
		in.rect.extent.height = r.h;

		XrWeaveOutputDXR out = {XR_TYPE_WEAVE_OUTPUT_DXR};
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		XrResult sr = g_pfnWeaveSubmit(session, &in, &out);
		QueryPerformanceCounter(&t1);
		if (XR_FAILED(sr)) {
			LogXrResult("xrWeaveSubmitDXR", sr);
			continue;
		}
		totalMs += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;

		StoreEyes(out, bridge);
		if (!have_handback_ || out.weavedTexture != nullptr) {
			if (!OpenHandback(out)) {
				continue;
			}
			have_handback_ = (weaved_ != nullptr);
		}
		if (!weaved_ || !weaved_srv_) {
			continue;
		}

		if (fence_) {
			context_->Wait(fence_.Get(), out.fenceValue);
		}
		context_->PSSetShaderResources(0, 1, nullSrv); // unbind: copy writes the SRV's resource
		context_->CopyResource(weaved_srv_tex_.Get(), weaved_.Get());

		const float wsw = weaved_w_ ? (float)weaved_w_ : (float)winW;
		const float wsh = weaved_h_ ? (float)weaved_h_ : (float)winH;
		BlitQuad(weaved_srv_.Get(), (float)r.x / (float)winW, (float)r.y / (float)winH, (float)r.w / (float)winW,
		         (float)r.h / (float)winH, (float)r.x / wsw, (float)r.y / wsh, (float)r.w / wsw,
		         (float)r.h / wsh, nullptr);
		wove++;
	}
	if (outMs) {
		*outMs = totalMs;
	}
	return wove;
}

bool
WeaveCompositor::Frame(XrSession session,
                       IDXGISwapChain1 *swapchain,
                       uint32_t winW,
                       uint32_t winH,
                       HostBridge &bridge,
                       double *outMs)
{
	if (!bridge.pageTex || bridge.elements.empty() || winW == 0 || winH == 0) {
		return false;
	}

	// INTERSECT each element rect with the page texture — do not clamp it. An
	// element scrolled off the top reports a NEGATIVE y; clamping y to 0 while
	// keeping the height turns it into a full-size rect pinned at the top of the
	// window, so the host feeds page TEXT to the weave and draws woven text back
	// over the page (the ragged-canvas-while-scrolling artifact). A rect with no
	// visible area is dropped outright: an off-screen element must not be woven
	// at all.
	//
	// Vertical clipping is safe — the SBS split is horizontal, so a partially
	// visible element still has its left view in the left half of what remains.
	// A HORIZONTAL clip would skew the split; expressing that crop correctly is
	// the display-side work in browser#73 and is out of scope here (this demo
	// page never scrolls sideways).
	const int32_t pageW = (int32_t)bridge.pageW;
	const int32_t pageH = (int32_t)bridge.pageH;
	std::vector<ClampedRect> rects;
	rects.reserve(bridge.elements.size());
	for (const Element3D &el : bridge.elements) {
		const int32_t x0 = std::max(el.x, 0);
		const int32_t y0 = std::max(el.y, 0);
		const int32_t x1 = std::min(el.x + (int32_t)el.w, pageW);
		const int32_t y1 = std::min(el.y + (int32_t)el.h, pageH);
		if (x1 <= x0 || y1 <= y0) {
			continue; // fully off-screen — not a weave region this frame
		}
		rects.push_back({x0, y0, x1 - x0, y1 - y0});
	}
	if (rects.empty()) {
		return false;
	}

	// Back buffer + page SRV + render state — set up once; the page base and all
	// woven element sub-rects composite into the same back buffer.
	ComPtr<ID3D11Texture2D> back;
	if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
		return false;
	}
	ComPtr<ID3D11RenderTargetView> rtv;
	if (FAILED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv))) {
		return false;
	}
	ComPtr<ID3D11ShaderResourceView> pageSrv;
	if (FAILED(device_->CreateShaderResourceView(bridge.pageTex.Get(), nullptr, &pageSrv))) {
		return false;
	}
	back_rtv_ = rtv;
	ID3D11RenderTargetView *rtvs[1] = {rtv.Get()};
	context_->OMSetRenderTargets(1, rtvs, nullptr);
	float bf[4] = {1, 1, 1, 1};
	context_->OMSetBlendState(blend_.Get(), bf, 0xffffffff);
	D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)winW, (float)winH, 0.0f, 1.0f};
	context_->RSSetViewports(1, &vp);
	ID3D11ShaderResourceView *nullSrv[1] = {nullptr};

	// Page base (full window): the 2D surround + each element's flat SBS pixels,
	// the latter replaced by the woven 3D below.
	BlitQuad(pageSrv.Get(), 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, nullptr);

	// The RUNTIME's reported spec version picks the submit path — never the
	// vendored header's (see xr_session.h). The v6 N-view atlas additionally
	// needs the active mode's geometry, and needs its view count to be one the
	// page can actually fill: the demo renders a squeezed SBS pair per element,
	// so viewCount must be 2 (the same "no view synthesis exists" limit every
	// 2-view DisplayXR client has). Anything else falls back to the v3 batch.
	//
	// The mode is queried ONCE, by main at startup (SetModeInfo). A mid-session
	// mode change (the V toggle) would need a re-query on
	// XrEventDataRenderingModeChangedDXR; this host does not track that, which
	// is the one thing to add if you copy this path.
	const bool useNView = g_weaveSpecVersion >= 6 && mode_.valid && mode_.viewCount == 2 &&
	                      mode_.viewCount == mode_.tileColumns * mode_.tileRows;

	static bool logged_path = false;
	if (!logged_path) {
		logged_path = true;
		LOG_INFO("weave submit path: %s (runtime XR_DXR_weave spec v%u)",
		         useNView ? "v6 N-view worst-case atlas (host-packed, whole-window draw-back)"
		         : g_weaveSpecVersion >= 3
		             ? (g_weaveSpecVersion >= 5 ? "v3 batched + v5 firstChunk (whole-window draw-back)"
		                                        : "v3 batched (per-tile draw-back)")
		             : "legacy per-element",
		         g_weaveSpecVersion);
	}

	uint32_t wove = 0;
	if (useNView) {
		wove = CompositeNView(session, rects, winW, winH, bridge, outMs);
		if (wove == 0) {
			// One-way latch: if the atlas path can't run (bad geometry), drop to
			// the batch rather than presenting nothing every frame.
			static bool warned = false;
			if (!warned) {
				warned = true;
				LOG_ERROR("v6 atlas submit failed — falling back to the v3 batch path");
			}
			mode_.valid = false;
		}
	}
	if (wove == 0) {
		wove = (g_weaveSpecVersion >= 3) ? CompositeBatched(session, rects, winW, winH, bridge, outMs)
		                                 : CompositeLegacy(session, rects, winW, winH, bridge, outMs);
	}

	// Unbind the SRV (defensive; the targets are released next frame anyway).
	context_->PSSetShaderResources(0, 1, nullSrv);
	return wove > 0;
}
