// WGL per-thread current-context regression for the v86 OpenGL proxy.
//
// WineD3D 1.7.52 creates one shared HGLRC per D3D calling thread and keeps
// each context current after context_release(). A process-global current
// context therefore rejects the second thread even when D3D calls are
// serialized. This test keeps ctxA current on the main thread while ctxB is
// made current on a worker thread and checks both threads' logical bindings.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

static const char g_window_class[] = "V86GLWGLThreadCurrentTest";
static HINSTANCE g_instance;
static HWND g_main_window;
static HDC g_main_dc;
static HGLRC g_main_context;
static HWND g_worker_window;
static HDC g_worker_dc;
static HGLRC g_worker_context;
static HANDLE g_worker_ready;
static HANDLE g_worker_continue;
static DWORD g_worker_failure;

static void trace_text(const char *text)
{
    OutputDebugStringA("[wgl-thread-current] ");
    OutputDebugStringA(text);
    OutputDebugStringA("\r\n");
}

static void fail_worker(DWORD stage)
{
    g_worker_failure = stage;
    SetEvent(g_worker_ready);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message,
        WPARAM wparam, LPARAM lparam)
{
    if (message == WM_DESTROY)
    {
        if (hwnd == g_main_window) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

static BOOL set_default_pixel_format(HDC dc)
{
    PIXELFORMATDESCRIPTOR pfd;
    int format;

    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL
            | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    format = ChoosePixelFormat(dc, &pfd);
    if (!format) return FALSE;
    if (!DescribePixelFormat(dc, format, sizeof(pfd), &pfd)) return FALSE;
    return SetPixelFormat(dc, format, &pfd);
}

static DWORD WINAPI worker_proc(void *parameter)
{
    (void)parameter;
    g_worker_window = CreateWindowA(g_window_class, "WGL worker",
            WS_OVERLAPPEDWINDOW, 40, 40, 320, 240,
            NULL, NULL, g_instance, NULL);
    if (!g_worker_window) { fail_worker(101); return 101; }
    g_worker_dc = GetDC(g_worker_window);
    if (!g_worker_dc) { fail_worker(102); return 102; }
    if (!set_default_pixel_format(g_worker_dc))
    {
        fail_worker(103);
        return 103;
    }
    g_worker_context = wglCreateContext(g_worker_dc);
    if (!g_worker_context) { fail_worker(104); return 104; }
    if (!wglShareLists(g_main_context, g_worker_context))
    {
        fail_worker(105);
        return 105;
    }

    /* This is the call that the old process-global proxy rejected with
     * ERROR_BUSY while the main thread kept g_main_context current. */
    if (!wglMakeCurrent(g_worker_dc, g_worker_context))
    {
        fail_worker(106);
        return 106;
    }
    if (wglGetCurrentContext() != g_worker_context)
    {
        fail_worker(107);
        return 107;
    }
    if (wglGetCurrentDC() != g_worker_dc)
    {
        fail_worker(108);
        return 108;
    }

    trace_text("worker ctxB current while main ctxA remains current");
    SetEvent(g_worker_ready);
    if (WaitForSingleObject(g_worker_continue, 10000) != WAIT_OBJECT_0)
    {
        g_worker_failure = 109;
    }
    if (wglGetCurrentContext() != g_worker_context)
    {
        g_worker_failure = 110;
    }
    if (!wglMakeCurrent(NULL, NULL))
    {
        g_worker_failure = 111;
    }
    if (!wglDeleteContext(g_worker_context))
    {
        g_worker_failure = 112;
    }
    g_worker_context = NULL;
    ReleaseDC(g_worker_window, g_worker_dc);
    g_worker_dc = NULL;
    DestroyWindow(g_worker_window);
    g_worker_window = NULL;
    return g_worker_failure;
}

static int run_test(void)
{
    WNDCLASSA wc;
    HANDLE worker;
    DWORD worker_exit = 0;
    DWORD last_error;
    char message[256];

    ZeroMemory(&wc, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = g_window_class;
    if (!RegisterClassA(&wc)) return 1;

    g_main_window = CreateWindowA(g_window_class,
            "WGL per-thread current-context test: running",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 720, 420,
            NULL, NULL, g_instance, NULL);
    if (!g_main_window) return 2;
    g_main_dc = GetDC(g_main_window);
    if (!g_main_dc) return 3;
    if (!set_default_pixel_format(g_main_dc)) return 4;
    g_main_context = wglCreateContext(g_main_dc);
    if (!g_main_context) return 5;
    if (!wglMakeCurrent(g_main_dc, g_main_context)) return 6;

    g_worker_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_worker_continue = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_worker_ready || !g_worker_continue) return 7;
    worker = CreateThread(NULL, 0, worker_proc, NULL, 0, NULL);
    if (!worker) return 8;
    if (WaitForSingleObject(g_worker_ready, 10000) != WAIT_OBJECT_0)
        return 9;
    if (g_worker_failure) return (int)g_worker_failure;

    if (wglGetCurrentContext() != g_main_context) return 10;
    if (wglGetCurrentDC() != g_main_dc) return 11;

    SetLastError(ERROR_SUCCESS);
    if (wglMakeCurrent(g_worker_dc, g_worker_context)) return 12;
    last_error = GetLastError();
    if (last_error != ERROR_BUSY) return 13;

    /* A context that is current on another thread must not be deleted. */
    SetLastError(ERROR_SUCCESS);
    if (wglDeleteContext(g_worker_context)) return 14;
    if (GetLastError() != ERROR_BUSY) return 15;

    SetEvent(g_worker_continue);
    if (WaitForSingleObject(worker, 10000) != WAIT_OBJECT_0) return 16;
    GetExitCodeThread(worker, &worker_exit);
    CloseHandle(worker);
    if (worker_exit || g_worker_failure) {
        return (int)(worker_exit ? worker_exit : g_worker_failure);
    }

    /* Releasing ctxB must not have released ctxA's logical binding. */
    if (wglGetCurrentContext() != g_main_context) return 17;
    if (wglGetCurrentDC() != g_main_dc) return 18;
    if (!wglMakeCurrent(NULL, NULL)) return 19;
    if (!wglDeleteContext(g_main_context)) return 20;
    g_main_context = NULL;

    trace_text("PASS: independent per-thread bindings and release lifetime");
    SetWindowTextA(g_main_window,
            "WGL per-thread current-context test: PASS");
    wsprintfA(message,
            "PASS\r\n\r\n"
            "ctxA remained current on the main thread while ctxB was current "
            "on the worker thread. Releasing ctxB did not release ctxA.");
    MessageBoxA(g_main_window, message, "WGL thread-current regression",
            MB_OK | MB_ICONINFORMATION);
    return 0;
}

void WINAPI WinMainCRTStartup(void)
{
    int result;
    char message[192];

    g_instance = GetModuleHandleA(NULL);
    result = run_test();
    if (result)
    {
        wsprintfA(message,
                "FAIL at stage %d, GetLastError=0x%08lX",
                result, (unsigned long)GetLastError());
        trace_text(message);
        if (g_main_window)
            SetWindowTextA(g_main_window,
                    "WGL per-thread current-context test: FAIL");
        MessageBoxA(g_main_window, message, "WGL thread-current regression",
                MB_OK | MB_ICONERROR);
    }

    if (g_main_context)
    {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(g_main_context);
    }
    if (g_main_dc && g_main_window) ReleaseDC(g_main_window, g_main_dc);
    if (g_main_window) DestroyWindow(g_main_window);
    if (g_worker_ready) CloseHandle(g_worker_ready);
    if (g_worker_continue) CloseHandle(g_worker_continue);
    ExitProcess((UINT)result);
}
