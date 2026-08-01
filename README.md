# displayxr-cef-host — minimal `XR_DXR_weave` reference host

A small **CEF offscreen-render (OSR)** application that drives the **real**
DisplayXR display-processor weave through the shipped `XR_DXR_weave` RPC. The
host **never weaves**; the display processor does, inside the runtime
(ADR-007 / ADR-019). Vendor-neutral throughout.

> **This is not a browser you should use.** That is
> [`displayxr-browser`](https://github.com/DisplayXR/displayxr-browser) — a real
> Chromium fork that weaves inline 3D on any page. This repo is a *reference and
> a test tool*, and it is deliberately kept small.

## Why it still exists

It began as Step A of the inline-3D-in-the-browser roadmap (issue #625): prove
the full weave round-trip and phase/position exactness under scroll / zoom /
window-drag on a Chromium-faithful engine, before committing to a Chromium fork.
That milestone shipped and was hardware-validated on Leia, and the browser
superseded it as the *product* path. Two jobs outlived it:

- **A fast reproducer for weave/DP bugs.** It builds in minutes. The browser
  fork is a 1–3 hour build on a dedicated 64-vCPU box, so when the weave
  extension or a display processor regresses, this is where you bisect it —
  not in Chromium.
- **The integration example for third parties.** It shows how to drive the weave
  from your own present-owner without forking Chromium: hand the runtime a
  side-by-side texture and a window rect, composite the result you get back.
  The browser can't serve that purpose, because it *is* the fork.

Expect maintenance, not features. If it stops building against a current runtime,
that is a bug worth fixing — a reproducer that doesn't run is worth nothing.

Structurally it is the runtime's `weave_rpc_probe_d3d11_win` present-owner
skeleton with a real Chromium engine (CEF) as the content source instead of a
synthetic side-by-side painter.

## How it works

```
demo page (WebGL SBS)        CEF (OSR)              host (this exe)
 render 3D element SBS  ───▶  composite page  ───▶  OnAcceleratedPaint(shared D3D11 tex)
 post device-px rect    ──cefQuery(rect)──────────▶ extract element sub-rect → SBS input
 (re-render off-axis)   ◀──cefQuery(eyes)────────── xrWeaveSubmitDXR(SBS, rect)
                                                     → weaved tex + fence + eyes
                                                     composite weaved sub-rect over page; present
```

The page renders the 3D element side-by-side (left half = left eye, right half =
right eye) with an off-axis Kooima projection driven by the eyes the host feeds
back. The host extracts that element's committed device-pixel rect as the
pre-weave input, weaves it via the RPC, and composites the woven result back over
the same rect — replacing the flat canvas with true interlaced 3D, at correct
z-order, since CEF/the host own the present.

## Build (Windows)

```bat
scripts\setup-deps.bat   :: one-time: download CEF + the OpenXR loader to C:\dev\...
scripts\build.bat        :: configure + build (Ninja, Release, static CRT /MT)
```

Requires Visual Studio 2022 (C++ workload) + Ninja. Output (exe + CEF payload +
`web/`) lands under `build/` (CEF target output dir).

## Run (on the 3D display)

1. Ensure the DisplayXR runtime is installed/registered with `XR_DXR_weave`
   support and `displayxr-cli selftest` passes; start `displayxr-service` if it
   isn't running (it's the orchestrator — don't leave it down).
2. The weave service is **IPC-only** — run forced-IPC: set `XRT_FORCE_MODE=ipc`
   **process-level** (env, not inside a `.bat`), then launch
   `build\...\displayxr_cef_host.exe` at the **same integrity as the
   (non-elevated) service** (launch via `explorer.exe` for medium integrity).

## Validation (Step A targets)

- Round-trip: the 3D element shows correct stereo / look-around, composited in
  the page with the 2D surround intact.
- Phase/position: scroll, zoom (100% + fractional), and drag/resize the window —
  the lattice stays locked (no banding drift / 3D collapse).
- Capture: `touch %TEMP%\weave_host_trigger` → `%TEMP%\weave_host_output.bmp`
  (the composited back buffer). Eyeball the live display for final correctness.
- Latency: the per-frame weave round-trip is logged
  (`%LOCALAPPDATA%\DisplayXR\...cef_weave_host...log`).

## Dependencies (not vendored)

- **CEF** binary distribution, windows64 standard build (pinned in
  `scripts/setup-deps.bat`). Fetched to `C:\dev\cef\<ver>`.
- **OpenXR loader** (matches the runtime's pin). Fetched to `C:\dev\openxr_sdk_*`.
- **displayxr-common** (`XrSessionManager`, logging, Kooima math) via
  FetchContent.
- DisplayXR OpenXR extension headers are **vendored** under
  `third_party/displayxr_openxr_includes/` (synced from the runtime's
  `src/external/openxr_includes`).
