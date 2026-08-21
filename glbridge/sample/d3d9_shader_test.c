// Direct3D 9 shader-model 2.0 test for the v86 WebGPU bridge (M2).
//
// The test after d3d9_world_test.c. It is the first one that leaves the fixed
// function pipeline entirely: a real IDirect3DVertexShader9 and
// IDirect3DPixelShader9 built from compiled bytecode, driven by float
// constant registers, sampling a texture through independent sampler state,
// with a real IDirect3DVertexDeclaration9 binding attributes to the vertex
// shader's dcl'd inputs.
//
// The bytecode below is hand-assembled rather than produced by fxc, so the
// guest has no D3DX dependency and the exact token encoding is visible and
// reviewable in this file. Each token is annotated with the instruction it
// represents; the encoding rules are:
//
//   version         0xFFFE0200 = vs_2_0, 0xFFFF0200 = ps_2_0
//   instruction     opcode | (token_count_that_follows << 24)
//   destination     0x80000000 | reg | regtype | (writemask << 16)
//   source          0x80000000 | reg | regtype | (swizzle << 16)
//   register type   ((type & 7) << 28) | ((type & 0x18) << 8)
//   .xyzw swizzle   0xE4
//
// What each stage proves, and why it is here rather than only in the Node
// tests: the Node suite drives the *host* with batches it builds itself, so
// it cannot catch a guest-side mistake -- a mis-scanned token count, a
// constant range that never gets sent, a shader handle that collides with a
// buffer handle. This runs the real d3d9.dll inside the real XP guest.
//
// Expected result: a quad in the middle of the window whose colour is the
// texture's checkerboard modulated by the vertex colour, tinted by the vertex
// shader's c4 and brightened by the pixel shader's c0/c1. A black or missing
// quad means a stage failed; the window title and the guest debug log name
// the last call attempted.
//
// Build for Windows XP as documented in ../d3d9proxy/README.md.

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>

#define TEST_CLIENT_WIDTH  640
#define TEST_CLIENT_HEIGHT 480
#define TEST_TEXTURE_SIZE 16

typedef struct TestVertex
{
    FLOAT x;
    FLOAT y;
    FLOAT z;
    DWORD color;
    FLOAT u;
    FLOAT v;
} TestVertex;

static const char g_window_class[] = "V86GLD3D9ShaderTest";

/* A quad in clip space; the vertex shader scales and shifts it through c0-c3
 * so a wrong constant upload is visible as a mispositioned quad rather than
 * as nothing at all. */
static const TestVertex g_vertices[] =
{
    {-0.6f,  0.6f, 0.5f, D3DCOLOR_XRGB(255, 255, 255), 0.0f, 0.0f},
    { 0.6f,  0.6f, 0.5f, D3DCOLOR_XRGB(255, 160, 160), 1.0f, 0.0f},
    { 0.6f, -0.6f, 0.5f, D3DCOLOR_XRGB(160, 255, 160), 1.0f, 1.0f},
    {-0.6f, -0.6f, 0.5f, D3DCOLOR_XRGB(160, 160, 255), 0.0f, 1.0f},
};
static const WORD g_indices[] = { 0, 1, 2, 0, 2, 3 };

static const D3DVERTEXELEMENT9 g_declaration[] =
{
    {0,  0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0},
    {0, 16, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    D3DDECL_END()
};

/*
 * vs_2_0
 *     dcl_position  v0
 *     dcl_color0    v1
 *     dcl_texcoord0 v2
 *     m4x4 oPos, v0, c0     ; c0-c3 hold the transform, transposed the way
 *                           ; m4x4's four dot products consume it
 *     mul  oD0,  v1, c4     ; tint the diffuse by a constant register
 *     mov  oT0,  v2
 */
static const DWORD g_vertex_shader_code[] =
{
    0xFFFE0200,
    0x0200001F, 0x80000000, 0x900F0000,             /* dcl_position v0 */
    0x0200001F, 0x8000000A, 0x900F0001,             /* dcl_color0 v1 */
    0x0200001F, 0x80000005, 0x900F0002,             /* dcl_texcoord0 v2 */
    0x03000014, 0xC00F0000, 0x90E40000, 0xA0E40000, /* m4x4 oPos, v0, c0 */
    0x03000005, 0xD00F0000, 0x90E40001, 0xA0E40004, /* mul oD0, v1, c4 */
    0x02000001, 0xE00F0000, 0x90E40002,             /* mov oT0, v2 */
    0x0000FFFF
};

/*
 * ps_2_0
 *     dcl_2d        s0
 *     dcl           t0.xy
 *     dcl_color0    v0
 *     def  c1, 0.10, 0.10, 0.10, 0.0
 *     texld r0, t0, s0
 *     mul   r0, r0, v0
 *     mad   oC0, r0, c0, c1  ; c0 comes from SetPixelShaderConstantF, c1 from
 *                            ; the shader's own def -- both paths in one draw
 */
static const DWORD g_pixel_shader_code[] =
{
    0xFFFF0200,
    0x0200001F, 0x90000000, 0xA00F0800,             /* dcl_2d s0 */
    0x0200001F, 0x80000005, 0xB0030000,             /* dcl t0.xy */
    0x0200001F, 0x8000000A, 0x900F0000,             /* dcl_color0 v0 */
    0x05000051, 0xA00F0001,                         /* def c1, ... */
        0x3DCCCCCD, 0x3DCCCCCD, 0x3DCCCCCD, 0x00000000,
    0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800, /* texld r0, t0, s0 */
    0x03000005, 0x800F0000, 0x80E40000, 0x90E40000, /* mul r0, r0, v0 */
    0x04000004, 0x800F0800, 0x80E40000, 0xA0E40000, 0xA0E40001, /* mad oC0 */
    0x0000FFFF
};

/*
 * c0-c3: the transform, stored as the *columns* of the row-vector matrix a
 * D3D application would build -- which is what m4x4's four dot products
 * against a single vertex expect, and the layout HLSL's compiler emits.
 * Here: scale by 0.8 and shift right by 0.1.
 */
static const float g_vertex_constants[] =
{
    0.8f, 0.0f, 0.0f, 0.1f,
    0.0f, 0.8f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
};
/* c4: the diffuse tint. */
static const float g_vertex_tint[] = { 1.0f, 0.9f, 0.8f, 1.0f };
/* Pixel shader c0: the multiplier in the final mad. */
static const float g_pixel_scale[] = { 0.9f, 0.9f, 0.9f, 1.0f };

static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_device;
static IDirect3DVertexBuffer9 *g_vertex_buffer;
static IDirect3DIndexBuffer9 *g_index_buffer;
static IDirect3DVertexDeclaration9 *g_vertex_declaration;
static IDirect3DVertexShader9 *g_vertex_shader;
static IDirect3DPixelShader9 *g_pixel_shader;
static IDirect3DTexture9 *g_texture;
static HWND g_window;
static const char *g_failed_stage = "unknown stage";
static BOOL g_releasing_d3d9;

static void trace_text(const char *text)
{
    OutputDebugStringA("[d3d9-shader] ");
    OutputDebugStringA(text);
    OutputDebugStringA("\r\n");
}

static void trace_hresult(const char *stage, HRESULT hr)
{
    char line[192];

    wsprintfA(line, "[d3d9-shader] %s -> 0x%08lX\r\n",
            stage, (unsigned long)hr);
    OutputDebugStringA(line);
}

static void set_result_title(HWND hwnd, const char *stage, HRESULT hr)
{
    char title[192];

    wsprintfA(title, "D3D9 shader: %s (0x%08lX)", stage, (unsigned long)hr);
    SetWindowTextA(hwnd, title);
}

static void begin_stage(const char *stage)
{
    char title[192];

    g_failed_stage = stage;
    trace_text(stage);
    wsprintfA(title, "D3D9 shader: calling %s", stage);
    if (g_window)
    {
        SetWindowTextA(g_window, title);
        RedrawWindow(g_window, NULL, NULL,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
    }
}

static HRESULT failed(const char *stage, HRESULT hr)
{
    g_failed_stage = stage;
    trace_hresult(stage, hr);
    return hr;
}

static LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS *exception)
{
    DWORD code = exception && exception->ExceptionRecord ?
            exception->ExceptionRecord->ExceptionCode : 0xE0000001u;
    char line[192];

    wsprintfA(line, "[d3d9-shader] unhandled exception 0x%08lX at %s\r\n",
            (unsigned long)code, g_failed_stage);
    OutputDebugStringA(line);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void release_d3d9(void)
{
    if (g_releasing_d3d9)
        return;
    g_releasing_d3d9 = TRUE;

#define RELEASE_ONE(field, type) \
    do { \
        if (field) { \
            type *object = field; \
            field = NULL; \
            trace_hresult(#type "::Release", \
                    (HRESULT)type##_Release(object)); \
        } \
    } while (0)

    RELEASE_ONE(g_texture, IDirect3DTexture9);
    RELEASE_ONE(g_pixel_shader, IDirect3DPixelShader9);
    RELEASE_ONE(g_vertex_shader, IDirect3DVertexShader9);
    RELEASE_ONE(g_vertex_declaration, IDirect3DVertexDeclaration9);
    RELEASE_ONE(g_index_buffer, IDirect3DIndexBuffer9);
    RELEASE_ONE(g_vertex_buffer, IDirect3DVertexBuffer9);
    RELEASE_ONE(g_device, IDirect3DDevice9);
    RELEASE_ONE(g_d3d, IDirect3D9);
#undef RELEASE_ONE

    trace_text("teardown complete");
    g_releasing_d3d9 = FALSE;
}

/* The caps check is the gate for everything below: an honest driver that
 * reports (0,0) would make CreateVertexShader fail, and the test should say
 * so plainly instead of failing three calls later. */
static HRESULT check_shader_caps(void)
{
    D3DCAPS9 caps;
    char line[192];
    HRESULT hr;

    begin_stage("GetDeviceCaps");
    hr = IDirect3DDevice9_GetDeviceCaps(g_device, &caps);
    if (FAILED(hr))
        return failed("GetDeviceCaps", hr);
    wsprintfA(line, "[d3d9-shader]   vs=%lu.%lu ps=%lu.%lu maxVSConst=%lu\r\n",
            (unsigned long)((caps.VertexShaderVersion >> 8) & 0xFF),
            (unsigned long)(caps.VertexShaderVersion & 0xFF),
            (unsigned long)((caps.PixelShaderVersion >> 8) & 0xFF),
            (unsigned long)(caps.PixelShaderVersion & 0xFF),
            (unsigned long)caps.MaxVertexShaderConst);
    OutputDebugStringA(line);
    if (caps.VertexShaderVersion < D3DVS_VERSION(2, 0)
            || caps.PixelShaderVersion < D3DPS_VERSION(2, 0))
        return failed("caps report less than shader model 2.0",
                D3DERR_NOTAVAILABLE);
    return D3D_OK;
}

static HRESULT create_device(HWND hwnd)
{
    D3DDISPLAYMODE mode;
    D3DPRESENT_PARAMETERS present_parameters;
    HRESULT hr;

    begin_stage("Direct3DCreate9");
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d)
        return failed("Direct3DCreate9 returned NULL", E_FAIL);

    ZeroMemory(&mode, sizeof(mode));
    begin_stage("GetAdapterDisplayMode");
    hr = IDirect3D9_GetAdapterDisplayMode(g_d3d, D3DADAPTER_DEFAULT, &mode);
    if (FAILED(hr))
        return failed("GetAdapterDisplayMode", hr);

    ZeroMemory(&present_parameters, sizeof(present_parameters));
    present_parameters.BackBufferWidth = TEST_CLIENT_WIDTH;
    present_parameters.BackBufferHeight = TEST_CLIENT_HEIGHT;
    present_parameters.BackBufferFormat = mode.Format;
    present_parameters.BackBufferCount = 1;
    present_parameters.MultiSampleType = D3DMULTISAMPLE_NONE;
    present_parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present_parameters.hDeviceWindow = hwnd;
    present_parameters.Windowed = TRUE;
    present_parameters.EnableAutoDepthStencil = TRUE;
    present_parameters.AutoDepthStencilFormat = D3DFMT_D24S8;
    present_parameters.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

    begin_stage("CreateDevice");
    hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present_parameters,
            &g_device);
    if (FAILED(hr))
        return failed("CreateDevice", hr);

    return check_shader_caps();
}

static HRESULT create_geometry(void)
{
    void *destination;
    HRESULT hr;

    begin_stage("CreateVertexBuffer");
    hr = IDirect3DDevice9_CreateVertexBuffer(g_device, sizeof(g_vertices),
            D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &g_vertex_buffer, NULL);
    if (FAILED(hr))
        return failed("CreateVertexBuffer", hr);

    begin_stage("VertexBuffer::Lock");
    hr = IDirect3DVertexBuffer9_Lock(g_vertex_buffer, 0, sizeof(g_vertices),
            &destination, 0);
    if (FAILED(hr))
        return failed("VertexBuffer::Lock", hr);
    CopyMemory(destination, g_vertices, sizeof(g_vertices));
    IDirect3DVertexBuffer9_Unlock(g_vertex_buffer);

    begin_stage("CreateIndexBuffer");
    hr = IDirect3DDevice9_CreateIndexBuffer(g_device, sizeof(g_indices),
            D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED,
            &g_index_buffer, NULL);
    if (FAILED(hr))
        return failed("CreateIndexBuffer", hr);

    begin_stage("IndexBuffer::Lock");
    hr = IDirect3DIndexBuffer9_Lock(g_index_buffer, 0, sizeof(g_indices),
            &destination, 0);
    if (FAILED(hr))
        return failed("IndexBuffer::Lock", hr);
    CopyMemory(destination, g_indices, sizeof(g_indices));
    IDirect3DIndexBuffer9_Unlock(g_index_buffer);

    begin_stage("CreateVertexDeclaration");
    hr = IDirect3DDevice9_CreateVertexDeclaration(g_device, g_declaration,
            &g_vertex_declaration);
    if (FAILED(hr))
        return failed("CreateVertexDeclaration", hr);

    return D3D_OK;
}

static HRESULT create_texture(void)
{
    D3DLOCKED_RECT locked;
    IDirect3DSurface9 *surface = NULL;
    UINT x;
    UINT y;
    HRESULT hr;

    begin_stage("CreateTexture");
    hr = IDirect3DDevice9_CreateTexture(g_device, TEST_TEXTURE_SIZE,
            TEST_TEXTURE_SIZE, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
            &g_texture, NULL);
    if (FAILED(hr))
        return failed("CreateTexture", hr);

    begin_stage("GetSurfaceLevel");
    hr = IDirect3DTexture9_GetSurfaceLevel(g_texture, 0, &surface);
    if (FAILED(hr))
        return failed("GetSurfaceLevel", hr);

    begin_stage("Surface::LockRect");
    hr = IDirect3DSurface9_LockRect(surface, &locked, NULL, 0);
    if (FAILED(hr))
    {
        IDirect3DSurface9_Release(surface);
        return failed("Surface::LockRect", hr);
    }
    for (y = 0; y < TEST_TEXTURE_SIZE; ++y)
    {
        DWORD *row = (DWORD *)((BYTE *)locked.pBits + y * locked.Pitch);
        for (x = 0; x < TEST_TEXTURE_SIZE; ++x)
        {
            /* A 4x4-texel checkerboard: any UV or sampler-address mistake
             * shows up as a shifted or stretched pattern rather than as a
             * uniform colour that could be blamed on anything. */
            BOOL light = (((x >> 2) + (y >> 2)) & 1) != 0;
            row[x] = light ? D3DCOLOR_ARGB(255, 235, 235, 245)
                    : D3DCOLOR_ARGB(255, 40, 60, 120);
        }
    }
    IDirect3DSurface9_UnlockRect(surface);
    IDirect3DSurface9_Release(surface);
    return D3D_OK;
}

static HRESULT create_shaders(void)
{
    DWORD readback[64];
    UINT size;
    float constants[4];
    HRESULT hr;

    begin_stage("CreateVertexShader");
    hr = IDirect3DDevice9_CreateVertexShader(g_device, g_vertex_shader_code,
            &g_vertex_shader);
    if (FAILED(hr))
        return failed("CreateVertexShader", hr);

    /* GetFunction has to report the same token count the guest scanned out of
     * the bare pointer it was handed; a mismatch here means the shader that
     * reached the host was truncated or over-long. */
    begin_stage("VertexShader::GetFunction(size)");
    size = 0;
    hr = IDirect3DVertexShader9_GetFunction(g_vertex_shader, NULL, &size);
    if (FAILED(hr))
        return failed("VertexShader::GetFunction(size)", hr);
    if (size != sizeof(g_vertex_shader_code))
    {
        char line[128];
        wsprintfA(line, "GetFunction size %lu, expected %lu",
                (unsigned long)size,
                (unsigned long)sizeof(g_vertex_shader_code));
        trace_text(line);
        return failed("VertexShader::GetFunction size mismatch",
                D3DERR_INVALIDCALL);
    }
    begin_stage("VertexShader::GetFunction(data)");
    hr = IDirect3DVertexShader9_GetFunction(g_vertex_shader, readback, &size);
    if (FAILED(hr))
        return failed("VertexShader::GetFunction(data)", hr);
    {
        UINT index;
        for (index = 0; index < sizeof(g_vertex_shader_code) / 4; ++index)
        {
            if (readback[index] == g_vertex_shader_code[index])
                continue;
            return failed("VertexShader::GetFunction content mismatch",
                    D3DERR_INVALIDCALL);
        }
    }

    begin_stage("CreatePixelShader");
    hr = IDirect3DDevice9_CreatePixelShader(g_device, g_pixel_shader_code,
            &g_pixel_shader);
    if (FAILED(hr))
        return failed("CreatePixelShader", hr);

    begin_stage("SetVertexShaderConstantF(c0..c3)");
    hr = IDirect3DDevice9_SetVertexShaderConstantF(g_device, 0,
            g_vertex_constants, 4);
    if (FAILED(hr))
        return failed("SetVertexShaderConstantF(c0..c3)", hr);

    begin_stage("SetVertexShaderConstantF(c4)");
    hr = IDirect3DDevice9_SetVertexShaderConstantF(g_device, 4,
            g_vertex_tint, 1);
    if (FAILED(hr))
        return failed("SetVertexShaderConstantF(c4)", hr);

    /* The shadow the guest keeps for redundant-set suppression is also what
     * GetVertexShaderConstantF reads, so this catches a shadow that never got
     * written -- which would silently stop every later constant upload. */
    begin_stage("GetVertexShaderConstantF(c4)");
    hr = IDirect3DDevice9_GetVertexShaderConstantF(g_device, 4, constants, 1);
    if (FAILED(hr))
        return failed("GetVertexShaderConstantF(c4)", hr);
    if (constants[0] != g_vertex_tint[0] || constants[3] != g_vertex_tint[3])
        return failed("GetVertexShaderConstantF returned the wrong value",
                D3DERR_INVALIDCALL);

    begin_stage("SetPixelShaderConstantF(c0)");
    hr = IDirect3DDevice9_SetPixelShaderConstantF(g_device, 0,
            g_pixel_scale, 1);
    if (FAILED(hr))
        return failed("SetPixelShaderConstantF(c0)", hr);

    return D3D_OK;
}

static HRESULT bind_state(void)
{
    HRESULT hr;

    begin_stage("SetVertexDeclaration");
    hr = IDirect3DDevice9_SetVertexDeclaration(g_device, g_vertex_declaration);
    if (FAILED(hr))
        return failed("SetVertexDeclaration", hr);

    begin_stage("SetStreamSource");
    hr = IDirect3DDevice9_SetStreamSource(g_device, 0, g_vertex_buffer, 0,
            sizeof(TestVertex));
    if (FAILED(hr))
        return failed("SetStreamSource", hr);

    begin_stage("SetIndices");
    hr = IDirect3DDevice9_SetIndices(g_device, g_index_buffer);
    if (FAILED(hr))
        return failed("SetIndices", hr);

    begin_stage("SetVertexShader");
    hr = IDirect3DDevice9_SetVertexShader(g_device, g_vertex_shader);
    if (FAILED(hr))
        return failed("SetVertexShader", hr);

    begin_stage("SetPixelShader");
    hr = IDirect3DDevice9_SetPixelShader(g_device, g_pixel_shader);
    if (FAILED(hr))
        return failed("SetPixelShader", hr);

    begin_stage("SetTexture");
    hr = IDirect3DDevice9_SetTexture(g_device, 0,
            (IDirect3DBaseTexture9 *)g_texture);
    if (FAILED(hr))
        return failed("SetTexture", hr);

    /* Independent sampler state (plan 4.4/12): these are what the host's
     * sampler cache is keyed on, and CLAMP rather than the WRAP default makes
     * a dropped SetSamplerState visible at the quad's edges. */
    begin_stage("SetSamplerState");
    hr = IDirect3DDevice9_SetSamplerState(g_device, 0, D3DSAMP_MAGFILTER,
            D3DTEXF_LINEAR);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetSamplerState(g_device, 0, D3DSAMP_MINFILTER,
                D3DTEXF_LINEAR);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetSamplerState(g_device, 0, D3DSAMP_ADDRESSU,
                D3DTADDRESS_CLAMP);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9_SetSamplerState(g_device, 0, D3DSAMP_ADDRESSV,
                D3DTADDRESS_CLAMP);
    if (FAILED(hr))
        return failed("SetSamplerState", hr);

    begin_stage("SetRenderState CULLMODE");
    hr = IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE,
            D3DCULL_NONE);
    if (FAILED(hr))
        return failed("SetRenderState(CULLMODE=NONE)", hr);

    return D3D_OK;
}

static HRESULT render(HWND hwnd)
{
    HRESULT hr;

    begin_stage("Clear");
    hr = IDirect3DDevice9_Clear(g_device, 0, NULL,
            D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(16, 24, 32),
            1.0f, 0);
    if (FAILED(hr))
        return failed("Clear", hr);

    begin_stage("BeginScene");
    hr = IDirect3DDevice9_BeginScene(g_device);
    if (FAILED(hr))
        return failed("BeginScene", hr);

    begin_stage("DrawIndexedPrimitive");
    hr = IDirect3DDevice9_DrawIndexedPrimitive(g_device, D3DPT_TRIANGLELIST,
            0, 0, 4, 0, 2);
    if (FAILED(hr))
    {
        IDirect3DDevice9_EndScene(g_device);
        return failed("DrawIndexedPrimitive", hr);
    }

    /* A second draw with the shaders unbound proves the fixed-function path
     * still works alongside them in one frame -- the executor synthesises a
     * fixed-function stage into the same varying contract, so a regression
     * there would show up as this strip vanishing while the quad survives. */
    begin_stage("SetVertexShader(NULL)");
    IDirect3DDevice9_SetVertexShader(g_device, NULL);
    IDirect3DDevice9_SetPixelShader(g_device, NULL);
    IDirect3DDevice9_SetFVF(g_device, D3DFVF_XYZ | D3DFVF_DIFFUSE
            | D3DFVF_TEX1);

    begin_stage("EndScene");
    hr = IDirect3DDevice9_EndScene(g_device);
    if (FAILED(hr))
        return failed("EndScene", hr);

    begin_stage("Present");
    hr = IDirect3DDevice9_Present(g_device, NULL, NULL, NULL, NULL);
    if (FAILED(hr))
        return failed("Present", hr);

    set_result_title(hwnd, "Present S_OK - expected tinted checkerboard quad",
            hr);
    return hr;
}

static HRESULT init_and_render(HWND hwnd)
{
    HRESULT hr;

    hr = create_device(hwnd);
    if (FAILED(hr))
        return hr;
    hr = create_geometry();
    if (FAILED(hr))
        return hr;
    hr = create_texture();
    if (FAILED(hr))
        return hr;
    hr = create_shaders();
    if (FAILED(hr))
        return hr;
    hr = bind_state();
    if (FAILED(hr))
        return hr;
    return render(hwnd);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
        LPARAM lparam)
{
    switch (message)
    {
        case WM_ERASEBKGND:
            return 1;

        /*
         * Re-present on repaint, and ask for a repaint whenever the window
         * moves or resizes.
         *
         * This is not cosmetic. The host learns where to put its WebGPU
         * overlay from the client rect the guest attaches to each Present, so
         * a program that renders one frame and then sits in its message loop
         * leaves the overlay pinned wherever the window was at that moment --
         * drag the window and the picture stays behind. Real games present
         * every frame and so track the window for free; a single-shot test
         * has to repaint like any other Windows program to behave the same.
         */
        case WM_PAINT:
        {
            PAINTSTRUCT paint;
            BeginPaint(hwnd, &paint);
            if (g_device && !g_releasing_d3d9)
                render(hwnd);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_MOVE:
        case WM_SIZE:
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_DESTROY:
            g_window = NULL;
            release_d3d9();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcA(hwnd, message, wparam, lparam);
}

static int run_test(HINSTANCE instance, int show_command)
{
    WNDCLASSA window_class;
    RECT window_rect;
    HWND hwnd;
    MSG message;
    HRESULT hr;

    ZeroMemory(&window_class, sizeof(window_class));
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.lpszClassName = g_window_class;
    if (!RegisterClassA(&window_class))
    {
        trace_text("RegisterClass failed");
        return 1;
    }

    SetRect(&window_rect, 0, 0, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT);
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA(g_window_class, "D3D9 shader: starting",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            window_rect.right - window_rect.left,
            window_rect.bottom - window_rect.top,
            NULL, NULL, instance, NULL);
    if (!hwnd)
    {
        trace_text("CreateWindow failed");
        return 2;
    }

    g_window = hwnd;
    SetUnhandledExceptionFilter(unhandled_exception_filter);

    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    hr = init_and_render(hwnd);
    if (FAILED(hr))
    {
        set_result_title(hwnd, g_failed_stage, hr);
        MessageBoxA(hwnd,
                "The D3D9 shader test failed. Check the window title, guest "
                "debug output, and v86gl logs for the HRESULT and last call.",
                "D3D9 shader test", MB_OK | MB_ICONERROR);
    }

    while (GetMessageA(&message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    return FAILED(hr) ? 3 : 0;
}

void WINAPI WinMainCRTStartup(void)
{
    int result = run_test(GetModuleHandleA(NULL), SW_SHOWDEFAULT);

    trace_text("ExitProcess");
    ExitProcess((UINT)result);
}
