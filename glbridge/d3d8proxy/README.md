# Direct D3D8 to WebGPU guest frontend

This app-local `d3d8.dll` bypasses WineD3D, `opengl32.dll`, gl4es, and
WebGL. It emits high-level D3D8 commands into the existing `v86gl.sys` 16 MiB
DMA arena. The VGL2 descriptor is only the transport envelope; command
`0xFFE0` carries a versioned D8WG batch decoded by
`../d3d8-webgpu/d3d8_executor.js`.

Implemented Maple fixed-function, lifecycle and shader-model-1.x core
(D8WG protocol v1.7):

- adapter enumeration, MapleStory v83 format probes, caps, and `CreateDevice`;
- `Clear`, `BeginScene`, `EndScene`, and `Present`;
- vertex/index-buffer `Lock`/`Unlock`, dirty-range upload, stream/index
  binding, and FVF shadowing;
- ordered mid-frame buffer updates through WebGPU staging copies, without a
  synchronous PCI round trip or an extra queue submission;
- `DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP`, and
  `DrawIndexedPrimitiveUP` for point/line/triangle list and strip topologies;
- CPU-side conversion of regular and indexed `D3DPT_TRIANGLEFAN` draws into
  WebGPU triangle-list index buffers;
- `D3DFMT_INDEX16` and `D3DFMT_INDEX32`;
- Texture/level-Surface COM objects with `LockRect`/`UnlockRect`, subrect
  uploads, managed shadow storage, `GetSurfaceLevel`, and `UpdateTexture`;
- `A8R8G8B8`, `X8R8G8B8`, `R5G6B5`, `X1R5G5B5`, `A1R5G5B5`,
  `A4R4G4B4`, `L8`, and `A8` conversion to sampled RGBA8;
- DXT1/DXT3/DXT5 CPU decode fallback, including independent mip/subrect
  uploads;
- FVF `XYZRHW` and `XYZ`, optional `NORMAL`/`PSIZE`/`DIFFUSE`/`SPECULAR`, and
  `TEX1`/`TEX2`, including the exact Maple `0x142` layout and D3D8
  texture-coordinate-size encodings;
- world/view/projection and texture transforms, including
  `MultiplyTransform` and texture-coordinate generation;
- automatic D16/D24S8 negotiation backed by WebGPU D24S8, viewport depth
  range, depth compare/write/bias, all D3D8 stencil compares/operations/masks,
  and target/depth/stencil clears;
- material state, eight directional/point/spot lights, global ambient,
  material colour sources, local viewer, specular and normalized normals;
- linear/EXP/EXP2 vertex/table fog, range fog, flat/Gouraud interpolation,
  culling, colour masks, blend factors and `D3DRS_BLENDOP`;
- texture stages 0/1, common color/alpha operations and arguments, texture
  factor, alpha test, alpha blending, point/linear sampling, wrap/mirror/clamp,
  and mip-level selection;
- WebGPU scissor application for the D3D8 viewport;
- ordered `D3DLOCK_DISCARD` buffer orphaning and `D3DLOCK_NOOVERWRITE`
  updates for dynamic VB/IB resources;
- a shared 16 MiB transient upload ring for `Draw*UP`, fan conversion, and
  ordered buffer/texture staging;
- bounded LRU caches for pipelines, samplers, uniforms, and bind groups;
- guest-side suppression of repeated render and texture-stage states;
- client-area position updates on Win32 move/size/show events, including apps
  that render only one frame;
- immediate device/window teardown notification so the host overlay is hidden
  when the program closes;
- recorded and predefined state blocks, including capture/apply/delete and
  retained COM references for texture, stream and index bindings;
- epoch-changing `Reset`, strict DEFAULT-pool/lock/surface blockers, managed
  buffer/texture shadow reconstruction, resize tracking and stale-handle
  rejection on the host;
- automatic WebGPU device-loss recovery from a canonical CPU checkpoint,
  including replacement-device resource uploads and invalidation of all
  old-device pipeline/sampler objects;
- additional swap-chain lifetime/back-buffer compatibility and `Present`
  routing to its target window;
- explicit render targets, depth surfaces, image surfaces, lockable render
  targets, `Set/GetRenderTarget`, `CopyRects` and clear/front-buffer CPU
  readback mirrors;
- `QueryInterface`, `GetDevice`, Surface `GetContainer`, failed-output
  nulling, parent references and device-owned resource list boundaries;
- v86 save/load integration that serializes a canonical D3D8 device/resource/
  state checkpoint and rebuilds the exact saved epoch before guest work resumes;
- a 64-bit per-process session namespace carried by every D8WG batch and
  verified by `HELLO`; identical numeric handles from sequential or concurrent
  XP processes cannot alias, and late teardown from an old process cannot
  destroy resources or hide the canvas owned by a newer process;
- vertex shader 1.1 and pixel shader 1.1-1.4: COM-free shader handles,
  declaration parsing, bytecode validation, translation to WGSL, constant
  register banks, state-block capture and `Reset` reconstruction;
- one main PCI submit at `Present` (extra submits occur only when the DMA arena
  fills or when a window lifecycle event must reach the host immediately).

The current v1.7 boundary remains conservative: clip planes and cube/volume
textures are not implemented, and their caps are not advertised. Within shader
model 1.x, the supported instruction set covers the common ALU, texture-
addressing and comparison opcodes; matrix macros (`m4x4` and friends) and the
bump-mapping/`texm3x3` family are deliberately rejected rather than
approximated, and more than two simultaneous texture stages remain
unsupported. Unsupported calls return `D3DERR_INVALIDCALL`. Because WebGPU has
no synchronous texture mapping API, CPU readback is exact for uploads,
`Clear` and `CopyRects`; pixels produced only by an arbitrary GPU draw are not
synchronously reflected into the guest shadow. Maple does not use that cold
readback path, but another title that depends on it needs an asynchronous
guest-stall/readback protocol before its compatibility gate can be closed.

Build an XP-compatible DLL without a C runtime dependency:

```sh
./glbridge/d3d8proxy/build.sh /private/tmp/d3d8.dll
```

Build all XP Stage 3 acceptance tests without a C runtime dependency:

```sh
./glbridge/d3d8proxy/build_stage3_tests.sh /private/tmp/d3d8-stage3-tests
```

Place the new `d3d8.dll` beside each test. The suite covers geometry,
texture/format conversion, texture-stage operations, TEX2, dynamic resources,
mip/filter state, and the rendered Maple Gr2D probe. The Gr2D pass should show
two clipped sprite panels and a title containing `PASS`.

Build the XP Stage 4 fixed-function acceptance tests:

```sh
./glbridge/d3d8proxy/build_stage4_tests.sh /private/tmp/d3d8-stage4-tests
```

This builds transform/depth, raster/stencil, lighting/material, fog, and
textured-cube tests. Run them with the current v1.7 DLL beside each executable before
testing MapleStory itself.

Build the XP Stage 5 lifecycle acceptance tests:

```sh
./glbridge/d3d8proxy/build_stage5_tests.sh /private/tmp/d3d8-stage5-tests
```

The four executables cover state blocks, repeated Reset and managed-resource
reconstruction, additional swap-chain/reset blockers, render/depth/image
surfaces, lockable render-target copy/readback, COM parent/container identity,
invalid-call outputs, 96 create/destroy cycles, eight device epochs, and final
collection of a device/bound-resource COM reference cycle. The capability audit
also enforces an honest Stage 5 boundary: core/DXT/render-target texture paths
must be advertised, while cube/volume textures, SM1.1, and more than two active
texture stages remain hidden until their implementations exist. Shader model
1.x graduated out of that hidden set in Stage 6 (see below).
Device-loss, save/load reconstruction, stale batches, and the host resource-map
stress loop are covered by `d3d8_webgpu_executor_test.js` and
`v86_network_bridge_state_test.js`. The executor regression also creates two
guest sessions with identical numeric device/resource handles and verifies that
delayed teardown from one session cannot destroy or hide the other, including
after a multi-session save/load round trip. It additionally validates scoped
color/depth/stencil rectangle clears and queue-fenced physical resource
destruction after logical D3D8 release.

Build the XP Stage 6 shader-model-1.x acceptance test:

```sh
./glbridge/d3d8proxy/build_stage6_tests.sh /private/tmp/d3d8-stage6-tests
```

Stage 6 implements D3D8 shader model 1.x: `CreateVertexShader`/
`CreatePixelShader`, `SetVertexShader` with a real shader handle (FVF tokens
still take the fixed-function path), `Set/Get{Vertex,Pixel}ShaderConstant`,
`Get{Vertex,Pixel}ShaderFunction`, `GetVertexShaderDeclaration`, and
`Delete{Vertex,Pixel}Shader`. `GetDeviceCaps` now advertises `vs_1_1` and
`ps_1_4` accordingly. Both the guest DLL and the host executor validate the
token stream against the same supported-instruction table before a shader is
created, so unsupported-but-legal D3D8 opcodes (matrix macros, bump-mapping
texture ops) and malformed bytecode are rejected up front rather than
mistranslated into wrong WGSL. Shaders survive `Reset` by being re-created
under the new device epoch from the guest's token shadow, and constant banks
are resent with them.

Host-side protocol/executor tests:

```sh
node glbridge/tests/d3d8_protocol_consistency_test.js
node --test glbridge/tests/d3d8_webgpu_executor_test.js
node glbridge/tests/d3d8_webgpu_perf_test.js
```

`d3d8_webgpu_perf_test.js` pins the stage 7 steady-state budgets against a
counting fake device: zero pipeline, bind group, buffer and texture creations
per frame, one render pass and one queue submit, no GPU readback, and uniform
upload traffic proportional to distinct uniform state rather than to draw count.
Uniform data comes from a persistent ring bound with a dynamic offset, so the
bind group depends only on the bound textures and samplers; the test also
asserts the bind group cache does not grow with uniform state, which is the
only way to catch a regression that puts the uniform back into bind group
identity.

`d3d8_webgpu_executor_test.js` carries the per-instruction numeric coverage
for the VS1.1/PS1.1-1.4 to WGSL translator (one assertion per supported
opcode, plus write masks, saturate/shift destination modifiers, source
modifiers, swizzles, and `def` constant folding), the rejection cases for
unsupported versions/opcodes/registers and truncated bytecode, and an
end-to-end real-shader draw that also proves the shader pipeline is cached
rather than rebuilt per frame.

The real-GPU validation page is
`glbridge/tests/d3d8_webgpu_browser_test.html`; serve the repository over
localhost and open it in a WebGPU-enabled browser. It reports `PASS` only
after creating both pre-transformed and XYZ/normal/texture-transform pipelines
without WebGPU validation errors.

The v1.7 guest DLL and host executor must be deployed together. The executor
rejects a different protocol minor version instead of silently skipping newer
texture or fixed-function commands.

Install the resulting DLL beside the target executable. Use a separate game
deployment profile that does not contain the custom `opengl32.dll` proxy:
both frontends currently share one mapped DMA arena and cannot produce batches
concurrently. Keep WineD3D and the OpenGL proxy in a different profile for the
fallback; both backends must not own `d3d8.dll` in the same application
directory.
