// MapleStory v83 Gr2D Direct3D 8 mode/capability regression probe.
//
// This follows the observed Gr2D_DX8.dll selector order:
//   GetAdapterModeCount -> EnumAdapterModes -> CheckDeviceFormat for
//   A4R4G4B4, A8R8G8B8, R5G6B5, and DXT3 -> CreateDevice with 0x46,
//   falling back to 0x26.
//
// Build for Windows XP as documented in ../winproxy/README.md. The command
// uses a 32-bit MinGW compiler and deliberately avoids the MinGW C runtime.

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d8.h>

#define TEST_CLIENT_WIDTH       800
#define TEST_CLIENT_HEIGHT      600
#define MAPLE_REQUESTED_REFRESH   0
#define MAX_MODE_CANDIDATES     256
#define REPORT_LINE_COUNT        38
#define REPORT_LINE_LENGTH      176

#define MAPLE_CREATE_HARDWARE \
    (D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED \
            | D3DCREATE_HARDWARE_VERTEXPROCESSING)
#define MAPLE_CREATE_SOFTWARE \
    (D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED \
            | D3DCREATE_SOFTWARE_VERTEXPROCESSING)

typedef struct FormatProbe
{
    const char *name;
    D3DFORMAT format;
} FormatProbe;

typedef struct ModeCandidate
{
    D3DDISPLAYMODE mode;
    D3DFORMAT texture_format;
    UINT mode_index;
    BOOL format_supported[4];
} ModeCandidate;

typedef struct MapleVertex
{
    FLOAT x;
    FLOAT y;
    FLOAT z;
    FLOAT rhw;
    DWORD diffuse;
    DWORD specular;
    FLOAT u0;
    FLOAT v0;
    FLOAT u1;
    FLOAT v1;
} MapleVertex;

#define MAPLE_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR \
        | D3DFVF_TEX2)

static const FormatProbe g_format_probes[] =
{
    {"A4R4G4B4", D3DFMT_A4R4G4B4},
    {"A8R8G8B8", D3DFMT_A8R8G8B8},
    {"R5G6B5", D3DFMT_R5G6B5},
    {"DXT3", D3DFMT_DXT3}
};

static const char g_window_class[] = "V86GLD3D8MapleGr2DTest";
static IDirect3D8 *g_d3d;
static IDirect3DDevice8 *g_device;
static HWND g_window;
static ModeCandidate g_candidates[MAX_MODE_CANDIDATES];
static char g_report_lines[REPORT_LINE_COUNT][REPORT_LINE_LENGTH];
static UINT g_report_line_start;
static UINT g_report_line_count;
static UINT g_report_lines_dropped;

static const char *format_name(D3DFORMAT format)
{
    switch (format)
    {
        case D3DFMT_UNKNOWN: return "UNKNOWN";
        case D3DFMT_R8G8B8: return "R8G8B8";
        case D3DFMT_A8R8G8B8: return "A8R8G8B8";
        case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case D3DFMT_R5G6B5: return "R5G6B5";
        case D3DFMT_X1R5G5B5: return "X1R5G5B5";
        case D3DFMT_A1R5G5B5: return "A1R5G5B5";
        case D3DFMT_A4R4G4B4: return "A4R4G4B4";
        case D3DFMT_DXT1: return "DXT1";
        case D3DFMT_DXT2: return "DXT2";
        case D3DFMT_DXT3: return "DXT3";
        case D3DFMT_DXT4: return "DXT4";
        case D3DFMT_DXT5: return "DXT5";
        default: return "OTHER";
    }
}

static void append_report_line(const char *text)
{
    UINT target;
    if (g_report_line_count < REPORT_LINE_COUNT)
    {
        target = (g_report_line_start + g_report_line_count)
                % REPORT_LINE_COUNT;
        ++g_report_line_count;
    }
    else
    {
        target = g_report_line_start;
        g_report_line_start = (g_report_line_start + 1) % REPORT_LINE_COUNT;
        ++g_report_lines_dropped;
    }
    lstrcpynA(g_report_lines[target], text, REPORT_LINE_LENGTH);
    if (g_window)
    {
        InvalidateRect(g_window, NULL, TRUE);
        UpdateWindow(g_window);
    }
}

static void trace_text(const char *text)
{
    OutputDebugStringA("[d3d8-maple-gr2d] ");
    OutputDebugStringA(text);
    OutputDebugStringA("\r\n");
    append_report_line(text);
}

static void trace_hresult(const char *stage, HRESULT hr)
{
    char line[REPORT_LINE_LENGTH];
    wsprintfA(line, "%s -> hr=0x%08lX (%s)", stage,
            (unsigned long)hr, SUCCEEDED(hr) ? "SUCCESS" : "FAILED");
    trace_text(line);
}

static void trace_mode(UINT index, HRESULT hr, const D3DDISPLAYMODE *mode)
{
    char line[REPORT_LINE_LENGTH];
    if (FAILED(hr))
    {
        wsprintfA(line, "03.%03u EnumAdapterModes(%u) -> hr=0x%08lX FAILED",
                index, index, (unsigned long)hr);
    }
    else
    {
        wsprintfA(line,
                "03.%03u EnumAdapterModes(%u) -> hr=0x%08lX "
                "%lux%lu@%lu format=%s(%lu)",
                index, index, (unsigned long)hr,
                (unsigned long)mode->Width, (unsigned long)mode->Height,
                (unsigned long)mode->RefreshRate, format_name(mode->Format),
                (unsigned long)mode->Format);
    }
    trace_text(line);
}

static void trace_format_probe(D3DFORMAT adapter_format,
        const FormatProbe *probe, HRESULT hr)
{
    char line[REPORT_LINE_LENGTH];
    wsprintfA(line,
            "04 CheckDeviceFormat adapter=%s(%lu), texture=%s(%lu), "
            "usage=0,type=TEXTURE -> hr=0x%08lX %s",
            format_name(adapter_format), (unsigned long)adapter_format,
            probe->name, (unsigned long)probe->format, (unsigned long)hr,
            SUCCEEDED(hr) ? "SUPPORTED" : "UNSUPPORTED");
    trace_text(line);
}

static void trace_candidate(UINT candidate_index,
        const ModeCandidate *candidate)
{
    char line[REPORT_LINE_LENGTH];
    wsprintfA(line,
            "04 candidate[%u] mode[%u]=%lux%lu@%lu %s, "
            "A4=%u A8=%u R5=%u DXT3=%u, texture=%s",
            candidate_index, candidate->mode_index,
            (unsigned long)candidate->mode.Width,
            (unsigned long)candidate->mode.Height,
            (unsigned long)candidate->mode.RefreshRate,
            format_name(candidate->mode.Format),
            candidate->format_supported[0],
            candidate->format_supported[1],
            candidate->format_supported[2],
            candidate->format_supported[3],
            format_name(candidate->texture_format));
    trace_text(line);
}

static void release_d3d8(void)
{
    if (g_device)
    {
        IDirect3DDevice8_Release(g_device);
        g_device = NULL;
    }
    if (g_d3d)
    {
        IDirect3D8_Release(g_d3d);
        g_d3d = NULL;
    }
}

static void set_quad(MapleVertex *vertices, FLOAT left, FLOAT top,
        FLOAT right, FLOAT bottom, DWORD diffuse, DWORD specular,
        FLOAT repeat)
{
    static const BYTE corners[6][2] =
    {
        {0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}
    };
    UINT i;
    for (i = 0; i < 6; ++i)
    {
        FLOAT u = (FLOAT)corners[i][0] * repeat;
        FLOAT v = (FLOAT)corners[i][1] * repeat;
        vertices[i].x = corners[i][0] ? right : left;
        vertices[i].y = corners[i][1] ? bottom : top;
        vertices[i].z = 0.5f;
        vertices[i].rhw = 1.0f;
        vertices[i].diffuse = diffuse;
        vertices[i].specular = specular;
        vertices[i].u0 = u;
        vertices[i].v0 = v;
        vertices[i].u1 = u;
        vertices[i].v1 = v;
    }
}

static HRESULT run_render_probe(void)
{
    IDirect3DTexture8 *base_texture = NULL;
    IDirect3DTexture8 *light_texture = NULL;
    IDirect3DSurface8 *base_surface = NULL;
    D3DLOCKED_RECT locked;
    D3DVIEWPORT8 viewport;
    MapleVertex vertices[6];
    UINT x;
    UINT y;
    HRESULT hr = D3D_OK;

#define RENDER_CALL(stage, expression) do { \
    hr = (expression); \
    if (FAILED(hr)) { trace_hresult(stage, hr); goto cleanup; } \
} while (0)

    trace_text("09 Maple 2D render probe begin");
    RENDER_CALL("09 CreateTexture(A4R4G4B4)",
            IDirect3DDevice8_CreateTexture(g_device, 32, 32, 1, 0,
            D3DFMT_A4R4G4B4, D3DPOOL_MANAGED, &base_texture));
    RENDER_CALL("10 GetSurfaceLevel(A4R4G4B4)",
            IDirect3DTexture8_GetSurfaceLevel(base_texture, 0,
            &base_surface));
    RENDER_CALL("11 Surface::LockRect(A4R4G4B4)",
            IDirect3DSurface8_LockRect(base_surface, &locked, NULL, 0));
    for (y = 0; y < 32; ++y)
    {
        WORD *row = (WORD *)((BYTE *)locked.pBits + y * locked.Pitch);
        for (x = 0; x < 32; ++x)
        {
            UINT checker = ((x >> 2) ^ (y >> 2)) & 1;
            UINT alpha = x < 2 || y < 2 || x >= 30 || y >= 30 ? 0 : 15;
            UINT red = checker ? 15 : 3;
            UINT green = checker ? 6 : 14;
            UINT blue = checker ? 2 : 13;
            row[x] = (WORD)((alpha << 12) | (red << 8)
                    | (green << 4) | blue);
        }
    }
    RENDER_CALL("12 Surface::UnlockRect(A4R4G4B4)",
            IDirect3DSurface8_UnlockRect(base_surface));
    IDirect3DSurface8_Release(base_surface);
    base_surface = NULL;

    RENDER_CALL("13 CreateTexture(A8R8G8B8)",
            IDirect3DDevice8_CreateTexture(g_device, 32, 32, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &light_texture));
    RENDER_CALL("14 Texture::LockRect(A8R8G8B8)",
            IDirect3DTexture8_LockRect(light_texture, 0, &locked, NULL, 0));
    for (y = 0; y < 32; ++y)
    {
        DWORD *row = (DWORD *)((BYTE *)locked.pBits + y * locked.Pitch);
        for (x = 0; x < 32; ++x)
        {
            UINT dx = x > 15 ? x - 15 : 15 - x;
            UINT dy = y > 15 ? y - 15 : 15 - y;
            UINT alpha = 255 - (dx + dy) * 7;
            row[x] = (alpha << 24) | 0x00C0E8FFu;
        }
    }
    RENDER_CALL("15 Texture::UnlockRect(A8R8G8B8)",
            IDirect3DTexture8_UnlockRect(light_texture, 0));

    RENDER_CALL("16 SetVertexShader(XYZRHW|DIFFUSE|SPECULAR|TEX2)",
            IDirect3DDevice8_SetVertexShader(g_device, MAPLE_FVF));
    RENDER_CALL("17 LIGHTING=FALSE", IDirect3DDevice8_SetRenderState(
            g_device, D3DRS_LIGHTING, FALSE));
    RENDER_CALL("18 ZENABLE=FALSE", IDirect3DDevice8_SetRenderState(
            g_device, D3DRS_ZENABLE, FALSE));
    RENDER_CALL("19 CULLMODE=NONE", IDirect3DDevice8_SetRenderState(
            g_device, D3DRS_CULLMODE, D3DCULL_NONE));
    RENDER_CALL("20 SPECULARENABLE=TRUE", IDirect3DDevice8_SetRenderState(
            g_device, D3DRS_SPECULARENABLE, TRUE));
    RENDER_CALL("21 ALPHABLEND=TRUE", IDirect3DDevice8_SetRenderState(
            g_device, D3DRS_ALPHABLENDENABLE, TRUE));
    RENDER_CALL("22 SRCBLEND=SRCALPHA", IDirect3DDevice8_SetRenderState(
            g_device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
    RENDER_CALL("23 DESTBLEND=INVSRCALPHA", IDirect3DDevice8_SetRenderState(
            g_device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));
    RENDER_CALL("24 ALPHATEST=TRUE", IDirect3DDevice8_SetRenderState(
            g_device, D3DRS_ALPHATESTENABLE, TRUE));
    RENDER_CALL("25 ALPHAREF=8", IDirect3DDevice8_SetRenderState(
            g_device, D3DRS_ALPHAREF, 8));
    RENDER_CALL("26 ALPHAFUNC=GREATER", IDirect3DDevice8_SetRenderState(
            g_device, D3DRS_ALPHAFUNC, D3DCMP_GREATER));

    RENDER_CALL("27 Clear", IDirect3DDevice8_Clear(g_device, 0, NULL,
            D3DCLEAR_TARGET, 0xFF132038u, 1.0f, 0));
    RENDER_CALL("28 BeginScene", IDirect3DDevice8_BeginScene(g_device));

    RENDER_CALL("29 SetTexture(0,NULL)", IDirect3DDevice8_SetTexture(
            g_device, 0, NULL));
    RENDER_CALL("30 stage0 SELECTARG1", IDirect3DDevice8_SetTextureStageState(
            g_device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    RENDER_CALL("31 stage0 COLORARG1=DIFFUSE",
            IDirect3DDevice8_SetTextureStageState(g_device, 0,
            D3DTSS_COLORARG1, D3DTA_DIFFUSE));
    RENDER_CALL("32 stage0 ALPHAARG1=DIFFUSE",
            IDirect3DDevice8_SetTextureStageState(g_device, 0,
            D3DTSS_ALPHAARG1, D3DTA_DIFFUSE));
    RENDER_CALL("33 stage1 DISABLE", IDirect3DDevice8_SetTextureStageState(
            g_device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE));
    set_quad(vertices, 20.0f, 20.0f, 780.0f, 580.0f,
            0xFF203A5Au, 0, 1.0f);
    RENDER_CALL("34 DrawUP background", IDirect3DDevice8_DrawPrimitiveUP(
            g_device, D3DPT_TRIANGLELIST, 2, vertices,
            sizeof(MapleVertex)));

    viewport.X = 40;
    viewport.Y = 80;
    viewport.Width = 720;
    viewport.Height = 440;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    RENDER_CALL("35 SetViewport", IDirect3DDevice8_SetViewport(g_device,
            &viewport));
    RENDER_CALL("36 SetTexture(0,A4)", IDirect3DDevice8_SetTexture(g_device,
            0, (IDirect3DBaseTexture8 *)base_texture));
    RENDER_CALL("37 stage0 MODULATE", IDirect3DDevice8_SetTextureStageState(
            g_device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE));
    RENDER_CALL("38 stage0 COLORARG1=TEXTURE",
            IDirect3DDevice8_SetTextureStageState(g_device, 0,
            D3DTSS_COLORARG1, D3DTA_TEXTURE));
    RENDER_CALL("39 stage0 COLORARG2=DIFFUSE",
            IDirect3DDevice8_SetTextureStageState(g_device, 0,
            D3DTSS_COLORARG2, D3DTA_DIFFUSE));
    RENDER_CALL("40 stage0 ALPHAARG1=TEXTURE",
            IDirect3DDevice8_SetTextureStageState(g_device, 0,
            D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
    RENDER_CALL("41 stage0 POINT", IDirect3DDevice8_SetTextureStageState(
            g_device, 0, D3DTSS_MINFILTER, D3DTEXF_POINT));
    RENDER_CALL("42 stage0 WRAP", IDirect3DDevice8_SetTextureStageState(
            g_device, 0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP));
    set_quad(vertices, 20.0f, 100.0f, 370.0f, 500.0f,
            0xFFFFFFFFu, 0x00100810u, 4.0f);
    RENDER_CALL("43 DrawUP A4 sprite", IDirect3DDevice8_DrawPrimitiveUP(
            g_device, D3DPT_TRIANGLELIST, 2, vertices,
            sizeof(MapleVertex)));

    RENDER_CALL("44 SetTexture(1,A8)", IDirect3DDevice8_SetTexture(g_device,
            1, (IDirect3DBaseTexture8 *)light_texture));
    RENDER_CALL("45 stage1 TEXCOORDINDEX=1",
            IDirect3DDevice8_SetTextureStageState(g_device, 1,
            D3DTSS_TEXCOORDINDEX, 1));
    RENDER_CALL("46 stage1 ADD", IDirect3DDevice8_SetTextureStageState(
            g_device, 1, D3DTSS_COLOROP, D3DTOP_ADD));
    RENDER_CALL("47 stage1 COLORARG1=CURRENT",
            IDirect3DDevice8_SetTextureStageState(g_device, 1,
            D3DTSS_COLORARG1, D3DTA_CURRENT));
    RENDER_CALL("48 stage1 COLORARG2=TEXTURE",
            IDirect3DDevice8_SetTextureStageState(g_device, 1,
            D3DTSS_COLORARG2, D3DTA_TEXTURE));
    RENDER_CALL("49 stage1 ALPHA SELECT CURRENT",
            IDirect3DDevice8_SetTextureStageState(g_device, 1,
            D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    RENDER_CALL("50 stage1 ALPHAARG1=CURRENT",
            IDirect3DDevice8_SetTextureStageState(g_device, 1,
            D3DTSS_ALPHAARG1, D3DTA_CURRENT));
    RENDER_CALL("51 stage1 LINEAR", IDirect3DDevice8_SetTextureStageState(
            g_device, 1, D3DTSS_MINFILTER, D3DTEXF_LINEAR));
    RENDER_CALL("52 stage1 CLAMP", IDirect3DDevice8_SetTextureStageState(
            g_device, 1, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP));
    set_quad(vertices, 430.0f, 100.0f, 780.0f, 500.0f,
            0xFFE8F4FFu, 0x00101010u, 1.0f);
    RENDER_CALL("53 DrawUP TEX2 sprite", IDirect3DDevice8_DrawPrimitiveUP(
            g_device, D3DPT_TRIANGLELIST, 2, vertices,
            sizeof(MapleVertex)));

    RENDER_CALL("54 EndScene", IDirect3DDevice8_EndScene(g_device));
    RENDER_CALL("55 Present", IDirect3DDevice8_Present(g_device,
            NULL, NULL, NULL, NULL));
    trace_text("RESULT RENDER PASS: A4/A8 Surface, TEX2, alpha, viewport, DrawUP");

cleanup:
    if (g_device)
    {
        IDirect3DDevice8_SetTexture(g_device, 1, NULL);
        IDirect3DDevice8_SetTexture(g_device, 0, NULL);
    }
    if (base_surface) IDirect3DSurface8_Release(base_surface);
    if (light_texture) IDirect3DTexture8_Release(light_texture);
    if (base_texture) IDirect3DTexture8_Release(base_texture);
#undef RENDER_CALL
    return hr;
}

static HRESULT run_gr2d_regression(HWND hwnd)
{
    D3DPRESENT_PARAMETERS present_parameters;
    D3DDEVICE_CREATION_PARAMETERS creation_parameters;
    UINT mode_count;
    UINT candidate_count = 0;
    UINT enum_failure_count = 0;
    UINT selected_candidate = 0;
    DWORD highest_refresh = 0;
    BOOL has_selected_candidate = FALSE;
    BOOL has_r5g6b5_candidate = FALSE;
    UINT i;
    UINT j;
    HRESULT hr;
    HRESULT hardware_hr;
    HRESULT software_hr = E_FAIL;
    char line[REPORT_LINE_LENGTH];

    ZeroMemory(g_candidates, sizeof(g_candidates));

    wsprintfA(line, "01 Direct3DCreate8(D3D_SDK_VERSION=%u)",
            D3D_SDK_VERSION);
    trace_text(line);
    g_d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!g_d3d)
    {
        trace_text("01 Direct3DCreate8 -> NULL "
                "(API returns a pointer, not HRESULT)");
        return E_FAIL;
    }
    trace_text("01 Direct3DCreate8 -> non-NULL interface "
            "(API returns a pointer, not HRESULT)");

    mode_count = IDirect3D8_GetAdapterModeCount(g_d3d,
            D3DADAPTER_DEFAULT);
    wsprintfA(line,
            "02 GetAdapterModeCount(adapter=0) -> count=%u "
            "(API returns UINT, not HRESULT)",
            mode_count);
    trace_text(line);
    if (!mode_count)
    {
        trace_hresult("02 no adapter modes", E_FAIL);
        return E_FAIL;
    }

    for (i = 0; i < mode_count; ++i)
    {
        D3DDISPLAYMODE mode;
        ModeCandidate candidate;
        BOOL a4_or_a8;
        ZeroMemory(&mode, sizeof(mode));
        hr = IDirect3D8_EnumAdapterModes(g_d3d, D3DADAPTER_DEFAULT,
                i, &mode);
        trace_mode(i, hr, &mode);
        if (FAILED(hr))
        {
            ++enum_failure_count;
            continue;
        }
        if (mode.Width != TEST_CLIENT_WIDTH
                || mode.Height != TEST_CLIENT_HEIGHT)
        {
            wsprintfA(line,
                    "03.%03u selector skip: requested=%ux%u, actual=%lux%lu",
                    i, TEST_CLIENT_WIDTH, TEST_CLIENT_HEIGHT,
                    (unsigned long)mode.Width, (unsigned long)mode.Height);
            trace_text(line);
            continue;
        }

        ZeroMemory(&candidate, sizeof(candidate));
        candidate.mode = mode;
        candidate.mode_index = i;
        wsprintfA(line,
                "04 mode[%u] passed 800x600 filter; format matrix begin",
                i);
        trace_text(line);
        for (j = 0;
                j < sizeof(g_format_probes) / sizeof(g_format_probes[0]);
                ++j)
        {
            hr = IDirect3D8_CheckDeviceFormat(g_d3d,
                    D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                    mode.Format, 0, D3DRTYPE_TEXTURE,
                    g_format_probes[j].format);
            trace_format_probe(mode.Format, &g_format_probes[j], hr);
            candidate.format_supported[j] = SUCCEEDED(hr);
        }
        a4_or_a8 = candidate.format_supported[0]
                || candidate.format_supported[1];
        if (!a4_or_a8)
        {
            wsprintfA(line,
                    "04 mode[%u] rejected: neither A4R4G4B4 nor "
                    "A8R8G8B8 is supported",
                    i);
            trace_text(line);
            continue;
        }

        if (candidate.format_supported[3])
            candidate.texture_format = D3DFMT_DXT3;
        else if (candidate.format_supported[0])
            candidate.texture_format = D3DFMT_A4R4G4B4;
        else
            candidate.texture_format = D3DFMT_A8R8G8B8;

        if (candidate_count >= MAX_MODE_CANDIDATES)
        {
            trace_hresult("04 too many valid 800x600 candidates", E_FAIL);
            return E_FAIL;
        }
        g_candidates[candidate_count] = candidate;
        trace_candidate(candidate_count, &g_candidates[candidate_count]);
        if (candidate.mode.Format == D3DFMT_R5G6B5)
            has_r5g6b5_candidate = TRUE;
        ++candidate_count;
    }

    if (!candidate_count)
    {
        trace_text("RESULT FAIL: selector found no valid 800x600 mode; "
                "requires A4R4G4B4 or A8R8G8B8 texture support");
        return E_FAIL;
    }

    wsprintfA(line,
            "05 selector: %u valid candidates, requested refresh=%u, "
            "R5G6B5 candidate=%s",
            candidate_count, MAPLE_REQUESTED_REFRESH,
            has_r5g6b5_candidate ? "YES" : "NO");
    trace_text(line);

    if (MAPLE_REQUESTED_REFRESH > 0)
    {
        for (i = 0; i < candidate_count; ++i)
        {
            if (has_r5g6b5_candidate
                    && g_candidates[i].mode.Format == D3DFMT_X8R8G8B8)
                continue;
            if (g_candidates[i].mode.RefreshRate == MAPLE_REQUESTED_REFRESH)
            {
                selected_candidate = i;
                has_selected_candidate = TRUE;
                trace_text("05 selected first exact refresh match");
                break;
            }
        }
    }

    if (!has_selected_candidate)
    {
        for (i = 0; i < candidate_count; ++i)
        {
            if (has_r5g6b5_candidate
                    && g_candidates[i].mode.Format == D3DFMT_X8R8G8B8)
            {
                wsprintfA(line,
                        "05 prune candidate[%u]: R5G6B5 exists, remove X8R8G8B8",
                        i);
                trace_text(line);
                continue;
            }
            if (!has_selected_candidate
                    || g_candidates[i].mode.RefreshRate >= highest_refresh)
            {
                selected_candidate = i;
                highest_refresh = g_candidates[i].mode.RefreshRate;
                has_selected_candidate = TRUE;
            }
        }
    }

    if (!has_selected_candidate)
    {
        trace_text("RESULT FAIL: all selector candidates were pruned");
        return E_FAIL;
    }
    wsprintfA(line,
            "05 selected candidate[%u]: mode[%u] %lux%lu@%lu, "
            "backbuffer=%s(%lu), texture=%s(%lu)",
            selected_candidate, g_candidates[selected_candidate].mode_index,
            (unsigned long)g_candidates[selected_candidate].mode.Width,
            (unsigned long)g_candidates[selected_candidate].mode.Height,
            (unsigned long)g_candidates[selected_candidate].mode.RefreshRate,
            format_name(g_candidates[selected_candidate].mode.Format),
            (unsigned long)g_candidates[selected_candidate].mode.Format,
            format_name(g_candidates[selected_candidate].texture_format),
            (unsigned long)g_candidates[selected_candidate].texture_format);
    trace_text(line);

    ZeroMemory(&present_parameters, sizeof(present_parameters));
    present_parameters.BackBufferWidth = TEST_CLIENT_WIDTH;
    present_parameters.BackBufferHeight = TEST_CLIENT_HEIGHT;
    present_parameters.BackBufferFormat =
            g_candidates[selected_candidate].mode.Format;
    present_parameters.BackBufferCount = 1;
    present_parameters.MultiSampleType = D3DMULTISAMPLE_NONE;
    present_parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present_parameters.hDeviceWindow = hwnd;
    present_parameters.Windowed = TRUE;
    present_parameters.EnableAutoDepthStencil = FALSE;
    /* Gr2D selects a refresh-rate-bearing mode, but D3D8 requires zero here
     * for its windowed presentation path. */
    present_parameters.FullScreen_RefreshRateInHz = 0;
    present_parameters.FullScreen_PresentationInterval =
            D3DPRESENT_INTERVAL_DEFAULT;

    trace_text("06 CreateDevice flags=0x46 "
            "(FPU_PRESERVE|MULTITHREADED|HARDWARE_VERTEXPROCESSING)");
    hardware_hr = IDirect3D8_CreateDevice(g_d3d, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, hwnd, MAPLE_CREATE_HARDWARE,
            &present_parameters, &g_device);
    trace_hresult("06 CreateDevice(flags=0x46)", hardware_hr);

    if (FAILED(hardware_hr))
    {
        trace_text("07 hardware-VP attempt failed; applying Maple fallback");
        software_hr = IDirect3D8_CreateDevice(g_d3d, D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL, hwnd, MAPLE_CREATE_SOFTWARE,
                &present_parameters, &g_device);
        trace_hresult("07 CreateDevice(flags=0x26 "
                "FPU_PRESERVE|MULTITHREADED|SOFTWARE_VERTEXPROCESSING)",
                software_hr);
        hr = software_hr;
    }
    else
    {
        trace_text("07 CreateDevice(flags=0x26) not attempted: "
                "0x46 succeeded");
        hr = hardware_hr;
    }
    if (FAILED(hr)) return hr;

    ZeroMemory(&creation_parameters, sizeof(creation_parameters));
    hr = IDirect3DDevice8_GetCreationParameters(g_device,
            &creation_parameters);
    trace_hresult("08 GetCreationParameters", hr);
    if (SUCCEEDED(hr))
    {
        wsprintfA(line,
                "08 actual BehaviorFlags=0x%08lX, AdapterOrdinal=%u, "
                "DeviceType=%lu",
                (unsigned long)creation_parameters.BehaviorFlags,
                creation_parameters.AdapterOrdinal,
                (unsigned long)creation_parameters.DeviceType);
        trace_text(line);
    }

    hr = run_render_probe();
    if (FAILED(hr))
        return hr;

    if (enum_failure_count)
    {
        wsprintfA(line,
                "RESULT WARNING: %u EnumAdapterModes calls failed and "
                "were skipped",
                enum_failure_count);
        trace_text(line);
    }

    wsprintfA(line,
            "RESULT PASS: %u modes, %u valid 800x600 candidates; "
            "selected candidate[%u], CreateDevice flags=0x%02lX",
            mode_count, candidate_count, selected_candidate,
            (unsigned long)(SUCCEEDED(hardware_hr)
                    ? MAPLE_CREATE_HARDWARE : MAPLE_CREATE_SOFTWARE));
    trace_text(line);
    return D3D_OK;
}

static void paint_report(HWND hwnd)
{
    PAINTSTRUCT paint;
    HDC dc;
    RECT client;
    HBRUSH background;
    HFONT font;
    HFONT old_font;
    UINT i;
    int y = 8;
    char dropped[96];

    dc = BeginPaint(hwnd, &paint);
    GetClientRect(hwnd, &client);
    background = CreateSolidBrush(RGB(8, 12, 22));
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(210, 226, 245));
    font = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
    old_font = (HFONT)SelectObject(dc, font);
    for (i = 0; i < g_report_line_count; ++i)
    {
        UINT index = (g_report_line_start + i) % REPORT_LINE_COUNT;
        TextOutA(dc, 8, y, g_report_lines[index],
                lstrlenA(g_report_lines[index]));
        y += 16;
    }
    if (g_report_lines_dropped)
    {
        wsprintfA(dropped,
                "... %u additional lines are in OutputDebugString ...",
                g_report_lines_dropped);
        SetTextColor(dc, RGB(255, 205, 80));
        TextOutA(dc, 8, y, dropped, lstrlenA(dropped));
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    EndPaint(hwnd, &paint);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message,
        WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
        case WM_PAINT:
            paint_report(hwnd);
            return 0;

        case WM_DESTROY:
            release_d3d8();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

static int run_test(HINSTANCE instance, int show_command)
{
    WNDCLASSA window_class;
    RECT window_rect;
    MSG message;
    HRESULT hr;
    char title[180];

    ZeroMemory(&window_class, sizeof(window_class));
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.lpszClassName = g_window_class;
    if (!RegisterClassA(&window_class))
    {
        OutputDebugStringA(
                "[d3d8-maple-gr2d] RegisterClassA failed\r\n");
        return 1;
    }

    SetRect(&window_rect, 0, 0, 1040, 660);
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);
    g_window = CreateWindowA(g_window_class,
            "MapleStory v83 Gr2D regression: starting",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            window_rect.right - window_rect.left,
            window_rect.bottom - window_rect.top,
            NULL, NULL, instance, NULL);
    if (!g_window)
    {
        OutputDebugStringA(
                "[d3d8-maple-gr2d] CreateWindowA failed\r\n");
        return 2;
    }

    ShowWindow(g_window, show_command);
    UpdateWindow(g_window);
    hr = run_gr2d_regression(g_window);
    wsprintfA(title, "MapleStory v83 Gr2D regression: %s (0x%08lX)",
            SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    SetWindowTextA(g_window, title);
    InvalidateRect(g_window, NULL, TRUE);
    UpdateWindow(g_window);

    if (FAILED(hr))
    {
        MessageBoxA(g_window,
                "The MapleStory v83 Gr2D regression failed.\r\n\r\n"
                "The exact stage and HRESULT are in this window and guest "
                "debug output under [d3d8-maple-gr2d].",
                "MapleStory Gr2D regression", MB_OK | MB_ICONERROR);
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
    ExitProcess((UINT)result);
}
