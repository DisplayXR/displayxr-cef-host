// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Weave compositing implementation for the CEF OSR host (#625, Step A).
 */

#include "weave_compositor.h"
#include "xr_session.h" // g_pfnWeaveSubmit
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
float4 PSMain(VSOut i) : SV_TARGET { return float4(gTex.Sample(gSamp, i.uv).rgb, 1.0); }
)";

bool
WeaveCompositor::Init(ID3D11Device5 *dev, ID3D11DeviceContext4 *ctx)
{
	device_ = dev;
	context_ = ctx;
	return EnsureBlitPipeline();
}

bool
WeaveCompositor::EnsureBlitPipeline()
{
	if (vs_ && ps_) {
		return true;
	}
	ComPtr<ID3DBlob> vsb, psb, err;
	UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "blit", nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &vsb,
	                      &err))) {
		LOG_ERROR("blit VS compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
		return false;
	}
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "blit", nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &psb,
	                      &err))) {
		LOG_ERROR("blit PS compile failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
		return false;
	}
	if (FAILED(device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) ||
	    FAILED(device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_))) {
		LOG_ERROR("blit shader create failed");
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

	D3D11_BLEND_DESC blend = {}; // opaque overwrite (PS already forces alpha = 1)
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	device_->CreateBlendState(&blend, &blend_);
	return true;
}

bool
WeaveCompositor::EnsureSbsInput(uint32_t w, uint32_t h)
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
WeaveCompositor::OpenHandback(const XrWeaveOutputEXT &out)
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
                          float srcH)
{
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(context_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		return;
	}
	BlitCB cb = {{dstX, dstY, dstW, dstH}, {srcU, srcV, srcW, srcH}};
	memcpy(m.pData, &cb, sizeof(cb));
	context_->Unmap(cb_.Get(), 0);

	context_->VSSetShader(vs_.Get(), nullptr, 0);
	context_->PSSetShader(ps_.Get(), nullptr, 0);
	context_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
	ID3D11ShaderResourceView *srvs[1] = {srv};
	context_->PSSetShaderResources(0, 1, srvs);
	context_->PSSetSamplers(0, 1, samp_.GetAddressOf());
	context_->IASetInputLayout(nullptr);
	context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context_->Draw(4, 0);
}

bool
WeaveCompositor::Frame(XrSession session,
                       IDXGISwapChain1 *swapchain,
                       uint32_t winW,
                       uint32_t winH,
                       HostBridge &bridge,
                       double *outMs)
{
	if (!bridge.pageTex || !bridge.haveRect || winW == 0 || winH == 0) {
		return false;
	}

	// Clamp the element rect to the page texture + window.
	int32_t rx = bridge.rectX, ry = bridge.rectY;
	int32_t rw = (int32_t)bridge.rectW, rh = (int32_t)bridge.rectH;
	rx = std::max(0, std::min(rx, (int32_t)bridge.pageW - 1));
	ry = std::max(0, std::min(ry, (int32_t)bridge.pageH - 1));
	rw = std::max(1, std::min(rw, (int32_t)bridge.pageW - rx));
	rh = std::max(1, std::min(rh, (int32_t)bridge.pageH - ry));

	if (!EnsureSbsInput((uint32_t)rw, (uint32_t)rh)) {
		return false;
	}

	// Extract the element sub-rect (the pre-weave SBS pair) into the keyed-mutex
	// input texture. key 0 = "caller done writing, runtime may read" — release
	// BEFORE xrWeaveSubmitEXT or the service's same-key acquire would block.
	if (sbs_mutex_->AcquireSync(0, 1000) != S_OK) {
		return false;
	}
	D3D11_BOX box = {};
	box.left = (UINT)rx;
	box.top = (UINT)ry;
	box.front = 0;
	box.right = (UINT)(rx + rw);
	box.bottom = (UINT)(ry + rh);
	box.back = 1;
	context_->CopySubresourceRegion(sbs_.Get(), 0, 0, 0, 0, bridge.pageTex.Get(), 0, &box);
	context_->Flush();
	sbs_mutex_->ReleaseSync(0);

	// Drive the weave RPC.
	XrWeaveSubmitInfoEXT in = {XR_TYPE_WEAVE_SUBMIT_INFO_EXT};
	in.inputTexture = (void *)sbs_handle_;
	in.inputIsDxgi = XR_FALSE;
	in.rect.offset.x = rx;
	in.rect.offset.y = ry;
	in.rect.extent.width = rw;
	in.rect.extent.height = rh;

	XrWeaveOutputEXT out = {XR_TYPE_WEAVE_OUTPUT_EXT};
	LARGE_INTEGER f, t0, t1;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	XrResult sr = g_pfnWeaveSubmit(session, &in, &out);
	QueryPerformanceCounter(&t1);
	if (XR_FAILED(sr)) {
		LogXrResult("xrWeaveSubmitEXT", sr);
		return false;
	}
	if (outMs) {
		*outMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
	}

	// Returned eyes -> bridge (delivered to the page for off-axis render).
	if (out.eyesValid == XR_TRUE && out.eyeCount > 0) {
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

	// Open the weaved handback on the first frame / on re-allocation.
	if (!have_handback_ || out.weavedTexture != nullptr) {
		if (!OpenHandback(out)) {
			return false;
		}
		have_handback_ = true;
	}
	if (!weaved_ || !weaved_srv_) {
		return false;
	}

	// GPU-wait the runtime's weave-complete signal, then make an SRV-able copy.
	if (fence_) {
		context_->Wait(fence_.Get(), out.fenceValue);
	}
	context_->CopyResource(weaved_srv_tex_.Get(), weaved_.Get());

	// Composite into the swap-chain back buffer: page (base) + weaved sub-rect.
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

	ID3D11RenderTargetView *rtvs[1] = {rtv.Get()};
	context_->OMSetRenderTargets(1, rtvs, nullptr);
	float bf[4] = {1, 1, 1, 1};
	context_->OMSetBlendState(blend_.Get(), bf, 0xffffffff);
	D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)winW, (float)winH, 0.0f, 1.0f};
	context_->RSSetViewports(1, &vp);

	// Page base (full window).
	BlitQuad(pageSrv.Get(), 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);

	// Weaved sub-rect over the element rect (replaces the flat canvas).
	float dstx = (float)rx / (float)winW;
	float dsty = (float)ry / (float)winH;
	float dstw = (float)rw / (float)winW;
	float dsth = (float)rh / (float)winH;
	float wsw = weaved_w_ ? (float)weaved_w_ : (float)winW;
	float wsh = weaved_h_ ? (float)weaved_h_ : (float)winH;
	BlitQuad(weaved_srv_.Get(), dstx, dsty, dstw, dsth, (float)rx / wsw, (float)ry / wsh, (float)rw / wsw,
	         (float)rh / wsh);

	// Unbind the SRV (defensive; the targets are released next frame anyway).
	ID3D11ShaderResourceView *nullSrv[1] = {nullptr};
	context_->PSSetShaderResources(0, 1, nullSrv);
	return true;
}
