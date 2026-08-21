// Direct3D 9 world-space transform test for the v86 WebGPU bridge.
//
// This is the test after d3d9_texture_test.c. It exercises the fixed
// pipeline's *other* vertex path: non-pretransformed POSITION vertices run
// through SetTransform(WORLD/VIEW/PROJECTION), rather than the screen-space
// XYZRHW path the triangle/texture tests use. It also uses a real
// IDirect3DVertexDeclaration9 (CreateVertexDeclaration/SetVertexDeclaration)
// instead of SetFVF, covering the other legacy vertex-format entry point.
//
// The WORLD matrix here is a non-trivial scale + translation (not identity),
// so a wrong multiply order or a missing transpose in the host's WGSL
// uniform upload (d3d9_executor.js transpose4x4()/wvp()) shows up as an
// obviously wrong triangle position/shape rather than passing by accident.
//
// Build for Windows XP as documented in ../d3d9proxy/README.md. The command
// uses a 32-bit MinGW compiler and avoids the MinGW C runtime.

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>

#define TEST_CLIENT_WIDTH  640
#define TEST_CLIENT_HEIGHT 480

typedef struct TestVertex
{
    FLOAT x;
    FLOAT y;
    FLOAT z;
    DWORD color;
} TestVertex;

static const char g_window_class[] = "V86GLD3D9WorldTest";
/* A triangle spanning most of clip space at identity transform; the WORLD
 * matrix below scales and offsets it so the effect is visible. */
static const TestVertex g_vertices[] =
{
    {-1.0f, -1.0f, 0.0f, D3DCOLOR_XRGB(255,   0,   0)},
    { 1.0f, -1.0f, 0.0f, D3DCOLOR_XRGB(  0, 255,   0)},
    { 0.0f,  1.0f, 0.0f, D3DCOLOR_XRGB(  0,   0, 255)},
};
static const D3DVERTEXELEMENT9 g_declaration[] =
{
    { 0, 0,  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
    D3DDECL_END()
};

static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_device;
static IDirect3DVertexBuffer9 *g_vertex_buffer;
static IDirect3DVertexDeclaration9 *g_declaration_object;
static HWND g_window;
static const char *g_failed_stage = "unknown stage";
static BOOL g_teardown_started;
static BOOL g_releasing_d3d9;

static void trace_text(const char *text)
{
    OutputDebugStringA("[d3d9-world] ");
    OutputDebugStringA(text);
    OutputDebugStringA("\r\n");
}

static void trace_hresult(const char *stage, HRESULT hr)
{
    char line[192];

    wsprintfA(line, "[d3d9-world] %s -> 0x%08lX\r\n",
            stage, (unsigned long)hr);
    OutputDebugStringA(line);
}

static void set_result_title(HWND hwnd, const char *stage, HRESULT hr)
{
    char title[192];

    wsprintfA(title, "D3D9 world: %s (0x%08lX)",
            stage, (unsigned long)hr);
    SetWindowTextA(hwnd, title);
}

static LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS *exception)
{
    DWORD code = exception && exception->ExceptionRecord ?
            exception->ExceptionRecord->ExceptionCode : 0xE0000001u;
    char line[192];

    wsprintfA(line, "UNHANDLED 0x%08lX during %s",
            (unsigned long)code, g_failed_stage);
    trace_text(line);
    if (g_window && !g_teardown_started)
        set_result_title(g_window, line, (HRESULT)code);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void begin_stage(const char *stage)
{
    char title[192];

    g_failed_stage = stage;
    trace_text(stage);
    wsprintfA(title, "D3D9 world: calling %s", stage);
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

static void begin_teardown_stage(const char *stage)
{
    g_failed_stage = stage;
    trace_text(stage);
}

static void trace_release_result(const char *stage, ULONG refcount)
{
    trace_hresult(stage, (HRESULT)refcount);
}

static void release_d3d9(void)
{
    ULONG refcount;

    if (g_releasing_d3d9)
        return;

    g_teardown_started = TRUE;
    g_releasing_d3d9 = TRUE;

    if (g_declaration_object)
    {
        IDirect3DVertexDeclaration9 *declaration = g_declaration_object;

        g_declaration_object = NULL;
        begin_teardown_stage("VertexDeclaration::Release");
        refcount = IDirect3DVertexDeclaration9_Release(declaration);
        trace_release_result("VertexDeclaration::Release", refcount);
    }

    if (g_vertex_buffer)
    {
        IDirect3DVertexBuffer9 *vertex_buffer = g_vertex_buffer;

        g_vertex_buffer = NULL;
        begin_teardown_stage("VertexBuffer::Release");
        refcount = IDirect3DVertexBuffer9_Release(vertex_buffer);
        trace_release_result("VertexBuffer::Release", refcount);
    }

    if (g_device)
    {
        IDirect3DDevice9 *device = g_device;

        g_device = NULL;
        begin_teardown_stage("Device::Release");
        refcount = IDirect3DDevice9_Release(device);
        trace_release_result("Device::Release", refcount);
    }

    if (g_d3d)
    {
        IDirect3D9 *d3d = g_d3d;

        g_d3d = NULL;
        begin_teardown_stage("Direct3D9::Release");
        refcount = IDirect3D9_Release(d3d);
        trace_release_result("Direct3D9::Release", refcount);
    }

    begin_teardown_stage("teardown complete");
    g_releasing_d3d9 = FALSE;
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
    trace_hresult("Direct3DCreate9", D3D_OK);

    ZeroMemory(&mode, sizeof(mode));
    begin_stage("GetAdapterDisplayMode");
    hr = IDirect3D9_GetAdapterDisplayMode(g_d3d, D3DADAPTER_DEFAULT, &mode);
    if (FAILED(hr))
        return failed("GetAdapterDisplayMode", hr);
    trace_hresult("GetAdapterDisplayMode", hr);

    begin_stage("CheckDeviceType");
    hr = IDirect3D9_CheckDeviceType(g_d3d, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, mode.Format, mode.Format, TRUE);
    if (FAILED(hr))
        return failed("CheckDeviceType", hr);
    trace_hresult("CheckDeviceType(windowed HAL)", hr);

    ZeroMemory(&present_parameters, sizeof(present_parameters));
    present_parameters.BackBufferWidth = TEST_CLIENT_WIDTH;
    present_parameters.BackBufferHeight = TEST_CLIENT_HEIGHT;
    present_parameters.BackBufferFormat = mode.Format;
    present_parameters.BackBufferCount = 1;
    present_parameters.MultiSampleType = D3DMULTISAMPLE_NONE;
    present_parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present_parameters.hDeviceWindow = hwnd;
    present_parameters.Windowed = TRUE;
    present_parameters.EnableAutoDepthStencil = FALSE;
    present_parameters.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

    begin_stage("CreateDevice");
    hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
            &present_parameters, &g_device);
    if (FAILED(hr))
        return failed("CreateDevice", hr);
    trace_hresult("CreateDevice(windowed, software VP, no MSAA)", hr);

    return D3D_OK;
}

static HRESULT create_triangle_resources(void)
{
    void *destination;
    HRESULT hr;

    begin_stage("CreateVertexDeclaration");
    hr = IDirect3DDevice9_CreateVertexDeclaration(g_device, g_declaration,
            &g_declaration_object);
    if (FAILED(hr))
        return failed("CreateVertexDeclaration", hr);
    trace_hresult("CreateVertexDeclaration(POSITION+COLOR)", hr);

    begin_stage("SetVertexDeclaration");
    hr = IDirect3DDevice9_SetVertexDeclaration(g_device, g_declaration_object);
    if (FAILED(hr))
        return failed("SetVertexDeclaration", hr);
    trace_hresult("SetVertexDeclaration", hr);

    begin_stage("CreateVertexBuffer");
    hr = IDirect3DDevice9_CreateVertexBuffer(g_device, sizeof(g_vertices),
            D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &g_vertex_buffer, NULL);
    if (FAILED(hr))
        return failed("CreateVertexBuffer", hr);
    trace_hresult("CreateVertexBuffer", hr);

    destination = NULL;
    begin_stage("VertexBuffer::Lock");
    hr = IDirect3DVertexBuffer9_Lock(g_vertex_buffer, 0,
            sizeof(g_vertices), &destination, 0);
    if (FAILED(hr))
        return failed("VertexBuffer::Lock", hr);
    CopyMemory(destination, g_vertices, sizeof(g_vertices));
    begin_stage("VertexBuffer::Unlock");
    hr = IDirect3DVertexBuffer9_Unlock(g_vertex_buffer);
    if (FAILED(hr))
        return failed("VertexBuffer::Unlock", hr);
    trace_hresult("VertexBuffer::Lock/Unlock", hr);

    begin_stage("SetRenderState LIGHTING");
    hr = IDirect3DDevice9_SetRenderState(g_device, D3DRS_LIGHTING, FALSE);
    if (FAILED(hr))
        return failed("SetRenderState(LIGHTING=FALSE)", hr);
    trace_hresult("SetRenderState(LIGHTING=FALSE)", hr);

    begin_stage("SetRenderState CULLMODE");
    hr = IDirect3DDevice9_SetRenderState(g_device, D3DRS_CULLMODE,
            D3DCULL_NONE);
    if (FAILED(hr))
        return failed("SetRenderState(CULLMODE=NONE)", hr);
    trace_hresult("SetRenderState(CULLMODE=NONE)", hr);

    begin_stage("SetStreamSource");
    hr = IDirect3DDevice9_SetStreamSource(g_device, 0, g_vertex_buffer, 0,
            sizeof(TestVertex));
    if (FAILED(hr))
        return failed("SetStreamSource", hr);
    trace_hresult("SetStreamSource(stream 0)", hr);

    return D3D_OK;
}

static HRESULT set_transforms(void)
{
    D3DMATRIX world, view, projection;
    HRESULT hr;

    ZeroMemory(&world, sizeof(world));
    world.m[0][0] = 0.6f; /* scale X */
    world.m[1][1] = 0.6f; /* scale Y */
    world.m[2][2] = 1.0f;
    world.m[3][0] = 0.2f; /* translate X */
    world.m[3][1] = -0.1f; /* translate Y */
    world.m[3][3] = 1.0f;

    ZeroMemory(&view, sizeof(view));
    view.m[0][0] = view.m[1][1] = view.m[2][2] = view.m[3][3] = 1.0f;

    ZeroMemory(&projection, sizeof(projection));
    projection.m[0][0] = projection.m[1][1] = projection.m[2][2] =
            projection.m[3][3] = 1.0f;

    begin_stage("SetTransform WORLD");
    hr = IDirect3DDevice9_SetTransform(g_device, D3DTS_WORLD, &world);
    if (FAILED(hr))
        return failed("SetTransform(WORLD)", hr);
    trace_hresult("SetTransform(WORLD scale 0.6 + translate)", hr);

    begin_stage("SetTransform VIEW");
    hr = IDirect3DDevice9_SetTransform(g_device, D3DTS_VIEW, &view);
    if (FAILED(hr))
        return failed("SetTransform(VIEW)", hr);
    trace_hresult("SetTransform(VIEW identity)", hr);

    begin_stage("SetTransform PROJECTION");
    hr = IDirect3DDevice9_SetTransform(g_device, D3DTS_PROJECTION,
            &projection);
    if (FAILED(hr))
        return failed("SetTransform(PROJECTION)", hr);
    trace_hresult("SetTransform(PROJECTION identity)", hr);

    return D3D_OK;
}

static HRESULT render_triangle(HWND hwnd)
{
    HRESULT hr;

    begin_stage("Clear");
    hr = IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET,
            D3DCOLOR_XRGB(0, 0, 40), 1.0f, 0);
    if (FAILED(hr))
        return failed("Clear", hr);
    trace_hresult("Clear(dark blue)", hr);

    begin_stage("BeginScene");
    hr = IDirect3DDevice9_BeginScene(g_device);
    if (FAILED(hr))
        return failed("BeginScene", hr);

    begin_stage("DrawPrimitive");
    hr = IDirect3DDevice9_DrawPrimitive(g_device, D3DPT_TRIANGLELIST, 0, 1);
    if (FAILED(hr))
    {
        IDirect3DDevice9_EndScene(g_device);
        return failed("DrawPrimitive", hr);
    }
    trace_hresult("DrawPrimitive(TRIANGLELIST, 1 primitive)", hr);

    begin_stage("EndScene");
    hr = IDirect3DDevice9_EndScene(g_device);
    if (FAILED(hr))
        return failed("EndScene", hr);

    begin_stage("Present");
    hr = IDirect3DDevice9_Present(g_device, NULL, NULL, NULL, NULL);
    if (FAILED(hr))
        return failed("Present", hr);
    trace_hresult("Present", hr);

    set_result_title(hwnd,
            "Present S_OK - expected small scaled/offset RGB triangle", hr);
    return hr;
}

static HRESULT init_and_render(HWND hwnd)
{
    HRESULT hr;

    hr = create_device(hwnd);
    if (FAILED(hr))
        return hr;

    hr = create_triangle_resources();
    if (FAILED(hr))
        return hr;

    hr = set_transforms();
    if (FAILED(hr))
        return hr;

    return render_triangle(hwnd);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
        LPARAM lparam)
{
    switch (message)
    {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT paint;
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            return 0;
        }

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
    hwnd = CreateWindowA(g_window_class, "D3D9 world: starting",
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
                "The D3D9 world test failed. Check the window title, "
                "guest debug output, and v86gl logs for the HRESULT and "
                "last call.",
                "D3D9 world test", MB_OK | MB_ICONERROR);
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

    begin_teardown_stage("ExitProcess");
    ExitProcess((UINT)result);
}
