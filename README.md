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
That milestone shipped and was validated on real 3D-display hardware, and the browser
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
 post device-px rects   ──cefQuery(rects)─────────▶ pack every element into ONE input atlas
 (re-render off-axis)   ◀──cefQuery(eyes)────────── xrWeaveSubmitDXR(atlas, rects)  ← one call/frame
                                                     → weaved tex + fence + eyes
                                                     composite the woven output; present
```

The page renders each 3D element side-by-side (left half = left eye, right half =
right eye) with an off-axis Kooima projection driven by the eyes the host feeds
back. The host packs every visible element's committed device-pixel rect into a
single pre-weave input, weaves them all in **one** `xrWeaveSubmitDXR` per frame,
and composites the woven output back — replacing the flat canvases with true
interlaced 3D, at correct z-order, since CEF/the host own the present.

## Spec coverage — which `XR_DXR_weave` version this host drives

The submit path is chosen from the spec version **the runtime reports**, never
from the vendored header's, so a newer header never forces a newer runtime. The
host negotiates *down* through this ladder:

| Runtime spec | Path | Input layout |
|---|---|---|
| **≥ 6** (preferred) | `XrWeaveSubmitLayoutDXR` N-view atlas | Worst-case atlas sized from the **display**; the host packs tile *v* itself at `contentView{Width,Height}` = window × the active mode's `viewScale`. |
| 3 – 5 | `XrWeaveSubmitRectsDXR` batch | Window-sized input, each rect's squeezed SBS at its own window position. Sets `firstChunk` on spec ≥ 5. |
| < 3 | legacy single-rect | One submit per element, each with its own element-sized SBS atlas. |

**v6 is the path to copy.** It makes the host an ordinary N-view DisplayXR
client (ADR-010 worst-case swapchain + ADR-030 crop-before-DP): the host owns the
atlas *and its alpha*, so the transparency between elements is the host's own —
no dependence on a runtime-side clear, and the woven output draws back
whole-window in one premultiplied "over" blit. It requires `XR_DXR_display_info`
(for the active mode's `viewCount` / `viewScale` / tile grid) and an active mode
with `viewCount == 2`, since the page renders a squeezed SBS *pair* per element
and no view synthesis exists anywhere. Anything else falls back a rung.

Deliberately **not** implemented, to keep a minimal example minimal:

- **v4 `XrWeaveSubmitOverlaysDXR`** (DP-composited 2D overlay atlas). This demo
  paints no crisp 2D *over* the woven 3D — the page's 2D surround sits around
  and under the elements, which the page base already handles. Adopting v4 would
  mean inventing a second window-sized premultiplied-RGBA atlas producer and a
  stacking-order rasteriser that exist only to exercise the field. See
  `displayxr-browser` patch 0046, which needs it for real.
- **v6 beyond `viewCount == 2`.** Filling a 4-view (2×2 quad) mode would require
  the *page* to render 4 views, which is a demo-page rewrite, not a host change.

`DXR_WEAVE_MAX_SPEC=<n>` caps the negotiated version (it never raises it), so a
single build reproduces every submit shape the extension has shipped against a
current runtime — set it to `2` to force the legacy path, `4` for the batch
without the v5 clear. That is the knob to reach for when bisecting "is this bug
batch-only?".

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

## Verified against

Last hardware run of this tree, on a real 3D display with a vendor display
processor (not `sim_display`):

| | |
|---|---|
| Runtime | `v2.5.0-3-g7262f3bc1` (reports `XR_DXR_weave` **spec v6**) |
| Display processor | vendor plug-in v2.0.7, plug-in ABI v5 |
| Active rendering mode | 2 views, 2×1 tile grid, viewScale 0.5 × 0.5 |
| CEF | 149.0.4 (Chromium 149.0.7827.156) |
| displayxr-common | v2.6.1 |
| Result | v6 N-view atlas path, one submit/frame, 3 elements, ~1.4 ms round-trip; 3D correct at rest and under scroll |

For reference, the same tree on the v3 batch path measured ~1.26 ms/frame versus
~2.6 ms/frame for the legacy per-element path with the same 3 elements — the
per-call fixed cost (IPC round-trip + shared-texture open + keyed-mutex + fence)
is what batching amortizes.

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
  FetchContent, pinned in `CMakeLists.txt` (currently `v2.6.1`).
- DisplayXR OpenXR extension headers are **vendored** under
  `third_party/displayxr_openxr_includes/`, byte-for-byte copies of the
  runtime's canonical `src/external/openxr_includes/openxr/`. Re-sync with a
  plain copy:

  ```bash
  cp <displayxr-runtime>/src/external/openxr_includes/openxr/* \
     third_party/displayxr_openxr_includes/openxr/
  ```

  The `header-sync` CI job diffs them against `displayxr-runtime@main` and
  annotates on drift (non-blocking) — drift is what makes this host teach a
  stale API.
