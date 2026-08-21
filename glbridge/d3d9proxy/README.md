# Direct D3D9 to WebGPU guest frontend (M5 rendering path)

This app-local `d3d9.dll` bypasses WineD3D, `opengl32.dll`, gl4es, and
WebGL, the same way `../d3d8proxy/d3d8.dll` does for D3D8. It emits D9WG
commands into the existing `v86gl.sys` 16 MiB DMA arena. The VGL2 descriptor
is only the transport envelope; command `0xFFE1` carries a versioned D9WG
batch decoded by `../d3d9-webgpu/d3d9_executor.js`. D9WG is an independent
protocol from D8WG (own opcode numbering, own resource handle namespace,
own payload shapes) — see `d3d9_protocol.h` and
`docs/d3d9-webgpu-implementation-plan.zh-CN.md` section 6.

This implements the M1-M5 rendering path (protocol, SM2 translation,
fixed-function rendering, M4 resource operations, the M4.5 caps profile, and
M5 main-world primitives). It is still not a general-purpose D3D9
implementation. Implemented:

- `IDirect3D9`/`IDirect3DDevice9` COM lifecycle, adapter enumeration, caps
  (`VertexShaderVersion`/`PixelShaderVersion` now report `(2,0)`, with
  `VS20Caps`/`PS20Caps` describing what the host translator genuinely
  handles — see `fill_caps()` for why `PS20Caps.DynamicFlowControlDepth`
  stays 0), and `CreateDevice`;
- `Reset`, `Present`, `Clear`, `BeginScene`, `EndScene`;
- vertex/index buffer create/`Lock`/`Unlock` with dirty-range upload;
- 2D textures: create, `LockRect`/`UnlockRect` with subrect upload,
  `A8R8G8B8`/`X8R8G8B8`/`R5G6B5`/`X1R5G5B5`/`A1R5G5B5`/`A4R4G4B4`/`L8`/`A8`/
  DXT1/DXT3/DXT5;
- vertex declarations (`CreateVertexDeclaration`/`SetVertexDeclaration`):
  `POSITION`/`POSITIONT`/`NORMAL`/`COLOR`/`TEXCOORD`/`PSIZE`/`BLENDWEIGHT`/
  `BLENDINDICES`/`TANGENT`/`BINORMAL`/`FOG` usages, default method,
  `FLOAT1`-`FLOAT4`/`D3DCOLOR`/`UBYTE4`/`SHORT2`/`SHORT4`/`UBYTE4N`/
  `SHORT2N`/`SHORT4N`/`USHORT2N`/`USHORT4N`/`UDEC3`/`DEC3N`/`FLOAT16_2`/
  `FLOAT16_4` types, up to 4 streams;
  `SetFVF`/`GetFVF` as a compatibility path that expands the FVF bits into
  the same element shape (`XYZ`/`XYZRHW`, `NORMAL`, `DIFFUSE`, `SPECULAR`,
  up to 8 default-size 2D `TEXn`) and sends it to the host the same way
  `CreateVertexDeclaration` does — the host never decodes raw FVF bits
  (see plan section 4.3);
- `SetStreamSource`, `SetIndices`, `SetTransform`, `SetViewport`,
  `SetRenderState`, `SetTextureStageState`, `SetTexture`;
- `DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP`,
  `DrawIndexedPrimitiveUP`, for the fixed-function `XYZ`/`XYZRHW` path and
  for programmable shaders alike;
- **M2:** `IDirect3DVertexShader9`/`IDirect3DPixelShader9`
  (`CreateVertexShader`/`CreatePixelShader`/`SetVertexShader`/
  `SetPixelShader`/`GetFunction`) for shader model 1.x–3.0 bytecode. The
  guest never interprets the bytecode: it walks the token stream to find the
  terminator, hashes it, keeps a shadow copy for `GetFunction` and Reset
  replay, and ships the raw stream. Translation to WGSL happens host-side in
  `../d3d9-webgpu/d3d9_shader_pipeline.js`;
- **M2:** all six `Set*ShaderConstant{F,I,B}` entry points and their `Get*`
  counterparts, backed by a device-side shadow of the whole register file.
  The shadow suppresses redundant sets and narrows a changed set to the
  dirty sub-range, so re-uploading a 32-register bone palette where one bone
  moved costs one `float4` on the wire;
- **M2:** `SetSamplerState` as real independent sampler state, feeding the
  host's `GPUSampler` cache rather than being recorded and ignored;
- exclusive-fullscreen behaviour a real D3D9 runtime provides and no part of
  this stack otherwise does: the display mode change, and *maintaining* the
  foreground for the device window rather than claiming it once at
  `CreateDevice`. See `maintain_fullscreen_foreground` for why one claim is not
  enough and why `SetForegroundWindow` needs help to work at all;
- a 64-bit per-process session namespace carried by every D9WG batch and
  verified by `HELLO`, and epoch-changing `Reset` with managed buffer/
  texture/cube-texture/vertex-declaration shadow reconstruction, reusing the
  exact transport/batching/handle-allocation strategy already validated by the
  D3D8 path;
- **M3:** `SetMaterial`/`SetLight`/`LightEnable` are now consumed by the host's
  fixed-function vertex stage rather than only recorded, and the whole
  `SetTextureStageState` surface drives a real multi-stage blending cascade.
  Both were caps promises `fill_caps()` had been making since M1
  (`MaxTextureBlendStages = 8`, `D3DVTXPCAPS_DIRECTIONALLIGHTS`,
  `MaxActiveLights = 8`, a large `TextureOpCaps` set) without keeping;
- **M3:** `IDirect3DCubeTexture9` (six faces per level, `LockRect`/`UnlockRect`
  per face). `GetCubeMapSurface` still fails honestly — the only thing an app
  does with a face surface is lock it, which `LockRect` covers directly, and
  handing back a surface whose `LockRect` wrote to face 0 would be worse;
- **M3:** `IDirect3DStateBlock9` — `CreateStateBlock(ALL/PIXELSTATE/
  VERTEXSTATE)`, `Apply`, `Capture`, and `BeginStateBlock`/`EndStateBlock`.
  Recording keeps an explicit Set-call mask (including same-value and
  write-then-revert calls), and captured textures, shaders, declarations and
  buffers retain their COM references until the block is released;
- **M3:** `SetScissorRect`/`GetScissorRect`, gated by
  `D3DRS_SCISSORTESTENABLE`;
- **M3 (brought forward from M4):** render targets and depth surfaces —
  `CreateRenderTarget`, `CreateDepthStencilSurface`,
  `D3DUSAGE_RENDERTARGET`/`D3DUSAGE_DEPTHSTENCIL` textures,
  `SetRenderTarget` for four MRT slots, `SetDepthStencilSurface` and both
  `Get*`, plus `StretchRect` and `ColorFill`. A 2005-era D3D9 title renders
  most of its frame into textures, so without these it has no picture at all.
  `StretchRect` handles the back buffer as source or destination (deferred to
  Present, because that is the only point where the swap chain has a view), and
  scales or converts format through a blit pass when a plain copy cannot express
  it;
- **M3:** `IDirect3DQuery9` for `OCCLUSION`/`EVENT`, answered inside the guest
  with a deliberately conservative result. The reasoning is in the comment
  above `IDirect3DQuery9`; the short version is that failing `CreateQuery`
  makes engines disable whole render branches and returning `S_FALSE` forever
  deadlocks the standard polling loop, so over-reporting visibility (which only
  costs frame time) is the least-wrong answer until the host→guest return
  channel of plan section 6.7 exists;
- **M3:** `D9WG_DUMP_SHADERS=1` writes every shader's raw token stream to
  `d3d9_dump\` beside this DLL, named by content hash, for offline replay
  through `../tests/d3d9_shader_corpus_test.js`. See the comment above
  `dump_shader_bytecode` for why a hand-written translator needs real-game
  bytecode more than it needs more hand-written tests;
- **M4:** rectangle-list `Clear`, partial `ColorFill`, scaled/converted
  `StretchRect`, and exact `GetRenderTargetData` for render-target contents
  whose complete value is known from `Clear`/`ColorFill`/known-source copy.
  Any GPU draw invalidates that CPU mirror, so arbitrary GPU-produced pixels
  still fail honestly instead of returning stale data;
- **M4.5:** `D9WG_CAPS_PROFILE=ffp` (aliases `m4.5` and `low`) exposes the
  supported fixed-function-only caps profile. `sm2`/`m5` selects the default
  shader-model-2 profile;
- **M5:** declaration-specific shader variants convert compact skinning inputs
  `UBYTE4`/`SHORT2`/`SHORT4`/`UDEC3`/`DEC3N`; sRGB reads and writes use
  compatible texture/target views; blend constants, separate-alpha blending,
  front/back stencil state, stencil reference, constant/slope depth bias, and
  per-draw scissor reset are all carried into WebGPU;
- **Warcraft III shadow fix:** projected texture division now preserves the
  sign of `q` in both fixed-function and shader-modifier paths. Clamping a
  negative `q` to positive epsilon made behind-projector UVs sample an opaque
  shadow-edge texel across whole terrain triangles, producing large black
  wedges. The shadow pass also receives its complete blend/stencil/depth state.
- **Warcraft III campaign state/cursor fix:** the guest state cache now starts
  from the real D3D9 render, texture-stage, and sampler defaults. This is
  correctness-critical because equal-value Set calls are suppressed: an
  all-zero cache discarded the first `ZENABLE=FALSE`/`ZWRITEENABLE=FALSE`
  transition and left scene depth active over UI, fog-of-war, and shadow
  overlays. Stage defaults now also preserve the first transition from stage
  1's `TEXCOORDINDEX=1` to War3's shared texcoord 0. Fixed-function BORDER
  addressing selects `D3DSAMP_BORDERCOLOR` in WGSL instead of stretching an
  opaque edge texel beyond projected shadow/fog masks. The GDI cursor capture
  fallback is now enabled by default because the browser cursor is hidden; set
  `D9WG_GDI_CURSOR=0` only for a title that intentionally draws its own cursor
  geometry.
- **Kart Rider texture-surface lifetime fix:** a surface from
  `IDirect3DTexture9::GetSurfaceLevel` is a sub-object of its texture, not a
  free-standing COM object. The texture now owns one surface per level
  (`D9TextureLevel::level_surface`), returns that same pointer from every
  `GetSurfaceLevel` for the level, forwards the surface's `AddRef`/`Release` to
  the texture's refcount, and frees the surface only in `texture_release`.
  Allocating a self-owning surface per call instead was a crash rather than a
  leak: Kart Rider uploads with `GetSurfaceLevel`, `LockRect`, `Release`,
  `UnlockRect`, dropping its surface reference while it still holds the texture
  and then using the pointer it kept — which is legal, and which the old code
  answered by freeing the object, so `UnlockRect` read a zeroed vtable and
  called through a null pointer during the loading screen.
  `../sample/d3d9_surface_lifetime_test.c` covers the sequence, plus the
  pointer identity that apps rely on to tell two level surfaces apart.
  `../d3d8proxy/d3d8_proxy.c`'s `texture_get_surface_level` still has the
  original per-call behaviour.
- **GTA San Andreas SDK-version fix:** `Direct3DCreate9` accepts both 31
  (`D3D9b_SDK_VERSION`, the DirectX 9.0b SDK) and 32 (`D3D_SDK_VERSION`, 9.0c),
  and masks off the `0x80000000` bit an app compiled with `D3D_DEBUG_INFO` sets
  to ask for the debug runtime. The real d3d9.dll does not police this. Checking
  the argument against the single value in mingw's header made the very first
  call return NULL for any 9.0b title, and San Andreas — which passes 31 — can
  only report that as a generic "unable to initialise DirectX". `../d3d8proxy`
  had already learned the identical lesson for 120 (8.0) vs 220 (8.1).
- **GTA San Andreas implicit-surface lifetime fix:** the back buffer and the
  auto depth-stencil are sub-objects of the *device*, exactly as a texture level
  surface is a sub-object of its texture. Each is now created once, cached on
  `D9Device` (`implicit_back_buffer`/`implicit_depth_stencil`), handed back by
  the same pointer from every `GetBackBuffer`/`GetDepthStencilSurface`, has its
  `AddRef`/`Release` forwarded to the device, and is freed only in device
  teardown. Allocating a self-owning surface per call meant the app's release
  destroyed it, and the trace showed the heap handing that freed block straight
  back out as the next index buffer while RenderWare still held the pointer it
  had cached during device setup — the same failure shape as the Kart Rider
  entry above, one level up. `surface_is_implicit()` identifies both from fields
  that already existed (`swap_chain`, `auto_depth_stencil`), each written by
  exactly one place. `../sample/d3d9_surface_lifetime_test.c`'s
  `verify_implicit_surfaces` covers the pointer identity and the
  survives-past-zero-references property; the test device needed
  `EnableAutoDepthStencil` before it had an auto depth-stencil to ask for.
- **Kart Rider BCn mip-tail fix (host side):** `writeTexture` now rounds a
  block-compressed copy extent up to the 4x4 block grid. WebGPU measures such a
  copy in whole blocks and a mip level's physical extent is its logical size
  rounded up, so the 2x2 and 1x1 tail of a DXT chain has to be written as a full
  block. Passing the logical size failed validation as an *uncaptured device
  error* rather than an exception, so the only symptom was the smallest mips
  sampling as garbage behind several hundred console errors.
- **Kart Rider back-buffer size fix (host side):** the swap-chain colour
  attachment is sized from what the guest asked for at `CreateDevice`/`Reset`
  (`deviceState.backBufferWidth/Height`), not from `deviceState.surface`.
  `emit_present_and_flush` fills the `PRESENT` width/height from
  `GetClientRect` so the page can place the overlay canvas; a windowed game's
  client area is shorter than its back buffer (800x587 against 800x600 here).
  Reading it as the render size made every back-buffer pass look like it
  disagreed with the auto depth target, and the mismatch path then dropped
  depth — depth testing off for the whole game, caused by a window title bar.
- **Kart Rider half-pixel fix (host side):** every vertex stage now applies the
  D3D9 half-pixel offset — `pos.x += pos.w / viewport.x`,
  `pos.y -= pos.w / viewport.y` — in both the fixed-function generator
  (`HALF_PIXEL_OFFSET_BODY`) and the DXSO translator's `o_position` fixup, the
  latter reading a `viewport` uniform now present in every translated vertex
  shader. D3D9 samples a pixel at its integer corner; WebGPU, like D3D10 and
  everything after it, samples at the centre. A title that blits UI 1:1 has
  already subtracted that half pixel itself (the "Directly Mapping Texels to
  Pixels" adjustment), so replaying its geometry unchanged lands every sample
  exactly on a texel boundary and bilinear filtering returns the mean of two
  texels. 3D art does not care — none of it is pixel-aligned — but 12px CJK
  glyphs turn to mush, which is the split Kart Rider showed: a clean track and
  an unreadable shop. Same fix as wined3d's `posFixup` and DXVK's half-pixel
  offset.

- **Kart Rider viewport fix (host side):** a D3D9 viewport *clips*; WebGPU's
  `setViewport` only maps NDC to pixels. Nothing else cut a draw off at the
  viewport edge, so geometry an app restricted to a small panel with
  `SetViewport` alone was drawn across the whole target. Every draw now sets a
  clip rect (`intersectRects`) — the viewport intersected with
  `D3DRS_SCISSORTESTENABLE`'s rect, since D3D9 applies both — clamped into the
  attachment. The same call also stopped discarding the viewport's `MinZ`/`MaxZ`,
  which `D9WGSetViewport` had carried since M1 while `recordDraw` hardcoded
  `0, 1`: an app compositing a 3D object into a 2D panel routinely restricts the
  depth range so the object cannot collide in depth with the interface around
  it, and ignoring that puts the object at its natural depth instead.

- **Kart Rider shop-panel fix (host side):** XYZRHW ("pre-transformed")
  coordinates are absolute render-target pixels, not viewport-relative ones, so
  the fixed-function screen path subtracts the viewport origin before
  normalising — `setViewport` puts that origin back when it maps NDC into the
  viewport rect, and doing both is what makes the round trip an identity. The
  two cancelled out only for a viewport at 0,0, which is every full-screen UI
  pass, so this stayed invisible until a title drew pre-transformed geometry
  through a small offset viewport: Kart Rider renders each shop item preview
  through a 110x109 viewport at x=368..636, and its pre-transformed geometry
  landed several viewport-widths outside the box and was clipped away. The
  panels whose contents were entirely pre-transformed came out empty, while the
  one item built from world-space geometry rendered perfectly. That split is
  what identified it, and it was only visible by counting pre-transformed draws
  per viewport for one frame — a temporary probe, removed before release
  because it walked every draw op and built strings on every present. If a
  comparable symptom returns (geometry missing or mis-scaled only inside a
  sub-viewport), re-adding that count in `finishFrame` is the shortest path.
  Same correction as wined3d's transformed-position projection matrix, which
  carries a `-2x/w` term for this reason.

**Guest refusals now reach the browser console.** `D9WG_OP_GUEST_LOG` (opcode
11) carries a short ASCII string from the guest DLL to the executor, which
prints it as `[d3d9-guest] …`. The protocol only ever ran one way, so a call the
guest turned down was invisible everywhere a developer can look: the console
sees a clean stream of valid commands, and the guest's own trace file lives
inside a VM whose filesystem the page cannot reach — the exact reason several
"the picture is wrong" investigations here ran on guesswork. `host_log()` is
compiled into the *ordinary* DLL, not just the diagnostic one (needing a special
build to learn that a call was refused is most of the problem), and deduplicates
by exact text so a failure inside a per-frame path costs one message rather than
one per frame, capped at 48 distinct messages. It is deliberately not a general
logging channel: only refusals and failures are sent — every `UNSUPPORTED()`
site, `SetViewport` rejection, `CreateOffscreenPlainSurface`'s format
restriction, and the `CreateTexture`/`CreateVertexBuffer`/`CreateIndexBuffer`
failure paths. An executor too old to know the opcode counts it in
`unsupportedCommands` and skips it, so a new DLL against a stale page degrades
quietly instead of breaking.

Every unimplemented entry point now traces `STUB <Method> -> D3DERR_INVALIDCALL`
(see `UNSUPPORTED()`), including the ones that used to refuse in complete
silence: `UpdateSurface`, `ProcessVertices`, `MultiplyTransform`,
`CreateVolumeTexture`, `GetFrontBufferData`, `SetStreamSourceFreq`,
`Surface::GetDC`/`ReleaseDC`, `GetCubeMapSurface`, clip planes, palettes and
patches, plus `CreateOffscreenPlainSurface`'s deliberate cursor-format
restriction. A silent `D3DERR_INVALIDCALL` is indistinguishable from "the app
never called it" — from the host console, from the trace, from everywhere — and
that blind spot is what left Kart Rider's missing shop art with no evidence at
all to reason from: a picture that was wrong, an executor reporting clean frames
and zero dropped draws, and nothing anywhere recording that the guest had been
turned down. The D9WG protocol has no guest-to-host log channel, so these land
in the diagnostic DLL's trace; `grep STUB` over a trace taken while reproducing
names the APIs a title actually wanted in one step.

**The diagnostic DLL now traces the paths a title can quietly give up in.**
Chasing GTA San Andreas's silent exit turned up three blind spots, each able to
swallow a whole call. The whole `IDirect3D9` enumeration and caps family was
completely dark and now logs arguments and results, including three `CAPS` lines
carrying the fields titles actually gate on. `create_shader` rejected bytecode
*before* emitting anything, so a refused `CreateVertexShader` left no `OK` and no
`FAIL` — only a gap; both paths now trace, with the version token. And
`PROCESS_DETACH` dumps `LAST` (the last marked method entered and left,
previously printed only from the exception handler) alongside `WINDOW` (whether
the device window outlived the process).

The third was the ~100 device methods with no trace of their own —
`SetRenderState`, `SetTextureStageState`, `SetTransform` and the rest. One
`TRACE` at `reserve_command_locked`, the single chokepoint every host-bound
command passes through, covers all of them in one line and is what identified
where San Andreas stopped. It is left commented in that function rather than
compiled in, because it costs one `WriteFile` per command: invaluable up to the
first frame, unusable once a title is drawing. `Device.TestCooperativeLevel` is
untraced for the same reason. Both are two-line changes when a startup
investigation needs them.

For an exit D3D9 cannot explain at all, the diagnostic build installs
thread-local `WH_CALLWNDPROC` and `WH_GETMESSAGE` hooks and logs the window
messages that end a program, as `MSG sent` / `MSG posted`. `WM_QUIT` only ever
travels through the message pump, so `WH_GETMESSAGE` is the one place it is
visible. The ordinary DLL installs nothing.

That combination is what settled San Andreas, and the answer was *not* D3D9.
With both fixes above in place the title runs RenderWare's complete device setup
— device, six buffers, three vertex declarations, six `ps_1_1` pixel shaders,
`DXT1` and `D24S8` format checks — and every single D3D9 call succeeds: no
`FAIL`, no `STUB`, no `EXCEPTION`. Then, in the ~10ms after the last query and
with no D3D9 activity in between, it tears down its three windows in exactly the
order `DestroyWindow` on a foreground window produces (`WM_WINDOWPOSCHANGED`,
`WM_ACTIVATEAPP` wparam=0, `WM_KILLFOCUS`, `WM_DESTROY`, `WM_NCDESTROY`) with no
`WM_CLOSE` and no `WM_QUIT`, and exits without ever releasing the device — the
deactivation is a consequence of the teardown, not a cause of it. That is a
title running its own early-exit path for a reason invisible from here. The
stage after `rsRWINITIALIZE` is input and audio initialisation, and a failure
there sends GTA San Andreas's `WinMain` straight to destroy-window-and-return
without RenderWare teardown, which is the shape observed. The same proxy
technique used here, in `../d3d8proxy` and in `../winproxy` applies directly to
`dinput8.dll`/`dsound.dll` if that thread is picked up.

Known gap worth naming because it renders wrong rather than failing:
**fixed-function vertex blending** (`D3DRS_VERTEXBLEND` with
`D3DTS_WORLDMATRIX(1..3)`) is unimplemented — only `D3DTS_WORLD` is consumed.
A fixed-function declaration carrying `BLENDWEIGHT`/`BLENDINDICES` therefore has
every vertex posed by world matrix 0, which collapses a skinned mesh instead of
animating it. `fixedFunctionVertexSignature` now counts this as
`drawsWithUnappliedVertexBlend` and warns once, so "the model is missing but its
shadow is there" is attributable instead of being one of a dozen possible
causes. The translated-shader skinning path (M5 `UBYTE4`/`SHORT2`/`SHORT4`/
`UDEC3`/`DEC3N` inputs) is unaffected.

Still unimplemented, each returning `D3DERR_INVALIDCALL` (or the closest
matching real error) rather than pretending: volume textures
(`CreateVolumeTexture`), user clip planes (`MaxUserClipPlanes` reports 0, so
nothing asks for them — WGSL has no clip-distance facility, see plan section
9.11), instancing (`SetStreamSourceFreq`), `ProcessVertices`, additional swap
chains, palettes, patches, `MultiplyTransform`, arbitrary GPU-produced
`GetRenderTargetData`, and `GetFrontBufferData` (plan section 2.2).
`d3d9_protocol.h` reserves the opcodes the first few would need, frozen at v0.1
so archived traces stay decodable across milestones.

Set `D9WG_CAPS_PROFILE=ffp` in the guest environment to make `GetDeviceCaps`
report the supported M4.5 fixed-function profile (`VertexShaderVersion`/
`PixelShaderVersion` = `(0,0)`); `D9WG_CAPS_PROFILE=sm2` is the default M5
profile. The legacy `D9WG_SHADER_MODEL=0` spelling remains accepted.

Build an XP-compatible DLL without a C runtime dependency:

```sh
./glbridge/d3d9proxy/build.sh /private/tmp/d3d9.dll
```

The build enforces `-Wall -Wextra -Werror` and asserts the output imports
only `KERNEL32.dll`/`USER32.dll`/`GDI32.dll` — no MSVCRT/UCRT dependency,
matching the D3D8 path's XP-compatibility requirement.

`Direct3DCreate9` is exported alongside `DebugSetMute` and the `D3DPERF_*`
PIX-instrumentation hooks (`BeginEvent`/`EndEvent`/`SetMarker`/`SetRegion`/
`QueryRepeatFrame`/`SetOptions`/`GetStatus`) as harmless no-ops. The D3D8
path hit exactly this class of problem with Warcraft III's
`ValidateVertexShader`/`ValidatePixelShader` static imports — a title that
imports a symbol this DLL does not export fails to *load*, so
`Direct3DCreate9` is never even reached, and the failure surfaces as a
generic "unable to initialize DirectX."

Install the resulting DLL beside the target executable. Use a separate game
deployment profile from `d3d8.dll` and the custom `opengl32.dll` proxy: all
three frontends share one mapped DMA arena and a game directory may load
only one of them (see plan section 4.6/20).

Host-side executor: `../d3d9-webgpu/d3d9_executor.js`, which requires
`../d3d9-webgpu/d3d9_shader_pipeline.js` to be loaded first (it resolves the
translator at load time, not lazily — see the `<script>` order in
`game.html`).

Tests:

- `../tests/d3d9_shader_pipeline_test.js` — bytecode → WGSL translation,
  driven by hand-assembled D3D9 tokens (`node`, no browser);
- `../tests/d3d9_shader_wgsl_validation_test.js` — runs the generated WGSL
  through `naga` when one is installed (`cargo install naga-cli`, or point
  `D9_NAGA` at the binary), so a syntax/type error surfaces in a second
  rather than as a black screen inside v86;
- `../tests/d3d9_webgpu_executor_test.js` — real D9WG batches against a fake
  WebGPU device that enforces bind-group/`writeBuffer` validation rules;
- `../tests/d3d9_webgpu_browser_test.html` — the same paths against real
  WebGPU in a browser, including a translated `vs_2_0`/`ps_2_0` pair with an
  M5 compact vertex input, a lit two-stage cube-sampling draw rendered into a
  texture, partial Clear/ColorFill, and an sRGB back-buffer write.
  Run it against real WebGPU rather than trusting `naga` alone: `naga` and Tint
  disagree on real cases, and the M3 round found one only Tint catches (a bind
  group layout must declare `viewDimension`, because `naga` validates a module
  in isolation and never sees the module/layout pairing);
- `../tests/d3d9_shader_corpus_test.js` — replays a directory of `.d9sh` files
  dumped by `D9WG_DUMP_SHADERS=1` through the translator, grouping failures by
  message. Opt-in via `D9_SHADER_CORPUS`; it is a measurement, not a gate,
  unless `D9_CORPUS_STRICT=1`;
- `../sample/d3d9_shader_test.c` — the real DLL in the real XP guest
  (`./build_smoke_test.sh`).
