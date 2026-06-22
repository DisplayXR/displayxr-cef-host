// Copyright 2026, DisplayXR — SPDX-License-Identifier: BSL-1.0
// Demo page logic for the CEF OSR weave host (#625, Step A — multi-element).
//
// Renders EACH `.element3d` canvas as a side-by-side stereo pair (left half =
// left eye, right half = right eye) using off-axis asymmetric-frustum (Kooima)
// projection driven by the eyes the host feeds back. Reports ALL elements'
// committed device-pixel rects to the host each frame ("rects <n> ..."), and
// subscribes to the host's tracked eyes (one viewer, shared by every element).
//
// The host extracts each canvas's device-pixel rect from CEF's composited-page
// texture as that element's PRE-WEAVE SBS input, drives the real weave RPC per
// element, and composites each woven result back over its own rect — so the flat
// SBS each canvas draws is replaced by true interlaced 3D on the display. Mixed
// 2D/3D is therefore DOM-driven: add a `.element3d` → it's 3D; remove it → 2D.

(function () {
  "use strict";

  // ---- tiny column-major mat4 ------------------------------------------------
  const M = {
    ident: () => new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]),
    mul: (a, b) => {
      const o = new Float32Array(16);
      for (let c = 0; c < 4; c++)
        for (let r = 0; r < 4; r++)
          o[c*4+r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1] + a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
      return o;
    },
    translate: (x, y, z) => new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1]),
    scale: (s) => new Float32Array([s,0,0,0, 0,s,0,0, 0,0,s,0, 0,0,0,1]),
    rotY: (a) => { const c=Math.cos(a), s=Math.sin(a); return new Float32Array([c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1]); },
    rotX: (a) => { const c=Math.cos(a), s=Math.sin(a); return new Float32Array([1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1]); },
    // Off-axis frustum: screen rect [-W/2,W/2]x[-H/2,H/2] at world z=0; eye at
    // (ex,ey,ez), ez>0 in front. Look-around + stereo emerge from ex/ey/ez.
    frustumForEye: (ex, ey, ez, W, H, n, f) => {
      const l = (-W/2 - ex) * n / ez, r = ( W/2 - ex) * n / ez;
      const b = (-H/2 - ey) * n / ez, t = ( H/2 - ey) * n / ez;
      const proj = new Float32Array(16);
      proj[0] = 2*n/(r-l); proj[5] = 2*n/(t-b);
      proj[8] = (r+l)/(r-l); proj[9] = (t+b)/(t-b);
      proj[10] = -(f+n)/(f-n); proj[11] = -1;
      proj[14] = -2*f*n/(f-n);
      // View: camera at the eye, axis-aligned (screen normal = +z).
      return M.mul(proj, M.translate(-ex, -ey, -ez));
    }
  };

  // ---- cube geometry (pos + normal) -----------------------------------------
  function cube() {
    const p = [], nrm = [];
    const faces = [
      [[ 1,0,0],[[1,-1,-1],[1,1,-1],[1,1,1],[1,-1,1]]],
      [[-1,0,0],[[-1,-1,1],[-1,1,1],[-1,1,-1],[-1,-1,-1]]],
      [[0, 1,0],[[-1,1,-1],[-1,1,1],[1,1,1],[1,1,-1]]],
      [[0,-1,0],[[-1,-1,1],[-1,-1,-1],[1,-1,-1],[1,-1,1]]],
      [[0,0, 1],[[-1,-1,1],[1,-1,1],[1,1,1],[-1,1,1]]],
      [[0,0,-1],[[1,-1,-1],[-1,-1,-1],[-1,1,-1],[1,1,-1]]],
    ];
    for (const [nv, vs] of faces) {
      const idx = [0,1,2, 0,2,3];
      for (const i of idx) { p.push(...vs[i]); nrm.push(...nv); }
    }
    return { pos: new Float32Array(p), nrm: new Float32Array(nrm), count: p.length/3 };
  }

  const VS = `attribute vec3 aPos; attribute vec3 aNrm; uniform mat4 uMVP; uniform mat4 uModel;
    varying vec3 vN; void main(){ vN = mat3(uModel)*aNrm; gl_Position = uMVP*vec4(aPos,1.0); }`;
  const FS = `precision mediump float; varying vec3 vN; uniform vec3 uColor;
    void main(){ float d = max(dot(normalize(vN), normalize(vec3(0.4,0.7,0.6))),0.0);
      gl_FragColor = vec4(uColor*(0.35+0.65*d), 1.0); }`;

  function sh(gl, type, src) { const s=gl.createShader(type); gl.shaderSource(s,src); gl.compileShader(s);
    if(!gl.getShaderParameter(s,gl.COMPILE_STATUS)) console.error(gl.getShaderInfoLog(s)); return s; }

  // ---- shared state ----------------------------------------------------------
  const hud = document.getElementById("hud");
  const dpr = () => window.devicePixelRatio || 1;
  // Element physical size (notional, metres) for the Kooima screen rect.
  const SCREEN_W = 0.30;
  // Eyes (display-space metres). Default: 63 mm IPD, 0.5 m out, until tracking.
  // ONE viewer — the same eyes drive every element's off-axis projection.
  let eyes = [ {x:-0.0315,y:0,z:0.5}, {x:0.0315,y:0,z:0.5} ];
  let eyesInfo = { valid:false, tracking:false };

  // Per-element scene presets — visually distinct depths so multi-element 3D is
  // unmistakable. Cycled across the `.element3d` canvases in DOM order.
  const SCENES = [
    [ // 3 depth-staggered cubes (front / at-plane / behind)
      { p:[-0.06, 0.0,  0.045], c:[0.92,0.30,0.25], s:0.028 },
      { p:[ 0.0,  0.0,  0.0  ], c:[0.30,0.85,0.40], s:0.030 },
      { p:[ 0.07, 0.0, -0.06 ], c:[0.35,0.55,0.95], s:0.034 },
    ],
    [ // a single cube popping far out IN FRONT of the screen
      { p:[ 0.0,  0.0,  0.09 ], c:[0.95,0.75,0.20], s:0.050 },
    ],
    [ // two cubes, one deep BEHIND the screen plane
      { p:[-0.05, 0.0, -0.10 ], c:[0.65,0.40,0.95], s:0.040 },
      { p:[ 0.05, 0.02, 0.02 ], c:[0.20,0.85,0.85], s:0.030 },
    ],
  ];

  // ---- per-canvas renderer ---------------------------------------------------
  function makeRenderer(canvas, cubes) {
    const gl = canvas.getContext("webgl", { alpha:false, antialias:true, preserveDrawingBuffer:true });
    if (!gl) return null;
    const prog = gl.createProgram();
    gl.attachShader(prog, sh(gl, gl.VERTEX_SHADER, VS));
    gl.attachShader(prog, sh(gl, gl.FRAGMENT_SHADER, FS));
    gl.linkProgram(prog); gl.useProgram(prog);
    const aPos = gl.getAttribLocation(prog,"aPos"), aNrm = gl.getAttribLocation(prog,"aNrm");
    const uMVP = gl.getUniformLocation(prog,"uMVP"), uModel = gl.getUniformLocation(prog,"uModel"),
          uColor = gl.getUniformLocation(prog,"uColor");
    const geo = cube();
    const pb = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, pb); gl.bufferData(gl.ARRAY_BUFFER, geo.pos, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(aPos); gl.vertexAttribPointer(aPos,3,gl.FLOAT,false,0,0);
    const nb = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, nb); gl.bufferData(gl.ARRAY_BUFFER, geo.nrm, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(aNrm); gl.vertexAttribPointer(aNrm,3,gl.FLOAT,false,0,0);
    gl.enable(gl.DEPTH_TEST);

    function drawEye(eye, vx, vy, vw, vh, W, H, t) {
      gl.viewport(vx, vy, vw, vh);
      gl.enable(gl.SCISSOR_TEST); gl.scissor(vx, vy, vw, vh);
      gl.clearColor(0.06,0.07,0.09,1.0); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
      const VP = M.frustumForEye(eye.x, eye.y, eye.z, W, H, 0.01, 10.0);
      for (const cu of cubes) {
        let model = M.mul(M.translate(cu.p[0], cu.p[1], cu.p[2]), M.mul(M.rotY(t*0.6), M.mul(M.rotX(t*0.4), M.scale(cu.s))));
        gl.uniformMatrix4fv(uMVP, false, M.mul(VP, model));
        gl.uniformMatrix4fv(uModel, false, model);
        gl.uniform3fv(uColor, cu.c);
        gl.drawArrays(gl.TRIANGLES, 0, geo.count);
      }
      gl.disable(gl.SCISSOR_TEST);
    }

    // Draw this element's SBS pair and return its committed device-px rect
    // (window-relative, y-down) for the host to weave.
    return function drawAndRect(t) {
      const rect = canvas.getBoundingClientRect();
      const scale = dpr();
      const dw = Math.max(2, Math.round(rect.width * scale));
      const dh = Math.max(2, Math.round(rect.height * scale));
      if (canvas.width !== dw || canvas.height !== dh) { canvas.width = dw; canvas.height = dh; }
      const H = SCREEN_W * (dh / dw);
      const halfW = (dw / 2) | 0;
      drawEye(eyes[0], 0,     0, halfW,      dh, SCREEN_W, H, t);
      drawEye(eyes[1], halfW, 0, dw - halfW, dh, SCREEN_W, H, t);
      return { x: Math.round(rect.left * scale), y: Math.round(rect.top * scale), w: dw, h: dh };
    };
  }

  // ---- discover elements + build renderers -----------------------------------
  const canvases = Array.prototype.slice.call(document.querySelectorAll(".element3d"));
  const renderers = canvases
    .map((c, i) => makeRenderer(c, SCENES[i % SCENES.length]))
    .filter(Boolean);
  if (!renderers.length) { hud.textContent = "WebGL unavailable"; return; }

  function render(tms) {
    const t = tms * 0.001;

    // Draw every element + collect its committed rect.
    const rects = [];
    for (const draw of renderers) rects.push(draw(t));

    // Report ALL element rects to the host in one message (full list each frame).
    if (window.cefQuery) {
      let msg = "rects " + rects.length;
      for (const r of rects) msg += " " + r.x + " " + r.y + " " + r.w + " " + r.h;
      window.cefQuery({ request: msg, persistent: false, onSuccess: function(){}, onFailure: function(){} });
    }

    hud.textContent =
      renderers.length + " 3D element(s)  dpr " + dpr().toFixed(2) +
      "\neyes valid=" + eyesInfo.valid + " track=" + eyesInfo.tracking +
      "  L.x=" + eyes[0].x.toFixed(4) + " R.x=" + eyes[1].x.toFixed(4) + " z=" + eyes[0].z.toFixed(3);

    requestAnimationFrame(render);
  }

  // Subscribe to the host's tracked eyes (persistent query; host pushes each frame).
  function subscribeEyes() {
    if (!window.cefQuery) return;
    window.cefQuery({
      request: "subscribe-eyes",
      persistent: true,
      onSuccess: function (resp) {
        try {
          const e = JSON.parse(resp);
          eyesInfo.valid = !!e.valid; eyesInfo.tracking = !!e.tracking;
          // Only adopt the runtime's eyes once tracking is LIVE — the untracked
          // fallback collapses both eyes to centre (zero disparity). Until then
          // keep the default-IPD pair so baseline 3D is visible without a face.
          if (e.valid && e.tracking && e.eyes && e.eyes.length >= 2 && e.eyes[0].z > 0.01) {
            eyes = [ {x:e.eyes[0].x, y:e.eyes[0].y, z:e.eyes[0].z},
                     {x:e.eyes[1].x, y:e.eyes[1].y, z:e.eyes[1].z} ];
          }
        } catch (err) { /* keep last good eyes */ }
      },
      onFailure: function () {}
    });
  }

  subscribeEyes();
  requestAnimationFrame(render);
})();
