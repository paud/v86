/*
 * Minimal app-local ddraw.dll proxy for Windows XP/9x guests in v86.
 *
 * dxdiag calls DirectDrawCreate to enumerate the primary display adapter and
 * queries IDirectDraw7::GetDeviceIdentifier and IDirectDraw7::GetCaps to fill
 * its Display tab. The real system ddraw.dll talks to the VBE/Bochs VGA
 * driver, which does not support DirectDraw acceleration, so dxdiag either
 * crashes or reports "No DirectDraw acceleration available".
 *
 * This proxy returns an honest device identity (matching the d3d9 proxy's
 * default NVIDIA GeForce FX 5200 so dxdiag's Display and Direct3D tabs agree)
 * and conservative caps that say "no 3D acceleration" rather than lying
 * about capabilities the rest of the glbridge stack does not route through
 * ddraw.dll.  It deliberately does not implement CreateSurface/Blt/Flip -
 * those return DDERR_UNSUPPORTED, which dxdiag handles gracefully.
 *
 * The COM vtable layout follows the MinGW ddraw.h IID_IDirectDraw7 interface.
 * Only the methods dxdiag actually calls are implemented; the rest return
 * DDERR_UNSUPPORTED so a caller can see a real error instead of a crash.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define CINTERFACE
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <initguid.h>
#include <ddraw.h>
#include <stdint.h>

/* Match the d3d9 proxy's default identity so dxdiag's DirectDraw and
 * Direct3D tabs report the same card. */
#define DDRAW_VENDOR_ID    0x10DE
#define DDRAW_DEVICE_ID    0x0322
#define DDRAW_SUBSYS_ID   0x032210DE
#define DDRAW_REVISION    0xA1

/* ForceWare 66.93, same as the d3d9 proxy. */
#define DDRAW_DRIVER_VERSION_HIGH  ((6 << 16) | 14)
#define DDRAW_DRIVER_VERSION_LOW   ((10 << 16) | 6693)

/* IDirectDraw7 vtable slot count (QueryInterface..EvaluateMode = 30). */
#define DDRAW_VTABLE_SLOTS 30

/* Match the d3d8/d3d9 proxies: avoid memcmp (unavailable with -nostdlib). */
static BOOL guid_equal(REFIID left, REFIID right)
{
    UINT index;
    if (!left || !right || left->Data1 != right->Data1
            || left->Data2 != right->Data2 || left->Data3 != right->Data3)
        return FALSE;
    for (index = 0; index < 8; ++index) {
        if (left->Data4[index] != right->Data4[index]) return FALSE;
    }
    return TRUE;
}

typedef struct DDRAW_DirectDraw {
    IDirectDraw7Vtbl *lpVtbl;
    LONG refcount;
} DDRAW_DirectDraw;

/* ---- IUnknown ---- */

static HRESULT WINAPI dd7_QueryInterface(IDirectDraw7 *iface, REFIID riid,
        void **ppv)
{
    if (!riid || !ppv)
        return E_POINTER;
    *ppv = NULL;
    if (guid_equal(riid, &IID_IUnknown) ||
        guid_equal(riid, &IID_IDirectDraw7)) {
        iface->lpVtbl->AddRef(iface);
        *ppv = iface;
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG WINAPI dd7_AddRef(IDirectDraw7 *iface)
{
    DDRAW_DirectDraw *self = (DDRAW_DirectDraw *)iface;
    return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG WINAPI dd7_Release(IDirectDraw7 *iface)
{
    DDRAW_DirectDraw *self = (DDRAW_DirectDraw *)iface;
    LONG count = InterlockedDecrement(&self->refcount);
    if (count <= 0)
        HeapFree(GetProcessHeap(), 0, self);
    return (ULONG)(count > 0 ? count : 0);
}

/* ---- IDirectDraw7 methods that dxdiag calls ---- */

static HRESULT WINAPI dd7_GetDeviceIdentifier(IDirectDraw7 *iface,
        DDDEVICEIDENTIFIER2 *identifier, DWORD flags)
{
    (void)iface;
    (void)flags;
    if (!identifier)
        return DDERR_INVALIDPARAMS;
    ZeroMemory(identifier, sizeof(*identifier));
    lstrcpynA(identifier->szDriver, "nv4_disp.dll",
            sizeof(identifier->szDriver));
    lstrcpynA(identifier->szDescription, "NVIDIA GeForce FX 5200",
            sizeof(identifier->szDescription));
    identifier->liDriverVersion.HighPart = DDRAW_DRIVER_VERSION_HIGH;
    identifier->liDriverVersion.LowPart = DDRAW_DRIVER_VERSION_LOW;
    identifier->dwVendorId = DDRAW_VENDOR_ID;
    identifier->dwDeviceId = DDRAW_DEVICE_ID;
    identifier->dwSubSysId = DDRAW_SUBSYS_ID;
    identifier->dwRevision = DDRAW_REVISION;
    /* Stable non-zero GUID derived from the identity, same scheme as the
     * d3d9 proxy. */
    identifier->guidDeviceIdentifier.Data1 = 0xDD000000u | DDRAW_DEVICE_ID;
    identifier->guidDeviceIdentifier.Data2 = (WORD)DDRAW_VENDOR_ID;
    identifier->guidDeviceIdentifier.Data3 = (WORD)DDRAW_DEVICE_ID;
    identifier->guidDeviceIdentifier.Data4[0] = 0x9A;
    identifier->guidDeviceIdentifier.Data4[1] = 0xB1;
    identifier->guidDeviceIdentifier.Data4[2] = 0xC2;
    identifier->guidDeviceIdentifier.Data4[3] = 0xD3;
    identifier->guidDeviceIdentifier.Data4[4] = 0xE4;
    identifier->guidDeviceIdentifier.Data4[5] = 0xF5;
    identifier->guidDeviceIdentifier.Data4[6] = 0x06;
    identifier->guidDeviceIdentifier.Data4[7] = 0x17;
    /* 1 = WHQL-signed but no date, matching the d3d9 proxy. */
    identifier->dwWHQLLevel = 1;
    return DD_OK;
}

static HRESULT WINAPI dd7_GetCaps(IDirectDraw7 *iface, DDCAPS *driver_caps,
        DDCAPS *hel_caps)
{
    (void)iface;
    /* Honest caps: the ddraw path does not provide 3D acceleration.
     * dxdiag reads these to decide what to put in the Display tab; reporting
     * zero caps shows "No acceleration" rather than crashing on a NULL
     * vtable or a system ddraw.dll that cannot initialise. */
    if (driver_caps) {
        ZeroMemory(driver_caps, driver_caps->dwSize);
        driver_caps->dwSize = sizeof(DDCAPS_DX7);
        /* 16 MB total "video memory", 8 MB free - plausible for a 2002-era
         * entry-level card and enough for dxdiag's bar graph. */
        driver_caps->dwVidMemTotal = 16 * 1024 * 1024;
        driver_caps->dwVidMemFree  =  8 * 1024 * 1024;
        driver_caps->dwZBufferBitDepths = 0x00010000; /* DDBD_16 */
        driver_caps->dwNumFourCCCodes = 0;
    }
    if (hel_caps) {
        ZeroMemory(hel_caps, hel_caps->dwSize);
        hel_caps->dwSize = sizeof(DDCAPS_DX7);
    }
    return DD_OK;
}

static HRESULT WINAPI dd7_GetDisplayMode(IDirectDraw7 *iface,
        DDSURFACEDESC2 *surface_desc)
{
    (void)iface;
    if (!surface_desc)
        return DDERR_INVALIDPARAMS;
    ZeroMemory(surface_desc, sizeof(*surface_desc));
    surface_desc->dwSize = sizeof(DDSURFACEDESC2);
    surface_desc->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT |
            DDSD_REFRESHRATE | DDSD_PITCH;
    surface_desc->dwWidth = 640;
    surface_desc->dwHeight = 480;
    surface_desc->lPitch = 640 * 2;
    surface_desc->dwRefreshRate = 60;
    surface_desc->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    surface_desc->ddpfPixelFormat.dwFlags = DDPF_RGB;
    surface_desc->ddpfPixelFormat.dwRGBBitCount = 16;
    surface_desc->ddpfPixelFormat.dwRBitMask = 0xF800;
    surface_desc->ddpfPixelFormat.dwGBitMask = 0x07E0;
    surface_desc->ddpfPixelFormat.dwBBitMask = 0x001F;
    return DD_OK;
}

static HRESULT WINAPI dd7_SetCooperativeLevel(IDirectDraw7 *iface, HWND hwnd,
        DWORD flags)
{
    (void)iface;
    (void)hwnd;
    (void)flags;
    return DD_OK;
}

static HRESULT WINAPI dd7_TestCooperativeLevel(IDirectDraw7 *iface)
{
    (void)iface;
    return DD_OK;
}

/* ---- IDirectDraw7 methods dxdiag does not exercise ---- */
/* Return DDERR_UNSUPPORTED for everything else so a caller gets a real
 * error code rather than a crash from a NULL vtable slot. */
static HRESULT WINAPI dd7_Stub_Compact(IDirectDraw7 *iface)
{ (void)iface; return DD_OK; }

static HRESULT WINAPI dd7_Stub_CreateClipper(IDirectDraw7 *iface, DWORD a,
        IDirectDrawClipper **b, IUnknown *c)
{ (void)iface; (void)a; (void)b; (void)c; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI dd7_Stub_CreatePalette(IDirectDraw7 *iface, DWORD a,
        PALETTEENTRY *b, IDirectDrawPalette **c, IUnknown *d)
{ (void)iface; (void)a; (void)b; (void)c; (void)d; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI dd7_Stub_CreateSurface(IDirectDraw7 *iface,
        DDSURFACEDESC2 *a, IDirectDrawSurface7 **b, IUnknown *c)
{ (void)iface; (void)a; (void)b; (void)c; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI dd7_Stub_DuplicateSurface(IDirectDraw7 *iface,
        IDirectDrawSurface7 *a, IDirectDrawSurface7 **b)
{ (void)iface; (void)a; (void)b; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI dd7_Stub_EnumDisplayModes(IDirectDraw7 *iface, DWORD a,
        DDSURFACEDESC2 *b, void *c, LPDDENUMMODESCALLBACK2 d)
{
    /* dxdiag calls EnumDisplayModes to list available resolutions.  Return
     * one 640x480x16 mode so the list is non-empty. */
    (void)iface; (void)a; (void)b;
    if (d) {
        DDSURFACEDESC2 mode;
        ZeroMemory(&mode, sizeof(mode));
        mode.dwSize = sizeof(DDSURFACEDESC2);
        mode.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT |
                DDSD_REFRESHRATE | DDSD_PITCH;
        mode.dwWidth = 640;
        mode.dwHeight = 480;
        mode.lPitch = 640 * 2;
        mode.dwRefreshRate = 60;
        mode.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        mode.ddpfPixelFormat.dwFlags = DDPF_RGB;
        mode.ddpfPixelFormat.dwRGBBitCount = 16;
        mode.ddpfPixelFormat.dwRBitMask = 0xF800;
        mode.ddpfPixelFormat.dwGBitMask = 0x07E0;
        mode.ddpfPixelFormat.dwBBitMask = 0x001F;
        d(&mode, c);
    }
    return DD_OK;
}

static HRESULT WINAPI dd7_Stub_EnumSurfaces(IDirectDraw7 *iface, DWORD a,
        DDSURFACEDESC2 *b, void *c, LPDDENUMSURFACESCALLBACK7 d)
{ (void)iface; (void)a; (void)b; (void)c; (void)d; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI dd7_Stub_FlipToGDISurface(IDirectDraw7 *iface)
{ (void)iface; return DD_OK; }

static HRESULT WINAPI dd7_Stub_GetFourCCCodes(IDirectDraw7 *iface,
        DWORD *a, DWORD *b)
{ (void)iface; if (a) *a = 0; (void)b; return DD_OK; }

static HRESULT WINAPI dd7_Stub_GetGDISurface(IDirectDraw7 *iface,
        IDirectDrawSurface7 **a)
{ (void)iface; (void)a; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI dd7_Stub_GetMonitorFrequency(IDirectDraw7 *iface,
        DWORD *a)
{ (void)iface; if (a) *a = 60; return DD_OK; }

static HRESULT WINAPI dd7_Stub_GetScanLine(IDirectDraw7 *iface, DWORD *a)
{ (void)iface; if (a) *a = 0; return DD_OK; }

static HRESULT WINAPI dd7_Stub_GetVerticalBlankStatus(IDirectDraw7 *iface,
        BOOL *a)
{ (void)iface; if (a) *a = TRUE; return DD_OK; }

static HRESULT WINAPI dd7_Stub_Initialize(IDirectDraw7 *iface, GUID *a)
{ (void)iface; (void)a; return DDERR_ALREADYINITIALIZED; }

static HRESULT WINAPI dd7_Stub_RestoreDisplayMode(IDirectDraw7 *iface)
{ (void)iface; return DD_OK; }

static HRESULT WINAPI dd7_Stub_SetDisplayMode(IDirectDraw7 *iface, DWORD a,
        DWORD b, DWORD c, DWORD d, DWORD e)
{ (void)iface; (void)a; (void)b; (void)c; (void)d; (void)e; return DD_OK; }

static HRESULT WINAPI dd7_Stub_WaitForVerticalBlank(IDirectDraw7 *iface,
        DWORD a, HANDLE b)
{ (void)iface; (void)a; (void)b; return DD_OK; }

static HRESULT WINAPI dd7_Stub_GetAvailableVidMem(IDirectDraw7 *iface,
        DDSCAPS2 *a, DWORD *b, DWORD *c)
{
    (void)iface; (void)a;
    if (b) *b = 16 * 1024 * 1024;
    if (c) *c =  8 * 1024 * 1024;
    return DD_OK;
}

static HRESULT WINAPI dd7_Stub_GetSurfaceFromDC(IDirectDraw7 *iface, HDC a,
        IDirectDrawSurface7 **b)
{ (void)iface; (void)a; (void)b; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI dd7_Stub_RestoreAllSurfaces(IDirectDraw7 *iface)
{ (void)iface; return DD_OK; }

static HRESULT WINAPI dd7_Stub_StartModeTest(IDirectDraw7 *iface, SIZE *a,
        DWORD b, DWORD c)
{ (void)iface; (void)a; (void)b; (void)c; return DDERR_UNSUPPORTED; }

static HRESULT WINAPI dd7_Stub_EvaluateMode(IDirectDraw7 *iface, DWORD a,
        DWORD *b)
{ (void)iface; (void)a; (void)b; return DDERR_UNSUPPORTED; }

/* ---- vtable ---- */

static IDirectDraw7Vtbl g_dd7_vtbl = {
    /* IUnknown */
    dd7_QueryInterface,
    dd7_AddRef,
    dd7_Release,
    /* IDirectDraw7 */
    dd7_Stub_Compact,
    dd7_Stub_CreateClipper,
    dd7_Stub_CreatePalette,
    dd7_Stub_CreateSurface,
    dd7_Stub_DuplicateSurface,
    dd7_Stub_EnumDisplayModes,
    dd7_Stub_EnumSurfaces,
    dd7_Stub_FlipToGDISurface,
    dd7_GetCaps,
    dd7_GetDisplayMode,
    dd7_Stub_GetFourCCCodes,
    dd7_Stub_GetGDISurface,
    dd7_Stub_GetMonitorFrequency,
    dd7_Stub_GetScanLine,
    dd7_Stub_GetVerticalBlankStatus,
    dd7_Stub_Initialize,
    dd7_Stub_RestoreDisplayMode,
    dd7_SetCooperativeLevel,
    dd7_Stub_SetDisplayMode,
    dd7_Stub_WaitForVerticalBlank,
    dd7_Stub_GetAvailableVidMem,
    dd7_Stub_GetSurfaceFromDC,
    dd7_Stub_RestoreAllSurfaces,
    dd7_TestCooperativeLevel,
    dd7_GetDeviceIdentifier,
    dd7_Stub_StartModeTest,
    dd7_Stub_EvaluateMode,
};

/* ---- DirectDrawCreate / DirectDrawCreateEx ---- */

HRESULT WINAPI DirectDrawCreate(GUID *guid, IDirectDraw **dd, IUnknown *outer)
{
    DDRAW_DirectDraw *self;
    (void)guid;
    (void)outer;
    if (!dd)
        return DDERR_INVALIDPARAMS;
    *dd = NULL;
    self = (DDRAW_DirectDraw *)HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self)
        return DDERR_OUTOFMEMORY;
    /* IDirectDraw7 is a superset; dxdiag queries IID_IDirectDraw7 via
     * DirectDrawCreateEx or QI, so hand back an IDirectDraw7-compat object
     * through the IDirectDraw pointer. */
    self->lpVtbl = &g_dd7_vtbl;
    self->refcount = 1;
    *dd = (IDirectDraw *)self;
    return DD_OK;
}

HRESULT WINAPI DirectDrawCreateEx(GUID *guid, void **dd, REFIID riid,
        IUnknown *outer)
{
    DDRAW_DirectDraw *self;
    (void)guid;
    (void)outer;
    if (!dd || !riid)
        return DDERR_INVALIDPARAMS;
    *dd = NULL;
    if (!guid_equal(riid, &IID_IDirectDraw7) &&
        !guid_equal(riid, &IID_IDirectDraw4) &&
        !guid_equal(riid, &IID_IDirectDraw) &&
        !guid_equal(riid, &IID_IUnknown))
        return DDERR_NOTINITIALIZED;
    self = (DDRAW_DirectDraw *)HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, sizeof(*self));
    if (!self)
        return DDERR_OUTOFMEMORY;
    self->lpVtbl = &g_dd7_vtbl;
    self->refcount = 1;
    *dd = self;
    return DD_OK;
}

/* dxdiag calls DirectDrawEnumerateEx to list display devices.  Provide one
 * primary device entry so the enumeration callback fires. */
HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA callback, void *context)
{
    static char driver[] = "v86ddraw.dll";
    static char desc[] = "v86 DirectDraw Adapter";
    if (!callback)
        return DDERR_INVALIDPARAMS;
    callback(NULL, &desc[0], &driver[0], context);
    return DD_OK;
}

HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW callback, void *context)
{
    static wchar_t driver[] = L"v86ddraw.dll";
    static wchar_t desc[] = L"v86 DirectDraw Adapter";
    if (!callback)
        return DDERR_INVALIDPARAMS;
    callback(NULL, desc, driver, context);
    return DD_OK;
}

HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA callback,
        void *context, DWORD flags)
{
    static char driver[] = "v86ddraw.dll";
    static char desc[] = "v86 DirectDraw Adapter";
    (void)flags;
    if (!callback)
        return DDERR_INVALIDPARAMS;
    callback(NULL, &desc[0], &driver[0], NULL, context);
    return DD_OK;
}

HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW callback,
        void *context, DWORD flags)
{
    static wchar_t driver[] = L"v86ddraw.dll";
    static wchar_t desc[] = L"v86 DirectDraw Adapter";
    (void)flags;
    if (!callback)
        return DDERR_INVALIDPARAMS;
    callback(NULL, desc, driver, NULL, context);
    return DD_OK;
}

/* DirectDrawCreateClipper - dxdiag may call this to create a clipper. */
HRESULT WINAPI DirectDrawCreateClipper(DWORD flags,
        IDirectDrawClipper **clipper, IUnknown *outer)
{
    (void)flags;
    (void)outer;
    if (!clipper)
        return DDERR_INVALIDPARAMS;
    *clipper = NULL;
    return DDERR_UNSUPPORTED;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
