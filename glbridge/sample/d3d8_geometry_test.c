// Direct3D 8 WebGPU geometry milestone smoke test.
//
// Expected output: four colour-interpolated panels on black. The calls cover
// resident indexed drawing, resident triangle-fan conversion, DrawPrimitiveUP,
// and DrawIndexedPrimitiveUP without textures, transforms, depth, or shaders.

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d8.h>

#define CLIENT_WIDTH  640
#define CLIENT_HEIGHT 480
#define TEST_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

typedef struct TestVertex {
    FLOAT x;
    FLOAT y;
    FLOAT z;
    FLOAT rhw;
    DWORD color;
} TestVertex;

static const char g_window_class[] = "D8WGGeometryTest";
static IDirect3D8 *g_d3d;
static IDirect3DDevice8 *g_device;
static IDirect3DVertexBuffer8 *g_vertex_buffer;
static IDirect3DIndexBuffer8 *g_index_buffer;
static HWND g_window;

static const TestVertex g_resident_vertices[] = {
    { 40,  40, 0.5f, 1, 0xFFFF0000},
    {280,  40, 0.5f, 1, 0xFF00FF00},
    {280, 210, 0.5f, 1, 0xFF0000FF},
    { 40, 210, 0.5f, 1, 0xFFFFFF00},
    {360, 270, 0.5f, 1, 0xFFFF00FF},
    {600, 270, 0.5f, 1, 0xFF00FFFF},
    {600, 440, 0.5f, 1, 0xFFFFFFFF},
    {360, 440, 0.5f, 1, 0xFFFF8000},
};

static const WORD g_resident_indices[] = {0, 1, 2, 0, 2, 3};

static const TestVertex g_up_vertices[] = {
    {360,  40, 0.5f, 1, 0xFFFF0000},
    {600,  40, 0.5f, 1, 0xFF00FF00},
    {600, 210, 0.5f, 1, 0xFF0000FF},
    {360, 210, 0.5f, 1, 0xFFFFFF00},
};

static const TestVertex g_indexed_up_vertices[] = {
    { 40, 270, 0.5f, 1, 0xFFFF00FF},
    {280, 270, 0.5f, 1, 0xFF00FFFF},
    {280, 440, 0.5f, 1, 0xFFFFFFFF},
    { 40, 440, 0.5f, 1, 0xFFFF8000},
};
static const WORD g_indexed_up_indices[] = {0, 1, 2, 3};

static void log_stage(const char *stage, HRESULT hr)
{
    char line[192];
    wsprintfA(line, "[d3d8-geometry] %s -> 0x%08lX\r\n",
            stage, (unsigned long)hr);
    OutputDebugStringA(line);
    if (g_window) {
        wsprintfA(line, "D3D8 geometry: %s (0x%08lX)",
                stage, (unsigned long)hr);
        SetWindowTextA(g_window, line);
    }
}

static HRESULT checked(const char *stage, HRESULT hr)
{
    log_stage(stage, hr);
    return hr;
}

static void release_objects(void)
{
    if (g_device) {
        IDirect3DDevice8_SetStreamSource(g_device, 0, NULL, 0);
        IDirect3DDevice8_SetIndices(g_device, NULL, 0);
    }
    if (g_index_buffer) {
        IDirect3DIndexBuffer8_Release(g_index_buffer);
        g_index_buffer = NULL;
    }
    if (g_vertex_buffer) {
        IDirect3DVertexBuffer8_Release(g_vertex_buffer);
        g_vertex_buffer = NULL;
    }
    if (g_device) {
        IDirect3DDevice8_Release(g_device);
        g_device = NULL;
    }
    if (g_d3d) {
        IDirect3D8_Release(g_d3d);
        g_d3d = NULL;
    }
}

static HRESULT create_device(HWND hwnd)
{
    D3DDISPLAYMODE mode;
    D3DPRESENT_PARAMETERS present;
    HRESULT hr;

    g_d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!g_d3d)
        return E_FAIL;
    ZeroMemory(&mode, sizeof(mode));
    hr = checked("GetAdapterDisplayMode",
            IDirect3D8_GetAdapterDisplayMode(g_d3d, D3DADAPTER_DEFAULT,
                    &mode));
    if (FAILED(hr)) return hr;

    ZeroMemory(&present, sizeof(present));
    present.BackBufferWidth = CLIENT_WIDTH;
    present.BackBufferHeight = CLIENT_HEIGHT;
    present.BackBufferFormat = mode.Format;
    present.BackBufferCount = 1;
    present.MultiSampleType = D3DMULTISAMPLE_NONE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = hwnd;
    present.Windowed = TRUE;
    present.EnableAutoDepthStencil = FALSE;
    present.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    return checked("CreateDevice", IDirect3D8_CreateDevice(g_d3d,
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present, &g_device));
}

static HRESULT create_buffers(void)
{
    BYTE *destination;
    HRESULT hr;

    hr = checked("CreateVertexBuffer", IDirect3DDevice8_CreateVertexBuffer(
            g_device, sizeof(g_resident_vertices), D3DUSAGE_WRITEONLY,
            TEST_FVF, D3DPOOL_DEFAULT, &g_vertex_buffer));
    if (FAILED(hr)) return hr;
    hr = checked("VertexBuffer Lock", IDirect3DVertexBuffer8_Lock(
            g_vertex_buffer, 0, sizeof(g_resident_vertices), &destination, 0));
    if (FAILED(hr)) return hr;
    CopyMemory(destination, g_resident_vertices, sizeof(g_resident_vertices));
    hr = checked("VertexBuffer Unlock",
            IDirect3DVertexBuffer8_Unlock(g_vertex_buffer));
    if (FAILED(hr)) return hr;

    hr = checked("CreateIndexBuffer", IDirect3DDevice8_CreateIndexBuffer(
            g_device, sizeof(g_resident_indices), D3DUSAGE_WRITEONLY,
            D3DFMT_INDEX16, D3DPOOL_DEFAULT, &g_index_buffer));
    if (FAILED(hr)) return hr;
    hr = checked("IndexBuffer Lock", IDirect3DIndexBuffer8_Lock(
            g_index_buffer, 0, sizeof(g_resident_indices), &destination, 0));
    if (FAILED(hr)) return hr;
    CopyMemory(destination, g_resident_indices, sizeof(g_resident_indices));
    return checked("IndexBuffer Unlock",
            IDirect3DIndexBuffer8_Unlock(g_index_buffer));
}

static HRESULT configure_pipeline(void)
{
    HRESULT hr;

    hr = IDirect3DDevice8_SetRenderState(g_device, D3DRS_LIGHTING, FALSE);
    if (FAILED(hr)) return checked("disable lighting", hr);
    hr = IDirect3DDevice8_SetRenderState(g_device, D3DRS_ZENABLE, FALSE);
    if (FAILED(hr)) return checked("disable depth", hr);
    hr = IDirect3DDevice8_SetRenderState(g_device, D3DRS_CULLMODE,
            D3DCULL_NONE);
    if (FAILED(hr)) return checked("disable culling", hr);
    hr = IDirect3DDevice8_SetTextureStageState(g_device, 0,
            D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    if (FAILED(hr)) return checked("COLOROP", hr);
    hr = IDirect3DDevice8_SetTextureStageState(g_device, 0,
            D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    if (FAILED(hr)) return checked("COLORARG1", hr);
    hr = IDirect3DDevice8_SetTextureStageState(g_device, 0,
            D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    if (FAILED(hr)) return checked("ALPHAOP", hr);
    hr = IDirect3DDevice8_SetTextureStageState(g_device, 0,
            D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    if (FAILED(hr)) return checked("ALPHAARG1", hr);
    hr = IDirect3DDevice8_SetVertexShader(g_device, TEST_FVF);
    return checked("SetVertexShader FVF", hr);
}

static HRESULT render_geometry(void)
{
    IDirect3DVertexBuffer8 *stream_after_up = (IDirect3DVertexBuffer8 *)1;
    IDirect3DIndexBuffer8 *indices_after_up = (IDirect3DIndexBuffer8 *)1;
    UINT stride_after_up = 0xFFFFFFFFu;
    UINT base_after_up = 0xFFFFFFFFu;
    HRESULT hr;

    hr = checked("Clear", IDirect3DDevice8_Clear(g_device, 0, NULL,
            D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0));
    if (FAILED(hr)) return hr;
    hr = checked("BeginScene", IDirect3DDevice8_BeginScene(g_device));
    if (FAILED(hr)) return hr;

    hr = checked("SetStreamSource", IDirect3DDevice8_SetStreamSource(g_device,
            0, g_vertex_buffer, sizeof(TestVertex)));
    if (FAILED(hr)) goto end_scene;
    hr = checked("SetIndices", IDirect3DDevice8_SetIndices(g_device,
            g_index_buffer, 0));
    if (FAILED(hr)) goto end_scene;
    hr = checked("DrawIndexedPrimitive", IDirect3DDevice8_DrawIndexedPrimitive(
            g_device, D3DPT_TRIANGLELIST, 0, 4, 0, 2));
    if (FAILED(hr)) goto end_scene;
    hr = checked("DrawPrimitive triangle fan",
            IDirect3DDevice8_DrawPrimitive(g_device, D3DPT_TRIANGLEFAN, 4, 2));
    if (FAILED(hr)) goto end_scene;
    hr = checked("DrawPrimitiveUP triangle fan",
            IDirect3DDevice8_DrawPrimitiveUP(g_device, D3DPT_TRIANGLEFAN, 2,
                    g_up_vertices, sizeof(TestVertex)));
    if (FAILED(hr)) goto end_scene;
    hr = checked("DrawIndexedPrimitiveUP triangle fan",
            IDirect3DDevice8_DrawIndexedPrimitiveUP(g_device,
                    D3DPT_TRIANGLEFAN, 0, 4, 2, g_indexed_up_indices,
                    D3DFMT_INDEX16, g_indexed_up_vertices,
                    sizeof(TestVertex)));
    if (FAILED(hr)) goto end_scene;

    hr = checked("GetStreamSource after UP",
            IDirect3DDevice8_GetStreamSource(g_device, 0, &stream_after_up,
                    &stride_after_up));
    if (FAILED(hr)) goto end_scene;
    if (stream_after_up || stride_after_up != 0) {
        if (stream_after_up) IDirect3DVertexBuffer8_Release(stream_after_up);
        hr = D3DERR_DRIVERINTERNALERROR;
        checked("UP did not clear stream 0", hr);
        goto end_scene;
    }
    hr = checked("GetIndices after indexed UP",
            IDirect3DDevice8_GetIndices(g_device, &indices_after_up,
                    &base_after_up));
    if (FAILED(hr)) goto end_scene;
    if (indices_after_up || base_after_up != 0) {
        if (indices_after_up) IDirect3DIndexBuffer8_Release(indices_after_up);
        hr = D3DERR_DRIVERINTERNALERROR;
        checked("indexed UP did not clear indices", hr);
        goto end_scene;
    }

end_scene:
    {
        HRESULT end_hr = checked("EndScene",
                IDirect3DDevice8_EndScene(g_device));
        if (SUCCEEDED(hr)) hr = end_hr;
    }
    if (FAILED(hr)) return hr;
    return checked("Present", IDirect3DDevice8_Present(g_device,
            NULL, NULL, NULL, NULL));
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
        LPARAM lparam)
{
    (void)wparam;
    (void)lparam;
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_DESTROY) {
        release_objects();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line,
        int show_command)
{
    WNDCLASSA window_class;
    RECT rect;
    MSG message;
    HRESULT hr;
    (void)previous;
    (void)command_line;

    ZeroMemory(&window_class, sizeof(window_class));
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.lpszClassName = g_window_class;
    if (!RegisterClassA(&window_class))
        return 1;
    SetRect(&rect, 0, 0, CLIENT_WIDTH, CLIENT_HEIGHT);
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    g_window = CreateWindowA(g_window_class, "D3D8 geometry: starting",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            NULL, NULL, instance, NULL);
    if (!g_window)
        return 2;
    ShowWindow(g_window, show_command);
    UpdateWindow(g_window);

    hr = create_device(g_window);
    if (SUCCEEDED(hr)) hr = create_buffers();
    if (SUCCEEDED(hr)) hr = configure_pipeline();
    if (SUCCEEDED(hr)) hr = render_geometry();
    if (SUCCEEDED(hr)) {
        SetWindowTextA(g_window,
                "D3D8 geometry PASS - expected four coloured panels");
    }

    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return FAILED(hr) ? 3 : 0;
}

void WINAPI WinMainCRTStartup(void)
{
    HINSTANCE instance = GetModuleHandleA(NULL);
    int show_command = SW_SHOWNORMAL;
    STARTUPINFOA startup;
    int result;

    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    GetStartupInfoA(&startup);
    if (startup.dwFlags & STARTF_USESHOWWINDOW)
        show_command = startup.wShowWindow;
    result = WinMain(instance, NULL, GetCommandLineA(), show_command);
    ExitProcess((UINT)result);
}
