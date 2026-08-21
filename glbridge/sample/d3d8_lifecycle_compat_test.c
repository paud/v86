// Direct3D 8 Stage 5 parent/refcount, swap-chain, surface and stress test.

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d8.h>

#define TEST_WIDTH  400
#define TEST_HEIGHT 300
#define STRESS_ITERATIONS 96
#define RESET_ITERATIONS 8

static const char g_class_name[] = "V86GLD3D8LifecycleCompatTest";
static HWND g_window;
static IDirect3D8 *g_d3d;
static IDirect3DDevice8 *g_device;
static D3DPRESENT_PARAMETERS g_present;

static HRESULT fail_stage(const char *stage, HRESULT hr)
{
    char title[250];
    char line[250];
    wsprintfA(line, "[d3d8-lifecycle-compat] %s -> 0x%08lX\r\n",
            stage, (unsigned long)hr);
    OutputDebugStringA(line);
    wsprintfA(title, "D3D8 lifecycle compatibility: %s (0x%08lX)",
            stage, (unsigned long)hr);
    if (g_window) SetWindowTextA(g_window, title);
    return hr;
}

static void begin_stage(const char *stage)
{
    char title[250];
    OutputDebugStringA("[d3d8-lifecycle-compat] ");
    OutputDebugStringA(stage);
    OutputDebugStringA("\r\n");
    wsprintfA(title, "D3D8 lifecycle compatibility: %s", stage);
    if (g_window) SetWindowTextA(g_window, title);
}

static HRESULT expect_invalid(HRESULT hr, const char *stage)
{
    return hr == D3DERR_INVALIDCALL ? D3D_OK : fail_stage(stage,
            SUCCEEDED(hr) ? E_FAIL : hr);
}

static HRESULT create_device(void)
{
    D3DDISPLAYMODE mode;
    HRESULT hr;
    g_d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!g_d3d) return fail_stage("Direct3DCreate8", E_FAIL);
    hr = IDirect3D8_GetAdapterDisplayMode(g_d3d, 0, &mode);
    if (FAILED(hr)) return fail_stage("GetAdapterDisplayMode", hr);
    ZeroMemory(&g_present, sizeof(g_present));
    g_present.BackBufferWidth = TEST_WIDTH;
    g_present.BackBufferHeight = TEST_HEIGHT;
    g_present.BackBufferFormat = mode.Format;
    g_present.BackBufferCount = 1;
    g_present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_present.hDeviceWindow = g_window;
    g_present.Windowed = TRUE;
    g_present.EnableAutoDepthStencil = TRUE;
    g_present.AutoDepthStencilFormat = D3DFMT_D24S8;
    hr = IDirect3D8_CreateDevice(g_d3d, 0, D3DDEVTYPE_HAL, g_window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_present, &g_device);
    return FAILED(hr) ? fail_stage("CreateDevice", hr) : hr;
}

static HRESULT test_additional_swapchain(void)
{
    D3DPRESENT_PARAMETERS pp = g_present;
    IDirect3DSwapChain8 *chain = NULL;
    IDirect3DSurface8 *backbuffer = NULL;
    IDirect3DDevice8 *parent = NULL;
    HRESULT hr;
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 192;
    begin_stage("additional swap chain parent and Reset blocker");
    hr = IDirect3DDevice8_CreateAdditionalSwapChain(g_device, &pp, &chain);
    if (FAILED(hr) || !chain)
        return fail_stage("CreateAdditionalSwapChain", FAILED(hr) ? hr : E_FAIL);
    hr = IDirect3DSwapChain8_GetBackBuffer(chain, 0,
            D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    if (FAILED(hr) || !backbuffer)
        goto failed;
    hr = IDirect3DSurface8_GetDevice(backbuffer, &parent);
    if (FAILED(hr) || parent != g_device) {
        hr = FAILED(hr) ? hr : E_FAIL;
        goto failed;
    }
    IDirect3DDevice8_Release(parent);
    parent = NULL;
    hr = expect_invalid(IDirect3DDevice8_Reset(g_device, &g_present),
            "Reset must reject a live additional swap chain/backbuffer");
    if (FAILED(hr)) goto failed;
    IDirect3DSurface8_Release(backbuffer);
    backbuffer = NULL;
    IDirect3DSwapChain8_Release(chain);
    return D3D_OK;
failed:
    if (parent) IDirect3DDevice8_Release(parent);
    if (backbuffer) IDirect3DSurface8_Release(backbuffer);
    if (chain) IDirect3DSwapChain8_Release(chain);
    return fail_stage("additional swap chain checks", hr);
}

static HRESULT verify_surface_color(IDirect3DSurface8 *surface,
        DWORD expected, const char *stage)
{
    D3DLOCKED_RECT lock;
    DWORD actual;
    HRESULT hr = IDirect3DSurface8_LockRect(surface, &lock, NULL,
            D3DLOCK_READONLY);
    if (FAILED(hr)) return fail_stage(stage, hr);
    actual = *(DWORD *)lock.pBits;
    hr = IDirect3DSurface8_UnlockRect(surface);
    if (FAILED(hr)) return fail_stage(stage, hr);
    return actual == expected ? D3D_OK : fail_stage(stage, E_FAIL);
}

static HRESULT test_surfaces(void)
{
    const DWORD offscreen_color = D3DCOLOR_ARGB(255, 22, 91, 203);
    const DWORD front_color = D3DCOLOR_ARGB(255, 12, 180, 74);
    IDirect3DSurface8 *default_target = NULL;
    IDirect3DSurface8 *target = NULL;
    IDirect3DSurface8 *depth = NULL;
    IDirect3DSurface8 *image = NULL;
    IDirect3DSurface8 *front = NULL;
    IDirect3DDevice8 *container_device = NULL;
    HRESULT hr;

    begin_stage("lockable render target, CopyRects and front-buffer mirror");
    hr = IDirect3DDevice8_GetRenderTarget(g_device, &default_target);
    if (FAILED(hr)) goto failed;
    hr = IDirect3DDevice8_CreateRenderTarget(g_device, 64, 64,
            D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, TRUE, &target);
    if (FAILED(hr)) goto failed;
    hr = IDirect3DDevice8_CreateDepthStencilSurface(g_device, 64, 64,
            D3DFMT_D24S8, D3DMULTISAMPLE_NONE, &depth);
    if (FAILED(hr)) goto failed;
    hr = IDirect3DDevice8_CreateImageSurface(g_device, 64, 64,
            D3DFMT_A8R8G8B8, &image);
    if (FAILED(hr)) goto failed;
    hr = IDirect3DSurface8_GetContainer(image, &IID_IDirect3DDevice8,
            (void **)&container_device);
    if (FAILED(hr) || container_device != g_device) {
        hr = FAILED(hr) ? hr : E_FAIL;
        goto failed;
    }
    IDirect3DDevice8_Release(container_device);
    container_device = NULL;
    hr = IDirect3DDevice8_SetRenderTarget(g_device, target, depth);
    if (FAILED(hr)) goto failed;
    hr = IDirect3DDevice8_Clear(g_device, 0, NULL,
            D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, offscreen_color, 1.0f, 0);
    if (FAILED(hr)) goto failed;
    hr = IDirect3DDevice8_CopyRects(g_device, target, NULL, 0,
            image, NULL);
    if (FAILED(hr)) goto failed;
    hr = verify_surface_color(image, offscreen_color,
            "lockable render-target CopyRects color");
    if (FAILED(hr)) goto failed;
    hr = IDirect3DDevice8_SetRenderTarget(g_device, default_target, NULL);
    if (FAILED(hr)) goto failed;
    hr = IDirect3DDevice8_Clear(g_device, 0, NULL, D3DCLEAR_TARGET,
            front_color, 1.0f, 0);
    if (FAILED(hr)) goto failed;
    hr = IDirect3DDevice8_CreateImageSurface(g_device, TEST_WIDTH,
            TEST_HEIGHT, g_present.BackBufferFormat, &front);
    if (FAILED(hr)) goto failed;
    hr = IDirect3DDevice8_GetFrontBuffer(g_device, front);
    if (FAILED(hr)) goto failed;
    if (g_present.BackBufferFormat == D3DFMT_A8R8G8B8
            || g_present.BackBufferFormat == D3DFMT_X8R8G8B8) {
        hr = verify_surface_color(front, front_color,
                "GetFrontBuffer clear/readback color");
        if (FAILED(hr)) goto failed;
    }
    IDirect3DSurface8_Release(front);
    IDirect3DSurface8_Release(image);
    IDirect3DSurface8_Release(depth);
    IDirect3DSurface8_Release(target);
    IDirect3DSurface8_Release(default_target);
    return D3D_OK;
failed:
    if (container_device) IDirect3DDevice8_Release(container_device);
    if (default_target)
        IDirect3DDevice8_SetRenderTarget(g_device, default_target, NULL);
    if (front) IDirect3DSurface8_Release(front);
    if (image) IDirect3DSurface8_Release(image);
    if (depth) IDirect3DSurface8_Release(depth);
    if (target) IDirect3DSurface8_Release(target);
    if (default_target) IDirect3DSurface8_Release(default_target);
    return fail_stage("surface compatibility checks", hr);
}

static HRESULT test_resource_stress_and_reset(void)
{
    IDirect3DTexture8 *survivor = NULL;
    UINT iteration;
    HRESULT hr;
    begin_stage("96 create/lock/container/destroy resource cycles");
    for (iteration = 0; iteration < STRESS_ITERATIONS; ++iteration) {
        IDirect3DTexture8 *texture = NULL;
        IDirect3DSurface8 *surface = NULL;
        IDirect3DTexture8 *container = NULL;
        IDirect3DVertexBuffer8 *buffer = NULL;
        D3DLOCKED_RECT lock;
        BYTE *bytes = NULL;
        hr = IDirect3DDevice8_CreateTexture(g_device, 8, 8, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture);
        if (FAILED(hr)) return fail_stage("stress CreateTexture", hr);
        hr = IDirect3DTexture8_LockRect(texture, 0, &lock, NULL, 0);
        if (SUCCEEDED(hr)) {
            FillMemory(lock.pBits, 8 * lock.Pitch, (BYTE)iteration);
            hr = IDirect3DTexture8_UnlockRect(texture, 0);
        }
        if (FAILED(hr)) return fail_stage("stress texture Lock/Unlock", hr);
        hr = IDirect3DTexture8_GetSurfaceLevel(texture, 0, &surface);
        if (FAILED(hr)) return fail_stage("stress GetSurfaceLevel", hr);
        hr = IDirect3DSurface8_GetContainer(surface, &IID_IDirect3DTexture8,
                (void **)&container);
        if (FAILED(hr) || container != texture)
            return fail_stage("surface GetContainer texture identity",
                    FAILED(hr) ? hr : E_FAIL);
        IDirect3DTexture8_Release(container);
        IDirect3DSurface8_Release(surface);
        if (IDirect3DTexture8_Release(texture) != 0)
            return fail_stage("stress texture refcount did not reach zero", E_FAIL);
        hr = IDirect3DDevice8_CreateVertexBuffer(g_device, 256, 0, 0,
                D3DPOOL_MANAGED, &buffer);
        if (FAILED(hr)) return fail_stage("stress CreateVertexBuffer", hr);
        hr = IDirect3DVertexBuffer8_Lock(buffer, 0, 0, &bytes, 0);
        if (SUCCEEDED(hr)) {
            FillMemory(bytes, 256, (BYTE)(iteration ^ 0x5a));
            hr = IDirect3DVertexBuffer8_Unlock(buffer);
        }
        if (FAILED(hr)) return fail_stage("stress buffer Lock/Unlock", hr);
        if (IDirect3DVertexBuffer8_Release(buffer) != 0)
            return fail_stage("stress buffer refcount did not reach zero", E_FAIL);
    }

    hr = IDirect3DDevice8_CreateTexture(g_device, 16, 16, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &survivor);
    if (FAILED(hr)) return fail_stage("managed survivor CreateTexture", hr);
    begin_stage("8 repeated Reset epochs with managed resource rebuild");
    for (iteration = 0; iteration < RESET_ITERATIONS; ++iteration) {
        D3DSURFACE_DESC desc;
        g_present.BackBufferWidth = (iteration & 1) ? TEST_WIDTH : 480;
        g_present.BackBufferHeight = (iteration & 1) ? TEST_HEIGHT : 320;
        hr = IDirect3DDevice8_Reset(g_device, &g_present);
        if (FAILED(hr)) return fail_stage("repeated Reset", hr);
        hr = IDirect3DTexture8_GetLevelDesc(survivor, 0, &desc);
        if (FAILED(hr) || desc.Width != 16 || desc.Height != 16)
            return fail_stage("managed resource lost after Reset",
                    FAILED(hr) ? hr : E_FAIL);
        hr = IDirect3DDevice8_Clear(g_device, 0, NULL, D3DCLEAR_TARGET,
                D3DCOLOR_XRGB(10 + iteration * 12, 32, 90), 1.0f, 0);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice8_Present(g_device,
                NULL, NULL, NULL, NULL);
        if (FAILED(hr)) return fail_stage("draw after repeated Reset", hr);
    }
    if (IDirect3DTexture8_Release(survivor) != 0)
        return fail_stage("managed survivor refcount did not reach zero", E_FAIL);
    return D3D_OK;
}

static HRESULT test_invalid_boundaries(void)
{
    IDirect3DSurface8 *surface = (IDirect3DSurface8 *)(ULONG_PTR)1;
    HRESULT hr;
    begin_stage("invalid parameter and HRESULT boundaries");
    hr = expect_invalid(IDirect3DDevice8_CreateImageSurface(g_device, 0, 16,
            D3DFMT_A8R8G8B8, &surface), "zero-width image surface");
    if (FAILED(hr) || surface != NULL)
        return fail_stage("failed create must null the output", E_FAIL);
    hr = expect_invalid(IDirect3DDevice8_GetBackBuffer(g_device, 1,
            D3DBACKBUFFER_TYPE_MONO, &surface), "invalid backbuffer index");
    return FAILED(hr) ? hr : D3D_OK;
}

static HRESULT test_bound_resource_device_release(void)
{
    IDirect3DTexture8 *texture = NULL;
    ULONG refs;
    HRESULT hr;
    begin_stage("bound-resource/device COM cycle collection");
    hr = IDirect3DDevice8_CreateTexture(g_device, 8, 8, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture);
    if (FAILED(hr)) return fail_stage("cycle CreateTexture", hr);
    hr = IDirect3DDevice8_SetTexture(g_device, 0,
            (IDirect3DBaseTexture8 *)texture);
    if (FAILED(hr)) {
        IDirect3DTexture8_Release(texture);
        return fail_stage("cycle SetTexture", hr);
    }
    refs = IDirect3DTexture8_Release(texture);
    if (refs != 1)
        return fail_stage("SetTexture must retain one texture reference",
                E_FAIL);
    refs = IDirect3DDevice8_Release(g_device);
    g_device = NULL;
    return refs == 0 ? D3D_OK
            : fail_stage("final device Release must collect bound state",
                    E_FAIL);
}

static void release_all(void)
{
    if (g_device) {
        IDirect3DDevice8_Release(g_device);
        g_device = NULL;
    }
    if (g_d3d) {
        IDirect3D8_Release(g_d3d);
        g_d3d = NULL;
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message,
        WPARAM wparam, LPARAM lparam)
{
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous,
        LPSTR command_line, int show_command)
{
    WNDCLASSA wc;
    RECT rect = {0, 0, TEST_WIDTH, TEST_HEIGHT};
    MSG message;
    HRESULT hr;
    (void)previous; (void)command_line; (void)show_command;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = g_class_name;
    if (!RegisterClassA(&wc)) return 1;
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    g_window = CreateWindowA(g_class_name, "D3D8 lifecycle: starting",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            NULL, NULL, instance, NULL);
    if (!g_window) return 1;
    hr = create_device();
    if (SUCCEEDED(hr)) hr = test_additional_swapchain();
    if (SUCCEEDED(hr)) hr = test_surfaces();
    if (SUCCEEDED(hr)) hr = test_resource_stress_and_reset();
    if (SUCCEEDED(hr)) hr = test_invalid_boundaries();
    if (SUCCEEDED(hr)) hr = test_bound_resource_device_release();
    if (SUCCEEDED(hr)) SetWindowTextA(g_window,
            "D3D8 lifecycle compatibility: PASS - swapchain | surfaces | 96x stress | 8x Reset | refcycle");
    else MessageBoxA(g_window, "Stage 5 lifecycle compatibility test failed.",
            "D3D8 lifecycle compatibility", MB_OK | MB_ICONERROR);
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    release_all();
    return FAILED(hr) ? 1 : 0;
}

void WINAPI WinMainCRTStartup(void)
{
    ExitProcess((UINT)WinMain(GetModuleHandleA(NULL), NULL,
            GetCommandLineA(), SW_SHOWDEFAULT));
}
