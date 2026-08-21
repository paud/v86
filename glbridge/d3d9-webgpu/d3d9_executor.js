// D9WG high-level Direct3D 9 command executor -- M1 skeleton.
//
// The guest DLL (glbridge/d3d9proxy/d3d9_proxy.c) keeps COM objects, shadow
// state, Lock/Unlock memory and batching inside Windows XP. This host owns
// only WebGPU resources and immutable cache objects, mirroring the D3D8
// path's division of responsibility (see ../d3d8-webgpu/d3d8_executor.js)
// but as an independent protocol/implementation: D9WG has its own opcode
// numbering, resource handle namespace and payload shapes (d3d9_protocol.h).
//
// M1 scope: batch decode, a resource table for vertex/index buffers, 2D
// textures and vertex declarations, WebGPU device lifecycle, and the
// fixed-function XYZ/XYZRHW draw path with no programmable shaders.
//
// M2 adds shader model 2.0: CREATE/SET_{VERTEX,PIXEL}_SHADER translated to
// WGSL by d3d9_shader_pipeline.js, the float/int/bool constant register file
// packed into a uniform buffer (plan 9.7), independent sampler state driving
// a GPUSampler cache (plan 4.4/12), and multi-stream vertex declarations.
//
// M3 finishes the fixed-function pipeline, which until then stored a great deal
// of state it never acted on: vertex lighting (SetLight/SetMaterial/
// LightEnable, computed in view space as D3D9 does), the whole D3DTOP_*
// texture-blending cascade across up to eight stages, per-stage coordinate
// selection/generation/transform, cube textures, and the scissor rect. It also
// brings render targets, depth surfaces and MRT forward from M4 -- a 2005-era
// D3D9 game renders most of its frame into textures, so without them it has no
// picture at all -- plus per-process session isolation and device-loss
// recovery, both M1 leftovers.
//
// The fixed-function and programmable paths are one path, not two. Both
// stages are always separate GPUShaderModules meeting over a fixed
// inter-stage varying contract (COLOR0/COLOR1 at locations 0-1, TEXCOORD0..7
// at 2-9, FOG at 10 -- see VARYING_* in d3d9_shader_pipeline.js), with the
// fixed-function stage synthesised into a module that obeys the same
// contract. That is what makes the mixed configurations D3D9 allows work at
// all: fixed-function T&L feeding a real pixel shader, or a vertex shader
// feeding the fixed-function texture pipeline, are both routine in games of
// this era and neither would link if each path had its own varying layout.
//
// Every other D9WG opcode already has a number reserved in d3d9_protocol.h
// for a later milestone, but the guest never emits it yet, so this executor
// does not need a handler for it -- unknown/future opcodes are skipped by
// their `size` field (see decodeCommand) rather than treated as an error,
// matching the parser-safety rule in the implementation plan's section 6.8.
//
// This skeleton deliberately does not yet have the D3D8 path's per-process
// session isolation (multiple concurrent XP sessions with colliding numeric
// handles). It tracks one flat device/resource table, which is sufficient
// for a single running game under v86 but should grow session isolation
// before this path is trusted the way the D3D8 executor now is.

(function(global) {
    "use strict";

    const shaderPipeline = global.D3D9ShaderPipeline ||
        (typeof require === "function" ? require("./d3d9_shader_pipeline.js") : null);
    if (!shaderPipeline)
        throw new Error("d3d9_executor.js requires d3d9_shader_pipeline.js to " +
            "be loaded first");

    const DEFAULT_SHADER_WORKER_URL = (() => {
        try {
            if (typeof document === "undefined" || !document.currentScript ||
                    !document.currentScript.src)
                return null;
            return new URL("d3d9_shader_worker.js",
                document.currentScript.src).href;
        } catch (error) { return null; }
    })();

    const D9WG_MAGIC = 0x47573944; // "D9WG"
    const D9WG_VERSION_MAJOR = 1;
    const D9WG_VERSION_MINOR = 0;
    const D9WG_BATCH_HEADER_BYTES = 32;
    const D9WG_COMMAND_HEADER_BYTES = 16;
    const D9WG_BATCH_FLAG_PRESENT = 1 << 0;
    const D9WG_FEATURE_SHADER_MODEL_2 = 1 << 0;

    const OP_HELLO = 1;
    const OP_CREATE_DEVICE = 2;
    const OP_RESET = 3;
    const OP_PRESENT = 4;
    const OP_CLEAR = 5;
    const OP_BEGIN_SCENE = 6;
    const OP_END_SCENE = 7;
    const OP_CREATE_BUFFER = 0x100;
    const OP_UPDATE_BUFFER = 0x101;
    const OP_DESTROY_RESOURCE = 0x103;
    const OP_STRETCH_RECT = 8;
    const OP_COLOR_FILL = 9;
    const OP_GUEST_LOG = 11;
    const GUEST_LOG_SEVERITY_INFO = 0;
    const GUEST_LOG_SEVERITY_FAILED = 2;
    const OP_CREATE_TEXTURE_2D = 0x110;
    const OP_CREATE_TEXTURE_CUBE = 0x111;
    const OP_UPDATE_TEXTURE = 0x113;
    const OP_SET_SCISSOR_RECT = 0x205;
    const OP_SET_RENDER_TARGET = 0x20F;
    const OP_SET_DEPTH_STENCIL_SURFACE = 0x210;
    // D9WGSetDepthStencilSurface.depth_texture_handle sentinel: the device's own
    // auto depth-stencil surface. It needs a value distinct from 0 because
    // SetDepthStencilSurface(NULL) -- which really does turn depth testing off
    // -- also has no texture handle, and an app that renders to a texture and
    // then restores the back buffer's depth surface must be able to say which
    // of the two it means.
    const D9WG_AUTO_DEPTH_STENCIL_HANDLE = 0xFFFFFFFF;
    const OP_CREATE_VERTEX_DECLARATION = 0x120;
    const OP_CREATE_VERTEX_SHADER = 0x121;
    const OP_CREATE_PIXEL_SHADER = 0x122;
    const OP_SET_RENDER_STATE = 0x200;
    const OP_SET_SAMPLER_STATE = 0x201;
    const OP_SET_TEXTURE_STAGE_STATE = 0x202;
    const OP_SET_TEXTURE = 0x203;
    const OP_SET_VIEWPORT = 0x204;
    const OP_SET_TRANSFORM = 0x206;
    const OP_SET_MATERIAL = 0x207;
    const OP_SET_LIGHT = 0x208;
    const OP_LIGHT_ENABLE = 0x209;
    const OP_SET_STREAM_SOURCE = 0x20A;
    const OP_SET_INDICES = 0x20C;
    const OP_SET_VERTEX_DECLARATION = 0x20D;
    const OP_SET_FVF = 0x20E;
    const OP_SET_CURSOR_PROPERTIES = 0x21A;
    const OP_SET_CURSOR_POSITION = 0x21B;
    const OP_SHOW_CURSOR = 0x21C;
    const OP_WINDOW_STATE = 0x21D;
    const D9WG_WINDOW_IS_WINDOW = 1 << 0;
    const D9WG_WINDOW_VISIBLE = 1 << 1;
    const D9WG_WINDOW_ICONIC = 1 << 2;
    const D9WG_WINDOW_FOREGROUND = 1 << 3;
    const D9WG_WINDOW_FULLSCREEN = 1 << 4;
    const OP_SET_VERTEX_SHADER = 0x211;
    const OP_SET_PIXEL_SHADER = 0x212;
    const OP_SET_VERTEX_SHADER_CONSTANT_F = 0x213;
    const OP_SET_VERTEX_SHADER_CONSTANT_I = 0x214;
    const OP_SET_VERTEX_SHADER_CONSTANT_B = 0x215;
    const OP_SET_PIXEL_SHADER_CONSTANT_F = 0x216;
    const OP_SET_PIXEL_SHADER_CONSTANT_I = 0x217;
    const OP_SET_PIXEL_SHADER_CONSTANT_B = 0x218;
    const OP_DRAW_PRIMITIVE = 0x300;
    const OP_DRAW_INDEXED_PRIMITIVE = 0x301;
    const OP_DRAW_PRIMITIVE_UP = 0x302;
    const OP_DRAW_INDEXED_PRIMITIVE_UP = 0x303;

    const RESOURCE_BUFFER_VERTEX = 1;
    const RESOURCE_BUFFER_INDEX = 2;
    const RESOURCE_TEXTURE_2D = 3;
    const RESOURCE_TEXTURE_CUBE = 4;
    const RESOURCE_VERTEX_DECLARATION = 6;
    const RESOURCE_VERTEX_SHADER = 7;
    const RESOURCE_PIXEL_SHADER = 8;

    // Constant register file sizes, matching D9_MAX_* in d3d9_proxy.c.
    const MAX_VS_CONST_F = 256;
    const MAX_PS_CONST_F = 224;
    const MAX_CONST_I = 16;
    const MAX_CONST_B = 16;
    const MAX_SAMPLERS = 16;
    const MAX_STREAMS = 4;
    // fill_caps() reports NumSimultaneousRTs = 4.
    const MAX_RENDER_TARGETS = 4;
    // WebGPU's minUniformBufferOffsetAlignment default. The vertex and pixel
    // constant regions share one buffer, so the pixel region starts here.
    const UNIFORM_OFFSET_ALIGNMENT = 256;
    // M6 keeps per-draw constants in one persistent buffer. 16 MiB covers
    // tens of thousands of ordinary UI/particle draws while remaining small
    // compared with the texture working set of the target games. A frame that
    // exceeds it falls back to a retired one-off buffer rather than wrapping
    // over constants that have already been recorded.
    const UNIFORM_RING_BYTES = 16 * 1024 * 1024;

    const D3DFMT_R8G8B8 = 20;
    const D3DFMT_A8R8G8B8 = 21;
    const D3DFMT_X8R8G8B8 = 22;
    const D3DFMT_R5G6B5 = 23;
    const D3DFMT_X1R5G5B5 = 24;
    const D3DFMT_A1R5G5B5 = 25;
    const D3DFMT_A4R4G4B4 = 26;
    const D3DFMT_R3G3B2 = 27;
    const D3DFMT_A8 = 28;
    const D3DFMT_A8R3G3B2 = 29;
    const D3DFMT_X4R4G4B4 = 30;
    const D3DFMT_A8B8G8R8 = 32;
    const D3DFMT_X8B8G8R8 = 33;
    const D3DFMT_L8 = 50;
    const D3DFMT_A8L8 = 51;
    const D3DFMT_A4L4 = 52;
    const D3DFMT_V8U8 = 60;
    const D3DFMT_L6V5U5 = 61;
    const D3DFMT_X8L8V8U8 = 62;
    const D3DFMT_Q8W8V8U8 = 63;
    const D3DFMT_V16U16 = 64;
    const D3DFMT_A2W10V10U10 = 67;
    const D3DFMT_L16 = 81;
    const D3DFMT_CxV8U8 = 117;
    const D3DFMT_DXT1 = 0x31545844;
    const D3DFMT_DXT2 = 0x32545844;
    const D3DFMT_DXT3 = 0x33545844;
    const D3DFMT_DXT4 = 0x34545844;
    const D3DFMT_DXT5 = 0x35545844;
    const D3DFMT_INDEX16 = 101;
    const D3DFMT_INDEX32 = 102;

    const D3DUSAGE_RENDERTARGET = 0x1;
    const D3DUSAGE_DEPTHSTENCIL = 0x2;

    const D3DCLEAR_TARGET = 0x1;
    const D3DCLEAR_ZBUFFER = 0x2;
    const D3DCLEAR_STENCIL = 0x4;

    // D3DRENDERSTATETYPE values the M1 fixed-function pipeline now honours.
    // Everything else the guest sends is still recorded in the device's
    // renderStates map but has no effect yet.
    const D3DRS_ZENABLE = 7;
    const D3DRS_ZWRITEENABLE = 14;
    const D3DRS_ALPHATESTENABLE = 15;
    const D3DRS_ALPHAREF = 24;
    const D3DRS_ALPHAFUNC = 25;
    const D3DRS_SRCBLEND = 19;
    const D3DRS_DESTBLEND = 20;
    const D3DRS_CULLMODE = 22;
    const D3DRS_ZFUNC = 23;
    const D3DRS_ALPHABLENDENABLE = 27;
    const D3DRS_STENCILENABLE = 52;
    const D3DRS_STENCILFAIL = 53;
    const D3DRS_STENCILZFAIL = 54;
    const D3DRS_STENCILPASS = 55;
    const D3DRS_STENCILFUNC = 56;
    const D3DRS_STENCILREF = 57;
    const D3DRS_STENCILMASK = 58;
    const D3DRS_STENCILWRITEMASK = 59;
    const D3DRS_COLORWRITEENABLE = 168;
    const D3DRS_SLOPESCALEDEPTHBIAS = 175;
    const D3DRS_SCISSORTESTENABLE = 174;
    const D3DRS_TWOSIDEDSTENCILMODE = 185;
    const D3DRS_CCW_STENCILFAIL = 186;
    const D3DRS_CCW_STENCILZFAIL = 187;
    const D3DRS_CCW_STENCILPASS = 188;
    const D3DRS_CCW_STENCILFUNC = 189;
    const D3DRS_SRGBWRITEENABLE = 194;
    const D3DRS_COLORWRITEENABLE1 = 190;
    const D3DRS_COLORWRITEENABLE2 = 191;
    const D3DRS_COLORWRITEENABLE3 = 192;
    const D3DRS_BLENDOP = 171;
    const D3DRS_BLENDFACTOR = 193;
    const D3DRS_DEPTHBIAS = 195;
    const D3DRS_SEPARATEALPHABLENDENABLE = 206;
    const D3DRS_SRCBLENDALPHA = 207;
    const D3DRS_DESTBLENDALPHA = 208;
    const D3DRS_BLENDOPALPHA = 209;
    // Fixed-function fog. fill_caps() in d3d9_proxy.c advertises
    // D3DPRASTERCAPS_FOGVERTEX/FOGTABLE/WFOG, so a game is entitled to expect
    // these to work; ignoring them left Warcraft III's fogged scenery drawn at
    // full texture colour with none of the atmospheric tint.
    const D3DRS_FOGENABLE = 28;
    const D3DRS_FOGCOLOR = 34;
    const D3DRS_FOGTABLEMODE = 35;
    const D3DRS_FOGSTART = 36;
    const D3DRS_FOGEND = 37;
    const D3DRS_FOGDENSITY = 38;
    const D3DRS_FOGVERTEXMODE = 140;
    const D3DRS_RANGEFOGENABLE = 48;
    const D3DRS_LIGHTING = 137;
    const D3DRS_AMBIENT = 139;
    // D3DFOGMODE
    const D3DFOG_NONE = 0, D3DFOG_EXP = 1, D3DFOG_EXP2 = 2, D3DFOG_LINEAR = 3;

    // Fixed-function lighting and the texture-blending cascade (M3). The guest
    // has emitted all of these since M1 -- SetMaterial/SetLight/LightEnable and
    // every texture stage state -- and the host recorded them without acting on
    // any, which is why a lit scene came out flat white and every stage past 0
    // was ignored. fill_caps() in d3d9_proxy.c has meanwhile been advertising
    // MaxTextureBlendStages = 8, D3DVTXPCAPS_DIRECTIONALLIGHTS/POSITIONALLIGHTS
    // and a large TextureOpCaps set, so those were caps promises the renderer
    // did not keep; this section is what makes them true.
    const D3DRS_SPECULARENABLE = 29;
    const D3DRS_TEXTUREFACTOR = 60;
    const D3DRS_COLORVERTEX = 141;
    const D3DRS_LOCALVIEWER = 142;
    const D3DRS_NORMALIZENORMALS = 143;
    const D3DRS_DIFFUSEMATERIALSOURCE = 145;
    const D3DRS_SPECULARMATERIALSOURCE = 146;
    const D3DRS_AMBIENTMATERIALSOURCE = 147;
    const D3DRS_EMISSIVEMATERIALSOURCE = 148;
    // Point primitives/point sprites. WebGPU only exposes one-pixel points,
    // so M6 expands every D3D point to a six-vertex quad in the vertex stage.
    const D3DRS_POINTSIZE = 154;
    const D3DRS_POINTSIZE_MIN = 155;
    const D3DRS_POINTSPRITEENABLE = 156;
    const D3DRS_POINTSCALEENABLE = 157;
    const D3DRS_POINTSCALE_A = 158;
    const D3DRS_POINTSCALE_B = 159;
    const D3DRS_POINTSCALE_C = 160;
    const D3DRS_POINTSIZE_MAX = 166;

    // D3DLIGHTTYPE
    const D3DLIGHT_POINT = 1, D3DLIGHT_SPOT = 2, D3DLIGHT_DIRECTIONAL = 3;
    // D3DMATERIALCOLORSOURCE
    const D3DMCS_MATERIAL = 0, D3DMCS_COLOR1 = 1, D3DMCS_COLOR2 = 2;
    // fill_caps() reports MaxActiveLights = 8.
    const MAX_LIGHTS = 8;

    // D3DTEXTURESTAGESTATETYPE
    const D3DTSS_COLOROP = 1, D3DTSS_COLORARG1 = 2, D3DTSS_COLORARG2 = 3;
    const D3DTSS_ALPHAOP = 4, D3DTSS_ALPHAARG1 = 5, D3DTSS_ALPHAARG2 = 6;
    const D3DTSS_TEXCOORDINDEX = 11;
    const D3DTSS_TEXTURETRANSFORMFLAGS = 24;
    const D3DTSS_COLORARG0 = 26, D3DTSS_ALPHAARG0 = 27, D3DTSS_RESULTARG = 28;
    const D3DTSS_CONSTANT = 32;

    // D3DTEXTUREOP. Only the operations fill_caps() advertises in TextureOpCaps
    // are implemented below; PREMODULATE and the BUMPENVMAP pair are absent
    // from both, and a stage asking for one is counted rather than approximated
    // (an approximated bump map renders as wrong-but-plausible shading, which
    // is the failure mode hardest to attribute).
    const D3DTOP_DISABLE = 1, D3DTOP_SELECTARG1 = 2, D3DTOP_SELECTARG2 = 3;
    const D3DTOP_MODULATE = 4, D3DTOP_MODULATE2X = 5, D3DTOP_MODULATE4X = 6;
    const D3DTOP_ADD = 7, D3DTOP_ADDSIGNED = 8, D3DTOP_ADDSIGNED2X = 9;
    const D3DTOP_SUBTRACT = 10, D3DTOP_ADDSMOOTH = 11;
    const D3DTOP_BLENDDIFFUSEALPHA = 12, D3DTOP_BLENDTEXTUREALPHA = 13;
    const D3DTOP_BLENDFACTORALPHA = 14, D3DTOP_BLENDTEXTUREALPHAPM = 15;
    const D3DTOP_BLENDCURRENTALPHA = 16;
    const D3DTOP_DOTPRODUCT3 = 24, D3DTOP_MULTIPLYADD = 25, D3DTOP_LERP = 26;

    // D3DTA_* argument selectors and their two modifier bits.
    const D3DTA_SELECTMASK = 0x0000000f;
    const D3DTA_COMPLEMENT = 0x00000010;
    const D3DTA_ALPHAREPLICATE = 0x00000020;
    const D3DTA_DIFFUSE = 0, D3DTA_CURRENT = 1, D3DTA_TEXTURE = 2;
    const D3DTA_TFACTOR = 3, D3DTA_SPECULAR = 4, D3DTA_TEMP = 5;
    const D3DTA_CONSTANT = 6;

    // D3DTSS_TEXCOORDINDEX's high bits: automatic coordinate generation.
    const D3DTSS_TCI_MASK = 0xffff0000;
    const D3DTSS_TCI_PASSTHRU = 0x00000000;
    const D3DTSS_TCI_CAMERASPACENORMAL = 0x00010000;
    const D3DTSS_TCI_CAMERASPACEPOSITION = 0x00020000;
    const D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR = 0x00030000;
    const D3DTSS_TCI_SPHEREMAP = 0x00040000;

    // D3DTSS_TEXTURETRANSFORMFLAGS
    const D3DTTFF_PROJECTED = 0x100;

    // D3DTSS_RESULTARG picks where a stage writes: the cascade register
    // (D3DTA_CURRENT) or the scratch register (D3DTA_TEMP), which a later stage
    // can read back as an argument.
    const MAX_TEXTURE_STAGES = 8;
    // TEXCOORD0..7 are the only coordinate sets D3D9 has, so this bounds both
    // the vertex attributes and the varyings the cascade can reference.
    const MAX_TEXCOORD_SETS = 8;

    const D3DZB_FALSE = 0;
    const D3DCULL_NONE = 1;
    const D3DCULL_CW = 2;
    const D3DCULL_CCW = 3;

    // D3DCMPFUNC -> GPUCompareFunction
    const COMPARE_FUNCS = [
        undefined, "never", "less", "equal", "less-equal",
        "greater", "not-equal", "greater-equal", "always",
    ];

    // D3DBLEND -> GPUBlendFactor. The BOTH* values are resolved as a source /
    // destination pair in pipelineStateFor(); BLENDFACTOR maps to WebGPU's
    // dynamic blend constant and is installed before each draw.
    const BLEND_FACTORS = [
        undefined,
        "zero",                 // D3DBLEND_ZERO = 1
        "one",                  // D3DBLEND_ONE
        "src",                  // D3DBLEND_SRCCOLOR
        "one-minus-src",        // D3DBLEND_INVSRCCOLOR
        "src-alpha",            // D3DBLEND_SRCALPHA
        "one-minus-src-alpha",  // D3DBLEND_INVSRCALPHA
        "dst-alpha",            // D3DBLEND_DESTALPHA
        "one-minus-dst-alpha",  // D3DBLEND_INVDESTALPHA
        "dst",                  // D3DBLEND_DESTCOLOR
        "one-minus-dst",        // D3DBLEND_INVDESTCOLOR
        "src-alpha-saturated",  // D3DBLEND_SRCALPHASAT
        undefined,              // D3DBLEND_BOTHSRCALPHA (pair alias)
        undefined,              // D3DBLEND_BOTHINVSRCALPHA (pair alias)
        "constant",             // D3DBLEND_BLENDFACTOR
        "one-minus-constant",   // D3DBLEND_INVBLENDFACTOR
    ];

    // D3DBLENDOP -> GPUBlendOperation
    const BLEND_OPS = [
        undefined, "add", "subtract", "reverse-subtract", "min", "max",
    ];

    // D3DSTENCILOP -> GPUStencilOperation.
    const STENCIL_OPS = [
        undefined, "keep", "zero", "replace", "increment-clamp",
        "decrement-clamp", "invert", "increment-wrap", "decrement-wrap",
    ];

    // WebGPU's depth format for the auto depth-stencil surface. D3D9 apps ask
    // for D16/D24S8/D24X8/etc; all of them are satisfied with one real
    // depth24plus-stencil8 target rather than trying to match bit layouts the
    // guest can never observe (it cannot read the depth buffer back in M1).
    const DEPTH_FORMAT = "depth24plus-stencil8";
    const TEXTURE_USAGE_RENDER_ATTACHMENT = 0x10;

    // D3DDECLUSAGE / D3DDECLTYPE, the subset d3d9_proxy.c's
    // declaration_element_supported() lets through.
    const DECLUSAGE_POSITION = 0;
    const DECLUSAGE_BLENDWEIGHT = 1;
    const DECLUSAGE_BLENDINDICES = 2;
    const DECLUSAGE_NORMAL = 3;
    const DECLUSAGE_PSIZE = 4;
    const DECLUSAGE_TEXCOORD = 5;
    const DECLUSAGE_POSITIONT = 9;
    const DECLUSAGE_COLOR = 10;
    const DECLTYPE_FLOAT1 = 0;
    const DECLTYPE_FLOAT2 = 1;
    const DECLTYPE_FLOAT3 = 2;
    const DECLTYPE_FLOAT4 = 3;
    const DECLTYPE_D3DCOLOR = 4;

    // D3DDECLTYPE -> [GPUVertexFormat, byte size]. D3DCOLOR is read as raw
    // little-endian BGRA bytes by unorm8x4, so anything consuming it has to
    // swizzle; both shader generators below do that at the point where the
    // attribute is copied into its register, never with a CPU pass over the
    // vertex data.
    const DECLTYPE_FORMATS = {
        [DECLTYPE_FLOAT1]: ["float32", 4],
        [DECLTYPE_FLOAT2]: ["float32x2", 8],
        [DECLTYPE_FLOAT3]: ["float32x3", 12],
        [DECLTYPE_FLOAT4]: ["float32x4", 16],
        [DECLTYPE_D3DCOLOR]: ["unorm8x4", 4],
        5: ["uint8x4", 4],     // D3DDECLTYPE_UBYTE4
        6: ["sint16x2", 4],    // D3DDECLTYPE_SHORT2
        7: ["sint16x4", 8],    // D3DDECLTYPE_SHORT4
        8: ["unorm8x4", 4],    // D3DDECLTYPE_UBYTE4N
        9: ["snorm16x2", 4],   // D3DDECLTYPE_SHORT2N
        10: ["snorm16x4", 8],  // D3DDECLTYPE_SHORT4N
        11: ["unorm16x2", 4],  // D3DDECLTYPE_USHORT2N
        12: ["unorm16x4", 8],  // D3DDECLTYPE_USHORT4N
        13: ["uint32", 4],     // D3DDECLTYPE_UDEC3 (unpacked in WGSL)
        14: ["uint32", 4],     // D3DDECLTYPE_DEC3N (unpacked in WGSL)
        15: ["float16x2", 4],  // D3DDECLTYPE_FLOAT16_2
        16: ["float16x4", 8],  // D3DDECLTYPE_FLOAT16_4
    };

    // D3DSAMPLERSTATETYPE, and the enums its values come from.
    const D3DSAMP_ADDRESSU = 1;
    const D3DSAMP_ADDRESSV = 2;
    const D3DSAMP_ADDRESSW = 3;
    const D3DSAMP_BORDERCOLOR = 4;
    const D3DSAMP_MAGFILTER = 5;
    const D3DSAMP_MINFILTER = 6;
    const D3DSAMP_MIPFILTER = 7;
    const D3DSAMP_MAXANISOTROPY = 10;
    // D3D9 decodes an sRGB-tagged texture to linear *on read*. WebGPU has no
    // sampler-level equivalent -- it is a property of the texture format -- so
    // this is honoured by sampling through an "-srgb" view of the same texture
    // rather than by anything in the sampler.
    const D3DSAMP_SRGBTEXTURE = 11;

    // D3DTEXTUREADDRESS -> GPUAddressMode. WebGPU has no BORDER mode and no
    // MIRRORONCE; both fall back to clamp-to-edge, which is the closest
    // available behaviour and is noted once per occurrence rather than
    // silently substituted.
    const ADDRESS_MODES = [
        undefined, "repeat", "mirror-repeat", "clamp-to-edge",
        "clamp-to-edge", "clamp-to-edge",
    ];
    // D3DTEXTUREFILTERTYPE -> GPUFilterMode. ANISOTROPIC becomes linear plus
    // a maxAnisotropy value; PYRAMIDALQUAD/GAUSSIANQUAD have no equivalent.
    const FILTER_MODES = [
        "nearest", "nearest", "linear", "linear",
        "linear", "linear", "linear",
    ];

    // Which states the renderer actually consumes. Kept next to the constants
    // rather than derived from them, because "we read this" is a statement about
    // the code below, not about the enum.
    const CONSUMED_SAMPLER_STATES = new Set([
        D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW,
        D3DSAMP_BORDERCOLOR,
        D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
        D3DSAMP_MAXANISOTROPY, D3DSAMP_SRGBTEXTURE,
    ]);

    const CONSUMED_RENDER_STATES = new Set([
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF,
        D3DRS_ALPHAFUNC, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_CULLMODE,
        D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_COLORWRITEENABLE,
        D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3,
        D3DRS_BLENDOP, D3DRS_BLENDFACTOR, D3DRS_SEPARATEALPHABLENDENABLE,
        D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA,
        D3DRS_STENCILENABLE, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL,
        D3DRS_STENCILPASS, D3DRS_STENCILFUNC, D3DRS_STENCILREF,
        D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK,
        D3DRS_TWOSIDEDSTENCILMODE, D3DRS_CCW_STENCILFAIL,
        D3DRS_CCW_STENCILZFAIL, D3DRS_CCW_STENCILPASS,
        D3DRS_CCW_STENCILFUNC, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS,
        D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE,
        D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_FOGVERTEXMODE,
        D3DRS_RANGEFOGENABLE, D3DRS_LIGHTING, D3DRS_AMBIENT,
        D3DRS_SPECULARENABLE, D3DRS_TEXTUREFACTOR, D3DRS_COLORVERTEX,
        D3DRS_LOCALVIEWER, D3DRS_NORMALIZENORMALS, D3DRS_DIFFUSEMATERIALSOURCE,
        D3DRS_SPECULARMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE,
        D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_SCISSORTESTENABLE,
        D3DRS_SRGBWRITEENABLE, D3DRS_POINTSIZE, D3DRS_POINTSIZE_MIN,
        D3DRS_POINTSPRITEENABLE, D3DRS_POINTSCALEENABLE,
        D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B, D3DRS_POINTSCALE_C,
        D3DRS_POINTSIZE_MAX,
    ]);

    const D3DTS_VIEW = 2;
    const D3DTS_PROJECTION = 3;
    const D3DTS_WORLD = 256;
    const D3DTS_TEXTURE0 = 16;

    // D3DPRIMITIVETYPE -> WebGPU topology, and element-count helpers mirror
    // d3d9_proxy.c's primitive_element_count() so guest/host agree on how
    // many vertices/indices a given primitive_count consumes. Only list/strip
    // forms map onto a single WebGPU draw call directly; FAN is converted to
    // a triangle list index buffer on upload, same discipline as the D3D8
    // path.
    const D3DPT_POINTLIST = 1;
    const D3DPT_LINELIST = 2;
    const D3DPT_LINESTRIP = 3;
    const D3DPT_TRIANGLELIST = 4;
    const D3DPT_TRIANGLESTRIP = 5;
    const D3DPT_TRIANGLEFAN = 6;

    const BUFFER_USAGE_VERTEX = 0x20;
    const BUFFER_USAGE_INDEX = 0x10;
    const BUFFER_USAGE_UNIFORM = 0x40;
    const BUFFER_USAGE_COPY_SRC = 0x4;
    const BUFFER_USAGE_COPY_DST = 0x8;
    const TEXTURE_USAGE_COPY_SRC = 0x1;
    const TEXTURE_USAGE_COPY_DST = 0x2;
    const SHADER_STAGE_VERTEX = 0x1;
    const SHADER_STAGE_FRAGMENT = 0x2;
    const TEXTURE_USAGE_TEXTURE_BINDING = 0x4;

    const TEXTURE_FORMAT_NAMES = {
        20: "R8G8B8", 21: "A8R8G8B8", 22: "X8R8G8B8", 23: "R5G6B5",
        24: "X1R5G5B5", 25: "A1R5G5B5", 26: "A4R4G4B4",
        27: "R3G3B2", 28: "A8", 29: "A8R3G3B2", 30: "X4R4G4B4",
        32: "A8B8G8R8", 33: "X8B8G8R8", 50: "L8", 51: "A8L8",
        52: "A4L4", 60: "V8U8", 61: "L6V5U5", 62: "X8L8V8U8",
        63: "Q8W8V8U8", 64: "V16U16", 67: "A2W10V10U10", 81: "L16",
        117: "CxV8U8",
        0x31545844: "DXT1", 0x32545844: "DXT2",
        0x33545844: "DXT3", 0x34545844: "DXT4",
        0x35545844: "DXT5",
    };

    // D3D9 stores FOGSTART/FOGEND/FOGDENSITY as float bits inside a DWORD.
    const FLOAT_BITS_BUFFER = new ArrayBuffer(4);
    const FLOAT_BITS_U32 = new Uint32Array(FLOAT_BITS_BUFFER);
    const FLOAT_BITS_F32 = new Float32Array(FLOAT_BITS_BUFFER);

    function alignUp(value, alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    function floatFromDWORD(value) {
        FLOAT_BITS_U32[0] = value >>> 0;
        return FLOAT_BITS_F32[0];
    }

    // The "-srgb" sibling of a GPU format, or null when there is none. A view
    // can only use a format listed in the texture's viewFormats at creation, so
    // this has to be known up front rather than at bind time.
    function srgbSiblingOf(gpuFormat) {
        return {
            "bgra8unorm": "bgra8unorm-srgb",
            "rgba8unorm": "rgba8unorm-srgb",
            "bc1-rgba-unorm": "bc1-rgba-unorm-srgb",
            "bc2-rgba-unorm": "bc2-rgba-unorm-srgb",
            "bc3-rgba-unorm": "bc3-rgba-unorm-srgb",
        }[gpuFormat] || null;
    }

    function isCompressedFormat(format) {
        return format === D3DFMT_DXT1 || format === D3DFMT_DXT2 ||
            format === D3DFMT_DXT3 || format === D3DFMT_DXT4 ||
            format === D3DFMT_DXT5;
    }

    // WebGPU measures a block-compressed copy in whole texel blocks: copySize
    // must be a multiple of the BCn 4x4 block, and a mip level's *physical*
    // extent is its logical size rounded up to that grid. So the tail of a DXT
    // mip chain has to be written as a full 4x4 block even though its logical
    // size is 2x2 or 1x1. Passing the logical size makes writeTexture fail
    // validation, and because that failure surfaces as an uncaptured device
    // error rather than an exception, the level silently keeps whatever the
    // texture was created with -- hundreds of "copySize.width (1) is not a
    // multiple of compressed texture format block width (4)" errors while Kart
    // Rider loaded its UI atlases, with no other symptom than the smallest mips
    // sampling as garbage.
    //
    // Callers pass a block-aligned origin, which WebGPU requires independently,
    // so rounding the extent up cannot run past the physical mip extent.
    function blockAlignedCopyExtent(size, compressed) {
        return compressed ? Math.ceil(size / 4) * 4 : size;
    }

    // The pixel rect a draw may touch: the D3D9 viewport (which clips, unlike
    // WebGPU's) intersected with the scissor rect when D3DRS_SCISSORTESTENABLE
    // is on, then clamped into the attachment -- WebGPU rejects a scissor that
    // leaves the render target, and an app is free to set a viewport that does.
    // An empty intersection stays empty (zero width or height), which draws
    // nothing, exactly as D3D9 would.
    function intersectRects(viewport, scissor, targetWidth, targetHeight) {
        let left = viewport.x;
        let top = viewport.y;
        let right = viewport.x + viewport.width;
        let bottom = viewport.y + viewport.height;
        if (scissor) {
            left = Math.max(left, scissor.x);
            top = Math.max(top, scissor.y);
            right = Math.min(right, scissor.x + scissor.width);
            bottom = Math.min(bottom, scissor.y + scissor.height);
        }
        left = Math.max(0, Math.min(left, targetWidth));
        top = Math.max(0, Math.min(top, targetHeight));
        right = Math.max(left, Math.min(right, targetWidth));
        bottom = Math.max(top, Math.min(bottom, targetHeight));
        return { x: left, y: top, width: right - left, height: bottom - top };
    }

    function isHalfFloatExpansionFormat(format) {
        return format === D3DFMT_L6V5U5 || format === D3DFMT_X8L8V8U8 ||
            format === D3DFMT_V16U16 || format === D3DFMT_A2W10V10U10 ||
            format === D3DFMT_L16 || format === D3DFMT_CxV8U8;
    }

    function gpuBytesPerTexel(format) {
        return isHalfFloatExpansionFormat(format) ? 8 : 4;
    }

    function isRenderableGPUFormat(format) {
        return format === "rgba8unorm" || format === "rgba16float";
    }

    function formatToGPU(format) {
        switch (format) {
        case D3DFMT_R8G8B8:
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_R3G3B2:
        case D3DFMT_A8R3G3B2:
        case D3DFMT_X4R4G4B4:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
        case D3DFMT_R5G6B5:
        case D3DFMT_L8:
        case D3DFMT_A8:
        case D3DFMT_A8L8:
        case D3DFMT_A4L4:
            // All of these are CPU-expanded to tightly-packed RGBA8 on
            // upload (see expandRowToGPU), matching the D3D8 path's
            // approach: WebGPU has no native 16-bit BGR/BGRA formats.
            return "rgba8unorm";
        case D3DFMT_V8U8:
        case D3DFMT_Q8W8V8U8:
            return "rgba8snorm";
        case D3DFMT_L6V5U5:
        case D3DFMT_X8L8V8U8:
        case D3DFMT_V16U16:
        case D3DFMT_A2W10V10U10:
        case D3DFMT_L16:
        case D3DFMT_CxV8U8:
            // Mixed signed/unsigned bump formats, CxV8U8's reconstructed
            // component, and high-precision L16 cannot be represented by one
            // normalized 8-bit WebGPU format without losing their semantics.
            // Half-float preserves the sampled values closely.
            return "rgba16float";
        case D3DFMT_DXT1:
            return "bc1-rgba-unorm";
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:
            return "bc2-rgba-unorm";
        case D3DFMT_DXT4:
        case D3DFMT_DXT5:
            return "bc3-rgba-unorm";
        default:
            return null;
        }
    }

    const HALF_BITS_BUFFER = new ArrayBuffer(4);
    const HALF_BITS_F32 = new Float32Array(HALF_BITS_BUFFER);
    const HALF_BITS_U32 = new Uint32Array(HALF_BITS_BUFFER);

    function floatToHalfBits(value) {
        HALF_BITS_F32[0] = value;
        const bits = HALF_BITS_U32[0];
        const sign = (bits >>> 16) & 0x8000;
        let exponent = (bits >>> 23) & 0xff;
        let mantissa = bits & 0x7fffff;
        if (exponent === 0xff)
            return sign | (mantissa ? 0x7e00 : 0x7c00);
        exponent = exponent - 127 + 15;
        if (exponent >= 31) return sign | 0x7c00;
        if (exponent <= 0) {
            if (exponent < -10) return sign;
            const normalized = mantissa | 0x800000;
            const shift = 14 - exponent;
            let halfMantissa = normalized >>> shift;
            const remainderMask = (1 << shift) - 1;
            const remainder = normalized & remainderMask;
            const halfway = 1 << (shift - 1);
            if (remainder > halfway ||
                    (remainder === halfway && (halfMantissa & 1)))
                ++halfMantissa;
            return sign | halfMantissa;
        }
        let halfMantissa = mantissa >>> 13;
        const remainder = mantissa & 0x1fff;
        if (remainder > 0x1000 ||
                (remainder === 0x1000 && (halfMantissa & 1))) {
            ++halfMantissa;
            if (halfMantissa === 0x400) {
                halfMantissa = 0;
                if (++exponent >= 31) return sign | 0x7c00;
            }
        }
        return sign | (exponent << 10) | halfMantissa;
    }

    function writeHalf(dest, offset, value) {
        const bits = floatToHalfBits(value);
        dest[offset] = bits & 0xff;
        dest[offset + 1] = bits >>> 8;
    }

    function signedNormalized(value, bitCount) {
        const signBit = 1 << (bitCount - 1);
        const fullRange = 1 << bitCount;
        const signed = value & signBit ? value - fullRange : value;
        return signed === -signBit ? -1 : signed / (signBit - 1);
    }

    // Expands one source row to the WebGPU format returned by formatToGPU().
    // Ordinary colour/luminance formats become RGBA8 UNORM, signed bump maps
    // become RGBA8 SNORM, and mixed/high-precision formats become RGBA16F.
    // BCn formats bypass this routine and stay block-compressed end-to-end.
    function expandRowToGPU(format, source, sourceOffset, count, dest, destOffset) {
        for (let i = 0; i < count; ++i) {
            let r, g, b, a;
            if (isHalfFloatExpansionFormat(format)) {
                let u, v, w, q;
                if (format === D3DFMT_CxV8U8) {
                    const at = sourceOffset + i * 2;
                    u = signedNormalized(source[at], 8);
                    v = signedNormalized(source[at + 1], 8);
                    w = Math.sqrt(Math.max(0, 1 - u * u - v * v));
                    q = 1;
                } else if (format === D3DFMT_L16) {
                    const at = sourceOffset + i * 2;
                    const luminance = (source[at] |
                        (source[at + 1] << 8)) / 65535;
                    u = v = w = luminance;
                    q = 1;
                } else if (format === D3DFMT_L6V5U5) {
                    const value = source[sourceOffset + i * 2] |
                        (source[sourceOffset + i * 2 + 1] << 8);
                    u = signedNormalized(value & 0x1f, 5);
                    v = signedNormalized((value >>> 5) & 0x1f, 5);
                    w = ((value >>> 10) & 0x3f) / 63;
                    q = 1;
                } else if (format === D3DFMT_X8L8V8U8) {
                    const at = sourceOffset + i * 4;
                    u = signedNormalized(source[at], 8);
                    v = signedNormalized(source[at + 1], 8);
                    w = source[at + 2] / 255;
                    q = 1;
                } else if (format === D3DFMT_V16U16) {
                    const at = sourceOffset + i * 4;
                    const rawU = source[at] | (source[at + 1] << 8);
                    const rawV = source[at + 2] | (source[at + 3] << 8);
                    u = signedNormalized(rawU, 16);
                    v = signedNormalized(rawV, 16);
                    w = q = 1;
                } else {
                    const at = sourceOffset + i * 4;
                    const value = (source[at] | (source[at + 1] << 8) |
                        (source[at + 2] << 16) |
                        (source[at + 3] << 24)) >>> 0;
                    u = signedNormalized(value & 0x3ff, 10);
                    v = signedNormalized((value >>> 10) & 0x3ff, 10);
                    w = signedNormalized((value >>> 20) & 0x3ff, 10);
                    q = (value >>> 30) / 3;
                }
                const out = destOffset + i * 8;
                writeHalf(dest, out, u);
                writeHalf(dest, out + 2, v);
                writeHalf(dest, out + 4, w);
                writeHalf(dest, out + 6, q);
                continue;
            }
            switch (format) {
            case D3DFMT_R8G8B8: {
                const at = sourceOffset + i * 3;
                b = source[at]; g = source[at + 1]; r = source[at + 2];
                a = 0xff;
                break;
            }
            case D3DFMT_A8R8G8B8:
            case D3DFMT_X8R8G8B8: {
                const value = source[sourceOffset + i * 4] |
                    (source[sourceOffset + i * 4 + 1] << 8) |
                    (source[sourceOffset + i * 4 + 2] << 16) |
                    (source[sourceOffset + i * 4 + 3] << 24);
                b = value & 0xff; g = (value >>> 8) & 0xff;
                r = (value >>> 16) & 0xff;
                a = format === D3DFMT_A8R8G8B8 ? (value >>> 24) & 0xff : 0xff;
                break;
            }
            case D3DFMT_R5G6B5: {
                const value = source[sourceOffset + i * 2] |
                    (source[sourceOffset + i * 2 + 1] << 8);
                r = ((value >>> 11) & 0x1f) * 255 / 31;
                g = ((value >>> 5) & 0x3f) * 255 / 63;
                b = (value & 0x1f) * 255 / 31;
                a = 0xff;
                break;
            }
            case D3DFMT_X1R5G5B5:
            case D3DFMT_A1R5G5B5: {
                const value = source[sourceOffset + i * 2] |
                    (source[sourceOffset + i * 2 + 1] << 8);
                r = ((value >>> 10) & 0x1f) * 255 / 31;
                g = ((value >>> 5) & 0x1f) * 255 / 31;
                b = (value & 0x1f) * 255 / 31;
                a = format === D3DFMT_A1R5G5B5 ?
                    ((value >>> 15) & 0x1) * 255 : 0xff;
                break;
            }
            case D3DFMT_A4R4G4B4: {
                const value = source[sourceOffset + i * 2] |
                    (source[sourceOffset + i * 2 + 1] << 8);
                r = ((value >>> 8) & 0xf) * 255 / 15;
                g = ((value >>> 4) & 0xf) * 255 / 15;
                b = (value & 0xf) * 255 / 15;
                a = ((value >>> 12) & 0xf) * 255 / 15;
                break;
            }
            case D3DFMT_R3G3B2: {
                const value = source[sourceOffset + i];
                r = ((value >>> 5) & 0x7) * 255 / 7;
                g = ((value >>> 2) & 0x7) * 255 / 7;
                b = (value & 0x3) * 255 / 3;
                a = 0xff;
                break;
            }
            case D3DFMT_A8R3G3B2: {
                const at = sourceOffset + i * 2;
                const value = source[at];
                r = ((value >>> 5) & 0x7) * 255 / 7;
                g = ((value >>> 2) & 0x7) * 255 / 7;
                b = (value & 0x3) * 255 / 3;
                a = source[at + 1];
                break;
            }
            case D3DFMT_X4R4G4B4: {
                const value = source[sourceOffset + i * 2] |
                    (source[sourceOffset + i * 2 + 1] << 8);
                r = ((value >>> 8) & 0xf) * 17;
                g = ((value >>> 4) & 0xf) * 17;
                b = (value & 0xf) * 17;
                a = 0xff;
                break;
            }
            case D3DFMT_A8B8G8R8:
            case D3DFMT_X8B8G8R8: {
                const at = sourceOffset + i * 4;
                r = source[at]; g = source[at + 1]; b = source[at + 2];
                a = format === D3DFMT_A8B8G8R8 ? source[at + 3] : 0xff;
                break;
            }
            case D3DFMT_L8:
                r = g = b = source[sourceOffset + i];
                a = 0xff;
                break;
            case D3DFMT_A8:
                // A8 is the sole D3D9 format whose missing colour channels
                // default to zero rather than one.
                r = g = b = 0;
                a = source[sourceOffset + i];
                break;
            case D3DFMT_A8L8: {
                const at = sourceOffset + i * 2;
                r = g = b = source[at]; a = source[at + 1];
                break;
            }
            case D3DFMT_A4L4: {
                const value = source[sourceOffset + i];
                r = g = b = (value & 0xf) * 17;
                a = (value >>> 4) * 17;
                break;
            }
            case D3DFMT_V8U8: {
                const at = sourceOffset + i * 2;
                r = source[at]; g = source[at + 1]; b = a = 0x7f;
                break;
            }
            case D3DFMT_Q8W8V8U8: {
                const at = sourceOffset + i * 4;
                r = source[at]; g = source[at + 1];
                b = source[at + 2]; a = source[at + 3];
                break;
            }
            default:
                r = g = b = a = 0;
                break;
            }
            dest[destOffset + i * 4] = r | 0;
            dest[destOffset + i * 4 + 1] = g | 0;
            dest[destOffset + i * 4 + 2] = b | 0;
            dest[destOffset + i * 4 + 3] = a | 0;
        }
    }

    // Row-major multiply: out[row][col] = sum_k a[row][k] * b[k][col]. This is
    // D3D's own convention, so multiply4x4(W, V) chains the way a row vector
    // would travel through them (v * W * V). See uniformBufferFor for why no
    // transpose is needed when handing the result to WGSL.
    function multiply4x4(a, b) {
        const out = new Float32Array(16);
        for (let row = 0; row < 4; ++row) {
            for (let col = 0; col < 4; ++col) {
                let sum = 0;
                for (let k = 0; k < 4; ++k)
                    sum += a[row * 4 + k] * b[k * 4 + col];
                out[row * 4 + col] = sum;
            }
        }
        return out;
    }

    const IDENTITY4x4 = new Float32Array([
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
    ]);

    // The normal matrix for row-vector maths. A normal n must satisfy
    // n' = n * (M^-1)^T for the transformed geometry to keep its perpendicular
    // relationship, and for a 3x3 that expression reduces to the cofactor
    // matrix divided by the determinant -- no explicit inverse or transpose
    // needed. Only the upper 3x3 participates: a normal has w = 0, so the
    // translation row can never reach it.
    //
    // A singular matrix (a degenerate scale, which engines do produce for
    // collapsed geometry) has no inverse; falling back to identity keeps the
    // draw legal and unlit rather than filling the buffer with NaN, which would
    // propagate into the position and make the whole mesh vanish.
    function inverseTranspose3x3(m) {
        const a = m[0], b = m[1], c = m[2];
        const d = m[4], e = m[5], f = m[6];
        const g = m[8], h = m[9], i = m[10];
        const c00 = e * i - f * h, c01 = f * g - d * i, c02 = d * h - e * g;
        const determinant = a * c00 + b * c01 + c * c02;
        const out = new Float32Array(16);
        out[15] = 1;
        if (!determinant || !isFinite(determinant)) {
            out[0] = out[5] = out[10] = 1;
            return out;
        }
        const scale = 1 / determinant;
        out[0] = c00 * scale;
        out[1] = c01 * scale;
        out[2] = c02 * scale;
        out[4] = (c * h - b * i) * scale;
        out[5] = (a * i - c * g) * scale;
        out[6] = (b * g - a * h) * scale;
        out[8] = (b * f - c * e) * scale;
        out[9] = (c * d - a * f) * scale;
        out[10] = (a * e - b * d) * scale;
        return out;
    }

    // Row-vector transforms, matching multiply4x4's convention: v' = v * M.
    function transformPoint(m, v) {
        return [
            v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12],
            v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13],
            v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14],
        ];
    }

    function transformDirection(m, v) {
        return [
            v[0] * m[0] + v[1] * m[4] + v[2] * m[8],
            v[0] * m[1] + v[1] * m[5] + v[2] * m[9],
            v[0] * m[2] + v[1] * m[6] + v[2] * m[10],
        ];
    }

    function normalize3(v) {
        const length = Math.hypot(v[0], v[1], v[2]);
        return length > 0 ? [v[0] / length, v[1] / length, v[2] / length]
            : [0, 0, 1];
    }

    // What D3D9 lights with when the app never called SetMaterial. Not all
    // zeroes: a zero diffuse would render the mesh black, which looks like a
    // bug in the lighting rather than like missing state.
    const DEFAULT_MATERIAL = {
        diffuse: [1, 1, 1, 1], ambient: [1, 1, 1, 1],
        specular: [0, 0, 0, 0], emissive: [0, 0, 0, 0], power: 0,
    };

    // The inter-stage contract shared with translated shaders. Both stages
    // always agree on it, whichever of the four VS/PS combinations a draw
    // uses (see the file header).
    const VARYING_COUNT = shaderPipeline.VARYING_COUNT;
    const VARYING_COLOR0 = shaderPipeline.VARYING_COLOR0;
    const VARYING_TEXCOORD0 = shaderPipeline.VARYING_TEXCOORD0;

    // Vertex attribute locations the fixed-function vertex stage consumes.
    // These are assigned by *semantic*, not by the element's position in the
    // declaration array. M1 assigned them by iteration order and hardcoded
    // position/colour/texcoord as locations 0/1/2 in the WGSL, which agreed
    // only for declarations that happened to list the elements in that
    // order; a declaration with TEXCOORD before COLOR silently fed the
    // texcoord bytes into the colour attribute.
    // M3 widened this from three locations to twelve: fixed-function lighting
    // reads NORMAL, the specular material source can read COLOR1, and a
    // multi-stage cascade can reference all eight coordinate sets rather than
    // only TEXCOORD0.
    const FF_LOCATION_POSITION = 0;
    const FF_LOCATION_COLOR0 = 1;
    const FF_LOCATION_COLOR1 = 2;
    const FF_LOCATION_NORMAL = 3;
    const FF_LOCATION_TEXCOORD0 = 4; // .. 11 for TEXCOORD0..7
    const FF_LOCATION_PSIZE = 12;

    // D3D9's alpha test has no fixed-function equivalent in WebGPU: it has to
    // become a `discard` in the fragment shader, which means the comparison
    // and the reference value are baked into the shader and therefore into
    // the pipeline key. Returns "" when no test is needed.
    //
    // This matters far beyond a subtle shading difference. UI atlases and
    // billboarded foliage lean on alpha test to cut fully transparent texels;
    // without it those texels are drawn opaque, which reads as wrong or
    // missing texture on exactly the panels and edges that should be cut out.
    //
    // D3DCMPFUNC values are 1..8; the expression below is the *discard*
    // condition, i.e. the negation of "the fragment passes".
    function alphaTestDiscard(alphaTest, alphaExpression) {
        if (!alphaTest || !alphaTest.enabled) return "";
        const reference = (alphaTest.reference / 255).toFixed(6);
        const condition = {
            1: "true",                                        // NEVER
            2: "!(" + alphaExpression + " < " + reference + ")",   // LESS
            3: "!(" + alphaExpression + " == " + reference + ")",  // EQUAL
            4: "!(" + alphaExpression + " <= " + reference + ")",  // LESSEQUAL
            5: "!(" + alphaExpression + " > " + reference + ")",   // GREATER
            6: "!(" + alphaExpression + " != " + reference + ")",  // NOTEQUAL
            7: "!(" + alphaExpression + " >= " + reference + ")",  // GREATEREQUAL
        }[alphaTest.func];
        if (!condition) return ""; // ALWAYS (8) and anything unknown: no test
        return "    if (" + condition + ") { discard; }\n";
    }

    function fixedFunctionLocationFor(element) {
        if (element.usage === DECLUSAGE_POSITION ||
                element.usage === DECLUSAGE_POSITIONT)
            return element.usageIndex === 0 ? FF_LOCATION_POSITION : -1;
        if (element.usage === DECLUSAGE_COLOR)
            return element.usageIndex === 0 ? FF_LOCATION_COLOR0
                : (element.usageIndex === 1 ? FF_LOCATION_COLOR1 : -1);
        if (element.usage === DECLUSAGE_NORMAL)
            return element.usageIndex === 0 ? FF_LOCATION_NORMAL : -1;
        if (element.usage === DECLUSAGE_TEXCOORD &&
                element.usageIndex < MAX_TEXCOORD_SETS)
            return FF_LOCATION_TEXCOORD0 + element.usageIndex;
        if (element.usage === DECLUSAGE_PSIZE)
            return element.usageIndex === 0 ? FF_LOCATION_PSIZE : -1;
        // Skinning usages are accepted by the guest's declaration validator so
        // lit or skinned vertex data does not have to be reformatted, but no
        // fixed-function stage reads them.
        return -1;
    }

    // The WGSL declaration for a vertex attribute is always vec4<f32>: WebGPU
    // fills the components a narrower vertex format does not supply with
    // (_, 0, 0, 1), which is exactly D3D9's rule for a FLOAT3 POSITION or a
    // FLOAT2 texcoord. One declared type per location also keeps a shader
    // module independent of which declaration is bound with it.
    function vertexInputDeclaration(location) {
        return "@location(" + location + ") in" + location + ": vec4<f32>";
    }

    // ---- fixed-function uniform blocks ----
    //
    // Both fixed-function stages get a uniform block whose *shape follows the
    // signature*: an unlit untextured draw uploads 80 bytes, one with eight
    // lights and three texture-coordinate transforms uploads over a kilobyte.
    // The shape has to vary because a uniform buffer is built and written per
    // draw (see constantBufferFor), so paying the worst case on every draw
    // would multiply a War3 session's ~100k draws by an order of magnitude of
    // upload traffic for state most of them do not use.
    //
    // One field table drives both the WGSL struct text and the JS writer, so
    // the two cannot drift. That matters more than the usual DRY argument: a
    // mismatched offset here produces a garbage matrix or a black light, not a
    // compile error, which is the class of bug that costs a day to locate.
    //
    // Every field is a vec4, a mat4x4 or an array of a 16-multiple struct, so
    // all of them align to 16 in WGSL's uniform address space and offsets
    // accumulate by plain size with no padding rules to apply. assertAligned
    // holds us to that.
    function uniformBlockLayout(fields) {
        let offset = 0;
        const entries = [];
        for (const field of fields) {
            if (!field) continue;
            if (field.bytes % 16 !== 0 || offset % 16 !== 0)
                throw new Error("fixed-function uniform field " + field.name +
                    " is not 16-byte aligned; WGSL uniform layout would " +
                    "silently disagree with the writer");
            entries.push({ name: field.name, type: field.type,
                bytes: field.bytes, offset, source: field.source });
            offset += field.bytes;
        }
        return { entries, byteLength: Math.max(16, offset),
            byName: new Map(entries.map(entry => [entry.name, entry])) };
    }

    function uniformBlockStruct(structName, layout) {
        // A zero-field block still has to be a legal struct, and every
        // signature that produces one also produces a shader that reads
        // nothing from it, so a dummy vec4 costs one binding slot and no
        // correctness.
        const body = layout.entries.length
            ? layout.entries.map(entry =>
                "    " + entry.name + ": " + entry.type + ",").join("\n")
            : "    _unused: vec4<f32>,";
        return "struct " + structName + " {\n" + body + "\n};";
    }

    // The vertex block. `source` names what fills the field; writeFixedVertex-
    // Uniforms below is the single place that knows how.
    // D3D9 rasterises with the sample point at a pixel's integer corner; WebGPU,
    // like D3D10 and everything after it, samples at the pixel centre -- half a
    // pixel further along both axes. A D3D9 title blitting UI 1:1 has already
    // subtracted that half pixel itself (the "Directly Mapping Texels to Pixels"
    // adjustment every 2000s-era engine carries), so replaying its geometry
    // unchanged puts every sample exactly on a texel boundary and bilinear
    // filtering returns the mean of two texels. 3D art does not care -- nothing
    // in it is pixel-aligned to begin with -- but 12px CJK glyphs turn to mush,
    // which is exactly the split Kart Rider showed: a clean track and an
    // unreadable shop.
    //
    // Adding the half pixel back in clip space, scaled by w so it stays half a
    // pixel at any depth, is what wined3d's posFixup and DXVK's half-pixel
    // offset both do. Screen y grows downward while NDC y grows upward, hence
    // the sign flip on the second component.
    const HALF_PIXEL_OFFSET_BODY =
        "    result.position = vec4<f32>(\n" +
        "        result.position.x + result.position.w / uniforms.viewport.x,\n" +
        "        result.position.y - result.position.w / uniforms.viewport.y,\n" +
        "        result.position.zw);";

    function fixedVertexUniformLayout(signature) {
        const fields = [
            { name: "world_view_projection", type: "mat4x4<f32>", bytes: 64 },
            { name: "viewport", type: "vec4<f32>", bytes: 16 },
        ];
        if (signature.needsViewSpace) {
            fields.push({ name: "world_view", type: "mat4x4<f32>", bytes: 64 });
            // Inverse-transpose of world_view's upper 3x3, widened to a mat4 so
            // it obeys the same 16-byte rule as everything else here.
            fields.push({ name: "normal_matrix", type: "mat4x4<f32>", bytes: 64 });
        }
        if (signature.fogMode)
            fields.push({ name: "fog_params", type: "vec4<f32>", bytes: 16 });
        for (const stage of signature.coordStages) {
            if (stage.transformCount)
                fields.push({ name: "texture_transform" + stage.index,
                    type: "mat4x4<f32>", bytes: 64, source: stage.index });
        }
        if (signature.lighting) {
            fields.push(
                { name: "material_diffuse", type: "vec4<f32>", bytes: 16 },
                { name: "material_ambient", type: "vec4<f32>", bytes: 16 },
                { name: "material_specular", type: "vec4<f32>", bytes: 16 },
                { name: "material_emissive", type: "vec4<f32>", bytes: 16 },
                // xyz = D3DRS_AMBIENT, w = the material's specular power.
                { name: "ambient_power", type: "vec4<f32>", bytes: 16 });
            if (signature.lighting.lights.length)
                fields.push({ name: "lights",
                    type: "array<D9Light, " + signature.lighting.lights.length + ">",
                    bytes: 112 * signature.lighting.lights.length });
        }
        if (signature.clipPlaneCount)
            fields.push({ name: "clip_planes",
                type: "array<vec4<f32>, " + signature.clipPlaneCount + ">",
                bytes: 16 * signature.clipPlaneCount });
        if (signature.pointExpansion) {
            fields.push(
                { name: "point_viewport", type: "vec4<f32>", bytes: 16 },
                // x=size, y=min, z=max, w reserved.
                { name: "point_params", type: "vec4<f32>", bytes: 16 });
            if (signature.pointScale)
                fields.push({ name: "point_scale", type: "vec4<f32>", bytes: 16 });
        }
        return uniformBlockLayout(fields);
    }

    // The pixel block: only what the texture cascade and the fog blend read.
    function fixedPixelUniformLayout(signature) {
        const fields = [];
        if (signature.fogMode)
            fields.push({ name: "fog_color", type: "vec4<f32>", bytes: 16 });
        if (signature.usesTextureFactor)
            fields.push({ name: "texture_factor", type: "vec4<f32>", bytes: 16 });
        for (const stage of signature.stages) {
            if (stage.usesConstant)
                fields.push({ name: "stage_constant" + stage.index,
                    type: "vec4<f32>", bytes: 16, source: stage.index });
        }
        return uniformBlockLayout(fields);
    }

    // ---- D3DTEXTUREOP -> WGSL ----
    //
    // D3D9's texture cascade runs the colour and alpha channels through
    // *separate* operations with separate arguments, so each operation is
    // emitted twice: once over vec3 and once over f32. `blend` names the alpha
    // an op blends with (diffuse/texture/factor/current), which the caller
    // supplies as an already-built scalar expression.
    //
    // Only the operations fill_caps() lists in TextureOpCaps appear here.
    // Returning null makes the caller count the draw and fall back to
    // SELECTARG1 rather than inventing an approximation.
    function textureOpExpression(op, type, args, blend) {
        const one = type === "vec3<f32>" ? "vec3<f32>(1.0)" : "1.0";
        const half = type === "vec3<f32>" ? "vec3<f32>(0.5)" : "0.5";
        const a0 = args[0], a1 = args[1], a2 = args[2];
        switch (op) {
        case D3DTOP_SELECTARG1: return a1;
        case D3DTOP_SELECTARG2: return a2;
        case D3DTOP_MODULATE: return "(" + a1 + " * " + a2 + ")";
        case D3DTOP_MODULATE2X: return "((" + a1 + " * " + a2 + ") * 2.0)";
        case D3DTOP_MODULATE4X: return "((" + a1 + " * " + a2 + ") * 4.0)";
        case D3DTOP_ADD: return "(" + a1 + " + " + a2 + ")";
        case D3DTOP_ADDSIGNED:
            return "(" + a1 + " + " + a2 + " - " + half + ")";
        case D3DTOP_ADDSIGNED2X:
            return "((" + a1 + " + " + a2 + " - " + half + ") * 2.0)";
        case D3DTOP_SUBTRACT: return "(" + a1 + " - " + a2 + ")";
        case D3DTOP_ADDSMOOTH:
            return "(" + a1 + " + " + a2 + " * (" + one + " - " + a1 + "))";
        case D3DTOP_BLENDDIFFUSEALPHA:
        case D3DTOP_BLENDTEXTUREALPHA:
        case D3DTOP_BLENDFACTORALPHA:
        case D3DTOP_BLENDCURRENTALPHA:
            return "mix(" + a2 + ", " + a1 + ", " + blend + ")";
        case D3DTOP_BLENDTEXTUREALPHAPM:
            // Pre-multiplied: arg1 already carries the alpha it was scaled by.
            return "(" + a1 + " + " + a2 + " * (1.0 - " + blend + "))";
        case D3DTOP_DOTPRODUCT3: {
            // Signed-scaled dot product replicated to every channel including
            // alpha, which is why the alpha form is the same expression.
            const dot = "(4.0 * dot(" + args.rgb1 + " - vec3<f32>(0.5), " +
                args.rgb2 + " - vec3<f32>(0.5)))";
            return type === "vec3<f32>" ? "vec3<f32>(" + dot + ")" : dot;
        }
        case D3DTOP_MULTIPLYADD:
            return "(" + a0 + " + " + a1 + " * " + a2 + ")";
        case D3DTOP_LERP:
            return "mix(" + a2 + ", " + a1 + ", " + a0 + ")";
        default:
            return null;
        }
    }

    // Fixed-function vertex stage: position transform, optional lighting,
    // per-stage texture coordinate generation/transform and fog, all written
    // into the shared varying set. `signature` comes from
    // fixedFunctionVertexSignature() plus the state-derived fields
    // programFor() adds.
    function buildFixedFunctionVertexShader(signature) {
        const layout = fixedVertexUniformLayout(signature);
        const lighting = signature.lighting;
        const position = "in" + FF_LOCATION_POSITION;

        const parameters = [vertexInputDeclaration(FF_LOCATION_POSITION)];
        if (signature.hasColor)
            parameters.push(vertexInputDeclaration(FF_LOCATION_COLOR0));
        if (signature.hasColor1)
            parameters.push(vertexInputDeclaration(FF_LOCATION_COLOR1));
        if (signature.hasNormal)
            parameters.push(vertexInputDeclaration(FF_LOCATION_NORMAL));
        if (signature.hasPointSize)
            parameters.push(vertexInputDeclaration(FF_LOCATION_PSIZE));
        for (const set of signature.texCoordSets)
            parameters.push(vertexInputDeclaration(FF_LOCATION_TEXCOORD0 + set));
        if (signature.pointExpansion)
            parameters.push("@builtin(vertex_index) d9_vertex_index: u32");

        const varyings = [];
        for (let slot = 0; slot < VARYING_COUNT; ++slot)
            varyings.push("    @location(" + slot + ") varying" + slot + ": vec4<f32>,");

        // XYZRHW ("screen") vertices arrive already in viewport pixel space and
        // bypass the world/view/projection chain entirely. D3D9 also treats
        // them as already lit, which is why lighting is forced off for them.
        // XYZRHW ("screen") coordinates are absolute render-target pixels, not
        // pixels relative to the viewport -- so the viewport's origin has to
        // come off before normalising, because setViewport puts it back when it
        // maps NDC into the viewport rect. Omitting it made the two cancel only
        // for a viewport at 0,0, which is every full-screen UI pass and hid the
        // bug completely. Kart Rider draws its shop item previews through
        // 110x109 viewports at x=368..636, and pre-transformed geometry there
        // landed several viewport-widths outside the box and was clipped away:
        // the panels whose contents are entirely pre-transformed came out
        // empty, while the one item drawn from world-space geometry rendered
        // perfectly. Same fix as wined3d's transformed-position projection
        // matrix, which carries the -2x/w term for exactly this reason.
        const positionBody = (signature.positionType === "screen"
            ? `    let viewport = uniforms.viewport;
    let ndc_x = ((${position}.x - viewport.z) / viewport.x) * 2.0 - 1.0;
    let ndc_y = 1.0 - ((${position}.y - viewport.w) / viewport.y) * 2.0;
    result.position = vec4<f32>(ndc_x, ndc_y, ${position}.z, 1.0);`
            : `    result.position = uniforms.world_view_projection * ${position};`)
            + "\n" + HALF_PIXEL_OFFSET_BODY;

        // View space is where D3D9 lights live and where the camera-space
        // coordinate generation modes are defined, so one block serves both.
        // The light positions/directions themselves arrive already multiplied by
        // the view matrix (see writeFixedVertexUniforms) rather than as a second
        // matrix in the shader.
        let viewSpaceBody = "";
        if (signature.needsViewSpace) {
            viewSpaceBody = "    let position_view = uniforms.world_view * " +
                position + ";\n";
            if (signature.hasNormal) {
                viewSpaceBody += "    var normal_view = (uniforms.normal_matrix" +
                    " * vec4<f32>(in" + FF_LOCATION_NORMAL + ".xyz, 0.0)).xyz;\n";
                // D3DRS_NORMALIZENORMALS is honoured rather than always
                // normalising: D3D9 genuinely does not renormalise unless asked,
                // so a scaled world matrix produces over- or under-bright
                // lighting, and silently fixing that would make this renderer
                // disagree with the reference for content tuned against it.
                if (signature.normalizeNormals)
                    viewSpaceBody += "    normal_view = normalize(normal_view);\n";
            } else {
                // A declaration with no normal cannot be lit; D3D9 uses (0,0,0),
                // which zeroes every N.L term and leaves ambient + emissive.
                viewSpaceBody += "    let normal_view = vec3<f32>(0.0);\n";
            }
        }

        // Vertex colours, before any material-source selection.
        const vertexDiffuse = signature.hasColor
            ? "in" + FF_LOCATION_COLOR0 + (signature.colorIsBGRA ? ".bgra" : "")
            : "vec4<f32>(1.0, 1.0, 1.0, 1.0)";
        const vertexSpecular = signature.hasColor1
            ? "in" + FF_LOCATION_COLOR1 + (signature.color1IsBGRA ? ".bgra" : "")
            : "vec4<f32>(0.0, 0.0, 0.0, 0.0)";

        let colorBody = "    let vertex_diffuse = " + vertexDiffuse + ";\n" +
            "    let vertex_specular = " + vertexSpecular + ";\n";
        if (lighting) {
            // D3DRS_COLORVERTEX off forces every channel to the material;
            // otherwise D3DRS_*MATERIALSOURCE picks between the material and
            // the two vertex colours. The D3D9 defaults are COLOR1 for diffuse
            // and COLOR2 for specular, which is why a lit mesh with per-vertex
            // colours is tinted by them rather than by the material alone.
            const sourceExpression = (source, materialField) => {
                if (!lighting.colorVertex) return "uniforms." + materialField;
                if (source === D3DMCS_COLOR1) return "vertex_diffuse";
                if (source === D3DMCS_COLOR2) return "vertex_specular";
                return "uniforms." + materialField;
            };
            colorBody +=
                "    let material_diffuse = " +
                    sourceExpression(lighting.diffuseSource, "material_diffuse") + ";\n" +
                "    let material_ambient = " +
                    sourceExpression(lighting.ambientSource, "material_ambient") + ";\n" +
                "    let material_specular = " +
                    sourceExpression(lighting.specularSource, "material_specular") + ";\n" +
                "    let material_emissive = " +
                    sourceExpression(lighting.emissiveSource, "material_emissive") + ";\n" +
                "    var total_diffuse = vec3<f32>(0.0);\n" +
                "    var total_ambient = uniforms.ambient_power.xyz;\n" +
                "    var total_specular = vec3<f32>(0.0);\n" +
                // Without D3DRS_LOCALVIEWER the eye vector is the constant
                // (0,0,-1) of D3D9's left-handed view space, which is the
                // cheaper approximation engines of this era normally leave on.
                "    let eye_direction = " + (lighting.localViewer
                    ? "normalize(-position_view.xyz)"
                    : "vec3<f32>(0.0, 0.0, -1.0)") + ";\n";
            lighting.lights.forEach((light, index) => {
                colorBody += "    {\n" +
                    "        let light = uniforms.lights[" + index + "];\n";
                if (light.type === D3DLIGHT_DIRECTIONAL) {
                    colorBody +=
                        "        let light_direction = -light.direction.xyz;\n" +
                        "        let attenuation = 1.0;\n";
                } else {
                    colorBody +=
                        "        let to_light = light.position.xyz - position_view.xyz;\n" +
                        "        let light_distance = length(to_light);\n" +
                        "        let light_direction = select(vec3<f32>(0.0, 0.0, 1.0),\n" +
                        "            to_light / max(light_distance, 1e-6), light_distance > 0.0);\n" +
                        // D3D9 attenuates by 1/(a0 + a1*d + a2*d^2) and drops
                        // the light entirely past its range.
                        "        var range_attenuation = 1.0 / max(light.attenuation.x +\n" +
                        "            light.attenuation.y * light_distance +\n" +
                        "            light.attenuation.z * light_distance * light_distance, 1e-6);\n" +
                        "        range_attenuation = min(range_attenuation, 1.0);\n" +
                        "        if (light_distance > light.range_falloff.x) {\n" +
                        "            range_attenuation = 0.0;\n" +
                        "        }\n";
                    if (light.type === D3DLIGHT_SPOT) {
                        // range_falloff = (range, falloff, cos(theta/2), cos(phi/2)).
                        colorBody +=
                            "        let rho = dot(light.direction.xyz, -light_direction);\n" +
                            "        var spot = 0.0;\n" +
                            "        if (rho > light.range_falloff.z) {\n" +
                            "            spot = 1.0;\n" +
                            "        } else if (rho > light.range_falloff.w) {\n" +
                            "            spot = pow(max((rho - light.range_falloff.w) /\n" +
                            "                max(light.range_falloff.z - light.range_falloff.w, 1e-6),\n" +
                            "                0.0), max(light.range_falloff.y, 1e-4));\n" +
                            "        }\n" +
                            "        let attenuation = range_attenuation * spot;\n";
                    } else {
                        colorBody += "        let attenuation = range_attenuation;\n";
                    }
                }
                colorBody +=
                    "        let n_dot_l = max(dot(normal_view, light_direction), 0.0);\n" +
                    "        total_diffuse = total_diffuse + light.diffuse.xyz *\n" +
                    "            (n_dot_l * attenuation);\n" +
                    "        total_ambient = total_ambient + light.ambient.xyz * attenuation;\n";
                if (lighting.specularEnable) {
                    colorBody +=
                        "        let half_vector = normalize(light_direction + eye_direction);\n" +
                        "        let n_dot_h = max(dot(normal_view, half_vector), 0.0);\n" +
                        // Back faces contribute no highlight, and pow(0, p) is
                        // only well-defined for p > 0, hence both guards.
                        "        let highlight = select(0.0,\n" +
                        "            pow(n_dot_h, max(uniforms.ambient_power.w, 1e-4)),\n" +
                        "            n_dot_l > 0.0);\n" +
                        "        total_specular = total_specular + light.specular.xyz *\n" +
                        "            (highlight * attenuation);\n";
                }
                colorBody += "    }\n";
            });
            colorBody +=
                "    let lit_rgb = clamp(total_ambient * material_ambient.xyz +\n" +
                "        total_diffuse * material_diffuse.xyz + material_emissive.xyz,\n" +
                "        vec3<f32>(0.0), vec3<f32>(1.0));\n" +
                // D3D9 takes the lit vertex's alpha from the diffuse material
                // channel only -- lighting never affects alpha.
                "    let out_diffuse = vec4<f32>(lit_rgb, clamp(material_diffuse.a, 0.0, 1.0));\n" +
                "    let out_specular = vec4<f32>(clamp(total_specular *\n" +
                "        material_specular.xyz, vec3<f32>(0.0), vec3<f32>(1.0)), 0.0);\n";
        } else {
            colorBody += "    let out_diffuse = vertex_diffuse;\n" +
                "    let out_specular = vertex_specular;\n";
        }

        // Fog distance is the clip-space w, which for a standard projection is
        // the eye-space depth -- exactly D3DPRASTERCAPS_WFOG, which the guest
        // advertises. D3DRS_RANGEFOGENABLE asks for true radial distance
        // instead, which removes the "fog thins towards the screen edges"
        // artefact of depth-based fog.
        const fogDistance = signature.fogRange && signature.needsViewSpace
            ? "length(position_view.xyz)" : "abs(result.position.w)";
        const fogFactor = {
            [D3DFOG_LINEAR]: "clamp((uniforms.fog_params.y - fog_distance) / " +
                "max(uniforms.fog_params.y - uniforms.fog_params.x, 1e-6), 0.0, 1.0)",
            [D3DFOG_EXP]: "clamp(exp(-(uniforms.fog_params.z * fog_distance)), 0.0, 1.0)",
            [D3DFOG_EXP2]: "clamp(exp(-((uniforms.fog_params.z * fog_distance) * " +
                "(uniforms.fog_params.z * fog_distance))), 0.0, 1.0)",
        }[signature.fogMode];
        const fogBody = fogFactor
            ? "    let fog_distance = " + fogDistance + ";\n" +
              "    result.varying" + shaderPipeline.VARYING_FOG +
              " = vec4<f32>(" + fogFactor + ", 0.0, 0.0, 0.0);\n"
            : "";

        // One texture-coordinate varying per *stage*, not per declared
        // coordinate set: D3DTSS_TEXCOORDINDEX chooses which set (or which
        // generated vector) feeds a stage, and D3DTS_TEXTURE0+n transforms it,
        // so the varying a pixel stage reads for stage n is already the final
        // coordinate. That is also what D3D9 hands a real pixel shader, whose
        // texcoord input n corresponds to texture stage n.
        let coordBody = "";
        for (const stage of signature.coordStages) {
            let raw;
            switch (stage.tciMode) {
            case D3DTSS_TCI_CAMERASPACENORMAL:
                raw = "vec4<f32>(normal_view, 1.0)";
                break;
            case D3DTSS_TCI_CAMERASPACEPOSITION:
                raw = "vec4<f32>(position_view.xyz, 1.0)";
                break;
            case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR:
                raw = "vec4<f32>(reflect(normalize(position_view.xyz), " +
                    "normalize(normal_view)), 1.0)";
                break;
            default:
                raw = signature.texCoordSets.includes(stage.texCoordIndex)
                    ? "in" + (FF_LOCATION_TEXCOORD0 + stage.texCoordIndex)
                    : "vec4<f32>(0.0, 0.0, 0.0, 1.0)";
                break;
            }
            let expression = raw;
            if (stage.transformCount) {
                // The coordinate enters the matrix as a *row* vector padded
                // with ones, which is why a scrolling animation puts its offset
                // in row 3 (_31/_32) for the two-component form. The matrix is
                // uploaded untransposed for the same reason the WVP is: D3D's
                // row-major bytes read back as the transpose in WGSL, and that
                // transpose is exactly the column-vector form of D3D's
                // row-vector multiply.
                const padded = {
                    1: "vec4<f32>(" + raw + ".x, 1.0, 1.0, 1.0)",
                    2: "vec4<f32>(" + raw + ".xy, 1.0, 1.0)",
                    3: "vec4<f32>(" + raw + ".xyz, 1.0)",
                    4: raw,
                }[stage.transformCount] || raw;
                expression = "(uniforms.texture_transform" + stage.index +
                    " * " + padded + ")";
            }
            coordBody += "    result.varying" +
                (VARYING_TEXCOORD0 + stage.index) + " = " + expression + ";\n";
        }

        let pointBody = "";
        if (signature.pointExpansion) {
            const baseSize = signature.hasPointSize
                ? "in" + FF_LOCATION_PSIZE + ".x" : "uniforms.point_params.x";
            pointBody +=
                "    let d9_point_uvs = array<vec2<f32>, 6>(\n" +
                "        vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0),\n" +
                "        vec2<f32>(0.0, 1.0), vec2<f32>(0.0, 1.0),\n" +
                "        vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0));\n" +
                "    let d9_point_uv = d9_point_uvs[d9_vertex_index % 6u];\n" +
                "    var d9_point_size = " + baseSize + ";\n";
            if (signature.pointScale) {
                pointBody +=
                    "    let d9_point_distance = length(position_view.xyz);\n" +
                    "    let d9_point_denom = max(uniforms.point_scale.x +\n" +
                    "        uniforms.point_scale.y * d9_point_distance +\n" +
                    "        uniforms.point_scale.z * d9_point_distance * d9_point_distance, 1e-6);\n" +
                    "    d9_point_size = d9_point_size * uniforms.point_viewport.y *\n" +
                    "        inverseSqrt(d9_point_denom);\n";
            }
            pointBody +=
                "    d9_point_size = clamp(d9_point_size, uniforms.point_params.y,\n" +
                "        max(uniforms.point_params.y, uniforms.point_params.z));\n" +
                "    let d9_point_ndc = vec2<f32>(\n" +
                "        (d9_point_uv.x * 2.0 - 1.0) * d9_point_size / uniforms.point_viewport.x,\n" +
                "        (1.0 - d9_point_uv.y * 2.0) * d9_point_size / uniforms.point_viewport.y);\n" +
                "    result.position = vec4<f32>(result.position.xy +\n" +
                "        d9_point_ndc * result.position.w, result.position.zw);\n";
            if (signature.pointSprite) {
                for (let stage = 0; stage < MAX_TEXCOORD_SETS; ++stage) {
                    pointBody += "    result.varying" +
                        (VARYING_TEXCOORD0 + stage) +
                        " = vec4<f32>(d9_point_uv, 0.0, 1.0);\n";
                }
            }
        }

        const lightStruct = lighting && lighting.lights.length ? `struct D9Light {
    diffuse: vec4<f32>,
    specular: vec4<f32>,
    ambient: vec4<f32>,
    position: vec4<f32>,
    direction: vec4<f32>,
    range_falloff: vec4<f32>,
    attenuation: vec4<f32>,
};
` : "";

        return `${lightStruct}${uniformBlockStruct("D9FixedUniforms", layout)}
@group(0) @binding(0) var<uniform> uniforms: D9FixedUniforms;

struct D9VertexOutput {
    @builtin(position) position: vec4<f32>,
${varyings.join("\n")}
};

@vertex
fn d9_vs_main(${parameters.join(", ")}) -> D9VertexOutput {
    var result: D9VertexOutput;
${positionBody}
${varyings.map((_, slot) =>
        "    result.varying" + slot + " = vec4<f32>(0.0);").join("\n")}
${viewSpaceBody}${colorBody}    result.varying${VARYING_COLOR0} = out_diffuse;
    result.varying${shaderPipeline.VARYING_COLOR1} = out_specular;
${coordBody}${fogBody}${pointBody}    return result;
}
`;
    }

    // Fixed-function pixel stage: D3D9's texture blending cascade.
    //
    // M1/M2 hardcoded one stage of MODULATE(texture, diffuse). This walks the
    // real cascade instead: stages 0..N-1 (N ends at the first D3DTOP_DISABLE),
    // each with its own colour and alpha operation over arguments drawn from
    // diffuse, the running result, the stage's texture, D3DRS_TEXTUREFACTOR,
    // the specular colour, a scratch register and the stage's own constant --
    // which is what terrain splatting, detail texturing and light mapping are
    // actually built from. Every stage saturates its result, as D3D9 does.
    //
    // debugMode (null in normal operation) replaces the output with something
    // unambiguous, so "the screen is black" can be attributed to a specific
    // input rather than guessed at:
    //   "solid"   flat green  -- proves geometry coverage and that fragments land
    //   "color"   vertex colour only, textures ignored
    //   "texture" stage 0's texture sample only, vertex colour ignored
    //   "uv"      stage 0's texcoords as red/green -- shows whether UVs are sane
    function buildFixedFunctionPixelShader(signature, debugMode) {
        const layout = fixedPixelUniformLayout(signature);
        const diffuse = "stage_in.varying" + VARYING_COLOR0;
        const specular = "stage_in.varying" + shaderPipeline.VARYING_COLOR1;

        // Per-stage texture declarations. Cube and volume textures reach the
        // fixed-function cascade too (environment mapping, volume fog), so the
        // WGSL texture type and the coordinate arity both come from whatever is
        // actually bound.
        const declarations = [];
        const samples = [];
        for (const stage of signature.stages) {
            if (!stage.samplesTexture) continue;
            const wgslType = { cube: "texture_cube<f32>", "3d": "texture_3d<f32>" }[
                stage.textureType] || "texture_2d<f32>";
            declarations.push("@group(0) @binding(" + (2 + stage.index * 2) +
                ") var d9_tex" + stage.index + ": " + wgslType + ";");
            declarations.push("@group(0) @binding(" + (3 + stage.index * 2) +
                ") var d9_smp" + stage.index + ": sampler;");
            const coord = "stage_in.varying" +
                (VARYING_TEXCOORD0 + stage.coordVarying);
            const components = stage.textureType === "2d" ? 2 : 3;
            let coordExpression = coord +
                (components === 2 ? ".xy" : ".xyz");
            if (stage.projected) {
                // D3DTTFF_PROJECTED divides by the last component of the
                // transformed coordinate, so the divisor follows the transform's
                // component count, not the texture's dimensionality.
                const divisor = coord + "." +
                    ["x", "x", "y", "z", "w"][stage.transformCount || 4];
                // Preserve the sign of q. Clamping a negative divisor with
                // max(q, epsilon) turns every behind-projector coordinate into
                // an enormous positive UV; Warcraft III's projected tree
                // shadows then sample the opaque edge texel over whole terrain
                // triangles, producing the characteristic black wedges.
                const safeDivisor = "select(-max(abs(" + divisor +
                    "), 1e-6), max(abs(" + divisor + "), 1e-6), " +
                    divisor + " >= 0.0)";
                coordExpression = "(" + coordExpression + " / (" +
                    safeDivisor + "))";
            }
            const sampled = "textureSample(d9_tex" + stage.index +
                ", d9_smp" + stage.index + ", " + coordExpression + ")";
            const borderAxes = stage.textureType === "cube" ? [] :
                (stage.textureType === "3d"
                    ? [[stage.addressU, "x"], [stage.addressV, "y"],
                       [stage.addressW, "z"]]
                    : [[stage.addressU, "x"], [stage.addressV, "y"]])
                .filter(axis => axis[0] === 4).map(axis => axis[1]);
            if (borderAxes.length) {
                // WebGPU has no border-colour sampler.  Clamp for the physical
                // sample, then replace every coordinate outside the D3D unit
                // domain on an axis whose addressing mode is BORDER.  This is
                // especially important after projected division: sampling an
                // opaque edge texel outside a shadow/fog projector turns whole
                // terrain triangles into black masks.
                const inside = borderAxes.map(axis => "(" + coordExpression +
                    ")." + axis + " >= 0.0 && (" + coordExpression + ")." +
                    axis + " <= 1.0").join(" && ");
                const color = stage.borderColor === undefined
                    ? 0 : stage.borderColor >>> 0;
                const component = shift =>
                    (((color >>> shift) & 0xff) / 255).toFixed(8);
                const border = "vec4<f32>(" + component(16) + ", " +
                    component(8) + ", " + component(0) + ", " +
                    component(24) + ")";
                samples.push("    let tex" + stage.index + " = select(" +
                    border + ", " + sampled + ", " + inside + ");");
            } else {
                samples.push("    let tex" + stage.index + " = " + sampled + ";");
            }
        }

        // The argument pool. `current` and `temp` are the two mutable registers
        // the cascade threads through, so they are read from variables the
        // stage loop below reassigns.
        function argumentExpression(argument, stageIndex, channel) {
            const selector = argument & D3DTA_SELECTMASK;
            let value;
            switch (selector) {
            case D3DTA_DIFFUSE: value = diffuse; break;
            case D3DTA_CURRENT: value = "current"; break;
            case D3DTA_TEXTURE: value = "tex" + stageIndex; break;
            case D3DTA_TFACTOR: value = "uniforms.texture_factor"; break;
            case D3DTA_SPECULAR: value = specular; break;
            case D3DTA_TEMP: value = "temp"; break;
            case D3DTA_CONSTANT: value = "uniforms.stage_constant" + stageIndex; break;
            default: value = diffuse; break;
            }
            // D3DTA_ALPHAREPLICATE broadcasts the argument's alpha over the
            // colour channels; D3DTA_COMPLEMENT inverts whatever came out.
            let expression;
            if (channel === "rgb")
                expression = (argument & D3DTA_ALPHAREPLICATE)
                    ? "vec3<f32>(" + value + ".a)" : value + ".rgb";
            else
                expression = value + ".a";
            if (argument & D3DTA_COMPLEMENT) {
                expression = channel === "rgb"
                    ? "(vec3<f32>(1.0) - " + expression + ")"
                    : "(1.0 - " + expression + ")";
            }
            return expression;
        }

        let cascadeBody = "    var current = " + diffuse + ";\n" +
            // D3D9 leaves the scratch register undefined until written; zero is
            // the one starting value that cannot make an unwritten read look
            // like meaningful data.
            "    var temp = vec4<f32>(0.0);\n";
        for (const stage of signature.stages) {
            const rgb = argument => argumentExpression(argument, stage.index, "rgb");
            const alpha = argument => argumentExpression(argument, stage.index, "a");
            const blendAlpha = {
                [D3DTOP_BLENDDIFFUSEALPHA]: diffuse + ".a",
                [D3DTOP_BLENDTEXTUREALPHA]: "tex" + stage.index + ".a",
                [D3DTOP_BLENDTEXTUREALPHAPM]: "tex" + stage.index + ".a",
                [D3DTOP_BLENDFACTORALPHA]: "uniforms.texture_factor.a",
                [D3DTOP_BLENDCURRENTALPHA]: "current.a",
            };
            const colorArgs = [rgb(stage.colorArg0), rgb(stage.colorArg1),
                rgb(stage.colorArg2)];
            colorArgs.rgb1 = rgb(stage.colorArg1);
            colorArgs.rgb2 = rgb(stage.colorArg2);
            const alphaArgs = [alpha(stage.alphaArg0), alpha(stage.alphaArg1),
                alpha(stage.alphaArg2)];
            alphaArgs.rgb1 = rgb(stage.colorArg1);
            alphaArgs.rgb2 = rgb(stage.colorArg2);
            const colorExpression = textureOpExpression(stage.colorOp,
                "vec3<f32>", colorArgs, blendAlpha[stage.colorOp] || "0.0");
            const alphaExpression = stage.alphaOp === D3DTOP_DISABLE ? null
                : textureOpExpression(stage.alphaOp, "f32", alphaArgs,
                    blendAlpha[stage.alphaOp] || "0.0");
            const destination = stage.resultArg === D3DTA_TEMP ? "temp" : "current";
            cascadeBody += "    {\n" +
                "        let stage_rgb = clamp(" +
                    (colorExpression || rgb(stage.colorArg1)) +
                    ", vec3<f32>(0.0), vec3<f32>(1.0));\n" +
                "        let stage_a = clamp(" +
                    (alphaExpression || destination + ".a") + ", 0.0, 1.0);\n" +
                "        " + destination + " = vec4<f32>(stage_rgb, stage_a);\n" +
                "    }\n";
        }

        let value = "current";
        if (debugMode === "solid") value = "vec4<f32>(0.0, 1.0, 0.0, 1.0)";
        else if (debugMode === "color")
            value = "vec4<f32>(" + diffuse + ".rgb, 1.0)";
        else if (debugMode === "uv")
            value = signature.stages.length
                ? "vec4<f32>(stage_in.varying" +
                    (VARYING_TEXCOORD0 + signature.stages[0].coordVarying) +
                    ".xy, 0.0, 1.0)"
                : "vec4<f32>(0.0, 0.0, 1.0, 1.0)";
        else if (debugMode === "texture")
            value = signature.stages.some(stage => stage.samplesTexture)
                ? "vec4<f32>(tex" + signature.stages.find(
                    stage => stage.samplesTexture).index + ".rgb, 1.0)"
                : "vec4<f32>(1.0, 0.0, 0.0, 1.0)";

        // D3D9 adds the specular colour after the texture cascade, before fog.
        const specularBody = signature.specularEnable && !debugMode
            ? "    result = vec4<f32>(clamp(result.rgb + " + specular +
              ".rgb, vec3<f32>(0.0), vec3<f32>(1.0)), result.a);\n"
            : "";
        const fogBody = signature.fogMode
            ? "    result = vec4<f32>(mix(uniforms.fog_color.rgb, result.rgb,\n" +
              "        clamp(stage_in.varying" + shaderPipeline.VARYING_FOG +
              ".x, 0.0, 1.0)), result.a);\n"
            : "";
        const varyings = [];
        for (let slot = 0; slot < VARYING_COUNT; ++slot)
            varyings.push("    @location(" + slot + ") varying" + slot + ": vec4<f32>,");

        return `${layout.entries.length
            ? uniformBlockStruct("D9FixedPixelUniforms", layout) +
              "\n@group(0) @binding(1) var<uniform> uniforms: D9FixedPixelUniforms;"
            : ""}
${declarations.join("\n")}

struct D9PixelInput {
    @builtin(position) position: vec4<f32>,
${varyings.join("\n")}
};

@fragment
fn d9_ps_main(stage_in: D9PixelInput) -> @location(0) vec4<f32> {
${samples.join("\n")}
${cascadeBody}    var result = ${value};
${specularBody}${alphaTestDiscard(signature.alphaTest, "result.a")}${fogBody}    return result;
}
`;
    }

    function createIndexedDBShaderCacheStorage(indexedDB) {
        const databaseName = "d9wg-shader-cache";
        const storeName = "snapshots";
        const open = () => new Promise((resolve, reject) => {
            const request = indexedDB.open(databaseName, 1);
            request.onupgradeneeded = () => {
                const db = request.result;
                if (!db.objectStoreNames.contains(storeName))
                    db.createObjectStore(storeName);
            };
            request.onsuccess = () => resolve(request.result);
            request.onerror = () => reject(request.error ||
                new Error("could not open the shader-cache database"));
            request.onblocked = () => reject(new Error(
                "shader-cache database upgrade is blocked by another page"));
        });
        return {
            async load(key) {
                const db = await open();
                try {
                    return await new Promise((resolve, reject) => {
                        const request = db.transaction(storeName, "readonly")
                            .objectStore(storeName).get(key);
                        request.onsuccess = () => resolve(request.result || null);
                        request.onerror = () => reject(request.error ||
                            new Error("could not read the shader cache"));
                    });
                } finally {
                    db.close();
                }
            },
            async save(key, payload) {
                const db = await open();
                try {
                    await new Promise((resolve, reject) => {
                        const transaction = db.transaction(storeName, "readwrite");
                        transaction.objectStore(storeName).put(payload, key);
                        transaction.oncomplete = () => resolve();
                        transaction.onerror = () => reject(transaction.error ||
                            new Error("could not write the shader cache"));
                        transaction.onabort = () => reject(transaction.error ||
                            new Error("shader-cache transaction was aborted"));
                    });
                } finally {
                    db.close();
                }
            },
        };
    }

    class D3D9WebGPUExecutor {
        constructor(canvas, options) {
            if (!canvas) throw new Error("D3D9 WebGPU canvas is required");
            this.canvas = canvas;
            this.options = options || {};
            this.gpu = this.options.gpu ||
                (global.navigator && global.navigator.gpu);
            this.adapter = this.options.adapter || null;
            this.device = this.options.device || null;
            this.context = this.options.context || null;
            this.format = this.options.format || null;
            this.devices = new Map();      // device_handle -> device state
            this.resources = new Map();    // resource_handle -> resource state
            this.pipelineCache = new Map(); // layout signature -> GPURenderPipeline
            this.bindGroupCache = new Map(); // GPU resources -> GPUBindGroup
            this.maxBindGroups = Math.max(256,
                this.options.maxBindGroups || 4096);
            this.uniformRingCapacity = Math.max(64 * 1024,
                this.options.uniformRingBytes || UNIFORM_RING_BYTES);
            this.uniformRing = null;
            this.uniformRingCursor = 0;
            this.objectIds = new WeakMap();
            this.nextObjectId = 1;
            // Bytecode hash -> {ok, wgsl, reflection}. Survives device loss:
            // WGSL text is not tied to a GPUDevice (plan 8.5), only the
            // GPUShaderModules in moduleCache are.
            this.shaderCache = new shaderPipeline.D3D9ShaderCache();
            this.shaderCacheStorageKey = "d9wg.shader-cache.m6.20260809.v1";
            let indexedDBStorage = null;
            try {
                if (!this.options.shaderCacheStorage && global.window &&
                        global.indexedDB) {
                    indexedDBStorage = createIndexedDBShaderCacheStorage(
                        global.indexedDB);
                }
            } catch (error) { /* persistence is optional for restricted origins */ }
            this.shaderCacheStorage = this.options.shaderCacheStorage ||
                indexedDBStorage;
            this.shaderCacheStorageBackend = this.options.shaderCacheStorage ?
                "injected" : (indexedDBStorage ? "indexeddb" : "memory");
            this.persistentShaderCachePromise = null;
            this.shaderCacheSaveTimer = null;
            this.shaderCacheDirty = false;
            this.shaderWorker = null;
            this.shaderWorkerRequests = new Map();
            this.shaderWorkerSerial = 0;
            this.moduleCache = new Map();  // wgsl -> GPUShaderModule
            this.samplerCache = new Map(); // sampler-state signature -> GPUSampler
            // D3D9 hardware cursor: bitmap, hotspot, position, visibility.
            this.cursor = { texture: null, view: null, width: 0, height: 0,
                hotspotX: 0, hotspotY: 0, x: 0, y: 0, visible: false,
                pipeline: null, sampler: null, uniform: null };
            this.fallbackTexture = null;
            this.fallbackView = null;
            // The guest process whose handles the resource table currently
            // holds (D9WGHello.session_id_*), or null before the first HELLO.
            this.sessionKey = null;
            this.frame = null;             // { ops, transientBuffers, serial }
            this.frameSerial = 0;
            this.readyPromise = null;
            this.work = Promise.resolve();
            this.failed = null;
            // Console-togglable diagnostics, e.g.
            //   v86gl.d3d9Executor.debug.forceClearColor = {r:1,g:0,b:1,a:1}
            // These exist to split "the canvas is not on screen" from "the
            // canvas is on screen but the drawn content is black", which the
            // stats alone cannot distinguish.
            this.debug = {
                forceClearColor: null,  // {r,g,b,a} overrides every Clear
                disableCull: false,     // force cullMode "none"
                disableDepthTest: false,// force depthCompare "always"
                shaderMode: null,       // "solid"|"color"|"texture"|"uv"
                // Clamps every sampler to the top mip level. If a texture
                // looks wrong because levels below 0 were never uploaded,
                // this makes it correct immediately -- which is the cheapest
                // way to confirm or rule out that cause.
                forceMipLevel0: false,
            };
            this.debug.dumpSmallTextures = o => this.dumpSmallTextures(o);
            this.debug.dumpPipelineStates = () => this.dumpPipelineStates();
            this.stats = {
                batches: 0, commands: 0, presents: 0, queueSubmits: 0,
                drawCalls: 0, indexedDrawCalls: 0, upDrawCalls: 0,
                pipelineCreations: 0, pipelineHits: 0,
                bindGroupCreations: 0, bindGroupHits: 0,
                bindGroupCacheEvictions: 0,
                uniformSlotReuses: 0, uniformRingOverflows: 0,
                unsupportedCommands: 0, malformedBatches: 0,
                droppedDraws: 0,
                guestReports: 0,
                texturesCreated: 0, textureUploads: 0, textureBytesUploaded: 0,
                texturePreviewsSkipped: 0,
                drawsWithTexture: 0, drawsWithFallbackTexture: 0,
                shadersTranslated: 0, shaderTranslationFailures: 0,
                shaderVariantsTranslated: 0,
                shaderModulesCreated: 0, shaderCompileErrors: 0,
                shaderCachePersistentLoads: 0,
                shaderCachePersistentSaves: 0,
                shaderCachePersistentFailures: 0,
                shaderWorkerCompiles: 0, shaderWorkerFallbacks: 0,
                samplersCreated: 0, samplerHits: 0,
                // Flicker diagnostics. WebGPU does not preserve a canvas's
                // contents across Present, so a frame that draws without ever
                // clearing the colour target composites on top of an
                // undefined older swapchain buffer -- which looks like
                // alternating frames, i.e. flicker. Likewise a present that
                // produced no GPU work at all leaves whatever was composited
                // last on screen. Both are legal D3D9 behaviour, so they are
                // counted rather than warned about.
                framesWithoutColorClear: 0, framesWithNoOps: 0,
                // Dynamic buffers renamed because a draw already recorded in
                // the same frame reads their previous contents (see
                // applyBufferUpdate). Zero means the deferred-draw path never
                // had a write-after-record hazard to begin with.
                bufferRenames: 0, textureUpdateHazards: 0,
                textureRenames: 0, textureFullCopyRenameBytes: 0,
                bufferFullCopyRenames: 0, bufferNoOverwriteWrites: 0,
                emptySurfaceReports: 0, surfaceChanges: 0, sessionChanges: 0,
                deviceLosses: 0, deviceRecoveries: 0,
                guestFeatureBits: 0, guestShaderModel2: false,
                windowStateChanges: 0, windowNotForegroundReports: 0,
                cursorUploads: 0, cursorDraws: 0,
                texturesRejected: 0,
                srgbTextureSamples: 0, srgbViewsCreated: 0,
                srgbTextureUnavailable: 0, srgbWriteRequests: 0,
                srgbWriteUnavailable: 0,
                cubeTexturesCreated: 0, renderTargetsCreated: 0,
                renderTargetBinds: 0, renderPasses: 0,
                depthTargetSizeMismatches: 0,
                blits: 0, blitsSkipped: 0, blitsThroughBackBuffer: 0,
                colorFills: 0,
                partialClears: 0,
                drawsWithScissor: 0,
                drawsWithIncompleteMipChain: 0,
                lastDrawTexture: 0,
                drawsWithUnsupportedTextureOp: 0,
                drawsWithTexCoordIndex: 0, drawsWithTextureTransform: 0,
                drawsWithUnmappedBlend: 0, drawsWithUnappliedFog: 0,
                drawsWithUnappliedLighting: 0,
                drawsWithUnappliedVertexBlend: 0,
                programmableDraws: 0, drawsSkippedForBadShader: 0,
                drawsWithCompactVertexInputs: 0,
                constantUploadBytes: 0,
                pointSpriteDraws: 0, pointSpriteInstances: 0,
                indexedPointExpansions: 0,
            };
            this.mrtAttachmentDraws = [0, 0, 0, 0, 0];
            this.lastFrameStats = {
                pipelineCreations: 0, bindGroupCreations: 0,
                queueSubmits: 0, renderPasses: 0,
            };
        }

        initialize() {
            if (this.readyPromise) return this.readyPromise;
            this.readyPromise = (async () => {
                // Persistent I/O must not delay adapter/device acquisition.
                // The first submitted batch awaits the same promise before it
                // can create a shader, so starting both jobs here preserves
                // cache hits without putting IndexedDB on the startup path.
                this.restorePersistentShaderCache();
                this.initializeShaderWorker();
                if (!this.device) {
                    if (!this.gpu || typeof this.gpu.requestAdapter !== "function")
                        throw new Error("WebGPU is unavailable");
                    this.adapter = this.adapter ||
                        await this.gpu.requestAdapter({ powerPreference: "high-performance" });
                    if (!this.adapter) throw new Error("WebGPU adapter request failed");
                    // A WebGPU device gets *no* optional features unless it asks
                    // for them by name at creation. Without this, every
                    // createTexture for a DXT format throws
                    // ("Use of the 'bc2-rgba-unorm' texture format requires the
                    // 'texture-compression-bc' feature"), which killed the whole
                    // batch and with it the frame -- and since DXT1/3/5 are the
                    // formats a 2002-and-later D3D9 game keeps almost all of its
                    // art in, that is most of the screen. The format table has
                    // listed BCn since M1, so this was a promise the device
                    // setup never kept.
                    const requested = [];
                    const features = this.adapter.features;
                    const supports = name => !!(features &&
                        typeof features.has === "function" && features.has(name));
                    if (supports("texture-compression-bc"))
                        requested.push("texture-compression-bc");
                    this.deviceFeatures = { bc: requested.includes(
                        "texture-compression-bc") };
                    this.device = await this.adapter.requestDevice(
                        requested.length ? { requiredFeatures: requested } : {});
                }
                // A device supplied by the caller (the fake device in tests, or
                // a shared one) reports its own features.
                if (!this.deviceFeatures) {
                    const features = this.device.features;
                    this.deviceFeatures = { bc: !!(features &&
                        typeof features.has === "function" &&
                        features.has("texture-compression-bc")) };
                }
                this.context = this.context || this.canvas.getContext("webgpu");
                if (!this.context) throw new Error("could not acquire a WebGPU canvas context");
                this.format = this.format || (this.gpu &&
                    typeof this.gpu.getPreferredCanvasFormat === "function" ?
                    this.gpu.getPreferredCanvasFormat() : "bgra8unorm");
                this.swapchainSrgbFormat = srgbSiblingOf(this.format);
                // A canvas context defaults to RENDER_ATTACHMENT only. The
                // back buffer also has to be readable and writable as a texture,
                // because StretchRect to and from it is how a D3D9 game does
                // full-screen post-processing: grab the frame into a texture,
                // process it, put it back.
                this.context.configure({
                    device: this.device, format: this.format,
                    ...(this.swapchainSrgbFormat
                        ? { viewFormats: [this.swapchainSrgbFormat] } : {}),
                    alphaMode: "opaque",
                    usage: TEXTURE_USAGE_RENDER_ATTACHMENT |
                        TEXTURE_USAGE_COPY_SRC | TEXTURE_USAGE_COPY_DST |
                        TEXTURE_USAGE_TEXTURE_BINDING,
                });
                this.fallbackTexture = this.device.createTexture({
                    label: "D3D9 fallback white texture",
                    size: { width: 1, height: 1, depthOrArrayLayers: 1 },
                    format: "rgba8unorm",
                    usage: TEXTURE_USAGE_COPY_DST | TEXTURE_USAGE_TEXTURE_BINDING,
                });
                this.fallbackView = this.fallbackTexture.createView();
                this.device.queue.writeTexture({ texture: this.fallbackTexture },
                    new Uint8Array([255, 255, 255, 255]),
                    { bytesPerRow: 4, rowsPerImage: 1 },
                    { width: 1, height: 1, depthOrArrayLayers: 1 });
                this.uniformRing = this.device.createBuffer({
                    label: "D3D9 uniform ring",
                    size: this.uniformRingCapacity,
                    usage: BUFFER_USAGE_UNIFORM | BUFFER_USAGE_COPY_DST,
                });
                this.uniformRingCursor = 0;
                this.watchForDeviceLoss();
                return this;
            })().catch(error => {
                this.failed = error;
                console.error("[d3d9-webgpu] initialization failed", error);
                throw error;
            });
            return this.readyPromise;
        }

        async restorePersistentShaderCache() {
            if (this.persistentShaderCachePromise)
                return this.persistentShaderCachePromise;
            this.persistentShaderCachePromise = (async () => {
                try {
                    let payload = null;
                    const storage = this.shaderCacheStorage;
                    if (storage && typeof storage.load === "function") {
                        payload = await storage.load(this.shaderCacheStorageKey);
                    }
                    if (typeof payload === "string") payload = JSON.parse(payload);
                    const restored = this.shaderCache.importEntries(payload);
                    if (restored) this.stats.shaderCachePersistentLoads += restored;
                } catch (error) {
                    ++this.stats.shaderCachePersistentFailures;
                    this.warnOnce("shader-cache-load",
                        "persistent shader cache could not be restored; using the " +
                        "in-memory cache for this session", { message: String(error) });
                }
            })();
            return this.persistentShaderCachePromise;
        }

        initializeShaderWorker() {
            if (this.options.useShaderWorker === false || this.shaderWorker)
                return;
            const WorkerClass = this.options.Worker || global.Worker;
            const url = this.options.shaderWorkerUrl || DEFAULT_SHADER_WORKER_URL;
            if (typeof WorkerClass !== "function" || !url) return;
            try {
                const worker = new WorkerClass(url);
                worker.onmessage = event => {
                    const message = event.data || {};
                    const pending = this.shaderWorkerRequests.get(message.id);
                    if (!pending) return;
                    this.shaderWorkerRequests.delete(message.id);
                    pending.resolve(message.result);
                };
                worker.onerror = event => {
                    const error = new Error("shader compiler worker failed: " +
                        ((event && event.message) || "unknown worker error"));
                    for (const pending of this.shaderWorkerRequests.values())
                        pending.reject(error);
                    this.shaderWorkerRequests.clear();
                    try { worker.terminate(); } catch (ignored) {}
                    if (this.shaderWorker === worker) this.shaderWorker = null;
                };
                this.shaderWorker = worker;
            } catch (error) {
                ++this.stats.shaderWorkerFallbacks;
                this.warnOnce("shader-worker-create",
                    "shader compile Worker is unavailable; compiling on the " +
                    "executor thread", { message: String(error) });
            }
        }

        compileShaderInWorker(tokens) {
            if (!this.shaderWorker) return null;
            const id = ++this.shaderWorkerSerial;
            return new Promise((resolve, reject) => {
                this.shaderWorkerRequests.set(id, { resolve, reject });
                try {
                    this.shaderWorker.postMessage({ id,
                        tokens: Array.from(tokens) });
                } catch (error) {
                    this.shaderWorkerRequests.delete(id);
                    reject(error);
                }
            });
        }

        schedulePersistentShaderCacheSave() {
            const storage = this.shaderCacheStorage;
            if (!storage || typeof storage.save !== "function") return;
            if (this.shaderCacheSaveTimer !== null)
                global.clearTimeout(this.shaderCacheSaveTimer);
            this.shaderCacheSaveTimer = global.setTimeout(() => {
                this.shaderCacheSaveTimer = null;
                this.flushPersistentShaderCache();
            }, 250);
        }

        async flushPersistentShaderCache() {
            if (!this.shaderCacheDirty) return;
            this.shaderCacheDirty = false;
            try {
                const payload = this.shaderCache.exportEntries(2 * 1024 * 1024);
                const storage = this.shaderCacheStorage;
                if (!storage || typeof storage.save !== "function") return;
                await storage.save(this.shaderCacheStorageKey, payload);
                ++this.stats.shaderCachePersistentSaves;
            } catch (error) {
                this.shaderCacheDirty = true;
                ++this.stats.shaderCachePersistentFailures;
                this.warnOnce("shader-cache-save",
                    "persistent shader cache could not be saved; translated " +
                    "WGSL remains cached in memory", { message: String(error) });
            }
        }

        submit(bytes, metadata) {
            const owned = bytes instanceof Uint8Array ? bytes.slice() : new Uint8Array(bytes || []);
            this.work = this.work.then(() => this.initialize())
                .then(() => this.restorePersistentShaderCache())
                .then(() => this.executeBatch(owned, metadata || {}))
                .catch(error => {
                    this.failed = error;
                    console.error("[d3d9-webgpu] batch failed", error, metadata || {});
                    this.discardFrame();
                });
            return this.work;
        }

        idle() { return this.work; }

        // ---- batch decode ----

        async executeBatch(bytes, metadata) {
            const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
            if (bytes.byteLength < D9WG_BATCH_HEADER_BYTES) {
                ++this.stats.malformedBatches;
                throw new Error("D9WG batch shorter than its header");
            }
            const magic = view.getUint32(0, true);
            const versionMajor = view.getUint16(4, true);
            const versionMinor = view.getUint16(6, true);
            const commandCount = view.getUint32(16, true);
            const commandBytes = view.getUint32(20, true);
            if (magic !== D9WG_MAGIC) {
                ++this.stats.malformedBatches;
                throw new Error("D9WG batch has the wrong magic");
            }
            if (versionMajor !== D9WG_VERSION_MAJOR || versionMinor > D9WG_VERSION_MINOR) {
                ++this.stats.malformedBatches;
                throw new Error(`unsupported D9WG version ${versionMajor}.${versionMinor}`);
            }
            if (commandBytes > bytes.byteLength - D9WG_BATCH_HEADER_BYTES) {
                ++this.stats.malformedBatches;
                throw new Error("D9WG batch command_bytes overruns the record");
            }
            ++this.stats.batches;

            let offset = D9WG_BATCH_HEADER_BYTES;
            const end = D9WG_BATCH_HEADER_BYTES + commandBytes;
            let decoded = 0;
            while (offset + D9WG_COMMAND_HEADER_BYTES <= end) {
                const opcode = view.getUint16(offset, true);
                const size = view.getUint32(offset + 4, true);
                if (size < D9WG_COMMAND_HEADER_BYTES || offset + size > end) {
                    ++this.stats.malformedBatches;
                    throw new Error("D9WG command size is invalid");
                }
                const payloadOffset = offset + D9WG_COMMAND_HEADER_BYTES;
                const payloadBytes = size - D9WG_COMMAND_HEADER_BYTES;
                const pending = this.dispatchCommand(opcode, bytes, view,
                    payloadOffset, payloadBytes);
                if (pending && typeof pending.then === "function") await pending;
                offset += size;
                ++decoded;
                ++this.stats.commands;
            }
            if (decoded !== commandCount) {
                ++this.stats.malformedBatches;
                throw new Error("D9WG command_count does not match the decoded stream");
            }

            const present = (view.getUint32(12, true) & D9WG_BATCH_FLAG_PRESENT) !== 0;
            if (present) this.finishFrame();
            if (this.shaderCacheDirty) this.schedulePersistentShaderCacheSave();
        }

        dispatchCommand(opcode, bytes, view, offset, length) {
            const handler = this.handlers[opcode];
            if (!handler) {
                // Per plan section 6.8: an unrecognized opcode is skipped by
                // its size (already advanced by the caller), never treated
                // as "executed" -- it simply never produces GPU state.
                ++this.stats.unsupportedCommands;
                return;
            }
            return handler.call(this, bytes, view, offset, length);
        }

        // ---- device/resource state ----

        deviceState(handle) {
            let state = this.devices.get(handle);
            if (!state) {
                state = this.createDeviceState(handle);
                this.devices.set(handle, state);
            }
            return state;
        }

        createDeviceState(handle) {
            return {
                handle,
                // Where the guest's window is on the guest desktop, resent on
                // every Present so the page can place the overlay canvas.
                // Deliberately NOT the back buffer's size: emit_present_and_flush
                // fills width/height from GetClientRect, and a windowed game's
                // client area is smaller than the back buffer it hosts.
                surface: { hwnd: 0, x: 0, y: 0, width: 0, height: 0,
                    visible: true, sessionKey: null },
                // What the guest asked for at CreateDevice/Reset, and therefore
                // the real size of the swap-chain colour attachment and of the
                // auto depth target created beside it.
                backBufferWidth: 0,
                backBufferHeight: 0,
                viewport: { x: 0, y: 0, width: 1, height: 1,
                    minZ: 0, maxZ: 1 },
                transforms: new Map([
                    [D3DTS_VIEW, IDENTITY4x4], [D3DTS_PROJECTION, IDENTITY4x4],
                    [D3DTS_WORLD, IDENTITY4x4],
                ]),
                renderStates: new Map(),
                // Auto depth-stencil surface, created on CREATE_DEVICE/RESET
                // when the guest asked for one. hasDepth drives both the
                // render pass attachment and whether pipelines declare a
                // depthStencil block.
                hasDepth: false,
                depthTexture: null,
                depthView: null,
                samplerStates: new Map(), // sampler*64+state -> value, read by samplerFor()
                textureStageStates: new Map(),
                vertexShaderHandle: 0,
                pixelShaderHandle: 0,
                // The D3D9 constant register file. Device state, not shader
                // state: it survives SetVertexShader and Reset, so it lives
                // here and is packed into a uniform buffer per draw by
                // constantBufferFor(). Kept as flat typed arrays because the
                // packing step is a straight subarray copy.
                vsConstF: new Float32Array(MAX_VS_CONST_F * 4),
                vsConstI: new Int32Array(MAX_CONST_I * 4),
                vsConstB: new Uint32Array(MAX_CONST_B),
                psConstF: new Float32Array(MAX_PS_CONST_F * 4),
                psConstI: new Int32Array(MAX_CONST_I * 4),
                psConstB: new Uint32Array(MAX_CONST_B),
                material: null,          // set by SET_MATERIAL; not yet consumed (M2/M3 lighting)
                lights: new Map(),       // light index -> D3DLIGHT9-shaped object; not yet consumed
                lightEnabled: new Map(), // light index -> bool
                streams: new Map(),      // stream index -> { bufferHandle, stride }
                indexBufferHandle: 0,
                vertexDeclarationHandle: 0, // also used for the SET_FVF synthesized layout
                fvfLayout: null,         // set by SET_FVF, cleared by SET_VERTEX_DECLARATION
                textures: new Map(),     // stage -> resource handle
                // Explicitly bound colour targets. A slot holding 0 (and slot 0
                // by default) means the swap chain's back buffer, which is the
                // only target whose view cannot be taken until Present.
                renderTargets: [0, 0, 0, 0],
                renderTargetLevels: [0, 0, 0, 0],
                // 0 = the device's auto depth-stencil; depthUnbound is
                // SetDepthStencilSurface(NULL), which is not the same thing --
                // it turns depth testing off for the draws that follow.
                depthTargetHandle: 0,
                depthUnbound: false,
                scissorRect: null,
                inScene: false,
            };
        }

        // World * View * Projection in D3D's own row-vector order, so that
        // a vertex would be transformed as v * W * V * P. multiply4x4 is a
        // plain row-major multiply, which is exactly that chaining.
        wvp(state) {
            const world = state.transforms.get(D3DTS_WORLD) || IDENTITY4x4;
            const view_ = state.transforms.get(D3DTS_VIEW) || IDENTITY4x4;
            const projection = state.transforms.get(D3DTS_PROJECTION) || IDENTITY4x4;
            return multiply4x4(multiply4x4(world, view_), projection);
        }

        // ---- opcode handlers ----

        get handlers() {
            if (this._handlers) return this._handlers;
            this._handlers = {
                [OP_HELLO]: this.onHello,
                [OP_CREATE_DEVICE]: this.onCreateDevice,
                [OP_RESET]: this.onReset,
                [OP_PRESENT]: this.onPresent,
                [OP_CLEAR]: this.onClear,
                [OP_BEGIN_SCENE]: this.onBeginScene,
                [OP_END_SCENE]: this.onEndScene,
                [OP_CREATE_BUFFER]: this.onCreateBuffer,
                [OP_UPDATE_BUFFER]: this.onUpdateBuffer,
                [OP_DESTROY_RESOURCE]: this.onDestroyResource,
                [OP_CREATE_TEXTURE_2D]: this.onCreateTexture2D,
                [OP_CREATE_TEXTURE_CUBE]: this.onCreateTextureCube,
                [OP_UPDATE_TEXTURE]: this.onUpdateTexture,
                [OP_SET_SCISSOR_RECT]: this.onSetScissorRect,
                [OP_SET_RENDER_TARGET]: this.onSetRenderTarget,
                [OP_SET_DEPTH_STENCIL_SURFACE]: this.onSetDepthStencilSurface,
                [OP_STRETCH_RECT]: this.onStretchRect,
                [OP_COLOR_FILL]: this.onColorFill,
                [OP_GUEST_LOG]: this.onGuestLog,
                [OP_CREATE_VERTEX_DECLARATION]: this.onCreateVertexDeclaration,
                [OP_CREATE_VERTEX_SHADER]: this.onCreateVertexShader,
                [OP_CREATE_PIXEL_SHADER]: this.onCreatePixelShader,
                [OP_SET_CURSOR_PROPERTIES]: this.onSetCursorProperties,
                [OP_SET_CURSOR_POSITION]: this.onSetCursorPosition,
                [OP_SHOW_CURSOR]: this.onShowCursor,
                [OP_WINDOW_STATE]: this.onWindowState,
                [OP_SET_VERTEX_SHADER]: this.onSetVertexShader,
                [OP_SET_PIXEL_SHADER]: this.onSetPixelShader,
                [OP_SET_VERTEX_SHADER_CONSTANT_F]: this.onSetVertexShaderConstantF,
                [OP_SET_VERTEX_SHADER_CONSTANT_I]: this.onSetVertexShaderConstantI,
                [OP_SET_VERTEX_SHADER_CONSTANT_B]: this.onSetVertexShaderConstantB,
                [OP_SET_PIXEL_SHADER_CONSTANT_F]: this.onSetPixelShaderConstantF,
                [OP_SET_PIXEL_SHADER_CONSTANT_I]: this.onSetPixelShaderConstantI,
                [OP_SET_PIXEL_SHADER_CONSTANT_B]: this.onSetPixelShaderConstantB,
                [OP_SET_RENDER_STATE]: this.onSetRenderState,
                [OP_SET_SAMPLER_STATE]: this.onSetSamplerState,
                [OP_SET_TEXTURE_STAGE_STATE]: this.onSetTextureStageState,
                [OP_SET_TEXTURE]: this.onSetTexture,
                [OP_SET_VIEWPORT]: this.onSetViewport,
                [OP_SET_TRANSFORM]: this.onSetTransform,
                [OP_SET_MATERIAL]: this.onSetMaterial,
                [OP_SET_LIGHT]: this.onSetLight,
                [OP_LIGHT_ENABLE]: this.onLightEnable,
                [OP_SET_STREAM_SOURCE]: this.onSetStreamSource,
                [OP_SET_INDICES]: this.onSetIndices,
                [OP_SET_VERTEX_DECLARATION]: this.onSetVertexDeclaration,
                [OP_SET_FVF]: this.onSetFVF,
                [OP_DRAW_PRIMITIVE]: this.onDrawPrimitive,
                [OP_DRAW_INDEXED_PRIMITIVE]: this.onDrawIndexedPrimitive,
                [OP_DRAW_PRIMITIVE_UP]: this.onDrawPrimitiveUP,
                [OP_DRAW_INDEXED_PRIMITIVE_UP]: this.onDrawIndexedPrimitiveUP,
            };
            return this._handlers;
        }

        onHello(bytes, view, offset) {
            void view.getUint32(offset, true); // guest_pointer_bits
            const featureBits = view.getUint32(offset + 4, true);
            // Per-process session isolation (an M1 leftover). Each guest process
            // picks a 64-bit session id at load time; its numeric device and
            // resource handles are only unique *within* that process, so two XP
            // processes that both load d3d9.dll will hand out colliding handles.
            // Without this, the second process's CREATE_BUFFER silently
            // overwrites the first's entry and the first process then draws with
            // whatever geometry the second uploaded.
            //
            // A new session drops everything the previous one owned rather than
            // namespacing handles: a process that has gone away cannot be
            // presenting, and keeping its resources alive would leak them for
            // the lifetime of the page.
            const sessionLow = view.getUint32(offset + 8, true);
            const sessionHigh = view.getUint32(offset + 12, true);
            const sessionKey = sessionHigh * 0x100000000 + sessionLow;
            if (this.sessionKey !== null && this.sessionKey !== sessionKey) {
                ++this.stats.sessionChanges;
                console.info("[d3d9-webgpu] a new guest process (session 0x" +
                    sessionHigh.toString(16) + sessionLow.toString(16).padStart(8, "0") +
                    ") replaced session 0x" +
                    this.sessionKey.toString(16) + "; the previous process's " +
                    "devices and resources are released, because its numeric " +
                    "handles are about to be reused for different objects");
                this.releaseSession();
            }
            this.sessionKey = sessionKey;
            this.stats.guestFeatureBits = featureBits;
            // The host executor reloads with the page; the guest DLL lives
            // inside the disk image. "New host, stale guest" is the normal
            // state after a milestone lands, and it looks exactly like "this
            // scene uses no shaders" -- both show shadersTranslated: 0. This
            // makes getStats() answer the question directly.
            this.stats.guestShaderModel2 =
                (featureBits & D9WG_FEATURE_SHADER_MODEL_2) !== 0;
        }

        onCreateDevice(bytes, view, offset) {
            const handle = view.getUint32(offset, true);
            const hwnd = view.getUint32(offset + 4, true);
            const x = view.getInt32(offset + 8, true);
            const y = view.getInt32(offset + 12, true);
            const width = view.getUint32(offset + 16, true);
            const height = view.getUint32(offset + 20, true);
            const enableAutoDepth = view.getUint32(offset + 36, true);
            // A frame left un-presented by a previous device -- typically a
            // process that exited mid-frame -- must not bleed into this one.
            // Its recorded ops reference that device's depth target and
            // back-buffer size, which WebGPU rejects as soon as the sizes
            // differ ("depth stencil attachment size does not match ...").
            this.discardFrame();
            const state = this.deviceState(handle);
            state.viewport = { x: 0, y: 0, width, height, minZ: 0, maxZ: 1 };
            state.surface = { hwnd, x, y, width, height, visible: true, sessionKey: null };
            state.backBufferWidth = width;
            state.backBufferHeight = height;
            this.resizeCanvasIfNeeded(width, height);
            this.ensureDepthTarget(state, width, height, enableAutoDepth !== 0);
            this.notifySurface(state, "create");
        }

        onReset(bytes, view, offset) {
            const oldHandle = view.getUint32(offset, true);
            const newHandle = view.getUint32(offset + 4, true);
            const hwnd = view.getUint32(offset + 8, true);
            const x = view.getInt32(offset + 12, true);
            const y = view.getInt32(offset + 16, true);
            const width = view.getUint32(offset + 20, true);
            const height = view.getUint32(offset + 24, true);
            const enableAutoDepth = view.getUint32(offset + 40, true);
            const oldState = this.devices.get(oldHandle);
            if (oldState) this.retireGPUObject(oldState.depthTexture);
            this.devices.delete(oldHandle);
            const state = this.deviceState(newHandle);
            state.viewport = { x: 0, y: 0, width, height, minZ: 0, maxZ: 1 };
            state.surface = { hwnd, x, y, width, height, visible: true, sessionKey: null };
            state.backBufferWidth = width;
            state.backBufferHeight = height;
            this.resizeCanvasIfNeeded(width, height);
            this.ensureDepthTarget(state, width, height, enableAutoDepth !== 0);
            this.notifySurface(state, "reset");
        }

        resizeCanvasIfNeeded(width, height) {
            if (this.canvas.width !== width) this.canvas.width = width;
            if (this.canvas.height !== height) this.canvas.height = height;
        }

        // The swap-chain colour attachment is context.getCurrentTexture(), which
        // is the canvas, which resizeCanvasIfNeeded sized to the back buffer.
        // Reading state.surface here instead was a real defect: Present rewrites
        // state.surface with the window's client rect every frame, so a windowed
        // game reported a back buffer 13 rows shorter than the auto depth target
        // created with it (Kart Rider: 800x587 against 800x600). The pass then
        // looked mismatched and dropped depth, which turns off depth testing for
        // the entire game rather than for anything the app actually did.
        backBufferWidthOf(state) {
            return state.backBufferWidth || this.canvas.width ||
                state.surface.width || 1;
        }

        backBufferHeightOf(state) {
            return state.backBufferHeight || this.canvas.height ||
                state.surface.height || 1;
        }

        // Creates (or drops) the device's auto depth-stencil target. D3D9
        // reports many depth formats but the guest can never read any of
        // them back in M1, so one depth24plus-stencil8 target satisfies all
        // of them; only whether a depth buffer exists at all is observable.
        ensureDepthTarget(state, width, height, enabled) {
            if (state.depthTexture) {
                // Never destroy it inline: the frame currently being recorded
                // may already have pinned this texture's view (see
                // recordDraw's frame.depthView) and will not submit until
                // Present, so an immediate destroy produces
                // "Destroyed texture ... used in a submit" -- a real crash
                // observed when a device was re-created or Reset mid-frame.
                this.retireGPUObject(state.depthTexture);
                state.depthTexture = null;
                state.depthView = null;
            }
            state.hasDepth = !!enabled;
            if (!enabled || !width || !height) return;
            state.depthTexture = this.device.createTexture({
                label: "D3D9 auto depth-stencil",
                size: { width, height, depthOrArrayLayers: 1 },
                format: DEPTH_FORMAT,
                usage: TEXTURE_USAGE_RENDER_ATTACHMENT,
            });
            state.depthView = state.depthTexture.createView();
            state.depthWidth = width;
            state.depthHeight = height;
        }

        // One line per distinct condition, so a per-frame problem cannot flood
        // the console while still being reported the first time it happens.
        warnOnce(key, message, details) {
            this.warned = this.warned || new Set();
            if (this.warned.has(key)) return;
            this.warned.add(key);
            console.warn("[d3d9-webgpu] " + message, details || "");
        }

        // Releases a GPU object that the in-flight frame may still reference.
        // If a frame is being recorded, the object rides along with that
        // frame's transient list and is destroyed only once the frame's
        // submit has completed; otherwise it waits on the queue directly.
        retireGPUObject(object) {
            if (!object || typeof object.destroy !== "function") return;
            if (this.frame) {
                this.frame.transientBuffers.push(object);
                return;
            }
            const destroy = () => object.destroy();
            if (this.device.queue
                    && typeof this.device.queue.onSubmittedWorkDone === "function")
                this.device.queue.onSubmittedWorkDone().then(destroy, destroy);
            else
                destroy();
        }

        // Drops everything tied to the previous guest process. Called from
        // onHello when the session id changes.
        releaseSession() {
            this.discardFrame();
            for (const state of this.devices.values())
                this.retireGPUObject(state.depthTexture);
            for (const resource of this.resources.values()) {
                this.retireGPUObject(resource.gpuBuffer);
                this.retireGPUObject(resource.gpuTexture);
            }
            this.devices.clear();
            this.resources.clear();
            // Pipelines are keyed by state and stay useful across a process
            // change. Bind groups capture concrete texture views, so clear
            // those references when their owning resources depart.
            this.bindGroupCache.clear();
            // The cursor belonged to the departing process too.
            this.cursor.visible = false;
            this.retireGPUObject(this.cursor.texture);
            this.cursor.texture = null;
            this.cursor.view = null;
        }

        // WebGPU can take the device away (driver reset, tab backgrounded on
        // some platforms, an unrecoverable validation failure). Before this, the
        // executor logged one error and then failed every subsequent batch
        // forever, which on screen is a frozen canvas with no explanation.
        //
        // What recovery achieves and what it does not, precisely:
        //
        //   It does   bring the host back to a working GPUDevice, canvas context
        //             and cache set, so the page is alive and the next Present
        //             does not throw.
        //   It does   preserve every translation result: WGSL text is not tied
        //             to a GPUDevice (plan 8.5), so the translator does not run
        //             again -- only the GPUShaderModules are rebuilt, lazily.
        //   It does not restore the guest's resources. Their contents live in
        //             the guest's CPU shadows, and the only way to get them back
        //             is for the guest to replay them -- which it does on Reset
        //             (recreate_device_resources in d3d9_proxy.c) but has no
        //             reason to do, because nothing tells it the device was
        //             lost. Until the host->guest notification of plan 6.7
        //             exists, draws after a loss reference handles the new
        //             device never saw and are counted in droppedDraws.
        //
        // So this converts "the page is dead" into "the page is alive and the
        // stats say exactly what happened", which is the honest extent of it.
        watchForDeviceLoss() {
            if (!this.device || !this.device.lost ||
                    typeof this.device.lost.then !== "function")
                return;
            const lostDevice = this.device;
            this.device.lost.then(info => {
                if (this.device !== lostDevice) return;
                ++this.stats.deviceLosses;
                console.error("[d3d9-webgpu] WebGPU device lost (" +
                    ((info && info.reason) || "unknown") + "): " +
                    ((info && info.message) || "") +
                    " -- rebuilding; the guest will see D3DERR_DEVICELOST and " +
                    "re-create its resources through its own Reset path");
                this.recoverDevice();
            }, () => {});
        }

        recoverDevice() {
            // Everything below was created by the lost device and cannot be
            // destroyed or reused; dropping the references is all that is
            // possible and all that is needed.
            this.discardFrame();
            this.devices.clear();
            this.resources.clear();
            this.pipelineCache.clear();
            this.bindGroupCache.clear();
            this.moduleCache.clear();
            this.samplerCache.clear();
            this.cursor = { ...this.cursor, texture: null, view: null,
                pipeline: null, sampler: null, uniform: null,
                bindGroupLayout: null, visible: false };
            this.fallbackTexture = null;
            this.fallbackView = null;
            this.uniformRing = null;
            this.uniformRingCursor = 0;
            this.objectIds = new WeakMap();
            this.nextObjectId = 1;
            this.device = null;
            this.context = null;
            this.readyPromise = null;
            this.failed = null;
            this.sessionKey = null;
            if (typeof this.options.onDeviceLost === "function")
                this.options.onDeviceLost();
            // initialize() is idempotent through readyPromise, and submit()
            // already awaits it before every batch, so the next batch after a
            // loss brings the new device up.
            this.work = this.initialize().then(() => {
                ++this.stats.deviceRecoveries;
            }, error => {
                console.error("[d3d9-webgpu] device recovery failed", error);
            });
        }

        notifySurface(state, reason) {
            if (typeof this.options.onSurface === "function")
                this.options.onSurface(state.surface, reason);
        }

        onPresent(bytes, view, offset) {
            // The actual GPU submit happens in finishFrame(), called once
            // per executeBatch() when the outer D9WGBatchHeader carries
            // D9WG_BATCH_FLAG_PRESENT -- see the end of executeBatch(). The
            // guest recomputes the window's current screen position on
            // every Present (d3d9_proxy.c has no window-move subclassing in
            // M1), so this is the live source of truth for canvas placement
            // rather than a separate UPDATE_SURFACE event.
            const handle = view.getUint32(offset, true);
            const state = this.deviceState(handle);
            const hwnd = view.getUint32(offset + 4, true);
            const x = view.getInt32(offset + 8, true);
            const y = view.getInt32(offset + 12, true);
            let width = view.getUint32(offset + 16, true);
            let height = view.getUint32(offset + 20, true);
            // The guest recomputes the client rect on every Present, and
            // GetClientRect is known to return an empty rect in fullscreen
            // (recorded as an open issue at M1). Letting a 0x0 report through
            // makes the host repeatedly resize/reposition the overlay canvas
            // between the real size and nothing, which reads on screen as
            // flicker. The last non-empty size is the better answer: a window
            // that genuinely has no client area has nothing to show anyway.
            if (!width || !height) {
                ++this.stats.emptySurfaceReports;
                width = state.surface.width || width;
                height = state.surface.height || height;
            }
            const changed = state.surface.hwnd !== hwnd || state.surface.x !== x ||
                state.surface.y !== y || state.surface.width !== width ||
                state.surface.height !== height;
            state.surface = { ...state.surface, hwnd, x, y, width, height, visible: true };
            if (changed) {
                ++this.stats.surfaceChanges;
                this.notifySurface(state, "present");
            }
            this.presentingDevice = state;
        }

        onBeginScene(bytes, view, offset) {
            const handle = view.getUint32(offset, true);
            this.deviceState(handle).inScene = true;
        }

        onEndScene(bytes, view, offset) {
            const handle = view.getUint32(offset, true);
            this.deviceState(handle).inScene = false;
        }

        // A "frame" here is pure JS bookkeeping -- a list of pending clear/
        // draw operations -- with no WebGPU objects created yet. This is
        // deliberate: a canvas's context.getCurrentTexture() is only valid
        // for the task that acquired it; once control returns to the
        // browser's event loop, that texture is liable to be presented and
        // invalidated out from under you ("Destroyed texture ... used in a
        // submit"). A single D3D9 frame's Clear/Draw/Present calls do not
        // reliably arrive in one task: d3d9_proxy.c flushes a partial batch
        // over PCI whenever the DMA ring fills up (reserve_command_locked's
        // intermediate submit_batch_locked(FALSE)), and each such PCI record
        // is delivered to this executor as a separate worker postMessage --
        // a separate macrotask. Acquiring the swapchain texture eagerly on
        // the first Clear of a frame and holding it until a much-later
        // Present-carrying submit is exactly the pattern that goes stale.
        // Recording lightweight ops now and only turning them into real
        // WebGPU calls inside finishFrame() -- acquired, recorded, and
        // submitted in one synchronous stretch -- avoids that regardless of
        // how many separate PCI submits contributed to the frame.
        ensureFrame() {
            if (this.frame) return this.frame;
            // `serial` identifies this frame for the write-after-record check in
            // applyBufferUpdate(); it must be unique per frame, never reused.
            this.uniformRingCursor = 0;
            this.frame = { ops: [], transientBuffers: [], uniformSlots: new Map(),
                serial: ++this.frameSerial,
                statStart: {
                    pipelineCreations: this.stats.pipelineCreations,
                    bindGroupCreations: this.stats.bindGroupCreations,
                    queueSubmits: this.stats.queueSubmits,
                    renderPasses: this.stats.renderPasses,
                } };
            return this.frame;
        }

        // If a command throws partway through a batch (see submit()'s catch
        // handler), this.frame may be left holding recorded-but-unreplayed
        // ops. Since no WebGPU render pass/encoder exists yet at this point
        // (those are only created in finishFrame(), at Present time), there
        // is nothing to end -- just drop the ops and free any transient
        // buffers already created for them so the next frame starts clean.
        discardFrame() {
            const frame = this.frame;
            if (!frame) return;
            this.frame = null;
            if (frame.transientBuffers && frame.transientBuffers.length) {
                for (const buffer of frame.transientBuffers) buffer.destroy();
            }
        }

        onClear(bytes, view, offset, length) {
            const deviceHandle = view.getUint32(offset, true);
            const flags = view.getUint32(offset + 4, true);
            const color = view.getUint32(offset + 8, true);
            const depth = view.getFloat32(offset + 12, true);
            const stencil = view.getUint32(offset + 16, true);
            const state = this.deviceState(deviceHandle);
            const clearsColor = (flags & D3DCLEAR_TARGET) !== 0;
            // A depth/stencil clear is only meaningful if the device
            // actually has an auto depth-stencil surface.
            const targets = this.renderTargetsFor(state);
            if (!targets) return;
            const clearsDepth = (flags & D3DCLEAR_ZBUFFER) !== 0 && targets.hasDepth;
            const clearsStencil = (flags & D3DCLEAR_STENCIL) !== 0 && targets.hasDepth;
            if (!clearsColor && !clearsDepth && !clearsStencil) return;
            const a = ((color >>> 24) & 0xff) / 255;
            const r = ((color >>> 16) & 0xff) / 255;
            const g = ((color >>> 8) & 0xff) / 255;
            const b = (color & 0xff) / 255;
            const frame = this.ensureFrame();
            const rectCount = view.getUint32(offset + 20, true);
            if (rectCount) {
                if (24 + rectCount * 16 > length) {
                    ++this.stats.malformedBatches;
                    throw new Error("D9WG Clear rectangles overrun the command");
                }
                const rects = [];
                for (let index = 0; index < rectCount; ++index) {
                    const base = offset + 24 + index * 16;
                    const left = Math.max(0, Math.min(targets.width,
                        view.getInt32(base, true)));
                    const top = Math.max(0, Math.min(targets.height,
                        view.getInt32(base + 4, true)));
                    const right = Math.max(left, Math.min(targets.width,
                        view.getInt32(base + 8, true)));
                    const bottom = Math.max(top, Math.min(targets.height,
                        view.getInt32(base + 12, true)));
                    if (right > left && bottom > top)
                        rects.push({ left, top, right, bottom });
                }
                if (rects.length) {
                    frame.ops.push({ kind: "rect-clear", targets, clearsColor,
                        clearsDepth, clearsStencil, color: { r, g, b, a },
                        depth, stencil, rects });
                    ++this.stats.partialClears;
                }
                return;
            }
            frame.ops.push({
                kind: "clear", targets,
                clearsColor: clearsColor || !!this.debug.forceClearColor,
                clearsDepth, clearsStencil,
                color: this.debug.forceClearColor || { r, g, b, a },
                depth, stencil,
            });
        }

        // Replays every recorded clear/draw op against a freshly-acquired
        // swapchain texture, all synchronously, right before submit -- see
        // the comment on ensureFrame() for why acquisition cannot happen any
        // earlier than this. A "clear" op starts a new render pass (WebGPU
        // has no mid-pass re-clear); a "draw" op opens a loadOp:"load" pass
        // first if none is open yet (a draw with no preceding Clear in this
        // frame means "keep whatever the swapchain texture already has").
        finishFrame() {
            const frame = this.frame;
            if (!frame) return;
            this.frame = null;
            if (!frame.ops.length) ++this.stats.framesWithNoOps;
            else if (!frame.ops.some(op => op.kind === "clear" && op.clearsColor))
                ++this.stats.framesWithoutColorClear;
            if (frame.ops.length) {
                const encoder = this.device.createCommandEncoder();
                const swapTexture = this.context.getCurrentTexture();
                const swapView = swapTexture.createView();
                const swapSrgbView = this.swapchainSrgbFormat
                    ? swapTexture.createView({ format: this.swapchainSrgbFormat })
                    : swapView;
                // Every op carries the target set it was recorded against (see
                // renderTargetsFor). A pass covers the longest run of ops that
                // share a target set; a Clear always starts a new one, because
                // WebGPU expresses a clear only as a pass's loadOp.
                let pass = null;
                let openKey = null;
                const beginPass = (targets, clearColor, clearDepthStencil) => {
                    const descriptor = {
                        colorAttachments: targets.colors.map(color => ({
                            view: color.swapchain
                                ? (color.format === this.swapchainSrgbFormat
                                    ? swapSrgbView : swapView)
                                : color.view,
                            loadOp: clearColor ? "clear" : "load",
                            storeOp: "store",
                            ...(clearColor ? { clearValue: clearColor } : {}),
                        })),
                    };
                    if (targets.depthView) {
                        const clearsDepth = clearDepthStencil &&
                            clearDepthStencil.depth !== undefined;
                        const clearsStencil = clearDepthStencil &&
                            clearDepthStencil.stencil !== undefined;
                        descriptor.depthStencilAttachment = {
                            view: targets.depthView,
                            depthLoadOp: clearsDepth ? "clear" : "load",
                            depthStoreOp: "store",
                            stencilLoadOp: clearsStencil ? "clear" : "load",
                            stencilStoreOp: "store",
                            ...(clearsDepth ? {
                                depthClearValue: clearDepthStencil.depth } : {}),
                            ...(clearsStencil ? {
                                stencilClearValue: clearDepthStencil.stencil } : {}),
                        };
                    }
                    ++this.stats.renderPasses;
                    return encoder.beginRenderPass(descriptor);
                };
                // The back buffer and the auto depth-stencil must agree on size
                // or WebGPU rejects the whole command buffer -- and with it
                // every later frame, not just this one.
                // A blit op carries its own attachment views rather than a
                // target set, so it is not part of this check.
                const backBufferOps = frame.ops.filter(op => op.targets &&
                    op.targets.colors.some(color => color.swapchain));
                const mismatched = backBufferOps.find(op => op.targets.depthView &&
                    (op.targets.width !== swapTexture.width ||
                     op.targets.height !== swapTexture.height));
                if (mismatched) {
                    this.warnOnce("depth-size-mismatch",
                        "dropping a frame whose depth target " +
                        mismatched.targets.width + "x" + mismatched.targets.height +
                        " does not match the back buffer " +
                        swapTexture.width + "x" + swapTexture.height);
                    frame.ops.length = 0;
                }
                for (const op of frame.ops) {
                    if (op.kind === "copy") {
                        if (pass) { pass.end(); pass = null; openKey = null; }
                        encoder.copyTextureToTexture(op.source, op.destination,
                            op.size);
                        continue;
                    }
                    if (op.kind === "blit") {
                        // A blit is its own pass against its own attachment.
                        if (pass) { pass.end(); pass = null; openKey = null; }
                        const transient = this.replayBlit(encoder, op, swapView);
                        if (transient) frame.transientBuffers.push(transient);
                        continue;
                    }
                    if (op.kind === "rect-clear") {
                        if (pass) { pass.end(); pass = null; openKey = null; }
                        const transient = this.replayRectClear(encoder, op,
                            swapView, swapSrgbView);
                        if (transient) frame.transientBuffers.push(transient);
                        continue;
                    }
                    if (op.kind === "color-fill") {
                        if (pass) { pass.end(); pass = null; openKey = null; }
                        const transient = this.replayColorFill(encoder, op);
                        if (transient) frame.transientBuffers.push(transient);
                        continue;
                    }
                    if (op.kind === "clear") {
                        if (pass) pass.end();
                        // A Clear that only touches depth still has to keep the
                        // colour already drawn this frame, and vice versa --
                        // hence the two independent load ops.
                        pass = beginPass(op.targets,
                            op.clearsColor ? op.color : null,
                            (op.clearsDepth || op.clearsStencil) ? {
                                ...(op.clearsDepth ? { depth: op.depth } : {}),
                                ...(op.clearsStencil ? { stencil: op.stencil } : {}),
                            } : null);
                        openKey = op.targets.key;
                        continue;
                    }
                    if (!pass || openKey !== op.targets.key) {
                        if (pass) pass.end();
                        pass = beginPass(op.targets, null, null);
                        openKey = op.targets.key;
                    }
                    pass.setPipeline(op.pipeline);
                    pass.setBindGroup(0, op.bindGroup, op.dynamicOffsets || []);
                    pass.setBlendConstant(op.blendConstant);
                    pass.setStencilReference(op.stencilReference);
                    pass.setViewport(op.viewport.x, op.viewport.y,
                            op.viewport.width, op.viewport.height,
                            op.viewport.minZ, op.viewport.maxZ);
                    // A D3D9 viewport clips; a WebGPU one only maps NDC to
                    // pixels. Without a matching scissor, geometry an app
                    // expected the viewport to cut off is drawn across the whole
                    // target instead -- which is why the clip rect is the
                    // viewport intersected with D3DRS_SCISSORTESTENABLE's rect
                    // (D3D9 applies both), not just the latter. Scissor is
                    // dynamic pass state, so it is re-issued for every draw:
                    // omitting it would leak the previous draw's rect.
                    const clip = intersectRects(op.viewport, op.scissor,
                            op.targets.width, op.targets.height);
                    pass.setScissorRect(clip.x, clip.y, clip.width, clip.height);
                    for (let slot = 0; slot < op.vertexBuffers.length; ++slot) {
                        const binding = op.vertexBuffers[slot];
                        pass.setVertexBuffer(slot, binding.buffer, binding.offset);
                    }
                    if (op.indexInfo) {
                        pass.setIndexBuffer(op.indexInfo.buffer, op.indexInfo.format,
                                op.indexInfo.offset);
                        pass.drawIndexed(op.indexInfo.count, 1,
                                op.indexInfo.firstIndex, op.indexInfo.baseVertex);
                    } else {
                        // StartVertex is already folded into each stream's
                        // setVertexBuffer offset (see boundStreams), so
                        // firstVertex stays 0 here.
                        if ((op.instanceCount || 1) === 1)
                            pass.draw(op.vertexCount || 0);
                        else
                            pass.draw(op.vertexCount || 0, op.instanceCount);
                    }
                }
                if (pass) pass.end();
                this.drawCursor(encoder, swapView, swapTexture.width,
                        swapTexture.height);
                this.device.queue.submit([encoder.finish()]);
                ++this.stats.queueSubmits;
            }
            ++this.stats.presents;
            const start = frame.statStart || {};
            this.lastFrameStats = {
                pipelineCreations: this.stats.pipelineCreations -
                    (start.pipelineCreations || 0),
                bindGroupCreations: this.stats.bindGroupCreations -
                    (start.bindGroupCreations || 0),
                queueSubmits: this.stats.queueSubmits -
                    (start.queueSubmits || 0),
                renderPasses: this.stats.renderPasses -
                    (start.renderPasses || 0),
            };
            const transientBuffers = frame.transientBuffers;
            if (transientBuffers && transientBuffers.length) {
                const destroy = () => { for (const b of transientBuffers) b.destroy(); };
                if (this.device.queue && typeof this.device.queue.onSubmittedWorkDone === "function")
                    this.device.queue.onSubmittedWorkDone().then(destroy, destroy);
                else
                    destroy();
            }
            if (this.presentingDevice && typeof this.options.onPresent === "function")
                this.options.onPresent(this.presentingDevice.surface, this.getStats());
            this.presentingDevice = null;
        }

        // v86gl.d3d9Executor.debug.dumpSmallTextures() -> data: URLs that can
        // be opened straight from the console. Only uncompressed top-level
        // images up to 64x64 are retained (see onUpdateTexture).
        dumpSmallTextures(options) {
            const settings = options || {};
            const out = [];
            for (const [handle, resource] of this.resources) {
                if (!resource.preview) continue;
                if (settings.handle !== undefined && handle !== settings.handle)
                    continue;
                const { width, height, rgba } = resource.preview;
                let url = null;
                let error = null;
                try {
                    // A plain <canvas>, not OffscreenCanvas: only the former
                    // has toDataURL. OffscreenCanvas offers convertToBlob,
                    // which is async and cannot be returned from here -- that
                    // mismatch is why the first version of this helper
                    // reported url: null for every texture.
                    if (typeof document === "undefined")
                        throw new Error("no document to render into");
                    const canvas = document.createElement("canvas");
                    canvas.width = width;
                    canvas.height = height;
                    const context = canvas.getContext("2d");
                    const image = context.createImageData(width, height);
                    image.data.set(rgba.subarray(0, width * height * 4));
                    context.putImageData(image, 0, 0);
                    url = canvas.toDataURL();
                } catch (failure) {
                    error = failure && failure.message ? failure.message
                        : String(failure);
                }
                const entry = { handle, format: resource.format,
                    formatName: TEXTURE_FORMAT_NAMES[resource.format] ||
                        ("0x" + (resource.format >>> 0).toString(16)),
                    size: width + "x" + height,
                    declaredLevels: resource.levelCount,
                    uploadedLevels: resource.uploadedLevels
                        ? [...resource.uploadedLevels].sort((a, b) => a - b) : null,
                    url, error };
                out.push(entry);
                // Rendering them inline is the point of the helper: a data URL
                // in a console row is unreadable, an actual picture answers
                // "is the texture data wrong?" at a glance.
                if (settings.log !== false && url) {
                    const scale = Math.max(1, Math.round(64 / Math.max(width, height)));
                    console.log("%c ", "font-size:0;padding:" +
                        (height * scale / 2) + "px " + (width * scale / 2) +
                        "px;background:url(" + url +
                        ") no-repeat center/contain;image-rendering:pixelated",
                        handle, entry.formatName, entry.size);
                }
            }
            return out;
        }

        // Every distinct pipeline state actually in use, with the raw D3D9
        // render-state values behind it. Reading the real mix beats guessing
        // which blend/depth/cull combination a scene is built from.
        dumpPipelineStates() {
            const out = [];
            for (const key of this.pipelineCache.keys()) {
                const parts = key.split("|");
                let state = null;
                try { state = JSON.parse(parts[6]); } catch (error) { /* key shape changed */ }
                out.push({ vertex: parts[0], fragment: parts[1],
                    topology: parts[4], state });
            }
            return out;
        }

        getStats() {
            // The live surface rect is included because it is what positions
            // the overlay canvas, and a wrong rect is invisible in the picture
            // itself: the frame still looks correct, it is just drawn where
            // the guest does not think the window is -- so clicks land on
            // whatever the guest really has at that pixel.
            const device = this.presentingDevice ||
                this.devices.values().next().value || null;
            // The fog parameters as this executor decoded them. "Everything
            // is washed out towards one colour" and "fog is not applied at
            // all" look nothing alike in these numbers, and guessing between
            // them from a screenshot has already cost a round.
            let fog = null;
            if (device) {
                const rs = device.renderStates;
                const asFloat = id => {
                    const raw = rs.get(id);
                    if (raw === undefined) return null;
                    FLOAT_BITS_U32[0] = raw >>> 0;
                    return FLOAT_BITS_F32[0];
                };
                const color = rs.get(D3DRS_FOGCOLOR) || 0;
                fog = {
                    enabled: (rs.get(D3DRS_FOGENABLE) || 0) !== 0,
                    tableMode: rs.get(D3DRS_FOGTABLEMODE) || 0,
                    vertexMode: rs.get(D3DRS_FOGVERTEXMODE) || 0,
                    color: "#" + (color & 0xffffff).toString(16).padStart(6, "0"),
                    // null means the game never set it and the D3D9 default
                    // (start 0, end 1) applies -- which fogs everything past
                    // one unit completely, so a null here with LINEAR mode is
                    // itself the explanation for a uniformly washed-out frame.
                    start: asFloat(D3DRS_FOGSTART),
                    end: asFloat(D3DRS_FOGEND),
                    density: asFloat(D3DRS_FOGDENSITY),
                };
            }
            const shaderCache = this.shaderCache.snapshot();
            return { ...this.stats, devicesLive: this.devices.size,
                resourcesLive: this.resources.size,
                pipelinesCached: this.pipelineCache.size,
                bindGroupsCached: this.bindGroupCache.size,
                samplersCached: this.samplerCache.size,
                mrtAttachmentDraws: this.mrtAttachmentDraws.slice(1),
                lastFrame: { ...this.lastFrameStats },
                shaderCache,
                shaderCachePersistentBackend: this.shaderCacheStorageBackend,
                shaderCacheHits: shaderCache.hits,
                shaderCacheMisses: shaderCache.misses,
                shadersCached: shaderCache.cached,
                shaderWGSLBytesCached: shaderCache.totalWGSLBytes,
                shaderCompileLatencyMs: { ...shaderCache.compileLatencyMs },
                // M4 deliberately answers occlusion queries conservatively in
                // the guest instead of allocating host GPU query slots. Expose
                // that policy explicitly so zero slot usage cannot be mistaken
                // for a broken or unobserved query path.
                occlusionQueries: { mode: "guest-conservative",
                    slotsUsed: 0, slotsCapacity: 0,
                    slotExhaustionFallbacks: 0 },
                surface: device ? { ...device.surface } : null,
                window: this.windowState ? { ...this.windowState } : null,
                fog };
        }

        // ---- resources ----

        onCreateBuffer(bytes, view, offset) {
            const handle = view.getUint32(offset + 4, true);
            const kind = view.getUint32(offset + 8, true);
            const byteCount = view.getUint32(offset + 12, true);
            const format = view.getUint32(offset + 20, true); // index format, for INDEX kind
            const usage = kind === RESOURCE_BUFFER_INDEX
                ? BUFFER_USAGE_INDEX | BUFFER_USAGE_COPY_DST
                : BUFFER_USAGE_VERTEX | BUFFER_USAGE_COPY_DST;
            const alignedSize = Math.max(4, alignUp(byteCount, 4));
            const gpuBuffer = this.device.createBuffer({
                size: alignedSize,
                usage,
            });
            this.resources.set(handle, {
                kind, gpuBuffer, byteCount,
                // CPU mirror of the buffer's full (aligned) content. D3D9's
                // Lock/Unlock byte ranges can start and end anywhere, but
                // WebGPU's writeBuffer requires both the destination offset
                // and the size to be a multiple of 4 -- a 16-bit index
                // buffer partially updated starting at an odd index is a
                // routine, common example that is neither. Applying the
                // update to this plain byte array first (no alignment
                // concerns) and re-uploading only the small 4-byte-aligned
                // super-range that covers it keeps every write legal without
                // ever guessing at or corrupting the untouched bytes on
                // either edge (see applyBufferUpdate()).
                shadow: new Uint8Array(alignedSize),
                indexFormat: format === D3DFMT_INDEX32 ? "uint32" : "uint16",
            });
        }

        onUpdateBuffer(bytes, view, offset, length) {
            const handle = view.getUint32(offset, true);
            const destinationOffset = view.getUint32(offset + 4, true);
            const byteCount = view.getUint32(offset + 8, true);
            const dataOffset = view.getUint32(offset + 12, true);
            const lockFlags = view.getUint32(offset + 16, true);
            const resource = this.resources.get(handle);
            if (!resource || !byteCount) return;
            this.applyBufferUpdate(resource, destinationOffset, bytes, dataOffset,
                byteCount, lockFlags);
        }

        applyBufferUpdate(resource, destinationOffset, bytes, sourceOffset,
                byteCount, lockFlags) {
            lockFlags = lockFlags || 0;
            const shadow = resource.shadow;
            if (destinationOffset >= shadow.length) return;
            if (destinationOffset + byteCount > shadow.length)
                byteCount = shadow.length - destinationOffset;
            if (!byteCount) return;
            const source = new Uint8Array(bytes.buffer, bytes.byteOffset + sourceOffset, byteCount);
            shadow.set(source, destinationOffset);

            // Renaming (below) is what keeps deferred draws honest.
            //
            // Draws are not encoded when they arrive: they are recorded and
            // replayed at Present, because a swapchain texture is only valid
            // inside the task that acquired it (see ensureFrame). Buffer
            // writes, by contrast, take effect in queue order -- so every
            // writeBuffer issued during a frame lands *before* that frame's
            // single submit, and therefore before every draw in it.
            //
            // For the single most common dynamic-geometry idiom that is
            // catastrophic:
            //
            //     Lock(DISCARD); write batch A; DrawPrimitive
            //     Lock(DISCARD); write batch B; DrawPrimitive
            //     Present
            //
            // Both draws end up reading batch B. The first renders real
            // indices against the wrong vertices, which on screen is stray
            // geometry stretching across the frame, different every frame,
            // while static/managed resources look perfect.
            //
            // A real D3D9 driver answers this by *renaming*: DISCARD means
            // "I no longer care about the old contents", and the driver hands
            // back fresh storage while in-flight commands keep the old
            // allocation. Do the same here -- but only when this buffer has
            // actually been read by a draw already recorded in this frame, so
            // the ordinary "upload once, draw many" path allocates nothing.
            //
            // The lock flags decide *which* answer is needed, and getting this
            // distinction right is what keeps the cost sane. War3 renamed ~277
            // times per frame when every mid-frame write was treated the same:
            //
            //   NOOVERWRITE  the application has promised it is only writing
            //                bytes no issued draw reads -- that is precisely
            //                the guarantee this hazard needs, and it is how a
            //                game appends batch after batch into one buffer.
            //                Write in place; renaming here is pure waste.
            //   DISCARD      the old contents are dead, so the replacement
            //                only needs the bytes being written now. The rest
            //                is garbage the application has promised not to
            //                read, so there is nothing to copy forward.
            //   neither      a plain lock keeps the old contents readable, so
            //                the replacement has to carry the whole shadow.
            //                Rare, and the only case that costs a full upload.
            const D3DLOCK_NOOVERWRITE = 0x1000;
            const D3DLOCK_DISCARD = 0x2000;
            if (this.frame && resource.frameReferenced === this.frame.serial &&
                    !(lockFlags & D3DLOCK_NOOVERWRITE)) {
                const replacement = this.device.createBuffer({
                    label: "D3D9 renamed buffer",
                    size: resource.gpuBuffer.size,
                    usage: resource.kind === RESOURCE_BUFFER_INDEX
                        ? BUFFER_USAGE_INDEX | BUFFER_USAGE_COPY_DST
                        : BUFFER_USAGE_VERTEX | BUFFER_USAGE_COPY_DST,
                });
                if (lockFlags & D3DLOCK_DISCARD) {
                    const start = destinationOffset & ~3;
                    const end = Math.min(shadow.length,
                        alignUp(destinationOffset + byteCount, 4));
                    if (end > start)
                        this.device.queue.writeBuffer(replacement, start,
                            shadow.buffer, shadow.byteOffset + start, end - start);
                } else {
                    this.device.queue.writeBuffer(replacement, 0, shadow.buffer,
                        shadow.byteOffset, shadow.length);
                    ++this.stats.bufferFullCopyRenames;
                }
                this.retireGPUObject(resource.gpuBuffer);
                resource.gpuBuffer = replacement;
                resource.frameReferenced = 0;
                ++this.stats.bufferRenames;
                return;
            }
            if (this.frame && resource.frameReferenced === this.frame.serial)
                ++this.stats.bufferNoOverwriteWrites;

            const alignedStart = destinationOffset & ~3;
            const alignedEnd = Math.min(shadow.length,
                alignUp(destinationOffset + byteCount, 4));
            if (alignedEnd <= alignedStart) return;
            this.device.queue.writeBuffer(resource.gpuBuffer, alignedStart,
                shadow.buffer, shadow.byteOffset + alignedStart, alignedEnd - alignedStart);
        }

        // WebGPU requires writeBuffer's size (and destination offset) to be a
        // multiple of 4 bytes; D3D9's Lock/Unlock byte ranges carry no such
        // guarantee (a 16-bit index buffer update is a common example that
        // is not). D9WG command records are always padded to an 8-byte
        // boundary (D9WG_ALIGN8 in d3d9_proxy.c), so up to 3 extra
        // zero-padding bytes past `byteCount` are always safely readable
        // from the same batch -- this rounds the write size up into that
        // slack rather than crashing the whole batch on an unaligned
        // Direct3D-legal update.
        writeBufferAligned(gpuBuffer, dstOffset, bytes, sourceOffset, byteCount) {
            if (!byteCount) return;
            if (dstOffset % 4 !== 0) {
                console.warn("[d3d9-webgpu] dropping a buffer update at a " +
                    "non-4-byte-aligned destination offset", { dstOffset, byteCount });
                return;
            }
            let writeCount = alignUp(byteCount, 4);
            const available = gpuBuffer.size - dstOffset;
            if (writeCount > available) writeCount = available - (available % 4);
            if (writeCount <= 0) return;
            this.device.queue.writeBuffer(gpuBuffer, dstOffset,
                new Uint8Array(bytes.buffer, bytes.byteOffset + sourceOffset, writeCount));
        }

        onDestroyResource(bytes, view, offset) {
            const handle = view.getUint32(offset, true);
            const kind = view.getUint32(offset + 4, true);
            if (kind === 0) {
                // Matches the D3D8 guest convention: DESTROY_RESOURCE with
                // resource_kind 0 targets the device handle itself, emitted
                // once from device_release() when the app's last reference
                // drops (see d3d9_proxy.c).
                const state = this.devices.get(handle);
                if (state) {
                    state.surface = { ...state.surface, visible: false };
                    if (typeof this.options.onDestroy === "function")
                        this.options.onDestroy(state.surface, "device");
                    this.retireGPUObject(state.depthTexture);
                    this.devices.delete(handle);
                }
                return;
            }
            const resource = this.resources.get(handle);
            if (!resource) return;
            // Never destroy inline. A frame being recorded may already hold a
            // bind group referencing this texture's view or a pending draw
            // referencing this buffer, and none of it is submitted until
            // Present -- destroying now makes WebGPU reject the whole command
            // buffer ("Destroyed texture ... used in a submit"). Releasing a
            // texture in the same frame it was last drawn with is ordinary
            // application behaviour, not an edge case.
            this.retireGPUObject(resource.gpuBuffer);
            this.retireGPUObject(resource.gpuTexture);
            this.resources.delete(handle);
        }

        onCreateTexture2D(bytes, view, offset) {
            const handle = view.getUint32(offset + 4, true);
            const width = view.getUint32(offset + 8, true);
            const height = view.getUint32(offset + 12, true);
            const levelCount = view.getUint32(offset + 16, true);
            const format = view.getUint32(offset + 20, true);
            const usage = view.getUint32(offset + 24, true);
            // A render target or depth surface arrives as a CREATE_TEXTURE_2D
            // carrying the usage (see d3d9_protocol.h): the host needs a GPU
            // texture either way, and D3D9 reaches a texture's render target
            // through GetSurfaceLevel as often as through CreateRenderTarget, so
            // one opcode covers both without two host paths for one object.
            const isDepth = (usage & D3DUSAGE_DEPTHSTENCIL) !== 0;
            const isTarget = (usage & D3DUSAGE_RENDERTARGET) !== 0;
            // Every D3D9 depth format collapses onto one real depth target: the
            // guest cannot read any of them back, so only "a depth buffer
            // exists" is observable (same argument as ensureDepthTarget).
            const gpuFormat = isDepth ? DEPTH_FORMAT : formatToGPU(format);
            if (!gpuFormat) {
                console.warn("[d3d9-webgpu] unsupported texture format", format);
                return;
            }
            if (isTarget && !isDepth && !isRenderableGPUFormat(gpuFormat)) {
                ++this.stats.texturesRejected;
                this.warnOnce("non-renderable-d3d9-target-" + format,
                    "a texture was marked as a D3D9 render target but its " +
                    "WebGPU storage format cannot be a render attachment; " +
                    "the resource is rejected instead of creating an invalid " +
                    "GPU descriptor", { format, gpuFormat });
                return;
            }
            const srgbFormat = isDepth ? null : srgbSiblingOf(gpuFormat);
            const textureDescriptor = {
                label: isDepth ? "D3D9 depth surface"
                    : (isTarget ? "D3D9 render target" : undefined),
                size: { width, height, depthOrArrayLayers: 1 },
                format: gpuFormat,
                ...(srgbFormat ? { viewFormats: [srgbFormat] } : {}),
                mipLevelCount: Math.max(1, levelCount),
                usage: (isDepth
                        ? TEXTURE_USAGE_RENDER_ATTACHMENT
                        : TEXTURE_USAGE_COPY_DST | TEXTURE_USAGE_COPY_SRC |
                          TEXTURE_USAGE_TEXTURE_BINDING) |
                    // A blit writes through a render pass, so anything that can
                    // be a StretchRect destination needs the attachment usage.
                    // BCn cannot be an attachment at all, which is also why a
                    // StretchRect into a compressed texture stays unsupported.
                    ((!isDepth && isRenderableGPUFormat(gpuFormat) &&
                        (isTarget || !isCompressedFormat(format)))
                        ? TEXTURE_USAGE_RENDER_ATTACHMENT : 0),
            };
            const gpuTexture = this.createTextureOrNull(textureDescriptor, format);
            if (!gpuTexture) return;
            ++this.stats.texturesCreated;
            if (isTarget) ++this.stats.renderTargetsCreated;
            // No sampler is attached to the texture: since M2, sampling
            // parameters come from the device's per-stage sampler state
            // through samplerFor(), so the same texture bound to two stages
            // with different filtering behaves the way D3D9 says it should.
            this.resources.set(handle, {
                kind: RESOURCE_TEXTURE_2D, textureType: "2d",
                gpuTexture, gpuFormat, srgbFormat, format, usage, width, height,
                gpuBytesPerTexel: gpuBytesPerTexel(format),
                textureDescriptor,
                levelCount: Math.max(1, levelCount),
                // A mip level the guest never uploads has undefined contents.
                // Sampling one is not "slightly blurry" -- it is whatever was
                // in that memory, which reads as a completely wrong texture.
                // M1 could not hit this the same way because it sampled with
                // one hardcoded sampler; M2 honours D3DSAMP_MIPFILTER, so a
                // game asking for mip filtering now reaches levels that were
                // never written.
                // A render target's or depth surface's levels are written by
                // the GPU, never uploaded, so tracking them would make the
                // incomplete-mip-chain warning fire on every draw that samples
                // a perfectly valid target.
                uploadedLevels: (isTarget || isDepth) ? null : new Set(),
                view: isDepth ? null : gpuTexture.createView(),
            });
        }

        textureShadowFor(resource, level, layer, compressed) {
            if (!resource.textureShadows) resource.textureShadows = new Map();
            const key = level + ":" + layer;
            let shadow = resource.textureShadows.get(key);
            if (shadow) return shadow;
            const width = Math.max(1, resource.width >> level);
            const height = Math.max(1, resource.height >> level);
            let bytesPerRow;
            let rowsPerImage;
            if (compressed) {
                const blockBytes = resource.format === D3DFMT_DXT1 ? 8 : 16;
                bytesPerRow = Math.ceil(width / 4) * blockBytes;
                rowsPerImage = Math.ceil(height / 4);
            } else {
                bytesPerRow = width * resource.gpuBytesPerTexel;
                rowsPerImage = height;
            }
            shadow = { level, layer, width, height, bytesPerRow, rowsPerImage,
                compressed, data: new Uint8Array(bytesPerRow * rowsPerImage) };
            resource.textureShadows.set(key, shadow);
            return shadow;
        }

        updateTextureShadow(resource, level, layer, x, y, width, height,
                payload, sourceBytesPerRow, compressed) {
            const shadow = this.textureShadowFor(resource, level, layer,
                compressed);
            const blockBytes = compressed
                ? (resource.format === D3DFMT_DXT1 ? 8 : 16)
                : resource.gpuBytesPerTexel;
            const destinationX = compressed ? Math.floor(x / 4) : x;
            const destinationY = compressed ? Math.floor(y / 4) : y;
            const rowBytes = compressed
                ? Math.ceil(width / 4) * blockBytes
                : width * resource.gpuBytesPerTexel;
            const rowCount = compressed ? Math.ceil(height / 4) : height;
            for (let row = 0; row < rowCount; ++row) {
                const destinationOffset =
                    (destinationY + row) * shadow.bytesPerRow +
                    destinationX * blockBytes;
                const available = Math.max(0,
                    Math.min(rowBytes, shadow.data.length - destinationOffset,
                        payload.length - row * sourceBytesPerRow));
                if (available)
                    shadow.data.set(payload.subarray(row * sourceBytesPerRow,
                        row * sourceBytesPerRow + available), destinationOffset);
            }
        }

        renameTextureForUpdate(resource) {
            if (!resource.textureDescriptor ||
                    (resource.usage & (D3DUSAGE_RENDERTARGET |
                        D3DUSAGE_DEPTHSTENCIL)) !== 0)
                return false;
            let replacement;
            try {
                replacement = this.device.createTexture({
                    ...resource.textureDescriptor,
                    label: "D3D9 renamed texture",
                });
            } catch (error) {
                this.warnOnce("texture-rename-failed",
                    "a texture updated after an earlier draw could not be " +
                    "renamed; that earlier draw may see the newer pixels", {
                        format: resource.format,
                        size: resource.width + "x" + resource.height,
                        message: error && error.message,
                    });
                return false;
            }
            if (resource.textureShadows) {
                for (const shadow of resource.textureShadows.values()) {
                    this.device.queue.writeTexture({ texture: replacement,
                        mipLevel: shadow.level,
                        origin: { x: 0, y: 0, z: shadow.layer } }, shadow.data,
                        { bytesPerRow: shadow.bytesPerRow,
                            rowsPerImage: shadow.rowsPerImage },
                        { width: blockAlignedCopyExtent(shadow.width,
                                shadow.compressed),
                            height: blockAlignedCopyExtent(shadow.height,
                                shadow.compressed),
                            depthOrArrayLayers: 1 });
                    this.stats.textureFullCopyRenameBytes += shadow.data.length;
                }
            }
            const oldTexture = resource.gpuTexture;
            resource.gpuTexture = replacement;
            resource.view = replacement.createView({ dimension:
                resource.textureType === "cube" ? "cube" : "2d" });
            resource.srgbView = null;
            resource.blitViews = null;
            resource.targetViews = null;
            resource.frameReferenced = 0;
            this.retireGPUObject(oldTexture);
            ++this.stats.textureRenames;
            return true;
        }

        onUpdateTexture(bytes, view, offset) {
            const handle = view.getUint32(offset, true);
            const level = view.getUint32(offset + 4, true);
            const x = view.getUint32(offset + 8, true);
            const y = view.getUint32(offset + 12, true);
            // D9WGUpdateTexture.z is the cube face for a cube texture and the
            // slice for a volume texture; both land on the same WebGPU array
            // layer, which is why one field and one opcode serve both.
            const z = view.getUint32(offset + 16, true);
            const width = view.getUint32(offset + 20, true);
            const height = view.getUint32(offset + 24, true);
            const rowPitch = view.getUint32(offset + 32, true);
            const dataBytes = view.getUint32(offset + 40, true);
            const dataOffset = view.getUint32(offset + 44, true);
            const resource = this.resources.get(handle);
            if (!resource || !resource.gpuTexture) return;
            const source = new Uint8Array(bytes.buffer, bytes.byteOffset + dataOffset, dataBytes);
            const compressed = isCompressedFormat(resource.format);
            // Bind groups are built eagerly and retain the old view. Renaming
            // here therefore preserves pixels for draws already recorded in
            // this frame while later draws bind the replacement texture.
            if (this.frame && resource.frameReferenced === this.frame.serial) {
                ++this.stats.textureUpdateHazards;
                this.renameTextureForUpdate(resource);
            }
            let payload = source;
            let bytesPerRow = rowPitch;
            if (!compressed) {
                const gpuBpp = resource.gpuBytesPerTexel;
                // Expand to the tightly packed WebGPU representation chosen
                // by formatToGPU(): RGBA8 UNORM/SNORM or RGBA16F.
                const expanded = new Uint8Array(width * height * gpuBpp);
                for (let row = 0; row < height; ++row) {
                    expandRowToGPU(resource.format, source,
                        row * rowPitch, width, expanded,
                        row * width * gpuBpp);
                }
                payload = expanded;
                bytesPerRow = width * gpuBpp;
            }
            this.updateTextureShadow(resource, level, z, x, y, width, height,
                payload, bytesPerRow, compressed);
            ++this.stats.textureUploads;
            this.stats.textureBytesUploaded += payload.length;
            // Keyed per layer as well as per level: a cube whose face 0 has a
            // full mip chain and whose face 5 has none is a real defect the
            // per-level-only key would report as complete.
            if (resource.uploadedLevels)
                resource.uploadedLevels.add(level * 6 + (z % 6));
            // Retain a CPU copy of small top-level images. This is the one
            // piece of evidence that separates "the texture data we uploaded
            // is wrong" from "the data is right but we sample it wrong", and
            // guessing between those two has already cost several rounds.
            // Bounded to sprite-sized textures so it cannot grow without
            // limit -- cursors and UI glyphs are exactly this size.
            // 64x64 was too small a net: a game's cursor and UI glyphs
            // usually live in a larger atlas, so the one texture worth looking
            // at was the one never captured. 256x256 covers those at a bounded
            // total cost (previewBudget below).
            if (!compressed && resource.gpuFormat === "rgba8unorm" &&
                    level === 0 && z === 0 && width <= 256 &&
                    height <= 256 && x === 0 && y === 0) {
                const previewBytes = width * height * 4;
                if (!resource.preview) {
                    if (this.previewBudget === undefined)
                        this.previewBudget = 16 * 1024 * 1024;
                    if (this.previewBudget >= previewBytes) {
                        this.previewBudget -= previewBytes;
                        resource.preview = {
                            width, height, rgba: payload.slice()
                        };
                    } else {
                        // The preview is diagnostics only. Returning here used
                        // to drop the real queue.writeTexture below once War3
                        // had loaded 16 MiB of menu atlases. Textures first
                        // touched in battle (fog/minimap/command icons/software
                        // cursor) consequently stayed black or transparent.
                        ++this.stats.texturePreviewsSkipped;
                    }
                } else {
                    resource.preview = { width, height, rgba: payload.slice() };
                }
            }
            // rowsPerImage counts *block* rows for a block-compressed format,
            // not pixel rows -- BCn blocks are 4x4, so a DXT upload that
            // passes the pixel height describes an image four times taller
            // than the data actually is.
            this.device.queue.writeTexture(
                { texture: resource.gpuTexture, mipLevel: level, origin: { x, y, z } },
                payload,
                { bytesPerRow, rowsPerImage: compressed ? Math.ceil(height / 4) : height },
                { width: blockAlignedCopyExtent(width, compressed),
                    height: blockAlignedCopyExtent(height, compressed),
                    depthOrArrayLayers: 1 });
        }

        // ---- render targets, cube textures and blits (M3/M4) ----

        // A cube texture is a six-layer 2D WebGPU texture viewed as "cube".
        // Both views are kept: bind groups need the cube view, and a blit or an
        // upload addresses a single face, which only the layered 2D form can do.
        onCreateTextureCube(bytes, view, offset) {
            const handle = view.getUint32(offset + 4, true);
            const edge = view.getUint32(offset + 8, true);
            const levelCount = Math.max(1, view.getUint32(offset + 12, true));
            const format = view.getUint32(offset + 16, true);
            const usage = view.getUint32(offset + 20, true);
            const gpuFormat = formatToGPU(format);
            if (!gpuFormat) {
                console.warn("[d3d9-webgpu] unsupported cube texture format", format);
                return;
            }
            const srgbFormat = srgbSiblingOf(gpuFormat);
            const textureDescriptor = {
                label: "D3D9 cube " + edge,
                size: { width: edge, height: edge, depthOrArrayLayers: 6 },
                format: gpuFormat,
                ...(srgbFormat ? { viewFormats: [srgbFormat] } : {}),
                mipLevelCount: levelCount,
                usage: TEXTURE_USAGE_COPY_DST | TEXTURE_USAGE_COPY_SRC |
                    TEXTURE_USAGE_TEXTURE_BINDING,
            };
            const gpuTexture = this.createTextureOrNull(textureDescriptor, format);
            if (!gpuTexture) return;
            ++this.stats.texturesCreated;
            ++this.stats.cubeTexturesCreated;
            this.resources.set(handle, {
                kind: RESOURCE_TEXTURE_CUBE, textureType: "cube",
                gpuTexture, gpuFormat, srgbFormat,
                format, usage, width: edge, height: edge, layerCount: 6,
                gpuBytesPerTexel: gpuBytesPerTexel(format),
                textureDescriptor,
                levelCount,
                // Keyed by level*6+face, so the incomplete-mip warning counts a
                // cube's faces independently -- a game that fills face 0's whole
                // chain and leaves face 5 empty is a real bug this would hide if
                // the levels were tracked per-level only.
                uploadedLevels: new Set(),
                view: gpuTexture.createView({ dimension: "cube" }),
            });
        }

        // createTexture throws for a format the device does not support, and an
        // exception here propagates out of the batch and discards the whole
        // frame -- one unsupported texture would blank the screen instead of
        // costing one texture. Contained, counted, and named once instead: the
        // draws that bind it fall back to the 1x1 white stand-in, which is
        // visibly wrong in a way that points at the right place.
        createTextureOrNull(descriptor, d3dFormat) {
            try {
                return this.device.createTexture(descriptor);
            } catch (error) {
                ++this.stats.texturesRejected;
                this.warnOnce("createtexture-" + descriptor.format,
                    "the WebGPU device refused a texture format the D3D9 " +
                    "format table claims to support; every draw sampling it " +
                    "gets the 1x1 white fallback instead", {
                        d3dFormat, gpuFormat: descriptor.format,
                        size: descriptor.size,
                        bcSupported: this.deviceFeatures &&
                            this.deviceFeatures.bc,
                        message: error && error.message,
                    });
                return null;
            }
        }

        onSetScissorRect(bytes, view, offset) {
            const state = this.deviceState(view.getUint32(offset, true));
            const left = view.getInt32(offset + 4, true);
            const top = view.getInt32(offset + 8, true);
            const right = view.getInt32(offset + 12, true);
            const bottom = view.getInt32(offset + 16, true);
            state.scissorRect = { x: Math.max(0, left), y: Math.max(0, top),
                width: Math.max(0, right - left), height: Math.max(0, bottom - top) };
        }

        onSetRenderTarget(bytes, view, offset) {
            const state = this.deviceState(view.getUint32(offset, true));
            const index = view.getUint32(offset + 4, true);
            if (index >= MAX_RENDER_TARGETS) return;
            state.renderTargets[index] = view.getUint32(offset + 8, true);
            state.renderTargetLevels[index] = view.getUint32(offset + 12, true);
            ++this.stats.renderTargetBinds;
        }

        onSetDepthStencilSurface(bytes, view, offset) {
            const state = this.deviceState(view.getUint32(offset, true));
            const handle = view.getUint32(offset + 4, true);
            if (handle === D9WG_AUTO_DEPTH_STENCIL_HANDLE) {
                state.depthTargetHandle = 0;
                state.depthUnbound = false;
                return;
            }
            state.depthTargetHandle = handle;
            state.depthUnbound = handle === 0;
        }

        // Everything a render pass and a pipeline need to agree on about where a
        // draw lands. Resolved per draw rather than cached on the device,
        // because the swap chain's own view does not exist until Present -- a
        // null `view` in slot 0 is the marker finishFrame() substitutes it into.
        //
        // `key` is what groups consecutive ops into one pass: two draws with the
        // same key can share a pass, and a change of key has to end it.
        renderTargetsFor(state) {
            const colors = [];
            const wantsSRGB = (state.renderStates.get(D3DRS_SRGBWRITEENABLE) || 0)
                !== 0;
            let width = state.viewport.width || this.backBufferWidthOf(state);
            let height = state.viewport.height || this.backBufferHeightOf(state);
            let key = "";
            for (let index = 0; index < MAX_RENDER_TARGETS; ++index) {
                const handle = state.renderTargets[index];
                if (!handle) {
                    if (index === 0) {
                        const format = wantsSRGB && this.swapchainSrgbFormat
                            ? this.swapchainSrgbFormat : this.format;
                        colors.push({ view: null, format,
                            swapchain: true });
                        width = this.backBufferWidthOf(state);
                        height = this.backBufferHeightOf(state);
                        key += "bb" + (format === this.format ? "" : "s") + ";";
                    }
                    continue;
                }
                const resource = this.resources.get(handle);
                if (!resource || !resource.gpuTexture) {
                    // A bound-but-unknown target cannot be substituted with the
                    // back buffer: that would draw a render-to-texture pass
                    // straight onto the screen. Report it and drop the slot.
                    this.warnOnce("rt-unknown-" + index,
                        "a render target slot names a resource the host does " +
                        "not know; draws into it are dropped rather than " +
                        "redirected to the back buffer", { index, handle });
                    if (index === 0) return null;
                    continue;
                }
                const resourceFormat = resource.gpuFormat ||
                    formatToGPU(resource.format);
                if (!isRenderableGPUFormat(resourceFormat)) {
                    this.warnOnce("rt-non-renderable-" + index,
                        "a render target slot names a texture whose WebGPU " +
                        "format cannot be a render attachment; draws into it " +
                        "are dropped rather than submitting an invalid pass",
                        { index, handle, format: resource.format,
                            gpuFormat: resourceFormat });
                    if (index === 0) return null;
                    continue;
                }
                const level = state.renderTargetLevels[index] || 0;
                const srgb = wantsSRGB && !!resource.srgbFormat;
                if (wantsSRGB && !resource.srgbFormat)
                    ++this.stats.srgbWriteUnavailable;
                const targetView = this.targetViewFor(resource, level, srgb);
                colors.push({ view: targetView,
                    format: srgb ? resource.srgbFormat : resourceFormat,
                    swapchain: false,
                    resource });
                if (index === 0) {
                    width = Math.max(1, resource.width >> level);
                    height = Math.max(1, resource.height >> level);
                }
                key += handle + "." + level + (srgb ? "s" : "") + ";";
            }
            if (!colors.length) return null;

            // Depth: an explicitly bound surface wins, then the device's auto
            // depth-stencil, and SetDepthStencilSurface(NULL) means neither.
            let depthView = null;
            let depthWidth = 0;
            let depthHeight = 0;
            if (state.depthTargetHandle) {
                const resource = this.resources.get(state.depthTargetHandle);
                if (resource && resource.gpuTexture) {
                    depthView = resource.depthView ||
                        (resource.depthView = resource.gpuTexture.createView());
                    depthWidth = resource.width;
                    depthHeight = resource.height;
                    key += "d" + state.depthTargetHandle;
                }
            } else if (!state.depthUnbound && state.depthView) {
                depthView = state.depthView;
                depthWidth = state.depthWidth;
                depthHeight = state.depthHeight;
                key += "dauto";
            } else {
                key += "dnone";
            }
            // A depth attachment smaller than the colour target is a WebGPU
            // validation error that kills the whole submit, so the mismatch is
            // resolved here (drop depth for this pass) rather than at submit.
            if (depthView && (depthWidth !== width || depthHeight !== height)) {
                ++this.stats.depthTargetSizeMismatches;
                this.warnOnce("rt-depth-mismatch",
                    "a render target is a different size from the depth " +
                    "surface bound with it, so this pass runs without depth " +
                    "testing rather than being rejected wholesale", {
                        target: width + "x" + height,
                        depth: depthWidth + "x" + depthHeight,
                    });
                depthView = null;
                key += "!d";
            }
            return { key, colors, depthView, width, height,
                hasDepth: !!depthView,
                formats: colors.map(color => color.format) };
        }

        // StretchRect between two host-owned surfaces. A same-size, same-format
        // copy is a real GPU copy; anything that scales or changes format goes
        // through a small blit pipeline, because copyTextureToTexture cannot do
        // either. A source or destination naming the back buffer is refused
        // rather than approximated: the swap chain texture only exists inside
        // finishFrame(), and pretending otherwise would silently copy garbage.
        // ---- StretchRect ----
        //
        // Three cases, in increasing cost:
        //
        //   1. Same size, same format, neither side the back buffer -> a real
        //      copyTextureToTexture. No pass, no shader.
        //   2. Anything else -> a blit: one pass drawing a full-viewport quad
        //      that samples the source. This is what covers scaling, format
        //      conversion, and the back buffer -- whose format
        //      (getPreferredCanvasFormat, normally bgra8unorm) differs from the
        //      rgba8unorm every D3D9 texture becomes, so even a same-size
        //      back-buffer copy cannot be a copy.
        //   3. A compressed destination -> still unsupported and counted. BCn
        //      cannot be a render attachment, so there is nothing to draw into.
        //
        // A blit touching the back buffer has to be *deferred* into the frame op
        // list rather than submitted here, for the same reason draws are: the
        // swap chain texture is only valid inside the task that acquired it (see
        // ensureFrame), and a game's frame arrives across several PCI submits.
        // Doing it eagerly is what produced "the host cannot address this
        // surface" -- the back buffer genuinely has no view yet at this point.
        onStretchRect(bytes, view, offset) {
            const state = this.deviceState(view.getUint32(offset, true));
            const sourceHandle = view.getUint32(offset + 4, true);
            const sourceLevel = view.getUint32(offset + 8, true);
            const sourceRect = {
                left: view.getInt32(offset + 12, true),
                top: view.getInt32(offset + 16, true),
                right: view.getInt32(offset + 20, true),
                bottom: view.getInt32(offset + 24, true),
            };
            const destinationHandle = view.getUint32(offset + 28, true);
            const destinationLevel = view.getUint32(offset + 32, true);
            const destinationRect = {
                left: view.getInt32(offset + 36, true),
                top: view.getInt32(offset + 40, true),
                right: view.getInt32(offset + 44, true),
                bottom: view.getInt32(offset + 48, true),
            };
            const filterPoint = view.getUint32(offset + 52, true) !== 0;

            const source = sourceHandle ? this.resources.get(sourceHandle) : null;
            const destination = destinationHandle
                ? this.resources.get(destinationHandle) : null;
            // Handle 0 means the back buffer; a non-zero handle the host does not
            // know is a real error, not a back buffer.
            if ((sourceHandle && !source) || (destinationHandle && !destination) ||
                    (source && !source.gpuTexture) ||
                    (destination && !destination.gpuTexture)) {
                ++this.stats.blitsSkipped;
                this.warnOnce("stretchrect-unknown",
                    "StretchRect names a resource the host does not know; it is " +
                    "skipped rather than copying unrelated memory",
                    { sourceHandle, destinationHandle });
                return;
            }
            const width = sourceRect.right - sourceRect.left;
            const height = sourceRect.bottom - sourceRect.top;
            const destinationWidth = destinationRect.right - destinationRect.left;
            const destinationHeight = destinationRect.bottom - destinationRect.top;
            if (width <= 0 || height <= 0 ||
                    destinationWidth <= 0 || destinationHeight <= 0)
                return;
            if (destination && isCompressedFormat(destination.format)) {
                ++this.stats.blitsSkipped;
                this.warnOnce("stretchrect-compressed-destination",
                    "StretchRect into a block-compressed texture is not " +
                    "implemented: BCn cannot be a render attachment, so there " +
                    "is nothing to draw into, and re-compressing on the CPU " +
                    "would be a different image than the app asked for");
                return;
            }

            const sourceFormat = source ? formatToGPU(source.format) : this.format;
            const destinationFormat = destination
                ? formatToGPU(destination.format) : this.format;
            const scaled = width !== destinationWidth ||
                height !== destinationHeight;
            const swapchainInvolved = !source || !destination;
            if (!swapchainInvolved && !scaled && sourceFormat === destinationFormat) {
                const frame = this.ensureFrame();
                frame.ops.push({ kind: "copy",
                    source: { texture: source.gpuTexture, mipLevel: sourceLevel,
                        origin: { x: sourceRect.left, y: sourceRect.top, z: 0 } },
                    destination: { texture: destination.gpuTexture,
                        mipLevel: destinationLevel,
                        origin: { x: destinationRect.left,
                            y: destinationRect.top, z: 0 } },
                    size: { width, height, depthOrArrayLayers: 1 } });
                source.frameReferenced = frame.serial;
                destination.frameReferenced = frame.serial;
                ++this.stats.blits;
                return;
            }
            if (destination && !isRenderableGPUFormat(destinationFormat)) {
                ++this.stats.blitsSkipped;
                this.warnOnce("stretchrect-non-renderable-destination",
                    "a scaled or format-converting StretchRect targets a " +
                    "texture whose WebGPU format cannot be a render " +
                    "attachment; only a same-size same-format GPU copy is " +
                    "supported", { destinationHandle,
                        format: destination.format, destinationFormat });
                return;
            }

            // Source UVs are normalised against the *level* being read, not the
            // base level, or a StretchRect from mip 2 samples a quarter of the
            // image it asked for.
            const sourceWidth = source
                ? Math.max(1, source.width >> sourceLevel)
                : this.backBufferWidthOf(state);
            const sourceHeight = source
                ? Math.max(1, source.height >> sourceLevel)
                : this.backBufferHeightOf(state);
            const op = {
                kind: "blit",
                sourceView: source
                    ? this.blitSourceView(source, sourceLevel) : null,
                destinationView: destination
                    ? this.targetViewFor(destination, destinationLevel) : null,
                destinationFormat,
                sourceRect: [
                    sourceRect.left / sourceWidth, sourceRect.top / sourceHeight,
                    width / sourceWidth, height / sourceHeight,
                ],
                viewport: [destinationRect.left, destinationRect.top,
                    destinationWidth, destinationHeight],
                filterPoint,
            };
            if (source && this.frame) source.frameReferenced = this.frame.serial;
            this.ensureFrame().ops.push(op);
            ++this.stats.blits;
            if (swapchainInvolved) ++this.stats.blitsThroughBackBuffer;
        }

        // A sampled view of one mip level, cached on the resource.
        blitSourceView(resource, level) {
            if (!resource.blitViews) resource.blitViews = new Map();
            let cached = resource.blitViews.get(level);
            if (!cached) {
                cached = resource.gpuTexture.createView({
                    dimension: "2d", baseMipLevel: level, mipLevelCount: 1,
                    baseArrayLayer: 0, arrayLayerCount: 1,
                });
                resource.blitViews.set(level, cached);
            }
            return cached;
        }

        // Shared with renderTargetsFor(): a render-attachment view of one level.
        targetViewFor(resource, level, srgb) {
            if (!resource.targetViews) resource.targetViews = new Map();
            const key = level + (srgb ? "s" : "");
            let cached = resource.targetViews.get(key);
            if (!cached) {
                cached = resource.gpuTexture.createView({
                    baseMipLevel: level, mipLevelCount: 1, dimension: "2d",
                    baseArrayLayer: 0, arrayLayerCount: 1,
                    ...(srgb ? { format: resource.srgbFormat } : {}),
                });
                resource.targetViews.set(key, cached);
            }
            return cached;
        }

        // One pipeline per destination format. The quad is generated from the
        // vertex index, so a blit needs no vertex buffer at all.
        blitPipelineFor(format, filterPoint) {
            const key = format + "|" + (filterPoint ? "point" : "linear");
            if (!this.blitPipelines) this.blitPipelines = new Map();
            let entry = this.blitPipelines.get(key);
            if (entry) return entry;
            const module = this.moduleFor(`struct D9BlitUniforms {
    source_rect: vec4<f32>,
};
@group(0) @binding(0) var<uniform> blit: D9BlitUniforms;
@group(0) @binding(1) var d9_blit_source: texture_2d<f32>;
@group(0) @binding(2) var d9_blit_sampler: sampler;

struct D9BlitOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn d9_vs_main(@builtin(vertex_index) index: u32) -> D9BlitOutput {
    // Two triangles covering the whole viewport; setViewport restricts the
    // output to the destination rect, so no destination maths is needed here.
    var corners = array<vec2<f32>, 6>(
        vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
        vec2<f32>(0.0, 1.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0));
    let corner = corners[index];
    var result: D9BlitOutput;
    result.position = vec4<f32>(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0,
        0.0, 1.0);
    result.uv = blit.source_rect.xy + corner * blit.source_rect.zw;
    return result;
}

@fragment
fn d9_ps_main(stage_in: D9BlitOutput) -> @location(0) vec4<f32> {
    return textureSample(d9_blit_source, d9_blit_sampler, stage_in.uv);
}
`, "d3d9 blit " + key);
            const bindGroupLayout = this.device.createBindGroupLayout({
                entries: [
                    { binding: 0, visibility: SHADER_STAGE_VERTEX,
                      buffer: { type: "uniform" } },
                    { binding: 1, visibility: SHADER_STAGE_FRAGMENT, texture: {} },
                    { binding: 2, visibility: SHADER_STAGE_FRAGMENT, sampler: {} },
                ],
            });
            const pipeline = this.device.createRenderPipeline({
                label: "D3D9 blit " + key,
                layout: this.device.createPipelineLayout(
                    { bindGroupLayouts: [bindGroupLayout] }),
                vertex: { module, entryPoint: "d9_vs_main" },
                fragment: { module, entryPoint: "d9_ps_main",
                    targets: [{ format }] },
                primitive: { topology: "triangle-list" },
            });
            const sampler = this.device.createSampler({
                magFilter: filterPoint ? "nearest" : "linear",
                minFilter: filterPoint ? "nearest" : "linear",
                addressModeU: "clamp-to-edge", addressModeV: "clamp-to-edge",
            });
            entry = { pipeline, bindGroupLayout, sampler };
            this.blitPipelines.set(key, entry);
            return entry;
        }

        // Runs a recorded blit op inside finishFrame, where the swap chain view
        // exists. `swapView` substitutes for a null source/destination view.
        replayBlit(encoder, op, swapView) {
            const sourceView = op.sourceView || swapView;
            const destinationView = op.destinationView || swapView;
            if (!sourceView || !destinationView) return null;
            const entry = this.blitPipelineFor(op.destinationFormat,
                op.filterPoint);
            const uniform = this.device.createBuffer({
                size: 16,
                usage: BUFFER_USAGE_UNIFORM | BUFFER_USAGE_COPY_DST,
            });
            this.device.queue.writeBuffer(uniform, 0,
                new Float32Array(op.sourceRect));
            const bindGroup = this.device.createBindGroup({
                layout: entry.bindGroupLayout,
                entries: [
                    { binding: 0, resource: { buffer: uniform } },
                    { binding: 1, resource: sourceView },
                    { binding: 2, resource: entry.sampler },
                ],
            });
            const pass = encoder.beginRenderPass({
                colorAttachments: [{ view: destinationView,
                    loadOp: "load", storeOp: "store" }],
            });
            ++this.stats.renderPasses;
            pass.setPipeline(entry.pipeline);
            pass.setBindGroup(0, bindGroup);
            pass.setViewport(op.viewport[0], op.viewport[1], op.viewport[2],
                op.viewport[3], 0, 1);
            pass.draw(6);
            pass.end();
            return uniform;
        }

        rectClearPipelineFor(targets, clearsColor, clearsDepth, clearsStencil) {
            const key = [targets.formats.join(","), clearsColor ? "c" : "-",
                clearsDepth ? "d" : "-", clearsStencil ? "s" : "-"].join("|");
            if (!this.rectClearPipelines) this.rectClearPipelines = new Map();
            let entry = this.rectClearPipelines.get(key);
            if (entry) return entry;
            const colorFields = targets.formats.map((_, index) =>
                "    @location(" + index + ") color" + index + ": vec4<f32>,");
            const colorWrites = targets.formats.map((_, index) =>
                "    result.color" + index + " = clear.color;");
            const depthField = clearsDepth
                ? "    @builtin(frag_depth) depth: f32," : "";
            const depthWrite = clearsDepth
                ? "    result.depth = clear.depth.x;" : "";
            const module = this.moduleFor(`struct D9RectClearUniforms {
    color: vec4<f32>,
    depth: vec4<f32>,
};
@group(0) @binding(0) var<uniform> clear: D9RectClearUniforms;

struct D9RectClearOutput {
${colorFields.join("\n")}
${depthField}
};

@vertex
fn d9_vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    var positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0),
        vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    return vec4<f32>(positions[index], 0.0, 1.0);
}

@fragment
fn d9_ps_main() -> D9RectClearOutput {
    var result: D9RectClearOutput;
${colorWrites.join("\n")}
${depthWrite}
    return result;
}
`, "d3d9 rectangle clear " + key);
            const bindGroupLayout = this.device.createBindGroupLayout({
                entries: [{ binding: 0, visibility: SHADER_STAGE_FRAGMENT,
                    buffer: { type: "uniform" } }],
            });
            const descriptor = {
                label: "D3D9 rectangle clear " + key,
                layout: this.device.createPipelineLayout(
                    { bindGroupLayouts: [bindGroupLayout] }),
                vertex: { module, entryPoint: "d9_vs_main" },
                fragment: { module, entryPoint: "d9_ps_main",
                    targets: targets.formats.map(format => ({ format,
                        writeMask: clearsColor ? 0xF : 0 })) },
                primitive: { topology: "triangle-list" },
            };
            if (targets.hasDepth) {
                const stencil = clearsStencil ? { compare: "always",
                    failOp: "keep", depthFailOp: "keep", passOp: "replace" } : {};
                descriptor.depthStencil = { format: DEPTH_FORMAT,
                    depthWriteEnabled: clearsDepth, depthCompare: "always",
                    stencilFront: stencil, stencilBack: stencil,
                    stencilReadMask: clearsStencil ? 0xff : 0,
                    stencilWriteMask: clearsStencil ? 0xff : 0 };
            }
            entry = { pipeline: this.device.createRenderPipeline(descriptor),
                bindGroupLayout };
            this.rectClearPipelines.set(key, entry);
            return entry;
        }

        replayRectClear(encoder, op, swapView, swapSrgbView) {
            const entry = this.rectClearPipelineFor(op.targets, op.clearsColor,
                op.clearsDepth, op.clearsStencil);
            const uniform = this.device.createBuffer({ size: 32,
                usage: BUFFER_USAGE_UNIFORM | BUFFER_USAGE_COPY_DST });
            this.device.queue.writeBuffer(uniform, 0, new Float32Array([
                op.color.r, op.color.g, op.color.b, op.color.a,
                op.depth, 0, 0, 0,
            ]));
            const bindGroup = this.device.createBindGroup({
                layout: entry.bindGroupLayout,
                entries: [{ binding: 0, resource: { buffer: uniform } }],
            });
            const descriptor = {
                colorAttachments: op.targets.colors.map(color => ({
                    view: color.swapchain
                        ? (color.format === this.swapchainSrgbFormat
                            ? swapSrgbView : swapView)
                        : color.view,
                    loadOp: "load", storeOp: "store",
                })),
            };
            if (op.targets.depthView) descriptor.depthStencilAttachment = {
                view: op.targets.depthView,
                depthLoadOp: "load", depthStoreOp: "store",
                stencilLoadOp: "load", stencilStoreOp: "store",
            };
            const pass = encoder.beginRenderPass(descriptor);
            ++this.stats.renderPasses;
            pass.setPipeline(entry.pipeline);
            pass.setBindGroup(0, bindGroup);
            pass.setStencilReference(op.stencil & 0xff);
            for (const rect of op.rects) {
                pass.setViewport(rect.left, rect.top, rect.right - rect.left,
                    rect.bottom - rect.top, 0, 1);
                pass.draw(3);
            }
            pass.end();
            return uniform;
        }

        colorFillPipelineFor(format) {
            if (!this.colorFillPipelines) this.colorFillPipelines = new Map();
            let entry = this.colorFillPipelines.get(format);
            if (entry) return entry;
            const module = this.moduleFor(`struct D9ColorFillUniforms {
    color: vec4<f32>,
};
@group(0) @binding(0) var<uniform> fill: D9ColorFillUniforms;

@vertex
fn d9_vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    var positions = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0),
        vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    return vec4<f32>(positions[index], 0.0, 1.0);
}

@fragment
fn d9_ps_main() -> @location(0) vec4<f32> {
    return fill.color;
}
`, "d3d9 ColorFill " + format);
            const bindGroupLayout = this.device.createBindGroupLayout({
                entries: [{ binding: 0, visibility: SHADER_STAGE_FRAGMENT,
                    buffer: { type: "uniform" } }],
            });
            const pipeline = this.device.createRenderPipeline({
                label: "D3D9 ColorFill " + format,
                layout: this.device.createPipelineLayout(
                    { bindGroupLayouts: [bindGroupLayout] }),
                vertex: { module, entryPoint: "d9_vs_main" },
                fragment: { module, entryPoint: "d9_ps_main",
                    targets: [{ format }] },
                primitive: { topology: "triangle-list" },
            });
            entry = { pipeline, bindGroupLayout };
            this.colorFillPipelines.set(format, entry);
            return entry;
        }

        replayColorFill(encoder, op) {
            const pass = encoder.beginRenderPass({ colorAttachments: [{
                view: op.targetView, loadOp: op.isFull ? "clear" : "load",
                storeOp: "store", ...(op.isFull ? { clearValue: {
                    r: op.rgba[0], g: op.rgba[1], b: op.rgba[2], a: op.rgba[3],
                } } : {}),
            }] });
            ++this.stats.renderPasses;
            let transient = null;
            if (!op.isFull) {
                const entry = this.colorFillPipelineFor(op.format);
                transient = this.device.createBuffer({ size: 16,
                    usage: BUFFER_USAGE_UNIFORM | BUFFER_USAGE_COPY_DST });
                this.device.queue.writeBuffer(transient, 0,
                    new Float32Array(op.rgba));
                const bindGroup = this.device.createBindGroup({
                    layout: entry.bindGroupLayout,
                    entries: [{ binding: 0, resource: { buffer: transient } }],
                });
                pass.setPipeline(entry.pipeline);
                pass.setBindGroup(0, bindGroup);
                pass.setViewport(op.left, op.top, op.right - op.left,
                    op.bottom - op.top, 0, 1);
                pass.draw(3);
            }
            pass.end();
            return transient;
        }

        // A full ColorFill is a clear pass; a partial one is a viewport-limited
        // draw. This keeps the pixels outside the requested RECT intact.
        // A refusal or failure the guest DLL is reporting. This is the only
        // guest-to-host traffic in the protocol that is not a command, and it
        // exists because everything the guest turns down used to be invisible
        // from the page: the browser console sees a clean stream of valid
        // commands, and the guest's own trace file is inside a VM whose
        // filesystem is not reachable from here. The guest deduplicates by
        // exact text, so each distinct message arrives once no matter how many
        // frames it repeats on.
        onGuestLog(bytes, view, offset, length) {
            const severity = view.getUint32(offset, true);
            const textBytes = view.getUint32(offset + 4, true);
            if (8 + textBytes > length) {
                ++this.stats.malformedBatches;
                throw new Error("D9WG guest log text overruns the command");
            }
            let text = "";
            for (let index = 0; index < textBytes; ++index)
                text += String.fromCharCode(bytes[offset + 8 + index]);
            ++this.stats.guestReports;
            (this.guestReports || (this.guestReports = [])).push(text);
            // Severity picks the console channel only. The identification
            // line the guest sends at startup is info: its job is to make a
            // later silence mean "nothing was refused" rather than "the DLL in
            // the disk image is too old to say anything".
            const report = severity === GUEST_LOG_SEVERITY_FAILED
                ? console.error
                : (severity === GUEST_LOG_SEVERITY_INFO
                    ? console.log : console.warn);
            report.call(console, "[d3d9-guest] " + text);
        }

        onColorFill(bytes, view, offset) {
            const resource = this.resources.get(view.getUint32(offset + 4, true));
            const level = view.getUint32(offset + 8, true);
            const color = view.getUint32(offset + 12, true);
            if (!resource || !resource.gpuTexture ||
                    !(resource.usage & D3DUSAGE_RENDERTARGET)) {
                ++this.stats.blitsSkipped;
                this.warnOnce("colorfill-not-target",
                    "ColorFill on a surface that is not a render target is " +
                    "skipped: WebGPU can only clear an attachment, and faking " +
                    "it with an upload would need a CPU mirror this path " +
                    "deliberately does not keep for GPU-only content");
                return;
            }
            if (!resource.targetViews) resource.targetViews = new Map();
            let targetView = resource.targetViews.get(level);
            if (!targetView) {
                targetView = resource.gpuTexture.createView(
                    { baseMipLevel: level, mipLevelCount: 1 });
                resource.targetViews.set(level, targetView);
            }
            const left = view.getInt32(offset + 16, true);
            const top = view.getInt32(offset + 20, true);
            const right = view.getInt32(offset + 24, true);
            const bottom = view.getInt32(offset + 28, true);
            const fullWidth = Math.max(1, resource.width >> level);
            const fullHeight = Math.max(1, resource.height >> level);
            const clippedLeft = Math.max(0, Math.min(fullWidth, left));
            const clippedTop = Math.max(0, Math.min(fullHeight, top));
            const clippedRight = Math.max(clippedLeft,
                Math.min(fullWidth, right));
            const clippedBottom = Math.max(clippedTop,
                Math.min(fullHeight, bottom));
            if (clippedRight === clippedLeft || clippedBottom === clippedTop)
                return;
            const rgba = [((color >>> 16) & 0xff) / 255,
                ((color >>> 8) & 0xff) / 255, (color & 0xff) / 255,
                ((color >>> 24) & 0xff) / 255];
            const isFull = clippedLeft === 0 && clippedTop === 0 &&
                clippedRight === fullWidth && clippedBottom === fullHeight;
            const frame = this.ensureFrame();
            frame.ops.push({ kind: "color-fill", targetView,
                format: formatToGPU(resource.format), rgba, isFull,
                left: clippedLeft, top: clippedTop,
                right: clippedRight, bottom: clippedBottom });
            resource.frameReferenced = frame.serial;
            ++this.stats.colorFills;
        }

        decodeVertexElements(bytes, view, offset, count) {
            const elements = [];
            for (let i = 0; i < count; ++i) {
                const base = offset + i * 8;
                elements.push({
                    stream: view.getUint16(base, true),
                    byteOffset: view.getUint16(base + 2, true),
                    type: view.getUint8(base + 4),
                    method: view.getUint8(base + 5),
                    usage: view.getUint8(base + 6),
                    usageIndex: view.getUint8(base + 7),
                });
            }
            return elements;
        }

        // What the fixed-function vertex stage needs to know about a decoded
        // D9WGVertexElement array. Returns null when the declaration carries
        // no position at all, which is the one thing the fixed-function stage
        // cannot work around.
        fixedFunctionVertexSignature(elements) {
            let positionType = null;
            let hasColor = false;
            let colorIsBGRA = false;
            let hasColor1 = false;
            let color1IsBGRA = false;
            let hasNormal = false;
            let hasPointSize = false;
            let blendElements = 0;
            const texCoordSets = [];
            for (const element of elements) {
                if (element.usage === DECLUSAGE_BLENDWEIGHT ||
                        element.usage === DECLUSAGE_BLENDINDICES)
                    ++blendElements;
                if (element.usage === DECLUSAGE_POSITION && element.usageIndex === 0)
                    positionType = "world";
                else if (element.usage === DECLUSAGE_POSITIONT && element.usageIndex === 0)
                    positionType = "screen";
                else if (element.usage === DECLUSAGE_COLOR && element.usageIndex === 0) {
                    hasColor = true;
                    // Only a D3DCOLOR-typed diffuse arrives byte-swapped; a
                    // declaration is free to use FLOAT4 instead, and swizzling
                    // that would rotate the channels for no reason.
                    colorIsBGRA = element.type === DECLTYPE_D3DCOLOR;
                } else if (element.usage === DECLUSAGE_COLOR && element.usageIndex === 1) {
                    hasColor1 = true;
                    color1IsBGRA = element.type === DECLTYPE_D3DCOLOR;
                } else if (element.usage === DECLUSAGE_NORMAL && element.usageIndex === 0)
                    hasNormal = true;
                else if (element.usage === DECLUSAGE_PSIZE && element.usageIndex === 0)
                    hasPointSize = true;
                else if (element.usage === DECLUSAGE_TEXCOORD &&
                        element.usageIndex < MAX_TEXCOORD_SETS &&
                        !texCoordSets.includes(element.usageIndex))
                    texCoordSets.push(element.usageIndex);
            }
            if (!positionType) return null;
            // D3D9's fixed-function vertex blending (D3DRS_VERTEXBLEND with
            // D3DTS_WORLDMATRIX(1..3)) is not implemented: only D3DTS_WORLD is
            // ever consumed. A declaration carrying BLENDWEIGHT/BLENDINDICES is
            // therefore drawn with every vertex on world matrix 0, which
            // collapses or contorts a skinned mesh instead of posing it -- and
            // silently, since nothing else in the pipeline can tell. Say so:
            // "the character is missing but its shadow is there" is otherwise
            // indistinguishable from a dozen other causes.
            if (blendElements) {
                ++this.stats.drawsWithUnappliedVertexBlend;
                this.warnOnce("ff-vertex-blend",
                    "a fixed-function draw carries blend weights/indices, but " +
                    "fixed-function vertex blending is not implemented, so " +
                    "every vertex is transformed by world matrix 0 alone; a " +
                    "skinned mesh drawn this way collapses rather than posing",
                    { blendElements, positionType });
            }
            texCoordSets.sort((a, b) => a - b);
            return { positionType, hasColor, colorIsBGRA, hasColor1,
                color1IsBGRA, hasNormal, hasPointSize, texCoordSets,
                hasTexCoord: texCoordSets.length > 0 };
        }

        // ---- fixed-function state signatures (M3) ----
        //
        // Reads the texture-stage state the guest has been sending since M1 and
        // turns it into the shape buildFixedFunctionPixelShader() consumes. Both
        // the WGSL and the pipeline cache key derive from this one object, so a
        // state that changes rendering always changes the key.
        //
        // `coordVaryingFor` maps a stage to the varying that carries its
        // coordinates. With a fixed-function vertex stage that is the stage index
        // itself (the vertex stage already resolved TEXCOORDINDEX and the
        // transform); with a translated vertex shader the varyings are whatever
        // the shader wrote per semantic, so the stage's TEXCOORDINDEX selects
        // among them here instead.
        textureCascadeSignature(state, options) {
            const stageState = (stage, id, fallback) => {
                const value = state.textureStageStates.get(stage * 64 + id);
                return value === undefined ? fallback : value;
            };
            const samplerState = (stage, id, fallback) => {
                const value = state.samplerStates.get(stage * 64 + id);
                return value === undefined ? fallback : value;
            };
            const stages = [];
            let usesTextureFactor = false;
            let usesSpecular = false;
            const unsupported = [];
            for (let index = 0; index < MAX_TEXTURE_STAGES; ++index) {
                // D3D9 defaults: stage 0 modulates its texture with the running
                // colour (which at stage 0 *is* the diffuse colour), every later
                // stage is disabled. The first disabled stage ends the cascade.
                const colorOp = stageState(index, D3DTSS_COLOROP,
                    index === 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE);
                if (colorOp === D3DTOP_DISABLE) break;
                const alphaOp = stageState(index, D3DTSS_ALPHAOP,
                    index === 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
                const stage = {
                    index,
                    colorOp,
                    colorArg0: stageState(index, D3DTSS_COLORARG0, D3DTA_CURRENT),
                    colorArg1: stageState(index, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                    colorArg2: stageState(index, D3DTSS_COLORARG2, D3DTA_CURRENT),
                    alphaOp,
                    alphaArg0: stageState(index, D3DTSS_ALPHAARG0, D3DTA_CURRENT),
                    alphaArg1: stageState(index, D3DTSS_ALPHAARG1, D3DTA_TEXTURE),
                    alphaArg2: stageState(index, D3DTSS_ALPHAARG2, D3DTA_CURRENT),
                    resultArg: stageState(index, D3DTSS_RESULTARG, D3DTA_CURRENT),
                    usesConstant: false,
                    samplesTexture: false,
                    textureType: "2d",
                    transformCount: 0,
                    projected: false,
                    addressU: samplerState(index, D3DSAMP_ADDRESSU, 1),
                    addressV: samplerState(index, D3DSAMP_ADDRESSV, 1),
                    addressW: samplerState(index, D3DSAMP_ADDRESSW, 1),
                    borderColor: samplerState(index, D3DSAMP_BORDERCOLOR, 0),
                };
                // Which arguments a stage reads decides what has to be declared
                // and uploaded for it. Getting this wrong in either direction is
                // a hard failure rather than a shading difference: a texture
                // referenced but not declared is invalid WGSL, and a uniform
                // field referenced but not in the layout reads garbage.
                // Only MULTIPLYADD and LERP read arg0, so including it
                // unconditionally would declare textures and upload constants a
                // stage never touches -- and, worse, would leave the app's stale
                // arg0 deciding what gets bound.
                const readsArg0 = op =>
                    op === D3DTOP_MULTIPLYADD || op === D3DTOP_LERP;
                const argumentsUsed = [stage.colorArg1, stage.colorArg2];
                if (stage.alphaOp !== D3DTOP_DISABLE)
                    argumentsUsed.push(stage.alphaArg1, stage.alphaArg2);
                if (readsArg0(stage.colorOp)) argumentsUsed.push(stage.colorArg0);
                if (readsArg0(stage.alphaOp)) argumentsUsed.push(stage.alphaArg0);
                const opsUsed = [stage.colorOp];
                if (stage.alphaOp !== D3DTOP_DISABLE) opsUsed.push(stage.alphaOp);
                for (const argument of argumentsUsed) {
                    switch (argument & D3DTA_SELECTMASK) {
                    case D3DTA_TEXTURE: stage.samplesTexture = true; break;
                    case D3DTA_TFACTOR: usesTextureFactor = true; break;
                    case D3DTA_CONSTANT: stage.usesConstant = true; break;
                    case D3DTA_SPECULAR: usesSpecular = true; break;
                    default: break;
                    }
                }
                // These operations read a channel of something the arguments
                // never name, so they pull in their own dependency.
                if (opsUsed.includes(D3DTOP_BLENDTEXTUREALPHA) ||
                        opsUsed.includes(D3DTOP_BLENDTEXTUREALPHAPM) ||
                        stage.colorOp === D3DTOP_DOTPRODUCT3)
                    stage.samplesTexture = stage.samplesTexture ||
                        opsUsed.includes(D3DTOP_BLENDTEXTUREALPHA) ||
                        opsUsed.includes(D3DTOP_BLENDTEXTUREALPHAPM);
                if (opsUsed.includes(D3DTOP_BLENDFACTORALPHA))
                    usesTextureFactor = true;
                if (stage.samplesTexture) {
                    const texture = this.resources.get(state.textures.get(index));
                    stage.textureType = texture ? (texture.textureType || "2d") : "2d";
                    stage.hasTextureBound = !!texture;
                }
                const transformFlags =
                    stageState(index, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
                stage.transformCount = transformFlags & 0xFF;
                stage.projected = (transformFlags & D3DTTFF_PROJECTED) !== 0;
                const coordIndex = stageState(index, D3DTSS_TEXCOORDINDEX, index);
                stage.texCoordIndex = coordIndex & 0xFFFF;
                stage.tciMode = coordIndex & D3DTSS_TCI_MASK;
                if (stage.tciMode === D3DTSS_TCI_SPHEREMAP)
                    unsupported.push("stage " + index +
                        " asks for D3DTSS_TCI_SPHEREMAP coordinate generation");
                for (const op of opsUsed) {
                    if (op === D3DTOP_DISABLE) continue;
                    if (textureOpExpression(op, "f32", ["0.0", "0.0", "0.0"],
                            "0.0") === null)
                        unsupported.push("stage " + index + " asks for " +
                            "D3DTEXTUREOP " + op + ", which is outside the set " +
                            "fill_caps() advertises in TextureOpCaps");
                }
                stages.push(stage);
            }
            // With a translated vertex shader the fixed-function coordinate
            // generation and transform never ran, so the stage reads the
            // varying its TEXCOORDINDEX names, untransformed.
            if (!options.fixedVertexStage) {
                for (const stage of stages) {
                    stage.coordVarying = Math.min(stage.texCoordIndex,
                        MAX_TEXCOORD_SETS - 1);
                    if (stage.tciMode !== D3DTSS_TCI_PASSTHRU ||
                            stage.transformCount) {
                        unsupported.push("stage " + stage.index + " asks for " +
                            "fixed-function coordinate generation/transform " +
                            "while a vertex shader is bound, which D3D9 does " +
                            "not apply either");
                        stage.transformCount = 0;
                        stage.projected = false;
                    }
                }
            } else {
                for (const stage of stages) stage.coordVarying = stage.index;
            }
            return { stages, usesTextureFactor, usesSpecular, unsupported };
        }

        // Reads D3DRS_LIGHTING and the material/light state into the shape
        // buildFixedFunctionVertexShader() consumes, or null when the draw is
        // not lit. Each enabled light's *type* is baked into the signature so
        // the generated WGSL is a straight-line unroll with no branching and no
        // dynamic light count.
        lightingSignature(state, vertexSignature) {
            const rs = state.renderStates;
            const get = (id, fallback) => {
                const value = rs.get(id);
                return value === undefined ? fallback : value;
            };
            // Pre-transformed (XYZRHW) vertices are already lit by definition,
            // which is why D3D9 ignores lighting for them.
            if (get(D3DRS_LIGHTING, 1) === 0 ||
                    vertexSignature.positionType === "screen")
                return null;
            // No normal, no lighting. D3DRS_LIGHTING defaults to TRUE, so a
            // large majority of draws with plain pre-coloured vertex formats
            // arrive here with lighting nominally on; running the lighting maths
            // on a zero normal would leave only ambient + emissive, and with
            // D3DRS_AMBIENT defaulting to 0 that renders the geometry black.
            //
            // Dropping lighting instead is what WineD3D's fixed-function vertex
            // pipeline does (its ffp_vs_settings clears `lighting` when the
            // declaration has no normal), and it is the only choice that cannot
            // regress content which relied on the vertex colour reaching the
            // rasteriser. It is counted so a genuinely-lit mesh that lost its
            // normal on the way in is still visible in the stats.
            if (!vertexSignature.hasNormal) {
                ++this.stats.drawsWithUnappliedLighting;
                this.warnOnce("lighting-no-normal",
                    "a draw has D3DRS_LIGHTING enabled but its declaration " +
                    "carries no NORMAL, so lighting is skipped and the vertex " +
                    "colour passes through unchanged", {
                        materialSet: !!state.material,
                        lightsEnabled: [...state.lightEnabled.entries()]
                            .filter(entry => entry[1]).map(entry => entry[0]),
                        ambient: get(D3DRS_AMBIENT, 0),
                    });
                return null;
            }
            const lights = [];
            for (const [index, enabled] of state.lightEnabled) {
                if (!enabled) continue;
                const light = state.lights.get(index);
                if (!light) continue;
                if (lights.length >= MAX_LIGHTS) break;
                lights.push({ index, type: light.type });
            }
            // Sorted so two draws with the same set of enabled lights produce
            // the same key regardless of the order LightEnable arrived in.
            lights.sort((a, b) => a.index - b.index);
            const colorVertex = get(D3DRS_COLORVERTEX, 1) !== 0;
            return {
                lights,
                colorVertex,
                specularEnable: get(D3DRS_SPECULARENABLE, 0) !== 0,
                localViewer: get(D3DRS_LOCALVIEWER, 1) !== 0,
                diffuseSource: get(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1),
                ambientSource: get(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL),
                specularSource: get(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2),
                emissiveSource: get(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL),
            };
        }

        // Turns a declaration plus the currently bound streams into the
        // GPUVertexBufferLayout array a pipeline needs, using `locationFor` to
        // decide which shader input (if any) each element feeds. One function
        // serves both stages: the fixed-function stage passes its semantic
        // table, a translated shader passes a lookup over its own dcl'd
        // inputs. Elements no shader input consumes are simply left out --
        // they still occupy bytes in the vertex, but the stride comes from
        // SetStreamSource, never from summing the elements.
        vertexBufferLayoutsFor(elements, state, locationFor, instanceData) {
            const perStream = new Map();
            for (const element of elements) {
                const location = locationFor(element);
                if (location < 0) continue;
                const format = DECLTYPE_FORMATS[element.type];
                if (!format) {
                    this.warnOnce("decltype-" + element.type,
                        "unsupported D3DDECLTYPE " + element.type +
                        "; the attribute is dropped from the vertex layout");
                    continue;
                }
                const stream = state.streams.get(element.stream);
                if (!stream || !stream.stride) continue;
                let entry = perStream.get(element.stream);
                if (!entry) {
                    entry = { stream: element.stream, arrayStride: stream.stride,
                        stepMode: instanceData ? "instance" : "vertex",
                        attributes: [] };
                    perStream.set(element.stream, entry);
                }
                entry.attributes.push({ shaderLocation: location,
                    offset: element.byteOffset, format: format[0] });
            }
            // Sorted so the slot a buffer binds to is a stable function of the
            // declaration, which keeps the pipeline cache key stable too.
            const layouts = Array.from(perStream.values())
                .sort((a, b) => a.stream - b.stream);
            for (const layout of layouts)
                layout.attributes.sort((a, b) => a.shaderLocation - b.shaderLocation);
            return layouts;
        }

        onCreateVertexDeclaration(bytes, view, offset, length) {
            const handle = view.getUint32(offset + 4, true);
            const count = view.getUint32(offset + 8, true);
            const elements = this.decodeVertexElements(bytes, view, offset + 16, count);
            this.resources.set(handle,
                { kind: RESOURCE_VERTEX_DECLARATION, elements });
        }

        onSetVertexDeclaration(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const declarationHandle = view.getUint32(offset + 4, true);
            const state = this.deviceState(deviceHandle);
            state.vertexDeclarationHandle = declarationHandle;
            state.fvfElements = null;
        }

        onSetFVF(bytes, view, offset, length) {
            const deviceHandle = view.getUint32(offset, true);
            const count = view.getUint32(offset + 8, true);
            const state = this.deviceState(deviceHandle);
            state.fvfElements = this.decodeVertexElements(bytes, view, offset + 16, count);
            state.vertexDeclarationHandle = 0;
        }

        // The declaration in force, whether it arrived as a real
        // IDirect3DVertexDeclaration9 or as an FVF the guest expanded into the
        // same element shape (plan 4.3 -- the host has exactly one
        // vertex-layout code path).
        currentElements(state) {
            if (state.fvfElements) return state.fvfElements;
            const declaration = this.resources.get(state.vertexDeclarationHandle);
            return declaration ? declaration.elements : null;
        }

        onSetRenderState(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const stateId = view.getUint32(offset + 4, true);
            const value = view.getUint32(offset + 8, true);
            this.deviceState(deviceHandle).renderStates.set(stateId, value);
            this.noteUnreadState("renderState", CONSUMED_RENDER_STATES, stateId);
            if (stateId === D3DRS_SRGBWRITEENABLE && value !== 0) {
                ++this.stats.srgbWriteRequests;
            }
        }

        // Every state the guest sends that nothing here reads. This exists
        // because the expensive failures on this path have all been silent ones:
        // a state the app clearly cares about (it would not call Set otherwise)
        // that the renderer never looks at produces a picture that is wrong in a
        // plausible way, with nothing anywhere saying so. Listing them turns
        // "why does this look wrong" into a finite list to work through.
        //
        // Bounded by construction: it records ids, not occurrences, and there
        // are only a few hundred possible ids.
        noteUnreadState(kind, consumed, stateId) {
            if (consumed.has(stateId)) return;
            const key = kind + "s";
            if (!this.unreadStates) this.unreadStates = {};
            const seen = this.unreadStates[key] || (this.unreadStates[key] = new Set());
            if (seen.has(stateId)) return;
            seen.add(stateId);
            this.stats.unreadStateIds = Object.keys(this.unreadStates)
                .reduce((out, name) => {
                    out[name] = [...this.unreadStates[name]].sort((a, b) => a - b);
                    return out;
                }, {});
        }

        onSetTextureStageState(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const stage = view.getUint32(offset + 4, true);
            const stateId = view.getUint32(offset + 8, true);
            const value = view.getUint32(offset + 12, true);
            this.deviceState(deviceHandle).textureStageStates.set(stage * 64 + stateId, value);
        }

        onSetTexture(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const stage = view.getUint32(offset + 4, true);
            const textureHandle = view.getUint32(offset + 8, true);
            this.deviceState(deviceHandle).textures.set(stage, textureHandle);
        }

        // Stored to keep host state honest with the guest's D9WGSetSamplerState/
        // D9WGSetMaterial/D9WGSetLight/D9WGLightEnable emitters (d3d9_proxy.c),
        // but not yet read anywhere in pipelineFor()/recordDraw() -- M1's fixed
        // shader always uses one hardcoded default sampler and never applies
        // lighting math. Real consumption is M2 (sampler variants, 4.4/12) and
        // M2/M3 (lighting) work.
        onSetSamplerState(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const sampler = view.getUint32(offset + 4, true);
            const stateId = view.getUint32(offset + 8, true);
            this.noteUnreadState("samplerState", CONSUMED_SAMPLER_STATES, stateId);
            const value = view.getUint32(offset + 12, true);
            this.deviceState(deviceHandle).samplerStates.set(sampler * 64 + stateId, value);
        }

        onSetMaterial(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const readVec4 = base => [
                view.getFloat32(base, true), view.getFloat32(base + 4, true),
                view.getFloat32(base + 8, true), view.getFloat32(base + 12, true),
            ];
            this.deviceState(deviceHandle).material = {
                diffuse: readVec4(offset + 4), ambient: readVec4(offset + 20),
                specular: readVec4(offset + 36), emissive: readVec4(offset + 52),
                power: view.getFloat32(offset + 68, true),
            };
        }

        onSetLight(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const index = view.getUint32(offset + 4, true);
            const readVec = (base, count) => {
                const out = [];
                for (let i = 0; i < count; ++i) out.push(view.getFloat32(base + i * 4, true));
                return out;
            };
            this.deviceState(deviceHandle).lights.set(index, {
                type: view.getUint32(offset + 8, true),
                diffuse: readVec(offset + 12, 4), specular: readVec(offset + 28, 4),
                ambient: readVec(offset + 44, 4), position: readVec(offset + 60, 3),
                direction: readVec(offset + 72, 3), range: view.getFloat32(offset + 84, true),
                falloff: view.getFloat32(offset + 88, true),
                attenuation: readVec(offset + 92, 3),
                theta: view.getFloat32(offset + 104, true), phi: view.getFloat32(offset + 108, true),
            });
        }

        onLightEnable(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const index = view.getUint32(offset + 4, true);
            const enable = view.getUint32(offset + 8, true) !== 0;
            this.deviceState(deviceHandle).lightEnabled.set(index, enable);
        }

        onSetViewport(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const state = this.deviceState(deviceHandle);
            // MinZ/MaxZ have been on the wire since M1 (D9WGSetViewport) and
            // used to be dropped here, with recordDraw hardcoding 0..1 into
            // setViewport. D3D9's viewport depth range is not decoration: an app
            // that composites a 3D object into a 2D panel routinely restricts it
            // so the object cannot collide in depth with the interface around
            // it, and discarding that puts the object at its natural depth --
            // where whatever the app expected to be in front of it no longer is.
            const minZ = view.getFloat32(offset + 20, true);
            const maxZ = view.getFloat32(offset + 24, true);
            state.viewport = {
                x: view.getUint32(offset + 4, true),
                y: view.getUint32(offset + 8, true),
                width: view.getUint32(offset + 12, true),
                height: view.getUint32(offset + 16, true),
                // A device that never calls SetViewport, and any guest that
                // sends zeros, still has to mean the D3D9 default range.
                minZ: Number.isFinite(minZ) ? minZ : 0,
                maxZ: Number.isFinite(maxZ) && maxZ !== 0 ? maxZ : 1,
            };
        }

        onSetTransform(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const transformState = view.getUint32(offset + 4, true);
            const matrix = new Float32Array(16);
            for (let i = 0; i < 16; ++i) matrix[i] = view.getFloat32(offset + 8 + i * 4, true);
            // Stored exactly as D3D sent it (row-major, row-vector
            // convention). No transpose here -- see uniformBufferFor for why
            // none is needed anywhere on this path.
            this.deviceState(deviceHandle).transforms.set(transformState, matrix);
        }

        onSetStreamSource(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const stream = view.getUint32(offset + 4, true);
            const bufferHandle = view.getUint32(offset + 8, true);
            const stride = view.getUint32(offset + 12, true);
            const offsetInBytes = view.getUint32(offset + 16, true);
            this.deviceState(deviceHandle).streams.set(stream,
                { bufferHandle, stride, offsetInBytes });
        }

        onSetIndices(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const bufferHandle = view.getUint32(offset + 4, true);
            this.deviceState(deviceHandle).indexBufferHandle = bufferHandle;
        }

        // ---- programmable shaders (M2) ----

        // Translation happens here, at CREATE time, and never on a draw path
        // (plan 4.2): a shader first used mid-frame would otherwise produce an
        // unattributable latency spike. A shader this build cannot translate
        // is stored with its error rather than dropped -- recordDraw() then
        // skips draws that bind it and counts them, which is the difference
        // between "this shader is unsupported" and "the game stopped drawing".
        async onCreateShader(bytes, view, offset, kind) {
            const handle = view.getUint32(offset + 4, true);
            const tokenCount = view.getUint32(offset + 8, true);
            const codeOffset = view.getUint32(offset + 12, true);
            const hashLow = view.getUint32(offset + 16, true);
            const hashHigh = view.getUint32(offset + 20, true);
            if (codeOffset + tokenCount * 4 > bytes.byteLength) {
                ++this.stats.malformedBatches;
                throw new Error("D9WG shader bytecode overruns the batch");
            }
            // The DMA blob is not 4-byte aligned within `bytes` in general,
            // so copy rather than aliasing a Uint32Array onto it.
            const tokens = new Uint32Array(tokenCount);
            for (let i = 0; i < tokenCount; ++i)
                tokens[i] = view.getUint32(codeOffset + i * 4, true);
            const compilesBefore = this.shaderCache.stats.compiles;
            let translated = this.shaderCache.get(hashLow, hashHigh);
            if (!translated) {
                const started = typeof performance !== "undefined" && performance.now
                    ? performance.now() : Date.now();
                const workerCompile = this.compileShaderInWorker(tokens);
                if (workerCompile) {
                    try {
                        translated = await workerCompile;
                        if (!translated || typeof translated.ok !== "boolean")
                            throw new Error("shader worker returned an invalid result");
                        ++this.stats.shaderWorkerCompiles;
                        const ended = typeof performance !== "undefined" && performance.now
                            ? performance.now() : Date.now();
                        translated = this.shaderCache.store(hashLow, hashHigh,
                            translated, ended - started);
                    } catch (error) {
                        ++this.stats.shaderWorkerFallbacks;
                        this.warnOnce("shader-worker-runtime",
                            "shader compile Worker failed; falling back to the " +
                            "executor thread", { message: String(error) });
                        translated = this.shaderCache.compile(tokens,
                            hashLow, hashHigh);
                    }
                } else {
                    translated = this.shaderCache.compile(tokens,
                        hashLow, hashHigh);
                }
            }
            if (this.shaderCache.stats.compiles !== compilesBefore)
                this.shaderCacheDirty = true;
            if (translated.ok) ++this.stats.shadersTranslated;
            else {
                ++this.stats.shaderTranslationFailures;
                this.warnOnce("shader-translate-" + hashHigh + "-" + hashLow,
                    "cannot translate a " + (kind === RESOURCE_VERTEX_SHADER
                        ? "vertex" : "pixel") + " shader; draws that bind it " +
                    "will be skipped: " + translated.error);
            }
            this.resources.set(handle, {
                kind, tokens, hashLow, hashHigh,
                translated,
                // Variant key -> {translated, module}. A vertex shader needs
                // one variant per set of D3DCOLOR input locations; in practice
                // that is a single entry, because a given shader is used with
                // one vertex format.
                variants: new Map(),
            });
        }

        onCreateVertexShader(bytes, view, offset) {
            return this.onCreateShader(bytes, view, offset, RESOURCE_VERTEX_SHADER);
        }

        onCreatePixelShader(bytes, view, offset) {
            return this.onCreateShader(bytes, view, offset, RESOURCE_PIXEL_SHADER);
        }

        onSetVertexShader(bytes, view, offset) {
            const state = this.deviceState(view.getUint32(offset, true));
            state.vertexShaderHandle = view.getUint32(offset + 4, true);
        }

        onSetPixelShader(bytes, view, offset) {
            const state = this.deviceState(view.getUint32(offset, true));
            state.pixelShaderHandle = view.getUint32(offset + 4, true);
        }

        // Shared decode for all six SET_*_SHADER_CONSTANT_* opcodes: they use
        // one payload shape and differ only in the destination array and how
        // wide a register is on the wire (float4/int4 = 16 bytes, bool = 4).
        applyConstants(bytes, view, offset, target, componentsPerRegister, read) {
            const startRegister = view.getUint32(offset + 4, true);
            const vectorCount = view.getUint32(offset + 8, true);
            const dataOffset = view.getUint32(offset + 12, true);
            const stride = componentsPerRegister * 4;
            if (dataOffset + vectorCount * stride > bytes.byteLength) {
                ++this.stats.malformedBatches;
                throw new Error("D9WG shader constant data overruns the batch");
            }
            const capacity = target.length / componentsPerRegister;
            if (startRegister >= capacity) return;
            const count = Math.min(vectorCount, capacity - startRegister);
            for (let i = 0; i < count; ++i) {
                const base = dataOffset + i * stride;
                const destination = (startRegister + i) * componentsPerRegister;
                for (let c = 0; c < componentsPerRegister; ++c)
                    target[destination + c] = read(base + c * 4);
            }
        }

        onSetVertexShaderConstantF(bytes, view, offset) {
            const state = this.deviceState(view.getUint32(offset, true));
            this.applyConstants(bytes, view, offset, state.vsConstF, 4,
                at => view.getFloat32(at, true));
        }

        onSetPixelShaderConstantF(bytes, view, offset) {
            const state = this.deviceState(view.getUint32(offset, true));
            this.applyConstants(bytes, view, offset, state.psConstF, 4,
                at => view.getFloat32(at, true));
        }

        onSetVertexShaderConstantI(bytes, view, offset) {
            const state = this.deviceState(view.getUint32(offset, true));
            this.applyConstants(bytes, view, offset, state.vsConstI, 4,
                at => view.getInt32(at, true));
        }

        onSetPixelShaderConstantI(bytes, view, offset) {
            const state = this.deviceState(view.getUint32(offset, true));
            this.applyConstants(bytes, view, offset, state.psConstI, 4,
                at => view.getInt32(at, true));
        }

        onSetVertexShaderConstantB(bytes, view, offset) {
            const state = this.deviceState(view.getUint32(offset, true));
            this.applyConstants(bytes, view, offset, state.vsConstB, 1,
                at => view.getUint32(at, true));
        }

        onSetPixelShaderConstantB(bytes, view, offset) {
            const state = this.deviceState(view.getUint32(offset, true));
            this.applyConstants(bytes, view, offset, state.psConstB, 1,
                at => view.getUint32(at, true));
        }

        // ---- D3D9 hardware cursor ----
        //
        // A fullscreen D3D9 game draws its pointer through SetCursorProperties
        // rather than GDI, so it never reaches the VGA framebuffer the page
        // composites under this canvas -- and the page hides the browser
        // cursor. Without this the pointer is invisible even though input
        // still works, which makes the game effectively unplayable.
        onSetCursorProperties(bytes, view, offset) {
            const width = view.getUint32(offset + 12, true);
            const height = view.getUint32(offset + 16, true);
            const dataBytes = view.getUint32(offset + 20, true);
            const dataOffset = view.getUint32(offset + 24, true);
            if (!width || !height) return;
            if (dataOffset + dataBytes > bytes.byteLength) {
                ++this.stats.malformedBatches;
                throw new Error("D9WG cursor bitmap overruns the batch");
            }
            this.cursor.hotspotX = view.getUint32(offset + 4, true);
            this.cursor.hotspotY = view.getUint32(offset + 8, true);
            if (this.cursor.width !== width || this.cursor.height !== height ||
                    !this.cursor.texture) {
                this.retireGPUObject(this.cursor.texture);
                this.cursor.texture = this.device.createTexture({
                    label: "D3D9 hardware cursor",
                    size: { width, height, depthOrArrayLayers: 1 },
                    format: "rgba8unorm",
                    usage: TEXTURE_USAGE_COPY_DST | TEXTURE_USAGE_TEXTURE_BINDING,
                });
                this.cursor.view = this.cursor.texture.createView();
                this.cursor.width = width;
                this.cursor.height = height;
            }
            // The guest sends A8R8G8B8 at a tight width*4 stride; the same
            // BGRA-to-RGBA reorder every other texture upload does.
            const source = new Uint8Array(bytes.buffer,
                bytes.byteOffset + dataOffset, dataBytes);
            const rgba = new Uint8Array(width * height * 4);
            for (let row = 0; row < height; ++row)
                expandRowToGPU(D3DFMT_A8R8G8B8, source, row * width * 4,
                    width, rgba, row * width * 4);
            this.device.queue.writeTexture({ texture: this.cursor.texture },
                rgba, { bytesPerRow: width * 4, rowsPerImage: height },
                { width, height, depthOrArrayLayers: 1 });
            ++this.stats.cursorUploads;
        }

        onSetCursorPosition(bytes, view, offset) {
            this.cursor.x = view.getInt32(offset + 4, true);
            this.cursor.y = view.getInt32(offset + 8, true);
        }

        onShowCursor(bytes, view, offset) {
            this.cursor.visible = view.getUint32(offset + 4, true) !== 0;
        }

        // Purely diagnostic; see D9WG_OP_WINDOW_STATE in d3d9_protocol.h for
        // why the guest's window-manager view has to be reported rather than
        // inferred from the rendering.
        onWindowState(bytes, view, offset) {
            const flags = view.getUint32(offset + 12, true);
            const state = {
                hwnd: view.getUint32(offset + 4, true),
                foregroundHwnd: view.getUint32(offset + 8, true),
                isWindow: (flags & D9WG_WINDOW_IS_WINDOW) !== 0,
                visible: (flags & D9WG_WINDOW_VISIBLE) !== 0,
                iconic: (flags & D9WG_WINDOW_ICONIC) !== 0,
                foreground: (flags & D9WG_WINDOW_FOREGROUND) !== 0,
                fullscreen: (flags & D9WG_WINDOW_FULLSCREEN) !== 0,
                windowX: view.getInt32(offset + 16, true),
                windowY: view.getInt32(offset + 20, true),
                windowWidth: view.getUint32(offset + 24, true),
                windowHeight: view.getUint32(offset + 28, true),
                clientWidth: view.getUint32(offset + 32, true),
                clientHeight: view.getUint32(offset + 36, true),
            };
            this.windowState = state;
            ++this.stats.windowStateChanges;
            if (!state.foreground) {
                ++this.stats.windowNotForegroundReports;
                // The guest re-takes the foreground for a fullscreen device
                // (maintain_fullscreen_foreground in d3d9_proxy.c), so a single
                // report of this at startup is normal and self-healing. Repeated
                // reports mean the claim is being refused, which is worth saying
                // out loud: the picture looks perfect either way and every click
                // goes somewhere else, so there is nothing on screen to notice.
                if (this.stats.windowNotForegroundReports > 1)
                    this.warnOnce("window-not-foreground",
                        "the game's window keeps losing the guest's foreground " +
                        "even though the guest re-claims it, so clicks go to " +
                        "whatever is on top -- the overlay still shows this " +
                        "game's frames either way. Check getStats().window",
                        state);
            }
            if (state.iconic || !state.visible)
                this.warnOnce("window-not-visible",
                    "the game's window is minimised or hidden in the guest; " +
                    "input will not reach it", state);
        }

        // A self-contained screen-space quad, built once. It deliberately does
        // not go through programFor()/pipelineFor(): the cursor is host-owned
        // compositing, not a guest draw, and giving it its own trivial
        // pipeline keeps it out of the caches keyed on guest state.
        ensureCursorPipeline() {
            if (this.cursor.pipeline) return this.cursor.pipeline;
            const module = this.device.createShaderModule({
                label: "D3D9 cursor",
                code: `
struct CursorRect { origin: vec2<f32>, size: vec2<f32> };
@group(0) @binding(0) var<uniform> rect: CursorRect;
@group(0) @binding(1) var cursor_texture: texture_2d<f32>;
@group(0) @binding(2) var cursor_sampler: sampler;

struct VSOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VSOut {
    // Two triangles covering the cursor's rectangle, in normalised
    // back-buffer space supplied by the uniform.
    var corners = array<vec2<f32>, 6>(
        vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
        vec2<f32>(0.0, 1.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0));
    let corner = corners[index];
    let position = rect.origin + corner * rect.size;
    var out: VSOut;
    out.position = vec4<f32>(position.x * 2.0 - 1.0,
        1.0 - position.y * 2.0, 0.0, 1.0);
    out.uv = corner;
    return out;
}

@fragment
fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
    return textureSample(cursor_texture, cursor_sampler, in.uv);
}
`,
            });
            const bindGroupLayout = this.device.createBindGroupLayout({
                entries: [
                    { binding: 0, visibility: SHADER_STAGE_VERTEX, buffer: { type: "uniform" } },
                    { binding: 1, visibility: SHADER_STAGE_FRAGMENT, texture: {} },
                    { binding: 2, visibility: SHADER_STAGE_FRAGMENT, sampler: {} },
                ],
            });
            this.cursor.bindGroupLayout = bindGroupLayout;
            this.cursor.sampler = this.device.createSampler({
                magFilter: "nearest", minFilter: "nearest",
                addressModeU: "clamp-to-edge", addressModeV: "clamp-to-edge",
            });
            this.cursor.uniform = this.device.createBuffer({
                label: "D3D9 cursor rect", size: 16,
                usage: BUFFER_USAGE_UNIFORM | BUFFER_USAGE_COPY_DST,
            });
            this.cursor.pipeline = this.device.createRenderPipeline({
                layout: this.device.createPipelineLayout(
                    { bindGroupLayouts: [bindGroupLayout] }),
                vertex: { module, entryPoint: "vs_main" },
                fragment: { module, entryPoint: "fs_main", targets: [{
                    format: this.format,
                    // Straight alpha: a cursor bitmap's transparent texels
                    // must not paint over the frame.
                    blend: {
                        color: { srcFactor: "src-alpha",
                                 dstFactor: "one-minus-src-alpha", operation: "add" },
                        alpha: { srcFactor: "one",
                                 dstFactor: "one-minus-src-alpha", operation: "add" },
                    },
                }] },
                primitive: { topology: "triangle-list" },
            });
            return this.cursor.pipeline;
        }

        // Drawn last, in its own depth-less pass, so it sits on top of the
        // frame regardless of what the game left in the depth buffer.
        drawCursor(encoder, targetView, width, height) {
            const cursor = this.cursor;
            if (!cursor.visible || !cursor.view || !width || !height) return;
            const pipeline = this.ensureCursorPipeline();
            const originX = (cursor.x - cursor.hotspotX) / width;
            const originY = (cursor.y - cursor.hotspotY) / height;
            this.device.queue.writeBuffer(cursor.uniform, 0, new Float32Array([
                originX, originY, cursor.width / width, cursor.height / height,
            ]));
            const bindGroup = this.device.createBindGroup({
                layout: cursor.bindGroupLayout,
                entries: [
                    { binding: 0, resource: { buffer: cursor.uniform } },
                    { binding: 1, resource: cursor.view },
                    { binding: 2, resource: cursor.sampler },
                ],
            });
            const pass = encoder.beginRenderPass({
                colorAttachments: [{ view: targetView, loadOp: "load", storeOp: "store" }],
            });
            pass.setPipeline(pipeline);
            pass.setBindGroup(0, bindGroup);
            pass.draw(6);
            pass.end();
            ++this.stats.cursorDraws;
        }

        // GPUShaderModules are content-addressed by their WGSL text, so two
        // shaders that translate identically (or the same shader re-created
        // after a Reset) share one module. Compilation diagnostics are checked
        // asynchronously -- createShaderModule() never throws on bad WGSL, and
        // plan 9.6 requires the getCompilationInfo() check rather than
        // assuming successful translation implies valid output.
        moduleFor(wgsl, label) {
            let module = this.moduleCache.get(wgsl);
            if (module) return module;
            module = this.device.createShaderModule({ label, code: wgsl });
            ++this.stats.shaderModulesCreated;
            this.moduleCache.set(wgsl, module);
            if (typeof module.getCompilationInfo === "function") {
                module.getCompilationInfo().then(info => {
                    const errors = (info.messages || [])
                        .filter(message => message.type === "error");
                    if (!errors.length) return;
                    module._d9wgBroken = true;
                    this.stats.shaderCompileErrors += errors.length;
                    console.error("[d3d9-webgpu] WGSL compilation failed for " +
                        label, errors.map(e => e.lineNum + ":" + e.linePos +
                            " " + e.message), wgsl);
                }, () => {});
            }
            return module;
        }

        // ---- draw pipeline ----

        // Distils the render states a WebGPU render pipeline is allowed to
        // depend on into a small plain object. WebGPU bakes depth/blend/cull
        // into the immutable pipeline (unlike D3D9, where they are free-
        // floating device state), so this is also exactly the set that has
        // to participate in the pipeline cache key.
        pipelineStateFor(state, targets) {
            const rs = state.renderStates;
            const hasDepth = targets ? targets.hasDepth : !!state.hasDepth;
            const get = (id, fallback) => {
                const value = rs.get(id);
                return value === undefined ? fallback : value;
            };

            // D3DRS_ZENABLE defaults to on whenever a depth buffer exists;
            // with no depth attachment there is nothing to test against.
            const depthEnabled = hasDepth
                && get(D3DRS_ZENABLE, 1) !== D3DZB_FALSE;
            const depthWrite = get(D3DRS_ZWRITEENABLE, 1) !== 0;
            const depthCompare = this.debug.disableDepthTest ? "always"
                : (COMPARE_FUNCS[get(D3DRS_ZFUNC, 4)] || "less-equal");

            const blendEnabled = get(D3DRS_ALPHABLENDENABLE, 0) !== 0;
            const resolveBlend = (rawSrc, rawDst) => {
                // The legacy BOTH* source values override both halves of the
                // pair. D3D9 exposes them as one enum; WebGPU exposes the two
                // factors separately, so the mapping is exact once resolved.
                if (rawSrc === 12)
                    return { src: "src-alpha", dst: "one-minus-src-alpha",
                        valid: true };
                if (rawSrc === 13)
                    return { src: "one-minus-src-alpha", dst: "src-alpha",
                        valid: true };
                return { src: BLEND_FACTORS[rawSrc], dst: BLEND_FACTORS[rawDst],
                    valid: !!BLEND_FACTORS[rawSrc] && !!BLEND_FACTORS[rawDst] };
            };
            // D3D9 starts at ONE/ZERO.  Using SRCALPHA/INVSRCALPHA here was
            // mostly hidden while blending stayed disabled, but it changes an
            // app that enables blending before choosing new factors from an
            // overwrite into a translucent UI/overlay pass.
            const rawColorSrc = get(D3DRS_SRCBLEND, 2);
            const rawColorDst = get(D3DRS_DESTBLEND, 1);
            const rawColorOp = get(D3DRS_BLENDOP, 1);
            const colorBlend = resolveBlend(rawColorSrc, rawColorDst);
            const separateAlpha =
                get(D3DRS_SEPARATEALPHABLENDENABLE, 0) !== 0;
            const rawAlphaSrc = separateAlpha
                ? get(D3DRS_SRCBLENDALPHA, 2) : rawColorSrc;
            const rawAlphaDst = separateAlpha
                ? get(D3DRS_DESTBLENDALPHA, 1) : rawColorDst;
            const rawAlphaOp = separateAlpha
                ? get(D3DRS_BLENDOPALPHA, 1) : rawColorOp;
            const alphaBlend = separateAlpha
                ? resolveBlend(rawAlphaSrc, rawAlphaDst) : colorBlend;
            const srcFactor = colorBlend.src || "src-alpha";
            const dstFactor = colorBlend.dst || "one-minus-src-alpha";
            const blendOp = BLEND_OPS[rawColorOp] || "add";
            const alphaSrcFactor = alphaBlend.src || "one";
            const alphaDstFactor = alphaBlend.dst || "zero";
            const alphaBlendOp = BLEND_OPS[rawAlphaOp] || "add";
            if (blendEnabled && (!colorBlend.valid || !alphaBlend.valid ||
                    !BLEND_OPS[rawColorOp] || !BLEND_OPS[rawAlphaOp])) {
                ++this.stats.drawsWithUnmappedBlend;
                this.warnOnce("unmapped-blend-" + [rawColorSrc, rawColorDst,
                    rawColorOp, rawAlphaSrc, rawAlphaDst, rawAlphaOp].join("-"),
                    "a draw asks for a blend mode with no WebGPU equivalent; " +
                    "it silently falls back to src-alpha/inv-src-alpha, which " +
                    "renders as plausible-but-wrong compositing", {
                        D3DRS_SRCBLEND: rawColorSrc, mappedSrc: colorBlend.src,
                        D3DRS_DESTBLEND: rawColorDst, mappedDst: colorBlend.dst,
                        D3DRS_BLENDOP: rawColorOp, mappedOp: BLEND_OPS[rawColorOp],
                    });
            }
            const blendColor = get(D3DRS_BLENDFACTOR, 0xffffffff) >>> 0;
            const blendConstant = {
                r: ((blendColor >>> 16) & 0xff) / 255,
                g: ((blendColor >>> 8) & 0xff) / 255,
                b: (blendColor & 0xff) / 255,
                a: ((blendColor >>> 24) & 0xff) / 255,
            };

            // D3D9 stores both bias states as float bit patterns in DWORDs.
            // Its constant bias is in normalised depth units; depth24 WebGPU
            // expresses the same offset in one-ULP integer steps.
            const rawDepthBias = floatFromDWORD(get(D3DRS_DEPTHBIAS, 0));
            const scaledDepthBias = Number.isFinite(rawDepthBias)
                ? Math.round(rawDepthBias * 0x1000000) : 0;
            const depthBias = Math.max(-0x80000000,
                Math.min(0x7fffffff, scaledDepthBias));
            const rawSlopeBias = floatFromDWORD(
                get(D3DRS_SLOPESCALEDEPTHBIAS, 0));
            const depthBiasSlopeScale = Number.isFinite(rawSlopeBias)
                ? rawSlopeBias : 0;

            const stencilEnabled = hasDepth &&
                get(D3DRS_STENCILENABLE, 0) !== 0;
            const stencilFace = (failId, depthFailId, passId, funcId) => ({
                compare: COMPARE_FUNCS[get(funcId, 8)] || "always",
                failOp: STENCIL_OPS[get(failId, 1)] || "keep",
                depthFailOp: STENCIL_OPS[get(depthFailId, 1)] || "keep",
                passOp: STENCIL_OPS[get(passId, 1)] || "keep",
            });
            const stencilFront = stencilFace(D3DRS_STENCILFAIL,
                D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_STENCILFUNC);
            const stencilBack = get(D3DRS_TWOSIDEDSTENCILMODE, 0) !== 0
                ? stencilFace(D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL,
                    D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILFUNC)
                : stencilFront;

            // D3D9's front face is clockwise, so D3DCULL_CW means "cull the
            // front" and D3DCULL_CCW (its default) means "cull the back".
            const cullValue = get(D3DRS_CULLMODE, D3DCULL_CCW);
            let cullMode = "none";
            if (!this.debug.disableCull) {
                if (cullValue === D3DCULL_CW) cullMode = "front";
                else if (cullValue === D3DCULL_CCW) cullMode = "back";
            }

            // D3DCOLORWRITEENABLE's RED/GREEN/BLUE/ALPHA bits happen to be
            // 1/2/4/8, matching GPUColorWrite exactly.
            const writeMask = get(D3DRS_COLORWRITEENABLE, 0xF) & 0xF;
            const extraWriteMasks = [
                get(D3DRS_COLORWRITEENABLE1, 0xF) & 0xF,
                get(D3DRS_COLORWRITEENABLE2, 0xF) & 0xF,
                get(D3DRS_COLORWRITEENABLE3, 0xF) & 0xF,
            ];

            // Alpha test is a shader construct here, not pipeline state, so
            // it travels with the rest of the immutable state and lands in
            // the fragment key rather than in the GPURenderPipeline itself.
            const alphaTest = {
                enabled: get(D3DRS_ALPHATESTENABLE, 0) !== 0,
                func: get(D3DRS_ALPHAFUNC, 8) & 0xF,
                reference: get(D3DRS_ALPHAREF, 0) & 0xFF,
            };

            return { depthEnabled, depthWrite, depthCompare, depthBias,
                depthBiasSlopeScale, blendEnabled, srcFactor, dstFactor,
                blendOp, alphaSrcFactor, alphaDstFactor, alphaBlendOp,
                blendConstant, cullMode, writeMask, alphaTest, hasDepth,
                stencilEnabled, stencilFront, stencilBack,
                stencilReadMask: get(D3DRS_STENCILMASK, 0xffffffff) >>> 0,
                stencilWriteMask: get(D3DRS_STENCILWRITEMASK, 0xffffffff) >>> 0,
                stencilReference: get(D3DRS_STENCILREF, 0) >>> 0,
                extraWriteMasks,
                // Every colour attachment of the pass this pipeline runs in has
                // to appear in its fragment targets, in order -- WebGPU matches
                // them positionally, so an MRT pass needs the whole list baked
                // into the pipeline and therefore into its cache key.
                colorFormats: targets ? targets.formats : [this.format] };
        }

        // ---- independent sampler state (plan 4.4/12) ----
        //
        // D3D9 splits sampling parameters out of texture-stage state into
        // SetSamplerState, which maps almost one-to-one onto an immutable
        // GPUSampler. M1 recorded these values but sampled every texture with
        // one hardcoded linear/repeat sampler created alongside the texture;
        // they now drive a cache keyed by the parameter tuple, so a stage's
        // sampler follows the app's state rather than the texture it happens
        // to be bound to.
        samplerFor(state, stage) {
            const get = (id, fallback) => {
                const value = state.samplerStates.get(stage * 64 + id);
                return value === undefined ? fallback : value;
            };
            // D3D9 defaults: WRAP addressing, POINT min/mag, no mip filtering.
            const addressU = get(D3DSAMP_ADDRESSU, 1);
            const addressV = get(D3DSAMP_ADDRESSV, 1);
            const addressW = get(D3DSAMP_ADDRESSW, 1);
            const magFilter = get(D3DSAMP_MAGFILTER, 1);
            const minFilter = get(D3DSAMP_MINFILTER, 1);
            const mipFilter = get(D3DSAMP_MIPFILTER, 0);
            let maxAnisotropy = get(D3DSAMP_MAXANISOTROPY, 1) | 0;
            const key = [addressU, addressV, addressW, magFilter, minFilter,
                mipFilter, maxAnisotropy,
                this.debug.forceMipLevel0 ? "top" : ""].join(",");
            const cached = this.samplerCache.get(key);
            if (cached) { ++this.stats.samplerHits; return cached; }

            const descriptor = {
                addressModeU: ADDRESS_MODES[addressU] || "repeat",
                addressModeV: ADDRESS_MODES[addressV] || "repeat",
                addressModeW: ADDRESS_MODES[addressW] || "repeat",
                magFilter: FILTER_MODES[magFilter] || "nearest",
                minFilter: FILTER_MODES[minFilter] || "nearest",
                // D3DTEXF_NONE means "use only the top mip level", which is
                // what clamping the LOD range to 0 expresses in WebGPU.
                mipmapFilter: mipFilter === 2 ? "linear" : "nearest",
            };
            if (mipFilter === 0 || this.debug.forceMipLevel0) {
                descriptor.lodMinClamp = 0;
                descriptor.lodMaxClamp = 0;
            }
            // WebGPU only accepts maxAnisotropy > 1 when all three filters are
            // linear, so anisotropy is dropped rather than forcing filters the
            // app did not ask for.
            if (maxAnisotropy > 1 && descriptor.magFilter === "linear" &&
                    descriptor.minFilter === "linear" &&
                    descriptor.mipmapFilter === "linear" && mipFilter !== 0)
                descriptor.maxAnisotropy = Math.min(16, maxAnisotropy);
            if (addressU === 4 || addressV === 4 || addressW === 4)
                this.warnOnce("address-border", "D3DTADDRESS_BORDER has no " +
                    "native WebGPU sampler mode; the physical sample clamps " +
                    "to edge and the fixed-function fragment shader replaces " +
                    "out-of-domain coordinates with D3DSAMP_BORDERCOLOR");
            if (addressU === 5 || addressV === 5 || addressW === 5)
                this.warnOnce("address-mirroronce", "D3DTADDRESS_MIRRORONCE " +
                    "has no WebGPU equivalent; clamping to edge instead");
            const sampler = this.device.createSampler(descriptor);
            ++this.stats.samplersCreated;
            this.samplerCache.set(key, sampler);
            return sampler;
        }

        // ---- program resolution ----

        // Resolves the two stages into GPUShaderModules plus everything the
        // pipeline and bind group need. Returns {error} instead of throwing
        // for anything the caller should turn into a counted skipped draw.
        programFor(state, elements, pipelineState, drawOptions) {
            drawOptions = drawOptions || {};
            const alphaTest = pipelineState.alphaTest;
            const alphaTestKey = alphaTest.enabled
                ? "_a" + alphaTest.func + "_" + alphaTest.reference : "";
            const rs = state.renderStates;
            const vsHandle = state.vertexShaderHandle;
            const psHandle = state.pixelShaderHandle;
            const vsResource = vsHandle ? this.resources.get(vsHandle) : null;
            const psResource = psHandle ? this.resources.get(psHandle) : null;
            if (vsHandle && !vsResource)
                return { error: "bound vertex shader handle is unknown to the host",
                    shaderError: true };
            if (psHandle && !psResource)
                return { error: "bound pixel shader handle is unknown to the host",
                    shaderError: true };

            // The declaration decides whether the fixed-function vertex stage
            // can run at all, and it is also what tells the pixel stage which
            // vertex stage is in force -- D3D9 only applies texture-coordinate
            // generation and the texture matrices as part of fixed-function
            // T&L, so the answer changes what the cascade may assume about its
            // input varyings. Hence it is resolved first, before either module.
            const fixedVertexSignature = vsResource ? null
                : this.fixedFunctionVertexSignature(elements);
            if (!vsResource && !fixedVertexSignature)
                return { error: "declaration has no POSITION/POSITIONT element " +
                    "and no vertex shader is bound" };

            // Table fog (D3DRS_FOGTABLEMODE) takes precedence over vertex fog
            // when both are set, which is what D3D9 does. Screen-space XYZRHW
            // geometry is excluded: it has no eye-space depth to fog against.
            let fogMode = 0;
            if ((rs.get(D3DRS_FOGENABLE) || 0) !== 0 &&
                    (!fixedVertexSignature ||
                     fixedVertexSignature.positionType !== "screen")) {
                const table = rs.get(D3DRS_FOGTABLEMODE) || D3DFOG_NONE;
                fogMode = table !== D3DFOG_NONE ? table
                    : (rs.get(D3DRS_FOGVERTEXMODE) || D3DFOG_NONE);
            }

            // ---- pixel stage ----
            //
            // Resolved before the vertex stage because how many texture
            // coordinate sets the vertex stage has to produce, and with which
            // per-stage transform, depends on what consumes them: the cascade's
            // active stage count for a fixed-function pixel stage, or the
            // texcoord semantics a translated pixel shader declares.
            let fragmentModule, fragmentKey, pixelReflection = null;
            let samplerIndices = [];
            // Sampler slot -> WGSL view dimension ("2d" / "cube" / "3d").
            const samplerDimensions = {};
            let cascade = null;
            let pixelSignature = null;
            let coordStageCount = 0;
            if (psResource) {
                if (!psResource.translated.ok)
                    return { error: "pixel shader translation failed: " +
                        psResource.translated.error, shaderError: true };
                let variant = psResource.translated;
                if (alphaTest.enabled) {
                    variant = psResource.variants.get(alphaTestKey);
                    if (!variant) {
                        variant = shaderPipeline.compileShader(psResource.tokens, {
                            alphaTestDiscard: alphaTestDiscard(alphaTest,
                                "result.color0.a"),
                        });
                        psResource.variants.set(alphaTestKey, variant);
                        if (variant.ok) ++this.stats.shaderVariantsTranslated;
                    }
                    if (!variant.ok)
                        return { error: "pixel shader translation failed: " +
                            variant.error, shaderError: true };
                }
                pixelReflection = variant.reflection;
                // The declared sampler type has to match what is actually bound:
                // a texture_cube binding fed a 2D view is a WebGPU validation
                // error that kills the whole submit, not just this draw.
                for (const sampler of pixelReflection.samplers) {
                    const texture = this.resources.get(
                        state.textures.get(sampler.index));
                    const bound = texture ? (texture.textureType || "2d") : null;
                    if (bound && bound !== sampler.type)
                        return { error: "pixel shader declares sampler " +
                            sampler.index + " as " + sampler.type +
                            " but a " + bound + " texture is bound to that stage",
                            shaderError: true };
                }
                samplerIndices = pixelReflection.samplers.map(s => s.index);
                for (const sampler of pixelReflection.samplers)
                    samplerDimensions[sampler.index] = sampler.type;
                if (fogMode) {
                    ++this.stats.drawsWithUnappliedFog;
                    this.warnOnce("fog-programmable",
                        "fog is enabled on a draw with a translated pixel " +
                        "shader; the fixed-function fog blend is not applied " +
                        "there, so the fragment keeps its untinted colour");
                }
                // D3D9 hands a pixel shader the coordinates of texture stage n
                // in its texcoord n, so the vertex stage still has to run
                // generation/transform for every stage the shader reads.
                for (const input of pixelReflection.inputs || []) {
                    if (input.usage === DECLUSAGE_TEXCOORD)
                        coordStageCount = Math.max(coordStageCount,
                            input.usageIndex + 1);
                }
                if (!coordStageCount && pixelReflection.samplers.length)
                    coordStageCount = Math.max(...pixelReflection.samplers
                        .map(sampler => sampler.index)) + 1;
                fragmentKey = "ps" + psResource.hashHigh + "_" + psResource.hashLow +
                    alphaTestKey;
                fragmentModule = this.moduleFor(variant.wgsl, "d3d9 " + fragmentKey);
            } else {
                cascade = this.textureCascadeSignature(state,
                    { fixedVertexStage: !!fixedVertexSignature });
                coordStageCount = cascade.stages.length;
                for (const stage of cascade.stages) {
                    if (!stage.samplesTexture) continue;
                    if (stage.hasTextureBound) ++this.stats.drawsWithTexture;
                    else ++this.stats.drawsWithFallbackTexture;
                }
                if (cascade.unsupported.length) {
                    ++this.stats.drawsWithUnsupportedTextureOp;
                    this.warnOnce("texture-stage-" + cascade.unsupported[0],
                        "a draw asks for texture-stage behaviour outside what " +
                        "the fixed-function cascade implements; the stage falls " +
                        "back to selecting its first argument, which renders as " +
                        "plausible-but-wrong shading: " +
                        cascade.unsupported.join("; "));
                }
                samplerIndices = cascade.stages
                    .filter(stage => stage.samplesTexture)
                    .map(stage => stage.index);
                for (const stage of cascade.stages) {
                    if (stage.samplesTexture)
                        samplerDimensions[stage.index] = stage.textureType;
                }
                pixelSignature = {
                    stages: cascade.stages,
                    usesTextureFactor: cascade.usesTextureFactor,
                    fogMode, alphaTest,
                    // D3D9 adds the specular colour after the cascade whenever
                    // D3DRS_SPECULARENABLE is set, whether it came from lighting
                    // or straight off the vertex.
                    specularEnable: (rs.get(D3DRS_SPECULARENABLE) || 0) !== 0,
                };
                fragmentKey = "ffps" + cascade.stages.map(stage =>
                    [stage.index, stage.colorOp, stage.colorArg0, stage.colorArg1,
                     stage.colorArg2, stage.alphaOp, stage.alphaArg0,
                     stage.alphaArg1, stage.alphaArg2, stage.resultArg,
                     stage.samplesTexture ? stage.textureType : "-",
                     stage.coordVarying, stage.projected ? "p" + stage.transformCount : "",
                     stage.samplesTexture
                         ? [stage.addressU, stage.addressV, stage.addressW,
                            stage.borderColor >>> 0].join("b") : ""
                    ].join(".")).join("|") +
                    (pixelSignature.usesTextureFactor ? "_tf" : "") +
                    (pixelSignature.specularEnable ? "_s" : "") +
                    alphaTestKey + (fogMode ? "_f" + fogMode : "") +
                    (this.debug.shaderMode ? "_" + this.debug.shaderMode : "");
                fragmentModule = this.moduleFor(
                    buildFixedFunctionPixelShader(pixelSignature,
                        this.debug.shaderMode),
                    "d3d9 " + fragmentKey);
            }

            // ---- vertex stage ----
            let vertexModule, vertexKey, vertexReflection = null, locationFor;
            let fixedFunctionSignature = null;
            if (vsResource) {
                const inputLocations = new Map();
                if (!vsResource.translated.ok)
                    return { error: "vertex shader translation failed: " +
                        vsResource.translated.error, shaderError: true };
                for (const input of vsResource.translated.reflection.inputs)
                    inputLocations.set(input.usage * 16 + input.usageIndex, input.location);
                // Only the declaration knows which attributes are D3DCOLOR and
                // therefore arrive byte-swapped; see bgraInputLocations.
                const bgra = [];
                const inputConversions = {};
                for (const element of elements) {
                    const location = inputLocations.get(
                        element.usage * 16 + element.usageIndex);
                    if (location !== undefined && element.type === DECLTYPE_D3DCOLOR)
                        bgra.push(location);
                    if (location === undefined) continue;
                    const conversion = {
                        5: "ubyte4", 6: "short2", 7: "short4",
                        13: "udec3", 14: "dec3n",
                    }[element.type];
                    if (conversion) inputConversions[location] = conversion;
                }
                bgra.sort((a, b) => a - b);
                const conversionKey = Object.keys(inputConversions)
                    .map(Number).sort((a, b) => a - b)
                    .map(location => location + ":" + inputConversions[location])
                    .join(",");
                const pointKey = drawOptions.pointExpansion
                    ? "|p:" + (drawOptions.pointSprite ? "s" : "q") : "";
                const variantKey = "b:" + bgra.join(",") + "|c:" +
                    conversionKey + pointKey;
                const needsVariant = bgra.length || conversionKey.length ||
                    !!drawOptions.pointExpansion;
                let variant = vsResource.variants.get(variantKey);
                if (!variant) {
                    variant = needsVariant
                        ? shaderPipeline.compileShader(vsResource.tokens,
                            { bgraInputLocations: bgra, inputConversions,
                              pointExpansion: !!drawOptions.pointExpansion,
                              pointSprite: !!drawOptions.pointSprite })
                        : vsResource.translated;
                    vsResource.variants.set(variantKey, variant);
                    if (needsVariant && variant.ok)
                        ++this.stats.shaderVariantsTranslated;
                }
                if (!variant.ok)
                    return { error: "vertex shader translation failed: " + variant.error,
                        shaderError: true };
                vertexReflection = variant.reflection;
                vertexKey = "vs" + vsResource.hashHigh + "_" + vsResource.hashLow +
                    "_" + variantKey;
                vertexModule = this.moduleFor(variant.wgsl, "d3d9 " + vertexKey);
                locationFor = element => {
                    const location = inputLocations.get(
                        element.usage * 16 + element.usageIndex);
                    return location === undefined ? -1 : location;
                };
                if (conversionKey.length)
                    ++this.stats.drawsWithCompactVertexInputs;
            } else {
                const signature = fixedVertexSignature;
                signature.pointExpansion = !!drawOptions.pointExpansion;
                signature.pointSprite = signature.pointExpansion &&
                    (rs.get(D3DRS_POINTSPRITEENABLE) || 0) !== 0;
                signature.pointScale = signature.pointExpansion &&
                    signature.positionType !== "screen" &&
                    (rs.get(D3DRS_POINTSCALEENABLE) || 0) !== 0;
                signature.fogMode = fogMode;
                signature.fogRange = (rs.get(D3DRS_RANGEFOGENABLE) || 0) !== 0;
                signature.normalizeNormals =
                    (rs.get(D3DRS_NORMALIZENORMALS) || 0) !== 0;
                signature.lighting = this.lightingSignature(state, signature);
                signature.coordStages = this.coordStagePlan(state, coordStageCount);
                signature.clipPlaneCount = 0;
                // View space is needed for lighting, for the camera-space
                // coordinate generation modes and for range-based fog.
                signature.needsViewSpace = signature.positionType !== "screen" && (
                    !!signature.lighting ||
                    signature.pointScale ||
                    (signature.fogRange && !!fogMode) ||
                    signature.coordStages.some(stage =>
                        stage.tciMode !== D3DTSS_TCI_PASSTHRU));
                vertexKey = "ffvs_" + signature.positionType +
                    (signature.hasColor ? (signature.colorIsBGRA ? "_cb" : "_c") : "") +
                    (signature.hasColor1 ? (signature.color1IsBGRA ? "_sb" : "_s") : "") +
                    (signature.hasNormal ? "_n" : "") +
                    (signature.hasPointSize ? "_ps" : "") +
                    "_t" + signature.texCoordSets.join(".") +
                    "_x" + signature.coordStages.map(stage =>
                        [stage.index, stage.texCoordIndex, stage.tciMode,
                         stage.transformCount].join(".")).join(",") +
                    (signature.needsViewSpace ? "_v" : "") +
                    (signature.normalizeNormals ? "_nn" : "") +
                    (signature.lighting ? "_l" + signature.lighting.lights
                        .map(light => light.type).join(".") +
                        (signature.lighting.colorVertex ? "cv" : "") +
                        (signature.lighting.specularEnable ? "sp" : "") +
                        (signature.lighting.localViewer ? "lv" : "") +
                        "m" + [signature.lighting.diffuseSource,
                            signature.lighting.ambientSource,
                            signature.lighting.specularSource,
                            signature.lighting.emissiveSource].join("") : "") +
                    (fogMode ? "_f" + fogMode + (signature.fogRange ? "r" : "") : "") +
                    (signature.pointExpansion ? "_point" +
                        (signature.pointSprite ? "s" : "") +
                        (signature.pointScale ? "a" : "") : "");
                vertexModule = this.moduleFor(
                    buildFixedFunctionVertexShader(signature), "d3d9 " + vertexKey);
                vertexReflection = null;
                locationFor = fixedFunctionLocationFor;
                fixedFunctionSignature = signature;
            }
            if (vertexModule._d9wgBroken || fragmentModule._d9wgBroken)
                return { error: "a stage failed WGSL compilation (see the " +
                    "getCompilationInfo error logged at module creation)",
                    shaderError: true };

            const vertexBuffers = this.vertexBufferLayoutsFor(elements, state,
                locationFor, !!drawOptions.pointExpansion);
            if (!vertexBuffers.length)
                return { error: "no vertex stream supplies any attribute the " +
                    "vertex stage reads" };
            const pixelUniformLayout = pixelSignature
                ? fixedPixelUniformLayout(pixelSignature) : null;
            return { vertexModule, fragmentModule, vertexKey, fragmentKey,
                vertexReflection, pixelReflection, samplerIndices,
                samplerDimensions, vertexBuffers,
                fixedFunctionSignature, pixelSignature, pixelUniformLayout,
                vertexUniformLayout: fixedFunctionSignature
                    ? fixedVertexUniformLayout(fixedFunctionSignature) : null,
                // A translated pixel shader brings its own register file; the
                // fixed-function cascade's block is only as large as the fog
                // colour, texture factor and stage constants it actually reads.
                pixelUniformBytes: pixelReflection ? pixelReflection.uniformBytes
                    : (pixelUniformLayout && pixelUniformLayout.entries.length
                        ? pixelUniformLayout.byteLength : 0),
                fogMode, pointExpansion: !!drawOptions.pointExpansion,
                pointSprite: !!drawOptions.pointSprite };
        }

        // The per-stage coordinate plan the fixed-function vertex stage needs:
        // which coordinate set (or generated vector) feeds a stage and which
        // matrix transforms it. D3D9 runs this half of fixed-function T&L
        // whether or not a pixel shader is bound, which is why it is not folded
        // into textureCascadeSignature().
        coordStagePlan(state, stageCount) {
            const stageState = (stage, id, fallback) => {
                const value = state.textureStageStates.get(stage * 64 + id);
                return value === undefined ? fallback : value;
            };
            const stages = [];
            for (let index = 0; index < Math.min(stageCount, MAX_TEXTURE_STAGES);
                    ++index) {
                const flags = stageState(index, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
                // D3DTSS_TEXCOORDINDEX defaults to the stage's own number, so an
                // app that never sets it gets stage n reading TEXCOORD n.
                const coordIndex = stageState(index, D3DTSS_TEXCOORDINDEX, index);
                stages.push({ index,
                    texCoordIndex: coordIndex & 0xFFFF,
                    tciMode: coordIndex & D3DTSS_TCI_MASK,
                    transformCount: flags & 0xFF,
                    projected: (flags & D3DTTFF_PROJECTED) !== 0 });
            }
            return stages;
        }

        pipelineFor(program, pipelineState, topology, stripIndexFormat) {
            const key = program.vertexKey + "|" + program.fragmentKey + "|" +
                JSON.stringify(program.vertexBuffers) + "|" +
                topology + "|" + (stripIndexFormat || "") + "|" +
                JSON.stringify(pipelineState) + "|" +
                program.samplerIndices.map(index => index + ":" +
                    ((program.samplerDimensions &&
                      program.samplerDimensions[index]) || "2d")).join(",");
            let pipeline = this.pipelineCache.get(key);
            if (pipeline) { ++this.stats.pipelineHits; return pipeline; }

            // Binding 0 is the vertex stage's constant buffer (or the
            // fixed-function transform block, which occupies the same slot),
            // binding 1 the pixel stage's; samplers take 2+2n / 3+2n, the
            // numbering d3d9_shader_pipeline.js emits.
            const bindGroupEntries = [
                { binding: 0, visibility: SHADER_STAGE_VERTEX,
                  buffer: { type: "uniform", hasDynamicOffset: true } },
            ];
            if (program.pixelUniformBytes)
                bindGroupEntries.push({ binding: 1, visibility: SHADER_STAGE_FRAGMENT,
                    buffer: { type: "uniform", hasDynamicOffset: true } });
            for (const index of program.samplerIndices) {
                // The view dimension has to be declared here, not left to
                // default to "2d": WebGPU checks the layout against what the
                // shader declares, so a texture_cube<f32> binding paired with a
                // default 2D layout entry fails pipeline creation outright. naga
                // cannot catch that -- it validates one module, not the pairing.
                const dimension = (program.samplerDimensions &&
                    program.samplerDimensions[index]) || "2d";
                bindGroupEntries.push(
                    { binding: 2 + index * 2, visibility: SHADER_STAGE_FRAGMENT,
                      texture: { viewDimension: dimension } },
                    { binding: 3 + index * 2, visibility: SHADER_STAGE_FRAGMENT,
                      sampler: {} });
            }
            const bindGroupLayout = this.device.createBindGroupLayout(
                { entries: bindGroupEntries });

            // D3DRS_COLORWRITEENABLE1/2/3 mask the extra MRT attachments; slot
            // 0 uses D3DRS_COLORWRITEENABLE. An attachment the fragment shader
            // does not write still needs an entry, or WebGPU rejects the
            // pipeline against the pass.
            const colorTargets = (pipelineState.colorFormats || [this.format])
                .map((format, index) => {
                    const target = { format, writeMask: index === 0
                        ? pipelineState.writeMask
                        : (pipelineState.extraWriteMasks
                            ? pipelineState.extraWriteMasks[index - 1] : 0xF) };
                    if (pipelineState.blendEnabled) {
                        target.blend = {
                            color: { srcFactor: pipelineState.srcFactor,
                                     dstFactor: pipelineState.dstFactor,
                                     operation: pipelineState.blendOp },
                            alpha: { srcFactor: pipelineState.alphaSrcFactor,
                                     dstFactor: pipelineState.alphaDstFactor,
                                     operation: pipelineState.alphaBlendOp },
                        };
                    }
                    return target;
                });
            const primitive = { topology,
                cullMode: program.pointExpansion ? "none" : pipelineState.cullMode,
                frontFace: "cw" };
            // WebGPU needs to know the restart-index width up front for an
            // indexed strip draw; it must be absent for every other topology.
            if (stripIndexFormat) primitive.stripIndexFormat = stripIndexFormat;
            const descriptor = {
                layout: this.device.createPipelineLayout(
                    { bindGroupLayouts: [bindGroupLayout] }),
                vertex: {
                    module: program.vertexModule, entryPoint: "d9_vs_main",
                    buffers: program.vertexBuffers.map(layout =>
                        ({ arrayStride: layout.arrayStride,
                           stepMode: layout.stepMode || "vertex",
                           attributes: layout.attributes })),
                },
                fragment: { module: program.fragmentModule, entryPoint: "d9_ps_main",
                    targets: colorTargets },
                primitive,
            };
            // The pipeline must declare a depthStencil state whenever the
            // pass it runs in has a depth attachment, even for a draw that
            // does no depth testing -- hence depthCompare "always" plus
            // depthWriteEnabled false rather than omitting the block.
            if (pipelineState.hasDepth) {
                descriptor.depthStencil = {
                    format: DEPTH_FORMAT,
                    depthWriteEnabled: pipelineState.depthEnabled
                        ? pipelineState.depthWrite : false,
                    depthCompare: pipelineState.depthEnabled
                        ? pipelineState.depthCompare : "always",
                    depthBias: pipelineState.depthBias,
                    depthBiasSlopeScale: pipelineState.depthBiasSlopeScale,
                    stencilFront: pipelineState.stencilEnabled
                        ? pipelineState.stencilFront : {},
                    stencilBack: pipelineState.stencilEnabled
                        ? pipelineState.stencilBack : {},
                    stencilReadMask: pipelineState.stencilEnabled
                        ? pipelineState.stencilReadMask : 0,
                    stencilWriteMask: pipelineState.stencilEnabled
                        ? pipelineState.stencilWriteMask : 0,
                };
            }
            pipeline = this.device.createRenderPipeline(descriptor);
            pipeline._bindGroupLayout = bindGroupLayout;
            pipeline._d9wgId = this.objectId(pipeline);
            ++this.stats.pipelineCreations;
            this.pipelineCache.set(key, pipeline);
            return pipeline;
        }

        objectId(value) {
            if ((typeof value !== "object" && typeof value !== "function") ||
                    value === null)
                return String(value);
            let id = this.objectIds.get(value);
            if (!id) {
                id = this.nextObjectId++;
                this.objectIds.set(value, id);
            }
            return id;
        }

        allocateUniformSlot(byteCount) {
            const size = alignUp(byteCount, UNIFORM_OFFSET_ALIGNMENT);
            const offset = alignUp(this.uniformRingCursor,
                UNIFORM_OFFSET_ALIGNMENT);
            if (this.uniformRing && size <= this.uniformRingCapacity - offset) {
                this.uniformRingCursor = offset + size;
                return { buffer: this.uniformRing, offset, transient: false };
            }
            const buffer = this.device.createBuffer({
                label: "D3D9 uniform ring overflow",
                size,
                usage: BUFFER_USAGE_UNIFORM | BUFFER_USAGE_COPY_DST,
            });
            ++this.stats.uniformRingOverflows;
            this.retireAfterSubmit(buffer);
            return { buffer, offset: 0, transient: true };
        }

        uniformBytesHash(bytes) {
            let hash = 0x811c9dc5;
            for (let index = 0; index < bytes.length; ++index) {
                hash ^= bytes[index];
                hash = Math.imul(hash, 0x01000193) >>> 0;
            }
            return hash >>> 0;
        }

        // Both stages share one persistent uniform ring. Each binding uses the
        // same dynamic base offset, while binding 1's static pixelOffset keeps
        // the two register files separate. Identical blocks inside one frame
        // reuse a slot, which is common for particle/UI runs whose only state
        // changes are vertex data and blend ordering.
        constantBufferFor(state, program) {
            const vertexBytes = program.vertexReflection
                ? program.vertexReflection.uniformBytes
                : program.vertexUniformLayout.byteLength;
            const pixelBytes = program.pixelUniformBytes || 0;
            const pixelOffset = pixelBytes
                ? alignUp(vertexBytes, UNIFORM_OFFSET_ALIGNMENT) : 0;
            const total = Math.max(16, vertexBytes, pixelOffset + pixelBytes);
            const backing = new ArrayBuffer(alignUp(total, 4));

            if (program.vertexReflection) {
                this.writeConstantRegisters(backing, 0, program.vertexReflection,
                    state.vsConstF, state.vsConstI, state.vsConstB, state);
            } else {
                this.writeFixedVertexUniforms(state, program, backing, 0);
            }
            if (program.pixelReflection) {
                this.writeConstantRegisters(backing, pixelOffset,
                    program.pixelReflection, state.psConstF, state.psConstI,
                    state.psConstB);
            } else if (pixelBytes) {
                this.writeFixedPixelUniforms(state, program, backing, pixelOffset);
            }

            const bytes = new Uint8Array(backing);
            const frame = this.ensureFrame();
            const hash = this.uniformBytesHash(bytes);
            const key = vertexBytes + ":" + pixelOffset + ":" + pixelBytes +
                ":" + hash.toString(16);
            const candidates = frame.uniformSlots.get(key) || [];
            for (const candidate of candidates) {
                if (candidate.bytes.length !== bytes.length) continue;
                let equal = true;
                for (let index = 0; index < bytes.length; ++index) {
                    if (candidate.bytes[index] !== bytes[index]) {
                        equal = false;
                        break;
                    }
                }
                if (!equal) continue;
                ++this.stats.uniformSlotReuses;
                return { buffer: candidate.buffer, dynamicOffset: candidate.offset,
                    vertexBytes, pixelOffset, pixelBytes,
                    transient: candidate.transient };
            }

            const slot = this.allocateUniformSlot(backing.byteLength);
            this.device.queue.writeBuffer(slot.buffer, slot.offset, backing);
            this.stats.constantUploadBytes += backing.byteLength;
            candidates.push({ buffer: slot.buffer, offset: slot.offset,
                transient: slot.transient, bytes: bytes.slice() });
            frame.uniformSlots.set(key, candidates);
            return { buffer: slot.buffer, dynamicOffset: slot.offset,
                vertexBytes, pixelOffset, pixelBytes,
                transient: slot.transient };
        }

        // Fills the fixed-function vertex block. The field list comes from
        // fixedVertexUniformLayout(), the same call the WGSL struct was
        // generated from, so a field that exists in one exists in the other.
        //
        // D3D stores matrices row-major for row-vector maths (v * M); WGSL reads
        // a uniform mat4x4 column-major and applies M * v. Those two conventions
        // cancel: the *same bytes* that describe M to D3D describe M-transpose
        // to WGSL, and M-transpose is exactly the column-vector form of D3D's
        // row-vector M. So every matrix here is uploaded unchanged.
        writeFixedVertexUniforms(state, program, backing, byteOffset) {
            const signature = program.fixedFunctionSignature;
            const layout = program.vertexUniformLayout;
            const rs = state.renderStates;
            const floats = new Float32Array(backing, byteOffset,
                layout.byteLength / 4);
            const at = name => {
                const entry = layout.byName.get(name);
                return entry ? entry.offset / 4 : -1;
            };
            const screenSpace = signature.positionType === "screen";
            const worldView = multiply4x4(
                state.transforms.get(D3DTS_WORLD) || IDENTITY4x4,
                state.transforms.get(D3DTS_VIEW) || IDENTITY4x4);

            floats.set(screenSpace ? IDENTITY4x4 : this.wvp(state),
                at("world_view_projection"));
            const viewport = at("viewport");
            floats[viewport] = state.viewport.width || 1;
            floats[viewport + 1] = state.viewport.height || 1;
            // zw carry the viewport's origin, which the XYZRHW path subtracts:
            // D3D9 pre-transformed coordinates are absolute render-target
            // pixels, not viewport-relative ones.
            floats[viewport + 2] = state.viewport.x || 0;
            floats[viewport + 3] = state.viewport.y || 0;

            if (layout.byName.has("world_view")) {
                floats.set(worldView, at("world_view"));
                floats.set(inverseTranspose3x3(worldView), at("normal_matrix"));
            }
            if (layout.byName.has("fog_params")) {
                // FOGSTART/FOGEND/FOGDENSITY are floats carried inside a DWORD.
                const asFloat = (id, fallback) => {
                    const raw = rs.get(id);
                    if (raw === undefined) return fallback;
                    FLOAT_BITS_U32[0] = raw >>> 0;
                    return FLOAT_BITS_F32[0];
                };
                const base = at("fog_params");
                floats[base] = asFloat(D3DRS_FOGSTART, 0);
                floats[base + 1] = asFloat(D3DRS_FOGEND, 1);
                floats[base + 2] = asFloat(D3DRS_FOGDENSITY, 1);
            }
            for (const stage of signature.coordStages) {
                if (!stage.transformCount) continue;
                floats.set(state.transforms.get(D3DTS_TEXTURE0 + stage.index) ||
                    IDENTITY4x4, at("texture_transform" + stage.index));
            }
            if (signature.pointExpansion) {
                const pointFloat = (id, fallback) => {
                    const raw = rs.get(id);
                    return raw === undefined ? fallback : floatFromDWORD(raw);
                };
                const pointViewport = at("point_viewport");
                const width = Math.max(1, state.viewport.width || 1);
                const height = Math.max(1, state.viewport.height || 1);
                floats[pointViewport] = width;
                floats[pointViewport + 1] = height;
                floats[pointViewport + 2] = 1 / width;
                floats[pointViewport + 3] = 1 / height;
                const pointParams = at("point_params");
                floats[pointParams] = pointFloat(D3DRS_POINTSIZE, 1);
                floats[pointParams + 1] = Math.max(0,
                    pointFloat(D3DRS_POINTSIZE_MIN, 1));
                floats[pointParams + 2] = Math.max(floats[pointParams + 1],
                    pointFloat(D3DRS_POINTSIZE_MAX, 64));
                if (signature.pointScale) {
                    const pointScale = at("point_scale");
                    floats[pointScale] = pointFloat(D3DRS_POINTSCALE_A, 1);
                    floats[pointScale + 1] = pointFloat(D3DRS_POINTSCALE_B, 0);
                    floats[pointScale + 2] = pointFloat(D3DRS_POINTSCALE_C, 0);
                }
            }
            if (!signature.lighting) return;

            // D3D9's default material is not all zeroes: a device that never
            // calls SetMaterial still lights with white diffuse/ambient. Using
            // zeroes here would make "the app forgot SetMaterial" and "the app
            // asked for black" indistinguishable, and the former renders black.
            const material = state.material || DEFAULT_MATERIAL;
            floats.set(material.diffuse, at("material_diffuse"));
            floats.set(material.ambient, at("material_ambient"));
            floats.set(material.specular, at("material_specular"));
            floats.set(material.emissive, at("material_emissive"));
            const ambientPower = at("ambient_power");
            const ambient = rs.get(D3DRS_AMBIENT) || 0;
            floats[ambientPower] = ((ambient >>> 16) & 0xff) / 255;
            floats[ambientPower + 1] = ((ambient >>> 8) & 0xff) / 255;
            floats[ambientPower + 2] = (ambient & 0xff) / 255;
            floats[ambientPower + 3] = material.power;

            if (!signature.lighting.lights.length) return;
            // Lights arrive in world space and are lit in view space (which is
            // where D3D9's fixed pipeline puts them), so the view transform is
            // applied here rather than carried into the shader as a second
            // matrix: eight lights is at most a few dozen multiplies per draw,
            // against a full mat4 the vertex stage would otherwise re-apply per
            // vertex.
            const view = state.transforms.get(D3DTS_VIEW) || IDENTITY4x4;
            let base = at("lights");
            for (const entry of signature.lighting.lights) {
                const light = state.lights.get(entry.index);
                floats.set(light.diffuse, base);
                floats.set(light.specular, base + 4);
                floats.set(light.ambient, base + 8);
                const position = transformPoint(view, light.position);
                floats[base + 12] = position[0];
                floats[base + 13] = position[1];
                floats[base + 14] = position[2];
                floats[base + 15] = 1;
                const direction = normalize3(transformDirection(view, light.direction));
                floats[base + 16] = direction[0];
                floats[base + 17] = direction[1];
                floats[base + 18] = direction[2];
                floats[base + 19] = 0;
                floats[base + 20] = light.range;
                floats[base + 21] = light.falloff;
                // D3D9's Theta/Phi are the *full* cone angles, so the cosine
                // compared against rho is of the half angle.
                floats[base + 22] = Math.cos(light.theta / 2);
                floats[base + 23] = Math.cos(light.phi / 2);
                floats[base + 24] = light.attenuation[0];
                floats[base + 25] = light.attenuation[1];
                floats[base + 26] = light.attenuation[2];
                floats[base + 27] = light.type;
                base += 28;
            }
        }

        // Fills the fixed-function pixel block: only the fog colour, texture
        // factor and per-stage constants the cascade actually reads.
        writeFixedPixelUniforms(state, program, backing, byteOffset) {
            const layout = program.pixelUniformLayout;
            const floats = new Float32Array(backing, byteOffset,
                layout.byteLength / 4);
            const writeColor = (index, argb, alpha) => {
                floats[index] = ((argb >>> 16) & 0xff) / 255;
                floats[index + 1] = ((argb >>> 8) & 0xff) / 255;
                floats[index + 2] = (argb & 0xff) / 255;
                floats[index + 3] = alpha === undefined
                    ? ((argb >>> 24) & 0xff) / 255 : alpha;
            };
            for (const entry of layout.entries) {
                if (entry.name === "fog_color") {
                    // D3DRS_FOGCOLOR's alpha byte is defined as unused.
                    writeColor(entry.offset / 4,
                        state.renderStates.get(D3DRS_FOGCOLOR) || 0, 1);
                } else if (entry.name === "texture_factor") {
                    writeColor(entry.offset / 4,
                        state.renderStates.get(D3DRS_TEXTUREFACTOR) === undefined
                            ? 0xffffffff
                            : state.renderStates.get(D3DRS_TEXTUREFACTOR));
                } else {
                    // stage_constant<N>; D3DTSS_CONSTANT defaults to opaque
                    // black, unlike D3DRS_TEXTUREFACTOR's opaque white.
                    writeColor(entry.offset / 4,
                        state.textureStageStates.get(
                            entry.source * 64 + D3DTSS_CONSTANT) || 0xff000000);
                }
            }
        }

        // Packs one stage's register file into the layout the translated WGSL
        // declares (plan 9.7): float4 c# registers, then int4 i#, then one
        // 32-bit slot per bool b#. `def`/`defi`/`defb` literals are written
        // last because a shader's own constant definitions take effect while
        // it is bound, over whatever the app last set for that register.
        writeConstantRegisters(backing, byteOffset, reflection, constF, constI,
                constB, state) {
            const floats = new Float32Array(backing, byteOffset,
                reflection.floatConstCount * 4);
            floats.set(constF.subarray(0, Math.min(constF.length, floats.length)));
            for (const item of reflection.floatDefaults) {
                if ((item.register + 1) * 4 > floats.length) continue;
                floats.set(item.values, item.register * 4);
            }
            const intOffset = byteOffset + reflection.floatRegionBytes;
            const ints = new Int32Array(backing, intOffset, reflection.intConstCount * 4);
            ints.set(constI.subarray(0, Math.min(constI.length, ints.length)));
            for (const item of reflection.intDefaults) {
                if ((item.register + 1) * 4 > ints.length) continue;
                ints.set(item.values, item.register * 4);
            }
            const boolOffset = intOffset + reflection.intRegionBytes;
            const bools = new Uint32Array(backing, boolOffset,
                reflection.boolVectorCount * 4);
            bools.set(constB.subarray(0, Math.min(constB.length, bools.length)));
            for (const item of reflection.boolDefaults) {
                if (item.register >= bools.length) continue;
                bools[item.register] = item.value ? 1 : 0;
            }
            // Every translated vertex shader reads this for the D3D9 half-pixel
            // offset (see the o_position fixup in d3d9_shader_pipeline.js). The
            // viewport is what maps clip space to pixels, so it -- not the
            // render target extent -- is what a half pixel is measured against.
            if (reflection.viewportOffset >= 0 && state) {
                const values = new Float32Array(backing, byteOffset);
                const slot = reflection.viewportOffset / 4;
                values[slot] = Math.max(1, state.viewport.width || 1);
                values[slot + 1] = Math.max(1, state.viewport.height || 1);
            }
            if (reflection.pointExpansion && state) {
                const values = new Float32Array(backing, byteOffset);
                const viewport = reflection.pointViewportOffset / 4;
                const width = Math.max(1, state.viewport.width || 1);
                const height = Math.max(1, state.viewport.height || 1);
                values[viewport] = width;
                values[viewport + 1] = height;
                values[viewport + 2] = 1 / width;
                values[viewport + 3] = 1 / height;
                const point = reflection.pointParamsOffset / 4;
                const pointFloat = (id, fallback) => {
                    const raw = state.renderStates.get(id);
                    return raw === undefined ? fallback : floatFromDWORD(raw);
                };
                values[point] = pointFloat(D3DRS_POINTSIZE, 1);
                values[point + 1] = Math.max(0,
                    pointFloat(D3DRS_POINTSIZE_MIN, 1));
                values[point + 2] = Math.max(values[point + 1],
                    pointFloat(D3DRS_POINTSIZE_MAX, 64));
            }
        }

        // The view a stage samples through. D3DSAMP_SRGBTEXTURE asks D3D9 to
        // decode the texture from sRGB to linear on read; ignoring it hands the
        // shader values that are substantially *brighter* than the app intends
        // (sRGB 0.5 is linear 0.21), which on anything additive -- an
        // environment-mapped reflection above all -- reads as blown-out white
        // rather than as a subtle gamma difference.
        sampledViewFor(texture, wantsSRGB) {
            if (!wantsSRGB) return texture.view;
            if (!texture.srgbFormat) {
                ++this.stats.srgbTextureUnavailable;
                this.warnOnce("srgb-no-sibling",
                    "a stage asks for sRGB decoding on a texture whose format " +
                    "has no -srgb equivalent in WebGPU, so it is sampled as " +
                    "linear and comes out too bright", { format: texture.format,
                        gpuFormat: texture.gpuFormat });
                return texture.view;
            }
            if (!texture.srgbView) {
                texture.srgbView = texture.gpuTexture.createView({
                    format: texture.srgbFormat,
                    dimension: texture.textureType === "cube" ? "cube" : "2d",
                });
                ++this.stats.srgbViewsCreated;
            }
            ++this.stats.srgbTextureSamples;
            return texture.srgbView;
        }

        // A 1x1 opaque white stand-in per view dimension, for a stage the
        // shader samples but the app left unbound.
        fallbackViewFor(dimension) {
            if (dimension === "2d" || !dimension) return this.fallbackView;
            if (!this.fallbackViews) this.fallbackViews = new Map();
            let view = this.fallbackViews.get(dimension);
            if (view) return view;
            const layers = dimension === "cube" ? 6 : 1;
            const texture = this.device.createTexture({
                label: "D3D9 fallback white " + dimension,
                size: { width: 1, height: 1, depthOrArrayLayers: layers },
                format: "rgba8unorm",
                dimension: dimension === "3d" ? "3d" : "2d",
                usage: TEXTURE_USAGE_COPY_DST | TEXTURE_USAGE_TEXTURE_BINDING,
            });
            for (let layer = 0; layer < layers; ++layer) {
                this.device.queue.writeTexture(
                    { texture, origin: { x: 0, y: 0, z: layer } },
                    new Uint8Array([255, 255, 255, 255]),
                    { bytesPerRow: 4, rowsPerImage: 1 },
                    { width: 1, height: 1, depthOrArrayLayers: 1 });
            }
            view = texture.createView({ dimension });
            this.fallbackViews.set(dimension, view);
            return view;
        }

        retireAfterSubmit(buffer) {
            const frame = this.ensureFrame();
            (frame.transientBuffers || (frame.transientBuffers = [])).push(buffer);
        }

        bindGroupFor(state, pipeline, program, constants) {
            const entries = [{ binding: 0, resource: { buffer: constants.buffer,
                offset: 0, size: Math.max(16, constants.vertexBytes) } }];
            const identity = [pipeline._d9wgId || this.objectId(pipeline),
                this.objectId(constants.buffer), constants.vertexBytes,
                constants.pixelOffset, constants.pixelBytes];
            if (program.pixelUniformBytes)
                entries.push({ binding: 1, resource: { buffer: constants.buffer,
                    offset: constants.pixelOffset,
                    size: Math.max(16, constants.pixelBytes) } });
            for (const index of program.samplerIndices) {
                const texture = this.resources.get(state.textures.get(index));
                if (texture && this.frame) texture.frameReferenced = this.frame.serial;
                const expectedUploads = texture
                    ? texture.levelCount * (texture.layerCount || 1) : 0;
                if (texture && texture.uploadedLevels &&
                        texture.uploadedLevels.size < expectedUploads) {
                    ++this.stats.drawsWithIncompleteMipChain;
                    this.warnOnce("incomplete-mips",
                        "a bound texture declares more mip levels than were " +
                        "ever uploaded; the missing levels contain undefined " +
                        "data, so sampling them shows the wrong image " +
                        "entirely. Try v86gl.d3d9Executor.debug.forceMipLevel0" +
                        " = true to confirm.", {
                            format: texture.format,
                            size: texture.width + "x" + texture.height,
                            declaredLevels: texture.levelCount,
                            layers: texture.layerCount || 1,
                            uploaded: [...texture.uploadedLevels].sort(
                                (a, b) => a - b).map(key =>
                                    "level " + Math.floor(key / 6) +
                                    " layer " + (key % 6)),
                        });
                }
                // A shader can sample a stage the app left unbound; a 1x1 white
                // texture keeps the draw legal and visually neutral rather than
                // dropping it. It has to match the dimension the layout
                // declares, so there is one per dimension.
                const dimension = (program.samplerDimensions &&
                    program.samplerDimensions[index]) || "2d";
                const wantsSRGB = texture &&
                    (state.samplerStates.get(index * 64 + D3DSAMP_SRGBTEXTURE)
                        || 0) !== 0;
                const view = texture
                    ? this.sampledViewFor(texture, wantsSRGB)
                    : this.fallbackViewFor(dimension);
                const sampler = this.samplerFor(state, index);
                entries.push(
                    { binding: 2 + index * 2,
                      resource: view },
                    { binding: 3 + index * 2,
                      resource: sampler });
                identity.push(index, this.objectId(view), this.objectId(sampler));
            }
            const dynamicOffsets = program.pixelUniformBytes
                ? [constants.dynamicOffset, constants.dynamicOffset]
                : [constants.dynamicOffset];
            // Overflow buffers are destroyed after this submit, so a group
            // that captures one must not outlive the frame. The persistent
            // ring is the steady-state path and is safe to cache.
            const cacheable = !constants.transient;
            const key = identity.join(":");
            if (cacheable) {
                const cached = this.bindGroupCache.get(key);
                if (cached) {
                    this.bindGroupCache.delete(key);
                    this.bindGroupCache.set(key, cached);
                    ++this.stats.bindGroupHits;
                    return { group: cached, dynamicOffsets };
                }
            }
            const group = this.device.createBindGroup(
                { layout: pipeline._bindGroupLayout, entries });
            ++this.stats.bindGroupCreations;
            if (cacheable) {
                while (this.bindGroupCache.size >= this.maxBindGroups) {
                    const oldest = this.bindGroupCache.keys().next();
                    if (oldest.done) break;
                    this.bindGroupCache.delete(oldest.value);
                    ++this.stats.bindGroupCacheEvictions;
                }
                this.bindGroupCache.set(key, group);
            }
            return { group, dynamicOffsets };
        }

        // Builds the pipeline/uniform buffer/bind group eagerly (none of
        // those are tied to the swapchain's current texture, so there is no
        // staleness concern in creating them now) but only *records* the
        // draw as a pending op -- see the comment on ensureFrame() for why
        // the actual pass.draw()/drawIndexed() call must wait until
        // finishFrame() replays it against a freshly-acquired texture.
        //
        // The stride each stream contributes is the one the application bound
        // via SetStreamSource (or that a Draw*UP command carried). It must
        // never be inferred from the vertex declaration: a declaration's
        // consumed elements are only part of the vertex, so a computed stride
        // is too small whenever the format carries anything the shader skips
        // (NORMAL, extra texcoords, padding) and every vertex after the first
        // would then be fetched from the wrong offset.
        recordDraw(state, elements, which, geometry) {
            const targets = this.renderTargetsFor(state);
            if (!targets) {
                this.noteDroppedDraw(which, state,
                    ["no usable colour render target is bound"]);
                return;
            }
            const pipelineState = this.pipelineStateFor(state, targets);
            const pointExpansion = geometry.topology === "point-list" &&
                !geometry.indexInfo;
            const pointSprite = pointExpansion &&
                (state.renderStates.get(D3DRS_POINTSPRITEENABLE) || 0) !== 0;
            const program = this.programFor(state, elements, pipelineState,
                { pointExpansion, pointSprite });
            if (program.error) {
                if (program.shaderError) ++this.stats.drawsSkippedForBadShader;
                this.noteDroppedDraw(which, state, [program.error]);
                return;
            }
            const pipeline = this.pipelineFor(program, pipelineState,
                pointExpansion ? "triangle-list" : geometry.topology,
                pointExpansion ? undefined : geometry.stripIndexFormat);
            // The bind group captures concrete GPUTextureView objects. Create
            // the frame first so bindGroupFor can mark those textures as read;
            // a later UPDATE_TEXTURE can then rename instead of retroactively
            // changing this already-recorded draw. A draw without a preceding
            // Clear is legal and used by War3's UI passes, so relying on Clear
            // to create the frame misses exactly those textures.
            const frame = this.ensureFrame();
            const constants = this.constantBufferFor(state, program);
            const binding = this.bindGroupFor(state, pipeline, program, constants);
            // Bind each stream the pipeline declared a layout for, in the same
            // order, so slot N in the pipeline is slot N here.
            const vertexBuffers = [];
            for (const layout of program.vertexBuffers) {
                const binding = geometry.streams.get(layout.stream);
                if (!binding) {
                    this.noteDroppedDraw(which, state,
                        ["stream " + layout.stream + " is referenced by the " +
                         "declaration but not bound"]);
                    return;
                }
                vertexBuffers.push({ buffer: binding.buffer, offset: binding.offset });
            }
            // Mark every buffer this draw reads as "observed at this frame's
            // contents". applyBufferUpdate() uses that to notice a write that
            // would retroactively change what an already-recorded draw sees.
            for (const layout of program.vertexBuffers) {
                const binding = geometry.streams.get(layout.stream);
                if (binding && binding.resource)
                    binding.resource.frameReferenced = frame.serial;
            }
            if (geometry.indexResource)
                geometry.indexResource.frameReferenced = frame.serial;
            // D3DRS_SCISSORTESTENABLE gates the rect; without it D3D9 ignores
            // whatever SetScissorRect last set.
            const scissorEnabled =
                (state.renderStates.get(D3DRS_SCISSORTESTENABLE) || 0) !== 0 &&
                !!state.scissorRect;
            if (scissorEnabled) ++this.stats.drawsWithScissor;
            const attachmentCount = Math.max(1, Math.min(4,
                (targets.colors && targets.colors.length) || 1));
            ++this.mrtAttachmentDraws[attachmentCount];
            frame.ops.push({
                kind: "draw", pipeline, bindGroup: binding.group,
                dynamicOffsets: binding.dynamicOffsets, targets,
                viewport: { ...state.viewport },
                scissor: scissorEnabled ? { ...state.scissorRect } : null,
                blendConstant: pipelineState.blendConstant,
                stencilReference: pipelineState.stencilReference,
                vertexBuffers, indexInfo: geometry.indexInfo,
                vertexCount: pointExpansion ? 6 : geometry.vertexCount,
                instanceCount: pointExpansion ? geometry.vertexCount : 1,
            });
            if (pointExpansion) {
                ++this.stats.pointSpriteDraws;
                this.stats.pointSpriteInstances += geometry.vertexCount || 0;
            }
            // The pointer is almost always the final thing a frame draws, so
            // the texture bound by the last draw is the quickest way to name
            // the cursor's texture without hunting through the whole atlas set.
            if (program.samplerIndices.length)
                this.stats.lastDrawTexture =
                    state.textures.get(program.samplerIndices[0]) || 0;
            if (state.vertexShaderHandle || state.pixelShaderHandle)
                ++this.stats.programmableDraws;
            if (geometry.indexInfo) ++this.stats.indexedDrawCalls;
            else ++this.stats.drawCalls;
        }

        // Every draw path below can bail out for several different reasons,
        // and silently dropping them looks identical to "the app never drew"
        // from the outside -- exactly the blind spot that hid a stalled
        // renderer behind healthy-looking batch/present counters. Count every
        // drop and describe the first one in full.
        noteDroppedDraw(which, state, reasons) {
            ++this.stats.droppedDraws;
            const key = which + ":" + reasons.join(";");
            this.droppedDrawReasons = this.droppedDrawReasons || new Set();
            if (this.droppedDrawReasons.has(key)) return;
            this.droppedDrawReasons.add(key);
            const declaration = this.resources.get(state.vertexDeclarationHandle);
            console.warn("[d3d9-webgpu] " + which + " dropped: " +
                reasons.join("; "), {
                    reasons,
                    hasFvfElements: !!state.fvfElements,
                    vertexDeclarationHandle: state.vertexDeclarationHandle,
                    declarationResourceFound: !!declaration,
                    declarationElements: this.currentElements(state),
                    vertexShaderHandle: state.vertexShaderHandle,
                    pixelShaderHandle: state.pixelShaderHandle,
                    stream0: state.streams.get(0) || null,
                    indexBufferHandle: state.indexBufferHandle,
                    resourceCount: this.resources.size,
                });
        }

        // Collects the vertex buffers a draw will bind, keyed by stream. The
        // per-stream byte offset folds in both SetStreamSource's OffsetInBytes
        // and (for a non-indexed draw) StartVertex, because WebGPU takes the
        // first-vertex offset on setVertexBuffer rather than on draw().
        boundStreams(state, extraVertexOffset) {
            const streams = new Map();
            for (const [index, binding] of state.streams) {
                const resource = this.resources.get(binding.bufferHandle);
                if (!resource || !resource.gpuBuffer) continue;
                streams.set(index, { buffer: resource.gpuBuffer, resource,
                    offset: (binding.offsetInBytes || 0) +
                        (extraVertexOffset || 0) * (binding.stride || 0) });
            }
            return streams;
        }

        // WebGPU cannot combine an index buffer with instance-rate source
        // attributes to select an arbitrary point for each instance. Indexed
        // point lists are uncommon but legal, so compact the referenced
        // vertices from the guest-maintained CPU shadows into transient,
        // sequential streams before using the same quad-expansion pipeline.
        expandIndexedPointStreams(state, streams, indexResource, firstIndex,
                indexCount, baseVertex) {
            if (!indexResource || !indexResource.shadow) return null;
            const wide = indexResource.indexFormat === "uint32";
            const indices = wide
                ? new Uint32Array(indexResource.shadow.buffer,
                    indexResource.shadow.byteOffset,
                    indexResource.shadow.byteLength >>> 2)
                : new Uint16Array(indexResource.shadow.buffer,
                    indexResource.shadow.byteOffset,
                    indexResource.shadow.byteLength >>> 1);
            if (firstIndex + indexCount > indices.length) return null;
            const result = new Map();
            for (const [streamIndex, binding] of streams) {
                const streamState = state.streams.get(streamIndex);
                const stride = streamState && streamState.stride;
                const shadow = binding.resource && binding.resource.shadow;
                if (!stride || !shadow) return null;
                const outputBytes = indexCount * stride;
                const output = new Uint8Array(alignUp(outputBytes, 4));
                for (let point = 0; point < indexCount; ++point) {
                    const vertex = Number(indices[firstIndex + point]) + baseVertex;
                    if (vertex < 0) return null;
                    const source = binding.offset + vertex * stride;
                    if (source < 0 || source + stride > shadow.length) return null;
                    output.set(shadow.subarray(source, source + stride), point * stride);
                }
                const buffer = this.device.createBuffer({
                    label: "D3D9 indexed point expansion",
                    size: Math.max(4, output.byteLength),
                    usage: BUFFER_USAGE_VERTEX | BUFFER_USAGE_COPY_DST,
                });
                this.writeBufferAligned(buffer, 0, output, 0, outputBytes);
                this.retireAfterSubmit(buffer);
                result.set(streamIndex, { buffer, offset: 0 });
            }
            ++this.stats.indexedPointExpansions;
            return result;
        }

        onDrawPrimitive(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const primitiveType = view.getUint32(offset + 4, true);
            const startVertex = view.getUint32(offset + 8, true);
            const primitiveCount = view.getUint32(offset + 12, true);
            const state = this.deviceState(deviceHandle);
            const elements = this.currentElements(state);
            if (!elements) {
                this.noteDroppedDraw("DrawPrimitive", state,
                    ["no vertex declaration (SetFVF/SetVertexDeclaration)"]);
                return;
            }
            const vertexCount = primitiveElementCount(primitiveType, primitiveCount);
            if (vertexCount === null) {
                this.noteDroppedDraw("DrawPrimitive", state,
                    ["unsupported primitive type " + primitiveType]);
                return;
            }
            const streams = this.boundStreams(state, startVertex);
            if (primitiveType === D3DPT_TRIANGLEFAN) {
                // WebGPU has no fan topology; synthesise the index buffer that
                // turns one into a triangle list.
                const indexBuffer = this.triangleFanIndexBuffer(vertexCount);
                if (!indexBuffer) {
                    this.noteDroppedDraw("DrawPrimitive", state,
                        ["triangle fan with too few vertices"]);
                    return;
                }
                this.recordDraw(state, elements, "DrawPrimitive", {
                    topology: "triangle-list", streams,
                    indexInfo: { buffer: indexBuffer, format: "uint32", offset: 0,
                        count: (vertexCount - 2) * 3, firstIndex: 0, baseVertex: 0 },
                });
                return;
            }
            this.recordDraw(state, elements, "DrawPrimitive", {
                topology: topologyFor(primitiveType), streams,
                indexInfo: null, vertexCount,
            });
        }

        onDrawIndexedPrimitive(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const primitiveType = view.getUint32(offset + 4, true);
            const baseVertexIndex = view.getInt32(offset + 8, true);
            const startIndex = view.getUint32(offset + 20, true);
            const primitiveCount = view.getUint32(offset + 24, true);
            const state = this.deviceState(deviceHandle);
            const elements = this.currentElements(state);
            const ib = this.resources.get(state.indexBufferHandle);
            if (!elements || !ib) {
                const reasons = [];
                if (!elements) reasons.push("no vertex declaration (SetFVF/SetVertexDeclaration)");
                if (!ib) reasons.push("index buffer resource missing");
                this.noteDroppedDraw("DrawIndexedPrimitive", state, reasons);
                return;
            }
            const indexCount = primitiveElementCount(primitiveType, primitiveCount);
            if (indexCount === null) {
                this.noteDroppedDraw("DrawIndexedPrimitive", state,
                    ["unsupported primitive type " + primitiveType]);
                return;
            }
            const streams = this.boundStreams(state, 0);
            if (primitiveType === D3DPT_POINTLIST) {
                const expanded = this.expandIndexedPointStreams(state, streams,
                    ib, startIndex, indexCount, baseVertexIndex);
                if (!expanded) {
                    this.noteDroppedDraw("DrawIndexedPrimitive", state,
                        ["indexed point list could not be compacted from buffer shadows"]);
                    return;
                }
                this.recordDraw(state, elements, "DrawIndexedPrimitive", {
                    topology: "point-list", streams: expanded,
                    indexInfo: null, vertexCount: indexCount,
                });
                return;
            }
            if (primitiveType === D3DPT_TRIANGLEFAN) {
                // Re-index the fan through the buffer's CPU mirror; the GPU
                // copy is write-only from here.
                const converted = this.triangleFanFromIndices(ib, startIndex, indexCount);
                if (!converted) {
                    this.noteDroppedDraw("DrawIndexedPrimitive", state,
                        ["indexed triangle fan could not be converted"]);
                    return;
                }
                this.recordDraw(state, elements, "DrawIndexedPrimitive", {
                    topology: "triangle-list", streams,
                    indexInfo: { buffer: converted.buffer, format: "uint32",
                        offset: 0, count: converted.count, firstIndex: 0,
                        baseVertex: baseVertexIndex },
                });
                return;
            }
            const topology = topologyFor(primitiveType);
            this.recordDraw(state, elements, "DrawIndexedPrimitive", {
                topology, streams,
                stripIndexFormat: isStripTopology(topology) ? ib.indexFormat : undefined,
                indexResource: ib,
                indexInfo: { buffer: ib.gpuBuffer, format: ib.indexFormat, offset: 0,
                    count: indexCount, firstIndex: startIndex,
                    baseVertex: baseVertexIndex },
            });
        }

        onDrawPrimitiveUP(bytes, view, offset, length) {
            const deviceHandle = view.getUint32(offset, true);
            const primitiveType = view.getUint32(offset + 4, true);
            const primitiveCount = view.getUint32(offset + 8, true);
            const stride = view.getUint32(offset + 12, true);
            const vertexBytes = view.getUint32(offset + 20, true);
            const dataOffset = view.getUint32(offset + 24, true);
            const state = this.deviceState(deviceHandle);
            const elements = this.currentElements(state);
            if (!elements) {
                this.noteDroppedDraw("DrawPrimitiveUP", state,
                    ["no vertex declaration (SetFVF/SetVertexDeclaration)"]);
                return;
            }
            const elementCount = primitiveElementCount(primitiveType, primitiveCount);
            if (elementCount === null) {
                this.noteDroppedDraw("DrawPrimitiveUP", state,
                    ["unsupported primitive type " + primitiveType]);
                return;
            }
            const buffer = this.device.createBuffer({
                size: Math.max(4, alignUp(vertexBytes, 4)),
                usage: BUFFER_USAGE_VERTEX | BUFFER_USAGE_COPY_DST,
            });
            this.writeBufferAligned(buffer, 0, bytes, dataOffset, vertexBytes);
            this.retireAfterSubmit(buffer);
            // Draw*UP feeds one implicit stream 0 whose stride the command
            // carries, not one bound through SetStreamSource.
            const streams = new Map([[0, { buffer, offset: 0 }]]);
            const geometry = { streams, indexInfo: null, vertexCount: elementCount,
                topology: topologyFor(primitiveType) };
            if (primitiveType === D3DPT_TRIANGLEFAN) {
                const indexBuffer = this.triangleFanIndexBuffer(elementCount);
                if (!indexBuffer) {
                    this.noteDroppedDraw("DrawPrimitiveUP", state,
                        ["triangle fan with too few vertices"]);
                    return;
                }
                geometry.topology = "triangle-list";
                geometry.indexInfo = { buffer: indexBuffer, format: "uint32",
                    offset: 0, count: (elementCount - 2) * 3, firstIndex: 0,
                    baseVertex: 0 };
            }
            this.recordDrawWithStride(state, elements, "DrawPrimitiveUP",
                geometry, stride);
            ++this.stats.upDrawCalls;
        }

        onDrawIndexedPrimitiveUP(bytes, view, offset) {
            const deviceHandle = view.getUint32(offset, true);
            const primitiveType = view.getUint32(offset + 4, true);
            const primitiveCount = view.getUint32(offset + 16, true);
            const indexFormatValue = view.getUint32(offset + 20, true);
            // M1 never read `stride` out of this payload but referenced it
            // when recording the draw, so every DrawIndexedPrimitiveUP threw
            // a ReferenceError and took the whole batch down with it (the
            // batch's catch handler discarded the frame). War3's main menu
            // happens not to use this entry point, which is why it stayed
            // hidden.
            const stride = view.getUint32(offset + 24, true);
            const indexBytes = view.getUint32(offset + 32, true);
            const vertexBytes = view.getUint32(offset + 36, true);
            const indexDataOffset = view.getUint32(offset + 40, true);
            const vertexDataOffset = view.getUint32(offset + 44, true);
            const state = this.deviceState(deviceHandle);
            const elements = this.currentElements(state);
            if (!elements) {
                this.noteDroppedDraw("DrawIndexedPrimitiveUP", state,
                    ["no vertex declaration (SetFVF/SetVertexDeclaration)"]);
                return;
            }
            const elementCount = primitiveElementCount(primitiveType, primitiveCount);
            if (elementCount === null) {
                this.noteDroppedDraw("DrawIndexedPrimitiveUP", state,
                    ["unsupported primitive type " + primitiveType]);
                return;
            }
            if (primitiveType === D3DPT_POINTLIST) {
                const wide = indexFormatValue === D3DFMT_INDEX32;
                const indexWidth = wide ? 4 : 2;
                if (elementCount * indexWidth > indexBytes) {
                    this.noteDroppedDraw("DrawIndexedPrimitiveUP", state,
                        ["point index payload is shorter than its primitive count"]);
                    return;
                }
                const outputBytes = elementCount * stride;
                const output = new Uint8Array(alignUp(outputBytes, 4));
                for (let point = 0; point < elementCount; ++point) {
                    const indexOffset = indexDataOffset + point * indexWidth;
                    const vertex = wide ? view.getUint32(indexOffset, true)
                        : view.getUint16(indexOffset, true);
                    const source = vertexDataOffset + vertex * stride;
                    if (source < vertexDataOffset ||
                            source + stride > vertexDataOffset + vertexBytes ||
                            source + stride > bytes.byteLength) {
                        this.noteDroppedDraw("DrawIndexedPrimitiveUP", state,
                            ["point index references vertex data outside the payload"]);
                        return;
                    }
                    output.set(bytes.subarray(source, source + stride), point * stride);
                }
                const vertexBuffer = this.device.createBuffer({
                    label: "D3D9 indexed UP point expansion",
                    size: Math.max(4, output.byteLength),
                    usage: BUFFER_USAGE_VERTEX | BUFFER_USAGE_COPY_DST,
                });
                this.writeBufferAligned(vertexBuffer, 0, output, 0, outputBytes);
                this.retireAfterSubmit(vertexBuffer);
                this.recordDrawWithStride(state, elements,
                    "DrawIndexedPrimitiveUP", {
                        topology: "point-list",
                        streams: new Map([[0, { buffer: vertexBuffer, offset: 0 }]]),
                        indexInfo: null, vertexCount: elementCount,
                    }, stride);
                ++this.stats.indexedPointExpansions;
                ++this.stats.upDrawCalls;
                return;
            }
            const vertexBuffer = this.device.createBuffer({
                size: Math.max(4, alignUp(vertexBytes, 4)),
                usage: BUFFER_USAGE_VERTEX | BUFFER_USAGE_COPY_DST,
            });
            const indexBuffer = this.device.createBuffer({
                size: Math.max(4, alignUp(indexBytes, 4)),
                usage: BUFFER_USAGE_INDEX | BUFFER_USAGE_COPY_DST,
            });
            this.writeBufferAligned(vertexBuffer, 0, bytes, vertexDataOffset, vertexBytes);
            this.writeBufferAligned(indexBuffer, 0, bytes, indexDataOffset, indexBytes);
            this.retireAfterSubmit(vertexBuffer);
            this.retireAfterSubmit(indexBuffer);
            const format = indexFormatValue === D3DFMT_INDEX32 ? "uint32" : "uint16";
            const topology = topologyFor(primitiveType);
            if (primitiveType === D3DPT_TRIANGLEFAN) {
                this.noteDroppedDraw("DrawIndexedPrimitiveUP", state,
                    ["indexed triangle fans are not converted on the UP path"]);
                return;
            }
            this.recordDrawWithStride(state, elements, "DrawIndexedPrimitiveUP", {
                topology,
                streams: new Map([[0, { buffer: vertexBuffer, offset: 0 }]]),
                stripIndexFormat: isStripTopology(topology) ? format : undefined,
                indexInfo: { buffer: indexBuffer, format, offset: 0,
                    count: elementCount, firstIndex: 0, baseVertex: 0 },
            }, stride);
            ++this.stats.upDrawCalls;
        }

        // Draw*UP carries its own stride rather than having one bound through
        // SetStreamSource, so vertexBufferLayoutsFor() -- which reads
        // state.streams -- needs stream 0 temporarily standing in for it.
        recordDrawWithStride(state, elements, which, geometry, stride) {
            const saved = state.streams.get(0);
            state.streams.set(0, { bufferHandle: 0, stride, offsetInBytes: 0 });
            try {
                this.recordDraw(state, elements, which, geometry);
            } finally {
                if (saved) state.streams.set(0, saved);
                else state.streams.delete(0);
            }
        }

        // (0,1,2), (0,2,3), (0,3,4)... -- the triangle list a fan of
        // `vertexCount` vertices expands to.
        triangleFanIndexBuffer(vertexCount) {
            if (vertexCount < 3) return null;
            const triangles = vertexCount - 2;
            const indices = new Uint32Array(triangles * 3);
            for (let i = 0; i < triangles; ++i) {
                indices[i * 3] = 0;
                indices[i * 3 + 1] = i + 1;
                indices[i * 3 + 2] = i + 2;
            }
            const buffer = this.device.createBuffer({
                size: indices.byteLength,
                usage: BUFFER_USAGE_INDEX | BUFFER_USAGE_COPY_DST,
            });
            this.device.queue.writeBuffer(buffer, 0, indices);
            this.retireAfterSubmit(buffer);
            return buffer;
        }

        triangleFanFromIndices(indexResource, firstIndex, indexCount) {
            if (indexCount < 3 || !indexResource.shadow) return null;
            const wide = indexResource.indexFormat === "uint32";
            const source = wide
                ? new Uint32Array(indexResource.shadow.buffer,
                    indexResource.shadow.byteOffset, indexResource.shadow.length >> 2)
                : new Uint16Array(indexResource.shadow.buffer,
                    indexResource.shadow.byteOffset, indexResource.shadow.length >> 1);
            if (firstIndex + indexCount > source.length) return null;
            const triangles = indexCount - 2;
            const indices = new Uint32Array(triangles * 3);
            const hub = source[firstIndex];
            for (let i = 0; i < triangles; ++i) {
                indices[i * 3] = hub;
                indices[i * 3 + 1] = source[firstIndex + i + 1];
                indices[i * 3 + 2] = source[firstIndex + i + 2];
            }
            const buffer = this.device.createBuffer({
                size: indices.byteLength,
                usage: BUFFER_USAGE_INDEX | BUFFER_USAGE_COPY_DST,
            });
            this.device.queue.writeBuffer(buffer, 0, indices);
            this.retireAfterSubmit(buffer);
            return { buffer, count: indices.length };
        }
    }

    function primitiveElementCount(type, primitiveCount) {
        switch (type) {
        case D3DPT_POINTLIST: return primitiveCount;
        case D3DPT_LINELIST: return primitiveCount * 2;
        case D3DPT_LINESTRIP: return primitiveCount + 1;
        case D3DPT_TRIANGLELIST: return primitiveCount * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN: return primitiveCount + 2;
        default: return null;
        }
    }

    // D3DPRIMITIVETYPE -> GPUPrimitiveTopology. M1 hardcoded "triangle-list"
    // for every draw while still computing strip/fan element counts, so a
    // strip of N triangles was rasterised as floor((N+2)/3) unrelated
    // triangles -- geometry that is wrong rather than missing, and therefore
    // easy to mistake for a transform bug. TRIANGLEFAN has no WebGPU
    // topology at all and is converted to an indexed triangle list by the
    // callers instead.
    function topologyFor(type) {
        switch (type) {
        case D3DPT_POINTLIST: return "point-list";
        case D3DPT_LINELIST: return "line-list";
        case D3DPT_LINESTRIP: return "line-strip";
        case D3DPT_TRIANGLESTRIP: return "triangle-strip";
        default: return "triangle-list";
        }
    }

    function isStripTopology(topology) {
        return topology === "triangle-strip" || topology === "line-strip";
    }

    global.D3D9WebGPUExecutor = D3D9WebGPUExecutor;
    global.installD3D9WebGPUExecutor = function(canvas, options) {
        return new D3D9WebGPUExecutor(canvas, options);
    };

    if (typeof module !== "undefined" && module.exports) {
        module.exports = {
            D3D9WebGPUExecutor,
            V86GL_CTRL_D3D9_BATCH: 0xFFE1,
            // Exported so the WGSL validation test can run the synthesised
            // fixed-function stages through a real compiler alongside the
            // translated ones -- they meet over the same varying contract and
            // a mistake in either breaks the same pipelines.
            buildFixedFunctionVertexShader,
            buildFixedFunctionPixelShader,
        };
    }
})(typeof globalThis !== "undefined" ? globalThis : this);
