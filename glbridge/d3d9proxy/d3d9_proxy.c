/*
 * App-local Direct3D 9 frontend for Windows XP guests running in v86.
 *
 * Independent from d3d8.dll: separate COM objects, separate D9WG protocol
 * (d3d9_protocol.h), separate host executor. A game directory loads exactly
 * one of d3d8.dll/d3d9.dll/opengl32.dll -- never more than one.
 *
 * Scope as of M3 (see docs/d3d9-webgpu-implementation-plan.zh-CN.md section
 * 15): device/resource lifecycle, vertex declarations (plus the common FVF
 * combinations translated to an equivalent declaration per section 4.3),
 * vertex/index buffers, 2D and cube textures, render targets and depth
 * surfaces, the full fixed-function draw path (lighting, the texture-blending
 * cascade, coordinate generation and transforms, fog, alpha test, scissor),
 * shader model 1.1-3.0 through the host translator, state blocks, and
 * conservative occlusion/event queries.
 *
 * Deliberately still unimplemented, each returning D3DERR_INVALIDCALL or
 * D3DERR_NOTAVAILABLE rather than pretending: volume textures, user clip
 * planes (caps report zero of them, so nothing asks), SetStreamSourceFreq
 * instancing, ProcessVertices, additional swap chains, palettes, patches, and
 * GetRenderTargetData/GetFrontBufferData for GPU-produced pixels (plan 2.2).
 *
 * The discipline throughout: an entry point either does what D3D9 says or
 * fails, and GetDeviceCaps only claims what is actually implemented. Where an
 * approximation is unavoidable it is exposed through the host's bounded
 * getStats() counters.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d9.h>
#include <stdint.h>
#ifdef D9WG_DIAGNOSTIC_TRACE
#include <stdarg.h>
#endif
#include "../winproxy/v86gl_ioctl.h"
#include "d3d9_protocol.h"

#ifdef D9WG_DIAGNOSTIC_TRACE
/*
 * Small, CRT-free first-chance diagnostic trace. This is compiled only by
 * build_diagnostic.sh; the ordinary d3d9.dll has no file I/O or VEH overhead.
 * Every line is one WriteFile call.  Ordinary calls are flushed in batches so
 * tracing does not perturb the game as much; an exception and process detach
 * explicitly flush their complete diagnostic tail.
 */
static HANDLE g_trace_file = INVALID_HANDLE_VALUE;
static PVOID g_trace_veh;
static volatile LONG g_trace_in_veh;
static volatile LONG g_trace_call_sequence;
static volatile LONG g_trace_last_enter_sequence;
static volatile LONG g_trace_last_exit_sequence;
static volatile LONG g_trace_last_hresult;
static volatile LONG g_trace_last_output;
static const char *g_trace_last_enter_method = "none";
static const char *g_trace_last_exit_method = "none";
static HMODULE g_trace_exe_module;
static HMODULE g_trace_self_module;
static char g_trace_exe_path[MAX_PATH];
static char g_trace_self_path[MAX_PATH];
static volatile LONG g_trace_vb_count;
static volatile LONG g_trace_ib_count;
static volatile LONG g_trace_vb_bytes;
static volatile LONG g_trace_ib_bytes;
static volatile LONG g_trace_last_freed_surface;
/* The device window, so PROCESS_DETACH can say whether it outlived the
 * process. A title that quit because its window was destroyed and the message
 * pump drained a WM_QUIT looks identical, from D3D9's side, to one that called
 * ExitProcess outright -- this is the bit that tells the two apart. */
static HWND g_trace_device_window;
#define D9_TRACE_MAX_RANGES 4096
typedef struct D9TraceRange {
    const char *kind;
    DWORD ordinal;
    DWORD object;
    DWORD start;
    DWORD end;
} D9TraceRange;
static D9TraceRange g_trace_ranges[D9_TRACE_MAX_RANGES];
static volatile LONG g_trace_range_count;

static void trace_open(HINSTANCE instance)
{
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(instance, path, MAX_PATH);

    g_trace_self_module = (HMODULE)instance;
    g_trace_exe_module = GetModuleHandleA(NULL);
    ZeroMemory(g_trace_self_path, sizeof(g_trace_self_path));
    ZeroMemory(g_trace_exe_path, sizeof(g_trace_exe_path));
    GetModuleFileNameA(g_trace_self_module, g_trace_self_path,
            MAX_PATH - 1);
    GetModuleFileNameA(g_trace_exe_module, g_trace_exe_path, MAX_PATH - 1);
    if (!length || length >= MAX_PATH)
        return;
    while (length && path[length - 1] != '\\' && path[length - 1] != '/')
        --length;
    if (length + sizeof("d3d9_trace_4294967295.log") > MAX_PATH)
        return;
    wsprintfA(path + length, "d3d9_trace_%lu.log", GetCurrentProcessId());
    g_trace_file = CreateFileA(path, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL);
}

static void trace_write(const char *format, ...)
{
    char message[768];
    char line[896];
    DWORD written;
    va_list arguments;

    if (g_trace_file == INVALID_HANDLE_VALUE)
        return;
    va_start(arguments, format);
    wvsprintfA(message, format, arguments);
    va_end(arguments);
    wsprintfA(line, "[%08lX %lu:%lu] %s\r\n", GetTickCount(),
            GetCurrentProcessId(), GetCurrentThreadId(), message);
    WriteFile(g_trace_file, line, (DWORD)lstrlenA(line), &written, NULL);
}

static void trace_flush(void)
{
    if (g_trace_file != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_trace_file);
}

static void trace_mark_enter(const char *method)
{
    LONG sequence = InterlockedIncrement(&g_trace_call_sequence);
    g_trace_last_enter_method = method;
    InterlockedExchange(&g_trace_last_enter_sequence, sequence);
}

static void trace_mark_exit(const char *method, HRESULT result,
        const void *output)
{
    LONG sequence = InterlockedIncrement(&g_trace_call_sequence);
    g_trace_last_exit_method = method;
    InterlockedExchange(&g_trace_last_hresult, (LONG)result);
    InterlockedExchange(&g_trace_last_output, (LONG)(uintptr_t)output);
    InterlockedExchange(&g_trace_last_exit_sequence, sequence);
}

static void trace_buffer_created(BOOL index_buffer, UINT length)
{
    if (index_buffer) {
        InterlockedIncrement(&g_trace_ib_count);
        InterlockedExchangeAdd(&g_trace_ib_bytes, (LONG)length);
    } else {
        InterlockedIncrement(&g_trace_vb_count);
        InterlockedExchangeAdd(&g_trace_vb_bytes, (LONG)length);
    }
}

static void trace_register_range(const char *kind, DWORD ordinal,
        const void *object, const void *start, DWORD byte_count)
{
    LONG slot;
    D9TraceRange *range;
    DWORD address = (DWORD)(uintptr_t)start;

    if (!start || !byte_count)
        return;
    slot = InterlockedIncrement(&g_trace_range_count) - 1;
    if (slot < 0 || slot >= D9_TRACE_MAX_RANGES)
        return;
    range = &g_trace_ranges[slot];
    range->ordinal = ordinal;
    range->object = (DWORD)(uintptr_t)object;
    range->start = address;
    range->end = address + byte_count;
    range->kind = kind;
}

static void trace_match_address(const char *label, DWORD address)
{
    LONG count = InterlockedCompareExchange(&g_trace_range_count, 0, 0);
    LONG index;
    BOOL matched = FALSE;

    if (count > D9_TRACE_MAX_RANGES)
        count = D9_TRACE_MAX_RANGES;
    for (index = 0; index < count; ++index) {
        D9TraceRange *range = &g_trace_ranges[index];
        if (!range->kind || address < range->start || address >= range->end)
            continue;
        trace_write("POINTER_MATCH label=%s address=%08lX kind=%s "
                "ordinal=%lu object=%08lX range=%08lX-%08lX offset=%lu",
                label, address, range->kind, range->ordinal, range->object,
                range->start, range->end, address - range->start);
        matched = TRUE;
    }
    if (!matched)
        trace_write("POINTER_MATCH label=%s address=%08lX kind=none",
                label, address);
}

static void trace_hex_line(const char *label, DWORD address, const BYTE *data)
{
    trace_write("%s %08lX: %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X", label, address,
            data[0], data[1], data[2], data[3], data[4], data[5], data[6],
            data[7], data[8], data[9], data[10], data[11], data[12], data[13],
            data[14], data[15]);
}

static void trace_memory_block(const char *label, DWORD address)
{
    BYTE data[64];
    SIZE_T received = 0;

    ZeroMemory(data, sizeof(data));
    if (!address || !ReadProcessMemory(GetCurrentProcess(),
            (LPCVOID)(uintptr_t)address, data, sizeof(data), &received)
            || received != sizeof(data)) {
        trace_write("%s read failed address=%08lX got=%lu error=%lu", label,
                address, (DWORD)received, GetLastError());
        return;
    }
    trace_hex_line(label, address, data);
    trace_hex_line(label, address + 16u, data + 16);
    trace_hex_line(label, address + 32u, data + 32);
    trace_hex_line(label, address + 48u, data + 48);
}

/*
 * The last marked method to be entered and the last to be left. Written on the
 * way out of the process as well as on an exception: a title that decides to
 * quit rather than crash leaves no other record of where it got to, which is
 * how GTA San Andreas's exit looked from the trace -- a gap and then
 * PROCESS_DETACH.
 */
/*
 * Whether the device window outlived the process, and who holds the foreground.
 * Kept out of trace_last_call() because that one also runs from the vectored
 * exception handler, where calling into USER32 risks faulting a second time --
 * on a stack overflow especially. This is only meaningful at exit anyway.
 */
static void trace_window_state(void)
{
    HWND window = g_trace_device_window;
    BOOL alive = window && IsWindow(window);

    trace_write("WINDOW hwnd=%08lX alive=%lu visible=%lu foreground=%08lX "
            "active=%08lX", (DWORD)(uintptr_t)window, (DWORD)alive,
            (DWORD)(alive && IsWindowVisible(window)),
            (DWORD)(uintptr_t)GetForegroundWindow(),
            (DWORD)(uintptr_t)GetActiveWindow());
}

static void trace_last_call(const char *reason)
{
    trace_write("LAST reason=%s enter_seq=%ld enter=%s exit_seq=%ld exit=%s "
            "hr=%08lX out=%08lX vb=%ld/%luB ib=%ld/%luB", reason,
            InterlockedCompareExchange(&g_trace_last_enter_sequence, 0, 0),
            g_trace_last_enter_method,
            InterlockedCompareExchange(&g_trace_last_exit_sequence, 0, 0),
            g_trace_last_exit_method,
            (DWORD)InterlockedCompareExchange(&g_trace_last_hresult, 0, 0),
            (DWORD)InterlockedCompareExchange(&g_trace_last_output, 0, 0),
            InterlockedCompareExchange(&g_trace_vb_count, 0, 0),
            (DWORD)InterlockedCompareExchange(&g_trace_vb_bytes, 0, 0),
            InterlockedCompareExchange(&g_trace_ib_count, 0, 0),
            (DWORD)InterlockedCompareExchange(&g_trace_ib_bytes, 0, 0));
}

static LONG WINAPI trace_vectored_exception(PEXCEPTION_POINTERS pointers)
{
    EXCEPTION_RECORD *record;
    CONTEXT *context;
    ULONG_PTR parameter0 = 0;
    ULONG_PTR parameter1 = 0;
    BOOL deep_dump = FALSE;

    if (!pointers || !pointers->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&g_trace_in_veh, 1, 0))
        return EXCEPTION_CONTINUE_SEARCH;
    record = pointers->ExceptionRecord;
    context = pointers->ContextRecord;
    if (record->NumberParameters > 0)
        parameter0 = record->ExceptionInformation[0];
    if (record->NumberParameters > 1)
        parameter1 = record->ExceptionInformation[1];
#if defined(__i386__) || defined(_M_IX86)
    if (context) {
        trace_write("EXCEPTION code=%08lX flags=%08lX address=%08lX "
                "params=%lu p0=%08lX p1=%08lX eip=%08lX esp=%08lX "
                "ebp=%08lX eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX "
                "esi=%08lX edi=%08lX",
                record->ExceptionCode, record->ExceptionFlags,
                (DWORD)(uintptr_t)record->ExceptionAddress,
                record->NumberParameters, (DWORD)parameter0,
                (DWORD)parameter1, context->Eip, context->Esp, context->Ebp,
                context->Eax, context->Ebx, context->Ecx, context->Edx,
                context->Esi, context->Edi);
        deep_dump = record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
                || record->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION;
    } else
#endif
    {
        trace_write("EXCEPTION code=%08lX flags=%08lX address=%08lX "
                "params=%lu p0=%08lX p1=%08lX",
                record->ExceptionCode, record->ExceptionFlags,
                (DWORD)(uintptr_t)record->ExceptionAddress,
                record->NumberParameters, (DWORD)parameter0,
                (DWORD)parameter1);
    }
    trace_last_call("exception");
#if defined(__i386__) || defined(_M_IX86)
    if (context && deep_dump
            && record->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
        MEMORY_BASIC_INFORMATION memory;
        SIZE_T queried;
        BYTE code[48];
        SIZE_T received = 0;
        HANDLE process = GetCurrentProcess();
        DWORD code_start = context->Eip >= 16 ? context->Eip - 16
                : context->Eip;
        DWORD stack_values[16];
        DWORD stack_valid = 0;
        DWORD index;
        DWORD outer_field_18 = 0;
        DWORD candidate_vtbl = 0;

        ZeroMemory(&memory, sizeof(memory));
        queried = VirtualQuery((LPCVOID)(uintptr_t)context->Eip, &memory,
                sizeof(memory));
        if (queried) {
            DWORD allocation = (DWORD)(uintptr_t)memory.AllocationBase;
            const char *owner = "unknown";
            if (memory.AllocationBase == g_trace_exe_module)
                owner = "exe";
            else if (memory.AllocationBase == g_trace_self_module)
                owner = "proxy";
            trace_write("EIP_REGION owner=%s base=%08lX allocation=%08lX rva=%08lX "
                    "size=%08lX state=%08lX protect=%08lX type=%08lX",
                    owner, (DWORD)(uintptr_t)memory.BaseAddress, allocation,
                    context->Eip - allocation, (DWORD)memory.RegionSize,
                    memory.State, memory.Protect, memory.Type);
        } else {
            trace_write("EIP_REGION VirtualQuery failed error=%lu",
                    GetLastError());
        }

        ZeroMemory(code, sizeof(code));
        if (ReadProcessMemory(process, (LPCVOID)(uintptr_t)code_start, code,
                sizeof(code), &received) && received == sizeof(code)) {
            trace_hex_line("CODE", code_start, code);
            trace_hex_line("CODE", code_start + 16, code + 16);
            trace_hex_line("CODE", code_start + 32, code + 32);
        } else {
            trace_write("CODE read failed start=%08lX got=%lu error=%lu",
                    code_start, (DWORD)received, GetLastError());
        }

        ZeroMemory(stack_values, sizeof(stack_values));
        for (index = 0; index < 16; ++index) {
            DWORD value = 0;
            received = 0;
            if (ReadProcessMemory(process,
                    (LPCVOID)(uintptr_t)(context->Esp + index * 4u), &value,
                    sizeof(value), &received) && received == sizeof(value)) {
                stack_values[index] = value;
                stack_valid |= 1u << index;
            }
        }
        trace_write("STACK esp=%08lX valid=%04lX +00=%08lX +04=%08lX +08=%08lX +0C=%08lX",
                context->Esp, stack_valid, stack_values[0], stack_values[1],
                stack_values[2], stack_values[3]);
        trace_write("STACK esp=%08lX +10=%08lX +14=%08lX +18=%08lX +1C=%08lX",
                context->Esp, stack_values[4], stack_values[5],
                stack_values[6], stack_values[7]);
        trace_write("STACK esp=%08lX +20=%08lX +24=%08lX +28=%08lX +2C=%08lX",
                context->Esp, stack_values[8], stack_values[9],
                stack_values[10], stack_values[11]);
        trace_write("STACK esp=%08lX +30=%08lX +34=%08lX +38=%08lX +3C=%08lX",
                context->Esp, stack_values[12], stack_values[13],
                stack_values[14], stack_values[15]);

        received = 0;
        if (ReadProcessMemory(process,
                (LPCVOID)(uintptr_t)(context->Eax + 0x18u), &outer_field_18,
                sizeof(outer_field_18), &received)
                && received == sizeof(outer_field_18)) {
            received = 0;
            if (!ReadProcessMemory(process,
                    (LPCVOID)(uintptr_t)outer_field_18, &candidate_vtbl,
                    sizeof(candidate_vtbl), &received)
                    || received != sizeof(candidate_vtbl))
                candidate_vtbl = 0;
            trace_write("OBJECT_CHAIN outer=%08lX outer_plus_18=%08lX "
                    "candidate_vtbl=%08lX ecx=%08lX last_freed_surface=%08lX "
                    "candidate_matches_last_free=%lu",
                    context->Eax, outer_field_18, candidate_vtbl,
                    context->Ecx,
                    (DWORD)InterlockedCompareExchange(
                            &g_trace_last_freed_surface, 0, 0),
                    outer_field_18 == (DWORD)InterlockedCompareExchange(
                            &g_trace_last_freed_surface, 0, 0));
        } else {
            trace_write("OBJECT_CHAIN outer=%08lX +18 read failed got=%lu error=%lu",
                    context->Eax, (DWORD)received, GetLastError());
        }
        trace_memory_block("EAX_OBJECT", context->Eax);
        if (outer_field_18 && outer_field_18 != context->Eax)
            trace_memory_block("FIELD18_OBJECT", outer_field_18);
        if (context->Ecx && context->Ecx != outer_field_18
                && context->Ecx != context->Eax)
            trace_memory_block("ECX_OBJECT", context->Ecx);
        trace_match_address("EAX", context->Eax);
        trace_match_address("FIELD18", outer_field_18);
        if (context->Ecx != outer_field_18)
            trace_match_address("ECX", context->Ecx);

        {
            DWORD frame = context->Ebp;
            for (index = 0; index < 8 && frame; ++index) {
                DWORD pair[2];
                DWORD next;
                received = 0;
                if (frame & 3u || !ReadProcessMemory(process,
                        (LPCVOID)(uintptr_t)frame, pair, sizeof(pair), &received)
                        || received != sizeof(pair))
                    break;
                trace_write("FRAME %lu ebp=%08lX return=%08lX next=%08lX",
                        index, frame, pair[1], pair[0]);
                if (pair[1] >= 16u)
                    trace_memory_block("RETURN_CODE", pair[1] - 16u);
                next = pair[0];
                if (next <= frame || next - frame > 1024u * 1024u)
                    break;
                frame = next;
            }
        }
    }
#endif
    trace_flush();
    InterlockedExchange(&g_trace_in_veh, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

/*
 * Window-message tracing.
 *
 * D3D9 tracing has taken GTA San Andreas as far as it can: every call the game
 * makes succeeds, and then the process exits with its window already destroyed
 * and the device never released. Whatever ends the game is a window message, so
 * that is what has to be watched next -- WM_CLOSE/WM_DESTROY name what tore the
 * window down, WM_QUIT names what drained the message pump, and
 * WM_ACTIVATEAPP/WM_DISPLAYCHANGE/WM_SIZE would implicate this proxy's own
 * fullscreen setup, which calls ChangeDisplaySettings and SetWindowPos on the
 * app's window from inside CreateDevice.
 *
 * Both hooks are thread-local (this thread, this process) and diagnostic-only:
 * the ordinary d3d9.dll installs nothing.
 */
static HHOOK g_trace_callwnd_hook;
static HHOOK g_trace_getmsg_hook;

static BOOL trace_message_is_interesting(UINT message)
{
    switch (message) {
    case WM_CLOSE:
    case WM_DESTROY:
    case WM_NCDESTROY:
    case WM_QUIT:
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_SHOWWINDOW:
    case WM_SIZE:
    case WM_MOVE:
    case WM_WINDOWPOSCHANGED:
    case WM_DISPLAYCHANGE:
    case WM_SYSCOMMAND:
    case WM_QUERYENDSESSION:
    case WM_ENDSESSION:
        return TRUE;
    default:
        return FALSE;
    }
}

static LRESULT CALLBACK trace_call_wnd_proc(int code, WPARAM wparam,
        LPARAM lparam)
{
    if (code == HC_ACTION && lparam) {
        const CWPSTRUCT *sent = (const CWPSTRUCT *)lparam;
        if (trace_message_is_interesting(sent->message))
            trace_write("MSG sent hwnd=%08lX msg=%04lX wparam=%08lX "
                    "lparam=%08lX", (DWORD)(uintptr_t)sent->hwnd,
                    (DWORD)sent->message, (DWORD)sent->wParam,
                    (DWORD)sent->lParam);
    }
    return CallNextHookEx(g_trace_callwnd_hook, code, wparam, lparam);
}

static LRESULT CALLBACK trace_get_message(int code, WPARAM wparam,
        LPARAM lparam)
{
    /* WM_QUIT only ever arrives through the pump, never through SendMessage,
     * so it is visible here and nowhere else. */
    if (code == HC_ACTION && lparam) {
        const MSG *posted = (const MSG *)lparam;
        if (trace_message_is_interesting(posted->message))
            trace_write("MSG posted hwnd=%08lX msg=%04lX wparam=%08lX "
                    "lparam=%08lX", (DWORD)(uintptr_t)posted->hwnd,
                    (DWORD)posted->message, (DWORD)posted->wParam,
                    (DWORD)posted->lParam);
    }
    return CallNextHookEx(g_trace_getmsg_hook, code, wparam, lparam);
}

static void trace_install_message_hooks(void)
{
    DWORD thread;

    if (g_trace_callwnd_hook || g_trace_getmsg_hook)
        return;
    thread = GetCurrentThreadId();
    g_trace_callwnd_hook = SetWindowsHookExA(WH_CALLWNDPROC,
            trace_call_wnd_proc, NULL, thread);
    g_trace_getmsg_hook = SetWindowsHookExA(WH_GETMESSAGE, trace_get_message,
            NULL, thread);
    trace_write("HOOKS thread=%lu callwnd=%08lX getmsg=%08lX", thread,
            (DWORD)(uintptr_t)g_trace_callwnd_hook,
            (DWORD)(uintptr_t)g_trace_getmsg_hook);
}

static void trace_remove_message_hooks(void)
{
    if (g_trace_callwnd_hook) {
        UnhookWindowsHookEx(g_trace_callwnd_hook);
        g_trace_callwnd_hook = NULL;
    }
    if (g_trace_getmsg_hook) {
        UnhookWindowsHookEx(g_trace_getmsg_hook);
        g_trace_getmsg_hook = NULL;
    }
}

static void trace_close(void)
{
    if (g_trace_file != INVALID_HANDLE_VALUE) {
        trace_flush();
        CloseHandle(g_trace_file);
        g_trace_file = INVALID_HANDLE_VALUE;
    }
}

#define TRACE(...) trace_write(__VA_ARGS__)
#define TRACE_FLUSH() trace_flush()
#define TRACE_MARK_ENTER(method) trace_mark_enter(method)
#define TRACE_MARK_EXIT(method, result, output) \
    trace_mark_exit(method, result, output)
#define TRACE_BUFFER_CREATED(index_buffer, length) \
    trace_buffer_created(index_buffer, length)
#define TRACE_SURFACE_FREED(surface) \
    InterlockedExchange(&g_trace_last_freed_surface, \
            (LONG)(uintptr_t)(surface))
#define TRACE_REGISTER_RANGE(kind, ordinal, object, start, size) \
    trace_register_range(kind, ordinal, object, start, (DWORD)(size))
#else
#define TRACE(...) ((void)0)
#define TRACE_FLUSH() ((void)0)
#define TRACE_MARK_ENTER(method) ((void)0)
#define TRACE_MARK_EXIT(method, result, output) ((void)0)
#define TRACE_BUFFER_CREATED(index_buffer, length) ((void)0)
#define TRACE_SURFACE_FREED(surface) ((void)0)
#define TRACE_REGISTER_RANGE(kind, ordinal, object, start, size) ((void)0)
#endif

#define D9_MAX_RENDER_STATES 256u
#define D9_MAX_TEXTURE_STAGES 8u
#define D9_MAX_TEXTURE_STAGE_STATES 33u
#define D9_MAX_STREAMS 4u
#define D9_MAX_TRANSFORMS 512u
#define D9_MAX_LIGHTS 8u
#define D9_MAX_SAMPLERS 16u
/* fill_caps() reports NumSimultaneousRTs; MRT slots beyond 0 are only bindable
 * because a translated pixel shader can write oC1..oC3. */
#define D9_MAX_RENDER_TARGETS 4u
#define D9_MAX_CLIP_PLANES 6u
#define D9_MAX_SAMPLER_STATES 14u
/* Constant-register file sizes for the shader model fill_caps() reports.
 * vs_2_0 guarantees 256 float constants and ps_2_0 guarantees 32; the 224
 * here is the ps_3_0 ceiling, so the shadow is large enough for any shader
 * the host translator will accept without needing a second size later. */
#define D9_MAX_VS_CONST_F 256u
#define D9_MAX_PS_CONST_F 224u
#define D9_MAX_CONST_I 16u
#define D9_MAX_CONST_B 16u
/* A shader's token stream carries no length: the app hands over a bare
 * pointer and the terminator has to be found by walking it. This bounds that
 * walk so a malformed/non-shader pointer cannot read arbitrarily far past the
 * app's allocation. Real D3D9 shaders are limited to 512 (vs_2_0) / 32768
 * (vs_3_0 with flow control) instruction slots, so this is far above any
 * legitimate input. */
#define D9_MAX_SHADER_TOKENS 65536u

/*
 * Reported adapter identity (see d3d_get_adapter_identifier).
 *
 * This is deliberately a single switchable block because it is a *test
 * variable*, not a settled design decision. Warcraft III was observed
 * calling GetAdapterIdentifier and then releasing IDirect3D9 without ever
 * asking for caps, formats or a device -- which leaves two live
 * explanations: either it gates on recognising the hardware, or it only
 * wanted the video card name and never intended to render through D3D9 at
 * all. Reporting a GPU the game certainly knows discriminates between them:
 * if it still walks away, hardware recognition is definitively not the
 * gate.
 *
 * The identity must stay consistent with what fill_caps() reports, because a
 * game that recognises the card knows what that card can do. M1 paired
 * D9_ADAPTER_GEFORCE4_MX (NV17: hardware T&L, no programmable shaders) with
 * VertexShaderVersion/PixelShaderVersion = 0.0. M2 implements SM2.0, so the
 * default moved to D9_ADAPTER_GEFORCEFX_5200 (NV34), the entry-level card of
 * the first NVIDIA generation with vs_2_0/ps_2_0 -- claiming a GeForce4 MX
 * while advertising shader model 2.0 is exactly the inconsistency an engine's
 * hardware-detection table would trip over.
 *
 * Set D9_ADAPTER_IDENTITY to D9_ADAPTER_NATIVE to go back to advertising
 * ourselves honestly once the question is settled.
 */
#define D9_ADAPTER_NATIVE       0
#define D9_ADAPTER_GEFORCE4_MX  1
#define D9_ADAPTER_VMWARE_SVGA  2
#define D9_ADAPTER_GEFORCEFX_5200 3

#define D9_ADAPTER_IDENTITY D9_ADAPTER_GEFORCEFX_5200
#define D9_VGL2_RECORD_HEADER_BYTES 8u
#define D9_HANDLE_GENERATION_ONE (1u << 20)

typedef struct D9Direct3D D9Direct3D;
typedef struct D9Device D9Device;
typedef struct D9SwapChain D9SwapChain;
typedef struct D9VertexBuffer D9VertexBuffer;
typedef struct D9IndexBuffer D9IndexBuffer;
typedef struct D9Texture D9Texture;
typedef struct D9VertexDeclaration D9VertexDeclaration;
typedef struct D9Surface D9Surface;
typedef struct D9Shader D9Shader;
typedef struct D9CubeTexture D9CubeTexture;
typedef struct D9StateBlock D9StateBlock;
typedef struct D9Query D9Query;

typedef struct D9TextureLevel {
    BYTE *shadow;
    /* The one surface GetSurfaceLevel hands out for this level, created on
     * first request and destroyed with the texture.  D3D9 makes a level
     * surface a sub-object of its texture rather than a free-standing COM
     * object: the same pointer comes back every call, and its refcount is the
     * texture's, so an app may legally drop its surface reference and keep
     * using the pointer for as long as it still holds the texture.  Kart Rider
     * does exactly that -- GetSurfaceLevel, LockRect, Release, UnlockRect --
     * and a per-call surface that frees itself at refcount 0 turns that into a
     * call through a freed vtable. */
    D9Surface *level_surface;
    /* TRUE only when every pixel in shadow is known. Render targets allocate
     * this lazily for M4 Clear/ColorFill/copy readback and invalidate it on any
     * GPU draw, so GetRenderTargetData can return known content without ever
     * fabricating GPU-produced pixels. */
    BOOL shadow_valid;
    UINT width;
    UINT height;
    UINT row_pitch;
    UINT row_count;
    UINT byte_count;
    RECT lock_rect;
    DWORD lock_flags;
    BOOL locked;
} D9TextureLevel;

typedef struct D9StreamBinding {
    D9VertexBuffer *buffer;
    UINT stride;
    UINT offset; /* SetStreamSource OffsetInBytes */
} D9StreamBinding;

struct D9Direct3D {
    IDirect3D9 iface;
    LONG refcount;
};

/*
 * The primary swap chain is created implicitly with the device.  Keep its
 * COM identity embedded in D9Device so repeated GetSwapChain(0) calls return
 * the same object, just as the native runtime does.  The device itself does
 * not own a COM reference to this object (that would form a cycle); every
 * external swap-chain reference holds one ordinary device reference instead.
 */
struct D9SwapChain {
    IDirect3DSwapChain9 iface;
    LONG refcount;
    D9Device *device;
};

struct D9Device {
    IDirect3DDevice9 iface;
    D9SwapChain implicit_swap_chain;
    LONG refcount;
    LONG child_parent_refs;
    BOOL releasing_owned_refs;
    D9Direct3D *parent;
    uint32_t handle;
    D3DDEVICE_CREATION_PARAMETERS creation;
    D3DPRESENT_PARAMETERS present;
    D3DDISPLAYMODE display_mode;
    D3DVIEWPORT9 viewport;
    float transforms[D9_MAX_TRANSFORMS][16];
    DWORD render_states[D9_MAX_RENDER_STATES];
    DWORD texture_stage_states[D9_MAX_TEXTURE_STAGES][D9_MAX_TEXTURE_STAGE_STATES];
    DWORD sampler_states[D9_MAX_SAMPLERS][D9_MAX_SAMPLER_STATES];
    D3DMATERIAL9 material;
    D3DLIGHT9 lights[D9_MAX_LIGHTS];
    BOOL light_set[D9_MAX_LIGHTS];
    BOOL light_enabled[D9_MAX_LIGHTS];
    D9StreamBinding streams[D9_MAX_STREAMS];
    D9IndexBuffer *index_buffer;
    D9Texture *textures[D9_MAX_TEXTURE_STAGES];
    /* Parallel to `textures`: a stage holds either a 2D or a cube texture, and
     * exactly one of the two slots is non-NULL. Keeping them apart rather than
     * behind a tagged union keeps the release paths type-correct. */
    D9CubeTexture *cube_bindings[D9_MAX_TEXTURE_STAGES];
    DWORD fvf;
    D9VertexDeclaration *vertex_declaration;
    BOOL in_scene;
    D9VertexBuffer *vertex_buffers;
    D9IndexBuffer *index_buffers;
    D9Texture *texture_resources;
    D9CubeTexture *cube_textures;
    D9VertexDeclaration *vertex_declarations;
    D9Shader *shaders;
    D9StateBlock *state_blocks;
    D9Query *queries;
    /* Non-NULL between BeginStateBlock and EndStateBlock: `recording` is the
     * pre-Begin snapshot used to restore live device state, while
     * `recording_result` receives an explicit mask entry for every Set* call.
     * Keeping the call mask is essential: setting a state to its current value
     * is still part of a D3D9 state block. */
    D9StateBlock *recording;
    D9StateBlock *recording_result;
    /* Explicitly bound targets. A NULL slot 0 means the implicit back buffer,
     * which is what a device renders to until the app says otherwise. */
    D9Surface *render_target_surfaces[D9_MAX_RENDER_TARGETS];
    D9Surface *depth_stencil_surface;
    BOOL depth_stencil_unbound;
    /*
     * The implicit back buffer and the auto depth-stencil. D3D9 creates both
     * along with the device and hands the *same* object back from every
     * GetBackBuffer / GetDepthStencilSurface call, so they are cached here and
     * freed only in device teardown. Allocating a fresh surface per call
     * instead broke pointer identity -- and worse, releasing that surface freed
     * the block, which the heap then handed straight back out as an unrelated
     * resource while the application was still holding the pointer.
     */
    D9Surface *implicit_back_buffer;
    D9Surface *implicit_depth_stencil;
    RECT scissor_rect;
    BOOL scissor_set;
    D9Shader *vertex_shader;
    D9Shader *pixel_shader;
    BOOL cursor_ready;
    BOOL cursor_visible;
    /* Set once the application drives the D3D9 hardware cursor itself, which
     * takes precedence over the GDI capture fallback. */
    BOOL app_cursor;
    HCURSOR system_cursor;
    BOOL display_mode_changed;
    BOOL window_state_sent;
    /* Rate limiting for maintain_fullscreen_foreground(). */
    DWORD last_foreground_claim;
    DWORD foreground_claims;
    uint32_t last_window_flags;
    uint32_t last_foreground;
    /*
     * D3D9's constant registers are device state, not shader state: they
     * survive SetVertexShader and are what the app expects to still be there
     * after a Reset. Holding the authoritative copy here (rather than only on
     * the host) is what lets set_shader_constant_f() suppress the very common
     * "set the same matrix palette again every frame" traffic and lets
     * recreate_device_resources() replay the live values after a Reset.
     */
    float vs_const_f[D9_MAX_VS_CONST_F][4];
    int vs_const_i[D9_MAX_CONST_I][4];
    BOOL vs_const_b[D9_MAX_CONST_B];
    float ps_const_f[D9_MAX_PS_CONST_F][4];
    int ps_const_i[D9_MAX_CONST_I][4];
    BOOL ps_const_b[D9_MAX_CONST_B];
    uint32_t reset_epoch;
};

/*
 * IDirect3DVertexShader9 and IDirect3DPixelShader9 have byte-for-byte
 * identical vtable layouts (IUnknown + GetDevice + GetFunction), so one
 * struct backs both; only `is_pixel` and which vtable pointer is installed
 * differ. The guest keeps the raw token stream and never interprets it
 * beyond counting tokens and hashing (plan 4.2) -- all translation happens
 * host-side in d3d9_shader_pipeline.js.
 */
struct D9Shader {
    union {
        IDirect3DVertexShader9 vertex;
        IDirect3DPixelShader9 pixel;
    } iface;
    LONG refcount;
    D9Device *device;
    uint32_t handle;
    BOOL is_pixel;
    DWORD *code;
    UINT token_count;
    uint32_t hash_low;
    uint32_t hash_high;
    D9Shader *next_device_resource;
};

struct D9VertexBuffer {
    IDirect3DVertexBuffer9 iface;
    LONG refcount;
    D9Device *device;
    uint32_t handle;
    BYTE *shadow;
    UINT length;
    DWORD usage;
    DWORD fvf;
    D3DPOOL pool;
    DWORD priority;
    UINT lock_offset;
    UINT lock_size;
    DWORD lock_flags;
    BOOL locked;
    D9VertexBuffer *next_device_resource;
};

struct D9IndexBuffer {
    IDirect3DIndexBuffer9 iface;
    LONG refcount;
    D9Device *device;
    uint32_t handle;
    BYTE *shadow;
    UINT length;
    DWORD usage;
    D3DFORMAT format;
    D3DPOOL pool;
    DWORD priority;
    UINT lock_offset;
    UINT lock_size;
    DWORD lock_flags;
    BOOL locked;
    D9IndexBuffer *next_device_resource;
};

struct D9Texture {
    IDirect3DTexture9 iface;
    LONG refcount;
    D9Device *device;
    uint32_t handle;
    UINT width;
    UINT height;
    UINT level_count;
    DWORD usage;
    D3DFORMAT format;
    D3DPOOL pool;
    DWORD priority;
    DWORD lod;
    D9TextureLevel *levels;
    D9Texture *next_device_resource;
};

struct D9CubeTexture {
    IDirect3DCubeTexture9 iface;
    LONG refcount;
    D9Device *device;
    uint32_t handle;
    UINT edge_length;
    UINT level_count;
    DWORD usage;
    D3DFORMAT format;
    D3DPOOL pool;
    DWORD priority;
    DWORD lod;
    D9TextureLevel *levels; /* face * level_count + level */
    D9CubeTexture *next_device_resource;
};

static IDirect3DCubeTexture9Vtbl g_cube_vtbl;

/* D3DMAXDECLLENGTH (18) bounds the app-supplied element array; +0 is enough
 * since we never append our own sentinel back into this array. */
struct D9VertexDeclaration {
    IDirect3DVertexDeclaration9 iface;
    LONG refcount;
    D9Device *device;
    uint32_t handle;
    UINT element_count;
    D3DVERTEXELEMENT9 elements[D3DMAXDECLLENGTH];
    D9VertexDeclaration *next_device_resource;
};

/* GetBackBuffer's return value. M1 gives this real GetDesc() dimensions/
 * format (real games have been observed gating an entire render branch on
 * GetBackBuffer succeeding, even when they never actually read pixels back
 * from it), but it is not backed by any GPU resource: LockRect/GetDC honestly
 * fail rather than claim readback support the plan's non-goals (2.2) exclude
 * from M1. It is re-obtained fresh from device state on every call rather
 * than cached, so it never needs Reset-time recreation. */
struct D9Surface {
    IDirect3DSurface9 iface;
    LONG refcount;
    D9Device *device;
    /* Non-NULL for a surface obtained from IDirect3DTexture9::GetSurfaceLevel:
     * the surface is then just a view onto that texture level, and its
     * LockRect/UnlockRect share the texture's shadow storage and upload path.
     * NULL for the GetBackBuffer surface, which is not backed by anything. */
    D9Texture *texture;
    /* TRUE when this surface is one of the texture's level sub-objects, held
     * in D9TextureLevel::level_surface and living exactly as long as the
     * texture.  Its AddRef/Release forward to the texture and it is freed only
     * during texture teardown.  FALSE for the surface create_target_texture
     * builds, which points at a texture too but owns it the other way round:
     * that surface holds the texture's only reference and takes its own
     * refcount, so forwarding there would be a cycle. */
    BOOL texture_child;
    /* Non-NULL for the implicit back buffer.  This is a non-owning pointer:
     * the surface's tracked device reference already keeps the embedded swap
     * chain alive, while GetContainer takes a fresh public chain reference. */
    D9SwapChain *swap_chain;
    UINT level;
    UINT width;
    UINT height;
    D3DFORMAT format;
    /* Non-NULL for a standalone CPU surface from
     * CreateOffscreenPlainSurface: it owns its pixels and has no GPU resource
     * behind it at all. That is exactly what a cursor bitmap is -- the app
     * builds one, fills it, hands it to SetCursorProperties, and the surface
     * itself is never rendered with. */
    BYTE *shadow;
    UINT row_pitch;
    UINT byte_count;
    BOOL locked;
    /* TRUE for the surface GetDepthStencilSurface hands back for the device's
     * implicit auto depth-stencil. It has no texture and no pixels; its only
     * job is to be passed back to SetDepthStencilSurface, which is how an app
     * restores depth after a render-to-texture pass. */
    BOOL auto_depth_stencil;
};

/*
 * TRUE for the two surfaces the device owns outright rather than creating on
 * demand: the implicit back buffer and the auto depth-stencil. Each of the two
 * fields tested here is written by exactly one place, so the pair already
 * identifies them and no extra flag is needed.
 *
 * Such a surface behaves like a texture level surface: AddRef/Release adjust
 * the parent's reference count, and the object itself survives to the parent's
 * teardown, so an app is free to drop its reference and keep using the pointer.
 */
static BOOL surface_is_implicit(const D9Surface *surface)
{
    return surface->swap_chain != NULL || surface->auto_depth_stencil;
}

/* BeginStateBlock records calls, including calls which write the value the
 * device already has.  The block structure itself lives near its COM methods;
 * these helpers keep its layout out of the ordinary Set* implementations. */
static void state_block_record_render_state(D9Device *device, UINT state);
static void state_block_record_texture_stage_state(D9Device *device,
        UINT stage, UINT state);
static void state_block_record_sampler_state(D9Device *device,
        UINT sampler, UINT state);
static void state_block_record_transform(D9Device *device, UINT state);
static void state_block_record_texture(D9Device *device, UINT stage);
static void state_block_record_material(D9Device *device);
static void state_block_record_light(D9Device *device, UINT index);
static void state_block_record_light_enable(D9Device *device, UINT index);
static void state_block_record_viewport(D9Device *device);
static void state_block_record_scissor(D9Device *device);
static void state_block_record_vertex_shader(D9Device *device);
static void state_block_record_pixel_shader(D9Device *device);
static void state_block_record_constant(D9Device *device, BOOL is_pixel,
        BOOL is_float, BOOL is_bool, UINT start, UINT count);
static void state_block_record_vertex_format(D9Device *device);
static void state_block_record_stream(D9Device *device, UINT stream);
static void state_block_record_indices(D9Device *device);

static IDirect3D9Vtbl g_d3d_vtbl;
static IDirect3DDevice9Vtbl g_device_vtbl;
static IDirect3DSwapChain9Vtbl g_swap_chain_vtbl;
static IDirect3DVertexBuffer9Vtbl g_vb_vtbl;
static IDirect3DIndexBuffer9Vtbl g_ib_vtbl;
static IDirect3DTexture9Vtbl g_texture_vtbl;
static IDirect3DCubeTexture9Vtbl g_cube_vtbl;
static IDirect3DVertexDeclaration9Vtbl g_decl_vtbl;
static IDirect3DSurface9Vtbl g_surface_vtbl;
static IDirect3DVertexShader9Vtbl g_vertex_shader_vtbl;
static IDirect3DPixelShader9Vtbl g_pixel_shader_vtbl;

static void device_clear_bindings(D9Device *device);
static BOOL device_has_reset_blockers(D9Device *device);
static void device_release_owned_references(D9Device *device);
static BOOL recreate_device_resources(D9Device *device);
static BOOL emit_cube_texture_create(D9Device *device, D9CubeTexture *texture);
static BOOL emit_cube_texture_update(D9CubeTexture *texture, UINT face,
        UINT level, const RECT *rect);
static void device_child_add_ref(D9Device *device);
static void device_child_release(D9Device *device);
static BOOL shader_model_enabled(void);
static D9Surface *surface_from_iface(IDirect3DSurface9 *iface)
{
    return (D9Surface *)iface;
}
static void emit_cursor_position(D9Device *device, int x, int y, DWORD flags);
static void update_system_cursor(D9Device *device, HWND window);
static void emit_window_state(D9Device *device, HWND window);
static void claim_fullscreen_foreground(D9Device *device, HWND window);
static void maintain_fullscreen_foreground(D9Device *device, HWND window);
static void restore_display_mode(D9Device *device);

static D9Direct3D *d3d_from_iface(IDirect3D9 *iface)
{
    return (D9Direct3D *)iface;
}

static D9Device *device_from_iface(IDirect3DDevice9 *iface)
{
    return (D9Device *)iface;
}

static D9SwapChain *swap_chain_from_iface(IDirect3DSwapChain9 *iface)
{
    return (D9SwapChain *)iface;
}

static D9VertexBuffer *vb_from_iface(IDirect3DVertexBuffer9 *iface)
{
    return (D9VertexBuffer *)iface;
}

static D9IndexBuffer *ib_from_iface(IDirect3DIndexBuffer9 *iface)
{
    return (D9IndexBuffer *)iface;
}

static D9Texture *texture_from_iface(IDirect3DTexture9 *iface)
{
    return (D9Texture *)iface;
}

static D9VertexDeclaration *decl_from_iface(IDirect3DVertexDeclaration9 *iface)
{
    return (D9VertexDeclaration *)iface;
}

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

static BOOL iid_is_unknown(REFIID iid)
{
    static const IID unknown = { 0x00000000, 0x0000, 0x0000,
        { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
    return guid_equal(iid, &unknown);
}

static HINSTANCE g_module_instance;

/*
 * D9WG_DUMP_SHADERS=1: write every shader's raw token stream to disk, next to
 * this DLL, named by its bytecode hash.
 *
 * The point is corpus, not diagnostics. The translator in
 * d3d9_shader_pipeline.js is hand-written, so its only correctness evidence is
 * this repository's own tests -- and those run on shaders *we* assembled by
 * hand, which is exactly the wrong sample: hand-written test bytecode contains
 * the instruction encodings we already thought of. A real game's shaders
 * contain the ones we did not.
 *
 * Every D3D9 game is a corpus contributor whether or not it is playable here:
 * a client that only reaches its main menu before wedging still creates its
 * full shader set during startup, so a few minutes in v86 yields a permanent
 * regression corpus that can be replayed offline through
 * d3d9_shader_pipeline_test.js and naga without launching anything.
 *
 * Files are named d3d9_dump/{vs,ps}_<major><minor>_<hash16>.d9sh and are pure
 * DWORD token streams (no header), the exact bytes CreateVertexShader received,
 * so the offline harness can feed them straight to compileShader(). Naming by
 * content hash makes the write idempotent: a game that re-creates the same
 * shader every level load writes one file.
 */
static BOOL g_dump_shaders_checked;
static BOOL g_dump_shaders_enabled;
static char g_dump_shaders_directory[MAX_PATH];

/* Fills g_dump_shaders_directory with "<dll dir>\d3d9_dump\" and creates it.
 * It is anchored to the module because a game may SetCurrentDirectory during
 * startup, and a dump nobody can find is worse than no dump. */
static BOOL dump_shaders_enabled(void)
{
    char value[8];
    DWORD length;
    DWORD index;

    if (g_dump_shaders_checked)
        return g_dump_shaders_enabled;
    g_dump_shaders_checked = TRUE;
    length = GetEnvironmentVariableA("D9WG_DUMP_SHADERS", value, sizeof(value));
    if (length != 1 || value[0] != '1')
        return FALSE;

    length = GetModuleFileNameA(g_module_instance, g_dump_shaders_directory,
            sizeof(g_dump_shaders_directory) - 16);
    if (!length || length >= sizeof(g_dump_shaders_directory) - 16)
        return FALSE;
    for (index = length; index > 0; --index) {
        if (g_dump_shaders_directory[index - 1] == '\\'
                || g_dump_shaders_directory[index - 1] == '/')
            break;
    }
    lstrcpynA(g_dump_shaders_directory + index, "d3d9_dump\\",
            (int)(sizeof(g_dump_shaders_directory) - index));
    /* Strip the trailing separator for CreateDirectoryA, then keep it for the
     * per-file path concatenation below. */
    g_dump_shaders_directory[index + 9] = '\0';
    if (!CreateDirectoryA(g_dump_shaders_directory, NULL)
            && GetLastError() != ERROR_ALREADY_EXISTS)
        return FALSE;
    g_dump_shaders_directory[index + 9] = '\\';
    g_dump_shaders_directory[index + 10] = '\0';
    g_dump_shaders_enabled = TRUE;
    return TRUE;
}

static void dump_shader_bytecode(const DWORD *code, UINT token_count,
        BOOL is_pixel, uint32_t hash_low, uint32_t hash_high)
{
    char path[MAX_PATH];
    char name[64];
    HANDLE file;
    DWORD written;

    if (!dump_shaders_enabled())
        return;
    wsprintfA(name, "%s_%lu%lu_%08lX%08lX.d9sh", is_pixel ? "ps" : "vs",
            (unsigned long)((code[0] >> 8) & 0xFFu),
            (unsigned long)(code[0] & 0xFFu),
            (unsigned long)hash_high, (unsigned long)hash_low);
    if (lstrlenA(g_dump_shaders_directory) + lstrlenA(name) >= (int)sizeof(path))
        return;
    lstrcpynA(path, g_dump_shaders_directory, sizeof(path));
    lstrcatA(path, name);
    /* CREATE_NEW rather than CREATE_ALWAYS: the name is a content hash, so an
     * existing file already holds these exact bytes and rewriting it every
     * time the game re-creates the shader is pure I/O in a startup path. */
    file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    WriteFile(file, code, token_count * 4u, &written, NULL);
    CloseHandle(file);
}

static uint32_t g_next_handle = D9_HANDLE_GENERATION_ONE;

static uint32_t allocate_handle(void)
{
    uint32_t handle = (uint32_t)InterlockedIncrement((LONG *)&g_next_handle);
    if (!handle)
        handle = (uint32_t)InterlockedIncrement((LONG *)&g_next_handle);
    return handle;
}

/*
 * Shaders draw from a disjoint numeric range with bit 0 always set (see
 * D9WG_SHADER_HANDLE_BASE in d3d9_protocol.h), stepping by two so that
 * property holds for every handle. The host keeps one flat resource table,
 * so this is what guarantees a shader handle can never alias a buffer,
 * texture or declaration handle allocated by allocate_handle().
 */
static uint32_t g_next_shader_handle = D9WG_SHADER_HANDLE_BASE;

static uint32_t allocate_shader_handle(void)
{
    return (uint32_t)InterlockedExchangeAdd((LONG *)&g_next_shader_handle, 2);
}

/* ---- VGL2 transport / D9WG batch buffer ---- */

static HANDLE g_transport = INVALID_HANDLE_VALUE;
static uint8_t *g_dma_buffer;
static uint32_t g_dma_capacity;
static uint32_t g_batch_bytes;
static uint32_t g_command_count;
static uint32_t g_frame_id = 1;
static uint32_t g_sequence = 1;
static uint32_t g_session_id_low;
static uint32_t g_session_id_high;
static BOOL g_transport_failed;
static BOOL g_hello_emitted;
static CRITICAL_SECTION g_transport_lock;

static uint8_t *batch_base(void)
{
    return g_dma_buffer + sizeof(V86GLDMADesc) + D9_VGL2_RECORD_HEADER_BYTES;
}

static uint32_t batch_capacity(void)
{
    if (g_dma_capacity <= sizeof(V86GLDMADesc) + D9_VGL2_RECORD_HEADER_BYTES)
        return 0;
    return g_dma_capacity - (uint32_t)sizeof(V86GLDMADesc)
            - D9_VGL2_RECORD_HEADER_BYTES;
}

static void reset_batch_locked(void)
{
    D9WGBatchHeader *header;

    g_batch_bytes = sizeof(D9WGBatchHeader);
    g_command_count = 0;
    if (!g_dma_buffer || batch_capacity() < sizeof(D9WGBatchHeader))
        return;

    header = (D9WGBatchHeader *)batch_base();
    ZeroMemory(header, sizeof(*header));
    header->magic = D9WG_MAGIC;
    header->version_major = D9WG_VERSION_MAJOR;
    header->version_minor = D9WG_VERSION_MINOR;
    header->frame_id = g_frame_id;
    header->session_id_low = g_session_id_low;
    header->session_id_high = g_session_id_high;
}

static void close_transport_locked(void)
{
    DWORD returned = 0;

    if (g_transport != INVALID_HANDLE_VALUE) {
        if (g_dma_buffer) {
            DeviceIoControl(g_transport, V86GL_IOCTL_UNMAP_BUFFER,
                    NULL, 0, NULL, 0, &returned, NULL);
        }
        CloseHandle(g_transport);
    }
    g_transport = INVALID_HANDLE_VALUE;
    g_dma_buffer = NULL;
    g_dma_capacity = 0;
    g_batch_bytes = 0;
    g_command_count = 0;
}

static BOOL open_transport_locked(void)
{
    V86GLMapBuffer mapping;
    DWORD returned = 0;

    if (g_dma_buffer)
        return TRUE;
    if (g_transport_failed)
        return FALSE;

    g_transport = CreateFileA(V86GL_DEVICE_DOS_NAME,
            GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_transport == INVALID_HANDLE_VALUE) {
        g_transport_failed = TRUE;
        return FALSE;
    }

    ZeroMemory(&mapping, sizeof(mapping));
    if (!DeviceIoControl(g_transport, V86GL_IOCTL_MAP_BUFFER,
            NULL, 0, &mapping, sizeof(mapping), &returned, NULL)
            || returned != sizeof(mapping)
            || !mapping.user_address
            || mapping.buffer_bytes < sizeof(V86GLDMADesc)
                    + D9_VGL2_RECORD_HEADER_BYTES
                    + sizeof(D9WGBatchHeader)
                    + sizeof(D9WGCommandHeader)) {
        close_transport_locked();
        g_transport_failed = TRUE;
        return FALSE;
    }

    g_dma_buffer = (uint8_t *)(uintptr_t)mapping.user_address;
    g_dma_capacity = mapping.buffer_bytes;
    reset_batch_locked();
    return TRUE;
}

static BOOL submit_batch_locked(BOOL present)
{
    V86GLDMADesc *descriptor;
    D9WGBatchHeader *batch;
    V86GLSubmit submit;
    uint8_t *outer;
    uint32_t outer_bytes;
    DWORD returned = 0;

    if (!open_transport_locked())
        return FALSE;
    if (g_command_count == 0)
        return TRUE;

    batch = (D9WGBatchHeader *)batch_base();
    batch->frame_id = g_frame_id;
    batch->flags = present ? D9WG_BATCH_FLAG_PRESENT : 0;
    batch->command_count = g_command_count;
    batch->command_bytes = g_batch_bytes - sizeof(*batch);
    batch->session_id_low = g_session_id_low;
    batch->session_id_high = g_session_id_high;

    outer = g_dma_buffer + sizeof(V86GLDMADesc);
    outer[0] = (uint8_t)(V86GL_CTRL_D3D9_BATCH & 0xFFu);
    outer[1] = (uint8_t)(V86GL_CTRL_D3D9_BATCH >> 8);
    outer[2] = 0xFF;
    outer[3] = 0xFF;
    outer[4] = (uint8_t)(g_batch_bytes & 0xFFu);
    outer[5] = (uint8_t)((g_batch_bytes >> 8) & 0xFFu);
    outer[6] = (uint8_t)((g_batch_bytes >> 16) & 0xFFu);
    outer[7] = (uint8_t)((g_batch_bytes >> 24) & 0xFFu);

    outer_bytes = D9_VGL2_RECORD_HEADER_BYTES + g_batch_bytes;
    descriptor = (V86GLDMADesc *)g_dma_buffer;
    descriptor->magic = V86GL_MAGIC;
    descriptor->version = V86GL_VERSION;
    descriptor->flags = 0;
    descriptor->frame_id = g_frame_id;
    descriptor->command_count = 1;
    descriptor->command_bytes = outer_bytes;
    descriptor->reserved0 = D9WG_MAGIC;
    descriptor->reserved1 = 0;

    submit.descriptor_bytes = (uint32_t)sizeof(*descriptor) + outer_bytes;
    submit.flags = 0;
    if (!DeviceIoControl(g_transport, V86GL_IOCTL_SUBMIT,
            &submit, sizeof(submit), NULL, 0, &returned, NULL)) {
        close_transport_locked();
        g_transport_failed = TRUE;
        return FALSE;
    }

    if (present)
        ++g_frame_id;
    reset_batch_locked();
    return TRUE;
}

static BOOL reserve_command_locked(uint16_t opcode, uint32_t payload_bytes,
        uint32_t extra_bytes, D9WGCommandHeader **command_out,
        uint8_t **payload_out, uint8_t **extra_out)
{
    uint32_t raw_size;
    uint32_t record_size;
    D9WGCommandHeader *command;

    if (!open_transport_locked())
        return FALSE;
    if (payload_bytes > 0xFFFFFFFFu - sizeof(*command) - extra_bytes)
        return FALSE;
    raw_size = (uint32_t)sizeof(*command) + payload_bytes + extra_bytes;
    record_size = D9WG_ALIGN8(raw_size);
    if (record_size > batch_capacity() - sizeof(D9WGBatchHeader))
        return FALSE;
    if (g_batch_bytes + record_size > batch_capacity()
            && !submit_batch_locked(FALSE))
        return FALSE;

    command = (D9WGCommandHeader *)(batch_base() + g_batch_bytes);
    ZeroMemory(command, record_size);
    command->opcode = opcode;
    command->size = record_size;
    command->sequence = g_sequence++;
    if (command_out)
        *command_out = command;
    if (payload_out)
        *payload_out = (uint8_t *)(command + 1);
    if (extra_out)
        *extra_out = (uint8_t *)(command + 1) + payload_bytes;
    g_batch_bytes += record_size;
    ++g_command_count;
    /*
     * This is the single chokepoint every host-bound call passes through, so
     *
     *     TRACE("CMD opcode=%04lX seq=%lu bytes=%lu pending=%lu",
     *             (DWORD)opcode, (DWORD)command->sequence, record_size,
     *             g_command_count);
     *
     * here covers in one line the ~100 device methods that have no trace of
     * their own -- SetRenderState, SetTextureStageState, SetTransform and the
     * rest -- which is what identified where GTA San Andreas stopped during
     * startup. It is not compiled in because it is one WriteFile per command:
     * invaluable up to the first frame, unusable once a title is drawing.
     * Opcodes are numeric; d3d9_protocol.h decodes them.
     */
    return TRUE;
}

static BOOL emit_command(uint16_t opcode, const void *payload,
        uint32_t payload_bytes)
{
    uint8_t *destination;
    BOOL result;

    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(opcode, payload_bytes, 0, NULL,
            &destination, NULL);
    if (result && payload_bytes)
        CopyMemory(destination, payload, payload_bytes);
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

/*
 * Reports a refusal or failure to the host so it lands in the browser console.
 *
 * This exists because the guest's own trace file is written inside a VM whose
 * filesystem the developer cannot reach from the page, so until now the one
 * fact that ends a "the picture is wrong" investigation -- the app asked for
 * something and was told no -- was recorded only where nobody could read it.
 * Compiled into the ordinary DLL, not just the diagnostic one: needing a
 * special build to find out that a call was refused is most of the problem.
 *
 * Deduplicated by exact text. A failure inside a per-frame path would otherwise
 * bill one message per frame forever, which costs batch space and drowns the
 * console; the first occurrence is the informative one. The table is small and
 * never grows past its cap, so a pathological caller cannot leak through it.
 */
/* Bumped whenever guest-visible behaviour changes, so the console can say
 * which DLL is actually loaded rather than leaving it to be inferred. */
#define D9_PROXY_BUILD "guest-log-20260816"

#define D9_HOSTLOG_MAX_DISTINCT 128u

static char g_hostlog_seen[D9_HOSTLOG_MAX_DISTINCT][D9WG_LOG_MAX_TEXT];
static UINT g_hostlog_seen_count;
static BOOL g_hostlog_overflowed;

static BOOL hostlog_already_sent(const char *text)
{
    UINT index;
    for (index = 0; index < g_hostlog_seen_count; ++index) {
        if (lstrcmpA(g_hostlog_seen[index], text) == 0)
            return TRUE;
    }
    if (g_hostlog_seen_count < D9_HOSTLOG_MAX_DISTINCT) {
        lstrcpynA(g_hostlog_seen[g_hostlog_seen_count], text,
                (int)D9WG_LOG_MAX_TEXT);
        ++g_hostlog_seen_count;
        return FALSE;
    }
    /* Past the cap every further distinct message would be re-sent forever;
     * say so once and go quiet rather than flooding. */
    if (g_hostlog_overflowed)
        return TRUE;
    g_hostlog_overflowed = TRUE;
    return FALSE;
}

static void host_log(uint32_t severity, const char *format, ...)
{
    char text[D9WG_LOG_MAX_TEXT];
    uint8_t payload[sizeof(D9WGGuestLog) + D9WG_LOG_MAX_TEXT];
    D9WGGuestLog *header = (D9WGGuestLog *)payload;
    va_list arguments;
    int length;

    va_start(arguments, format);
    wvsprintfA(text, format, arguments);
    va_end(arguments);
    if (hostlog_already_sent(text))
        return;
    length = lstrlenA(text);
    if (length > (int)D9WG_LOG_MAX_TEXT)
        length = (int)D9WG_LOG_MAX_TEXT;
    header->severity = severity;
    header->text_bytes = (uint32_t)length;
    CopyMemory(payload + sizeof(*header), text, (SIZE_T)length);
    emit_command(D9WG_OP_GUEST_LOG, payload,
            (uint32_t)(sizeof(*header) + (uint32_t)length));
}

#define HOSTLOG_INFO(...) \
    host_log(D9WG_LOG_SEVERITY_INFO, __VA_ARGS__)
#define HOSTLOG_REFUSED(...) \
    host_log(D9WG_LOG_SEVERITY_REFUSED, __VA_ARGS__)
#define HOSTLOG_FAILED(...) \
    host_log(D9WG_LOG_SEVERITY_FAILED, __VA_ARGS__)

static BOOL emit_buffer_update(uint32_t handle, uint32_t destination_offset,
        const void *data, uint32_t byte_count, uint32_t lock_flags)
{
    D9WGUpdateBuffer update;
    uint8_t *payload;
    uint8_t *blob;
    BOOL result;

    ZeroMemory(&update, sizeof(update));
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D9WG_OP_UPDATE_BUFFER,
            sizeof(update), byte_count, NULL, &payload, &blob);
    if (result) {
        update.resource_handle = handle;
        update.destination_offset = destination_offset;
        update.byte_count = byte_count;
        update.data_offset = (uint32_t)(blob - batch_base());
        update.lock_flags = lock_flags;
        CopyMemory(payload, &update, sizeof(update));
        if (byte_count)
            CopyMemory(blob, data, byte_count);
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

/* ---- format / geometry helpers (format-agnostic; mirrors d3d8proxy) ---- */

static BOOL multiply_u32(UINT left, UINT right, UINT *result)
{
    if (left && right > 0xFFFFFFFFu / left)
        return FALSE;
    *result = left * right;
    return TRUE;
}

static BOOL texture_format_layout(D3DFORMAT format, UINT *block_width,
        UINT *block_height, UINT *block_bytes)
{
    *block_width = 1;
    *block_height = 1;
    switch (format) {
    case D3DFMT_R8G8B8:
        *block_bytes = 3;
        return TRUE;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_X8L8V8U8:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16:
    case D3DFMT_A2W10V10U10:
        *block_bytes = 4;
        return TRUE;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8R3G3B2:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8:
    case D3DFMT_V8U8:
    case D3DFMT_L6V5U5:
    case D3DFMT_CxV8U8:
    case D3DFMT_L16:
        *block_bytes = 2;
        return TRUE;
    case D3DFMT_R3G3B2:
    case D3DFMT_L8:
    case D3DFMT_A8:
    case D3DFMT_A4L4:
        *block_bytes = 1;
        return TRUE;
    case D3DFMT_DXT1:
        *block_width = 4;
        *block_height = 4;
        *block_bytes = 8;
        return TRUE;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        *block_width = 4;
        *block_height = 4;
        *block_bytes = 16;
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL texture_level_layout(D3DFORMAT format, UINT width, UINT height,
        UINT *row_pitch, UINT *row_count, UINT *byte_count)
{
    UINT block_width;
    UINT block_height;
    UINT block_bytes;
    UINT columns;

    if (!texture_format_layout(format, &block_width, &block_height,
            &block_bytes))
        return FALSE;
    columns = (width + block_width - 1u) / block_width;
    *row_count = (height + block_height - 1u) / block_height;
    return multiply_u32(columns, block_bytes, row_pitch)
            && multiply_u32(*row_pitch, *row_count, byte_count);
}

static BOOL supported_texture_format(D3DFORMAT format)
{
    switch (format) {
    case D3DFMT_R8G8B8:
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_R3G3B2:
    case D3DFMT_A8R3G3B2:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_L8:
    case D3DFMT_A8:
    case D3DFMT_A8L8:
    case D3DFMT_A4L4:
    case D3DFMT_V8U8:
    case D3DFMT_L6V5U5:
    case D3DFMT_X8L8V8U8:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16:
    case D3DFMT_A2W10V10U10:
    case D3DFMT_CxV8U8:
    case D3DFMT_L16:
    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        return TRUE;
    default:
        return FALSE;
    }
}

/* Sampling support and render-target support are deliberately separate.
 * Luminance, signed bump-map and block-compressed textures are valid shader
 * inputs, but not valid outputs for the proxy's RGBA render path. */
static BOOL supported_render_target_format(D3DFORMAT format)
{
    switch (format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL supported_backbuffer_format(D3DFORMAT format)
{
    return format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8
            || format == D3DFMT_R5G6B5;
}

static D9TextureLevel *surface_texture_level(D9Surface *surface)
{
    if (!surface || !surface->texture
            || surface->level >= surface->texture->level_count)
        return NULL;
    return &surface->texture->levels[surface->level];
}

static BOOL ensure_target_shadow(D9Surface *surface)
{
    D9TextureLevel *level = surface_texture_level(surface);
    if (!level || (surface->format != D3DFMT_A8R8G8B8
            && surface->format != D3DFMT_X8R8G8B8))
        return FALSE;
    if (level->shadow)
        return TRUE;
    if (!level->byte_count && !texture_level_layout(surface->format,
            level->width, level->height, &level->row_pitch,
            &level->row_count, &level->byte_count))
        return FALSE;
    level->shadow = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            level->byte_count);
    return level->shadow != NULL;
}

static void mirror_target_color_fill(D9Surface *surface, const RECT *area,
        D3DCOLOR color)
{
    D9TextureLevel *level = surface_texture_level(surface);
    RECT clipped;
    UINT x, y;
    BOOL full;

    if (!level || !area)
        return;
    clipped.left = area->left < 0 ? 0 : area->left;
    clipped.top = area->top < 0 ? 0 : area->top;
    clipped.right = area->right > (LONG)level->width
            ? (LONG)level->width : area->right;
    clipped.bottom = area->bottom > (LONG)level->height
            ? (LONG)level->height : area->bottom;
    if (clipped.right <= clipped.left || clipped.bottom <= clipped.top)
        return;
    full = clipped.left == 0 && clipped.top == 0
            && clipped.right == (LONG)level->width
            && clipped.bottom == (LONG)level->height;
    /* Filling part of an unknown image does not make the other pixels known. */
    if (!full && !level->shadow_valid)
        return;
    if (!ensure_target_shadow(surface))
        return;
    if (surface->format == D3DFMT_X8R8G8B8)
        color |= 0xFF000000u;
    for (y = (UINT)clipped.top; y < (UINT)clipped.bottom; ++y) {
        DWORD *row = (DWORD *)(level->shadow + y * level->row_pitch);
        for (x = (UINT)clipped.left; x < (UINT)clipped.right; ++x)
            row[x] = color;
    }
    if (full)
        level->shadow_valid = TRUE;
}

static void mirror_target_copy(D9Surface *source, const RECT *source_area,
        D9Surface *destination, const RECT *destination_area)
{
    D9TextureLevel *from = surface_texture_level(source);
    D9TextureLevel *to = surface_texture_level(destination);
    UINT width, height, row_bytes, row;
    BOOL destination_full;
    BYTE *temporary = NULL;

    if (!from || !to || !source_area || !destination_area
            || source->format != destination->format
            || (source->format != D3DFMT_A8R8G8B8
                && source->format != D3DFMT_X8R8G8B8)
            || source_area->left < 0 || source_area->top < 0
            || destination_area->left < 0 || destination_area->top < 0
            || source_area->right > (LONG)from->width
            || source_area->bottom > (LONG)from->height
            || destination_area->right > (LONG)to->width
            || destination_area->bottom > (LONG)to->height
            || source_area->right - source_area->left
                    != destination_area->right - destination_area->left
            || source_area->bottom - source_area->top
                    != destination_area->bottom - destination_area->top
            || !from->shadow || !from->shadow_valid) {
        if (to)
            to->shadow_valid = FALSE;
        return;
    }
    destination_full = destination_area->left == 0 && destination_area->top == 0
            && destination_area->right == (LONG)to->width
            && destination_area->bottom == (LONG)to->height;
    if (!destination_full && !to->shadow_valid)
        return;
    if (!ensure_target_shadow(destination)) {
        to->shadow_valid = FALSE;
        return;
    }
    width = (UINT)(source_area->right - source_area->left);
    height = (UINT)(source_area->bottom - source_area->top);
    row_bytes = width * 4u;
    if (from == to) {
        temporary = (BYTE *)HeapAlloc(GetProcessHeap(), 0, row_bytes * height);
        if (!temporary) {
            to->shadow_valid = FALSE;
            return;
        }
        for (row = 0; row < height; ++row)
            CopyMemory(temporary + row * row_bytes,
                    from->shadow + ((UINT)source_area->top + row) * from->row_pitch
                    + (UINT)source_area->left * 4u, row_bytes);
    }
    for (row = 0; row < height; ++row)
        CopyMemory(to->shadow + ((UINT)destination_area->top + row) * to->row_pitch
                + (UINT)destination_area->left * 4u,
                temporary ? temporary + row * row_bytes
                    : from->shadow + ((UINT)source_area->top + row) * from->row_pitch
                        + (UINT)source_area->left * 4u,
                row_bytes);
    if (temporary)
        HeapFree(GetProcessHeap(), 0, temporary);
    if (destination_full)
        to->shadow_valid = TRUE;
}

static void invalidate_render_target_mirrors(D9Device *device)
{
    UINT index;
    for (index = 0; index < D9_MAX_RENDER_TARGETS; ++index) {
        D9TextureLevel *level = surface_texture_level(
                device->render_target_surfaces[index]);
        if (level)
            level->shadow_valid = FALSE;
    }
}

static UINT full_mip_level_count(UINT width, UINT height)
{
    UINT levels = 1;
    while (width > 1 || height > 1) {
        if (width > 1) width >>= 1;
        if (height > 1) height >>= 1;
        ++levels;
    }
    return levels;
}

static BOOL emit_texture_update(D9Texture *texture, UINT level,
        const RECT *rect)
{
    D9TextureLevel *level_data = &texture->levels[level];
    D9WGUpdateTexture update;
    UINT block_width;
    UINT block_height;
    UINT block_bytes;
    UINT block_x;
    UINT block_y;
    UINT row_bytes;
    UINT row_count;
    UINT data_bytes;
    UINT row;
    uint8_t *payload;
    uint8_t *blob;
    BOOL result;

    if (!texture_format_layout(texture->format, &block_width, &block_height,
            &block_bytes))
        return FALSE;
    block_x = (UINT)rect->left / block_width;
    block_y = (UINT)rect->top / block_height;
    if (!multiply_u32(((UINT)(rect->right - rect->left)
            + block_width - 1u) / block_width, block_bytes, &row_bytes))
        return FALSE;
    row_count = ((UINT)(rect->bottom - rect->top)
            + block_height - 1u) / block_height;
    if (!multiply_u32(row_bytes, row_count, &data_bytes))
        return FALSE;

    ZeroMemory(&update, sizeof(update));
    update.resource_handle = texture->handle;
    update.level = level;
    update.x = (uint32_t)rect->left;
    update.y = (uint32_t)rect->top;
    update.z = 0;
    update.width = (uint32_t)(rect->right - rect->left);
    update.height = (uint32_t)(rect->bottom - rect->top);
    update.depth = 1;
    update.row_pitch = row_bytes;
    update.slice_pitch = 0;
    update.data_bytes = data_bytes;

    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D9WG_OP_UPDATE_TEXTURE,
            sizeof(update), data_bytes, NULL, &payload, &blob);
    if (result) {
        update.data_offset = (uint32_t)(blob - batch_base());
        CopyMemory(payload, &update, sizeof(update));
        for (row = 0; row < row_count; ++row) {
            CopyMemory(blob + row * row_bytes,
                    level_data->shadow
                    + (block_y + row) * level_data->row_pitch
                    + block_x * block_bytes, row_bytes);
        }
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

static BOOL primitive_element_count(D3DPRIMITIVETYPE type,
        UINT primitive_count, UINT *element_count)
{
    switch (type) {
    case D3DPT_POINTLIST:
        *element_count = primitive_count;
        return TRUE;
    case D3DPT_LINELIST:
        return multiply_u32(primitive_count, 2u, element_count);
    case D3DPT_LINESTRIP:
        if (primitive_count == 0xFFFFFFFFu)
            return FALSE;
        *element_count = primitive_count + 1u;
        return TRUE;
    case D3DPT_TRIANGLELIST:
        return multiply_u32(primitive_count, 3u, element_count);
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:
        if (primitive_count > 0xFFFFFFFDu)
            return FALSE;
        *element_count = primitive_count + 2u;
        return TRUE;
    default:
        return FALSE;
    }
}

/* ---- FVF -> vertex declaration (plan section 4.3) ---- */

/*
 * Only the position/normal/diffuse/specular/2D-texcoord subset that M1's
 * fixed pipeline understands. Blended positions (XYZB*), pretransformed
 * XYZW, and non-default (1D/3D/4D) texture coordinate sizes are honestly
 * rejected rather than silently truncated -- FALSE means "SetFVF/CreateVertex
 * Buffer(..., this fvf, ...) should fail with D3DERR_INVALIDCALL", not "best
 * effort".
 */
static BOOL fvf_to_declaration(DWORD fvf, D9WGVertexElement *elements,
        UINT *element_count)
{
    UINT count = 0;
    UINT offset = 0;
    UINT tex_count;
    UINT i;

    if (fvf & D3DFVF_RESERVED0)
        return FALSE;
    if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZ) {
        elements[count].stream = 0;
        elements[count].offset = (uint16_t)offset;
        elements[count].type = D3DDECLTYPE_FLOAT3;
        elements[count].method = D3DDECLMETHOD_DEFAULT;
        elements[count].usage = D3DDECLUSAGE_POSITION;
        elements[count].usage_index = 0;
        ++count;
        offset += 12;
    } else if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
        elements[count].stream = 0;
        elements[count].offset = (uint16_t)offset;
        elements[count].type = D3DDECLTYPE_FLOAT4;
        elements[count].method = D3DDECLMETHOD_DEFAULT;
        elements[count].usage = D3DDECLUSAGE_POSITIONT;
        elements[count].usage_index = 0;
        ++count;
        offset += 16;
    } else {
        return FALSE;
    }
    if (fvf & D3DFVF_NORMAL) {
        elements[count].stream = 0;
        elements[count].offset = (uint16_t)offset;
        elements[count].type = D3DDECLTYPE_FLOAT3;
        elements[count].method = D3DDECLMETHOD_DEFAULT;
        elements[count].usage = D3DDECLUSAGE_NORMAL;
        elements[count].usage_index = 0;
        ++count;
        offset += 12;
    }
    if (fvf & D3DFVF_PSIZE) {
        elements[count].stream = 0;
        elements[count].offset = (uint16_t)offset;
        elements[count].type = D3DDECLTYPE_FLOAT1;
        elements[count].method = D3DDECLMETHOD_DEFAULT;
        elements[count].usage = D3DDECLUSAGE_PSIZE;
        elements[count].usage_index = 0;
        ++count;
        offset += 4;
    }
    if (fvf & D3DFVF_DIFFUSE) {
        elements[count].stream = 0;
        elements[count].offset = (uint16_t)offset;
        elements[count].type = D3DDECLTYPE_D3DCOLOR;
        elements[count].method = D3DDECLMETHOD_DEFAULT;
        elements[count].usage = D3DDECLUSAGE_COLOR;
        elements[count].usage_index = 0;
        ++count;
        offset += 4;
    }
    if (fvf & D3DFVF_SPECULAR) {
        elements[count].stream = 0;
        elements[count].offset = (uint16_t)offset;
        elements[count].type = D3DDECLTYPE_D3DCOLOR;
        elements[count].method = D3DDECLMETHOD_DEFAULT;
        elements[count].usage = D3DDECLUSAGE_COLOR;
        elements[count].usage_index = 1;
        ++count;
        offset += 4;
    }
    tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if (tex_count > 8)
        return FALSE;
    for (i = 0; i < tex_count; ++i) {
        DWORD size_bits = (fvf >> (16 + i * 2)) & 0x3u;
        if (size_bits != 0)
            return FALSE; /* non-default (1D/3D/4D) texcoord size: not M1 */
        elements[count].stream = 0;
        elements[count].offset = (uint16_t)offset;
        elements[count].type = D3DDECLTYPE_FLOAT2;
        elements[count].method = D3DDECLMETHOD_DEFAULT;
        elements[count].usage = D3DDECLUSAGE_TEXCOORD;
        elements[count].usage_index = (uint8_t)i;
        ++count;
        offset += 8;
    }
    *element_count = count;
    return TRUE;
}

/* ---- vertex declaration validation ---- */

static BOOL declaration_element_supported(const D3DVERTEXELEMENT9 *e)
{
    if (e->Stream >= D9_MAX_STREAMS || e->Method != D3DDECLMETHOD_DEFAULT)
        return FALSE;
    switch (e->Usage) {
    case D3DDECLUSAGE_POSITION:
    case D3DDECLUSAGE_POSITIONT:
    case D3DDECLUSAGE_NORMAL:
    case D3DDECLUSAGE_COLOR:
    case D3DDECLUSAGE_TEXCOORD:
    case D3DDECLUSAGE_PSIZE:
    /* M2: a programmable vertex shader binds its inputs by semantic, so any
     * usage the host can name is now bindable even though the fixed-function
     * path still ignores most of them. */
    case D3DDECLUSAGE_BLENDWEIGHT:
    case D3DDECLUSAGE_BLENDINDICES:
    case D3DDECLUSAGE_TANGENT:
    case D3DDECLUSAGE_BINORMAL:
    case D3DDECLUSAGE_FOG:
        break;
    default:
        /* TESSFACTOR/DEPTH/SAMPLE have no consumer on this path. */
        return FALSE;
    }
    switch (e->Type) {
    case D3DDECLTYPE_FLOAT1:
    case D3DDECLTYPE_FLOAT2:
    case D3DDECLTYPE_FLOAT3:
    case D3DDECLTYPE_FLOAT4:
    case D3DDECLTYPE_D3DCOLOR:
    /* M5: compact skinning declarations. WebGPU exposes the unnormalised
     * integer formats as integer shader inputs, and UDEC3/DEC3N as a packed
     * uint32; the host builds a declaration-specific WGSL variant which
     * converts each of them to the float4 D3D9 promises in v#. */
    case D3DDECLTYPE_UBYTE4:
    case D3DDECLTYPE_SHORT2:
    case D3DDECLTYPE_SHORT4:
    /* The remaining compact formats D3D9 delivers to a shader as floats and
     * WebGPU has an exact vertex-format equivalent for. */
    case D3DDECLTYPE_UBYTE4N:
    case D3DDECLTYPE_SHORT2N:
    case D3DDECLTYPE_SHORT4N:
    case D3DDECLTYPE_USHORT2N:
    case D3DDECLTYPE_USHORT4N:
    case D3DDECLTYPE_UDEC3:
    case D3DDECLTYPE_DEC3N:
    case D3DDECLTYPE_FLOAT16_2:
    case D3DDECLTYPE_FLOAT16_4:
        return TRUE;
    default:
        return FALSE;
    }
}

/* Scans the app's D3DDECL_END()-terminated array (sentinel Stream==0xFF),
 * bounded by D3DMAXDECLLENGTH so a missing sentinel can never walk off the
 * app's allocation. On success fills `wire` with the exact wire shape
 * CREATE_VERTEX_DECLARATION/SET_FVF send. */
static BOOL parse_vertex_declaration(const D3DVERTEXELEMENT9 *elements,
        D9WGVertexElement *wire, UINT *count_out)
{
    UINT count = 0;
    while (count < D3DMAXDECLLENGTH && elements[count].Stream != 0xFF) {
        const D3DVERTEXELEMENT9 *e = &elements[count];
        if (!declaration_element_supported(e))
            return FALSE;
        wire[count].stream = e->Stream;
        wire[count].offset = e->Offset;
        wire[count].type = e->Type;
        wire[count].method = e->Method;
        wire[count].usage = e->Usage;
        wire[count].usage_index = e->UsageIndex;
        ++count;
    }
    if (count == D3DMAXDECLLENGTH && elements[count].Stream != 0xFF)
        return FALSE;
    *count_out = count;
    return TRUE;
}

/* ---- resource create/update emitters ---- */

static BOOL emit_vertex_buffer_create(D9Device *device, D9VertexBuffer *buffer)
{
    D9WGCreateBuffer command;
    command.device_handle = device->handle;
    command.resource_handle = buffer->handle;
    command.resource_kind = D9WG_RESOURCE_BUFFER_VERTEX;
    command.byte_count = buffer->length;
    command.usage = buffer->usage;
    command.fvf = buffer->fvf;
    command.pool = buffer->pool;
    command.reserved = 0;
    return emit_command(D9WG_OP_CREATE_BUFFER, &command, sizeof(command));
}

static BOOL emit_index_buffer_create(D9Device *device, D9IndexBuffer *buffer)
{
    D9WGCreateBuffer command;
    command.device_handle = device->handle;
    command.resource_handle = buffer->handle;
    command.resource_kind = D9WG_RESOURCE_BUFFER_INDEX;
    command.byte_count = buffer->length;
    command.usage = buffer->usage;
    command.fvf = (uint32_t)buffer->format;
    command.pool = buffer->pool;
    command.reserved = 0;
    return emit_command(D9WG_OP_CREATE_BUFFER, &command, sizeof(command));
}

static BOOL emit_texture_create(D9Device *device, D9Texture *texture)
{
    D9WGCreateTexture2D command;
    command.device_handle = device->handle;
    command.resource_handle = texture->handle;
    command.width = texture->width;
    command.height = texture->height;
    command.level_count = texture->level_count;
    command.format = texture->format;
    command.usage = texture->usage;
    command.pool = texture->pool;
    return emit_command(D9WG_OP_CREATE_TEXTURE_2D, &command, sizeof(command));
}

static BOOL emit_vertex_declaration_create(D9Device *device, uint32_t handle,
        const D9WGVertexElement *elements, UINT count)
{
    D9WGCreateVertexDeclaration command;
    uint8_t *payload;
    uint8_t *blob;
    uint32_t element_bytes = (uint32_t)count * sizeof(D9WGVertexElement);
    BOOL result;

    command.device_handle = device->handle;
    command.resource_handle = handle;
    command.element_count = count;
    command.reserved = 0;
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D9WG_OP_CREATE_VERTEX_DECLARATION,
            sizeof(command), element_bytes, NULL, &payload, &blob);
    if (result) {
        CopyMemory(payload, &command, sizeof(command));
        if (element_bytes)
            CopyMemory(blob, elements, element_bytes);
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

static BOOL emit_set_fvf(D9Device *device, DWORD fvf,
        const D9WGVertexElement *elements, UINT count)
{
    D9WGSetFVF command;
    uint8_t *payload;
    uint8_t *blob;
    uint32_t element_bytes = (uint32_t)count * sizeof(D9WGVertexElement);
    BOOL result;

    command.device_handle = device->handle;
    command.fvf = fvf;
    command.element_count = count;
    command.reserved = 0;
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D9WG_OP_SET_FVF,
            sizeof(command), element_bytes, NULL, &payload, &blob);
    if (result) {
        CopyMemory(payload, &command, sizeof(command));
        if (element_bytes)
            CopyMemory(blob, elements, element_bytes);
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

/* ---- shader bytecode: length, hash, upload ----
 *
 * CreateVertexShader/CreatePixelShader receive a bare `const DWORD *` with no
 * length. The stream has to be walked to its D3DVS_END()/D3DPS_END()
 * terminator (0x0000FFFF) to know how many bytes to copy and send.
 *
 * The walk cannot be a naive "scan for 0x0000FFFF": `def`/`defi` embed four
 * raw literal DWORDs, and an integer constant of 65535 or a denormal float
 * would be indistinguishable from the terminator. Those three opcodes are
 * therefore skipped by their known fixed size. Everything else is safe to
 * step over one token at a time, because every operand token has bit 31 set
 * and so can never be mistaken for the terminator -- which is also why this
 * needs no per-opcode operand table on the guest side. Instruction lengths
 * (SM2.0+) are used when present purely to skip faster.
 */
#define D9_SIO_OPCODE_MASK 0x0000FFFFu
#define D9_SIO_COMMENT 0xFFFEu
#define D9_SIO_END 0xFFFFu
#define D9_SIO_DEF 81u
#define D9_SIO_DEFI 48u
#define D9_SIO_DEFB 47u

static BOOL shader_token_count(const DWORD *code, UINT *count_out,
        BOOL *is_pixel_out)
{
    UINT index;
    DWORD version;
    UINT major;

    if (!code)
        return FALSE;
    version = code[0];
    if ((version >> 16) == 0xFFFEu)
        *is_pixel_out = FALSE;
    else if ((version >> 16) == 0xFFFFu)
        *is_pixel_out = TRUE;
    else
        return FALSE;
    major = (version >> 8) & 0xFFu;
    if (major < 1 || major > 3)
        return FALSE;

    for (index = 1; index < D9_MAX_SHADER_TOKENS; ) {
        DWORD token = code[index];
        DWORD opcode = token & D9_SIO_OPCODE_MASK;

        if (opcode == D9_SIO_END) {
            *count_out = index + 1;
            return TRUE;
        }
        if (opcode == D9_SIO_COMMENT) {
            index += 1 + ((token >> 16) & 0x7FFFu);
            continue;
        }
        if (!(token & 0x80000000u)
                && (opcode == D9_SIO_DEF || opcode == D9_SIO_DEFI)) {
            index += 6; /* instruction + destination + four literals */
            continue;
        }
        if (!(token & 0x80000000u) && opcode == D9_SIO_DEFB) {
            index += 3; /* instruction + destination + one literal */
            continue;
        }
        if (!(token & 0x80000000u) && major >= 2 && ((token >> 24) & 0xFu)) {
            index += 1 + ((token >> 24) & 0xFu);
            continue;
        }
        ++index;
    }
    return FALSE;
}

/* 64-bit FNV-1a over the raw token bytes. Kept byte-oriented (rather than
 * hashing DWORDs directly) so hashTokens() in d3d9_shader_pipeline.js can
 * reproduce it exactly in JavaScript, letting the host reuse the hash the
 * guest already computed as its translation-cache key. */
static void shader_bytecode_hash(const DWORD *code, UINT token_count,
        uint32_t *low_out, uint32_t *high_out)
{
    uint32_t low = 0x84222325u;
    uint32_t high = 0xCBF29CE4u;
    UINT index;
    UINT byte_index;

    for (index = 0; index < token_count; ++index) {
        DWORD token = code[index];
        for (byte_index = 0; byte_index < 4; ++byte_index) {
            uint32_t l0, l1, h0, h1, r0, r1, r2, r3;
            low ^= (token >> (byte_index * 8)) & 0xFFu;
            /* Multiply the 64-bit accumulator by the FNV prime
             * 0x100000001B3 in 16-bit limbs. The 0x100000000 part of the
             * prime contributes `low` into the high half. */
            l0 = low & 0xFFFFu;
            l1 = low >> 16;
            h0 = high & 0xFFFFu;
            h1 = high >> 16;
            r0 = l0 * 0x1B3u;
            r1 = l1 * 0x1B3u + (r0 >> 16);
            r2 = h0 * 0x1B3u + (r1 >> 16) + l0;
            r3 = h1 * 0x1B3u + (r2 >> 16) + l1;
            low = ((r1 & 0xFFFFu) << 16) | (r0 & 0xFFFFu);
            high = ((r3 & 0xFFFFu) << 16) | (r2 & 0xFFFFu);
        }
    }
    *low_out = low;
    *high_out = high;
}

static BOOL emit_shader_create(D9Device *device, D9Shader *shader)
{
    D9WGCreateVertexShader command;
    uint8_t *payload;
    uint8_t *blob;
    uint32_t code_bytes = (uint32_t)shader->token_count * 4u;
    BOOL result;

    ZeroMemory(&command, sizeof(command));
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(shader->is_pixel
                    ? D9WG_OP_CREATE_PIXEL_SHADER
                    : D9WG_OP_CREATE_VERTEX_SHADER,
            sizeof(command), code_bytes, NULL, &payload, &blob);
    if (result) {
        command.device_handle = device->handle;
        command.resource_handle = shader->handle;
        command.instruction_token_count = shader->token_count;
        command.code_offset = (uint32_t)(blob - batch_base());
        command.bytecode_hash_low = shader->hash_low;
        command.bytecode_hash_high = shader->hash_high;
        CopyMemory(payload, &command, sizeof(command));
        CopyMemory(blob, shader->code, code_bytes);
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

/* SET_*_SHADER_CONSTANT_F/I/B all use the same header shape, with the payload
 * blob holding vector_count * 16 bytes (float4 / int4) or, for the bool form,
 * one 32-bit slot per register. */
static BOOL emit_shader_constants(D9Device *device, uint16_t opcode,
        UINT start_register, UINT vector_count, const void *data,
        uint32_t data_bytes)
{
    D9WGSetShaderConstantF command;
    uint8_t *payload;
    uint8_t *blob;
    BOOL result;

    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(opcode, sizeof(command), data_bytes,
            NULL, &payload, &blob);
    if (result) {
        command.device_handle = device->handle;
        command.start_register = start_register;
        command.vector_count = vector_count;
        command.data_offset = (uint32_t)(blob - batch_base());
        CopyMemory(payload, &command, sizeof(command));
        CopyMemory(blob, data, data_bytes);
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

static BOOL emit_draw_primitive_up(const D9WGDrawPrimitiveUP *draw,
        const void *vertices)
{
    D9WGDrawPrimitiveUP payload_value = *draw;
    uint8_t *payload;
    uint8_t *blob;
    BOOL result;

    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D9WG_OP_DRAW_PRIMITIVE_UP,
            sizeof(payload_value), payload_value.vertex_bytes, NULL,
            &payload, &blob);
    if (result) {
        payload_value.vertex_data_offset = (uint32_t)(blob - batch_base());
        CopyMemory(payload, &payload_value, sizeof(payload_value));
        CopyMemory(blob, vertices, payload_value.vertex_bytes);
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

static BOOL emit_draw_indexed_primitive_up(
        const D9WGDrawIndexedPrimitiveUP *draw, const void *indices,
        const void *vertices)
{
    D9WGDrawIndexedPrimitiveUP payload_value = *draw;
    uint32_t extra_bytes;
    uint8_t *payload;
    uint8_t *blob;
    BOOL result;

    if (payload_value.index_bytes > 0xFFFFFFFFu - payload_value.vertex_bytes)
        return FALSE;
    extra_bytes = payload_value.index_bytes + payload_value.vertex_bytes;
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D9WG_OP_DRAW_INDEXED_PRIMITIVE_UP,
            sizeof(payload_value), extra_bytes, NULL, &payload, &blob);
    if (result) {
        payload_value.index_data_offset = (uint32_t)(blob - batch_base());
        payload_value.vertex_data_offset = payload_value.index_data_offset
                + payload_value.index_bytes;
        CopyMemory(payload, &payload_value, sizeof(payload_value));
        CopyMemory(blob, indices, payload_value.index_bytes);
        CopyMemory(blob + payload_value.index_bytes, vertices,
                payload_value.vertex_bytes);
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

/* ---- device lifecycle plumbing ---- */

static void emit_hello_once(void)
{
    D9WGHello hello;

    if (InterlockedCompareExchange((LONG *)&g_hello_emitted, TRUE, FALSE))
        return;
    hello.guest_pointer_bits = 32;
    /* Reports what this build's caps advertise, so the host's stats can say
     * which DLL the guest actually loaded (see D9WG_FEATURE_* in the
     * protocol header). */
    hello.feature_bits = shader_model_enabled() ? D9WG_FEATURE_SHADER_MODEL_2 : 0;
    hello.session_id_low = g_session_id_low;
    hello.session_id_high = g_session_id_high;
    emit_command(D9WG_OP_HELLO, &hello, sizeof(hello));
    /*
     * Identify this DLL in the browser console, once per process.
     *
     * Without it, "no [d3d9-guest] lines appeared" has two readings that call
     * for opposite next steps -- the guest refused nothing, or the guest is an
     * older DLL that cannot report at all -- and nothing else distinguishes
     * them. The host executor ships with the page and updates on reload while
     * this DLL lives inside a disk image and only changes when someone copies
     * it in, so "new host, stale guest" is the ordinary state of things, not an
     * edge case. One line of proof-of-life makes every later silence mean
     * exactly one thing.
     */
    HOSTLOG_INFO("proxy build %s loaded; refusals and failures will be "
            "reported here", D9_PROXY_BUILD);
}

static void initialize_session_id(HINSTANCE instance)
{
    FILETIME time;
    LARGE_INTEGER counter;
    DWORD process_id = GetCurrentProcessId();
    DWORD thread_id = GetCurrentThreadId();

    GetSystemTimeAsFileTime(&time);
    if (!QueryPerformanceCounter(&counter)) {
        counter.LowPart = GetTickCount();
        counter.HighPart = process_id ^ thread_id;
    }
    g_session_id_low = time.dwLowDateTime ^ counter.LowPart ^ process_id
            ^ (uint32_t)(uintptr_t)instance;
    g_session_id_high = time.dwHighDateTime ^ counter.HighPart
            ^ GetTickCount() ^ (thread_id * 0x9E3779B9u);
    if (!g_session_id_low && !g_session_id_high)
        g_session_id_high = 0xD9A80001u;
}

static void fill_display_mode(D3DDISPLAYMODE *mode, UINT width, UINT height,
        D3DFORMAT format)
{
    mode->Width = width;
    mode->Height = height;
    mode->RefreshRate = 60;
    mode->Format = format;
}

/*
 * Caps stay honest about what is actually implemented. M2 adds shader model
 * 2.0, so VertexShaderVersion/PixelShaderVersion now report (2,0) and the
 * VS20Caps/PS20Caps sub-structures describe what the host translator
 * (d3d9_shader_pipeline.js) genuinely handles rather than the model minimum.
 * Notably PS20Caps.DynamicFlowControlDepth stays 0: data-dependent branching
 * in a pixel shader translates, but WGSL forbids implicit-derivative texture
 * sampling inside it, so any sample in such a branch silently loses mip
 * selection -- claiming the capability would invite exactly the shaders that
 * hit that degradation.
 *
 * M4.5 makes the lower-capability path a supported negotiated profile:
 * D9WG_CAPS_PROFILE=ffp reports fixed-function T&L with no programmable
 * shaders, while D9WG_CAPS_PROFILE=sm2 (the default) reports the M5 path.
 * D9WG_SHADER_MODEL=0 remains accepted as a backwards-compatible spelling.
 */
static BOOL shader_model_enabled(void)
{
    static LONG cached = -1;
    char value[16];
    DWORD length;

    if (cached >= 0)
        return cached != 0;
    length = GetEnvironmentVariableA("D9WG_CAPS_PROFILE", value, sizeof(value));
    if (length && length < sizeof(value)) {
        if (!lstrcmpiA(value, "ffp") || !lstrcmpiA(value, "m4.5") ||
                !lstrcmpiA(value, "low")) {
            cached = 0;
            return FALSE;
        }
        if (!lstrcmpiA(value, "sm2") || !lstrcmpiA(value, "m5")) {
            cached = 1;
            return TRUE;
        }
    }
    length = GetEnvironmentVariableA("D9WG_SHADER_MODEL", value, sizeof(value));
    cached = (length == 1 && value[0] == '0') ? 0 : 1;
    return cached != 0;
}

static void fill_caps(D3DCAPS9 *caps)
{
    ZeroMemory(caps, sizeof(*caps));
    caps->DeviceType = D3DDEVTYPE_HAL;
    caps->AdapterOrdinal = D3DADAPTER_DEFAULT;
    caps->Caps2 = D3DCAPS2_CANMANAGERESOURCE;
    caps->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE
            | D3DPRESENT_INTERVAL_ONE;
    caps->DevCaps = D3DDEVCAPS_HWRASTERIZATION
            | D3DDEVCAPS_HWTRANSFORMANDLIGHT
            | D3DDEVCAPS_DRAWPRIMTLVERTEX
            | D3DDEVCAPS_EXECUTESYSTEMMEMORY
            | D3DDEVCAPS_EXECUTEVIDEOMEMORY
            | D3DDEVCAPS_TEXTURESYSTEMMEMORY
            | D3DDEVCAPS_TEXTUREVIDEOMEMORY;
    caps->PrimitiveMiscCaps = D3DPMISCCAPS_CULLNONE
            | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW
            | D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_BLENDOP
            | D3DPMISCCAPS_SEPARATEALPHABLEND
            | D3DPMISCCAPS_INDEPENDENTWRITEMASKS
            /* M3: D3DTA_TEMP as a stage argument and D3DTSS_RESULTARG
             * choosing it, both implemented by the host's cascade. */
            | D3DPMISCCAPS_TSSARGTEMP;
    caps->RasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_DEPTHBIAS
            | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS
            | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE
            | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_FOGRANGE
            /* M3: SetScissorRect reaches the host and its render pass. */
            | D3DPRASTERCAPS_SCISSORTEST;
    caps->ZCmpCaps = 0xFFu;
    /* ZERO through BOTHINVSRCALPHA plus the BLENDFACTOR pair. The host maps
     * BOTH* to its resolved source/destination pair and installs WebGPU's
     * dynamic blend constant for BLENDFACTOR/INVBLENDFACTOR. */
    caps->SrcBlendCaps = 0x3FFFu;
    caps->DestBlendCaps = 0x3FFFu;
    caps->AlphaCmpCaps = 0xFFu;
    caps->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO
            | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT
            | D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT
            | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR;
    caps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB
            | D3DPSHADECAPS_FOGGOURAUD | D3DPSHADECAPS_ALPHAGOURAUDBLEND;
    caps->TextureCaps = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP
            | D3DPTEXTURECAPS_PERSPECTIVE
            /* M3: real IDirect3DCubeTexture9, six faces per level, sampled
             * through a texture_cube<f32> by both the fixed-function cascade
             * and a translated pixel shader's `dcl_cube`. NONPOW2CONDITIONAL
             * and CUBEMAP_POW2 stay absent: WebGPU has no such restriction, so
             * claiming one would only make apps pad textures for nothing.
             * VOLUMEMAP is still absent -- IDirect3DVolumeTexture9 remains
             * unimplemented, see the plan's M3 status record. */
            | D3DPTEXTURECAPS_CUBEMAP;
    caps->TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT
            | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT
            | D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MIPFPOINT
            | D3DPTFILTERCAPS_MIPFLINEAR;
    caps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP
            | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP;
    caps->TextureOpCaps = D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1
            | D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE
            | D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X
            | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED
            | D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT
            | D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA
            | D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA
            | D3DTEXOPCAPS_BLENDTEXTUREALPHAPM
            | D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_DOTPRODUCT3
            | D3DTEXOPCAPS_MULTIPLYADD | D3DTEXOPCAPS_LERP;
    caps->MaxTextureWidth = 4096;
    caps->MaxTextureHeight = 4096;
    caps->MaxTextureRepeat = 8192;
    caps->MaxTextureAspectRatio = 4096;
    caps->MaxAnisotropy = 1;
    caps->MaxVertexW = 1.0e10f;
    caps->MaxPointSize = 256.0f;
    caps->MaxPrimitiveCount = 0xFFFFFu;
    caps->MaxVertexIndex = 0xFFFFFFu;
    caps->MaxStreams = D9_MAX_STREAMS;
    caps->MaxStreamStride = 255;
    /* M5 declaration formats beyond the baseline FLOATn/D3DCOLOR set. */
    caps->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N
            | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N
            | D3DDTCAPS_USHORT2N | D3DDTCAPS_USHORT4N
            | D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N
            | D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;
    if (shader_model_enabled()) {
        caps->VertexShaderVersion = (DWORD)D3DVS_VERSION(2, 0);
        caps->MaxVertexShaderConst = D9_MAX_VS_CONST_F;
        caps->PixelShaderVersion = (DWORD)D3DPS_VERSION(2, 0);
        /* The largest absolute value a ps_1_x register can hold. 8.0 is what
         * SM1.4-class hardware reported; anything smaller makes an engine
         * pre-scale its constants. */
        caps->PixelShader1xMaxValue = 8.0f;
        caps->DevCaps |= D3DDEVCAPS_PUREDEVICE;
        /* r0..r31 and four levels of static nesting are what the translator
         * emits WGSL for; predication is handled by guarding the instruction. */
        caps->VS20Caps.Caps = D3DVS20CAPS_PREDICATION;
        caps->VS20Caps.DynamicFlowControlDepth = D3DVS20_MAX_DYNAMICFLOWCONTROLDEPTH;
        caps->VS20Caps.NumTemps = 32;
        caps->VS20Caps.StaticFlowControlDepth = 4;
        caps->PS20Caps.Caps = D3DPS20CAPS_ARBITRARYSWIZZLE
                | D3DPS20CAPS_GRADIENTINSTRUCTIONS
                | D3DPS20CAPS_PREDICATION
                | D3DPS20CAPS_NODEPENDENTREADLIMIT
                | D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
        caps->PS20Caps.DynamicFlowControlDepth = 0; /* see the note above */
        caps->PS20Caps.NumTemps = 32;
        caps->PS20Caps.StaticFlowControlDepth = 4;
        caps->PS20Caps.NumInstructionSlots = 512;
        caps->MaxVShaderInstructionsExecuted = 65535;
        caps->MaxPShaderInstructionsExecuted = 65535;
        caps->VertexTextureFilterCaps = 0; /* vs_3_0 vertex texture fetch: 9.9 */
    } else {
        caps->VertexShaderVersion = (DWORD)D3DVS_VERSION(0, 0);
        caps->MaxVertexShaderConst = 0;
        caps->PixelShaderVersion = (DWORD)D3DPS_VERSION(0, 0);
    }
    caps->FVFCaps = 8u & D3DFVFCAPS_TEXCOORDCOUNTMASK;
    caps->MaxTextureBlendStages = D9_MAX_TEXTURE_STAGES;
    caps->MaxSimultaneousTextures = D9_MAX_TEXTURE_STAGES;
    caps->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN
            | D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS
            | D3DVTXPCAPS_LOCALVIEWER;
    caps->MaxActiveLights = 8;
    /* Still 0: WGSL has no clip-distance facility, so a user clip plane has to
     * become a fragment discard fed by a vertex-computed distance (plan 9.11),
     * and that changes the varying contract both stages agree on. Reporting a
     * non-zero count before that exists would make an app clip against planes
     * nothing evaluates -- geometry that should be cut would simply appear. */
    caps->MaxUserClipPlanes = 0;
    caps->MaxVertexBlendMatrices = 0;
    /* Render targets and MRT: the host binds up to four colour attachments and
     * a translated pixel shader's oC0..oC3 reach them (M4 work brought forward
     * because a 2005-era D3D9 game renders most of its frame into textures). */
    caps->NumSimultaneousRTs = D9_MAX_RENDER_TARGETS;
}

static BOOL device_has_reset_blockers(D9Device *device)
{
    D9VertexBuffer *vb;
    D9IndexBuffer *ib;
    D9Texture *texture;
    D9CubeTexture *cube;
    UINT level;

    if (device->in_scene)
        return TRUE;
    for (vb = device->vertex_buffers; vb; vb = vb->next_device_resource) {
        if (vb->pool == D3DPOOL_DEFAULT || vb->locked)
            return TRUE;
    }
    for (ib = device->index_buffers; ib; ib = ib->next_device_resource) {
        if (ib->pool == D3DPOOL_DEFAULT || ib->locked)
            return TRUE;
    }
    for (texture = device->texture_resources; texture;
            texture = texture->next_device_resource) {
        if (texture->pool == D3DPOOL_DEFAULT)
            return TRUE;
        for (level = 0; level < texture->level_count; ++level) {
            if (texture->levels[level].locked)
                return TRUE;
        }
    }
    for (cube = device->cube_textures; cube; cube = cube->next_device_resource) {
        if (cube->pool == D3DPOOL_DEFAULT)
            return TRUE;
        for (level = 0; level < cube->level_count * 6u; ++level) {
            if (cube->levels[level].locked)
                return TRUE;
        }
    }
    /* An open state-block recording spans the Reset, and the block it would
     * produce describes resources the Reset is about to invalidate. */
    if (device->recording)
        return TRUE;
    return FALSE;
}

static void device_clear_bindings(D9Device *device)
{
    UINT index;
    for (index = 0; index < D9_MAX_STREAMS; ++index) {
        D9VertexBuffer *buffer = device->streams[index].buffer;
        device->streams[index].buffer = NULL;
        device->streams[index].stride = 0;
        if (buffer) IDirect3DVertexBuffer9_Release(&buffer->iface);
    }
    if (device->index_buffer) {
        D9IndexBuffer *buffer = device->index_buffer;
        device->index_buffer = NULL;
        IDirect3DIndexBuffer9_Release(&buffer->iface);
    }
    for (index = 0; index < D9_MAX_TEXTURE_STAGES; ++index) {
        D9Texture *texture = device->textures[index];
        D9CubeTexture *cube = device->cube_bindings[index];
        device->textures[index] = NULL;
        device->cube_bindings[index] = NULL;
        if (texture) IDirect3DTexture9_Release(&texture->iface);
        if (cube) IDirect3DCubeTexture9_Release(&cube->iface);
    }
    for (index = 0; index < D9_MAX_RENDER_TARGETS; ++index) {
        D9Surface *surface = device->render_target_surfaces[index];
        device->render_target_surfaces[index] = NULL;
        if (surface) IDirect3DSurface9_Release(&surface->iface);
    }
    if (device->depth_stencil_surface) {
        D9Surface *surface = device->depth_stencil_surface;
        device->depth_stencil_surface = NULL;
        IDirect3DSurface9_Release(&surface->iface);
    }
    device->depth_stencil_unbound = FALSE;
    if (device->vertex_declaration) {
        D9VertexDeclaration *decl = device->vertex_declaration;
        device->vertex_declaration = NULL;
        IDirect3DVertexDeclaration9_Release(&decl->iface);
    }
    if (device->vertex_shader) {
        D9Shader *shader = device->vertex_shader;
        device->vertex_shader = NULL;
        IDirect3DVertexShader9_Release(&shader->iface.vertex);
    }
    if (device->pixel_shader) {
        D9Shader *shader = device->pixel_shader;
        device->pixel_shader = NULL;
        IDirect3DPixelShader9_Release(&shader->iface.pixel);
    }
    device->fvf = 0;
}

static void device_release_owned_references(D9Device *device)
{
    TRACE("ENTER Device.ReleaseOwnedReferences device=%08lX refs=%ld child_refs=%ld",
            device->handle,
            InterlockedCompareExchange(&device->refcount, 0, 0),
            InterlockedCompareExchange(&device->child_parent_refs, 0, 0));
    device_clear_bindings(device);
    TRACE("LEAVE Device.ReleaseOwnedReferences device=%08lX refs=%ld child_refs=%ld",
            device->handle,
            InterlockedCompareExchange(&device->refcount, 0, 0),
            InterlockedCompareExchange(&device->child_parent_refs, 0, 0));
}

static BOOL recreate_device_resources(D9Device *device)
{
    D9VertexBuffer *vb;
    D9IndexBuffer *ib;
    D9Texture *texture;
    D9CubeTexture *cube;
    D9VertexDeclaration *decl;
    D9Shader *shader;
    UINT level;

    for (vb = device->vertex_buffers; vb; vb = vb->next_device_resource) {
        vb->handle = allocate_handle();
        if (!emit_vertex_buffer_create(device, vb)
                || !emit_buffer_update(vb->handle, 0, vb->shadow,
                        vb->length, 0))
            return FALSE;
    }
    for (ib = device->index_buffers; ib; ib = ib->next_device_resource) {
        ib->handle = allocate_handle();
        if (!emit_index_buffer_create(device, ib)
                || !emit_buffer_update(ib->handle, 0, ib->shadow,
                        ib->length, 0))
            return FALSE;
    }
    for (texture = device->texture_resources; texture;
            texture = texture->next_device_resource) {
        texture->handle = allocate_handle();
        if (!emit_texture_create(device, texture)) return FALSE;
        for (level = 0; level < texture->level_count; ++level) {
            RECT full;
            SetRect(&full, 0, 0, (int)texture->levels[level].width,
                    (int)texture->levels[level].height);
            if (!emit_texture_update(texture, level, &full)) return FALSE;
        }
    }
    for (cube = device->cube_textures; cube; cube = cube->next_device_resource) {
        UINT face;
        cube->handle = allocate_handle();
        if (!emit_cube_texture_create(device, cube)) return FALSE;
        for (face = 0; face < 6u; ++face) {
            for (level = 0; level < cube->level_count; ++level) {
                D9TextureLevel *level_data =
                        &cube->levels[face * cube->level_count + level];
                RECT full;
                SetRect(&full, 0, 0, (int)level_data->width,
                        (int)level_data->height);
                if (!emit_cube_texture_update(cube, face, level, &full))
                    return FALSE;
            }
        }
    }
    for (decl = device->vertex_declarations; decl;
            decl = decl->next_device_resource) {
        D9WGVertexElement wire[D3DMAXDECLLENGTH];
        UINT i;
        decl->handle = allocate_handle();
        for (i = 0; i < decl->element_count; ++i) {
            wire[i].stream = decl->elements[i].Stream;
            wire[i].offset = decl->elements[i].Offset;
            wire[i].type = decl->elements[i].Type;
            wire[i].method = decl->elements[i].Method;
            wire[i].usage = decl->elements[i].Usage;
            wire[i].usage_index = decl->elements[i].UsageIndex;
        }
        if (!emit_vertex_declaration_create(device, decl->handle, wire,
                decl->element_count))
            return FALSE;
    }
    /*
     * Shaders and their constant registers are not pool-bound resources and
     * so survive Reset from the app's point of view -- but the host's
     * resource table is rebuilt from scratch, so both have to be replayed
     * here or the first draw after a Reset binds a handle the host has never
     * heard of. The constants go out unconditionally (bypassing the
     * dirty-range suppression in set_constant_*, which compares against a
     * shadow the host no longer shares) because after a Reset the host's
     * copy is empty, not stale.
     */
    for (shader = device->shaders; shader; shader = shader->next_device_resource) {
        shader->handle = allocate_shader_handle();
        if (!emit_shader_create(device, shader))
            return FALSE;
    }
    if (!emit_shader_constants(device, D9WG_OP_SET_VERTEX_SHADER_CONSTANT_F,
                    0, D9_MAX_VS_CONST_F, device->vs_const_f,
                    D9_MAX_VS_CONST_F * 16u)
            || !emit_shader_constants(device, D9WG_OP_SET_PIXEL_SHADER_CONSTANT_F,
                    0, D9_MAX_PS_CONST_F, device->ps_const_f,
                    D9_MAX_PS_CONST_F * 16u)
            || !emit_shader_constants(device, D9WG_OP_SET_VERTEX_SHADER_CONSTANT_I,
                    0, D9_MAX_CONST_I, device->vs_const_i, D9_MAX_CONST_I * 16u)
            || !emit_shader_constants(device, D9WG_OP_SET_PIXEL_SHADER_CONSTANT_I,
                    0, D9_MAX_CONST_I, device->ps_const_i, D9_MAX_CONST_I * 16u))
        return FALSE;
    {
        uint32_t packed[D9_MAX_CONST_B];
        UINT index;
        for (index = 0; index < D9_MAX_CONST_B; ++index)
            packed[index] = device->vs_const_b[index] ? 1u : 0u;
        if (!emit_shader_constants(device, D9WG_OP_SET_VERTEX_SHADER_CONSTANT_B,
                0, D9_MAX_CONST_B, packed, D9_MAX_CONST_B * 4u))
            return FALSE;
        for (index = 0; index < D9_MAX_CONST_B; ++index)
            packed[index] = device->ps_const_b[index] ? 1u : 0u;
        if (!emit_shader_constants(device, D9WG_OP_SET_PIXEL_SHADER_CONSTANT_B,
                0, D9_MAX_CONST_B, packed, D9_MAX_CONST_B * 4u))
            return FALSE;
    }
    return TRUE;
}

static void device_child_add_ref(D9Device *device)
{
    InterlockedIncrement(&device->child_parent_refs);
    IDirect3DDevice9_AddRef(&device->iface);
}

static void device_child_release(D9Device *device)
{
    InterlockedDecrement(&device->child_parent_refs);
    IDirect3DDevice9_Release(&device->iface);
}

static BOOL device_light_is_set(D9Device *device, UINT index)
{
    return index < D9_MAX_LIGHTS && device->light_set[index];
}

static void device_init_states(D9Device *device)
{
    UINT slot;
    UINT stage;

    /*
     * These arrays are not merely Get*State backing stores: every Set*State
     * path suppresses a command when the cached value already equals the new
     * one.  Zero-initialising them therefore is only correct for states whose
     * D3D9 default is actually zero.  In particular, a game commonly starts a
     * UI/fog/shadow pass with ZENABLE=FALSE and ZWRITEENABLE=FALSE.  With an
     * all-zero cache those first calls were discarded, while the host (quite
     * correctly) retained D3D9's default enabled depth state.  The result was
     * scene depth hiding UI glyphs/icons and depth-writing overlay passes
     * corrupting fog and projected shadows.
     *
     * Keep this table aligned with the fallback values in
     * d3d9_executor.js.  Initial state is not replayed: both ends begin from
     * these same D3D9 defaults, and subsequent Set calls carry every change.
     */
    ZeroMemory(device->render_states, sizeof(device->render_states));
    ZeroMemory(device->texture_stage_states,
            sizeof(device->texture_stage_states));
    ZeroMemory(device->sampler_states, sizeof(device->sampler_states));

    device->render_states[D3DRS_ZENABLE] = D3DZB_TRUE;
    device->render_states[D3DRS_FILLMODE] = D3DFILL_SOLID;
    device->render_states[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    device->render_states[D3DRS_ZWRITEENABLE] = TRUE;
    device->render_states[D3DRS_LASTPIXEL] = TRUE;
    device->render_states[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    device->render_states[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    device->render_states[D3DRS_CULLMODE] = D3DCULL_CCW;
    device->render_states[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    device->render_states[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    device->render_states[D3DRS_FOGEND] = 0x3F800000u;
    device->render_states[D3DRS_FOGDENSITY] = 0x3F800000u;
    device->render_states[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
    device->render_states[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    device->render_states[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    device->render_states[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    device->render_states[D3DRS_STENCILMASK] = 0xFFFFFFFFu;
    device->render_states[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu;
    device->render_states[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    device->render_states[D3DRS_CLIPPING] = TRUE;
    device->render_states[D3DRS_LIGHTING] = TRUE;
    device->render_states[D3DRS_COLORVERTEX] = TRUE;
    device->render_states[D3DRS_LOCALVIEWER] = TRUE;
    device->render_states[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    device->render_states[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    device->render_states[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
    device->render_states[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
    device->render_states[D3DRS_POINTSIZE] = 0x3F800000u;
    device->render_states[D3DRS_POINTSIZE_MIN] = 0x3F800000u;
    device->render_states[D3DRS_POINTSCALE_A] = 0x3F800000u;
    device->render_states[D3DRS_POINTSIZE_MAX] = 0x42800000u; /* 64.0f */
    device->render_states[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
    device->render_states[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFFu;
    device->render_states[D3DRS_PATCHEDGESTYLE] = D3DPATCHEDGE_DISCRETE;
    device->render_states[D3DRS_COLORWRITEENABLE] = 0xFu;
    device->render_states[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    device->render_states[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
    device->render_states[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    device->render_states[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    device->render_states[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    device->render_states[D3DRS_COLORWRITEENABLE1] = 0xFu;
    device->render_states[D3DRS_COLORWRITEENABLE2] = 0xFu;
    device->render_states[D3DRS_COLORWRITEENABLE3] = 0xFu;
    device->render_states[D3DRS_BLENDFACTOR] = 0xFFFFFFFFu;
    device->render_states[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    device->render_states[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    device->render_states[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;

    for (stage = 0; stage < D9_MAX_TEXTURE_STAGES; ++stage) {
        device->texture_stage_states[stage][D3DTSS_COLOROP] = stage == 0
                ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        device->texture_stage_states[stage][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        device->texture_stage_states[stage][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        device->texture_stage_states[stage][D3DTSS_ALPHAOP] = stage == 0
                ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        device->texture_stage_states[stage][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        device->texture_stage_states[stage][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        device->texture_stage_states[stage][D3DTSS_TEXCOORDINDEX] = stage;
        device->texture_stage_states[stage][D3DTSS_COLORARG0] = D3DTA_CURRENT;
        device->texture_stage_states[stage][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
        device->texture_stage_states[stage][D3DTSS_RESULTARG] = D3DTA_CURRENT;
    }
    for (slot = 0; slot < D9_MAX_SAMPLERS; ++slot) {
        device->sampler_states[slot][D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
        device->sampler_states[slot][D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
        device->sampler_states[slot][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        device->sampler_states[slot][D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
        device->sampler_states[slot][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        device->sampler_states[slot][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        device->sampler_states[slot][D3DSAMP_MAXANISOTROPY] = 1u;
    }
    for (slot = 0; slot < D9_MAX_TRANSFORMS; ++slot) {
        ZeroMemory(device->transforms[slot], sizeof(device->transforms[slot]));
        device->transforms[slot][0] = 1.0f;
        device->transforms[slot][5] = 1.0f;
        device->transforms[slot][10] = 1.0f;
        device->transforms[slot][15] = 1.0f;
    }
}

/* ---- IDirect3D9 ---- */

static HRESULT WINAPI d3d_query_interface(IDirect3D9 *iface, REFIID iid,
        void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid) && !guid_equal(iid, &IID_IDirect3D9))) {
        /* A refused QueryInterface is how a title discovers there is no
         * IDirect3D9Ex here, and some give up on the spot. Name the GUID it
         * asked for rather than leaving a bare E_NOINTERFACE. */
        TRACE("FAIL Direct3D9.QueryInterface iid=%08lX-%04lX-%04lX -> %08lX",
                iid ? iid->Data1 : 0, iid ? (DWORD)iid->Data2 : 0,
                iid ? (DWORD)iid->Data3 : 0, (DWORD)E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    *object = iface;
    IDirect3D9_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI d3d_add_ref(IDirect3D9 *iface)
{
    return (ULONG)InterlockedIncrement(&d3d_from_iface(iface)->refcount);
}

static ULONG WINAPI d3d_release(IDirect3D9 *iface)
{
    D9Direct3D *d3d = d3d_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&d3d->refcount);
    if (!refs) {
        HeapFree(GetProcessHeap(), 0, d3d);
    }
    return refs;
}

static HRESULT WINAPI d3d_register_software_device(IDirect3D9 *iface,
        void *initialize)
{
    (void)iface; (void)initialize;

    TRACE("FAIL RegisterSoftwareDevice -> %08lX", (DWORD)D3DERR_INVALIDCALL);
    return D3DERR_INVALIDCALL;
}

static UINT WINAPI d3d_get_adapter_count(IDirect3D9 *iface)
{
    (void)iface;

    TRACE("OK GetAdapterCount -> 1");
    return 1;
}

static HRESULT WINAPI d3d_get_adapter_identifier(IDirect3D9 *iface,
        UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *identifier)
{
    (void)iface; (void)flags;

    TRACE("CALL GetAdapterIdentifier adapter=%lu flags=%08lX", adapter, flags);
    if (adapter || !identifier) {
        TRACE("FAIL GetAdapterIdentifier -> %08lX", (DWORD)D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }
    ZeroMemory(identifier, sizeof(*identifier));
    /*
     * Fields other than the identity triple are the same in every variant:
     * no real driver reports an all-zero DriverVersion, an all-zero
     * DeviceIdentifier GUID (apps cache it to notice a driver change) or an
     * empty DeviceName, and a caller that sanity-checks the struct would
     * reject those regardless of which card we claim to be.
     */
    lstrcpynA(identifier->DeviceName, "\\\\.\\DISPLAY1",
            sizeof(identifier->DeviceName));
#if D9_ADAPTER_IDENTITY == D9_ADAPTER_GEFORCEFX_5200
    /* NV34. The entry-level card of the first NVIDIA generation with
     * vs_2_0/ps_2_0, matching the shader model fill_caps() reports from M2
     * on. */
    lstrcpynA(identifier->Driver, "nv4_disp.dll", sizeof(identifier->Driver));
    lstrcpynA(identifier->Description, "NVIDIA GeForce FX 5200",
            sizeof(identifier->Description));
    identifier->VendorId = 0x10DE;
    identifier->DeviceId = 0x0322;
    identifier->SubSysId = 0x032210DE;
    identifier->Revision = 0xA1;
    /* ForceWare 66.93, a real XP driver revision for this card. */
    identifier->DriverVersion.HighPart = (6 << 16) | 14;
    identifier->DriverVersion.LowPart = (10 << 16) | 6693;
#elif D9_ADAPTER_IDENTITY == D9_ADAPTER_GEFORCE4_MX
    /* NV17. Hardware T&L, no programmable shaders -- the one widely-known
     * card whose real capabilities match what fill_caps() reports. */
    lstrcpynA(identifier->Driver, "nv4_disp.dll", sizeof(identifier->Driver));
    lstrcpynA(identifier->Description, "NVIDIA GeForce4 MX 440",
            sizeof(identifier->Description));
    identifier->VendorId = 0x10DE;
    identifier->DeviceId = 0x0171;
    identifier->SubSysId = 0x017110DE;
    identifier->Revision = 0xA3;
    /* ForceWare 45.23, a real driver revision for this card on XP. */
    identifier->DriverVersion.HighPart = (6 << 16) | 14;
    identifier->DriverVersion.LowPart = (10 << 16) | 4523;
#elif D9_ADAPTER_IDENTITY == D9_ADAPTER_VMWARE_SVGA
    lstrcpynA(identifier->Driver, "vm3dum.dll", sizeof(identifier->Driver));
    lstrcpynA(identifier->Description, "VMware SVGA 3D",
            sizeof(identifier->Description));
    identifier->VendorId = 0x15AD;
    identifier->DeviceId = 0x0405;
    identifier->SubSysId = 0x040515AD;
    identifier->Revision = 0;
    identifier->DriverVersion.HighPart = (6 << 16) | 14;
    identifier->DriverVersion.LowPart = (1 << 16) | 1264;
#else
    lstrcpynA(identifier->Driver, "d3d9-webgpu.dll",
            sizeof(identifier->Driver));
    lstrcpynA(identifier->Description, "v86 Direct3D 9 WebGPU Adapter",
            sizeof(identifier->Description));
    identifier->VendorId = 0x1234;
    identifier->DeviceId = 0x5687;
    identifier->SubSysId = 0x56871234;
    identifier->Revision = 1;
    /* product.version.subversion.build packed as two DWORDs, the way real
     * drivers report it (here 6.14.10.6764, a plausible XP-era driver). */
    identifier->DriverVersion.HighPart = (6 << 16) | 14;
    identifier->DriverVersion.LowPart = (10 << 16) | 6764;
#endif
    /* Stable and non-zero; derived from the identity so switching variants
     * also looks like a different driver, which is what an app caching this
     * field would expect. */
    identifier->DeviceIdentifier.Data1 = 0xD9E60000u | identifier->DeviceId;
    identifier->DeviceIdentifier.Data2 = (WORD)identifier->VendorId;
    identifier->DeviceIdentifier.Data3 = (WORD)identifier->DeviceId;
    identifier->DeviceIdentifier.Data4[0] = 0x9A;
    identifier->DeviceIdentifier.Data4[1] = 0xB1;
    identifier->DeviceIdentifier.Data4[2] = 0xC2;
    identifier->DeviceIdentifier.Data4[3] = 0xD3;
    identifier->DeviceIdentifier.Data4[4] = 0xE4;
    identifier->DeviceIdentifier.Data4[5] = 0xF5;
    identifier->DeviceIdentifier.Data4[6] = 0x06;
    identifier->DeviceIdentifier.Data4[7] = 0x17;
    /* 1 = WHQL-signed but no date info, rather than 0 = "not certified",
     * which some titles treat as an unusable/blacklisted driver. */
    identifier->WHQLLevel = 1;

    TRACE("OK GetAdapterIdentifier vendor=%04lX device=%04lX desc=%s",
            identifier->VendorId, identifier->DeviceId,
            identifier->Description);
    return D3D_OK;
}

static UINT WINAPI d3d_get_adapter_mode_count(IDirect3D9 *iface, UINT adapter,
        D3DFORMAT format)
{
    (void)iface;

    if (adapter || (format != D3DFMT_X8R8G8B8 && format != D3DFMT_R5G6B5)) {
        /* A zero here empties the app's video-mode list for that format, which
         * is a common reason to abort startup before drawing anything. */
        TRACE("OK GetAdapterModeCount adapter=%lu format=%08lX -> 0", adapter,
                (DWORD)format);
        return 0;
    }
    TRACE("OK GetAdapterModeCount adapter=%lu format=%08lX -> 3", adapter,
            (DWORD)format);
    return 3;
}

static HRESULT WINAPI d3d_enum_adapter_modes(IDirect3D9 *iface, UINT adapter,
        D3DFORMAT format, UINT index, D3DDISPLAYMODE *mode)
{
    static const struct { UINT width; UINT height; } sizes[] = {
        { 640, 480 }, { 800, 600 }, { 1024, 768 }
    };
    (void)iface;

    if (adapter || !mode || (format != D3DFMT_X8R8G8B8
            && format != D3DFMT_R5G6B5)
            || index >= sizeof(sizes) / sizeof(sizes[0])) {
        TRACE("FAIL EnumAdapterModes adapter=%lu format=%08lX index=%lu -> %08lX",
                adapter, (DWORD)format, index, (DWORD)D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }
    fill_display_mode(mode, sizes[index].width, sizes[index].height, format);
    TRACE("OK EnumAdapterModes format=%08lX index=%lu -> %lux%lu refresh=%lu",
            (DWORD)format, index, mode->Width, mode->Height,
            mode->RefreshRate);
    return D3D_OK;
}

static HRESULT WINAPI d3d_get_adapter_display_mode(IDirect3D9 *iface,
        UINT adapter, D3DDISPLAYMODE *mode)
{
    (void)iface;

    if (adapter || !mode)
        return D3DERR_INVALIDCALL;
    fill_display_mode(mode, 1024, 768, D3DFMT_X8R8G8B8);
    TRACE("OK GetAdapterDisplayMode -> %lux%lu format=%08lX refresh=%lu",
            mode->Width, mode->Height, (DWORD)mode->Format, mode->RefreshRate);
    return D3D_OK;
}

/*
 * M1 does not implement depth testing at all (no depth attachment exists on
 * the host side yet), so this list is not a claim about what the renderer
 * honours -- it is only about which AutoDepthStencilFormat values are
 * allowed to get past CreateDevice. Restricting it to D16/D24S8 made
 * CreateDevice fail outright for the very common case of a game asking for
 * D24X8 (depth, no stencil), which is a much worse outcome than the already
 * documented "depth is ignored" boundary: the app cannot render anything at
 * all rather than rendering without depth. Widening it keeps the same
 * honesty level for every format instead of only two.
 */
static BOOL supported_depth_stencil_format(D3DFORMAT format)
{
    switch (format) {
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D15S1:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D32:
        return TRUE;
    default:
        return FALSE;
    }
}

static HRESULT WINAPI d3d_check_device_type(IDirect3D9 *iface, UINT adapter,
        D3DDEVTYPE type, D3DFORMAT display_format,
        D3DFORMAT backbuffer_format, WINBOOL windowed)
{
    BOOL ok;
    (void)iface; (void)windowed;
    ok = !adapter && type == D3DDEVTYPE_HAL
            && (display_format == D3DFMT_X8R8G8B8
                || display_format == D3DFMT_R5G6B5)
            && supported_backbuffer_format(backbuffer_format);

    TRACE("OK CheckDeviceType adapter=%lu type=%lu display=%08lX backbuffer=%08lX "
            "windowed=%lu -> %08lX", adapter, (DWORD)type,
            (DWORD)display_format, (DWORD)backbuffer_format, (DWORD)windowed,
            (DWORD)(ok ? D3D_OK : D3DERR_NOTAVAILABLE));
    return ok ? D3D_OK : D3DERR_NOTAVAILABLE;
}

static HRESULT WINAPI d3d_check_device_format(IDirect3D9 *iface,
        UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format,
        DWORD usage, D3DRESOURCETYPE resource_type, D3DFORMAT format)
{
    BOOL ok = FALSE;
    HRESULT result;
    (void)iface; (void)adapter_format;
    if (!adapter && type == D3DDEVTYPE_HAL) {
        const DWORD unsupported_usage = D3DUSAGE_AUTOGENMIPMAP
                | D3DUSAGE_DMAP | D3DUSAGE_NPATCHES
                | D3DUSAGE_SOFTWAREPROCESSING
                | D3DUSAGE_QUERY_LEGACYBUMPMAP;
        if ((resource_type == D3DRTYPE_TEXTURE
                    || resource_type == D3DRTYPE_CUBETEXTURE)
                && !(usage & D3DUSAGE_DEPTHSTENCIL)
                && !(usage & unsupported_usage)
                && (!(usage & D3DUSAGE_RENDERTARGET)
                    || (resource_type == D3DRTYPE_TEXTURE
                        && supported_render_target_format(format)))
                && supported_texture_format(format))
            ok = TRUE;
        else if (resource_type == D3DRTYPE_SURFACE
                && (usage & D3DUSAGE_DEPTHSTENCIL)
                && supported_depth_stencil_format(format))
            ok = TRUE;
        else if (resource_type == D3DRTYPE_SURFACE
                && (usage & D3DUSAGE_RENDERTARGET)
                && supported_render_target_format(format))
            ok = TRUE;
    }
    result = ok ? D3D_OK : D3DERR_NOTAVAILABLE;
    TRACE("%s CheckDeviceFormat adapter=%lu type=%lu adapter_fmt=%08lX "
            "usage=%08lX resource_type=%lu format=%08lX -> %08lX",
            SUCCEEDED(result) ? "OK" : "FAIL", adapter, (DWORD)type,
            (DWORD)adapter_format, usage, (DWORD)resource_type,
            (DWORD)format, (DWORD)result);
    /*
     * D3DERR_NOTAVAILABLE is a legitimate answer, not a refusal, which is
     * exactly why it needs reporting: an app that asks whether it may use a
     * format and is told no does not fail, it quietly does without -- loads no
     * texture, draws no model, logs nothing. That is indistinguishable from
     * "the app never wanted to draw it", and the difference is the whole
     * question when content is missing with a clean command stream. Deduped by
     * text, so each distinct usage/type/format triple costs one line however
     * many times it is probed.
     */
    if (!ok)
        HOSTLOG_REFUSED("CheckDeviceFormat said no: format=%08lX usage=%08lX "
                "resource_type=%lu (an app told no here silently does "
                "without)", (DWORD)format, usage, (DWORD)resource_type);
    return result;
}

static HRESULT WINAPI d3d_check_multisample(IDirect3D9 *iface, UINT adapter,
        D3DDEVTYPE type, D3DFORMAT format, WINBOOL windowed,
        D3DMULTISAMPLE_TYPE multisample, DWORD *quality_levels)
{
    BOOL ok;
    (void)iface; (void)format; (void)windowed;
    if (quality_levels) *quality_levels = 1;
    ok = !adapter && type == D3DDEVTYPE_HAL
            && multisample == D3DMULTISAMPLE_NONE;

    TRACE("OK CheckDeviceMultiSampleType adapter=%lu type=%lu format=%08lX "
            "multisample=%lu -> %08lX", adapter, (DWORD)type, (DWORD)format,
            (DWORD)multisample, (DWORD)(ok ? D3D_OK : D3DERR_NOTAVAILABLE));
    return ok ? D3D_OK : D3DERR_NOTAVAILABLE;
}

static HRESULT WINAPI d3d_check_depth_stencil(IDirect3D9 *iface,
        UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format,
        D3DFORMAT render_format, D3DFORMAT depth_format)
{
    BOOL ok;
    (void)iface; (void)adapter_format; (void)render_format;
    ok = !adapter && type == D3DDEVTYPE_HAL
            && supported_depth_stencil_format(depth_format);

    TRACE("OK CheckDepthStencilMatch adapter=%lu type=%lu adapter_fmt=%08lX "
            "render_fmt=%08lX depth_fmt=%08lX -> %08lX", adapter, (DWORD)type,
            (DWORD)adapter_format, (DWORD)render_format, (DWORD)depth_format,
            (DWORD)(ok ? D3D_OK : D3DERR_NOTAVAILABLE));
    return ok ? D3D_OK : D3DERR_NOTAVAILABLE;
}

static HRESULT WINAPI d3d_check_device_format_conversion(IDirect3D9 *iface,
        UINT adapter, D3DDEVTYPE type, D3DFORMAT source, D3DFORMAT target)
{
    HRESULT result;
    (void)iface;
    if (adapter || type != D3DDEVTYPE_HAL)
        result = D3DERR_NOTAVAILABLE;
    else
        result = source == target ? D3D_OK : D3DERR_NOTAVAILABLE;
    TRACE("OK CheckDeviceFormatConversion source=%08lX target=%08lX -> %08lX",
            (DWORD)source, (DWORD)target, (DWORD)result);
    return result;
}

/*
 * Caps are the classic silent-abort surface: a title reads them once, decides
 * the adapter cannot run it, and exits without drawing or complaining. Logging
 * the fields games actually gate on turns that into something readable.
 */
static void trace_caps(const char *source, const D3DCAPS9 *caps)
{
    /* TRACE compiles away entirely in the ordinary d3d9.dll. */
    (void)source; (void)caps;
    TRACE("CAPS %s dev=%08lX vs=%08lX ps=%08lX blend_stages=%lu "
            "simultaneous_textures=%lu streams=%lu rts=%lu lights=%lu",
            source, caps->DevCaps, caps->VertexShaderVersion,
            caps->PixelShaderVersion, caps->MaxTextureBlendStages,
            caps->MaxSimultaneousTextures, caps->MaxStreams,
            caps->NumSimultaneousRTs, caps->MaxActiveLights);
    TRACE("CAPS %s texture=%08lX raster=%08lX src_blend=%08lX dst_blend=%08lX "
            "texture_op=%08lX max_texture=%lux%lu anisotropy=%lu "
            "primitives=%lu", source, caps->TextureCaps, caps->RasterCaps,
            caps->SrcBlendCaps, caps->DestBlendCaps, caps->TextureOpCaps,
            caps->MaxTextureWidth, caps->MaxTextureHeight, caps->MaxAnisotropy,
            caps->MaxPrimitiveCount);
    TRACE("CAPS %s filter=%08lX address=%08lX stencil=%08lX zcmp=%08lX "
            "shade=%08lX alpha_cmp=%08lX vtxp=%08lX fvf=%08lX decl=%08lX "
            "max_index=%08lX vs_const=%lu texture_repeat=%lu stride=%lu",
            source, caps->TextureFilterCaps, caps->TextureAddressCaps,
            caps->StencilCaps, caps->ZCmpCaps, caps->ShadeCaps,
            caps->AlphaCmpCaps, caps->VertexProcessingCaps, caps->FVFCaps,
            caps->DeclTypes, caps->MaxVertexIndex, caps->MaxVertexShaderConst,
            caps->MaxTextureRepeat, caps->MaxStreamStride);
}

static HRESULT WINAPI d3d_get_device_caps(IDirect3D9 *iface, UINT adapter,
        D3DDEVTYPE type, D3DCAPS9 *caps)
{
    (void)iface;
    if (adapter || type != D3DDEVTYPE_HAL || !caps) {
        TRACE("FAIL Direct3D9.GetDeviceCaps adapter=%lu type=%lu -> %08lX",
                adapter, (DWORD)type, (DWORD)D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }
    fill_caps(caps);
    trace_caps("Direct3D9.GetDeviceCaps", caps);
    return D3D_OK;
}

static HMONITOR WINAPI d3d_get_adapter_monitor(IDirect3D9 *iface, UINT adapter)
{
    HMONITOR monitor;
    (void)iface;
    if (adapter)
        return NULL;
    monitor = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
    TRACE("OK GetAdapterMonitor -> %08lX", (DWORD)(uintptr_t)monitor);
    return monitor;
}

static HRESULT WINAPI d3d_create_device(IDirect3D9 *iface, UINT adapter,
        D3DDEVTYPE type, HWND focus_window, DWORD behavior,
        D3DPRESENT_PARAMETERS *parameters,
        IDirect3DDevice9 **device_out)
{
    D9Direct3D *d3d = d3d_from_iface(iface);
    D9Device *device;
    D9WGCreateDevice command;
    HWND window;
    RECT client;
    POINT origin;

    TRACE("CALL CreateDevice adapter=%lu type=%lu focus=%08lX behavior=%08lX",
            adapter, (DWORD)type, (DWORD)(uintptr_t)focus_window, behavior);

    if (!device_out) {
        TRACE("FAIL CreateDevice missing output -> %08lX",
                (DWORD)D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }
    *device_out = NULL;
    if (adapter || type != D3DDEVTYPE_HAL || !parameters) {
        TRACE("FAIL CreateDevice invalid arguments -> %08lX",
                (DWORD)D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }
    TRACE("CreateDevice params %lux%lu fmt=%08lX count=%lu multisample=%lu "
            "quality=%lu swap=%lu windowed=%lu auto_depth=%lu "
            "depth_fmt=%08lX flags=%08lX interval=%08lX hwnd=%08lX",
            parameters->BackBufferWidth, parameters->BackBufferHeight,
            (DWORD)parameters->BackBufferFormat, parameters->BackBufferCount,
            (DWORD)parameters->MultiSampleType, parameters->MultiSampleQuality,
            (DWORD)parameters->SwapEffect, (DWORD)parameters->Windowed,
            (DWORD)parameters->EnableAutoDepthStencil,
            (DWORD)parameters->AutoDepthStencilFormat, parameters->Flags,
            parameters->PresentationInterval,
            (DWORD)(uintptr_t)parameters->hDeviceWindow);
    if (parameters->MultiSampleType != D3DMULTISAMPLE_NONE) {
        TRACE("FAIL CreateDevice unsupported multisample=%lu -> %08lX",
                (DWORD)parameters->MultiSampleType,
                (DWORD)D3DERR_NOTAVAILABLE);
        return D3DERR_NOTAVAILABLE;
    }
    if (parameters->EnableAutoDepthStencil
            && !supported_depth_stencil_format(
                    parameters->AutoDepthStencilFormat)) {
        TRACE("FAIL CreateDevice unsupported depth_fmt=%08lX -> %08lX",
                (DWORD)parameters->AutoDepthStencilFormat,
                (DWORD)D3DERR_NOTAVAILABLE);
        return D3DERR_NOTAVAILABLE;
    }
    if (parameters->BackBufferFormat != D3DFMT_UNKNOWN
            && !supported_backbuffer_format(parameters->BackBufferFormat)) {
        TRACE("FAIL CreateDevice unsupported backbuffer_fmt=%08lX -> %08lX",
                (DWORD)parameters->BackBufferFormat,
                (DWORD)D3DERR_NOTAVAILABLE);
        return D3DERR_NOTAVAILABLE;
    }
    if (parameters->BackBufferWidth > 8192
            || parameters->BackBufferHeight > 8192) {
        TRACE("FAIL CreateDevice oversized backbuffer=%lux%lu -> %08lX",
                parameters->BackBufferWidth, parameters->BackBufferHeight,
                (DWORD)D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }
    if (parameters->BackBufferCount > 3) {
        TRACE("FAIL CreateDevice backbuffer_count=%lu -> %08lX",
                parameters->BackBufferCount, (DWORD)D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }

    device = (D9Device *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*device));
    if (!device) {
        TRACE("FAIL CreateDevice allocation -> %08lX", (DWORD)E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    device->iface.lpVtbl = &g_device_vtbl;
    device->implicit_swap_chain.iface.lpVtbl = &g_swap_chain_vtbl;
    device->implicit_swap_chain.device = device;
    device->refcount = 1;
    device->parent = d3d;
    IDirect3D9_AddRef(iface);
    device->handle = allocate_handle();
    device->present = *parameters;
    device->creation.AdapterOrdinal = adapter;
    device->creation.DeviceType = type;
    device->creation.hFocusWindow = focus_window;
    device->creation.BehaviorFlags = behavior;
    fill_display_mode(&device->display_mode,
            parameters->BackBufferWidth ? parameters->BackBufferWidth : 640,
            parameters->BackBufferHeight ? parameters->BackBufferHeight : 480,
            parameters->BackBufferFormat == D3DFMT_UNKNOWN
                    ? D3DFMT_X8R8G8B8 : parameters->BackBufferFormat);
    device->viewport.X = 0;
    device->viewport.Y = 0;
    device->viewport.Width = device->display_mode.Width;
    device->viewport.Height = device->display_mode.Height;
    device->viewport.MinZ = 0.0f;
    device->viewport.MaxZ = 1.0f;
    device_init_states(device);

    window = parameters->hDeviceWindow ? parameters->hDeviceWindow
            : focus_window;
    SetRect(&client, 0, 0, (int)device->display_mode.Width,
            (int)device->display_mode.Height);
    origin.x = 0;
    origin.y = 0;
    if (window) {
        GetClientRect(window, &client);
        ClientToScreen(window, &origin);
    }
    command.device_handle = device->handle;
    command.hwnd = (uint32_t)(uintptr_t)window;
    command.x = origin.x;
    command.y = origin.y;
    command.width = parameters->BackBufferWidth
            ? parameters->BackBufferWidth : (uint32_t)(client.right - client.left);
    command.height = parameters->BackBufferHeight
            ? parameters->BackBufferHeight : (uint32_t)(client.bottom - client.top);
    if (!command.width) command.width = 640;
    if (!command.height) command.height = 480;
    command.backbuffer_format = device->display_mode.Format;
    command.windowed = parameters->Windowed;
    command.behavior_flags = behavior;
    command.enable_auto_depth_stencil = parameters->EnableAutoDepthStencil;
    command.auto_depth_stencil_format = parameters->AutoDepthStencilFormat;
    if (!emit_command(D9WG_OP_CREATE_DEVICE, &command, sizeof(command))) {
        IDirect3D9_Release(iface);
        HeapFree(GetProcessHeap(), 0, device);
        TRACE("FAIL CreateDevice transport -> %08lX",
                (DWORD)D3DERR_DRIVERINTERNALERROR);
        return D3DERR_DRIVERINTERNALERROR;
    }
    claim_fullscreen_foreground(device, window);
    emit_window_state(device, window);
    *device_out = &device->iface;
#ifdef D9WG_DIAGNOSTIC_TRACE
    g_trace_device_window = window;
#endif
    TRACE("OK CreateDevice handle=%08lX surface=%lux%lu hwnd=%08lX",
            device->handle, command.width, command.height,
            (DWORD)(uintptr_t)window);
    return D3D_OK;
}

/* ---- IDirect3DDevice9: COM + lifecycle ---- */

static HRESULT WINAPI device_query_interface(IDirect3DDevice9 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DDevice9)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DDevice9_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI device_add_ref(IDirect3DDevice9 *iface)
{
    return (ULONG)InterlockedIncrement(&device_from_iface(iface)->refcount);
}

static ULONG WINAPI device_release(IDirect3DDevice9 *iface)
{
    D9Device *device = device_from_iface(iface);
    LONG refs_before = InterlockedCompareExchange(&device->refcount, 0, 0);
    LONG children_before = InterlockedCompareExchange(
            &device->child_parent_refs, 0, 0);
    ULONG refs;

    TRACE_MARK_ENTER("Device.Release");
    TRACE("ENTER Device.Release object=%08lX vtbl=%08lX handle=%08lX "
            "refs=%ld child_refs=%ld releasing=%lu",
            (DWORD)(uintptr_t)iface, (DWORD)(uintptr_t)iface->lpVtbl,
            device->handle, refs_before, children_before,
            (DWORD)device->releasing_owned_refs);
    (void)refs_before;
    (void)children_before;
    refs = (ULONG)InterlockedDecrement(&device->refcount);

    if (device->releasing_owned_refs) {
        TRACE("LEAVE Device.Release nested object=%08lX refs=%lu child_refs=%ld",
                (DWORD)(uintptr_t)iface, refs,
                InterlockedCompareExchange(&device->child_parent_refs, 0, 0));
        TRACE_MARK_EXIT("Device.Release", (HRESULT)refs, NULL);
        return refs;
    }
    if ((LONG)refs == InterlockedCompareExchange(
            &device->child_parent_refs, 0, 0)) {
        TRACE("Device.Release cycle-break object=%08lX refs=%lu child_refs=%ld",
                (DWORD)(uintptr_t)iface, refs,
                InterlockedCompareExchange(&device->child_parent_refs, 0, 0));
        device->releasing_owned_refs = TRUE;
        device_release_owned_references(device);
        device->releasing_owned_refs = FALSE;
        refs = (ULONG)InterlockedCompareExchange(&device->refcount, 0, 0);
    }
    if (!refs) {
        D9WGDestroyResource destroy;
        TRACE("Device.Release destroy object=%08lX handle=%08lX pending=%lu",
                (DWORD)(uintptr_t)iface, device->handle, g_command_count);
        restore_display_mode(device);
        destroy.resource_handle = device->handle;
        destroy.resource_kind = 0;
        emit_command(D9WG_OP_DESTROY_RESOURCE, &destroy, sizeof(destroy));
        EnterCriticalSection(&g_transport_lock);
        if (g_command_count)
            submit_batch_locked(FALSE);
        LeaveCriticalSection(&g_transport_lock);
        TRACE("LEAVE Device.Release free object=%08lX refs=0 child_refs=%ld",
                (DWORD)(uintptr_t)iface,
                InterlockedCompareExchange(&device->child_parent_refs, 0, 0));
        TRACE_MARK_EXIT("Device.Release", 0, NULL);
        TRACE_FLUSH();
        IDirect3D9_Release(&device->parent->iface);
        /* The implicit surfaces outlive every public reference to them by
         * design, so the device is what finally frees them. Reaching here means
         * their refcounts are zero: each public reference held one of the
         * device references that had to reach zero to get this far. */
        if (device->implicit_back_buffer)
            HeapFree(GetProcessHeap(), 0, device->implicit_back_buffer);
        if (device->implicit_depth_stencil)
            HeapFree(GetProcessHeap(), 0, device->implicit_depth_stencil);
        HeapFree(GetProcessHeap(), 0, device);
        return 0;
    }
    TRACE("LEAVE Device.Release object=%08lX refs=%lu child_refs=%ld",
            (DWORD)(uintptr_t)iface, refs,
            InterlockedCompareExchange(&device->child_parent_refs, 0, 0));
    TRACE_MARK_EXIT("Device.Release", (HRESULT)refs, NULL);
    return refs;
}

static HRESULT WINAPI device_test_cooperative_level(IDirect3DDevice9 *iface)
{
    /* Deliberately untraced: most titles call this every frame. */
    (void)iface;
    return D3D_OK;
}

static UINT WINAPI device_get_available_texture_mem(IDirect3DDevice9 *iface)
{
    (void)iface;
    /* Titles size their texture budget from this, and some refuse to start
     * below a threshold, so it is worth knowing when it was read. */
    TRACE("OK Device.GetAvailableTextureMem -> %lu", 256u * 1024u * 1024u);
    return 256u * 1024u * 1024u;
}

static HRESULT WINAPI device_evict_managed_resources(IDirect3DDevice9 *iface)
{
    (void)iface;
    return D3D_OK;
}

static HRESULT WINAPI device_get_direct3d(IDirect3DDevice9 *iface,
        IDirect3D9 **d3d_out)
{
    D9Device *device = device_from_iface(iface);
    if (!d3d_out)
        return D3DERR_INVALIDCALL;
    *d3d_out = &device->parent->iface;
    IDirect3D9_AddRef(*d3d_out);
    return D3D_OK;
}

static HRESULT WINAPI device_get_caps(IDirect3DDevice9 *iface, D3DCAPS9 *caps)
{
    (void)iface;
    if (!caps)
        return D3DERR_INVALIDCALL;
    fill_caps(caps);
    trace_caps("Device.GetDeviceCaps", caps);
    return D3D_OK;
}

static HRESULT WINAPI device_get_display_mode(IDirect3DDevice9 *iface,
        UINT swapchain, D3DDISPLAYMODE *mode)
{
    D9Device *device = device_from_iface(iface);
    if (swapchain || !mode)
        return D3DERR_INVALIDCALL;
    *mode = device->display_mode;
    TRACE("OK Device.GetDisplayMode -> %lux%lu format=%08lX refresh=%lu",
            mode->Width, mode->Height, (DWORD)mode->Format, mode->RefreshRate);
    return D3D_OK;
}

static HRESULT WINAPI device_get_creation_parameters(IDirect3DDevice9 *iface,
        D3DDEVICE_CREATION_PARAMETERS *parameters)
{
    D9Device *device = device_from_iface(iface);
    if (!parameters)
        return D3DERR_INVALIDCALL;
    *parameters = device->creation;
    TRACE("OK Device.GetCreationParameters adapter=%lu type=%lu behavior=%08lX",
            parameters->AdapterOrdinal, (DWORD)parameters->DeviceType,
            parameters->BehaviorFlags);
    return D3D_OK;
}

/* ---- D3D9 hardware cursor ----
 *
 * A fullscreen D3D9 game draws its pointer through these three calls, not
 * through GDI, so nothing about it ever reaches the VGA framebuffer the page
 * composites underneath the WebGPU overlay. With them stubbed the pointer is
 * simply invisible -- and the site hides the browser's own cursor, on the
 * assumption the guest draws one.
 *
 * D3D9's hardware cursor follows the OS pointer by itself once ShowCursor is
 * on; SetCursorPosition only warps it. emit_present_and_flush() therefore
 * re-sends the live pointer position every frame rather than relying on the
 * app to call SetCursorPosition.
 */
static HRESULT WINAPI device_set_cursor_properties(IDirect3DDevice9 *iface,
        UINT hotspot_x, UINT hotspot_y, IDirect3DSurface9 *bitmap)
{
    D9Device *device = device_from_iface(iface);
    D9Surface *surface;
    D9WGSetCursorProperties command;
    const BYTE *pixels;
    UINT row_pitch;
    uint8_t *payload;
    uint8_t *blob;
    UINT byte_count;
    BOOL result;

    if (!bitmap || bitmap->lpVtbl != &g_surface_vtbl) {
        return D3DERR_INVALIDCALL;
    }
    surface = surface_from_iface(bitmap);
    if (surface->shadow) {
        pixels = surface->shadow;
        row_pitch = surface->row_pitch;
    } else if (surface->texture
            && surface->level < surface->texture->level_count
            && surface->texture->levels[surface->level].shadow) {
        /* A cursor built as a texture level rather than a plain surface. */
        pixels = surface->texture->levels[surface->level].shadow;
        row_pitch = surface->texture->levels[surface->level].row_pitch;
    } else {

        return D3DERR_INVALIDCALL;
    }
    if (surface->format != D3DFMT_A8R8G8B8 && surface->format != D3DFMT_X8R8G8B8) {

        return D3DERR_INVALIDCALL;
    }
    if (!multiply_u32(surface->width * 4u, surface->height, &byte_count))
        return D3DERR_INVALIDCALL;

    ZeroMemory(&command, sizeof(command));
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D9WG_OP_SET_CURSOR_PROPERTIES,
            sizeof(command), byte_count, NULL, &payload, &blob);
    if (result) {
        UINT row;
        command.device_handle = device->handle;
        command.hotspot_x = hotspot_x;
        command.hotspot_y = hotspot_y;
        command.width = surface->width;
        command.height = surface->height;
        command.data_bytes = byte_count;
        command.data_offset = (uint32_t)(blob - batch_base());
        CopyMemory(payload, &command, sizeof(command));
        /* Repack to a tight width*4 stride; the source pitch is whatever the
         * surface was allocated with. */
        for (row = 0; row < surface->height; ++row)
            CopyMemory(blob + row * surface->width * 4u,
                    pixels + row * row_pitch, surface->width * 4u);
    }
    LeaveCriticalSection(&g_transport_lock);
    if (!result)
        return D3DERR_DRIVERINTERNALERROR;
    device->cursor_ready = TRUE;
    device->app_cursor = TRUE;

    return D3D_OK;
}

/*
 * Reports the guest window manager's view of the device window, once and then
 * only when it changes.
 *
 * The host draws its overlay on top unconditionally, so a game whose window is
 * minimised, hidden, or merely not in the foreground still looks perfectly
 * rendered -- while the guest routes every click to whatever window really
 * owns those pixels. That failure is invisible in the picture by construction,
 * so it has to be reported rather than deduced.
 */
static void emit_window_state(D9Device *device, HWND window)
{
    D9WGWindowState command;
    HWND foreground = GetForegroundWindow();
    RECT window_rect;
    RECT client;
    uint32_t flags = 0;

    SetRect(&window_rect, 0, 0, 0, 0);
    SetRect(&client, 0, 0, 0, 0);
    if (window && IsWindow(window)) {
        flags |= D9WG_WINDOW_IS_WINDOW;
        if (IsWindowVisible(window)) flags |= D9WG_WINDOW_VISIBLE;
        if (IsIconic(window)) flags |= D9WG_WINDOW_ICONIC;
        if (window == foreground) flags |= D9WG_WINDOW_FOREGROUND;
        GetWindowRect(window, &window_rect);
        GetClientRect(window, &client);
    }
    if (!device->present.Windowed)
        flags |= D9WG_WINDOW_FULLSCREEN;

    command.device_handle = device->handle;
    command.hwnd = (uint32_t)(uintptr_t)window;
    command.foreground_hwnd = (uint32_t)(uintptr_t)foreground;
    command.flags = flags;
    command.window_x = window_rect.left;
    command.window_y = window_rect.top;
    command.window_width = (uint32_t)(window_rect.right - window_rect.left);
    command.window_height = (uint32_t)(window_rect.bottom - window_rect.top);
    command.client_width = (uint32_t)(client.right - client.left);
    command.client_height = (uint32_t)(client.bottom - client.top);

    if (device->window_state_sent
            && device->last_window_flags == flags
            && device->last_foreground == command.foreground_hwnd)
        return;
    device->window_state_sent = TRUE;
    device->last_window_flags = flags;
    device->last_foreground = command.foreground_hwnd;
    emit_command(D9WG_OP_WINDOW_STATE, &command, sizeof(command));
}

/*
 * A real fullscreen D3D9 device takes the foreground and the top of the
 * z-order when it is created -- that is part of what "exclusive fullscreen"
 * means, and it is done by the driver/runtime, not by the game. Nothing here
 * is a real driver, so without this the game's window can sit behind whatever
 * the user last had focused while its frames are still composited on top by
 * the host: the picture looks right and every click lands in the window
 * underneath.
 */
/*
 * SetForegroundWindow alone is unreliable: Windows denies it to a process that
 * is not already in the foreground, and returns FALSE without doing anything.
 * Briefly attaching to the current foreground thread's input queue is the
 * documented way around that restriction, and is what a game or a launcher does
 * for exactly this reason. Detaching again immediately matters -- staying
 * attached couples the two message queues and can deadlock both.
 */
static void raise_window_to_foreground(HWND window)
{
    HWND foreground = GetForegroundWindow();
    DWORD foreground_thread = foreground
            ? GetWindowThreadProcessId(foreground, NULL) : 0;
    DWORD our_thread = GetCurrentThreadId();
    BOOL attached = FALSE;

    if (foreground_thread && foreground_thread != our_thread)
        attached = AttachThreadInput(our_thread, foreground_thread, TRUE);
    BringWindowToTop(window);
    SetForegroundWindow(window);
    SetActiveWindow(window);
    SetFocus(window);
    if (attached)
        AttachThreadInput(our_thread, foreground_thread, FALSE);
}

static void claim_fullscreen_foreground(D9Device *device, HWND window)
{
    DEVMODEA mode;
    LONG result;

    if (device->present.Windowed)
        return;

    /*
     * Switch the guest's display mode to the one the device asked for.
     *
     * This is the other half of what "exclusive fullscreen" means, and it is
     * not cosmetic. Warcraft III creates an 800x600 fullscreen device on a
     * 1024x768 desktop; with no mode change Windows keeps reporting a
     * 1024x768 screen, so the guest's pointer coordinates, the window
     * geometry and the game's own 800x600 layout all disagree -- the picture
     * looks right while a click lands somewhere else entirely. Doing the mode
     * change makes the guest's idea of the screen match what the game
     * renders, which is what every other part of this path already assumes.
     */
    ZeroMemory(&mode, sizeof(mode));
    mode.dmSize = sizeof(mode);
    mode.dmPelsWidth = device->display_mode.Width;
    mode.dmPelsHeight = device->display_mode.Height;
    mode.dmBitsPerPel = 32;
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    result = ChangeDisplaySettingsA(&mode, CDS_FULLSCREEN);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        /* 16bpp is the other mode a v86 guest is likely to actually have. */
        mode.dmBitsPerPel = 16;
        result = ChangeDisplaySettingsA(&mode, CDS_FULLSCREEN);
    }
    device->display_mode_changed = (result == DISP_CHANGE_SUCCESSFUL);

    if (!window || !IsWindow(window))
        return;
    if (IsIconic(window))
        ShowWindow(window, SW_RESTORE);
    /* Size the device window to the mode as well: testing showed War3's
     * window reporting an empty rect to both GetClientRect and GetWindowRect,
     * so nothing downstream can discover where the game's output belongs. */
    SetWindowPos(window, HWND_TOP, 0, 0, (int)device->display_mode.Width,
            (int)device->display_mode.Height, SWP_SHOWWINDOW);
    raise_window_to_foreground(window);
    device->last_foreground_claim = GetTickCount();

}

/*
 * Re-takes the foreground for a fullscreen device that has lost it.
 *
 * The one-shot claim at CreateDevice is not enough. Need for Speed: Most
 * Wanted reached its main menu with foreground: false in every window-state
 * report -- the picture was perfect and every click went to whatever window was
 * on top instead. Two things cause that and neither is visible in the frame:
 * a game that creates or activates another window after the device (a splash,
 * a launcher, a message-only window), and Windows refusing SetForegroundWindow
 * outright, which it does for a process that is not already in the foreground.
 *
 * A real exclusive-fullscreen D3D9 device holds the foreground for its whole
 * lifetime, re-acquiring it on activation, so maintaining it here is matching
 * the runtime rather than fighting the user: nothing else in a v86 guest whose
 * entire purpose is this one game wants the focus. It stays limited to
 * fullscreen devices -- a windowed game stealing focus every frame would be
 * indefensible -- and to one attempt per interval, because SetForegroundWindow
 * is a syscall and losing focus is not something a frame boundary can fix
 * faster than a person can notice.
 */
static void maintain_fullscreen_foreground(D9Device *device, HWND window)
{
    DWORD now;

    if (device->present.Windowed || !window || !IsWindow(window))
        return;
    if (GetForegroundWindow() == window)
        return;
    now = GetTickCount();
    /* GetTickCount wraps every ~49 days; the subtraction is unsigned so the
     * wrap is harmless, but a zero initial value must not look like "claimed
     * just now" either -- hence the explicit first-time case. */
    if (device->foreground_claims &&
            (DWORD)(now - device->last_foreground_claim) < 500u)
        return;
    device->last_foreground_claim = now;
    ++device->foreground_claims;
    if (IsIconic(window))
        ShowWindow(window, SW_RESTORE);
    raise_window_to_foreground(window);

}

/* Undo the mode change when the device goes away, so a crashed or closed game
 * does not leave the guest desktop stuck at the game's resolution. */
static void restore_display_mode(D9Device *device)
{
    if (!device->display_mode_changed)
        return;
    device->display_mode_changed = FALSE;
    ChangeDisplaySettingsA(NULL, 0);
}

static void emit_cursor_position(D9Device *device, int x, int y, DWORD flags)
{
    D9WGSetCursorPosition command;
    command.device_handle = device->handle;
    command.x = x;
    command.y = y;
    command.flags = flags;
    emit_command(D9WG_OP_SET_CURSOR_POSITION, &command, sizeof(command));
}

/*
 * Capture whatever cursor Windows is currently showing and ship it as if the
 * application had called SetCursorProperties.
 *
 * This exists because most games never call SetCursorProperties at all: on
 * real hardware Windows composites the GDI cursor onto the primary surface
 * even for a fullscreen D3D9 device, so the game simply lets it. Here the
 * "primary surface" is a WebGPU canvas Windows knows nothing about, so that
 * compositing never happens and the pointer is invisible no matter what the
 * game does -- Warcraft III's first run showed cursorUploads: 0 for exactly
 * this reason. Reading the cursor out of GDI covers both mechanisms with one
 * path.
 */
/*
 * Enabled by default.  The browser deliberately hides its CSS cursor above
 * the guest canvas, and the VGA framebuffer cannot contain a Windows cursor,
 * so leaving this opt-in makes any title which delegates its pointer to GDI
 * unplayable.  An application-provided D3D9 hardware cursor still wins through
 * device->app_cursor.  D9WG_GDI_CURSOR=0 remains available for a title which
 * draws its own pointer geometry and does not want the Windows pointer layered
 * over it.
 */
static BOOL gdi_cursor_fallback_enabled(void)
{
    static LONG cached = -1;
    char value[8];
    DWORD length;

    if (cached >= 0)
        return cached != 0;
    length = GetEnvironmentVariableA("D9WG_GDI_CURSOR", value, sizeof(value));
    cached = (length == 1 && value[0] == '0') ? 0 : 1;
    return cached != 0;
}

static BOOL emit_system_cursor_bitmap(D9Device *device, HCURSOR cursor)
{
    ICONINFO icon;
    BITMAP bitmap;
    HDC dc;
    struct { BITMAPINFOHEADER header; DWORD masks[3]; } info;
    BYTE *colors = NULL;
    BYTE *mask = NULL;
    D9WGSetCursorProperties command;
    uint8_t *payload;
    uint8_t *blob;
    UINT width;
    UINT height;
    UINT pixel_bytes;
    UINT index;
    BOOL monochrome;
    BOOL has_alpha = FALSE;
    BOOL result = FALSE;

    ZeroMemory(&icon, sizeof(icon));
    if (!GetIconInfo(cursor, &icon))
        return FALSE;
    monochrome = icon.hbmColor == NULL;
    if (!GetObjectA(monochrome ? icon.hbmMask : icon.hbmColor,
            sizeof(bitmap), &bitmap))
        goto done;
    width = (UINT)bitmap.bmWidth;
    /* A monochrome cursor packs an AND mask above an XOR mask in a single
     * bitmap of twice the real height. */
    height = (UINT)(monochrome ? bitmap.bmHeight / 2 : bitmap.bmHeight);
    if (!width || !height || width > 256u || height > 256u)
        goto done;
    if (!multiply_u32(width * 4u, height, &pixel_bytes))
        goto done;

    dc = CreateCompatibleDC(NULL);
    if (!dc)
        goto done;
    ZeroMemory(&info, sizeof(info));
    info.header.biSize = sizeof(info.header);
    info.header.biWidth = (LONG)width;
    /* Negative height asks for a top-down image, which is the row order the
     * wire format and every other upload on this path already use. */
    info.header.biHeight = -(LONG)(monochrome ? height * 2u : height);
    info.header.biPlanes = 1;
    info.header.biBitCount = 32;
    info.header.biCompression = BI_RGB;

    mask = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            monochrome ? pixel_bytes * 2u : pixel_bytes);
    if (!mask) {
        DeleteDC(dc);
        goto done;
    }
    if (!GetDIBits(dc, icon.hbmMask, 0, monochrome ? height * 2u : height,
            mask, (BITMAPINFO *)&info, DIB_RGB_COLORS)) {
        DeleteDC(dc);
        goto done;
    }
    if (!monochrome) {
        colors = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                pixel_bytes);
        if (!colors) {
            DeleteDC(dc);
            goto done;
        }
        info.header.biHeight = -(LONG)height;
        if (!GetDIBits(dc, icon.hbmColor, 0, height, colors,
                (BITMAPINFO *)&info, DIB_RGB_COLORS)) {
            DeleteDC(dc);
            goto done;
        }
    }
    DeleteDC(dc);

    if (monochrome) {
        /* AND=1,XOR=0 -> transparent; AND=1,XOR=1 -> inverted, approximated
         * as white; AND=0 -> the opaque XOR colour (black or white). */
        colors = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                pixel_bytes);
        if (!colors)
            goto done;
        for (index = 0; index < width * height; ++index) {
            BYTE and_bit = mask[index * 4] ? 1u : 0u;
            BYTE xor_bit = mask[(width * height + index) * 4] ? 1u : 0u;
            BYTE value = xor_bit ? 255u : 0u;
            colors[index * 4 + 0] = value;
            colors[index * 4 + 1] = value;
            colors[index * 4 + 2] = value;
            colors[index * 4 + 3] = and_bit ? 0u : 255u;
        }
    } else {
        for (index = 0; index < width * height; ++index) {
            if (colors[index * 4 + 3]) {
                has_alpha = TRUE;
                break;
            }
        }
        if (!has_alpha) {
            /* A pre-XP style colour cursor carries no alpha channel; the 1bpp
             * mask is what says which texels see through (white == cut out). */
            for (index = 0; index < width * height; ++index)
                colors[index * 4 + 3] = mask[index * 4] ? 0u : 255u;
        }
    }

    ZeroMemory(&command, sizeof(command));
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D9WG_OP_SET_CURSOR_PROPERTIES,
            sizeof(command), pixel_bytes, NULL, &payload, &blob);
    if (result) {
        command.device_handle = device->handle;
        command.hotspot_x = icon.xHotspot;
        command.hotspot_y = icon.yHotspot;
        command.width = width;
        command.height = height;
        command.data_bytes = pixel_bytes;
        command.data_offset = (uint32_t)(blob - batch_base());
        CopyMemory(payload, &command, sizeof(command));
        CopyMemory(blob, colors, pixel_bytes);
    }
    LeaveCriticalSection(&g_transport_lock);

done:
    if (colors) HeapFree(GetProcessHeap(), 0, colors);
    if (mask) HeapFree(GetProcessHeap(), 0, mask);
    if (icon.hbmColor) DeleteObject(icon.hbmColor);
    if (icon.hbmMask) DeleteObject(icon.hbmMask);
    return result;
}

/* Called once per Present. Only re-captures the bitmap when the HCURSOR
 * actually changes -- a game swaps cursors on hover, not every frame. */
static void update_system_cursor(D9Device *device, HWND window)
{
    CURSORINFO cursor_info;
    POINT pointer;

    if (!gdi_cursor_fallback_enabled())
        return;
    ZeroMemory(&cursor_info, sizeof(cursor_info));
    cursor_info.cbSize = sizeof(cursor_info);
    if (!GetCursorInfo(&cursor_info))
        return;

    if (!(cursor_info.flags & CURSOR_SHOWING) || !cursor_info.hCursor) {
        if (device->cursor_visible) {
            D9WGShowCursor hide;
            device->cursor_visible = FALSE;
            hide.device_handle = device->handle;
            hide.show = 0;
            emit_command(D9WG_OP_SHOW_CURSOR, &hide, sizeof(hide));
        }
        return;
    }
    if (cursor_info.hCursor != device->system_cursor) {
        if (!emit_system_cursor_bitmap(device, cursor_info.hCursor))
            return;
        device->system_cursor = cursor_info.hCursor;
        device->cursor_ready = TRUE;
    }
    if (!device->cursor_visible) {
        D9WGShowCursor show;
        device->cursor_visible = TRUE;
        show.device_handle = device->handle;
        show.show = 1;
        emit_command(D9WG_OP_SHOW_CURSOR, &show, sizeof(show));
    }
    pointer = cursor_info.ptScreenPos;
    if (window)
        ScreenToClient(window, &pointer);
    emit_cursor_position(device, pointer.x, pointer.y, 0);
}

static void WINAPI device_set_cursor_position(IDirect3DDevice9 *iface,
        int x, int y, DWORD flags)
{
    D9Device *device = device_from_iface(iface);
    POINT point;

    /* D3D9 takes screen coordinates here; the host draws into back-buffer
     * space, so convert through the device window the same way
     * emit_present_and_flush() does. */
    point.x = x;
    point.y = y;
    if (device->present.hDeviceWindow)
        ScreenToClient(device->present.hDeviceWindow, &point);
    emit_cursor_position(device, point.x, point.y, flags);
}

static WINBOOL WINAPI device_show_cursor(IDirect3DDevice9 *iface,
        WINBOOL show)
{
    D9Device *device = device_from_iface(iface);
    D9WGShowCursor command;
    WINBOOL previous = device->cursor_visible;

    /* Worth tracing on its own: ShowCursor(FALSE) from a game that never
     * called SetCursorProperties means it intends to draw the pointer itself,
     * as ordinary geometry. A missing cursor would then be a lost draw, not a
     * missing cursor feature -- a completely different bug. */

    device->cursor_visible = show ? TRUE : FALSE;
    command.device_handle = device->handle;
    command.show = device->cursor_visible ? 1u : 0u;
    emit_command(D9WG_OP_SHOW_CURSOR, &command, sizeof(command));
    return previous;
}

static UINT WINAPI device_get_number_of_swap_chains(IDirect3DDevice9 *iface)
{
    TRACE_MARK_ENTER("Device.GetNumberOfSwapChains");
    TRACE("CALL GetNumberOfSwapChains device=%08lX -> 1",
            device_from_iface(iface)->handle);
    TRACE_MARK_EXIT("Device.GetNumberOfSwapChains", 1,
            &device_from_iface(iface)->implicit_swap_chain.iface);
    (void)iface;
    return 1;
}

static HRESULT WINAPI device_reset(IDirect3DDevice9 *iface,
        D3DPRESENT_PARAMETERS *parameters)
{
    D9Device *device = device_from_iface(iface);
    D9WGResetDevice reset;
    RECT client;
    POINT origin;
    HWND window;

    if (!parameters || parameters->MultiSampleType != D3DMULTISAMPLE_NONE
            || parameters->BackBufferCount > 1
            || device_has_reset_blockers(device))
        return D3DERR_INVALIDCALL;
    if (parameters->EnableAutoDepthStencil
            && !supported_depth_stencil_format(
                    parameters->AutoDepthStencilFormat))
        return D3DERR_NOTAVAILABLE;
    if (parameters->BackBufferFormat != D3DFMT_UNKNOWN
            && !supported_backbuffer_format(parameters->BackBufferFormat))
        return D3DERR_NOTAVAILABLE;
    window = parameters->hDeviceWindow ? parameters->hDeviceWindow
            : device->creation.hFocusWindow;
    SetRect(&client, 0, 0, 640, 480);
    origin.x = origin.y = 0;
    if (window) {
        GetClientRect(window, &client);
        ClientToScreen(window, &origin);
    }
    ZeroMemory(&reset, sizeof(reset));
    reset.old_device_handle = device->handle;
    reset.new_device_handle = allocate_handle();
    reset.hwnd = (uint32_t)(uintptr_t)window;
    reset.x = origin.x;
    reset.y = origin.y;
    reset.width = parameters->BackBufferWidth
            ? parameters->BackBufferWidth : (uint32_t)(client.right - client.left);
    reset.height = parameters->BackBufferHeight
            ? parameters->BackBufferHeight : (uint32_t)(client.bottom - client.top);
    if (!reset.width) reset.width = device->display_mode.Width;
    if (!reset.height) reset.height = device->display_mode.Height;
    reset.backbuffer_format = parameters->BackBufferFormat == D3DFMT_UNKNOWN
            ? device->display_mode.Format : parameters->BackBufferFormat;
    reset.windowed = parameters->Windowed;
    reset.behavior_flags = device->creation.BehaviorFlags;
    reset.enable_auto_depth_stencil = parameters->EnableAutoDepthStencil;
    reset.auto_depth_stencil_format = parameters->AutoDepthStencilFormat;
    if (reset.width > 8192 || reset.height > 8192)
        return D3DERR_INVALIDCALL;
    if (!emit_command(D9WG_OP_RESET, &reset, sizeof(reset)))
        return D3DERR_DRIVERINTERNALERROR;
    device_clear_bindings(device);
    device->handle = reset.new_device_handle;
    ++device->reset_epoch;
    device->present = *parameters;
    fill_display_mode(&device->display_mode, reset.width, reset.height,
            reset.backbuffer_format);
    /* A Reset resizes the implicit surfaces rather than replacing them -- the
     * app keeps the same pointers across it -- so the cached objects have to
     * pick up the new geometry here or GetDesc keeps reporting the old size. */
    if (device->implicit_back_buffer) {
        device->implicit_back_buffer->width = device->display_mode.Width;
        device->implicit_back_buffer->height = device->display_mode.Height;
        device->implicit_back_buffer->format = device->display_mode.Format;
    }
    if (device->implicit_depth_stencil) {
        device->implicit_depth_stencil->width = reset.width;
        device->implicit_depth_stencil->height = reset.height;
        device->implicit_depth_stencil->format =
                parameters->AutoDepthStencilFormat;
    }
    device_init_states(device);
    device->viewport.X = device->viewport.Y = 0;
    device->viewport.Width = reset.width;
    device->viewport.Height = reset.height;
    device->viewport.MinZ = 0.0f;
    device->viewport.MaxZ = 1.0f;
    if (!recreate_device_resources(device))
        return D3DERR_DRIVERINTERNALERROR;

    return D3D_OK;
}

static BOOL emit_present_and_flush(D9Device *device, HWND override_window)
{
    D9WGPresent present;
    HWND window = override_window ? override_window
            : device->present.hDeviceWindow;
    RECT client;
    POINT origin;
    uint8_t *payload;
    BOOL result;

    if (!window)
        window = device->creation.hFocusWindow;
    SetRect(&client, 0, 0, (int)device->display_mode.Width,
            (int)device->display_mode.Height);
    origin.x = 0;
    origin.y = 0;
    if (window) {
        GetClientRect(window, &client);
        ClientToScreen(window, &origin);
    }
    present.device_handle = device->handle;
    present.hwnd = (uint32_t)(uintptr_t)window;
    present.x = origin.x;
    present.y = origin.y;
    /*
     * An empty client rect is not a curiosity to work around -- it means the
     * host has no idea where the game's output actually is, and it positions
     * the WebGPU overlay from exactly this. Get it wrong and the picture is
     * drawn somewhere the guest does not think the window is, so a click at
     * the pixel the user aimed at is delivered to whatever the guest really
     * has there: another window, the desktop, anything.
     *
     * GetWindowRect is the fallback because it works on windows GetClientRect
     * reports nothing useful for, and it answers both questions at once
     * (position and size, in screen coordinates).
     */
    if (client.right - client.left <= 0 || client.bottom - client.top <= 0) {
        RECT window_rect;
        if (window && GetWindowRect(window, &window_rect)
                && window_rect.right > window_rect.left
                && window_rect.bottom > window_rect.top) {
            origin.x = window_rect.left;
            origin.y = window_rect.top;
            SetRect(&client, 0, 0, window_rect.right - window_rect.left,
                    window_rect.bottom - window_rect.top);
        }

    }
    present.width = (uint32_t)(client.right - client.left);
    present.height = (uint32_t)(client.bottom - client.top);

    /* D3D9's hardware cursor follows the OS pointer on its own, so the app is
     * under no obligation to call SetCursorPosition -- most never do. Sample
     * it here instead, once per frame, and only while a cursor is actually
     * armed and visible. */
    maintain_fullscreen_foreground(device, window);
    emit_window_state(device, window);
    if (device->app_cursor) {
        if (device->cursor_visible) {
            POINT pointer;
            if (GetCursorPos(&pointer)) {
                if (window)
                    ScreenToClient(window, &pointer);
                emit_cursor_position(device, pointer.x, pointer.y, 0);
            }
        }
    } else {
        /* The application is leaving the pointer to Windows; capture it out
         * of GDI instead (see emit_system_cursor_bitmap). */
        update_system_cursor(device, window);
    }

    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D9WG_OP_PRESENT, sizeof(present), 0,
            NULL, &payload, NULL);
    if (result) {
        CopyMemory(payload, &present, sizeof(present));
        result = submit_batch_locked(TRUE);
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

static HRESULT WINAPI device_present(IDirect3DDevice9 *iface,
        const RECT *src_rect, const RECT *dst_rect, HWND override_window,
        const RGNDATA *dirty_region)
{
    BOOL ok;
    (void)src_rect; (void)dst_rect; (void)dirty_region;

    TRACE("CALL Present device=%08lX override=%08lX",
            device_from_iface(iface)->handle,
            (DWORD)(uintptr_t)override_window);
    ok = emit_present_and_flush(device_from_iface(iface), override_window);
    TRACE("%s Present device=%08lX -> %08lX", ok ? "OK" : "FAIL",
            device_from_iface(iface)->handle,
            (DWORD)(ok ? D3D_OK : D3DERR_DRIVERINTERNALERROR));
    return ok ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_set_dialog_box_mode(IDirect3DDevice9 *iface,
        WINBOOL enable)
{ (void)iface; (void)enable; return D3D_OK; }

static void WINAPI device_set_gamma_ramp(IDirect3DDevice9 *iface,
        UINT swapchain, DWORD flags, const D3DGAMMARAMP *ramp)
{ (void)iface; (void)swapchain; (void)flags; (void)ramp; }

static void WINAPI device_get_gamma_ramp(IDirect3DDevice9 *iface,
        UINT swapchain, D3DGAMMARAMP *ramp)
{ (void)iface; (void)swapchain; if (ramp) ZeroMemory(ramp, sizeof(*ramp)); }

static HRESULT WINAPI device_begin_scene(IDirect3DDevice9 *iface)
{
    D9Device *device = device_from_iface(iface);
    D9WGDeviceOnly command;

    if (device->in_scene)
        return D3DERR_INVALIDCALL;
    device->in_scene = TRUE;
    command.device_handle = device->handle;
    command.reserved = 0;
    return emit_command(D9WG_OP_BEGIN_SCENE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_end_scene(IDirect3DDevice9 *iface)
{
    D9Device *device = device_from_iface(iface);
    D9WGDeviceOnly command;

    if (!device->in_scene)
        return D3DERR_INVALIDCALL;
    device->in_scene = FALSE;
    command.device_handle = device->handle;
    command.reserved = 0;
    return emit_command(D9WG_OP_END_SCENE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_clear(IDirect3DDevice9 *iface, DWORD rect_count,
        const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z,
        DWORD stencil)
{
    D9Device *device = device_from_iface(iface);
    D9WGClear command;
    uint8_t *payload;
    uint8_t *rect_data;
    uint32_t rect_bytes;
    BOOL result;

    if (rect_count && !rects)
        return D3DERR_INVALIDCALL;
    if (!(flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)))
        return D3DERR_INVALIDCALL;
    if (rect_count > 0xFFFFFFFFu / sizeof(*rects))
        return D3DERR_INVALIDCALL;
    rect_bytes = rect_count * sizeof(*rects);
    command.device_handle = device->handle;
    command.clear_flags = flags;
    command.color = color;
    command.depth = z;
    command.stencil = stencil;
    command.rect_count = rect_count;

    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D9WG_OP_CLEAR, sizeof(command), rect_bytes,
            NULL, &payload, &rect_data);
    if (result) {
        CopyMemory(payload, &command, sizeof(command));
        if (rect_bytes)
            CopyMemory(rect_data, rects, rect_bytes);
    }
    LeaveCriticalSection(&g_transport_lock);
    if (result && (flags & D3DCLEAR_TARGET)) {
        UINT target;
        for (target = 0; target < D9_MAX_RENDER_TARGETS; ++target) {
            D9Surface *surface = device->render_target_surfaces[target];
            if (!surface)
                continue; /* slot 0 NULL is the implicit back buffer */
            if (!rect_count) {
                RECT full;
                SetRect(&full, 0, 0, (int)surface->width,
                        (int)surface->height);
                mirror_target_color_fill(surface, &full, color);
            } else {
                DWORD index;
                for (index = 0; index < rect_count; ++index) {
                    RECT area;
                    area.left = rects[index].x1;
                    area.top = rects[index].y1;
                    area.right = rects[index].x2;
                    area.bottom = rects[index].y2;
                    mirror_target_color_fill(surface, &area, color);
                }
            }
        }
    }
    return result ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_set_transform(IDirect3DDevice9 *iface,
        D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix)
{
    D9Device *device = device_from_iface(iface);
    D9WGSetTransform command;
    if (!matrix || (UINT)state >= D9_MAX_TRANSFORMS)
        return D3DERR_INVALIDCALL;
    state_block_record_transform(device, (UINT)state);
    CopyMemory(device->transforms[state], matrix, sizeof(float) * 16);
    command.device_handle = device->handle;
    command.state = (uint32_t)state;
    CopyMemory(command.matrix, matrix, sizeof(command.matrix));
    return emit_command(D9WG_OP_SET_TRANSFORM, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_transform(IDirect3DDevice9 *iface,
        D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
    D9Device *device = device_from_iface(iface);
    if (!matrix || (UINT)state >= D9_MAX_TRANSFORMS)
        return D3DERR_INVALIDCALL;
    CopyMemory(matrix, device->transforms[state], sizeof(float) * 16);
    return D3D_OK;
}

static HRESULT WINAPI device_set_viewport(IDirect3DDevice9 *iface,
        const D3DVIEWPORT9 *viewport)
{
    D9Device *device = device_from_iface(iface);
    D9WGSetViewport command;
    if (!viewport || !viewport->Width || !viewport->Height) {
        TRACE("REJECT SetViewport empty viewport=%08lX",
                (DWORD)(uintptr_t)viewport);
        return D3DERR_INVALIDCALL;
    }
    /*
     * A rejected SetViewport is one of the worst silent failures in this
     * surface: the call returns an error the app almost never checks, the old
     * viewport stays, and every later draw is rendered at whatever scale that
     * viewport implies. A title drawing a model into a small panel then gets it
     * at full-target scale -- oversized and off-centre, with nothing anywhere
     * saying why. Trace both outcomes; the accepted case is a few lines a frame
     * and is what makes the rejected case legible by contrast.
     */
    if (viewport->X > device->display_mode.Width
            || viewport->Y > device->display_mode.Height
            || viewport->Width > device->display_mode.Width - viewport->X
            || viewport->Height > device->display_mode.Height - viewport->Y
            || viewport->MinZ < 0.0f || viewport->MaxZ > 1.0f
            || viewport->MinZ > viewport->MaxZ) {
        TRACE("REJECT SetViewport x=%lu y=%lu %lux%lu minz=%ld/1000 "
                "maxz=%ld/1000 limit=%lux%lu",
                viewport->X, viewport->Y, viewport->Width, viewport->Height,
                (LONG)(viewport->MinZ * 1000.0f),
                (LONG)(viewport->MaxZ * 1000.0f),
                device->display_mode.Width, device->display_mode.Height);
        HOSTLOG_REFUSED("SetViewport %lux%lu at %lu,%lu refused; the target is "
                "%lux%lu, so every later draw keeps the previous viewport's "
                "scale", viewport->Width, viewport->Height, viewport->X,
                viewport->Y, device->display_mode.Width,
                device->display_mode.Height);
        return D3DERR_INVALIDCALL;
    }
    TRACE("OK SetViewport x=%lu y=%lu %lux%lu minz=%ld/1000 maxz=%ld/1000",
            viewport->X, viewport->Y, viewport->Width, viewport->Height,
            (LONG)(viewport->MinZ * 1000.0f),
            (LONG)(viewport->MaxZ * 1000.0f));
    state_block_record_viewport(device);
    device->viewport = *viewport;
    command.device_handle = device->handle;
    command.x = viewport->X;
    command.y = viewport->Y;
    command.width = viewport->Width;
    command.height = viewport->Height;
    command.min_z = viewport->MinZ;
    command.max_z = viewport->MaxZ;
    command.reserved = 0;
    return emit_command(D9WG_OP_SET_VIEWPORT, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_viewport(IDirect3DDevice9 *iface,
        D3DVIEWPORT9 *viewport)
{
    if (!viewport)
        return D3DERR_INVALIDCALL;
    *viewport = device_from_iface(iface)->viewport;
    return D3D_OK;
}

static HRESULT WINAPI device_set_render_state(IDirect3DDevice9 *iface,
        D3DRENDERSTATETYPE state, DWORD value)
{
    D9Device *device = device_from_iface(iface);
    D9WGSetRenderState command;
    if ((UINT)state >= D9_MAX_RENDER_STATES)
        return D3DERR_INVALIDCALL;
    state_block_record_render_state(device, (UINT)state);
    if (device->render_states[state] == value)
        return D3D_OK;
    device->render_states[state] = value;
    command.device_handle = device->handle;
    command.state = state;
    command.value = value;
    command.reserved = 0;
    return emit_command(D9WG_OP_SET_RENDER_STATE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_render_state(IDirect3DDevice9 *iface,
        D3DRENDERSTATETYPE state, DWORD *value)
{
    if (!value || (UINT)state >= D9_MAX_RENDER_STATES)
        return D3DERR_INVALIDCALL;
    *value = device_from_iface(iface)->render_states[state];
    return D3D_OK;
}

static HRESULT WINAPI device_get_texture(IDirect3DDevice9 *iface,
        DWORD stage, IDirect3DBaseTexture9 **texture_out)
{
    D9Device *device = device_from_iface(iface);
    if (!texture_out || stage >= D9_MAX_TEXTURE_STAGES)
        return D3DERR_INVALIDCALL;
    if (device->textures[stage])
        *texture_out = (IDirect3DBaseTexture9 *)&device->textures[stage]->iface;
    else if (device->cube_bindings[stage])
        *texture_out = (IDirect3DBaseTexture9 *)&device->cube_bindings[stage]->iface;
    else
        *texture_out = NULL;
    if (*texture_out)
        IDirect3DBaseTexture9_AddRef(*texture_out);
    return D3D_OK;
}

/*
 * SetTexture takes an IDirect3DBaseTexture9*, so the concrete type has to be
 * recovered from the vtable pointer -- there is no tag in the interface. Doing
 * it by vtable is also the validation: a pointer that is neither of ours is
 * rejected instead of being dereferenced as if it were.
 *
 * Cube textures (M3) bind through the same D9WG_OP_SET_TEXTURE, because the
 * host resource table is one flat handle space and the *host* already knows
 * each handle's kind from its CREATE command. Sending the kind again here would
 * be a second source of truth for the same fact.
 */
static HRESULT WINAPI device_set_texture(IDirect3DDevice9 *iface,
        DWORD stage, IDirect3DBaseTexture9 *texture_iface)
{
    D9Device *device = device_from_iface(iface);
    D9Texture *texture = NULL;
    D9CubeTexture *cube = NULL;
    uint32_t handle = 0;
    D9WGSetTexture command;

    if (stage >= D9_MAX_TEXTURE_STAGES)
        return D3DERR_INVALIDCALL;
    if (texture_iface) {
        const void *vtbl = ((const IDirect3DTexture9 *)texture_iface)->lpVtbl;
        if (vtbl == (const void *)&g_texture_vtbl) {
            texture = (D9Texture *)texture_iface;
            if (texture->device != device)
                return D3DERR_INVALIDCALL;
            handle = texture->handle;
        } else if (vtbl == (const void *)&g_cube_vtbl) {
            cube = (D9CubeTexture *)texture_iface;
            if (cube->device != device)
                return D3DERR_INVALIDCALL;
            handle = cube->handle;
        } else {
            return D3DERR_INVALIDCALL;
        }
    }
    state_block_record_texture(device, stage);
    if (device->textures[stage] == texture && device->cube_bindings[stage] == cube)
        return D3D_OK;
    if (texture) IDirect3DTexture9_AddRef(&texture->iface);
    if (cube) IDirect3DCubeTexture9_AddRef(&cube->iface);
    if (device->textures[stage])
        IDirect3DTexture9_Release(&device->textures[stage]->iface);
    if (device->cube_bindings[stage])
        IDirect3DCubeTexture9_Release(&device->cube_bindings[stage]->iface);
    device->textures[stage] = texture;
    device->cube_bindings[stage] = cube;
    command.device_handle = device->handle;
    command.stage = stage;
    command.texture_handle = handle;
    command.reserved = 0;
    return emit_command(D9WG_OP_SET_TEXTURE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_texture_stage_state(IDirect3DDevice9 *iface,
        DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD *value)
{
    if (!value || stage >= D9_MAX_TEXTURE_STAGES
            || (UINT)state >= D9_MAX_TEXTURE_STAGE_STATES)
        return D3DERR_INVALIDCALL;
    *value = device_from_iface(iface)->texture_stage_states[stage][state];
    return D3D_OK;
}

static HRESULT WINAPI device_set_texture_stage_state(IDirect3DDevice9 *iface,
        DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD value)
{
    D9Device *device = device_from_iface(iface);
    D9WGSetTextureStageState command;
    if (stage >= D9_MAX_TEXTURE_STAGES
            || (UINT)state >= D9_MAX_TEXTURE_STAGE_STATES)
        return D3DERR_INVALIDCALL;
    state_block_record_texture_stage_state(device, stage, (UINT)state);
    if (device->texture_stage_states[stage][state] == value)
        return D3D_OK;
    device->texture_stage_states[stage][state] = value;
    command.device_handle = device->handle;
    command.stage = stage;
    command.state = state;
    command.value = value;
    return emit_command(D9WG_OP_SET_TEXTURE_STAGE_STATE, &command,
            sizeof(command)) ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_validate_device(IDirect3DDevice9 *iface,
        DWORD *passes)
{
    (void)iface;
    if (!passes)
        return D3DERR_INVALIDCALL;
    *passes = 1;
    TRACE("OK Device.ValidateDevice passes=1");
    return D3D_OK;
}

static HRESULT WINAPI device_set_software_vertex_processing(
        IDirect3DDevice9 *iface, WINBOOL software)
{ (void)iface; (void)software; return D3D_OK; }

static WINBOOL WINAPI device_get_software_vertex_processing(
        IDirect3DDevice9 *iface)
{ (void)iface; return FALSE; }

static HRESULT WINAPI device_set_npatch_mode(IDirect3DDevice9 *iface,
        float segments)
{ (void)iface; (void)segments; return D3D_OK; }

static float WINAPI device_get_npatch_mode(IDirect3DDevice9 *iface)
{ (void)iface; return 0.0f; }

/* ---- IDirect3DDevice9: resources, streams, draws ---- */

static HRESULT WINAPI device_create_vertex_buffer(IDirect3DDevice9 *iface,
        UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
        IDirect3DVertexBuffer9 **buffer_out, HANDLE *shared_handle)
{
    D9Device *device = device_from_iface(iface);
    D9VertexBuffer *buffer;
    (void)shared_handle;
    TRACE_MARK_ENTER("Device.CreateVertexBuffer");
    if (!buffer_out) {
        TRACE("FAIL CreateVertexBuffer length=%lu missing output -> %08lX",
                length, (DWORD)D3DERR_INVALIDCALL);
        TRACE_MARK_EXIT("Device.CreateVertexBuffer", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    *buffer_out = NULL;
    if (!length || pool > D3DPOOL_SCRATCH) {
        TRACE("FAIL CreateVertexBuffer length=%lu usage=%08lX fvf=%08lX pool=%lu -> %08lX",
                length, usage, fvf, (DWORD)pool, (DWORD)D3DERR_INVALIDCALL);
        HOSTLOG_FAILED("CreateVertexBuffer refused: length=%lu usage=%08lX "
                "fvf=%08lX pool=%lu", length, usage, fvf, (DWORD)pool);
        TRACE_MARK_EXIT("Device.CreateVertexBuffer", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    buffer = (D9VertexBuffer *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*buffer));
    if (!buffer) {
        TRACE("FAIL CreateVertexBuffer object allocation length=%lu -> %08lX",
                length, (DWORD)E_OUTOFMEMORY);
        TRACE_MARK_EXIT("Device.CreateVertexBuffer", E_OUTOFMEMORY, NULL);
        TRACE_FLUSH();
        return E_OUTOFMEMORY;
    }
    buffer->shadow = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            length);
    if (!buffer->shadow) {
        HeapFree(GetProcessHeap(), 0, buffer);
        TRACE("FAIL CreateVertexBuffer shadow allocation length=%lu -> %08lX",
                length, (DWORD)E_OUTOFMEMORY);
        TRACE_MARK_EXIT("Device.CreateVertexBuffer", E_OUTOFMEMORY, NULL);
        TRACE_FLUSH();
        return E_OUTOFMEMORY;
    }
    buffer->iface.lpVtbl = &g_vb_vtbl;
    buffer->refcount = 1;
    buffer->device = device;
    device_child_add_ref(device);
    buffer->handle = allocate_handle();
    buffer->length = length;
    buffer->usage = usage;
    buffer->fvf = fvf;
    buffer->pool = pool;

    if (!emit_vertex_buffer_create(device, buffer)) {
        TRACE("FAIL CreateVertexBuffer transport length=%lu handle=%08lX -> %08lX",
                length, buffer->handle, (DWORD)D3DERR_DRIVERINTERNALERROR);
        device_child_release(device);
        HeapFree(GetProcessHeap(), 0, buffer->shadow);
        HeapFree(GetProcessHeap(), 0, buffer);
        TRACE_MARK_EXIT("Device.CreateVertexBuffer",
                D3DERR_DRIVERINTERNALERROR, NULL);
        TRACE_FLUSH();
        return D3DERR_DRIVERINTERNALERROR;
    }
    buffer->next_device_resource = device->vertex_buffers;
    device->vertex_buffers = buffer;
    *buffer_out = &buffer->iface;
    TRACE_BUFFER_CREATED(FALSE, length);
    TRACE_REGISTER_RANGE("VB_OBJECT", g_trace_vb_count, buffer, buffer,
            sizeof(*buffer));
    TRACE_REGISTER_RANGE("VB_SHADOW", g_trace_vb_count, buffer,
            buffer->shadow, buffer->length);
#ifdef D9WG_DIAGNOSTIC_TRACE
    if (g_trace_vb_count <= 4 || !(g_trace_vb_count & 127))
        TRACE("OK CreateVertexBuffer count=%ld bytes=%lu length=%lu usage=%08lX fvf=%08lX pool=%lu handle=%08lX object=%08lX shadow=%08lX shadow_end=%08lX",
                g_trace_vb_count, (DWORD)g_trace_vb_bytes, length, usage, fvf,
                (DWORD)pool, buffer->handle, (DWORD)(uintptr_t)*buffer_out,
                (DWORD)(uintptr_t)buffer->shadow,
                (DWORD)(uintptr_t)(buffer->shadow + buffer->length));
#endif
    TRACE_MARK_EXIT("Device.CreateVertexBuffer", D3D_OK, *buffer_out);
    return D3D_OK;
}

static HRESULT WINAPI device_create_index_buffer(IDirect3DDevice9 *iface,
        UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
        IDirect3DIndexBuffer9 **buffer_out, HANDLE *shared_handle)
{
    D9Device *device = device_from_iface(iface);
    D9IndexBuffer *buffer;
    UINT index_size;
    (void)shared_handle;

    TRACE_MARK_ENTER("Device.CreateIndexBuffer");
    if (!buffer_out) {
        TRACE("FAIL CreateIndexBuffer length=%lu missing output -> %08lX",
                length, (DWORD)D3DERR_INVALIDCALL);
        TRACE_MARK_EXIT("Device.CreateIndexBuffer", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    *buffer_out = NULL;
    if (!length || pool > D3DPOOL_SCRATCH) {
        TRACE("FAIL CreateIndexBuffer length=%lu usage=%08lX format=%08lX pool=%lu -> %08lX",
                length, usage, (DWORD)format, (DWORD)pool,
                (DWORD)D3DERR_INVALIDCALL);
        HOSTLOG_FAILED("CreateIndexBuffer refused: length=%lu usage=%08lX "
                "format=%08lX pool=%lu", length, usage, (DWORD)format,
                (DWORD)pool);
        TRACE_MARK_EXIT("Device.CreateIndexBuffer", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    if (format == D3DFMT_INDEX16) index_size = 2;
    else if (format == D3DFMT_INDEX32) index_size = 4;
    else {
        TRACE("FAIL CreateIndexBuffer format=%08lX -> %08lX",
                (DWORD)format, (DWORD)D3DERR_INVALIDCALL);
        TRACE_MARK_EXIT("Device.CreateIndexBuffer", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    if (length % index_size) {
        TRACE("FAIL CreateIndexBuffer unaligned length=%lu index_size=%lu -> %08lX",
                length, index_size, (DWORD)D3DERR_INVALIDCALL);
        TRACE_MARK_EXIT("Device.CreateIndexBuffer", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }

    buffer = (D9IndexBuffer *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*buffer));
    if (!buffer) {
        TRACE("FAIL CreateIndexBuffer object allocation length=%lu -> %08lX",
                length, (DWORD)E_OUTOFMEMORY);
        TRACE_MARK_EXIT("Device.CreateIndexBuffer", E_OUTOFMEMORY, NULL);
        TRACE_FLUSH();
        return E_OUTOFMEMORY;
    }
    buffer->shadow = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            length);
    if (!buffer->shadow) {
        HeapFree(GetProcessHeap(), 0, buffer);
        TRACE("FAIL CreateIndexBuffer shadow allocation length=%lu -> %08lX",
                length, (DWORD)E_OUTOFMEMORY);
        TRACE_MARK_EXIT("Device.CreateIndexBuffer", E_OUTOFMEMORY, NULL);
        TRACE_FLUSH();
        return E_OUTOFMEMORY;
    }
    buffer->iface.lpVtbl = &g_ib_vtbl;
    buffer->refcount = 1;
    buffer->device = device;
    device_child_add_ref(device);
    buffer->handle = allocate_handle();
    buffer->length = length;
    buffer->usage = usage;
    buffer->format = format;
    buffer->pool = pool;

    if (!emit_index_buffer_create(device, buffer)) {
        TRACE("FAIL CreateIndexBuffer transport length=%lu handle=%08lX -> %08lX",
                length, buffer->handle, (DWORD)D3DERR_DRIVERINTERNALERROR);
        device_child_release(device);
        HeapFree(GetProcessHeap(), 0, buffer->shadow);
        HeapFree(GetProcessHeap(), 0, buffer);
        TRACE_MARK_EXIT("Device.CreateIndexBuffer",
                D3DERR_DRIVERINTERNALERROR, NULL);
        TRACE_FLUSH();
        return D3DERR_DRIVERINTERNALERROR;
    }
    buffer->next_device_resource = device->index_buffers;
    device->index_buffers = buffer;
    *buffer_out = &buffer->iface;
    TRACE_BUFFER_CREATED(TRUE, length);
    TRACE_REGISTER_RANGE("IB_OBJECT", g_trace_ib_count, buffer, buffer,
            sizeof(*buffer));
    TRACE_REGISTER_RANGE("IB_SHADOW", g_trace_ib_count, buffer,
            buffer->shadow, buffer->length);
#ifdef D9WG_DIAGNOSTIC_TRACE
    if (g_trace_ib_count <= 4 || !(g_trace_ib_count & 127))
        TRACE("OK CreateIndexBuffer count=%ld bytes=%lu length=%lu usage=%08lX format=%08lX pool=%lu handle=%08lX object=%08lX shadow=%08lX shadow_end=%08lX",
                g_trace_ib_count, (DWORD)g_trace_ib_bytes, length, usage,
                (DWORD)format, (DWORD)pool, buffer->handle,
                (DWORD)(uintptr_t)*buffer_out,
                (DWORD)(uintptr_t)buffer->shadow,
                (DWORD)(uintptr_t)(buffer->shadow + buffer->length));
#endif
    TRACE_MARK_EXIT("Device.CreateIndexBuffer", D3D_OK, *buffer_out);
    return D3D_OK;
}

static HRESULT WINAPI device_create_texture(IDirect3DDevice9 *iface,
        UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
        D3DPOOL pool, IDirect3DTexture9 **texture_out, HANDLE *shared_handle)
{
    D9Device *device = device_from_iface(iface);
    D9Texture *texture;
    UINT full_levels;
    UINT level;
    UINT level_width;
    UINT level_height;
    HRESULT failure = E_OUTOFMEMORY;
    (void)shared_handle;

    TRACE("CALL CreateTexture %lux%lu levels=%lu usage=%08lX "
            "format=%08lX pool=%lu", width, height, levels, usage,
            (DWORD)format, (DWORD)pool);

    if (!texture_out) {
        TRACE("FAIL CreateTexture missing output -> %08lX",
                (DWORD)D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }
    *texture_out = NULL;
    if (!width || !height || width > 4096 || height > 4096
            || !supported_texture_format(format)
            || (usage & D3DUSAGE_AUTOGENMIPMAP)
            || ((usage & D3DUSAGE_DEPTHSTENCIL) && pool != D3DPOOL_DEFAULT)
            || ((usage & D3DUSAGE_RENDERTARGET)
                && (pool != D3DPOOL_DEFAULT
                    || !supported_render_target_format(format)))
            || pool > D3DPOOL_SCRATCH) {

        TRACE("FAIL CreateTexture invalid arguments -> %08lX",
                (DWORD)D3DERR_INVALIDCALL);
        HOSTLOG_FAILED("CreateTexture refused: %lux%lu levels=%lu usage=%08lX "
                "format=%08lX pool=%lu", width, height, levels, usage,
                (DWORD)format, (DWORD)pool);
        return D3DERR_INVALIDCALL;
    }
    full_levels = full_mip_level_count(width, height);
    if (!levels) levels = full_levels;
    if (levels > full_levels) {
        TRACE("FAIL CreateTexture levels=%lu exceeds full_levels=%lu -> %08lX",
                levels, full_levels, (DWORD)D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }

    texture = (D9Texture *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*texture));
    if (!texture) {
        TRACE("FAIL CreateTexture object allocation -> %08lX",
                (DWORD)E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    texture->levels = (D9TextureLevel *)HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, levels * sizeof(*texture->levels));
    if (!texture->levels) {
        HeapFree(GetProcessHeap(), 0, texture);
        TRACE("FAIL CreateTexture level allocation levels=%lu -> %08lX",
                levels, (DWORD)E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    texture->iface.lpVtbl = &g_texture_vtbl;
    texture->refcount = 1;
    texture->device = device;
    texture->handle = allocate_handle();
    texture->width = width;
    texture->height = height;
    texture->level_count = levels;
    texture->usage = usage;
    texture->format = format;
    texture->pool = pool;
    device_child_add_ref(device);

    level_width = width;
    level_height = height;
    for (level = 0; level < levels; ++level) {
        D9TextureLevel *level_data = &texture->levels[level];
        level_data->width = level_width;
        level_data->height = level_height;
        if (!texture_level_layout(format, level_width, level_height,
                &level_data->row_pitch, &level_data->row_count,
                &level_data->byte_count))
            goto allocation_failed;
        /* Targets are not directly lockable.  A render target can acquire a
         * lazy CPU mirror later when Clear/ColorFill gives us exact contents;
         * texture_lock_level() therefore rejects the usage flags explicitly. */
        if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) {
            if (level_width > 1) level_width >>= 1;
            if (level_height > 1) level_height >>= 1;
            continue;
        }
        level_data->shadow = (BYTE *)HeapAlloc(GetProcessHeap(),
                HEAP_ZERO_MEMORY, level_data->byte_count);
        if (!level_data->shadow)
            goto allocation_failed;
        level_data->shadow_valid = TRUE;
        if (level_width > 1) level_width >>= 1;
        if (level_height > 1) level_height >>= 1;
    }

    if (!emit_texture_create(device, texture)) {
        failure = D3DERR_DRIVERINTERNALERROR;
        goto allocation_failed;
    }
    texture->next_device_resource = device->texture_resources;
    device->texture_resources = texture;
    *texture_out = &texture->iface;
    TRACE_REGISTER_RANGE("TEXTURE_OBJECT", texture->handle, texture, texture,
            sizeof(*texture));
    TRACE_REGISTER_RANGE("TEXTURE_SHADOW0", texture->handle, texture,
            texture->levels[0].shadow, texture->levels[0].byte_count);
    TRACE("OK CreateTexture handle=%08lX object=%08lX %lux%lu levels=%lu "
            "usage=%08lX format=%08lX pool=%lu shadow0=%08lX "
            "shadow0_end=%08lX shadow0_bytes=%lu", texture->handle,
            (DWORD)(uintptr_t)*texture_out, width, height, levels, usage,
            (DWORD)format, (DWORD)pool,
            (DWORD)(uintptr_t)texture->levels[0].shadow,
            (DWORD)(uintptr_t)(texture->levels[0].shadow
                    ? texture->levels[0].shadow
                            + texture->levels[0].byte_count
                    : NULL),
            texture->levels[0].byte_count);
    return D3D_OK;

allocation_failed:
    for (level = 0; level < levels; ++level) {
        if (texture->levels[level].shadow)
            HeapFree(GetProcessHeap(), 0, texture->levels[level].shadow);
    }
    device_child_release(device);
    HeapFree(GetProcessHeap(), 0, texture->levels);
    HeapFree(GetProcessHeap(), 0, texture);
    TRACE("FAIL CreateTexture %lux%lu levels=%lu usage=%08lX "
            "format=%08lX pool=%lu -> %08lX", width, height, levels, usage,
            (DWORD)format, (DWORD)pool, (DWORD)failure);
    HOSTLOG_FAILED("CreateTexture failed: %lux%lu levels=%lu usage=%08lX "
            "format=%08lX pool=%lu hr=%08lX", width, height, levels, usage,
            (DWORD)format, (DWORD)pool, (DWORD)failure);
    return failure;
}

static HRESULT WINAPI device_set_stream_source(IDirect3DDevice9 *iface,
        UINT stream, IDirect3DVertexBuffer9 *buffer_iface, UINT offset,
        UINT stride)
{
    D9Device *device = device_from_iface(iface);
    D9VertexBuffer *buffer = buffer_iface ? vb_from_iface(buffer_iface) : NULL;
    D9WGSetStreamSource command;
    if (stream >= D9_MAX_STREAMS) {

        return D3DERR_INVALIDCALL;
    }
    if (buffer && (buffer_iface->lpVtbl != &g_vb_vtbl
            || buffer->device != device))
        return D3DERR_INVALIDCALL;
    if (buffer && offset >= buffer->length)
        return D3DERR_INVALIDCALL;
    state_block_record_stream(device, stream);
    if (device->streams[stream].buffer == buffer
            && device->streams[stream].stride == stride
            && device->streams[stream].offset == offset)
        return D3D_OK;
    if (buffer)
        IDirect3DVertexBuffer9_AddRef(buffer_iface);
    if (device->streams[stream].buffer)
        IDirect3DVertexBuffer9_Release(&device->streams[stream].buffer->iface);
    device->streams[stream].buffer = buffer;
    device->streams[stream].stride = stride;
    device->streams[stream].offset = offset;
    command.device_handle = device->handle;
    command.stream = stream;
    command.buffer_handle = buffer ? buffer->handle : 0;
    command.stride = stride;
    command.offset_in_bytes = offset;
    command.reserved = 0;
    return emit_command(D9WG_OP_SET_STREAM_SOURCE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_stream_source(IDirect3DDevice9 *iface,
        UINT stream, IDirect3DVertexBuffer9 **buffer_out, UINT *offset_out,
        UINT *stride_out)
{
    D9Device *device = device_from_iface(iface);
    if (stream >= D9_MAX_STREAMS || !buffer_out || !stride_out)
        return D3DERR_INVALIDCALL;
    if (offset_out) *offset_out = device->streams[stream].offset;
    *stride_out = device->streams[stream].stride;
    *buffer_out = device->streams[stream].buffer
            ? &device->streams[stream].buffer->iface : NULL;
    if (*buffer_out)
        IDirect3DVertexBuffer9_AddRef(*buffer_out);
    return D3D_OK;
}

static HRESULT WINAPI device_set_indices(IDirect3DDevice9 *iface,
        IDirect3DIndexBuffer9 *buffer_iface)
{
    D9Device *device = device_from_iface(iface);
    D9IndexBuffer *buffer = buffer_iface ? ib_from_iface(buffer_iface) : NULL;
    D9WGSetIndices command;

    if (buffer && (buffer_iface->lpVtbl != &g_ib_vtbl
            || buffer->device != device))
        return D3DERR_INVALIDCALL;
    state_block_record_indices(device);
    if (device->index_buffer == buffer)
        return D3D_OK;
    if (buffer)
        IDirect3DIndexBuffer9_AddRef(buffer_iface);
    if (device->index_buffer)
        IDirect3DIndexBuffer9_Release(&device->index_buffer->iface);
    device->index_buffer = buffer;
    command.device_handle = device->handle;
    command.buffer_handle = buffer ? buffer->handle : 0;
    return emit_command(D9WG_OP_SET_INDICES, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_indices(IDirect3DDevice9 *iface,
        IDirect3DIndexBuffer9 **buffer_out)
{
    D9Device *device = device_from_iface(iface);
    if (!buffer_out)
        return D3DERR_INVALIDCALL;
    *buffer_out = device->index_buffer ? &device->index_buffer->iface : NULL;
    if (*buffer_out)
        IDirect3DIndexBuffer9_AddRef(*buffer_out);
    return D3D_OK;
}

static BOOL device_has_vertex_format(D9Device *device)
{
    return device->vertex_declaration != NULL || device->fvf != 0;
}

static HRESULT WINAPI device_draw_primitive(IDirect3DDevice9 *iface,
        D3DPRIMITIVETYPE primitive_type, UINT start_vertex,
        UINT primitive_count)
{
    D9Device *device = device_from_iface(iface);
    D9WGDrawPrimitive command;
    UINT vertex_count = 0;
    UINT available_vertices;

    if (!device->streams[0].buffer || !device->streams[0].stride
            || device->streams[0].buffer->locked
            || !device_has_vertex_format(device) || !primitive_count
            || !primitive_element_count(primitive_type, primitive_count,
                    &vertex_count)) {
        return D3DERR_INVALIDCALL;
    }
    /* Vertices addressable by this draw start after the stream's
     * OffsetInBytes, not at the start of the buffer. */
    available_vertices = (device->streams[0].buffer->length
            - device->streams[0].offset) / device->streams[0].stride;
    if (start_vertex > available_vertices
            || vertex_count > available_vertices - start_vertex) {

        return D3DERR_INVALIDCALL;
    }
    command.device_handle = device->handle;
    command.primitive_type = primitive_type;
    command.start_vertex = start_vertex;
    command.primitive_count = primitive_count;

    if (!emit_command(D9WG_OP_DRAW_PRIMITIVE, &command, sizeof(command)))
        return D3DERR_DRIVERINTERNALERROR;
    invalidate_render_target_mirrors(device);
    return D3D_OK;
}

static HRESULT WINAPI device_draw_indexed_primitive(IDirect3DDevice9 *iface,
        D3DPRIMITIVETYPE primitive_type, INT base_vertex_index,
        UINT min_vertex_index, UINT vertex_count, UINT start_index,
        UINT primitive_count)
{
    D9Device *device = device_from_iface(iface);
    D9WGDrawIndexedPrimitive command;
    UINT index_count = 0;
    UINT index_size;
    UINT available_indices;
    UINT available_vertices;

    if (!device->streams[0].buffer || !device->streams[0].stride
            || device->streams[0].buffer->locked || !device->index_buffer
            || device->index_buffer->locked || !device_has_vertex_format(device)
            || !primitive_count || !vertex_count
            || !primitive_element_count(primitive_type, primitive_count,
                    &index_count)) {

        return D3DERR_INVALIDCALL;
    }
    index_size = device->index_buffer->format == D3DFMT_INDEX16 ? 2u : 4u;
    available_indices = device->index_buffer->length / index_size;
    if (start_index > available_indices
            || index_count > available_indices - start_index) {

        return D3DERR_INVALIDCALL;
    }
    /* Vertices addressable by this draw start after the stream's
     * OffsetInBytes, not at the start of the buffer. */
    available_vertices = (device->streams[0].buffer->length
            - device->streams[0].offset) / device->streams[0].stride;
    if (min_vertex_index > available_vertices
            || vertex_count > available_vertices - min_vertex_index) {

        return D3DERR_INVALIDCALL;
    }

    command.device_handle = device->handle;
    command.primitive_type = primitive_type;
    command.base_vertex_index = base_vertex_index;
    command.min_vertex_index = min_vertex_index;
    command.vertex_count = vertex_count;
    command.start_index = start_index;
    command.primitive_count = primitive_count;
    if (!emit_command(D9WG_OP_DRAW_INDEXED_PRIMITIVE, &command,
            sizeof(command)))
        return D3DERR_DRIVERINTERNALERROR;
    invalidate_render_target_mirrors(device);
    return D3D_OK;
}

static void clear_stream_zero_after_up(D9Device *device)
{
    D9VertexBuffer *old = device->streams[0].buffer;
    device->streams[0].buffer = NULL;
    device->streams[0].stride = 0;
    if (old) IDirect3DVertexBuffer9_Release(&old->iface);
}

static void clear_indices_after_indexed_up(D9Device *device)
{
    D9IndexBuffer *old = device->index_buffer;
    device->index_buffer = NULL;
    if (old) IDirect3DIndexBuffer9_Release(&old->iface);
}

static HRESULT WINAPI device_draw_primitive_up(IDirect3DDevice9 *iface,
        D3DPRIMITIVETYPE primitive_type, UINT primitive_count,
        const void *vertex_data, UINT stride)
{
    D9Device *device = device_from_iface(iface);
    D9WGDrawPrimitiveUP command;
    UINT vertex_count;
    UINT vertex_bytes;
    BOOL result;

    if (!vertex_data || !stride || !device_has_vertex_format(device)
            || !primitive_count
            || !primitive_element_count(primitive_type, primitive_count,
                    &vertex_count)
            || !multiply_u32(vertex_count, stride, &vertex_bytes))
        return D3DERR_INVALIDCALL;
    ZeroMemory(&command, sizeof(command));
    command.device_handle = device->handle;
    command.primitive_type = primitive_type;
    command.primitive_count = primitive_count;
    command.stride = stride;
    command.vertex_count = vertex_count;
    command.vertex_bytes = vertex_bytes;
    result = emit_draw_primitive_up(&command, vertex_data);
    if (result) {
        invalidate_render_target_mirrors(device);
        clear_stream_zero_after_up(device);
    }
    return result ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_draw_indexed_primitive_up(
        IDirect3DDevice9 *iface, D3DPRIMITIVETYPE primitive_type,
        UINT min_vertex_index, UINT vertex_count, UINT primitive_count,
        const void *index_data, D3DFORMAT index_format,
        const void *vertex_data, UINT stride)
{
    D9Device *device = device_from_iface(iface);
    D9WGDrawIndexedPrimitiveUP command;
    UINT index_count;
    UINT index_size;
    UINT vertex_elements;
    BOOL result;

    if (!index_data || !vertex_data || !stride
            || !device_has_vertex_format(device) || !primitive_count
            || !vertex_count
            || !primitive_element_count(primitive_type, primitive_count,
                    &index_count))
        return D3DERR_INVALIDCALL;
    if (index_format == D3DFMT_INDEX16) index_size = 2;
    else if (index_format == D3DFMT_INDEX32) index_size = 4;
    else return D3DERR_INVALIDCALL;
    if (min_vertex_index > 0xFFFFFFFFu - vertex_count)
        return D3DERR_INVALIDCALL;
    vertex_elements = min_vertex_index + vertex_count;
    ZeroMemory(&command, sizeof(command));
    if (!multiply_u32(index_count, index_size, &command.index_bytes)
            || !multiply_u32(vertex_elements, stride, &command.vertex_bytes))
        return D3DERR_INVALIDCALL;
    command.device_handle = device->handle;
    command.primitive_type = primitive_type;
    command.min_vertex_index = min_vertex_index;
    command.vertex_count = vertex_count;
    command.primitive_count = primitive_count;
    command.index_format = index_format;
    command.stride = stride;
    command.index_count = index_count;
    result = emit_draw_indexed_primitive_up(&command, index_data, vertex_data);
    if (result) {
        invalidate_render_target_mirrors(device);
        clear_stream_zero_after_up(device);
        clear_indices_after_indexed_up(device);
    }
    return result ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_create_vertex_declaration(
        IDirect3DDevice9 *iface, const D3DVERTEXELEMENT9 *elements,
        IDirect3DVertexDeclaration9 **decl_out)
{
    D9Device *device = device_from_iface(iface);
    D9VertexDeclaration *decl;
    D9WGVertexElement wire[D3DMAXDECLLENGTH];
    UINT count;

    if (!decl_out || !elements)
        return D3DERR_INVALIDCALL;
    *decl_out = NULL;
    if (!parse_vertex_declaration(elements, wire, &count)) {
        /* The single most likely silent killer for a real game: one
         * unsupported D3DDECLTYPE/usage anywhere in the array rejects the
         * whole declaration, and without this the app just never draws. */

        return D3DERR_INVALIDCALL;
    }

    decl = (D9VertexDeclaration *)HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, sizeof(*decl));
    if (!decl)
        return E_OUTOFMEMORY;
    decl->iface.lpVtbl = &g_decl_vtbl;
    decl->refcount = 1;
    decl->device = device;
    decl->handle = allocate_handle();
    decl->element_count = count;
    CopyMemory(decl->elements, elements, count * sizeof(D3DVERTEXELEMENT9));
    device_child_add_ref(device);

    if (!emit_vertex_declaration_create(device, decl->handle, wire, count)) {
        device_child_release(device);
        HeapFree(GetProcessHeap(), 0, decl);
        return D3DERR_DRIVERINTERNALERROR;
    }
    decl->next_device_resource = device->vertex_declarations;
    device->vertex_declarations = decl;
    *decl_out = &decl->iface;
    return D3D_OK;
}

static HRESULT WINAPI device_set_vertex_declaration(IDirect3DDevice9 *iface,
        IDirect3DVertexDeclaration9 *decl_iface)
{
    D9Device *device = device_from_iface(iface);
    D9VertexDeclaration *decl = decl_iface ? decl_from_iface(decl_iface) : NULL;
    D9WGSetVertexDeclaration command;

    if (decl && (decl_iface->lpVtbl != &g_decl_vtbl || decl->device != device))
        return D3DERR_INVALIDCALL;
    state_block_record_vertex_format(device);
    if (decl)
        IDirect3DVertexDeclaration9_AddRef(decl_iface);
    if (device->vertex_declaration)
        IDirect3DVertexDeclaration9_Release(
                &device->vertex_declaration->iface);
    device->vertex_declaration = decl;
    device->fvf = 0;
    command.device_handle = device->handle;
    command.declaration_handle = decl ? decl->handle : 0;
    return emit_command(D9WG_OP_SET_VERTEX_DECLARATION, &command,
            sizeof(command)) ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_vertex_declaration(IDirect3DDevice9 *iface,
        IDirect3DVertexDeclaration9 **decl_out)
{
    D9Device *device = device_from_iface(iface);
    if (!decl_out)
        return D3DERR_INVALIDCALL;
    *decl_out = device->vertex_declaration
            ? &device->vertex_declaration->iface : NULL;
    if (*decl_out)
        IDirect3DVertexDeclaration9_AddRef(*decl_out);
    return D3D_OK;
}

static HRESULT WINAPI device_set_fvf(IDirect3DDevice9 *iface, DWORD fvf)
{
    D9Device *device = device_from_iface(iface);
    D9WGVertexElement wire[D3DMAXDECLLENGTH];
    UINT count;

    if (!fvf_to_declaration(fvf, wire, &count)) {

        return D3DERR_INVALIDCALL;
    }
    state_block_record_vertex_format(device);
    if (device->vertex_declaration) {
        IDirect3DVertexDeclaration9_Release(
                &device->vertex_declaration->iface);
        device->vertex_declaration = NULL;
    }
    device->fvf = fvf;
    return emit_set_fvf(device, fvf, wire, count)
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_fvf(IDirect3DDevice9 *iface, DWORD *fvf)
{
    if (!fvf)
        return D3DERR_INVALIDCALL;
    *fvf = device_from_iface(iface)->fvf;
    return D3D_OK;
}

/* ---- programmable shaders (M2) ----
 *
 * D3D9 gives SetVertexShader/SetPixelShader a real COM pointer type, unlike
 * D3D8's overloaded DWORD, so there is no FVF-vs-shader-handle ambiguity to
 * resolve. Passing NULL means "go back to fixed function", which stays a
 * fully supported mode.
 *
 * The guest does no semantic work on the bytecode: it counts tokens, hashes
 * them, keeps a shadow copy for GetFunction and Reset replay, and ships the
 * raw stream. Translation to WGSL is entirely host-side (plan 4.2), so a
 * shader this build cannot translate still creates successfully here and is
 * refused where the failure is visible and countable -- in the executor --
 * rather than being second-guessed from inside the guest.
 */
static D9Shader *vertex_shader_from_iface(IDirect3DVertexShader9 *iface)
{
    return (D9Shader *)iface;
}

static D9Shader *pixel_shader_from_iface(IDirect3DPixelShader9 *iface)
{
    return (D9Shader *)iface;
}

static HRESULT create_shader(D9Device *device, const DWORD *bytecode,
        BOOL want_pixel, D9Shader **shader_out)
{
    D9Shader *shader;
    UINT token_count = 0;
    BOOL is_pixel = FALSE;
    uint32_t code_bytes;

    *shader_out = NULL;
    if (!bytecode) {
        TRACE("FAIL CreateShader want_pixel=%lu bytecode=NULL -> %08lX",
                (DWORD)want_pixel, (DWORD)D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }
    if (!shader_token_count(bytecode, &token_count, &is_pixel)
            || is_pixel != want_pixel) {
        /* Rejected before anything is emitted, so without this line the call
         * leaves no record whatsoever: no CMD, no OK, no FAIL. An app whose
         * shader is turned down here sees only D3DERR_INVALIDCALL, and the
         * trace shows only the gap where the shader should have been. */
        TRACE("FAIL CreateShader version=%08lX want_pixel=%lu is_pixel=%lu "
                "tokens=%lu -> %08lX", bytecode[0], (DWORD)want_pixel,
                (DWORD)is_pixel, token_count, (DWORD)D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }

    code_bytes = (uint32_t)token_count * 4u;
    shader = (D9Shader *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*shader));
    if (!shader)
        return E_OUTOFMEMORY;
    shader->code = (DWORD *)HeapAlloc(GetProcessHeap(), 0, code_bytes);
    if (!shader->code) {
        HeapFree(GetProcessHeap(), 0, shader);
        return E_OUTOFMEMORY;
    }
    CopyMemory(shader->code, bytecode, code_bytes);
    if (want_pixel)
        shader->iface.pixel.lpVtbl = &g_pixel_shader_vtbl;
    else
        shader->iface.vertex.lpVtbl = &g_vertex_shader_vtbl;
    shader->refcount = 1;
    shader->device = device;
    shader->handle = allocate_shader_handle();
    shader->is_pixel = want_pixel;
    shader->token_count = token_count;
    shader_bytecode_hash(shader->code, token_count, &shader->hash_low,
            &shader->hash_high);
    /* Before the emit, so a shader whose CREATE command cannot be reserved
     * (DMA ring full) still contributes to the corpus. */
    dump_shader_bytecode(shader->code, token_count, want_pixel,
            shader->hash_low, shader->hash_high);
    device_child_add_ref(device);

    if (!emit_shader_create(device, shader)) {
        device_child_release(device);
        HeapFree(GetProcessHeap(), 0, shader->code);
        HeapFree(GetProcessHeap(), 0, shader);
        return D3DERR_DRIVERINTERNALERROR;
    }

    shader->next_device_resource = device->shaders;
    device->shaders = shader;
    *shader_out = shader;
    TRACE("OK CreateShader version=%08lX pixel=%lu tokens=%lu handle=%08lX "
            "hash=%08lX%08lX", shader->code[0], (DWORD)want_pixel, token_count,
            shader->handle, shader->hash_high, shader->hash_low);
    return D3D_OK;
}

static void shader_destroy(D9Shader *shader)
{
    D9Shader **link = &shader->device->shaders;
    D9WGDestroyResource destroy;

    while (*link && *link != shader)
        link = &(*link)->next_device_resource;
    if (*link)
        *link = shader->next_device_resource;
    destroy.resource_handle = shader->handle;
    destroy.resource_kind = shader->is_pixel ? D9WG_RESOURCE_PIXEL_SHADER
            : D9WG_RESOURCE_VERTEX_SHADER;
    emit_command(D9WG_OP_DESTROY_RESOURCE, &destroy, sizeof(destroy));
    device_child_release(shader->device);
    HeapFree(GetProcessHeap(), 0, shader->code);
    HeapFree(GetProcessHeap(), 0, shader);
}

static HRESULT emit_set_shader(D9Device *device, D9Shader *shader,
        BOOL is_pixel)
{
    D9WGSetShader command;
    command.device_handle = device->handle;
    command.shader_handle = shader ? shader->handle : 0;
    return emit_command(is_pixel ? D9WG_OP_SET_PIXEL_SHADER
                    : D9WG_OP_SET_VERTEX_SHADER,
            &command, sizeof(command)) ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_create_vertex_shader(IDirect3DDevice9 *iface,
        const DWORD *bytecode, IDirect3DVertexShader9 **shader_out)
{
    D9Shader *shader;
    HRESULT hr;

    if (!shader_out)
        return D3DERR_INVALIDCALL;
    *shader_out = NULL;
    hr = create_shader(device_from_iface(iface), bytecode, FALSE, &shader);
    if (FAILED(hr))
        return hr;
    *shader_out = &shader->iface.vertex;
    return D3D_OK;
}

static HRESULT WINAPI device_create_pixel_shader(IDirect3DDevice9 *iface,
        const DWORD *bytecode, IDirect3DPixelShader9 **shader_out)
{
    D9Shader *shader;
    HRESULT hr;

    if (!shader_out)
        return D3DERR_INVALIDCALL;
    *shader_out = NULL;
    hr = create_shader(device_from_iface(iface), bytecode, TRUE, &shader);
    if (FAILED(hr))
        return hr;
    *shader_out = &shader->iface.pixel;
    return D3D_OK;
}

static HRESULT WINAPI device_set_vertex_shader(IDirect3DDevice9 *iface,
        IDirect3DVertexShader9 *shader_iface)
{
    D9Device *device = device_from_iface(iface);
    D9Shader *shader = shader_iface ? vertex_shader_from_iface(shader_iface) : NULL;

    if (shader && (shader_iface->lpVtbl != &g_vertex_shader_vtbl
            || shader->device != device))
        return D3DERR_INVALIDCALL;
    state_block_record_vertex_shader(device);
    if (shader)
        IDirect3DVertexShader9_AddRef(shader_iface);
    if (device->vertex_shader)
        IDirect3DVertexShader9_Release(&device->vertex_shader->iface.vertex);
    device->vertex_shader = shader;
    return emit_set_shader(device, shader, FALSE);
}

static HRESULT WINAPI device_get_vertex_shader(IDirect3DDevice9 *iface,
        IDirect3DVertexShader9 **shader_out)
{
    D9Device *device = device_from_iface(iface);
    if (!shader_out)
        return D3DERR_INVALIDCALL;
    *shader_out = device->vertex_shader ? &device->vertex_shader->iface.vertex
            : NULL;
    if (*shader_out)
        IDirect3DVertexShader9_AddRef(*shader_out);
    return D3D_OK;
}

static HRESULT WINAPI device_set_pixel_shader(IDirect3DDevice9 *iface,
        IDirect3DPixelShader9 *shader_iface)
{
    D9Device *device = device_from_iface(iface);
    D9Shader *shader = shader_iface ? pixel_shader_from_iface(shader_iface) : NULL;

    if (shader && (shader_iface->lpVtbl != &g_pixel_shader_vtbl
            || shader->device != device))
        return D3DERR_INVALIDCALL;
    state_block_record_pixel_shader(device);
    if (shader)
        IDirect3DPixelShader9_AddRef(shader_iface);
    if (device->pixel_shader)
        IDirect3DPixelShader9_Release(&device->pixel_shader->iface.pixel);
    device->pixel_shader = shader;
    return emit_set_shader(device, shader, TRUE);
}

static HRESULT WINAPI device_get_pixel_shader(IDirect3DDevice9 *iface,
        IDirect3DPixelShader9 **shader_out)
{
    D9Device *device = device_from_iface(iface);
    if (!shader_out)
        return D3DERR_INVALIDCALL;
    *shader_out = device->pixel_shader ? &device->pixel_shader->iface.pixel
            : NULL;
    if (*shader_out)
        IDirect3DPixelShader9_AddRef(*shader_out);
    return D3D_OK;
}

/* ---- shader constant registers ----
 *
 * Two things happen here beyond storing the value. First, the shadow makes
 * the redundant-set suppression the plan asks for in 7.5 possible: engines
 * routinely re-upload an entire constant bank every frame even when nothing
 * in it changed. Second, when something *has* changed, only the dirty
 * sub-range is sent -- a 32-register bone palette where one bone moved
 * becomes one float4 on the wire, not 32.
 */
static BOOL constant_range_valid(UINT start, UINT count, UINT capacity)
{
    return count != 0 && start < capacity && count <= capacity - start;
}

static HRESULT set_constant_f(D9Device *device, BOOL is_pixel, UINT start,
        const float *data, UINT count)
{
    float (*shadow)[4] = is_pixel ? device->ps_const_f : device->vs_const_f;
    UINT capacity = is_pixel ? D9_MAX_PS_CONST_F : D9_MAX_VS_CONST_F;
    UINT first_dirty = count;
    UINT last_dirty = 0;
    UINT index;

    if (!data || !constant_range_valid(start, count, capacity))
        return D3DERR_INVALIDCALL;
    state_block_record_constant(device, is_pixel, TRUE, FALSE, start, count);
    for (index = 0; index < count; ++index) {
        const float *source = data + index * 4;
        float *target = shadow[start + index];
        if (target[0] == source[0] && target[1] == source[1]
                && target[2] == source[2] && target[3] == source[3])
            continue;
        target[0] = source[0]; target[1] = source[1];
        target[2] = source[2]; target[3] = source[3];
        if (index < first_dirty) first_dirty = index;
        last_dirty = index;
    }
    if (first_dirty > last_dirty)
        return D3D_OK; /* nothing actually changed */
    return emit_shader_constants(device,
            is_pixel ? D9WG_OP_SET_PIXEL_SHADER_CONSTANT_F
                    : D9WG_OP_SET_VERTEX_SHADER_CONSTANT_F,
            start + first_dirty, last_dirty - first_dirty + 1,
            shadow[start + first_dirty],
            (last_dirty - first_dirty + 1) * 16u)
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT set_constant_i(D9Device *device, BOOL is_pixel, UINT start,
        const int *data, UINT count)
{
    int (*shadow)[4] = is_pixel ? device->ps_const_i : device->vs_const_i;
    UINT first_dirty = count;
    UINT last_dirty = 0;
    UINT index;

    if (!data || !constant_range_valid(start, count, D9_MAX_CONST_I))
        return D3DERR_INVALIDCALL;
    state_block_record_constant(device, is_pixel, FALSE, FALSE, start, count);
    for (index = 0; index < count; ++index) {
        const int *source = data + index * 4;
        int *target = shadow[start + index];
        if (target[0] == source[0] && target[1] == source[1]
                && target[2] == source[2] && target[3] == source[3])
            continue;
        target[0] = source[0]; target[1] = source[1];
        target[2] = source[2]; target[3] = source[3];
        if (index < first_dirty) first_dirty = index;
        last_dirty = index;
    }
    if (first_dirty > last_dirty)
        return D3D_OK;
    return emit_shader_constants(device,
            is_pixel ? D9WG_OP_SET_PIXEL_SHADER_CONSTANT_I
                    : D9WG_OP_SET_VERTEX_SHADER_CONSTANT_I,
            start + first_dirty, last_dirty - first_dirty + 1,
            shadow[start + first_dirty],
            (last_dirty - first_dirty + 1) * 16u)
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT set_constant_b(D9Device *device, BOOL is_pixel, UINT start,
        const WINBOOL *data, UINT count)
{
    BOOL *shadow = is_pixel ? device->ps_const_b : device->vs_const_b;
    uint32_t packed[D9_MAX_CONST_B];
    UINT first_dirty = count;
    UINT last_dirty = 0;
    UINT index;

    if (!data || !constant_range_valid(start, count, D9_MAX_CONST_B))
        return D3DERR_INVALIDCALL;
    state_block_record_constant(device, is_pixel, FALSE, TRUE, start, count);
    for (index = 0; index < count; ++index) {
        BOOL value = data[index] ? TRUE : FALSE;
        if (shadow[start + index] == value)
            continue;
        shadow[start + index] = value;
        if (index < first_dirty) first_dirty = index;
        last_dirty = index;
    }
    if (first_dirty > last_dirty)
        return D3D_OK;
    /* One 32-bit slot per register on the wire: the host's uniform layout
     * (plan 9.7) has no packed-bit representation, and this is a handful of
     * bytes either way. */
    for (index = first_dirty; index <= last_dirty; ++index)
        packed[index - first_dirty] = shadow[start + index] ? 1u : 0u;
    return emit_shader_constants(device,
            is_pixel ? D9WG_OP_SET_PIXEL_SHADER_CONSTANT_B
                    : D9WG_OP_SET_VERTEX_SHADER_CONSTANT_B,
            start + first_dirty, last_dirty - first_dirty + 1, packed,
            (last_dirty - first_dirty + 1) * 4u)
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_set_vertex_shader_constant_f(
        IDirect3DDevice9 *iface, UINT start, const float *data, UINT count)
{ return set_constant_f(device_from_iface(iface), FALSE, start, data, count); }

static HRESULT WINAPI device_set_pixel_shader_constant_f(
        IDirect3DDevice9 *iface, UINT start, const float *data, UINT count)
{ return set_constant_f(device_from_iface(iface), TRUE, start, data, count); }

static HRESULT WINAPI device_set_vertex_shader_constant_i(
        IDirect3DDevice9 *iface, UINT start, const int *data, UINT count)
{ return set_constant_i(device_from_iface(iface), FALSE, start, data, count); }

static HRESULT WINAPI device_set_pixel_shader_constant_i(
        IDirect3DDevice9 *iface, UINT start, const int *data, UINT count)
{ return set_constant_i(device_from_iface(iface), TRUE, start, data, count); }

static HRESULT WINAPI device_set_vertex_shader_constant_b(
        IDirect3DDevice9 *iface, UINT start, const WINBOOL *data, UINT count)
{ return set_constant_b(device_from_iface(iface), FALSE, start, data, count); }

static HRESULT WINAPI device_set_pixel_shader_constant_b(
        IDirect3DDevice9 *iface, UINT start, const WINBOOL *data, UINT count)
{ return set_constant_b(device_from_iface(iface), TRUE, start, data, count); }

static HRESULT WINAPI device_get_vertex_shader_constant_f(
        IDirect3DDevice9 *iface, UINT start, float *data, UINT count)
{
    D9Device *device = device_from_iface(iface);
    if (!data || !constant_range_valid(start, count, D9_MAX_VS_CONST_F))
        return D3DERR_INVALIDCALL;
    CopyMemory(data, device->vs_const_f[start], count * 16u);
    return D3D_OK;
}

static HRESULT WINAPI device_get_pixel_shader_constant_f(
        IDirect3DDevice9 *iface, UINT start, float *data, UINT count)
{
    D9Device *device = device_from_iface(iface);
    if (!data || !constant_range_valid(start, count, D9_MAX_PS_CONST_F))
        return D3DERR_INVALIDCALL;
    CopyMemory(data, device->ps_const_f[start], count * 16u);
    return D3D_OK;
}

static HRESULT WINAPI device_get_vertex_shader_constant_i(
        IDirect3DDevice9 *iface, UINT start, int *data, UINT count)
{
    D9Device *device = device_from_iface(iface);
    if (!data || !constant_range_valid(start, count, D9_MAX_CONST_I))
        return D3DERR_INVALIDCALL;
    CopyMemory(data, device->vs_const_i[start], count * 16u);
    return D3D_OK;
}

static HRESULT WINAPI device_get_pixel_shader_constant_i(
        IDirect3DDevice9 *iface, UINT start, int *data, UINT count)
{
    D9Device *device = device_from_iface(iface);
    if (!data || !constant_range_valid(start, count, D9_MAX_CONST_I))
        return D3DERR_INVALIDCALL;
    CopyMemory(data, device->ps_const_i[start], count * 16u);
    return D3D_OK;
}

static HRESULT WINAPI device_get_vertex_shader_constant_b(
        IDirect3DDevice9 *iface, UINT start, WINBOOL *data, UINT count)
{
    D9Device *device = device_from_iface(iface);
    UINT index;
    if (!data || !constant_range_valid(start, count, D9_MAX_CONST_B))
        return D3DERR_INVALIDCALL;
    for (index = 0; index < count; ++index)
        data[index] = device->vs_const_b[start + index];
    return D3D_OK;
}

static HRESULT WINAPI device_get_pixel_shader_constant_b(
        IDirect3DDevice9 *iface, UINT start, WINBOOL *data, UINT count)
{
    D9Device *device = device_from_iface(iface);
    UINT index;
    if (!data || !constant_range_valid(start, count, D9_MAX_CONST_B))
        return D3DERR_INVALIDCALL;
    for (index = 0; index < count; ++index)
        data[index] = device->ps_const_b[start + index];
    return D3D_OK;
}

/* ---- Everything else: honestly not implemented before a later milestone.
 * Typed per-method stubs (rather than one variadic stub) keep stdcall stack
 * cleanup correct on 32-bit XP. Returning D3DERR_INVALIDCALL rather than
 * pretending to succeed matches the D3D8 path's established discipline. */
#define DEV_STUB0(name) \
    static HRESULT WINAPI device_##name(IDirect3DDevice9 *iface) \
    { (void)iface; return D3DERR_INVALIDCALL; }
#define DEV_STUB(name, ...) \
    static HRESULT WINAPI device_##name(IDirect3DDevice9 *iface, __VA_ARGS__)

/*
 * Every refusal here is a real API the game asked for and did not get, and a
 * bare D3DERR_INVALIDCALL is indistinguishable from "the game never called it"
 * -- from the host console, from the trace, from everywhere. That blind spot is
 * what made Kart Rider's missing shop art un-diagnosable: the picture was wrong,
 * the executor reported a clean frame with no dropped draws, and nothing
 * anywhere recorded that the guest had been turned down. Name the refusal so
 * the next round starts from a fact instead of a hypothesis.
 *
 * This is trace-only: the D9WG protocol has no guest-to-host log channel, so
 * the diagnostic DLL is where these surface. `grep STUB` over a trace taken
 * while reproducing tells you in one step which APIs a title actually wanted.
 */
#define UNSUPPORTED(name) \
    do { \
        TRACE("STUB %s -> D3DERR_INVALIDCALL (unimplemented)", name); \
        HOSTLOG_REFUSED("%s is not implemented and returned " \
                "D3DERR_INVALIDCALL", name); \
    } while (0)

DEV_STUB(create_additional_swap_chain, D3DPRESENT_PARAMETERS *params,
        IDirect3DSwapChain9 **out)
{
    TRACE("STUB CreateAdditionalSwapChain device=%08lX params=%08lX outarg=%08lX -> %08lX",
            device_from_iface(iface)->handle, (DWORD)(uintptr_t)params,
            (DWORD)(uintptr_t)out, (DWORD)D3DERR_INVALIDCALL);
    (void)iface;
    (void)params;
    if (out)
        *out = NULL;
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI device_get_swap_chain(IDirect3DDevice9 *iface,
        UINT index, IDirect3DSwapChain9 **out)
{
    D9Device *device = device_from_iface(iface);

    TRACE_MARK_ENTER("Device.GetSwapChain");
    TRACE("CALL GetSwapChain device=%08lX index=%lu outarg=%08lX",
            device->handle, index, (DWORD)(uintptr_t)out);
    if (!out) {
        TRACE("FAIL GetSwapChain index=%lu missing output -> %08lX", index,
                (DWORD)D3DERR_INVALIDCALL);
        TRACE_MARK_EXIT("Device.GetSwapChain", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    *out = NULL;
    if (index != 0) {
        TRACE("FAIL GetSwapChain index=%lu out-of-range -> %08lX", index,
                (DWORD)D3DERR_INVALIDCALL);
        TRACE_MARK_EXIT("Device.GetSwapChain", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }

    *out = &device->implicit_swap_chain.iface;
    IDirect3DSwapChain9_AddRef(*out);
    TRACE("OK GetSwapChain index=0 object=%08lX refs=%ld",
            (DWORD)(uintptr_t)*out,
            InterlockedCompareExchange(&device->implicit_swap_chain.refcount,
                    0, 0));
    TRACE_MARK_EXIT("Device.GetSwapChain", D3D_OK, *out);
    return D3D_OK;
}
/* War3 testing confirmed that engines can gate an
 * entire render branch on this succeeding even when they never read pixels
 * back from the result (StretchRect/LockRect against it still honestly
 * fail -- see the D9Surface struct comment and surface_lock_rect()). Only
 * the single implicit swap chain / single back buffer M1 supports. */
static HRESULT WINAPI device_get_back_buffer(IDirect3DDevice9 *iface,
        UINT swapchain, UINT index, D3DBACKBUFFER_TYPE type,
        IDirect3DSurface9 **out)
{
    D9Device *device = device_from_iface(iface);
    D9Surface *surface;
    TRACE_MARK_ENTER("Device.GetBackBuffer");
    TRACE("CALL GetBackBuffer device=%08lX chain=%lu index=%lu type=%lu outarg=%08lX",
            device->handle, swapchain, index, (DWORD)type,
            (DWORD)(uintptr_t)out);
    if (!out) {
        TRACE_MARK_EXIT("Device.GetBackBuffer", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    *out = NULL;
    if (swapchain || index || type != D3DBACKBUFFER_TYPE_MONO) {
        TRACE("FAIL GetBackBuffer chain=%lu index=%lu type=%lu -> %08lX",
                swapchain, index, (DWORD)type, (DWORD)D3DERR_INVALIDCALL);
        TRACE_MARK_EXIT("Device.GetBackBuffer", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    /* One object per device, handed out again on every call: an app that
     * caches the pointer -- RenderWare caches both this and the auto
     * depth-stencil -- must see the same surface it saw the first time. */
    surface = device->implicit_back_buffer;
    if (!surface) {
        surface = (D9Surface *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                sizeof(*surface));
        if (!surface) {
            TRACE("FAIL GetBackBuffer allocation -> %08lX",
                    (DWORD)E_OUTOFMEMORY);
            TRACE_MARK_EXIT("Device.GetBackBuffer", E_OUTOFMEMORY, NULL);
            return E_OUTOFMEMORY;
        }
        surface->iface.lpVtbl = &g_surface_vtbl;
        surface->device = device;
        surface->swap_chain = &device->implicit_swap_chain;
        surface->width = device->display_mode.Width;
        surface->height = device->display_mode.Height;
        surface->format = device->display_mode.Format;
        device->implicit_back_buffer = surface;
        TRACE_REGISTER_RANGE("SURFACE_BACKBUFFER", 0, surface, surface,
                sizeof(*surface));
        TRACE("SURFACE CREATE kind=backbuffer object=%08lX vtbl=%08lX ref=%ld "
                "device=%08lX texture=00000000 swapchain=%08lX shadow=00000000 "
                "level=0 size=%lux%lu format=%08lX",
                (DWORD)(uintptr_t)&surface->iface,
                (DWORD)(uintptr_t)surface->iface.lpVtbl, surface->refcount,
                (DWORD)(uintptr_t)&device->iface,
                (DWORD)(uintptr_t)&device->implicit_swap_chain.iface,
                surface->width, surface->height, (DWORD)surface->format);
    }
    *out = &surface->iface;
    /* Takes the device reference standing behind this public reference. */
    IDirect3DSurface9_AddRef(*out);
    TRACE("OK GetBackBuffer object=%08lX %lux%lu fmt=%08lX refs=%ld",
            (DWORD)(uintptr_t)*out, surface->width, surface->height,
            (DWORD)surface->format,
            InterlockedCompareExchange(&surface->refcount, 0, 0));
    TRACE_MARK_EXIT("Device.GetBackBuffer", D3D_OK, *out);
    return D3D_OK;
}
DEV_STUB(get_raster_status, UINT swapchain, D3DRASTER_STATUS *status)
{ (void)iface; (void)swapchain; (void)status;
  UNSUPPORTED("Device.GetRasterStatus"); return D3DERR_INVALIDCALL; }
DEV_STUB(create_volume_texture, UINT w, UINT h, UINT d, UINT levels,
        DWORD usage, D3DFORMAT format, D3DPOOL pool,
        IDirect3DVolumeTexture9 **out, HANDLE *shared)
{ (void)iface; (void)w; (void)h; (void)d; (void)levels; (void)usage;
  (void)format; (void)pool; (void)shared;
  if (out) { *out = NULL; }
  UNSUPPORTED("Device.CreateVolumeTexture"); return D3DERR_INVALIDCALL; }
DEV_STUB(update_surface, IDirect3DSurface9 *src, const RECT *src_rect,
        IDirect3DSurface9 *dst, const POINT *dst_point)
{ (void)iface; (void)src; (void)src_rect; (void)dst; (void)dst_point;
  UNSUPPORTED("Device.UpdateSurface"); return D3DERR_INVALIDCALL; }
/*
 * The other classic upload route besides Lock/Unlock: fill a
 * D3DPOOL_SYSTEMMEM texture on the CPU, then blit it into the
 * D3DPOOL_DEFAULT one the device actually samples. Both sides already keep a
 * full per-level shadow here, so this is a shadow-to-shadow copy followed by
 * the same UPDATE_TEXTURE emission an unlock would have produced. Declared
 * after the texture helpers it calls; see the forward declaration above.
 */
static HRESULT WINAPI device_update_texture(IDirect3DDevice9 *iface,
        IDirect3DBaseTexture9 *src_iface, IDirect3DBaseTexture9 *dst_iface)
{
    D9Device *device = device_from_iface(iface);
    D9Texture *source = (D9Texture *)src_iface;
    D9Texture *destination = (D9Texture *)dst_iface;
    UINT level;

    if (!source || !destination
            || source->iface.lpVtbl != &g_texture_vtbl
            || destination->iface.lpVtbl != &g_texture_vtbl
            || source->device != device || destination->device != device
            || source->format != destination->format
            || source->width != destination->width
            || source->height != destination->height)
        return D3DERR_INVALIDCALL;
    /* D3D9 allows the source to carry fewer levels than the destination;
     * only the levels both actually have can be copied. */
    for (level = 0; level < source->level_count
            && level < destination->level_count; ++level) {
        D9TextureLevel *from = &source->levels[level];
        D9TextureLevel *to = &destination->levels[level];
        RECT full;
        if (from->locked || to->locked)
            return D3DERR_INVALIDCALL;
        if (from->byte_count != to->byte_count)
            return D3DERR_INVALIDCALL;
        CopyMemory(to->shadow, from->shadow, to->byte_count);
        SetRect(&full, 0, 0, (int)to->width, (int)to->height);
        if (!emit_texture_update(destination, level, &full))
            return D3DERR_DRIVERINTERNALERROR;
    }
    return D3D_OK;
}
static HRESULT WINAPI device_get_render_target_data(IDirect3DDevice9 *iface,
        IDirect3DSurface9 *rt_iface, IDirect3DSurface9 *dst_iface)
{
    D9Device *device = device_from_iface(iface);
    D9Surface *rt;
    D9Surface *dst;
    D9TextureLevel *source_level;
    D9TextureLevel *destination_level = NULL;
    BYTE *destination;
    UINT destination_pitch;
    UINT row;

    if (!rt_iface || !dst_iface || rt_iface->lpVtbl != &g_surface_vtbl
            || dst_iface->lpVtbl != &g_surface_vtbl)
        return D3DERR_INVALIDCALL;
    rt = surface_from_iface(rt_iface);
    dst = surface_from_iface(dst_iface);
    if (rt->device != device || dst->device != device
            || rt->width != dst->width || rt->height != dst->height
            || rt->format != dst->format || !rt->texture
            || !(rt->texture->usage & D3DUSAGE_RENDERTARGET))
        return D3DERR_INVALIDCALL;
    source_level = surface_texture_level(rt);
    if (!source_level || !source_level->shadow || !source_level->shadow_valid)
        /* A draw touched this target, so only an asynchronous GPU readback
         * could answer. Returning stale Clear/ColorFill pixels would violate
         * the M4 known-source-only rule. */
        return D3DERR_INVALIDCALL;
    if (dst->shadow) {
        destination = dst->shadow;
        destination_pitch = dst->row_pitch;
    } else {
        destination_level = surface_texture_level(dst);
        if (!destination_level || !destination_level->shadow)
            return D3DERR_INVALIDCALL;
        destination = destination_level->shadow;
        destination_pitch = destination_level->row_pitch;
    }
    for (row = 0; row < rt->height; ++row)
        CopyMemory(destination + row * destination_pitch,
                source_level->shadow + row * source_level->row_pitch,
                rt->width * 4u);
    if (destination_level)
        destination_level->shadow_valid = TRUE;
    return D3D_OK;
}
DEV_STUB(get_front_buffer_data, UINT swapchain, IDirect3DSurface9 *dst)
{ (void)iface; (void)swapchain; (void)dst;
  UNSUPPORTED("Device.GetFrontBufferData"); return D3DERR_INVALIDCALL; }
/* A plain system-memory surface with no GPU resource behind it. Implemented
 * because it is how an application builds a cursor bitmap for
 * SetCursorProperties; it is deliberately limited to the 32-bit formats a
 * cursor may use, so nothing else starts depending on it as a general
 * offscreen surface. */
static HRESULT WINAPI device_create_offscreen_plain_surface(
        IDirect3DDevice9 *iface, UINT width, UINT height, D3DFORMAT format,
        D3DPOOL pool, IDirect3DSurface9 **out, HANDLE *shared)
{
    D9Device *device = device_from_iface(iface);
    D9Surface *surface;
    UINT row_pitch;
    UINT byte_count;

    (void)pool;
    (void)shared;
    if (!out)
        return D3DERR_INVALIDCALL;
    *out = NULL;
    if (format != D3DFMT_A8R8G8B8 && format != D3DFMT_X8R8G8B8) {
        /* The deliberate cursor-format restriction, named for the same reason
         * as UNSUPPORTED(): an app building a CPU-side image in any other
         * format is turned away here and has no other way to find out. */
        TRACE("STUB CreateOffscreenPlainSurface format=%08lX %lux%lu -> "
                "D3DERR_INVALIDCALL (only A8R8G8B8/X8R8G8B8 are supported)",
                (DWORD)format, width, height);
        HOSTLOG_REFUSED("CreateOffscreenPlainSurface %lux%lu format=%08lX "
                "refused; only A8R8G8B8/X8R8G8B8 are supported here",
                width, height, (DWORD)format);
        return D3DERR_INVALIDCALL;
    }
    if (!width || !height)
        return D3DERR_INVALIDCALL;
    if (!multiply_u32(width, 4u, &row_pitch)
            || !multiply_u32(row_pitch, height, &byte_count))
        return D3DERR_INVALIDCALL;

    surface = (D9Surface *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*surface));
    if (!surface)
        return E_OUTOFMEMORY;
    surface->shadow = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            byte_count);
    if (!surface->shadow) {
        HeapFree(GetProcessHeap(), 0, surface);
        return E_OUTOFMEMORY;
    }
    surface->iface.lpVtbl = &g_surface_vtbl;
    surface->refcount = 1;
    surface->device = device;
    surface->width = width;
    surface->height = height;
    surface->format = format;
    surface->row_pitch = row_pitch;
    surface->byte_count = byte_count;
    device_child_add_ref(device);

    *out = &surface->iface;
    TRACE_REGISTER_RANGE("SURFACE_OFFSCREEN", 0, surface, surface,
            sizeof(*surface));
    TRACE_REGISTER_RANGE("SURFACE_SHADOW", 0, surface, surface->shadow,
            surface->byte_count);
    TRACE("SURFACE CREATE kind=offscreen object=%08lX vtbl=%08lX ref=%ld "
            "device=%08lX texture=00000000 swapchain=00000000 shadow=%08lX "
            "level=0 size=%lux%lu format=%08lX bytes=%lu",
            (DWORD)(uintptr_t)*out,
            (DWORD)(uintptr_t)surface->iface.lpVtbl, surface->refcount,
            (DWORD)(uintptr_t)&device->iface,
            (DWORD)(uintptr_t)surface->shadow, surface->width,
            surface->height, (DWORD)surface->format, surface->byte_count);
    return D3D_OK;
}
DEV_STUB(multiply_transform, D3DTRANSFORMSTATETYPE state,
        const D3DMATRIX *matrix)
{ (void)iface; (void)state; (void)matrix;
  UNSUPPORTED("Device.MultiplyTransform"); return D3DERR_INVALIDCALL; }
/*
 * SetMaterial/SetLight/LightEnable were emitted from M1 onwards but only
 * *stored* by the host; M3's fixed-function vertex stage consumes them for
 * real (d3d9_executor.js, lightingSignature/writeFixedVertexUniforms), which
 * is what finally makes the D3DVTXPCAPS_DIRECTIONALLIGHTS/POSITIONALLIGHTS
 * and MaxActiveLights = 8 that fill_caps() advertises true.
 */
static HRESULT WINAPI device_set_material(IDirect3DDevice9 *iface,
        const D3DMATERIAL9 *material)
{
    D9Device *device = device_from_iface(iface);
    D9WGSetMaterial command;
    if (!material)
        return D3DERR_INVALIDCALL;
    state_block_record_material(device);
    device->material = *material;
    command.device_handle = device->handle;
    CopyMemory(command.diffuse, &material->Diffuse, sizeof(command.diffuse));
    CopyMemory(command.ambient, &material->Ambient, sizeof(command.ambient));
    CopyMemory(command.specular, &material->Specular, sizeof(command.specular));
    CopyMemory(command.emissive, &material->Emissive, sizeof(command.emissive));
    command.power = material->Power;
    return emit_command(D9WG_OP_SET_MATERIAL, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_material(IDirect3DDevice9 *iface,
        D3DMATERIAL9 *material)
{
    if (!material)
        return D3DERR_INVALIDCALL;
    *material = device_from_iface(iface)->material;
    return D3D_OK;
}

static HRESULT WINAPI device_set_light(IDirect3DDevice9 *iface, DWORD index,
        const D3DLIGHT9 *light)
{
    D9Device *device = device_from_iface(iface);
    D9WGSetLight command;
    if (!light || index >= D9_MAX_LIGHTS)
        return D3DERR_INVALIDCALL;
    state_block_record_light(device, index);
    device->lights[index] = *light;
    device->light_set[index] = TRUE;
    command.device_handle = device->handle;
    command.index = index;
    command.type = (uint32_t)light->Type;
    CopyMemory(command.diffuse, &light->Diffuse, sizeof(command.diffuse));
    CopyMemory(command.specular, &light->Specular, sizeof(command.specular));
    CopyMemory(command.ambient, &light->Ambient, sizeof(command.ambient));
    CopyMemory(command.position, &light->Position, sizeof(command.position));
    CopyMemory(command.direction, &light->Direction, sizeof(command.direction));
    command.range = light->Range;
    command.falloff = light->Falloff;
    command.attenuation[0] = light->Attenuation0;
    command.attenuation[1] = light->Attenuation1;
    command.attenuation[2] = light->Attenuation2;
    command.theta = light->Theta;
    command.phi = light->Phi;
    return emit_command(D9WG_OP_SET_LIGHT, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_light(IDirect3DDevice9 *iface, DWORD index,
        D3DLIGHT9 *light)
{
    D9Device *device = device_from_iface(iface);
    if (!light || index >= D9_MAX_LIGHTS || !device->light_set[index])
        return D3DERR_INVALIDCALL;
    *light = device->lights[index];
    return D3D_OK;
}

static HRESULT WINAPI device_light_enable(IDirect3DDevice9 *iface,
        DWORD index, WINBOOL enable)
{
    D9Device *device = device_from_iface(iface);
    D9WGLightEnable command;
    if (index >= D9_MAX_LIGHTS)
        return D3DERR_INVALIDCALL;
    state_block_record_light_enable(device, index);
    device->light_enabled[index] = enable ? TRUE : FALSE;
    command.device_handle = device->handle;
    command.index = index;
    command.enable = enable ? 1u : 0u;
    command.reserved = 0;
    return emit_command(D9WG_OP_LIGHT_ENABLE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_light_enable(IDirect3DDevice9 *iface,
        DWORD index, WINBOOL *enable)
{
    D9Device *device = device_from_iface(iface);
    if (!enable || index >= D9_MAX_LIGHTS)
        return D3DERR_INVALIDCALL;
    *enable = device->light_enabled[index];
    return D3D_OK;
}
DEV_STUB(set_clip_plane, DWORD index, const float *plane)
{ (void)iface; (void)index; (void)plane;
  UNSUPPORTED("Device.SetClipPlane/GetClipPlane"); return D3DERR_INVALIDCALL; }
DEV_STUB(get_clip_plane, DWORD index, float *plane)
{ (void)iface; (void)index; (void)plane;
  UNSUPPORTED("Device.SetClipPlane/GetClipPlane"); return D3DERR_INVALIDCALL; }
DEV_STUB(set_clip_status, const D3DCLIPSTATUS9 *status)
{ (void)iface; (void)status; return D3DERR_INVALIDCALL; }
DEV_STUB(get_clip_status, D3DCLIPSTATUS9 *status)
{ (void)iface; (void)status; return D3DERR_INVALIDCALL; }
static HRESULT WINAPI device_get_sampler_state(IDirect3DDevice9 *iface,
        DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value)
{
    D9Device *device = device_from_iface(iface);
    if (!value || sampler >= D9_MAX_SAMPLERS || (UINT)type >= D9_MAX_SAMPLER_STATES)
        return D3DERR_INVALIDCALL;
    *value = device->sampler_states[sampler][type];
    return D3D_OK;
}

/* Drives the host's GPUSampler cache since M2 (plan 4.4/12): the parameter
 * tuple stored here is the cache key, so a stage's filtering follows the app's
 * state rather than the texture that happens to be bound to it. */
static HRESULT WINAPI device_set_sampler_state(IDirect3DDevice9 *iface,
        DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
    D9Device *device = device_from_iface(iface);
    D9WGSetSamplerState command;
    if (sampler >= D9_MAX_SAMPLERS || (UINT)type >= D9_MAX_SAMPLER_STATES)
        return D3DERR_INVALIDCALL;
    state_block_record_sampler_state(device, sampler, (UINT)type);
    if (device->sampler_states[sampler][type] == value)
        return D3D_OK;
    device->sampler_states[sampler][type] = value;
    command.device_handle = device->handle;
    command.sampler = sampler;
    command.state = type;
    command.value = value;
    return emit_command(D9WG_OP_SET_SAMPLER_STATE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}
DEV_STUB(set_palette_entries, UINT index, const PALETTEENTRY *entries)
{ (void)iface; (void)index; (void)entries;
  UNSUPPORTED("Device.SetPaletteEntries/GetPaletteEntries"); return D3DERR_INVALIDCALL; }
DEV_STUB(get_palette_entries, UINT index, PALETTEENTRY *entries)
{ (void)iface; (void)index; (void)entries;
  UNSUPPORTED("Device.SetPaletteEntries/GetPaletteEntries"); return D3DERR_INVALIDCALL; }
DEV_STUB(set_current_texture_palette, UINT index)
{ (void)iface; (void)index; return D3DERR_INVALIDCALL; }
DEV_STUB(get_current_texture_palette, UINT *index)
{ (void)iface; (void)index; return D3DERR_INVALIDCALL; }
DEV_STUB(process_vertices, UINT src_start, UINT dst_index, UINT count,
        IDirect3DVertexBuffer9 *dst, IDirect3DVertexDeclaration9 *decl,
        DWORD flags)
{ (void)iface; (void)src_start; (void)dst_index; (void)count; (void)dst;
  (void)decl; (void)flags;
  UNSUPPORTED("Device.ProcessVertices"); return D3DERR_INVALIDCALL; }
DEV_STUB(set_stream_source_freq, UINT stream, UINT divider)
{ (void)iface; (void)stream; (void)divider;
  UNSUPPORTED("Device.SetStreamSourceFreq/GetStreamSourceFreq"); return D3DERR_INVALIDCALL; }
DEV_STUB(get_stream_source_freq, UINT stream, UINT *divider)
{ (void)iface; (void)stream; (void)divider;
  UNSUPPORTED("Device.SetStreamSourceFreq/GetStreamSourceFreq"); return D3DERR_INVALIDCALL; }
DEV_STUB(draw_rect_patch, UINT handle, const float *segments,
        const D3DRECTPATCH_INFO *info)
{ (void)iface; (void)handle; (void)segments; (void)info;
  UNSUPPORTED("Device.DrawRectPatch/DrawTriPatch"); return D3DERR_INVALIDCALL; }
DEV_STUB(draw_tri_patch, UINT handle, const float *segments,
        const D3DTRIPATCH_INFO *info)
{ (void)iface; (void)handle; (void)segments; (void)info;
  UNSUPPORTED("Device.DrawRectPatch/DrawTriPatch"); return D3DERR_INVALIDCALL; }
DEV_STUB(delete_patch, UINT handle)
{ (void)iface; (void)handle; return D3DERR_INVALIDCALL; }

/* ---- M3: render targets, cube textures, scissor rect, queries ---- */

/*
 * A render target reaches the host as an ordinary CREATE_TEXTURE_2D carrying
 * D3DUSAGE_RENDERTARGET (or D3DUSAGE_DEPTHSTENCIL), not as its own resource
 * kind. That is not a shortcut: in D3D9 a render target *is* a surface of a
 * texture as often as it is a standalone surface, and the host needs a GPU
 * texture either way, so giving the standalone form its own opcode would only
 * mean two host code paths building the same object.
 *
 * CreateRenderTarget therefore builds a private D9Texture nobody else can see
 * and hands back its level-0 surface, then drops its own reference so the
 * surface's reference is the only one keeping it alive -- releasing the surface
 * releases the texture, which is exactly the app-visible lifetime D3D9 gives a
 * standalone render target.
 */
static HRESULT create_target_texture(D9Device *device, UINT width, UINT height,
        D3DFORMAT format, DWORD usage, IDirect3DSurface9 **surface_out)
{
    D9Texture *texture;
    D9Surface *surface;

    if (!surface_out)
        return D3DERR_INVALIDCALL;
    *surface_out = NULL;
    if (!width || !height || width > 4096 || height > 4096)
        return D3DERR_INVALIDCALL;

    texture = (D9Texture *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*texture));
    if (!texture)
        return E_OUTOFMEMORY;
    texture->levels = (D9TextureLevel *)HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, sizeof(*texture->levels));
    if (!texture->levels) {
        HeapFree(GetProcessHeap(), 0, texture);
        return E_OUTOFMEMORY;
    }
    texture->iface.lpVtbl = &g_texture_vtbl;
    texture->refcount = 1;
    texture->device = device;
    texture->handle = allocate_handle();
    texture->width = width;
    texture->height = height;
    texture->level_count = 1;
    texture->usage = usage;
    texture->format = format;
    texture->pool = D3DPOOL_DEFAULT;
    texture->levels[0].width = width;
    texture->levels[0].height = height;
    if (!texture_level_layout(format, width, height,
            &texture->levels[0].row_pitch, &texture->levels[0].row_count,
            &texture->levels[0].byte_count)) {
        HeapFree(GetProcessHeap(), 0, texture->levels);
        HeapFree(GetProcessHeap(), 0, texture);
        return D3DERR_INVALIDCALL;
    }
    /* Shadow storage remains lazy. Clear/ColorFill may make its contents fully
     * known for M4 readback; a GPU draw invalidates that knowledge. */
    device_child_add_ref(device);
    if (!emit_texture_create(device, texture)) {
        device_child_release(device);
        HeapFree(GetProcessHeap(), 0, texture->levels);
        HeapFree(GetProcessHeap(), 0, texture);
        return D3DERR_DRIVERINTERNALERROR;
    }
    texture->next_device_resource = device->texture_resources;
    device->texture_resources = texture;

    surface = (D9Surface *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*surface));
    if (!surface) {
        IDirect3DTexture9_Release(&texture->iface);
        return E_OUTOFMEMORY;
    }
    surface->iface.lpVtbl = &g_surface_vtbl;
    surface->refcount = 1;
    surface->device = device;
    surface->texture = texture;
    surface->level = 0;
    surface->width = width;
    surface->height = height;
    surface->format = format;
    device_child_add_ref(device);
    /* The surface took over the creation reference rather than adding a second
     * one, so the texture disappears with the surface. */
    *surface_out = &surface->iface;
    TRACE_REGISTER_RANGE("SURFACE_TARGET", texture->handle, surface, surface,
            sizeof(*surface));
    TRACE("SURFACE CREATE kind=target object=%08lX vtbl=%08lX ref=%ld "
            "device=%08lX texture=%08lX texture_handle=%08lX "
            "swapchain=00000000 shadow=00000000 level=0 size=%lux%lu "
            "format=%08lX usage=%08lX",
            (DWORD)(uintptr_t)*surface_out,
            (DWORD)(uintptr_t)surface->iface.lpVtbl, surface->refcount,
            (DWORD)(uintptr_t)&device->iface,
            (DWORD)(uintptr_t)&texture->iface, texture->handle,
            surface->width, surface->height, (DWORD)surface->format, usage);
    return D3D_OK;
}

static HRESULT WINAPI device_create_render_target(IDirect3DDevice9 *iface,
        UINT width, UINT height, D3DFORMAT format,
        D3DMULTISAMPLE_TYPE multisample, DWORD multisample_quality,
        WINBOOL lockable, IDirect3DSurface9 **surface_out, HANDLE *shared)
{
    (void)shared;
    (void)lockable;
    if (multisample != D3DMULTISAMPLE_NONE || multisample_quality) {

        return D3DERR_INVALIDCALL;
    }
    if (!supported_render_target_format(format))
        return D3DERR_INVALIDCALL;
    return create_target_texture(device_from_iface(iface), width, height, format,
            D3DUSAGE_RENDERTARGET, surface_out);
}

static HRESULT WINAPI device_create_depth_stencil_surface(
        IDirect3DDevice9 *iface, UINT width, UINT height, D3DFORMAT format,
        D3DMULTISAMPLE_TYPE multisample, DWORD multisample_quality,
        WINBOOL discard, IDirect3DSurface9 **surface_out, HANDLE *shared)
{
    (void)shared;
    (void)discard;
    if (multisample != D3DMULTISAMPLE_NONE || multisample_quality)
        return D3DERR_INVALIDCALL;
    if (!supported_depth_stencil_format(format))
        return D3DERR_INVALIDCALL;
    return create_target_texture(device_from_iface(iface), width, height, format,
            D3DUSAGE_DEPTHSTENCIL, surface_out);
}

/* Returns the surface's backing texture handle plus the level within it, or
 * FALSE for a surface that names no host resource (the back buffer, or a
 * standalone CPU surface). Handle 0 is how the wire says "the back buffer",
 * which is why the caller has to distinguish "no handle" from "not resolvable". */
static BOOL surface_target_handle(D9Surface *surface, uint32_t *handle_out,
        uint32_t *level_out)
{
    if (!surface)
        return FALSE;
    if (surface->shadow)
        return FALSE; /* CPU-only offscreen surface: never a GPU target */
    *handle_out = surface->texture ? surface->texture->handle : 0u;
    *level_out = surface->texture ? surface->level : 0u;
    return TRUE;
}

static HRESULT WINAPI device_set_render_target(IDirect3DDevice9 *iface,
        DWORD index, IDirect3DSurface9 *surface_iface)
{
    D9Device *device = device_from_iface(iface);
    D9Surface *surface = surface_iface ? surface_from_iface(surface_iface) : NULL;
    D9Surface *old_surface;
    D9WGSetRenderTarget command;
    uint32_t handle = 0;
    uint32_t level = 0;

    if (index >= D9_MAX_RENDER_TARGETS)
        return D3DERR_INVALIDCALL;
    /* D3D9 forbids unbinding slot 0: something always has to receive colour. */
    if (!surface_iface && index == 0)
        return D3DERR_INVALIDCALL;
    if (surface_iface && !surface_target_handle(surface, &handle, &level))
        return D3DERR_INVALIDCALL;
    if (surface_iface && surface->texture
            && !(surface->texture->usage & D3DUSAGE_RENDERTARGET))
        return D3DERR_INVALIDCALL;

    old_surface = device->render_target_surfaces[index];
    TRACE("CALL SetRenderTarget index=%lu old=%08lX new=%08lX same=%lu "
            "old_refs=%ld new_refs=%ld", index,
            (DWORD)(uintptr_t)old_surface, (DWORD)(uintptr_t)surface,
            old_surface == surface,
            old_surface ? InterlockedCompareExchange(&old_surface->refcount,
                    0, 0) : 0,
            surface ? InterlockedCompareExchange(&surface->refcount, 0, 0)
                    : 0);
    /* Take the new reference before dropping the old one.  Apart from being
     * the usual COM replacement discipline, this keeps same-object rebinding
     * safe even when the caller is holding only a borrowed alias. */
    if (surface)
        IDirect3DSurface9_AddRef(surface_iface);
    device->render_target_surfaces[index] = surface;
    if (old_surface)
        IDirect3DSurface9_Release(&old_surface->iface);

    command.device_handle = device->handle;
    command.target_index = index;
    command.color_texture_handle = handle;
    command.color_level = level;
    if (!emit_command(D9WG_OP_SET_RENDER_TARGET, &command, sizeof(command)))
        return D3DERR_DRIVERINTERNALERROR;

    /* D3D9 resets the viewport to the full new target on every successful
     * SetRenderTarget(0). Leaving the old viewport in place is a classic
     * source of "the render-to-texture pass only fills a corner". */
    if (index == 0 && surface) {
        D3DVIEWPORT9 viewport;
        viewport.X = 0;
        viewport.Y = 0;
        viewport.Width = surface->width;
        viewport.Height = surface->height;
        viewport.MinZ = 0.0f;
        viewport.MaxZ = 1.0f;
        return IDirect3DDevice9_SetViewport(iface, &viewport);
    }
    return D3D_OK;
}

static HRESULT WINAPI device_get_render_target(IDirect3DDevice9 *iface,
        DWORD index, IDirect3DSurface9 **surface_out)
{
    D9Device *device = device_from_iface(iface);

    if (!surface_out || index >= D9_MAX_RENDER_TARGETS)
        return D3DERR_INVALIDCALL;
    *surface_out = NULL;
    if (device->render_target_surfaces[index]) {
        *surface_out = &device->render_target_surfaces[index]->iface;
        IDirect3DSurface9_AddRef(*surface_out);
        return D3D_OK;
    }
    /* Nothing explicitly bound: slot 0 is the implicit back buffer, and the
     * other slots are genuinely empty (D3DERR_NOTFOUND, as D3D9 reports). */
    if (index != 0)
        return D3DERR_NOTFOUND;
    return IDirect3DDevice9_GetBackBuffer(iface, 0, 0,
            D3DBACKBUFFER_TYPE_MONO, surface_out);
}

static HRESULT WINAPI device_set_depth_stencil_surface(IDirect3DDevice9 *iface,
        IDirect3DSurface9 *surface_iface)
{
    D9Device *device = device_from_iface(iface);
    D9Surface *surface = surface_iface ? surface_from_iface(surface_iface) : NULL;
    D9Surface *old_surface;
    D9WGSetDepthStencilSurface command;
    uint32_t handle = 0;
    uint32_t level = 0;

    if (surface && surface->auto_depth_stencil) {
        handle = D9WG_AUTO_DEPTH_STENCIL_HANDLE;
    } else if (surface_iface) {
        if (!surface_target_handle(surface, &handle, &level) || !surface->texture
                || !(surface->texture->usage & D3DUSAGE_DEPTHSTENCIL))
            return D3DERR_INVALIDCALL;
    }
    old_surface = device->depth_stencil_surface;
    TRACE("CALL SetDepthStencilSurface old=%08lX new=%08lX same=%lu "
            "old_refs=%ld new_refs=%ld", (DWORD)(uintptr_t)old_surface,
            (DWORD)(uintptr_t)surface, old_surface == surface,
            old_surface ? InterlockedCompareExchange(&old_surface->refcount,
                    0, 0) : 0,
            surface ? InterlockedCompareExchange(&surface->refcount, 0, 0)
                    : 0);
    if (surface)
        IDirect3DSurface9_AddRef(surface_iface);
    device->depth_stencil_surface = surface;
    if (old_surface)
        IDirect3DSurface9_Release(&old_surface->iface);
    device->depth_stencil_unbound = surface_iface ? FALSE : TRUE;

    command.device_handle = device->handle;
    command.depth_texture_handle = handle;
    command.width = surface ? surface->width : 0u;
    command.height = surface ? surface->height : 0u;
    device->depth_stencil_unbound = surface ? FALSE : TRUE;
    return emit_command(D9WG_OP_SET_DEPTH_STENCIL_SURFACE, &command,
            sizeof(command)) ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_depth_stencil_surface(IDirect3DDevice9 *iface,
        IDirect3DSurface9 **surface_out)
{
    D9Device *device = device_from_iface(iface);

    if (!surface_out)
        return D3DERR_INVALIDCALL;
    *surface_out = NULL;
    if (device->depth_stencil_surface) {
        *surface_out = &device->depth_stencil_surface->iface;
        IDirect3DSurface9_AddRef(*surface_out);
        return D3D_OK;
    }
    if (device->depth_stencil_unbound || !device->present.EnableAutoDepthStencil)
        return D3DERR_NOTFOUND;
    /* A handle onto the implicit auto depth-stencil. Refusing here used to look
     * harmless -- nothing reads a depth surface's pixels -- but it breaks the
     * standard render-to-texture sequence: an app that cannot *get* the current
     * depth surface cannot put it back afterwards, and then every draw after the
     * first RTT pass runs with no depth buffer. */
    {
        /* Cached for the device's lifetime, for the same reason as the back
         * buffer above: this pointer is what the app hands back to
         * SetDepthStencilSurface, possibly long after releasing it. */
        D9Surface *surface = device->implicit_depth_stencil;
        if (!surface) {
            surface = (D9Surface *)HeapAlloc(GetProcessHeap(),
                    HEAP_ZERO_MEMORY, sizeof(*surface));
            if (!surface)
                return E_OUTOFMEMORY;
            surface->iface.lpVtbl = &g_surface_vtbl;
            surface->device = device;
            surface->auto_depth_stencil = TRUE;
            surface->width = device->present.BackBufferWidth;
            surface->height = device->present.BackBufferHeight;
            surface->format = device->present.AutoDepthStencilFormat;
            device->implicit_depth_stencil = surface;
            TRACE_REGISTER_RANGE("SURFACE_AUTO_DEPTH", 0, surface, surface,
                    sizeof(*surface));
            TRACE("SURFACE CREATE kind=auto_depth object=%08lX vtbl=%08lX "
                    "ref=%ld device=%08lX texture=00000000 swapchain=00000000 "
                    "shadow=00000000 level=0 size=%lux%lu format=%08lX",
                    (DWORD)(uintptr_t)&surface->iface,
                    (DWORD)(uintptr_t)surface->iface.lpVtbl, surface->refcount,
                    (DWORD)(uintptr_t)&device->iface, surface->width,
                    surface->height, (DWORD)surface->format);
        }
        *surface_out = &surface->iface;
        IDirect3DSurface9_AddRef(*surface_out);
    }
    return D3D_OK;
}

static void normalize_rect(const RECT *source, int width, int height, RECT *out)
{
    if (source) {
        *out = *source;
    } else {
        SetRect(out, 0, 0, width, height);
    }
}

static HRESULT WINAPI device_stretch_rect(IDirect3DDevice9 *iface,
        IDirect3DSurface9 *source_iface, const RECT *source_rect,
        IDirect3DSurface9 *destination_iface, const RECT *destination_rect,
        D3DTEXTUREFILTERTYPE filter)
{
    D9Device *device = device_from_iface(iface);
    D9Surface *source = source_iface ? surface_from_iface(source_iface) : NULL;
    D9Surface *destination = destination_iface
            ? surface_from_iface(destination_iface) : NULL;
    D9WGStretchRect command;
    RECT source_area;
    RECT destination_area;

    if (!source || !destination)
        return D3DERR_INVALIDCALL;
    ZeroMemory(&command, sizeof(command));
    if (!surface_target_handle(source, &command.source_texture_handle,
                &command.source_level)
            || !surface_target_handle(destination,
                &command.destination_texture_handle,
                &command.destination_level))
        return D3DERR_INVALIDCALL;
    command.device_handle = device->handle;
    normalize_rect(source_rect, (int)source->width, (int)source->height,
            &source_area);
    command.source_left = source_area.left;
    command.source_top = source_area.top;
    command.source_right = source_area.right;
    command.source_bottom = source_area.bottom;
    normalize_rect(destination_rect, (int)destination->width,
            (int)destination->height, &destination_area);
    command.destination_left = destination_area.left;
    command.destination_top = destination_area.top;
    command.destination_right = destination_area.right;
    command.destination_bottom = destination_area.bottom;
    command.filter_point = (filter == D3DTEXF_NONE || filter == D3DTEXF_POINT)
            ? 1u : 0u;
    if (!emit_command(D9WG_OP_STRETCH_RECT, &command, sizeof(command)))
        return D3DERR_DRIVERINTERNALERROR;
    mirror_target_copy(source, &source_area, destination, &destination_area);
    return D3D_OK;
}

static HRESULT WINAPI device_color_fill(IDirect3DDevice9 *iface,
        IDirect3DSurface9 *surface_iface, const RECT *rect, D3DCOLOR color)
{
    D9Device *device = device_from_iface(iface);
    D9Surface *surface = surface_iface ? surface_from_iface(surface_iface) : NULL;
    D9WGColorFill command;
    RECT area;

    if (!surface)
        return D3DERR_INVALIDCALL;
    ZeroMemory(&command, sizeof(command));
    if (!surface_target_handle(surface, &command.texture_handle, &command.level))
        return D3DERR_INVALIDCALL;
    command.device_handle = device->handle;
    command.color = color;
    normalize_rect(rect, (int)surface->width, (int)surface->height, &area);
    command.left = area.left;
    command.top = area.top;
    command.right = area.right;
    command.bottom = area.bottom;
    if (!emit_command(D9WG_OP_COLOR_FILL, &command, sizeof(command)))
        return D3DERR_DRIVERINTERNALERROR;
    mirror_target_color_fill(surface, &area, color);
    return D3D_OK;
}

static HRESULT WINAPI device_set_scissor_rect(IDirect3DDevice9 *iface,
        const RECT *rect)
{
    D9Device *device = device_from_iface(iface);
    D9WGSetScissorRect command;

    if (!rect)
        return D3DERR_INVALIDCALL;
    if (rect->left < 0 || rect->top < 0 || rect->right < rect->left
            || rect->bottom < rect->top)
        return D3DERR_INVALIDCALL;
    state_block_record_scissor(device);
    device->scissor_rect = *rect;
    device->scissor_set = TRUE;
    command.device_handle = device->handle;
    command.left = rect->left;
    command.top = rect->top;
    command.right = rect->right;
    command.bottom = rect->bottom;
    return emit_command(D9WG_OP_SET_SCISSOR_RECT, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_scissor_rect(IDirect3DDevice9 *iface,
        RECT *rect)
{
    D9Device *device = device_from_iface(iface);

    if (!rect)
        return D3DERR_INVALIDCALL;
    if (device->scissor_set) {
        *rect = device->scissor_rect;
    } else {
        /* D3D9's default scissor rect is the whole render target. */
        SetRect(rect, 0, 0, (int)device->present.BackBufferWidth,
                (int)device->present.BackBufferHeight);
    }
    return D3D_OK;
}

/* ---- IDirect3DQuery9 ----
 *
 * Deliberately answered entirely inside the guest, with a *conservative* result
 * rather than a real GPU measurement.
 *
 * The host could count samples, but reporting the answer back needs the
 * host->guest return channel plan 6.7 describes and nothing else needs yet.
 * The two remaining options are both worse than a conservative answer:
 *
 *   - Failing CreateQuery makes an engine disable whatever branch it gates on
 *     occlusion queries existing, which can be far more than culling.
 *   - Returning S_FALSE forever deadlocks the extremely common
 *     `while (GetData(...) == S_FALSE);` polling loop.
 *
 * So an EVENT query reports "the GPU finished" (true by the time the guest
 * looks: the batch it belongs to has already been handed over) and an OCCLUSION
 * query reports "every sample passed". Over-reporting visibility can only cost
 * frame time -- the app draws something it could have skipped -- while
 * under-reporting would delete visible geometry.
 */
typedef struct D9Query {
    IDirect3DQuery9 iface;
    LONG refcount;
    D9Device *device;
    D3DQUERYTYPE type;
    BOOL issued;
    struct D9Query *next_device_resource;
} D9Query;

static IDirect3DQuery9Vtbl g_query_vtbl;

static D9Query *query_from_iface(IDirect3DQuery9 *iface)
{
    return (D9Query *)iface;
}

static HRESULT WINAPI query_query_interface(IDirect3DQuery9 *iface, REFIID iid,
        void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid) && !guid_equal(iid, &IID_IDirect3DQuery9)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DQuery9_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI query_add_ref(IDirect3DQuery9 *iface)
{
    return (ULONG)InterlockedIncrement(&query_from_iface(iface)->refcount);
}

static ULONG WINAPI query_release(IDirect3DQuery9 *iface)
{
    D9Query *query = query_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&query->refcount);
    if (!refs) {
        D9Query **link = &query->device->queries;
        while (*link && *link != query)
            link = &(*link)->next_device_resource;
        if (*link) *link = query->next_device_resource;
        device_child_release(query->device);
        HeapFree(GetProcessHeap(), 0, query);
    }
    return refs;
}

static HRESULT WINAPI query_get_device(IDirect3DQuery9 *iface,
        IDirect3DDevice9 **device_out)
{
    D9Query *query = query_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &query->device->iface;
    IDirect3DDevice9_AddRef(*device_out);
    return D3D_OK;
}

static D3DQUERYTYPE WINAPI query_get_type(IDirect3DQuery9 *iface)
{
    return query_from_iface(iface)->type;
}

static DWORD WINAPI query_get_data_size(IDirect3DQuery9 *iface)
{
    return query_from_iface(iface)->type == D3DQUERYTYPE_OCCLUSION
            ? (DWORD)sizeof(DWORD) : 0u;
}

static HRESULT WINAPI query_issue(IDirect3DQuery9 *iface, DWORD flags)
{
    D9Query *query = query_from_iface(iface);
    if (flags & D3DISSUE_BEGIN) {
        if (query->type == D3DQUERYTYPE_EVENT)
            return D3DERR_INVALIDCALL;
        query->issued = FALSE;
    }
    if (flags & D3DISSUE_END)
        query->issued = TRUE;
    return D3D_OK;
}

static HRESULT WINAPI query_get_data(IDirect3DQuery9 *iface, void *data,
        DWORD size, DWORD flags)
{
    D9Query *query = query_from_iface(iface);
    (void)flags;
    if (!query->issued)
        return D3DERR_INVALIDCALL;
    if (query->type == D3DQUERYTYPE_OCCLUSION) {
        if (data) {
            if (size < sizeof(DWORD))
                return D3DERR_INVALIDCALL;

            *(DWORD *)data = query->device->present.BackBufferWidth
                    * query->device->present.BackBufferHeight;
        }
        return S_OK;
    }
    /* EVENT: the batch this query was issued in has already been submitted. */
    if (data) {
        if (size < sizeof(WINBOOL))
            return D3DERR_INVALIDCALL;
        *(WINBOOL *)data = TRUE;
    }
    return S_OK;
}

static HRESULT WINAPI device_create_query(IDirect3DDevice9 *iface,
        D3DQUERYTYPE type, IDirect3DQuery9 **query_out)
{
    D9Device *device = device_from_iface(iface);
    D9Query *query;

    if (type != D3DQUERYTYPE_OCCLUSION && type != D3DQUERYTYPE_EVENT) {

        if (query_out) *query_out = NULL;
        return D3DERR_NOTAVAILABLE;
    }
    /* A NULL out pointer is D3D9's "do you support this type?" probe. */
    if (!query_out)
        return D3D_OK;
    *query_out = NULL;
    query = (D9Query *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*query));
    if (!query)
        return E_OUTOFMEMORY;
    query->iface.lpVtbl = &g_query_vtbl;
    query->refcount = 1;
    query->device = device;
    query->type = type;
    device_child_add_ref(device);
    query->next_device_resource = device->queries;
    device->queries = query;
    *query_out = &query->iface;
    return D3D_OK;
}

static IDirect3DQuery9Vtbl g_query_vtbl = {
    .QueryInterface = query_query_interface,
    .AddRef = query_add_ref,
    .Release = query_release,
    .GetDevice = query_get_device,
    .GetType = query_get_type,
    .GetDataSize = query_get_data_size,
    .Issue = query_issue,
    .GetData = query_get_data
};

/* ---- IDirect3DCubeTexture9 ----
 *
 * Six square faces per mip level, stored as one flat level array indexed
 * face * level_count + level. On the wire a cube texture is a
 * CREATE_TEXTURE_CUBE plus ordinary UPDATE_TEXTURE commands whose `z` names the
 * face, which is the same field a volume texture uses for its slice -- in both
 * cases it selects the layer the upload lands in, and the host's WebGPU texture
 * is layered either way.
 */
static D9CubeTexture *cube_from_iface(IDirect3DCubeTexture9 *iface)
{
    return (D9CubeTexture *)iface;
}

static BOOL emit_cube_texture_create(D9Device *device, D9CubeTexture *texture)
{
    D9WGCreateTextureCube command;
    command.device_handle = device->handle;
    command.resource_handle = texture->handle;
    command.edge_length = texture->edge_length;
    command.level_count = texture->level_count;
    command.format = texture->format;
    command.usage = texture->usage;
    command.pool = texture->pool;
    command.reserved = 0;
    return emit_command(D9WG_OP_CREATE_TEXTURE_CUBE, &command, sizeof(command));
}

/* Shares the 2D upload path's payload shape; only the resource handle and the
 * face index differ, so a bug in one cannot silently diverge from the other. */
static BOOL emit_cube_texture_update(D9CubeTexture *texture, UINT face,
        UINT level, const RECT *rect)
{
    D9TextureLevel *level_data = &texture->levels[face * texture->level_count + level];
    D9WGUpdateTexture command;
    UINT block_width, block_height, block_bytes;
    UINT block_x, block_y, block_columns, block_rows;
    uint8_t *payload;
    uint8_t *blob;
    uint32_t data_bytes;
    BOOL result;

    if (!texture_format_layout(texture->format, &block_width, &block_height,
            &block_bytes))
        return FALSE;
    block_x = (UINT)rect->left / block_width;
    block_y = (UINT)rect->top / block_height;
    block_columns = ((UINT)rect->right - (UINT)rect->left + block_width - 1)
            / block_width;
    block_rows = ((UINT)rect->bottom - (UINT)rect->top + block_height - 1)
            / block_height;
    if (!block_columns || !block_rows)
        return TRUE;
    data_bytes = block_rows * block_columns * block_bytes;

    ZeroMemory(&command, sizeof(command));
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D9WG_OP_UPDATE_TEXTURE, sizeof(command),
            data_bytes, NULL, &payload, &blob);
    if (result) {
        UINT row;
        command.resource_handle = texture->handle;
        command.level = level;
        command.x = (UINT)rect->left;
        command.y = (UINT)rect->top;
        command.z = face;
        command.width = (UINT)rect->right - (UINT)rect->left;
        command.height = (UINT)rect->bottom - (UINT)rect->top;
        command.depth = 1;
        command.row_pitch = block_columns * block_bytes;
        command.slice_pitch = 0;
        command.data_bytes = data_bytes;
        command.data_offset = (uint32_t)(blob - batch_base());
        CopyMemory(payload, &command, sizeof(command));
        for (row = 0; row < block_rows; ++row) {
            CopyMemory(blob + row * block_columns * block_bytes,
                    level_data->shadow + (block_y + row) * level_data->row_pitch
                        + block_x * block_bytes,
                    block_columns * block_bytes);
        }
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

static HRESULT WINAPI device_create_cube_texture(IDirect3DDevice9 *iface,
        UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
        IDirect3DCubeTexture9 **texture_out, HANDLE *shared)
{
    D9Device *device = device_from_iface(iface);
    D9CubeTexture *texture;
    UINT full_levels;
    UINT face;
    UINT level;
    UINT total;
    HRESULT failure = E_OUTOFMEMORY;
    (void)shared;

    if (!texture_out)
        return D3DERR_INVALIDCALL;
    *texture_out = NULL;
    if (!edge || edge > 4096 || !supported_texture_format(format)
            || (usage & (D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_RENDERTARGET
                    | D3DUSAGE_AUTOGENMIPMAP))
            || pool > D3DPOOL_SCRATCH) {

        return D3DERR_INVALIDCALL;
    }
    full_levels = full_mip_level_count(edge, edge);
    if (!levels) levels = full_levels;
    if (levels > full_levels)
        return D3DERR_INVALIDCALL;
    if (!multiply_u32(levels, 6u, &total))
        return D3DERR_INVALIDCALL;

    texture = (D9CubeTexture *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*texture));
    if (!texture)
        return E_OUTOFMEMORY;
    texture->levels = (D9TextureLevel *)HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, total * sizeof(*texture->levels));
    if (!texture->levels) {
        HeapFree(GetProcessHeap(), 0, texture);
        return E_OUTOFMEMORY;
    }
    texture->iface.lpVtbl = &g_cube_vtbl;
    texture->refcount = 1;
    texture->device = device;
    texture->handle = allocate_handle();
    texture->edge_length = edge;
    texture->level_count = levels;
    texture->usage = usage;
    texture->format = format;
    texture->pool = pool;
    device_child_add_ref(device);

    for (face = 0; face < 6; ++face) {
        UINT size = edge;
        for (level = 0; level < levels; ++level) {
            D9TextureLevel *level_data = &texture->levels[face * levels + level];
            level_data->width = size;
            level_data->height = size;
            if (!texture_level_layout(format, size, size,
                    &level_data->row_pitch, &level_data->row_count,
                    &level_data->byte_count))
                goto allocation_failed;
            level_data->shadow = (BYTE *)HeapAlloc(GetProcessHeap(),
                    HEAP_ZERO_MEMORY, level_data->byte_count);
            if (!level_data->shadow)
                goto allocation_failed;
            if (size > 1) size >>= 1;
        }
    }
    if (!emit_cube_texture_create(device, texture)) {
        failure = D3DERR_DRIVERINTERNALERROR;
        goto allocation_failed;
    }
    texture->next_device_resource = device->cube_textures;
    device->cube_textures = texture;
    *texture_out = &texture->iface;
    return D3D_OK;

allocation_failed:
    for (face = 0; face < total; ++face) {
        if (texture->levels[face].shadow)
            HeapFree(GetProcessHeap(), 0, texture->levels[face].shadow);
    }
    device_child_release(device);
    HeapFree(GetProcessHeap(), 0, texture->levels);
    HeapFree(GetProcessHeap(), 0, texture);
    return failure;
}

static HRESULT WINAPI cube_query_interface(IDirect3DCubeTexture9 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DResource9)
            && !guid_equal(iid, &IID_IDirect3DBaseTexture9)
            && !guid_equal(iid, &IID_IDirect3DCubeTexture9)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DCubeTexture9_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI cube_add_ref(IDirect3DCubeTexture9 *iface)
{
    return (ULONG)InterlockedIncrement(&cube_from_iface(iface)->refcount);
}

static ULONG WINAPI cube_release(IDirect3DCubeTexture9 *iface)
{
    D9CubeTexture *texture = cube_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&texture->refcount);
    if (!refs) {
        D9CubeTexture **link = &texture->device->cube_textures;
        D9WGDestroyResource destroy;
        UINT index;
        while (*link && *link != texture)
            link = &(*link)->next_device_resource;
        if (*link) *link = texture->next_device_resource;
        destroy.resource_handle = texture->handle;
        destroy.resource_kind = D9WG_RESOURCE_TEXTURE_CUBE;
        emit_command(D9WG_OP_DESTROY_RESOURCE, &destroy, sizeof(destroy));
        for (index = 0; index < texture->level_count * 6u; ++index)
            HeapFree(GetProcessHeap(), 0, texture->levels[index].shadow);
        HeapFree(GetProcessHeap(), 0, texture->levels);
        device_child_release(texture->device);
        HeapFree(GetProcessHeap(), 0, texture);
    }
    return refs;
}

static HRESULT WINAPI cube_get_device(IDirect3DCubeTexture9 *iface,
        IDirect3DDevice9 **device_out)
{
    D9CubeTexture *texture = cube_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &texture->device->iface;
    IDirect3DDevice9_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI cube_set_private_data(IDirect3DCubeTexture9 *iface,
        REFGUID guid, const void *data, DWORD size, DWORD flags)
{ (void)iface; (void)guid; (void)data; (void)size; (void)flags;
  return D3DERR_INVALIDCALL; }
static HRESULT WINAPI cube_get_private_data(IDirect3DCubeTexture9 *iface,
        REFGUID guid, void *data, DWORD *size)
{ (void)iface; (void)guid; (void)data; (void)size; return D3DERR_NOTFOUND; }
static HRESULT WINAPI cube_free_private_data(IDirect3DCubeTexture9 *iface,
        REFGUID guid)
{ (void)iface; (void)guid; return D3DERR_NOTFOUND; }

static DWORD WINAPI cube_set_priority(IDirect3DCubeTexture9 *iface,
        DWORD priority)
{
    D9CubeTexture *texture = cube_from_iface(iface);
    DWORD previous = texture->priority;
    texture->priority = priority;
    return previous;
}

static DWORD WINAPI cube_get_priority(IDirect3DCubeTexture9 *iface)
{ return cube_from_iface(iface)->priority; }
static void WINAPI cube_preload(IDirect3DCubeTexture9 *iface) { (void)iface; }
static D3DRESOURCETYPE WINAPI cube_get_type(IDirect3DCubeTexture9 *iface)
{ (void)iface; return D3DRTYPE_CUBETEXTURE; }

static DWORD WINAPI cube_set_lod(IDirect3DCubeTexture9 *iface, DWORD lod)
{
    D9CubeTexture *texture = cube_from_iface(iface);
    DWORD previous = texture->lod;
    if (texture->pool == D3DPOOL_MANAGED) texture->lod = lod;
    return previous;
}

static DWORD WINAPI cube_get_lod(IDirect3DCubeTexture9 *iface)
{ return cube_from_iface(iface)->lod; }
static DWORD WINAPI cube_get_level_count(IDirect3DCubeTexture9 *iface)
{ return cube_from_iface(iface)->level_count; }
static HRESULT WINAPI cube_set_auto_gen_filter_type(
        IDirect3DCubeTexture9 *iface, D3DTEXTUREFILTERTYPE type)
{ (void)iface; (void)type; return D3DERR_INVALIDCALL; }
static D3DTEXTUREFILTERTYPE WINAPI cube_get_auto_gen_filter_type(
        IDirect3DCubeTexture9 *iface)
{ (void)iface; return D3DTEXF_LINEAR; }
static void WINAPI cube_generate_mip_sublevels(IDirect3DCubeTexture9 *iface)
{ (void)iface; }

static HRESULT WINAPI cube_get_level_desc(IDirect3DCubeTexture9 *iface,
        UINT level, D3DSURFACE_DESC *desc)
{
    D9CubeTexture *texture = cube_from_iface(iface);
    if (!desc || level >= texture->level_count)
        return D3DERR_INVALIDCALL;
    ZeroMemory(desc, sizeof(*desc));
    desc->Format = texture->format;
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = texture->usage;
    desc->Pool = texture->pool;
    desc->MultiSampleType = D3DMULTISAMPLE_NONE;
    desc->Width = texture->levels[level].width;
    desc->Height = texture->levels[level].height;
    return D3D_OK;
}

static HRESULT WINAPI cube_get_cube_map_surface(IDirect3DCubeTexture9 *iface,
        D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **surface_out)
{
    /* A per-face surface would need a D9Surface variant that knows its face, and
     * the only thing an app does with one is Lock it -- which LockRect already
     * covers directly. Refusing honestly beats handing back a surface whose
     * LockRect would write to face 0. */

    (void)iface; (void)face; (void)level;
    if (surface_out) *surface_out = NULL;
    UNSUPPORTED("CubeTexture.GetCubeMapSurface");
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI cube_lock_rect(IDirect3DCubeTexture9 *iface,
        D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked_rect,
        const RECT *rect, DWORD flags)
{
    D9CubeTexture *texture = cube_from_iface(iface);
    D9TextureLevel *level_data;
    RECT area;
    UINT block_width, block_height, block_bytes;

    if (!locked_rect || (UINT)face >= 6u || level >= texture->level_count)
        return D3DERR_INVALIDCALL;
    level_data = &texture->levels[(UINT)face * texture->level_count + level];
    if (level_data->locked)
        return D3DERR_INVALIDCALL;
    if (rect) {
        area = *rect;
    } else {
        SetRect(&area, 0, 0, (int)level_data->width, (int)level_data->height);
    }
    if (area.left < 0 || area.top < 0 || area.right <= area.left
            || area.bottom <= area.top
            || (UINT)area.right > level_data->width
            || (UINT)area.bottom > level_data->height
            || !texture_format_layout(texture->format, &block_width,
                    &block_height, &block_bytes))
        return D3DERR_INVALIDCALL;
    level_data->lock_rect = area;
    level_data->lock_flags = flags;
    level_data->locked = TRUE;
    locked_rect->Pitch = (INT)level_data->row_pitch;
    locked_rect->pBits = level_data->shadow
            + ((UINT)area.top / block_height) * level_data->row_pitch
            + ((UINT)area.left / block_width) * block_bytes;
    return D3D_OK;
}

static HRESULT WINAPI cube_unlock_rect(IDirect3DCubeTexture9 *iface,
        D3DCUBEMAP_FACES face, UINT level)
{
    D9CubeTexture *texture = cube_from_iface(iface);
    D9TextureLevel *level_data;
    BOOL result = TRUE;

    if ((UINT)face >= 6u || level >= texture->level_count)
        return D3DERR_INVALIDCALL;
    level_data = &texture->levels[(UINT)face * texture->level_count + level];
    if (!level_data->locked)
        return D3DERR_INVALIDCALL;
    if (!(level_data->lock_flags & D3DLOCK_READONLY))
        result = emit_cube_texture_update(texture, (UINT)face, level,
                &level_data->lock_rect);
    level_data->locked = FALSE;
    level_data->lock_flags = 0;
    ZeroMemory(&level_data->lock_rect, sizeof(level_data->lock_rect));
    return result ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI cube_add_dirty_rect(IDirect3DCubeTexture9 *iface,
        D3DCUBEMAP_FACES face, const RECT *rect)
{ (void)iface; (void)face; (void)rect; return D3D_OK; }

static IDirect3DCubeTexture9Vtbl g_cube_vtbl = {
    .QueryInterface = cube_query_interface,
    .AddRef = cube_add_ref,
    .Release = cube_release,
    .GetDevice = cube_get_device,
    .SetPrivateData = cube_set_private_data,
    .GetPrivateData = cube_get_private_data,
    .FreePrivateData = cube_free_private_data,
    .SetPriority = cube_set_priority,
    .GetPriority = cube_get_priority,
    .PreLoad = cube_preload,
    .GetType = cube_get_type,
    .SetLOD = cube_set_lod,
    .GetLOD = cube_get_lod,
    .GetLevelCount = cube_get_level_count,
    .SetAutoGenFilterType = cube_set_auto_gen_filter_type,
    .GetAutoGenFilterType = cube_get_auto_gen_filter_type,
    .GenerateMipSubLevels = cube_generate_mip_sublevels,
    .GetLevelDesc = cube_get_level_desc,
    .GetCubeMapSurface = cube_get_cube_map_surface,
    .LockRect = cube_lock_rect,
    .UnlockRect = cube_unlock_rect,
    .AddDirtyRect = cube_add_dirty_rect
};

/* ---- IDirect3DStateBlock9 (M3) ----
 *
 * A state block is a snapshot of a subset of device state plus a mask saying
 * which entries the subset contains: a captured zero is not the same thing as
 * an uncaptured state, so both halves are needed.
 *
 * Apply() replays the snapshot through the ordinary setters, which means it
 * inherits their deduplication and their D9WG emitters for free -- there is no
 * second, parallel path that could disagree with the first about what a given
 * state does.
 *
 * BeginStateBlock/EndStateBlock keeps both a pre-Begin snapshot and a separate
 * call mask. Every implemented Set* entry marks the latter before its normal
 * redundant-state suppression, so setting an existing value and setting then
 * reverting are recorded exactly as D3D9 requires. Setters run while recording
 * and End restores the snapshot; drawing during recording is invalid in D3D9,
 * so no draw can observe the temporary live values.
 */

/* D3DSBT_PIXELSTATE's documented render-state list. Capturing more than D3D9
 * does is not a safe simplification: Apply() would then restore states the app
 * deliberately kept, which shows up as an unrelated render mode snapping back. */
static const WORD g_pixel_state_render_states[] = {
    D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZWRITEENABLE,
    D3DRS_ALPHATESTENABLE, D3DRS_LASTPIXEL, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
    D3DRS_ZFUNC, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_DITHERENABLE,
    D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_ALPHABLENDENABLE,
    D3DRS_DEPTHBIAS, D3DRS_STENCILENABLE, D3DRS_STENCILFAIL,
    D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_STENCILFUNC,
    D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK,
    D3DRS_TEXTUREFACTOR, D3DRS_WRAP0, D3DRS_WRAP1, D3DRS_WRAP2, D3DRS_WRAP3,
    D3DRS_WRAP4, D3DRS_WRAP5, D3DRS_WRAP6, D3DRS_WRAP7, D3DRS_WRAP8,
    D3DRS_WRAP9, D3DRS_WRAP10, D3DRS_WRAP11, D3DRS_WRAP12, D3DRS_WRAP13,
    D3DRS_WRAP14, D3DRS_WRAP15, D3DRS_COLORWRITEENABLE, D3DRS_BLENDOP,
    D3DRS_SCISSORTESTENABLE, D3DRS_SLOPESCALEDEPTHBIAS,
    D3DRS_ANTIALIASEDLINEENABLE, D3DRS_TWOSIDEDSTENCILMODE,
    D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL, D3DRS_CCW_STENCILPASS,
    D3DRS_CCW_STENCILFUNC, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2,
    D3DRS_COLORWRITEENABLE3, D3DRS_BLENDFACTOR, D3DRS_SRGBWRITEENABLE,
    D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA,
    D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA
};

static const WORD g_vertex_state_render_states[] = {
    D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE,
    D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_RANGEFOGENABLE,
    D3DRS_AMBIENT, D3DRS_COLORVERTEX, D3DRS_FOGVERTEXMODE, D3DRS_CLIPPING,
    D3DRS_LIGHTING, D3DRS_LOCALVIEWER, D3DRS_EMISSIVEMATERIALSOURCE,
    D3DRS_AMBIENTMATERIALSOURCE, D3DRS_DIFFUSEMATERIALSOURCE,
    D3DRS_SPECULARMATERIALSOURCE, D3DRS_VERTEXBLEND, D3DRS_CLIPPLANEENABLE,
    D3DRS_POINTSIZE, D3DRS_POINTSIZE_MIN, D3DRS_POINTSPRITEENABLE,
    D3DRS_POINTSCALEENABLE, D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B,
    D3DRS_POINTSCALE_C, D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK,
    D3DRS_PATCHEDGESTYLE, D3DRS_POINTSIZE_MAX,
    D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_TWEENFACTOR,
    D3DRS_NORMALIZENORMALS, D3DRS_SPECULARENABLE, D3DRS_SHADEMODE
};

/* The transform indices a state block tracks. D3D9's full D3DTRANSFORMSTATETYPE
 * space is 512 entries wide (mostly the 256 world matrices vertex blending
 * uses), and fill_caps() reports MaxVertexBlendMatrices = 0, so snapshotting
 * all of them would cost 32 KB per block to preserve state nothing can read. */
static const WORD g_state_block_transforms[] = {
    D3DTS_VIEW, D3DTS_PROJECTION, D3DTS_WORLD,
    D3DTS_TEXTURE0, D3DTS_TEXTURE1, D3DTS_TEXTURE2, D3DTS_TEXTURE3,
    D3DTS_TEXTURE4, D3DTS_TEXTURE5, D3DTS_TEXTURE6, D3DTS_TEXTURE7
};
#define D9_STATE_BLOCK_TRANSFORMS \
    (sizeof(g_state_block_transforms) / sizeof(g_state_block_transforms[0]))

struct D9StateBlock {
    IDirect3DStateBlock9 iface;
    LONG refcount;
    D9Device *device;

    BOOL has_render_state[D9_MAX_RENDER_STATES];
    DWORD render_states[D9_MAX_RENDER_STATES];
    BOOL has_texture_stage_state[D9_MAX_TEXTURE_STAGES][D9_MAX_TEXTURE_STAGE_STATES];
    DWORD texture_stage_states[D9_MAX_TEXTURE_STAGES][D9_MAX_TEXTURE_STAGE_STATES];
    BOOL has_sampler_state[D9_MAX_SAMPLERS][D9_MAX_SAMPLER_STATES];
    DWORD sampler_states[D9_MAX_SAMPLERS][D9_MAX_SAMPLER_STATES];
    BOOL has_transform[D9_STATE_BLOCK_TRANSFORMS];
    float transforms[D9_STATE_BLOCK_TRANSFORMS][16];
    BOOL has_texture[D9_MAX_TEXTURE_STAGES];
    D9Texture *textures[D9_MAX_TEXTURE_STAGES];
    D9CubeTexture *cube_textures[D9_MAX_TEXTURE_STAGES];
    BOOL has_material;
    D3DMATERIAL9 material;
    BOOL has_light[D9_MAX_LIGHTS];
    D3DLIGHT9 lights[D9_MAX_LIGHTS];
    BOOL has_light_enable[D9_MAX_LIGHTS];
    BOOL light_enabled[D9_MAX_LIGHTS];
    BOOL has_viewport;
    D3DVIEWPORT9 viewport;
    BOOL has_scissor;
    RECT scissor_rect;
    BOOL has_vertex_shader;
    D9Shader *vertex_shader;
    BOOL has_pixel_shader;
    D9Shader *pixel_shader;
    BOOL has_vs_const_f[D9_MAX_VS_CONST_F];
    float vs_const_f[D9_MAX_VS_CONST_F][4];
    BOOL has_ps_const_f[D9_MAX_PS_CONST_F];
    float ps_const_f[D9_MAX_PS_CONST_F][4];
    BOOL has_vs_const_i[D9_MAX_CONST_I];
    int vs_const_i[D9_MAX_CONST_I][4];
    BOOL has_ps_const_i[D9_MAX_CONST_I];
    int ps_const_i[D9_MAX_CONST_I][4];
    BOOL has_vs_const_b[D9_MAX_CONST_B];
    BOOL vs_const_b[D9_MAX_CONST_B];
    BOOL has_ps_const_b[D9_MAX_CONST_B];
    BOOL ps_const_b[D9_MAX_CONST_B];
    BOOL has_vertex_format;
    DWORD fvf;
    D9VertexDeclaration *vertex_declaration;
    BOOL has_stream[D9_MAX_STREAMS];
    D9StreamBinding streams[D9_MAX_STREAMS];
    BOOL has_indices;
    D9IndexBuffer *index_buffer;
    struct D9StateBlock *next_device_resource;
};

static IDirect3DStateBlock9Vtbl g_state_block_vtbl;

static struct D9StateBlock *state_block_from_iface(IDirect3DStateBlock9 *iface)
{
    return (struct D9StateBlock *)iface;
}

static void state_block_record_render_state(D9Device *device, UINT state)
{
    if (device->recording_result && state < D9_MAX_RENDER_STATES)
        device->recording_result->has_render_state[state] = TRUE;
}

static void state_block_record_texture_stage_state(D9Device *device,
        UINT stage, UINT state)
{
    if (device->recording_result && stage < D9_MAX_TEXTURE_STAGES
            && state < D9_MAX_TEXTURE_STAGE_STATES)
        device->recording_result->has_texture_stage_state[stage][state] = TRUE;
}

static void state_block_record_sampler_state(D9Device *device,
        UINT sampler, UINT state)
{
    if (device->recording_result && sampler < D9_MAX_SAMPLERS
            && state < D9_MAX_SAMPLER_STATES)
        device->recording_result->has_sampler_state[sampler][state] = TRUE;
}

static void state_block_record_transform(D9Device *device, UINT state)
{
    UINT index;
    if (!device->recording_result)
        return;
    for (index = 0; index < D9_STATE_BLOCK_TRANSFORMS; ++index) {
        if (g_state_block_transforms[index] == state) {
            device->recording_result->has_transform[index] = TRUE;
            return;
        }
    }
}

static void state_block_record_texture(D9Device *device, UINT stage)
{
    if (device->recording_result && stage < D9_MAX_TEXTURE_STAGES)
        device->recording_result->has_texture[stage] = TRUE;
}

static void state_block_record_material(D9Device *device)
{
    if (device->recording_result)
        device->recording_result->has_material = TRUE;
}

static void state_block_record_light(D9Device *device, UINT index)
{
    if (device->recording_result && index < D9_MAX_LIGHTS)
        device->recording_result->has_light[index] = TRUE;
}

static void state_block_record_light_enable(D9Device *device, UINT index)
{
    if (device->recording_result && index < D9_MAX_LIGHTS)
        device->recording_result->has_light_enable[index] = TRUE;
}

static void state_block_record_viewport(D9Device *device)
{
    if (device->recording_result)
        device->recording_result->has_viewport = TRUE;
}

static void state_block_record_scissor(D9Device *device)
{
    if (device->recording_result)
        device->recording_result->has_scissor = TRUE;
}

static void state_block_record_vertex_shader(D9Device *device)
{
    if (device->recording_result)
        device->recording_result->has_vertex_shader = TRUE;
}

static void state_block_record_pixel_shader(D9Device *device)
{
    if (device->recording_result)
        device->recording_result->has_pixel_shader = TRUE;
}

static void state_block_record_constant(D9Device *device, BOOL is_pixel,
        BOOL is_float, BOOL is_bool, UINT start, UINT count)
{
    struct D9StateBlock *block = device->recording_result;
    UINT index;
    if (!block)
        return;
    for (index = start; index < start + count; ++index) {
        if (is_float) {
            if (is_pixel) block->has_ps_const_f[index] = TRUE;
            else block->has_vs_const_f[index] = TRUE;
        } else if (is_bool) {
            if (is_pixel) block->has_ps_const_b[index] = TRUE;
            else block->has_vs_const_b[index] = TRUE;
        } else {
            if (is_pixel) block->has_ps_const_i[index] = TRUE;
            else block->has_vs_const_i[index] = TRUE;
        }
    }
}

static void state_block_record_vertex_format(D9Device *device)
{
    if (device->recording_result)
        device->recording_result->has_vertex_format = TRUE;
}

static void state_block_record_stream(D9Device *device, UINT stream)
{
    if (device->recording_result && stream < D9_MAX_STREAMS)
        device->recording_result->has_stream[stream] = TRUE;
}

static void state_block_record_indices(D9Device *device)
{
    if (device->recording_result)
        device->recording_result->has_indices = TRUE;
}

/* D3D9 state blocks keep every captured COM object alive.  Without these
 * references an engine may release the texture/shader it used to build a UI
 * block, leaving Apply() with a dangling pointer and the host with a destroyed
 * resource handle. */
static void state_block_release_references(struct D9StateBlock *block)
{
    UINT index;
    for (index = 0; index < D9_MAX_TEXTURE_STAGES; ++index) {
        if (block->textures[index])
            IDirect3DTexture9_Release(&block->textures[index]->iface);
        if (block->cube_textures[index])
            IDirect3DCubeTexture9_Release(&block->cube_textures[index]->iface);
        block->textures[index] = NULL;
        block->cube_textures[index] = NULL;
    }
    if (block->vertex_shader)
        IDirect3DVertexShader9_Release(&block->vertex_shader->iface.vertex);
    if (block->pixel_shader)
        IDirect3DPixelShader9_Release(&block->pixel_shader->iface.pixel);
    block->vertex_shader = NULL;
    block->pixel_shader = NULL;
    if (block->vertex_declaration)
        IDirect3DVertexDeclaration9_Release(&block->vertex_declaration->iface);
    block->vertex_declaration = NULL;
    for (index = 0; index < D9_MAX_STREAMS; ++index) {
        if (block->streams[index].buffer)
            IDirect3DVertexBuffer9_Release(&block->streams[index].buffer->iface);
        block->streams[index].buffer = NULL;
    }
    if (block->index_buffer)
        IDirect3DIndexBuffer9_Release(&block->index_buffer->iface);
    block->index_buffer = NULL;
}

/* Copies the live device value into every slot the mask already marks. This is
 * both Capture() and the second half of block creation, so "which states does
 * this block hold" is decided in exactly one place per block type. */
static void state_block_capture(struct D9StateBlock *block)
{
    D9Device *device = block->device;
    UINT index, stage, slot;

    state_block_release_references(block);

    for (index = 0; index < D9_MAX_RENDER_STATES; ++index) {
        if (block->has_render_state[index])
            block->render_states[index] = device->render_states[index];
    }
    for (stage = 0; stage < D9_MAX_TEXTURE_STAGES; ++stage) {
        for (index = 0; index < D9_MAX_TEXTURE_STAGE_STATES; ++index) {
            if (block->has_texture_stage_state[stage][index])
                block->texture_stage_states[stage][index] =
                        device->texture_stage_states[stage][index];
        }
        if (block->has_texture[stage]) {
            block->textures[stage] = device->textures[stage];
            block->cube_textures[stage] = device->cube_bindings[stage];
            if (block->textures[stage])
                IDirect3DTexture9_AddRef(&block->textures[stage]->iface);
            if (block->cube_textures[stage])
                IDirect3DCubeTexture9_AddRef(&block->cube_textures[stage]->iface);
        }
    }
    for (slot = 0; slot < D9_MAX_SAMPLERS; ++slot) {
        for (index = 0; index < D9_MAX_SAMPLER_STATES; ++index) {
            if (block->has_sampler_state[slot][index])
                block->sampler_states[slot][index] =
                        device->sampler_states[slot][index];
        }
    }
    for (index = 0; index < D9_STATE_BLOCK_TRANSFORMS; ++index) {
        if (block->has_transform[index])
            CopyMemory(block->transforms[index],
                    device->transforms[g_state_block_transforms[index]],
                    sizeof(block->transforms[index]));
    }
    if (block->has_material) block->material = device->material;
    for (index = 0; index < D9_MAX_LIGHTS; ++index) {
        if (block->has_light[index]) block->lights[index] = device->lights[index];
        if (block->has_light_enable[index])
            block->light_enabled[index] = device->light_enabled[index];
    }
    if (block->has_viewport) block->viewport = device->viewport;
    if (block->has_scissor) block->scissor_rect = device->scissor_rect;
    if (block->has_vertex_shader) {
        block->vertex_shader = device->vertex_shader;
        if (block->vertex_shader)
            IDirect3DVertexShader9_AddRef(&block->vertex_shader->iface.vertex);
    }
    if (block->has_pixel_shader) {
        block->pixel_shader = device->pixel_shader;
        if (block->pixel_shader)
            IDirect3DPixelShader9_AddRef(&block->pixel_shader->iface.pixel);
    }
    for (index = 0; index < D9_MAX_VS_CONST_F; ++index) {
        if (block->has_vs_const_f[index])
            CopyMemory(block->vs_const_f[index], device->vs_const_f[index], 16);
    }
    for (index = 0; index < D9_MAX_PS_CONST_F; ++index) {
        if (block->has_ps_const_f[index])
            CopyMemory(block->ps_const_f[index], device->ps_const_f[index], 16);
    }
    for (index = 0; index < D9_MAX_CONST_I; ++index) {
        if (block->has_vs_const_i[index])
            CopyMemory(block->vs_const_i[index], device->vs_const_i[index], 16);
        if (block->has_ps_const_i[index])
            CopyMemory(block->ps_const_i[index], device->ps_const_i[index], 16);
    }
    for (index = 0; index < D9_MAX_CONST_B; ++index) {
        if (block->has_vs_const_b[index])
            block->vs_const_b[index] = device->vs_const_b[index];
        if (block->has_ps_const_b[index])
            block->ps_const_b[index] = device->ps_const_b[index];
    }
    if (block->has_vertex_format) {
        block->fvf = device->fvf;
        block->vertex_declaration = device->vertex_declaration;
        if (block->vertex_declaration)
            IDirect3DVertexDeclaration9_AddRef(
                    &block->vertex_declaration->iface);
    }
    for (index = 0; index < D9_MAX_STREAMS; ++index) {
        if (block->has_stream[index]) {
            block->streams[index] = device->streams[index];
            if (block->streams[index].buffer)
                IDirect3DVertexBuffer9_AddRef(
                        &block->streams[index].buffer->iface);
        }
    }
    if (block->has_indices) {
        block->index_buffer = device->index_buffer;
        if (block->index_buffer)
            IDirect3DIndexBuffer9_AddRef(&block->index_buffer->iface);
    }
}

static void state_block_mark_pixel_states(struct D9StateBlock *block)
{
    UINT index, stage, slot;
    for (index = 0; index < sizeof(g_pixel_state_render_states) /
            sizeof(g_pixel_state_render_states[0]); ++index) {
        WORD state = g_pixel_state_render_states[index];
        if (state < D9_MAX_RENDER_STATES) block->has_render_state[state] = TRUE;
    }
    for (stage = 0; stage < D9_MAX_TEXTURE_STAGES; ++stage) {
        for (index = 0; index < D9_MAX_TEXTURE_STAGE_STATES; ++index) {
            /* TEXCOORDINDEX and TEXTURETRANSFORMFLAGS belong to vertex state:
             * they decide which coordinates a stage gets, not how it blends. */
            if (index == D3DTSS_TEXCOORDINDEX ||
                    index == D3DTSS_TEXTURETRANSFORMFLAGS)
                continue;
            block->has_texture_stage_state[stage][index] = TRUE;
        }
    }
    for (slot = 0; slot < D9_MAX_SAMPLERS; ++slot) {
        for (index = 0; index < D9_MAX_SAMPLER_STATES; ++index)
            block->has_sampler_state[slot][index] = TRUE;
    }
    block->has_pixel_shader = TRUE;
    for (index = 0; index < D9_MAX_PS_CONST_F; ++index)
        block->has_ps_const_f[index] = TRUE;
    for (index = 0; index < D9_MAX_CONST_I; ++index)
        block->has_ps_const_i[index] = TRUE;
    for (index = 0; index < D9_MAX_CONST_B; ++index)
        block->has_ps_const_b[index] = TRUE;
}

static void state_block_mark_vertex_states(struct D9StateBlock *block)
{
    UINT index, stage;
    for (index = 0; index < sizeof(g_vertex_state_render_states) /
            sizeof(g_vertex_state_render_states[0]); ++index) {
        WORD state = g_vertex_state_render_states[index];
        if (state < D9_MAX_RENDER_STATES) block->has_render_state[state] = TRUE;
    }
    for (stage = 0; stage < D9_MAX_TEXTURE_STAGES; ++stage) {
        block->has_texture_stage_state[stage][D3DTSS_TEXCOORDINDEX] = TRUE;
        block->has_texture_stage_state[stage][D3DTSS_TEXTURETRANSFORMFLAGS] = TRUE;
    }
    block->has_material = TRUE;
    for (index = 0; index < D9_MAX_LIGHTS; ++index) {
        block->has_light[index] = device_light_is_set(block->device, index);
        block->has_light_enable[index] = TRUE;
    }
    block->has_vertex_shader = TRUE;
    block->has_vertex_format = TRUE;
    for (index = 0; index < D9_MAX_VS_CONST_F; ++index)
        block->has_vs_const_f[index] = TRUE;
    for (index = 0; index < D9_MAX_CONST_I; ++index)
        block->has_vs_const_i[index] = TRUE;
    for (index = 0; index < D9_MAX_CONST_B; ++index)
        block->has_vs_const_b[index] = TRUE;
}

static void state_block_mark_all(struct D9StateBlock *block)
{
    UINT index, stage;
    state_block_mark_pixel_states(block);
    state_block_mark_vertex_states(block);
    for (index = 0; index < D9_MAX_RENDER_STATES; ++index)
        block->has_render_state[index] = TRUE;
    for (stage = 0; stage < D9_MAX_TEXTURE_STAGES; ++stage) {
        for (index = 0; index < D9_MAX_TEXTURE_STAGE_STATES; ++index)
            block->has_texture_stage_state[stage][index] = TRUE;
        block->has_texture[stage] = TRUE;
    }
    for (index = 0; index < D9_STATE_BLOCK_TRANSFORMS; ++index)
        block->has_transform[index] = TRUE;
    block->has_viewport = TRUE;
    block->has_scissor = TRUE;
    for (index = 0; index < D9_MAX_STREAMS; ++index)
        block->has_stream[index] = TRUE;
    block->has_indices = TRUE;
}

static struct D9StateBlock *state_block_alloc(D9Device *device)
{
    struct D9StateBlock *block = (struct D9StateBlock *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*block));
    if (!block)
        return NULL;
    block->iface.lpVtbl = &g_state_block_vtbl;
    block->refcount = 1;
    block->device = device;
    device_child_add_ref(device);
    block->next_device_resource = device->state_blocks;
    device->state_blocks = block;
    return block;
}

/* Replays every captured entry through the public setters, so a state block can
 * never apply a state by a route the app's own equivalent call would not take. */
static HRESULT state_block_apply(struct D9StateBlock *block)
{
    D9Device *device = block->device;
    IDirect3DDevice9 *iface = &device->iface;
    UINT index, stage, slot;

    for (index = 0; index < D9_MAX_RENDER_STATES; ++index) {
        if (block->has_render_state[index])
            IDirect3DDevice9_SetRenderState(iface, (D3DRENDERSTATETYPE)index,
                    block->render_states[index]);
    }
    for (stage = 0; stage < D9_MAX_TEXTURE_STAGES; ++stage) {
        for (index = 0; index < D9_MAX_TEXTURE_STAGE_STATES; ++index) {
            if (block->has_texture_stage_state[stage][index])
                IDirect3DDevice9_SetTextureStageState(iface, stage,
                        (D3DTEXTURESTAGESTATETYPE)index,
                        block->texture_stage_states[stage][index]);
        }
        if (block->has_texture[stage]) {
            IDirect3DBaseTexture9 *texture = NULL;
            if (block->textures[stage])
                texture = (IDirect3DBaseTexture9 *)&block->textures[stage]->iface;
            else if (block->cube_textures[stage])
                texture = (IDirect3DBaseTexture9 *)&block->cube_textures[stage]->iface;
            IDirect3DDevice9_SetTexture(iface, stage, texture);
        }
    }
    for (slot = 0; slot < D9_MAX_SAMPLERS; ++slot) {
        for (index = 0; index < D9_MAX_SAMPLER_STATES; ++index) {
            if (block->has_sampler_state[slot][index])
                IDirect3DDevice9_SetSamplerState(iface, slot,
                        (D3DSAMPLERSTATETYPE)index,
                        block->sampler_states[slot][index]);
        }
    }
    for (index = 0; index < D9_STATE_BLOCK_TRANSFORMS; ++index) {
        if (block->has_transform[index])
            IDirect3DDevice9_SetTransform(iface,
                    (D3DTRANSFORMSTATETYPE)g_state_block_transforms[index],
                    (const D3DMATRIX *)block->transforms[index]);
    }
    if (block->has_material)
        IDirect3DDevice9_SetMaterial(iface, &block->material);
    for (index = 0; index < D9_MAX_LIGHTS; ++index) {
        if (block->has_light[index])
            IDirect3DDevice9_SetLight(iface, index, &block->lights[index]);
        if (block->has_light_enable[index])
            IDirect3DDevice9_LightEnable(iface, index,
                    block->light_enabled[index]);
    }
    if (block->has_viewport)
        IDirect3DDevice9_SetViewport(iface, &block->viewport);
    if (block->has_scissor)
        IDirect3DDevice9_SetScissorRect(iface, &block->scissor_rect);
    if (block->has_vertex_shader)
        IDirect3DDevice9_SetVertexShader(iface, block->vertex_shader
                ? &block->vertex_shader->iface.vertex : NULL);
    if (block->has_pixel_shader)
        IDirect3DDevice9_SetPixelShader(iface, block->pixel_shader
                ? &block->pixel_shader->iface.pixel : NULL);
    for (index = 0; index < D9_MAX_VS_CONST_F; ++index) {
        if (block->has_vs_const_f[index])
            IDirect3DDevice9_SetVertexShaderConstantF(iface, index,
                    block->vs_const_f[index], 1);
    }
    for (index = 0; index < D9_MAX_PS_CONST_F; ++index) {
        if (block->has_ps_const_f[index])
            IDirect3DDevice9_SetPixelShaderConstantF(iface, index,
                    block->ps_const_f[index], 1);
    }
    for (index = 0; index < D9_MAX_CONST_I; ++index) {
        if (block->has_vs_const_i[index])
            IDirect3DDevice9_SetVertexShaderConstantI(iface, index,
                    block->vs_const_i[index], 1);
        if (block->has_ps_const_i[index])
            IDirect3DDevice9_SetPixelShaderConstantI(iface, index,
                    block->ps_const_i[index], 1);
    }
    for (index = 0; index < D9_MAX_CONST_B; ++index) {
        if (block->has_vs_const_b[index])
            IDirect3DDevice9_SetVertexShaderConstantB(iface, index,
                    &block->vs_const_b[index], 1);
        if (block->has_ps_const_b[index])
            IDirect3DDevice9_SetPixelShaderConstantB(iface, index,
                    &block->ps_const_b[index], 1);
    }
    if (block->has_vertex_format) {
        /* A declaration and an FVF are mutually exclusive in D3D9: whichever
         * the device last had is the one to restore, and restoring both would
         * leave the loser silently overriding the winner. */
        if (block->vertex_declaration)
            IDirect3DDevice9_SetVertexDeclaration(iface,
                    &block->vertex_declaration->iface);
        else if (block->fvf)
            IDirect3DDevice9_SetFVF(iface, block->fvf);
        else
            IDirect3DDevice9_SetVertexDeclaration(iface, NULL);
    }
    for (index = 0; index < D9_MAX_STREAMS; ++index) {
        if (block->has_stream[index])
            IDirect3DDevice9_SetStreamSource(iface, index,
                    block->streams[index].buffer
                        ? &block->streams[index].buffer->iface : NULL,
                    block->streams[index].offset, block->streams[index].stride);
    }
    if (block->has_indices)
        IDirect3DDevice9_SetIndices(iface, block->index_buffer
                ? &block->index_buffer->iface : NULL);
    return D3D_OK;
}

static HRESULT WINAPI state_block_query_interface(IDirect3DStateBlock9 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DStateBlock9)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DStateBlock9_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI state_block_add_ref(IDirect3DStateBlock9 *iface)
{
    return (ULONG)InterlockedIncrement(&state_block_from_iface(iface)->refcount);
}

static ULONG WINAPI state_block_release(IDirect3DStateBlock9 *iface)
{
    struct D9StateBlock *block = state_block_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&block->refcount);
    if (!refs) {
        struct D9StateBlock **link = &block->device->state_blocks;
        while (*link && *link != block)
            link = &(*link)->next_device_resource;
        if (*link) *link = block->next_device_resource;
        state_block_release_references(block);
        device_child_release(block->device);
        HeapFree(GetProcessHeap(), 0, block);
    }
    return refs;
}

static HRESULT WINAPI state_block_get_device(IDirect3DStateBlock9 *iface,
        IDirect3DDevice9 **device_out)
{
    struct D9StateBlock *block = state_block_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &block->device->iface;
    IDirect3DDevice9_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI state_block_capture_method(IDirect3DStateBlock9 *iface)
{
    state_block_capture(state_block_from_iface(iface));
    return D3D_OK;
}

static HRESULT WINAPI state_block_apply_method(IDirect3DStateBlock9 *iface)
{
    struct D9StateBlock *block = state_block_from_iface(iface);
    if (block->device->recording)
        return D3DERR_INVALIDCALL;
    return state_block_apply(block);
}

static IDirect3DStateBlock9Vtbl g_state_block_vtbl = {
    .QueryInterface = state_block_query_interface,
    .AddRef = state_block_add_ref,
    .Release = state_block_release,
    .GetDevice = state_block_get_device,
    .Capture = state_block_capture_method,
    .Apply = state_block_apply_method
};

static HRESULT WINAPI device_create_state_block(IDirect3DDevice9 *iface,
        D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9 **block_out)
{
    D9Device *device = device_from_iface(iface);
    struct D9StateBlock *block;

    if (!block_out)
        return D3DERR_INVALIDCALL;
    *block_out = NULL;
    if (device->recording)
        return D3DERR_INVALIDCALL;
    if (type != D3DSBT_ALL && type != D3DSBT_PIXELSTATE
            && type != D3DSBT_VERTEXSTATE)
        return D3DERR_INVALIDCALL;
    block = state_block_alloc(device);
    if (!block)
        return E_OUTOFMEMORY;
    if (type == D3DSBT_ALL) state_block_mark_all(block);
    else if (type == D3DSBT_PIXELSTATE) state_block_mark_pixel_states(block);
    else state_block_mark_vertex_states(block);
    state_block_capture(block);
    *block_out = &block->iface;
    return D3D_OK;
}

static HRESULT WINAPI device_begin_state_block(IDirect3DDevice9 *iface)
{
    D9Device *device = device_from_iface(iface);
    struct D9StateBlock *baseline;
    struct D9StateBlock *result;

    if (device->recording)
        return D3DERR_INVALIDCALL;
    baseline = state_block_alloc(device);
    if (!baseline)
        return E_OUTOFMEMORY;
    result = state_block_alloc(device);
    if (!result) {
        IDirect3DStateBlock9_Release(&baseline->iface);
        return E_OUTOFMEMORY;
    }
    state_block_mark_all(baseline);
    state_block_capture(baseline);
    device->recording = baseline;
    device->recording_result = result;
    return D3D_OK;
}

static HRESULT WINAPI device_end_state_block(IDirect3DDevice9 *iface,
        IDirect3DStateBlock9 **block_out)
{
    D9Device *device = device_from_iface(iface);
    struct D9StateBlock *baseline = device->recording;
    struct D9StateBlock *block = device->recording_result;

    if (!baseline || !block)
        return D3DERR_INVALIDCALL;
    if (!block_out) {
        device->recording = NULL;
        device->recording_result = NULL;
        state_block_apply(baseline);
        IDirect3DStateBlock9_Release(&block->iface);
        IDirect3DStateBlock9_Release(&baseline->iface);
        return D3DERR_INVALIDCALL;
    }
    *block_out = NULL;
    /* Capture only the explicitly touched mask. This preserves same-value and
     * write-then-revert calls which a final-value diff necessarily loses. */
    state_block_capture(block);
    device->recording = NULL;
    device->recording_result = NULL;
    /* Put the device back the way the app left it before Begin. */
    state_block_apply(baseline);
    IDirect3DStateBlock9_Release(&baseline->iface);
    *block_out = &block->iface;
    return D3D_OK;
}

/* ---- IDirect3DVertexBuffer9 ---- */

static HRESULT WINAPI vb_query_interface(IDirect3DVertexBuffer9 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DResource9)
            && !guid_equal(iid, &IID_IDirect3DVertexBuffer9)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DVertexBuffer9_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI vb_add_ref(IDirect3DVertexBuffer9 *iface)
{
    return (ULONG)InterlockedIncrement(&vb_from_iface(iface)->refcount);
}

static ULONG WINAPI vb_release(IDirect3DVertexBuffer9 *iface)
{
    D9VertexBuffer *buffer = vb_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&buffer->refcount);
    if (!refs) {
        D9VertexBuffer **link = &buffer->device->vertex_buffers;
        D9WGDestroyResource destroy;
        while (*link && *link != buffer)
            link = &(*link)->next_device_resource;
        if (*link) *link = buffer->next_device_resource;
        destroy.resource_handle = buffer->handle;
        destroy.resource_kind = D9WG_RESOURCE_BUFFER_VERTEX;
        emit_command(D9WG_OP_DESTROY_RESOURCE, &destroy, sizeof(destroy));
        HeapFree(GetProcessHeap(), 0, buffer->shadow);
        device_child_release(buffer->device);
        HeapFree(GetProcessHeap(), 0, buffer);
    }
    return refs;
}

static HRESULT WINAPI vb_get_device(IDirect3DVertexBuffer9 *iface,
        IDirect3DDevice9 **device_out)
{
    D9VertexBuffer *buffer = vb_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &buffer->device->iface;
    IDirect3DDevice9_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI vb_set_private_data(IDirect3DVertexBuffer9 *iface,
        REFGUID guid, const void *data, DWORD size, DWORD flags)
{ (void)iface; (void)guid; (void)data; (void)size; (void)flags;
  return D3DERR_INVALIDCALL; }

static HRESULT WINAPI vb_get_private_data(IDirect3DVertexBuffer9 *iface,
        REFGUID guid, void *data, DWORD *size)
{ (void)iface; (void)guid; (void)data; (void)size; return D3DERR_NOTFOUND; }

static HRESULT WINAPI vb_free_private_data(IDirect3DVertexBuffer9 *iface,
        REFGUID guid)
{ (void)iface; (void)guid; return D3DERR_NOTFOUND; }

static DWORD WINAPI vb_set_priority(IDirect3DVertexBuffer9 *iface,
        DWORD priority)
{
    D9VertexBuffer *buffer = vb_from_iface(iface);
    DWORD old = buffer->priority;
    buffer->priority = priority;
    return old;
}

static DWORD WINAPI vb_get_priority(IDirect3DVertexBuffer9 *iface)
{ return vb_from_iface(iface)->priority; }

static void WINAPI vb_preload(IDirect3DVertexBuffer9 *iface)
{ (void)iface; }

static D3DRESOURCETYPE WINAPI vb_get_type(IDirect3DVertexBuffer9 *iface)
{ (void)iface; return D3DRTYPE_VERTEXBUFFER; }

static HRESULT WINAPI vb_lock(IDirect3DVertexBuffer9 *iface, UINT offset,
        UINT size, void **data_out, DWORD flags)
{
    D9VertexBuffer *buffer = vb_from_iface(iface);
    if (!data_out || buffer->locked || offset > buffer->length)
        return D3DERR_INVALIDCALL;
    if ((flags & D3DLOCK_DISCARD) && (flags & D3DLOCK_NOOVERWRITE))
        return D3DERR_INVALIDCALL;
    if ((flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
            && !(buffer->usage & D3DUSAGE_DYNAMIC))
        return D3DERR_INVALIDCALL;
    if ((flags & D3DLOCK_READONLY)
            && (flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)))
        return D3DERR_INVALIDCALL;
    if (!size)
        size = buffer->length - offset;
    if (size > buffer->length - offset)
        return D3DERR_INVALIDCALL;
    buffer->locked = TRUE;
    buffer->lock_offset = offset;
    buffer->lock_size = size;
    buffer->lock_flags = flags;
    if (flags & D3DLOCK_DISCARD)
        ZeroMemory(buffer->shadow, buffer->length);
    *data_out = buffer->shadow + offset;
    return D3D_OK;
}

static HRESULT WINAPI vb_unlock(IDirect3DVertexBuffer9 *iface)
{
    D9VertexBuffer *buffer = vb_from_iface(iface);
    BOOL result;
    if (!buffer->locked)
        return D3DERR_INVALIDCALL;
    result = (buffer->lock_flags & D3DLOCK_READONLY)
            || emit_buffer_update(buffer->handle, buffer->lock_offset,
                    buffer->shadow + buffer->lock_offset, buffer->lock_size,
                    buffer->lock_flags);
    buffer->locked = FALSE;
    buffer->lock_offset = 0;
    buffer->lock_size = 0;
    buffer->lock_flags = 0;
    return result ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI vb_get_desc(IDirect3DVertexBuffer9 *iface,
        D3DVERTEXBUFFER_DESC *desc)
{
    D9VertexBuffer *buffer = vb_from_iface(iface);
    if (!desc)
        return D3DERR_INVALIDCALL;
    ZeroMemory(desc, sizeof(*desc));
    desc->Format = D3DFMT_VERTEXDATA;
    desc->Type = D3DRTYPE_VERTEXBUFFER;
    desc->Usage = buffer->usage;
    desc->Pool = buffer->pool;
    desc->Size = buffer->length;
    desc->FVF = buffer->fvf;
    return D3D_OK;
}

/* ---- IDirect3DIndexBuffer9 ---- */

static HRESULT WINAPI ib_query_interface(IDirect3DIndexBuffer9 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DResource9)
            && !guid_equal(iid, &IID_IDirect3DIndexBuffer9)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DIndexBuffer9_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI ib_add_ref(IDirect3DIndexBuffer9 *iface)
{
    return (ULONG)InterlockedIncrement(&ib_from_iface(iface)->refcount);
}

static ULONG WINAPI ib_release(IDirect3DIndexBuffer9 *iface)
{
    D9IndexBuffer *buffer = ib_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&buffer->refcount);
    if (!refs) {
        D9IndexBuffer **link = &buffer->device->index_buffers;
        D9WGDestroyResource destroy;
        while (*link && *link != buffer)
            link = &(*link)->next_device_resource;
        if (*link) *link = buffer->next_device_resource;
        destroy.resource_handle = buffer->handle;
        destroy.resource_kind = D9WG_RESOURCE_BUFFER_INDEX;
        emit_command(D9WG_OP_DESTROY_RESOURCE, &destroy, sizeof(destroy));
        HeapFree(GetProcessHeap(), 0, buffer->shadow);
        device_child_release(buffer->device);
        HeapFree(GetProcessHeap(), 0, buffer);
    }
    return refs;
}

static HRESULT WINAPI ib_get_device(IDirect3DIndexBuffer9 *iface,
        IDirect3DDevice9 **device_out)
{
    D9IndexBuffer *buffer = ib_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &buffer->device->iface;
    IDirect3DDevice9_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI ib_set_private_data(IDirect3DIndexBuffer9 *iface,
        REFGUID guid, const void *data, DWORD size, DWORD flags)
{ (void)iface; (void)guid; (void)data; (void)size; (void)flags;
  return D3DERR_INVALIDCALL; }

static HRESULT WINAPI ib_get_private_data(IDirect3DIndexBuffer9 *iface,
        REFGUID guid, void *data, DWORD *size)
{ (void)iface; (void)guid; (void)data; (void)size; return D3DERR_NOTFOUND; }

static HRESULT WINAPI ib_free_private_data(IDirect3DIndexBuffer9 *iface,
        REFGUID guid)
{ (void)iface; (void)guid; return D3DERR_NOTFOUND; }

static DWORD WINAPI ib_set_priority(IDirect3DIndexBuffer9 *iface,
        DWORD priority)
{
    D9IndexBuffer *buffer = ib_from_iface(iface);
    DWORD old = buffer->priority;
    buffer->priority = priority;
    return old;
}

static DWORD WINAPI ib_get_priority(IDirect3DIndexBuffer9 *iface)
{ return ib_from_iface(iface)->priority; }

static void WINAPI ib_preload(IDirect3DIndexBuffer9 *iface)
{ (void)iface; }

static D3DRESOURCETYPE WINAPI ib_get_type(IDirect3DIndexBuffer9 *iface)
{ (void)iface; return D3DRTYPE_INDEXBUFFER; }

static HRESULT WINAPI ib_lock(IDirect3DIndexBuffer9 *iface, UINT offset,
        UINT size, void **data_out, DWORD flags)
{
    D9IndexBuffer *buffer = ib_from_iface(iface);
    if (!data_out || buffer->locked || offset > buffer->length)
        return D3DERR_INVALIDCALL;
    if ((flags & D3DLOCK_DISCARD) && (flags & D3DLOCK_NOOVERWRITE))
        return D3DERR_INVALIDCALL;
    if ((flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
            && !(buffer->usage & D3DUSAGE_DYNAMIC))
        return D3DERR_INVALIDCALL;
    if (!size)
        size = buffer->length - offset;
    if (size > buffer->length - offset)
        return D3DERR_INVALIDCALL;
    buffer->locked = TRUE;
    buffer->lock_offset = offset;
    buffer->lock_size = size;
    buffer->lock_flags = flags;
    if (flags & D3DLOCK_DISCARD)
        ZeroMemory(buffer->shadow, buffer->length);
    *data_out = buffer->shadow + offset;
    return D3D_OK;
}

static HRESULT WINAPI ib_unlock(IDirect3DIndexBuffer9 *iface)
{
    D9IndexBuffer *buffer = ib_from_iface(iface);
    BOOL result;
    if (!buffer->locked)
        return D3DERR_INVALIDCALL;
    result = (buffer->lock_flags & D3DLOCK_READONLY)
            || emit_buffer_update(buffer->handle, buffer->lock_offset,
                    buffer->shadow + buffer->lock_offset, buffer->lock_size,
                    buffer->lock_flags);
    buffer->locked = FALSE;
    buffer->lock_offset = 0;
    buffer->lock_size = 0;
    buffer->lock_flags = 0;
    return result ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI ib_get_desc(IDirect3DIndexBuffer9 *iface,
        D3DINDEXBUFFER_DESC *desc)
{
    D9IndexBuffer *buffer = ib_from_iface(iface);
    if (!desc)
        return D3DERR_INVALIDCALL;
    ZeroMemory(desc, sizeof(*desc));
    desc->Format = buffer->format;
    desc->Type = D3DRTYPE_INDEXBUFFER;
    desc->Usage = buffer->usage;
    desc->Pool = buffer->pool;
    desc->Size = buffer->length;
    return D3D_OK;
}

/* ---- IDirect3DTexture9 ---- */

static HRESULT texture_lock_level(D9Texture *texture, UINT level,
        D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags)
{
    D9TextureLevel *level_data;
    RECT area;
    UINT block_width;
    UINT block_height;
    UINT block_bytes;
    UINT block_x;
    UINT block_y;

    if (!locked_rect || level >= texture->level_count
            || (texture->usage & (D3DUSAGE_RENDERTARGET
                | D3DUSAGE_DEPTHSTENCIL)))
        return D3DERR_INVALIDCALL;
    level_data = &texture->levels[level];
    if (level_data->locked || !level_data->shadow)
        return D3DERR_INVALIDCALL;
    if (rect) {
        area = *rect;
    } else {
        SetRect(&area, 0, 0, (int)level_data->width, (int)level_data->height);
    }
    if (area.left < 0 || area.top < 0 || area.right <= area.left
            || area.bottom <= area.top
            || (UINT)area.right > level_data->width
            || (UINT)area.bottom > level_data->height
            || !texture_format_layout(texture->format, &block_width,
                    &block_height, &block_bytes))
        return D3DERR_INVALIDCALL;
    if (block_width > 1
            && (((UINT)area.left % block_width)
                || ((UINT)area.top % block_height)
                || ((UINT)area.right != level_data->width
                    && (UINT)area.right % block_width)
                || ((UINT)area.bottom != level_data->height
                    && (UINT)area.bottom % block_height)))
        return D3DERR_INVALIDCALL;

    block_x = (UINT)area.left / block_width;
    block_y = (UINT)area.top / block_height;
    level_data->lock_rect = area;
    level_data->lock_flags = flags;
    level_data->locked = TRUE;
    locked_rect->Pitch = (INT)level_data->row_pitch;
    locked_rect->pBits = level_data->shadow
            + block_y * level_data->row_pitch + block_x * block_bytes;
    return D3D_OK;
}

static HRESULT texture_unlock_level(D9Texture *texture, UINT level)
{
    D9TextureLevel *level_data;
    BOOL result = TRUE;

    if (level >= texture->level_count)
        return D3DERR_INVALIDCALL;
    level_data = &texture->levels[level];
    if (!level_data->locked)
        return D3DERR_INVALIDCALL;
    if (!(level_data->lock_flags & D3DLOCK_READONLY))
        result = emit_texture_update(texture, level, &level_data->lock_rect);
    level_data->locked = FALSE;
    level_data->lock_flags = 0;
    ZeroMemory(&level_data->lock_rect, sizeof(level_data->lock_rect));
    return result ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI texture_query_interface(IDirect3DTexture9 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DResource9)
            && !guid_equal(iid, &IID_IDirect3DBaseTexture9)
            && !guid_equal(iid, &IID_IDirect3DTexture9)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DTexture9_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI texture_add_ref(IDirect3DTexture9 *iface)
{
    return (ULONG)InterlockedIncrement(&texture_from_iface(iface)->refcount);
}

static ULONG WINAPI texture_release(IDirect3DTexture9 *iface)
{
    D9Texture *texture = texture_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&texture->refcount);
    if (!refs) {
        D9Texture **link = &texture->device->texture_resources;
        D9WGDestroyResource destroy;
        UINT level;
        while (*link && *link != texture)
            link = &(*link)->next_device_resource;
        if (*link) *link = texture->next_device_resource;
        destroy.resource_handle = texture->handle;
        destroy.resource_kind = D9WG_RESOURCE_TEXTURE_2D;
        emit_command(D9WG_OP_DESTROY_RESOURCE, &destroy, sizeof(destroy));
        for (level = 0; level < texture->level_count; ++level) {
            /* The level's surface sub-object dies with the texture; nothing
             * can still hold it, because every surface reference is one of the
             * texture references that just reached zero. */
            if (texture->levels[level].level_surface) {
                TRACE_SURFACE_FREED(
                        &texture->levels[level].level_surface->iface);
                HeapFree(GetProcessHeap(), 0,
                        texture->levels[level].level_surface);
                texture->levels[level].level_surface = NULL;
            }
            HeapFree(GetProcessHeap(), 0, texture->levels[level].shadow);
        }
        HeapFree(GetProcessHeap(), 0, texture->levels);
        device_child_release(texture->device);
        HeapFree(GetProcessHeap(), 0, texture);
    }
    return refs;
}

static HRESULT WINAPI texture_get_device(IDirect3DTexture9 *iface,
        IDirect3DDevice9 **device_out)
{
    D9Texture *texture = texture_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &texture->device->iface;
    IDirect3DDevice9_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI texture_set_private_data(IDirect3DTexture9 *iface,
        REFGUID guid, const void *data, DWORD size, DWORD flags)
{ (void)iface; (void)guid; (void)data; (void)size; (void)flags;
  return D3DERR_INVALIDCALL; }

static HRESULT WINAPI texture_get_private_data(IDirect3DTexture9 *iface,
        REFGUID guid, void *data, DWORD *size)
{ (void)iface; (void)guid; (void)data; (void)size; return D3DERR_NOTFOUND; }

static HRESULT WINAPI texture_free_private_data(IDirect3DTexture9 *iface,
        REFGUID guid)
{ (void)iface; (void)guid; return D3DERR_NOTFOUND; }

static DWORD WINAPI texture_set_priority(IDirect3DTexture9 *iface,
        DWORD priority)
{
    D9Texture *texture = texture_from_iface(iface);
    DWORD old = texture->priority;
    texture->priority = priority;
    return old;
}

static DWORD WINAPI texture_get_priority(IDirect3DTexture9 *iface)
{ return texture_from_iface(iface)->priority; }

static void WINAPI texture_preload(IDirect3DTexture9 *iface)
{ (void)iface; }

static D3DRESOURCETYPE WINAPI texture_get_type(IDirect3DTexture9 *iface)
{ (void)iface; return D3DRTYPE_TEXTURE; }

static DWORD WINAPI texture_set_lod(IDirect3DTexture9 *iface, DWORD lod)
{
    D9Texture *texture = texture_from_iface(iface);
    DWORD old = texture->lod;
    if (lod >= texture->level_count)
        lod = texture->level_count - 1;
    texture->lod = lod;
    return old;
}

static DWORD WINAPI texture_get_lod(IDirect3DTexture9 *iface)
{ return texture_from_iface(iface)->lod; }

static DWORD WINAPI texture_get_level_count(IDirect3DTexture9 *iface)
{ return texture_from_iface(iface)->level_count; }

/* Auto mipmap generation is not implemented in M1 (CreateTexture already
 * rejects D3DUSAGE_AUTOGENMIPMAP); these three exist only to keep the vtable
 * complete for titles that probe the capability defensively. */
static HRESULT WINAPI texture_set_auto_gen_filter_type(
        IDirect3DTexture9 *iface, D3DTEXTUREFILTERTYPE filter)
{ (void)iface; (void)filter; return D3DERR_INVALIDCALL; }

static D3DTEXTUREFILTERTYPE WINAPI texture_get_auto_gen_filter_type(
        IDirect3DTexture9 *iface)
{ (void)iface; return D3DTEXF_NONE; }

static void WINAPI texture_generate_mip_sublevels(IDirect3DTexture9 *iface)
{ (void)iface; }

static HRESULT WINAPI texture_get_level_desc(IDirect3DTexture9 *iface,
        UINT level, D3DSURFACE_DESC *desc)
{
    D9Texture *texture = texture_from_iface(iface);
    D9TextureLevel *level_data;
    if (!desc || level >= texture->level_count)
        return D3DERR_INVALIDCALL;
    level_data = &texture->levels[level];
    ZeroMemory(desc, sizeof(*desc));
    desc->Format = texture->format;
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = texture->usage;
    desc->Pool = texture->pool;
    desc->MultiSampleType = D3DMULTISAMPLE_NONE;
    desc->Width = level_data->width;
    desc->Height = level_data->height;
    return D3D_OK;
}

/*
 * Returns a surface view onto one texture level. Originally stubbed out on
 * the assumption that an app would always upload through
 * IDirect3DTexture9::LockRect directly -- Warcraft III does not: it takes the
 * level's surface and locks that, so failing here meant its textures were
 * created and bound but never received a single byte of pixel data (117
 * textures created, 0 uploads), rendering the whole scene black.
 *
 * The surface forwards Lock/Unlock to the same per-level shadow storage and
 * UPDATE_TEXTURE emitter the texture's own LockRect uses, so both upload
 * routes stay identical.
 *
 * D3D9 does not create a fresh surface per call: each level owns one surface
 * sub-object, returned by every GetSurfaceLevel for that level and kept alive
 * by the texture, with AddRef/Release forwarded to the texture's refcount.
 * Handing back a new self-owning surface each time instead is what crashed
 * Kart Rider -- it locks a level surface, releases its reference (legal, the
 * texture still owns the surface), then calls UnlockRect through the pointer
 * it kept.  Under the old code that Release freed the object, so UnlockRect
 * read a zeroed vtable and jumped through a null pointer.
 */
static HRESULT WINAPI texture_get_surface_level(IDirect3DTexture9 *iface,
        UINT level, IDirect3DSurface9 **surface_out)
{
    D9Texture *texture = texture_from_iface(iface);
    D9Surface *surface;

    TRACE_MARK_ENTER("Texture.GetSurfaceLevel");
    TRACE("ENTER Texture.GetSurfaceLevel texture=%08lX handle=%08lX "
            "ref=%ld level=%lu outarg=%08lX",
            (DWORD)(uintptr_t)iface, texture->handle,
            InterlockedCompareExchange(&texture->refcount, 0, 0), level,
            (DWORD)(uintptr_t)surface_out);
    if (!surface_out) {
        TRACE_MARK_EXIT("Texture.GetSurfaceLevel", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    *surface_out = NULL;
    if (level >= texture->level_count) {
        TRACE_MARK_EXIT("Texture.GetSurfaceLevel", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    surface = texture->levels[level].level_surface;
    if (!surface) {
        surface = (D9Surface *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                sizeof(*surface));
        if (!surface) {
            TRACE_MARK_EXIT("Texture.GetSurfaceLevel", E_OUTOFMEMORY, NULL);
            return E_OUTOFMEMORY;
        }
        surface->iface.lpVtbl = &g_surface_vtbl;
        /* Unused while texture_child is set: the texture's refcount is this
         * surface's refcount. */
        surface->refcount = 0;
        surface->device = texture->device;
        surface->texture = texture;
        surface->texture_child = TRUE;
        surface->level = level;
        surface->width = texture->levels[level].width;
        surface->height = texture->levels[level].height;
        surface->format = texture->format;
        texture->levels[level].level_surface = surface;
        TRACE_REGISTER_RANGE("SURFACE_TEXTURE_LEVEL", level, surface, surface,
                sizeof(*surface));
        TRACE("SURFACE CREATE kind=texture_level object=%08lX vtbl=%08lX "
                "device=%08lX texture=%08lX texture_handle=%08lX "
                "swapchain=00000000 shadow=%08lX level=%lu size=%lux%lu "
                "format=%08lX",
                (DWORD)(uintptr_t)&surface->iface,
                (DWORD)(uintptr_t)surface->iface.lpVtbl,
                (DWORD)(uintptr_t)&texture->device->iface,
                (DWORD)(uintptr_t)&texture->iface, texture->handle,
                (DWORD)(uintptr_t)texture->levels[level].shadow, level,
                surface->width, surface->height, (DWORD)surface->format);
    }
    /* The reference handed to the caller is a reference on the texture. */
    IDirect3DTexture9_AddRef(iface);
    *surface_out = &surface->iface;
    TRACE("SURFACE HANDOUT kind=texture_level object=%08lX texture=%08lX "
            "texture_handle=%08lX level=%lu texture_refs=%ld",
            (DWORD)(uintptr_t)*surface_out, (DWORD)(uintptr_t)&texture->iface,
            texture->handle, level,
            InterlockedCompareExchange(&texture->refcount, 0, 0));
    TRACE_MARK_EXIT("Texture.GetSurfaceLevel", D3D_OK, *surface_out);
    return D3D_OK;
}

static HRESULT WINAPI texture_lock_rect(IDirect3DTexture9 *iface, UINT level,
        D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags)
{
    return texture_lock_level(texture_from_iface(iface), level, locked_rect,
            rect, flags);
}

static HRESULT WINAPI texture_unlock_rect(IDirect3DTexture9 *iface,
        UINT level)
{
    return texture_unlock_level(texture_from_iface(iface), level);
}

static HRESULT WINAPI texture_add_dirty_rect(IDirect3DTexture9 *iface,
        const RECT *rect)
{
    D9Texture *texture = texture_from_iface(iface);
    if (rect && (rect->left < 0 || rect->top < 0
            || rect->right <= rect->left || rect->bottom <= rect->top
            || (UINT)rect->right > texture->width
            || (UINT)rect->bottom > texture->height))
        return D3DERR_INVALIDCALL;
    return D3D_OK;
}

/* ---- IDirect3DVertexDeclaration9 ---- */

static HRESULT WINAPI decl_query_interface(IDirect3DVertexDeclaration9 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DVertexDeclaration9)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DVertexDeclaration9_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI decl_add_ref(IDirect3DVertexDeclaration9 *iface)
{
    return (ULONG)InterlockedIncrement(&decl_from_iface(iface)->refcount);
}

static ULONG WINAPI decl_release(IDirect3DVertexDeclaration9 *iface)
{
    D9VertexDeclaration *decl = decl_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&decl->refcount);
    if (!refs) {
        D9VertexDeclaration **link = &decl->device->vertex_declarations;
        D9WGDestroyResource destroy;
        while (*link && *link != decl)
            link = &(*link)->next_device_resource;
        if (*link) *link = decl->next_device_resource;
        destroy.resource_handle = decl->handle;
        destroy.resource_kind = D9WG_RESOURCE_VERTEX_DECLARATION;
        emit_command(D9WG_OP_DESTROY_RESOURCE, &destroy, sizeof(destroy));
        device_child_release(decl->device);
        HeapFree(GetProcessHeap(), 0, decl);
    }
    return refs;
}

static HRESULT WINAPI decl_get_device(IDirect3DVertexDeclaration9 *iface,
        IDirect3DDevice9 **device_out)
{
    D9VertexDeclaration *decl = decl_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &decl->device->iface;
    IDirect3DDevice9_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI decl_get_declaration(IDirect3DVertexDeclaration9 *iface,
        D3DVERTEXELEMENT9 *elements, UINT *count_out)
{
    D9VertexDeclaration *decl = decl_from_iface(iface);
    if (!count_out)
        return D3DERR_INVALIDCALL;
    if (!elements) {
        *count_out = decl->element_count + 1; /* +1 for the END() sentinel */
        return D3D_OK;
    }
    if (*count_out < decl->element_count + 1)
        return D3DERR_INVALIDCALL;
    CopyMemory(elements, decl->elements,
            decl->element_count * sizeof(D3DVERTEXELEMENT9));
    {
        D3DVERTEXELEMENT9 end = D3DDECL_END();
        elements[decl->element_count] = end;
    }
    *count_out = decl->element_count + 1;
    return D3D_OK;
}

/* ---- IDirect3DVertexShader9 / IDirect3DPixelShader9 ----
 *
 * Both interfaces are IUnknown + GetDevice + GetFunction with identical
 * layouts, so the bodies below are shared through D9Shader and only the
 * vtable entry points differ. GetFunction hands back the shadow copy taken
 * at creation, which is what a game's own shader-cache or effect-reload path
 * reads back. */

static HRESULT shader_query_interface(D9Shader *shader, REFIID iid,
        void **object, const void *interface_guid)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid) && !guid_equal(iid, interface_guid)))
        return E_NOINTERFACE;
    *object = &shader->iface;
    InterlockedIncrement(&shader->refcount);
    return S_OK;
}

static HRESULT shader_get_function(D9Shader *shader, void *data, UINT *size)
{
    UINT byte_count = shader->token_count * 4u;
    if (!size)
        return D3DERR_INVALIDCALL;
    if (!data) {
        *size = byte_count;
        return D3D_OK;
    }
    if (*size < byte_count)
        return D3DERR_INVALIDCALL;
    CopyMemory(data, shader->code, byte_count);
    *size = byte_count;
    return D3D_OK;
}

static HRESULT WINAPI vertex_shader_query_interface(
        IDirect3DVertexShader9 *iface, REFIID iid, void **object)
{
    return shader_query_interface(vertex_shader_from_iface(iface), iid, object,
            &IID_IDirect3DVertexShader9);
}

static ULONG WINAPI vertex_shader_add_ref(IDirect3DVertexShader9 *iface)
{
    return (ULONG)InterlockedIncrement(
            &vertex_shader_from_iface(iface)->refcount);
}

static ULONG WINAPI vertex_shader_release(IDirect3DVertexShader9 *iface)
{
    D9Shader *shader = vertex_shader_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&shader->refcount);
    if (!refs)
        shader_destroy(shader);
    return refs;
}

static HRESULT WINAPI vertex_shader_get_device(IDirect3DVertexShader9 *iface,
        IDirect3DDevice9 **device_out)
{
    D9Shader *shader = vertex_shader_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &shader->device->iface;
    IDirect3DDevice9_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI vertex_shader_get_function(IDirect3DVertexShader9 *iface,
        void *data, UINT *size)
{
    return shader_get_function(vertex_shader_from_iface(iface), data, size);
}

static HRESULT WINAPI pixel_shader_query_interface(
        IDirect3DPixelShader9 *iface, REFIID iid, void **object)
{
    return shader_query_interface(pixel_shader_from_iface(iface), iid, object,
            &IID_IDirect3DPixelShader9);
}

static ULONG WINAPI pixel_shader_add_ref(IDirect3DPixelShader9 *iface)
{
    return (ULONG)InterlockedIncrement(
            &pixel_shader_from_iface(iface)->refcount);
}

static ULONG WINAPI pixel_shader_release(IDirect3DPixelShader9 *iface)
{
    D9Shader *shader = pixel_shader_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&shader->refcount);
    if (!refs)
        shader_destroy(shader);
    return refs;
}

static HRESULT WINAPI pixel_shader_get_device(IDirect3DPixelShader9 *iface,
        IDirect3DDevice9 **device_out)
{
    D9Shader *shader = pixel_shader_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &shader->device->iface;
    IDirect3DDevice9_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI pixel_shader_get_function(IDirect3DPixelShader9 *iface,
        void *data, UINT *size)
{
    return shader_get_function(pixel_shader_from_iface(iface), data, size);
}

/* ---- IDirect3DSurface9 (GetBackBuffer only; see the struct comment) ---- */

static HRESULT WINAPI surface_query_interface(IDirect3DSurface9 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DResource9)
            && !guid_equal(iid, &IID_IDirect3DSurface9)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DSurface9_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI surface_add_ref(IDirect3DSurface9 *iface)
{
    D9Surface *surface = surface_from_iface(iface);
    LONG before = 0;
    ULONG refs;

    /* A texture level surface has no refcount of its own: it is a sub-object
     * of the texture and shares its parent's, so that dropping the last
     * surface reference cannot outlive-check the pointer the app kept. */
    if (surface->texture_child) {
        TRACE_MARK_ENTER("Surface.AddRef");
        refs = IDirect3DTexture9_AddRef(&surface->texture->iface);
        TRACE("SURFACE ADDREF object=%08lX forwarded_to_texture=%08lX "
                "level=%lu texture_refs=%lu", (DWORD)(uintptr_t)iface,
                (DWORD)(uintptr_t)&surface->texture->iface, surface->level,
                refs);
        TRACE_MARK_EXIT("Surface.AddRef", (HRESULT)refs, iface);
        return refs;
    }
    /* The implicit back buffer and auto depth-stencil are sub-objects of the
     * device in the same way, so each public reference on them is backed by one
     * device reference -- that is what stops a device from being torn down
     * while the app still holds one of its implicit surfaces. */
    if (surface_is_implicit(surface)) {
        TRACE_MARK_ENTER("Surface.AddRef");
        refs = (ULONG)InterlockedIncrement(&surface->refcount);
        device_child_add_ref(surface->device);
        TRACE("SURFACE ADDREF object=%08lX implicit=1 after=%lu device=%08lX "
                "device_refs=%ld", (DWORD)(uintptr_t)iface, refs,
                (DWORD)(uintptr_t)surface->device,
                InterlockedCompareExchange(&surface->device->refcount, 0, 0));
        TRACE_MARK_EXIT("Surface.AddRef", (HRESULT)refs, iface);
        return refs;
    }
#ifdef D9WG_DIAGNOSTIC_TRACE
    before = InterlockedCompareExchange(&surface->refcount, 0, 0);
#endif
    TRACE_MARK_ENTER("Surface.AddRef");
    refs = (ULONG)InterlockedIncrement(&surface->refcount);
    TRACE("SURFACE ADDREF object=%08lX vtbl=%08lX before=%ld after=%lu "
            "texture=%08lX shadow=%08lX level=%lu",
            (DWORD)(uintptr_t)iface,
            (DWORD)(uintptr_t)surface->iface.lpVtbl, before, refs,
            (DWORD)(uintptr_t)surface->texture,
            (DWORD)(uintptr_t)surface->shadow, surface->level);
    (void)before;
    TRACE_MARK_EXIT("Surface.AddRef", (HRESULT)refs, iface);
    return refs;
}

static ULONG WINAPI surface_release(IDirect3DSurface9 *iface)
{
    D9Surface *surface = surface_from_iface(iface);
    LONG before = 0;
    ULONG refs;

    /* Mirror of surface_add_ref: the release lands on the texture, and the
     * surface object itself survives until the texture is destroyed.  This is
     * the whole point of the sub-object model -- the app is allowed to release
     * its surface reference and go on using the pointer. */
    if (surface->texture_child) {
        /* Read everything the trace needs first: this release can be the
         * texture's last, and the texture's teardown frees this surface. */
        D9Texture *texture = surface->texture;
        UINT level = surface->level;

        TRACE_MARK_ENTER("Surface.Release");
        refs = IDirect3DTexture9_Release(&texture->iface);
        TRACE("SURFACE RELEASE object=%08lX forwarded_to_texture=%08lX "
                "level=%lu texture_refs=%lu", (DWORD)(uintptr_t)iface,
                (DWORD)(uintptr_t)texture, level, refs);
        (void)level;
        TRACE_MARK_EXIT("Surface.Release", (HRESULT)refs, iface);
        return refs;
    }
    /* Mirror of the implicit branch in surface_add_ref. The object is never
     * freed here: it belongs to the device and is released with it, which is
     * precisely what lets the app go on using the pointer -- and what stops the
     * heap from handing this block out as some unrelated resource. */
    if (surface_is_implicit(surface)) {
        D9Device *device = surface->device;

        TRACE_MARK_ENTER("Surface.Release");
        refs = (ULONG)InterlockedDecrement(&surface->refcount);
        TRACE("SURFACE RELEASE object=%08lX implicit=1 after=%lu device=%08lX "
                "device_refs_before=%ld", (DWORD)(uintptr_t)iface, refs,
                (DWORD)(uintptr_t)device,
                InterlockedCompareExchange(&device->refcount, 0, 0));
        TRACE_MARK_EXIT("Surface.Release", (HRESULT)refs, iface);
        /* This can destroy the device, which frees `surface`; touch neither
         * object afterwards. */
        device_child_release(device);
        return refs;
    }
#ifdef D9WG_DIAGNOSTIC_TRACE
    before = InterlockedCompareExchange(&surface->refcount, 0, 0);
#endif
    TRACE_MARK_ENTER("Surface.Release");
    refs = (ULONG)InterlockedDecrement(&surface->refcount);
    TRACE("SURFACE RELEASE object=%08lX vtbl=%08lX before=%ld after=%lu "
            "device=%08lX texture=%08lX swapchain=%08lX shadow=%08lX "
            "level=%lu locked=%lu",
            (DWORD)(uintptr_t)iface,
            (DWORD)(uintptr_t)surface->iface.lpVtbl, before, refs,
            (DWORD)(uintptr_t)surface->device,
            (DWORD)(uintptr_t)surface->texture,
            (DWORD)(uintptr_t)surface->swap_chain,
            (DWORD)(uintptr_t)surface->shadow, surface->level,
            (DWORD)surface->locked);
    (void)before;
    if (!refs) {
        D9Device *device = surface->device;
        D9Texture *texture = surface->texture;
        BYTE *shadow = surface->shadow;

        TRACE("SURFACE FREE_BEGIN object=%08lX vtbl=%08lX device=%08lX "
                "texture=%08lX swapchain=%08lX shadow=%08lX level=%lu "
                "size=%lux%lu format=%08lX locked=%lu",
                (DWORD)(uintptr_t)iface,
                (DWORD)(uintptr_t)surface->iface.lpVtbl,
                (DWORD)(uintptr_t)device, (DWORD)(uintptr_t)texture,
                (DWORD)(uintptr_t)surface->swap_chain,
                (DWORD)(uintptr_t)shadow, surface->level, surface->width,
                surface->height, (DWORD)surface->format,
                (DWORD)surface->locked);
        /* Only a create_target_texture surface arrives here with a texture,
         * and it holds that texture's sole reference, so the texture goes with
         * it.  Texture level surfaces never reach this path at all: they are
         * sub-objects freed by texture_release.  The back-buffer, offscreen
         * and auto-depth surfaces have no texture and only hold the device. */
        if (texture)
            IDirect3DTexture9_Release(&texture->iface);
        if (shadow)
            HeapFree(GetProcessHeap(), 0, shadow);
        device_child_release(device);
        TRACE("SURFACE FREE_END object=%08lX", (DWORD)(uintptr_t)iface);
        TRACE_MARK_EXIT("Surface.Release", 0, iface);
        TRACE_SURFACE_FREED(iface);
        HeapFree(GetProcessHeap(), 0, surface);
        return 0;
    }
    TRACE_MARK_EXIT("Surface.Release", (HRESULT)refs, iface);
    return refs;
}

static HRESULT WINAPI surface_get_device(IDirect3DSurface9 *iface,
        IDirect3DDevice9 **device_out)
{
    D9Surface *surface = surface_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &surface->device->iface;
    IDirect3DDevice9_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI surface_set_private_data(IDirect3DSurface9 *iface,
        REFGUID guid, const void *data, DWORD size, DWORD flags)
{ (void)iface; (void)guid; (void)data; (void)size; (void)flags;
  return D3DERR_INVALIDCALL; }

static HRESULT WINAPI surface_get_private_data(IDirect3DSurface9 *iface,
        REFGUID guid, void *data, DWORD *size)
{ (void)iface; (void)guid; (void)data; (void)size; return D3DERR_NOTFOUND; }

static HRESULT WINAPI surface_free_private_data(IDirect3DSurface9 *iface,
        REFGUID guid)
{ (void)iface; (void)guid; return D3DERR_NOTFOUND; }

static DWORD WINAPI surface_set_priority(IDirect3DSurface9 *iface,
        DWORD priority)
{ (void)iface; (void)priority; return 0; }

static DWORD WINAPI surface_get_priority(IDirect3DSurface9 *iface)
{ (void)iface; return 0; }

static void WINAPI surface_preload(IDirect3DSurface9 *iface)
{ (void)iface; }

static D3DRESOURCETYPE WINAPI surface_get_type(IDirect3DSurface9 *iface)
{ (void)iface; return D3DRTYPE_SURFACE; }

static HRESULT WINAPI surface_get_container(IDirect3DSurface9 *iface,
        REFIID riid, void **container)
{
    D9Surface *surface = surface_from_iface(iface);
    if (!container)
        return D3DERR_INVALIDCALL;
    *container = NULL;
    if (surface->swap_chain) {
        if (!riid || (!iid_is_unknown(riid)
                && !guid_equal(riid, &IID_IDirect3DSwapChain9)))
            return E_NOINTERFACE;
        *container = &surface->swap_chain->iface;
        IDirect3DSwapChain9_AddRef(&surface->swap_chain->iface);
        TRACE("OK Surface.GetContainer surface=%08lX swapchain=%08lX",
                (DWORD)(uintptr_t)iface, (DWORD)(uintptr_t)*container);
        return D3D_OK;
    }
    if (!surface->texture)
        return D3DERR_INVALIDCALL;
    if (riid && !iid_is_unknown(riid)
            && !guid_equal(riid, &IID_IDirect3DResource9)
            && !guid_equal(riid, &IID_IDirect3DBaseTexture9)
            && !guid_equal(riid, &IID_IDirect3DTexture9))
        return E_NOINTERFACE;
    *container = &surface->texture->iface;
    IDirect3DTexture9_AddRef(&surface->texture->iface);
    return D3D_OK;
}

static HRESULT WINAPI surface_get_desc(IDirect3DSurface9 *iface,
        D3DSURFACE_DESC *desc)
{
    D9Surface *surface = surface_from_iface(iface);
    if (!desc)
        return D3DERR_INVALIDCALL;
    ZeroMemory(desc, sizeof(*desc));
    desc->Format = surface->format;
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = surface->swap_chain ? D3DUSAGE_RENDERTARGET : 0;
    desc->Pool = D3DPOOL_DEFAULT;
    desc->MultiSampleType = D3DMULTISAMPLE_NONE;
    desc->Width = surface->width;
    desc->Height = surface->height;
    return D3D_OK;
}

/*
 * A texture-level surface locks the very same shadow storage as
 * IDirect3DTexture9::LockRect on that level, so an app can upload through
 * either route and the UPDATE_TEXTURE emitted on unlock is identical.
 *
 * The back-buffer surface (texture == NULL) still fails honestly: M1 has no
 * GPU-backed readback (plan section 2.2 non-goal), and fabricating pixels
 * would be worse than saying so.
 */
static HRESULT WINAPI surface_lock_rect(IDirect3DSurface9 *iface,
        D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags)
{
    D9Surface *surface = surface_from_iface(iface);
    HRESULT result;

    TRACE_MARK_ENTER("Surface.LockRect");
    TRACE("ENTER Surface.LockRect object=%08lX ref=%ld texture=%08lX "
            "shadow=%08lX level=%lu locked=%lu rect=%08lX flags=%08lX",
            (DWORD)(uintptr_t)iface,
            InterlockedCompareExchange(&surface->refcount, 0, 0),
            (DWORD)(uintptr_t)surface->texture,
            (DWORD)(uintptr_t)surface->shadow, surface->level,
            (DWORD)surface->locked, (DWORD)(uintptr_t)rect, flags);

    if (surface->shadow) {
        /* A standalone CPU surface. The whole surface is handed over
         * regardless of `rect`: there is no upload to narrow, so a sub-rect
         * lock only changes which pointer the app is given. */
        UINT x = rect ? (UINT)rect->left : 0;
        UINT y = rect ? (UINT)rect->top : 0;
        if (!locked_rect || surface->locked) {
            result = D3DERR_INVALIDCALL;
            goto done;
        }
        if (x >= surface->width || y >= surface->height) {
            result = D3DERR_INVALIDCALL;
            goto done;
        }
        locked_rect->Pitch = (INT)surface->row_pitch;
        locked_rect->pBits = surface->shadow + y * surface->row_pitch + x * 4u;
        surface->locked = TRUE;
        (void)flags;
        result = D3D_OK;
        goto done;
    }
    if (!surface->texture)
        result = D3DERR_INVALIDCALL;
    else
        result = texture_lock_level(surface->texture, surface->level,
                locked_rect, rect, flags);
done:
    TRACE("LEAVE Surface.LockRect object=%08lX hr=%08lX pBits=%08lX "
            "pitch=%ld", (DWORD)(uintptr_t)iface, (DWORD)result,
            (DWORD)(uintptr_t)(SUCCEEDED(result) && locked_rect
                    ? locked_rect->pBits : NULL),
            SUCCEEDED(result) && locked_rect ? locked_rect->Pitch : 0);
    TRACE_MARK_EXIT("Surface.LockRect", result,
            SUCCEEDED(result) && locked_rect ? locked_rect->pBits : NULL);
    return result;
}

static HRESULT WINAPI surface_unlock_rect(IDirect3DSurface9 *iface)
{
    D9Surface *surface = surface_from_iface(iface);
    HRESULT result;

    TRACE_MARK_ENTER("Surface.UnlockRect");
    TRACE("ENTER Surface.UnlockRect object=%08lX ref=%ld vtbl=%08lX "
            "texture=%08lX shadow=%08lX level=%lu locked=%lu",
            (DWORD)(uintptr_t)iface,
            InterlockedCompareExchange(&surface->refcount, 0, 0),
            (DWORD)(uintptr_t)surface->iface.lpVtbl,
            (DWORD)(uintptr_t)surface->texture,
            (DWORD)(uintptr_t)surface->shadow, surface->level,
            (DWORD)surface->locked);
    if (surface->shadow) {
        if (!surface->locked)
            result = D3DERR_INVALIDCALL;
        else {
            surface->locked = FALSE;
            result = D3D_OK;
        }
    } else if (!surface->texture) {
        result = D3DERR_INVALIDCALL;
    } else {
        result = texture_unlock_level(surface->texture, surface->level);
    }
    TRACE("LEAVE Surface.UnlockRect object=%08lX hr=%08lX locked=%lu",
            (DWORD)(uintptr_t)iface, (DWORD)result,
            (DWORD)surface->locked);
    TRACE_MARK_EXIT("Surface.UnlockRect", result, iface);
    return result;
}

/* GetDC/ReleaseDC is how an app draws GDI content -- text, a loaded bitmap --
 * straight onto a D3D surface, so a refusal here is a whole class of 2D art
 * silently never arriving. Named rather than merely refused, for the reason
 * given above note_unsupported(). */
static HRESULT WINAPI surface_get_dc(IDirect3DSurface9 *iface, HDC *hdc)
{ (void)iface; if (hdc) { *hdc = NULL; }
  UNSUPPORTED("Surface.GetDC"); return D3DERR_INVALIDCALL; }

static HRESULT WINAPI surface_release_dc(IDirect3DSurface9 *iface, HDC hdc)
{ (void)iface; (void)hdc;
  UNSUPPORTED("Surface.ReleaseDC"); return D3DERR_INVALIDCALL; }

/* ---- IDirect3DSwapChain9: the device's implicit primary chain ---- */

static HRESULT WINAPI swap_chain_query_interface(IDirect3DSwapChain9 *iface,
        REFIID iid, void **object)
{
    TRACE_MARK_ENTER("SwapChain.QueryInterface");
    if (!object) {
        TRACE_MARK_EXIT("SwapChain.QueryInterface", E_POINTER, NULL);
        return E_POINTER;
    }
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DSwapChain9))) {
        TRACE_MARK_EXIT("SwapChain.QueryInterface", E_NOINTERFACE, NULL);
        return E_NOINTERFACE;
    }
    *object = iface;
    IDirect3DSwapChain9_AddRef(iface);
    TRACE_MARK_EXIT("SwapChain.QueryInterface", S_OK, iface);
    return S_OK;
}

static ULONG WINAPI swap_chain_add_ref(IDirect3DSwapChain9 *iface)
{
    D9SwapChain *swap_chain = swap_chain_from_iface(iface);
    ULONG refs;

    TRACE_MARK_ENTER("SwapChain.AddRef");
    /* A live swap-chain pointer must keep its embedded storage alive. */
    IDirect3DDevice9_AddRef(&swap_chain->device->iface);
    refs = (ULONG)InterlockedIncrement(&swap_chain->refcount);
    TRACE("CALL SwapChain.AddRef object=%08lX refs=%lu device_refs=%ld",
            (DWORD)(uintptr_t)iface, refs,
            InterlockedCompareExchange(&swap_chain->device->refcount, 0, 0));
    TRACE_MARK_EXIT("SwapChain.AddRef", (HRESULT)refs, iface);
    return refs;
}

static ULONG WINAPI swap_chain_release(IDirect3DSwapChain9 *iface)
{
    D9SwapChain *swap_chain = swap_chain_from_iface(iface);
    D9Device *device = swap_chain->device;
    ULONG device_refs;
    ULONG refs;

    TRACE_MARK_ENTER("SwapChain.Release");
    refs = (ULONG)InterlockedDecrement(&swap_chain->refcount);
    TRACE("CALL SwapChain.Release object=%08lX refs=%lu device_refs_before=%ld",
            (DWORD)(uintptr_t)iface, refs,
            InterlockedCompareExchange(&device->refcount, 0, 0));
    /* This can free device (and therefore swap_chain); do not touch either
     * object after the call. */
    device_refs = IDirect3DDevice9_Release(&device->iface);
    TRACE_MARK_EXIT("SwapChain.Release", (HRESULT)refs, NULL);
    TRACE("LEAVE SwapChain.Release object=%08lX refs=%lu device_refs=%lu",
            (DWORD)(uintptr_t)iface, refs, device_refs);
    (void)device_refs;
    return refs;
}

static HRESULT WINAPI swap_chain_present(IDirect3DSwapChain9 *iface,
        const RECT *src_rect, const RECT *dst_rect, HWND override_window,
        const RGNDATA *dirty_region, DWORD flags)
{
    D9SwapChain *swap_chain = swap_chain_from_iface(iface);
    HRESULT result;

    TRACE_MARK_ENTER("SwapChain.Present");
    TRACE("CALL SwapChain.Present object=%08lX flags=%08lX override=%08lX",
            (DWORD)(uintptr_t)iface, flags,
            (DWORD)(uintptr_t)override_window);
    (void)flags;
    /* The bridge presents synchronously, so DONOTWAIT cannot make it return
     * WASSTILLDRAWING.  LINEAR_CONTENT does not change the guest-side work;
     * both flags can otherwise use the device's normal presentation path. */
    result = device_present(&swap_chain->device->iface, src_rect, dst_rect,
            override_window, dirty_region);
    TRACE("%s SwapChain.Present object=%08lX -> %08lX",
            SUCCEEDED(result) ? "OK" : "FAIL", (DWORD)(uintptr_t)iface,
            (DWORD)result);
    TRACE_MARK_EXIT("SwapChain.Present", result, NULL);
    return result;
}

static HRESULT WINAPI swap_chain_get_front_buffer_data(
        IDirect3DSwapChain9 *iface, IDirect3DSurface9 *destination)
{
    D9SwapChain *swap_chain = swap_chain_from_iface(iface);
    HRESULT result;

    TRACE_MARK_ENTER("SwapChain.GetFrontBufferData");
    result = device_get_front_buffer_data(&swap_chain->device->iface, 0,
            destination);
    TRACE("%s SwapChain.GetFrontBufferData object=%08lX dst=%08lX -> %08lX",
            SUCCEEDED(result) ? "OK" : "FAIL", (DWORD)(uintptr_t)iface,
            (DWORD)(uintptr_t)destination, (DWORD)result);
    TRACE_MARK_EXIT("SwapChain.GetFrontBufferData", result, destination);
    return result;
}

static HRESULT WINAPI swap_chain_get_back_buffer(IDirect3DSwapChain9 *iface,
        UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **out)
{
    D9SwapChain *swap_chain = swap_chain_from_iface(iface);
    HRESULT result;

    TRACE_MARK_ENTER("SwapChain.GetBackBuffer");
    result = device_get_back_buffer(&swap_chain->device->iface, 0, index,
            type, out);
    TRACE("%s SwapChain.GetBackBuffer object=%08lX index=%lu type=%lu out=%08lX -> %08lX",
            SUCCEEDED(result) ? "OK" : "FAIL", (DWORD)(uintptr_t)iface,
            index, (DWORD)type,
            (DWORD)(uintptr_t)(out ? *out : NULL), (DWORD)result);
    TRACE_MARK_EXIT("SwapChain.GetBackBuffer", result, out ? *out : NULL);
    return result;
}

static HRESULT WINAPI swap_chain_get_raster_status(IDirect3DSwapChain9 *iface,
        D3DRASTER_STATUS *status)
{
    D9SwapChain *swap_chain = swap_chain_from_iface(iface);
    HRESULT result;

    TRACE_MARK_ENTER("SwapChain.GetRasterStatus");
    result = device_get_raster_status(&swap_chain->device->iface, 0, status);
    TRACE("%s SwapChain.GetRasterStatus object=%08lX -> %08lX",
            SUCCEEDED(result) ? "OK" : "FAIL", (DWORD)(uintptr_t)iface,
            (DWORD)result);
    TRACE_MARK_EXIT("SwapChain.GetRasterStatus", result, status);
    return result;
}

static HRESULT WINAPI swap_chain_get_display_mode(IDirect3DSwapChain9 *iface,
        D3DDISPLAYMODE *mode)
{
    D9SwapChain *swap_chain = swap_chain_from_iface(iface);

    TRACE_MARK_ENTER("SwapChain.GetDisplayMode");
    if (!mode) {
        TRACE("FAIL SwapChain.GetDisplayMode object=%08lX missing output -> %08lX",
                (DWORD)(uintptr_t)iface, (DWORD)D3DERR_INVALIDCALL);
        TRACE_MARK_EXIT("SwapChain.GetDisplayMode", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    *mode = swap_chain->device->display_mode;
    TRACE("OK SwapChain.GetDisplayMode object=%08lX %lux%lu fmt=%08lX refresh=%lu",
            (DWORD)(uintptr_t)iface, mode->Width, mode->Height,
            (DWORD)mode->Format, mode->RefreshRate);
    TRACE_MARK_EXIT("SwapChain.GetDisplayMode", D3D_OK, mode);
    return D3D_OK;
}

static HRESULT WINAPI swap_chain_get_device(IDirect3DSwapChain9 *iface,
        IDirect3DDevice9 **out)
{
    D9SwapChain *swap_chain = swap_chain_from_iface(iface);

    TRACE_MARK_ENTER("SwapChain.GetDevice");
    if (!out) {
        TRACE("FAIL SwapChain.GetDevice object=%08lX missing output -> %08lX",
                (DWORD)(uintptr_t)iface, (DWORD)D3DERR_INVALIDCALL);
        TRACE_MARK_EXIT("SwapChain.GetDevice", D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    *out = &swap_chain->device->iface;
    IDirect3DDevice9_AddRef(*out);
    TRACE("OK SwapChain.GetDevice object=%08lX device=%08lX refs=%ld",
            (DWORD)(uintptr_t)iface, (DWORD)(uintptr_t)*out,
            InterlockedCompareExchange(&swap_chain->device->refcount, 0, 0));
    TRACE_MARK_EXIT("SwapChain.GetDevice", D3D_OK, *out);
    return D3D_OK;
}

static HRESULT WINAPI swap_chain_get_present_parameters(
        IDirect3DSwapChain9 *iface, D3DPRESENT_PARAMETERS *parameters)
{
    D9SwapChain *swap_chain = swap_chain_from_iface(iface);

    TRACE_MARK_ENTER("SwapChain.GetPresentParameters");
    if (!parameters) {
        TRACE("FAIL SwapChain.GetPresentParameters object=%08lX missing output -> %08lX",
                (DWORD)(uintptr_t)iface, (DWORD)D3DERR_INVALIDCALL);
        TRACE_MARK_EXIT("SwapChain.GetPresentParameters",
                D3DERR_INVALIDCALL, NULL);
        return D3DERR_INVALIDCALL;
    }
    *parameters = swap_chain->device->present;
    TRACE("OK SwapChain.GetPresentParameters object=%08lX %lux%lu fmt=%08lX count=%lu windowed=%lu",
            (DWORD)(uintptr_t)iface, parameters->BackBufferWidth,
            parameters->BackBufferHeight, (DWORD)parameters->BackBufferFormat,
            parameters->BackBufferCount, (DWORD)parameters->Windowed);
    TRACE_MARK_EXIT("SwapChain.GetPresentParameters", D3D_OK, parameters);
    return D3D_OK;
}

/* ---- vtables ---- */

static IDirect3D9Vtbl g_d3d_vtbl = {
    .QueryInterface = d3d_query_interface,
    .AddRef = d3d_add_ref,
    .Release = d3d_release,
    .RegisterSoftwareDevice = d3d_register_software_device,
    .GetAdapterCount = d3d_get_adapter_count,
    .GetAdapterIdentifier = d3d_get_adapter_identifier,
    .GetAdapterModeCount = d3d_get_adapter_mode_count,
    .EnumAdapterModes = d3d_enum_adapter_modes,
    .GetAdapterDisplayMode = d3d_get_adapter_display_mode,
    .CheckDeviceType = d3d_check_device_type,
    .CheckDeviceFormat = d3d_check_device_format,
    .CheckDeviceMultiSampleType = d3d_check_multisample,
    .CheckDepthStencilMatch = d3d_check_depth_stencil,
    .CheckDeviceFormatConversion = d3d_check_device_format_conversion,
    .GetDeviceCaps = d3d_get_device_caps,
    .GetAdapterMonitor = d3d_get_adapter_monitor,
    .CreateDevice = d3d_create_device
};

static IDirect3DSwapChain9Vtbl g_swap_chain_vtbl = {
    .QueryInterface = swap_chain_query_interface,
    .AddRef = swap_chain_add_ref,
    .Release = swap_chain_release,
    .Present = swap_chain_present,
    .GetFrontBufferData = swap_chain_get_front_buffer_data,
    .GetBackBuffer = swap_chain_get_back_buffer,
    .GetRasterStatus = swap_chain_get_raster_status,
    .GetDisplayMode = swap_chain_get_display_mode,
    .GetDevice = swap_chain_get_device,
    .GetPresentParameters = swap_chain_get_present_parameters
};

static IDirect3DDevice9Vtbl g_device_vtbl = {
    .QueryInterface = device_query_interface,
    .AddRef = device_add_ref,
    .Release = device_release,
    .TestCooperativeLevel = device_test_cooperative_level,
    .GetAvailableTextureMem = device_get_available_texture_mem,
    .EvictManagedResources = device_evict_managed_resources,
    .GetDirect3D = device_get_direct3d,
    .GetDeviceCaps = device_get_caps,
    .GetDisplayMode = device_get_display_mode,
    .GetCreationParameters = device_get_creation_parameters,
    .SetCursorProperties = device_set_cursor_properties,
    .SetCursorPosition = device_set_cursor_position,
    .ShowCursor = device_show_cursor,
    .CreateAdditionalSwapChain = device_create_additional_swap_chain,
    .GetSwapChain = device_get_swap_chain,
    .GetNumberOfSwapChains = device_get_number_of_swap_chains,
    .Reset = device_reset,
    .Present = device_present,
    .GetBackBuffer = device_get_back_buffer,
    .GetRasterStatus = device_get_raster_status,
    .SetDialogBoxMode = device_set_dialog_box_mode,
    .SetGammaRamp = device_set_gamma_ramp,
    .GetGammaRamp = device_get_gamma_ramp,
    .CreateTexture = device_create_texture,
    .CreateVolumeTexture = device_create_volume_texture,
    .CreateCubeTexture = device_create_cube_texture,
    .CreateVertexBuffer = device_create_vertex_buffer,
    .CreateIndexBuffer = device_create_index_buffer,
    .CreateRenderTarget = device_create_render_target,
    .CreateDepthStencilSurface = device_create_depth_stencil_surface,
    .UpdateSurface = device_update_surface,
    .UpdateTexture = device_update_texture,
    .GetRenderTargetData = device_get_render_target_data,
    .GetFrontBufferData = device_get_front_buffer_data,
    .StretchRect = device_stretch_rect,
    .ColorFill = device_color_fill,
    .CreateOffscreenPlainSurface = device_create_offscreen_plain_surface,
    .SetRenderTarget = device_set_render_target,
    .GetRenderTarget = device_get_render_target,
    .SetDepthStencilSurface = device_set_depth_stencil_surface,
    .GetDepthStencilSurface = device_get_depth_stencil_surface,
    .BeginScene = device_begin_scene,
    .EndScene = device_end_scene,
    .Clear = device_clear,
    .SetTransform = device_set_transform,
    .GetTransform = device_get_transform,
    .MultiplyTransform = device_multiply_transform,
    .SetViewport = device_set_viewport,
    .GetViewport = device_get_viewport,
    .SetMaterial = device_set_material,
    .GetMaterial = device_get_material,
    .SetLight = device_set_light,
    .GetLight = device_get_light,
    .LightEnable = device_light_enable,
    .GetLightEnable = device_get_light_enable,
    .SetClipPlane = device_set_clip_plane,
    .GetClipPlane = device_get_clip_plane,
    .SetRenderState = device_set_render_state,
    .GetRenderState = device_get_render_state,
    .CreateStateBlock = device_create_state_block,
    .BeginStateBlock = device_begin_state_block,
    .EndStateBlock = device_end_state_block,
    .SetClipStatus = device_set_clip_status,
    .GetClipStatus = device_get_clip_status,
    .GetTexture = device_get_texture,
    .SetTexture = device_set_texture,
    .GetTextureStageState = device_get_texture_stage_state,
    .SetTextureStageState = device_set_texture_stage_state,
    .GetSamplerState = device_get_sampler_state,
    .SetSamplerState = device_set_sampler_state,
    .ValidateDevice = device_validate_device,
    .SetPaletteEntries = device_set_palette_entries,
    .GetPaletteEntries = device_get_palette_entries,
    .SetCurrentTexturePalette = device_set_current_texture_palette,
    .GetCurrentTexturePalette = device_get_current_texture_palette,
    .SetScissorRect = device_set_scissor_rect,
    .GetScissorRect = device_get_scissor_rect,
    .SetSoftwareVertexProcessing = device_set_software_vertex_processing,
    .GetSoftwareVertexProcessing = device_get_software_vertex_processing,
    .SetNPatchMode = device_set_npatch_mode,
    .GetNPatchMode = device_get_npatch_mode,
    .DrawPrimitive = device_draw_primitive,
    .DrawIndexedPrimitive = device_draw_indexed_primitive,
    .DrawPrimitiveUP = device_draw_primitive_up,
    .DrawIndexedPrimitiveUP = device_draw_indexed_primitive_up,
    .ProcessVertices = device_process_vertices,
    .CreateVertexDeclaration = device_create_vertex_declaration,
    .SetVertexDeclaration = device_set_vertex_declaration,
    .GetVertexDeclaration = device_get_vertex_declaration,
    .SetFVF = device_set_fvf,
    .GetFVF = device_get_fvf,
    .CreateVertexShader = device_create_vertex_shader,
    .SetVertexShader = device_set_vertex_shader,
    .GetVertexShader = device_get_vertex_shader,
    .SetVertexShaderConstantF = device_set_vertex_shader_constant_f,
    .GetVertexShaderConstantF = device_get_vertex_shader_constant_f,
    .SetVertexShaderConstantI = device_set_vertex_shader_constant_i,
    .GetVertexShaderConstantI = device_get_vertex_shader_constant_i,
    .SetVertexShaderConstantB = device_set_vertex_shader_constant_b,
    .GetVertexShaderConstantB = device_get_vertex_shader_constant_b,
    .SetStreamSource = device_set_stream_source,
    .GetStreamSource = device_get_stream_source,
    .SetStreamSourceFreq = device_set_stream_source_freq,
    .GetStreamSourceFreq = device_get_stream_source_freq,
    .SetIndices = device_set_indices,
    .GetIndices = device_get_indices,
    .CreatePixelShader = device_create_pixel_shader,
    .SetPixelShader = device_set_pixel_shader,
    .GetPixelShader = device_get_pixel_shader,
    .SetPixelShaderConstantF = device_set_pixel_shader_constant_f,
    .GetPixelShaderConstantF = device_get_pixel_shader_constant_f,
    .SetPixelShaderConstantI = device_set_pixel_shader_constant_i,
    .GetPixelShaderConstantI = device_get_pixel_shader_constant_i,
    .SetPixelShaderConstantB = device_set_pixel_shader_constant_b,
    .GetPixelShaderConstantB = device_get_pixel_shader_constant_b,
    .DrawRectPatch = device_draw_rect_patch,
    .DrawTriPatch = device_draw_tri_patch,
    .DeletePatch = device_delete_patch,
    .CreateQuery = device_create_query
};

static IDirect3DVertexBuffer9Vtbl g_vb_vtbl = {
    .QueryInterface = vb_query_interface,
    .AddRef = vb_add_ref,
    .Release = vb_release,
    .GetDevice = vb_get_device,
    .SetPrivateData = vb_set_private_data,
    .GetPrivateData = vb_get_private_data,
    .FreePrivateData = vb_free_private_data,
    .SetPriority = vb_set_priority,
    .GetPriority = vb_get_priority,
    .PreLoad = vb_preload,
    .GetType = vb_get_type,
    .Lock = vb_lock,
    .Unlock = vb_unlock,
    .GetDesc = vb_get_desc
};

static IDirect3DIndexBuffer9Vtbl g_ib_vtbl = {
    .QueryInterface = ib_query_interface,
    .AddRef = ib_add_ref,
    .Release = ib_release,
    .GetDevice = ib_get_device,
    .SetPrivateData = ib_set_private_data,
    .GetPrivateData = ib_get_private_data,
    .FreePrivateData = ib_free_private_data,
    .SetPriority = ib_set_priority,
    .GetPriority = ib_get_priority,
    .PreLoad = ib_preload,
    .GetType = ib_get_type,
    .Lock = ib_lock,
    .Unlock = ib_unlock,
    .GetDesc = ib_get_desc
};

static IDirect3DTexture9Vtbl g_texture_vtbl = {
    .QueryInterface = texture_query_interface,
    .AddRef = texture_add_ref,
    .Release = texture_release,
    .GetDevice = texture_get_device,
    .SetPrivateData = texture_set_private_data,
    .GetPrivateData = texture_get_private_data,
    .FreePrivateData = texture_free_private_data,
    .SetPriority = texture_set_priority,
    .GetPriority = texture_get_priority,
    .PreLoad = texture_preload,
    .GetType = texture_get_type,
    .SetLOD = texture_set_lod,
    .GetLOD = texture_get_lod,
    .GetLevelCount = texture_get_level_count,
    .SetAutoGenFilterType = texture_set_auto_gen_filter_type,
    .GetAutoGenFilterType = texture_get_auto_gen_filter_type,
    .GenerateMipSubLevels = texture_generate_mip_sublevels,
    .GetLevelDesc = texture_get_level_desc,
    .GetSurfaceLevel = texture_get_surface_level,
    .LockRect = texture_lock_rect,
    .UnlockRect = texture_unlock_rect,
    .AddDirtyRect = texture_add_dirty_rect
};

static IDirect3DVertexDeclaration9Vtbl g_decl_vtbl = {
    .QueryInterface = decl_query_interface,
    .AddRef = decl_add_ref,
    .Release = decl_release,
    .GetDevice = decl_get_device,
    .GetDeclaration = decl_get_declaration
};

static IDirect3DVertexShader9Vtbl g_vertex_shader_vtbl = {
    .QueryInterface = vertex_shader_query_interface,
    .AddRef = vertex_shader_add_ref,
    .Release = vertex_shader_release,
    .GetDevice = vertex_shader_get_device,
    .GetFunction = vertex_shader_get_function
};

static IDirect3DPixelShader9Vtbl g_pixel_shader_vtbl = {
    .QueryInterface = pixel_shader_query_interface,
    .AddRef = pixel_shader_add_ref,
    .Release = pixel_shader_release,
    .GetDevice = pixel_shader_get_device,
    .GetFunction = pixel_shader_get_function
};

static IDirect3DSurface9Vtbl g_surface_vtbl = {
    .QueryInterface = surface_query_interface,
    .AddRef = surface_add_ref,
    .Release = surface_release,
    .GetDevice = surface_get_device,
    .SetPrivateData = surface_set_private_data,
    .GetPrivateData = surface_get_private_data,
    .FreePrivateData = surface_free_private_data,
    .SetPriority = surface_set_priority,
    .GetPriority = surface_get_priority,
    .PreLoad = surface_preload,
    .GetType = surface_get_type,
    .GetContainer = surface_get_container,
    .GetDesc = surface_get_desc,
    .LockRect = surface_lock_rect,
    .UnlockRect = surface_unlock_rect,
    .GetDC = surface_get_dc,
    .ReleaseDC = surface_release_dc
};

/*
 * Same lesson the D3D8 path already learned: both SDK version tokens seen in
 * the wild must be accepted, exactly as the real d3d9.dll does. Titles built
 * against the DirectX 9.0b SDK pass 31 (D3D9b_SDK_VERSION) and 9.0c titles
 * pass 32 (D3D_SDK_VERSION); GTA San Andreas is a 9.0b build and passes 31.
 * Rejecting it makes Direct3DCreate9 return NULL at the very first call, which
 * a game can only report as a generic "unable to initialize DirectX".
 *
 * The high bit is the D3D_DEBUG_INFO marker -- an app compiled with that macro
 * passes (version | 0x80000000) to request the debug runtime. There is no debug
 * runtime here, so mask it off rather than failing on it.
 */
#define D3D_SDK_VERSION_9B      31u
#define D3D_SDK_VERSION_9C      32u
#define D3D_SDK_VERSION_DEBUG_BIT 0x80000000u

IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk_version)
{
    D9Direct3D *d3d;
    BOOL transport_ready;
    UINT sdk_base = sdk_version & ~D3D_SDK_VERSION_DEBUG_BIT;

#ifdef D9WG_DIAGNOSTIC_TRACE
    /* Earliest point that is reliably on the thread owning the game's window
     * and its message pump, so the hooks see the whole run rather than only
     * what happens after a device exists. */
    trace_install_message_hooks();
#endif
    TRACE("CALL Direct3DCreate9 sdk=%lu", sdk_version);
    if (sdk_base != D3D_SDK_VERSION_9B && sdk_base != D3D_SDK_VERSION_9C) {
        TRACE("FAIL Direct3DCreate9 sdk=%lu expected=%lu|%lu", sdk_version,
                (UINT)D3D_SDK_VERSION_9B, (UINT)D3D_SDK_VERSION_9C);
        return NULL;
    }
    EnterCriticalSection(&g_transport_lock);
    transport_ready = open_transport_locked();
    LeaveCriticalSection(&g_transport_lock);
    if (!transport_ready) {
        TRACE("FAIL Direct3DCreate9 transport");
        return NULL;
    }

    d3d = (D9Direct3D *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*d3d));
    if (!d3d) {
        TRACE("FAIL Direct3DCreate9 allocation");
        return NULL;
    }
    d3d->iface.lpVtbl = &g_d3d_vtbl;
    d3d->refcount = 1;
    emit_hello_once();
    TRACE("OK Direct3DCreate9 object=%08lX", (DWORD)(uintptr_t)&d3d->iface);
    return &d3d->iface;
}

/*
 * Secondary d3d9.dll exports. A title that statically imports any of these
 * fails to LOAD against a DLL that omits them -- Direct3DCreate9 is never
 * reached, and the failure surfaces as a generic "unable to initialize
 * DirectX" with no diagnostic. The D3D8 path hit exactly this with Warcraft
 * III's ValidateVertexShader/ValidatePixelShader imports; D3DPERF_* (PIX
 * instrumentation hooks) and DebugSetMute are the D3D9-side equivalent risk,
 * commonly pulled in by profiling-instrumented engine builds even when the
 * game never calls them at runtime. All are harmless no-ops here.
 */
void WINAPI DebugSetMute(void)
{
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, const WCHAR *name)
{
    (void)color; (void)name;
    return 0;
}

int WINAPI D3DPERF_EndEvent(void)
{
    return 0;
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR color, const WCHAR *name)
{
    (void)color; (void)name;
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR color, const WCHAR *name)
{
    (void)color; (void)name;
}

WINBOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{
    return FALSE;
}

void WINAPI D3DPERF_SetOptions(DWORD options)
{
    (void)options;
}

DWORD WINAPI D3DPERF_GetStatus(void)
{
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        g_module_instance = instance;
#ifdef D9WG_DIAGNOSTIC_TRACE
        trace_open(instance);
        g_trace_veh = AddVectoredExceptionHandler(1, trace_vectored_exception);
        TRACE("PROCESS_ATTACH diagnostic trace v4 surface-lifetime-20260815 "
                "pid=%lu module=%08lX veh=%08lX",
                GetCurrentProcessId(), (DWORD)(uintptr_t)instance,
                (DWORD)(uintptr_t)g_trace_veh);
        TRACE("MODULE exe=%08lX path=%s proxy=%08lX path=%s",
                (DWORD)(uintptr_t)g_trace_exe_module, g_trace_exe_path,
                (DWORD)(uintptr_t)g_trace_self_module, g_trace_self_path);
        TRACE_FLUSH();
#endif
        initialize_session_id(instance);
        InitializeCriticalSection(&g_transport_lock);
    } else if (reason == DLL_PROCESS_DETACH) {
        TRACE("PROCESS_DETACH reserved=%08lX pending_commands=%lu",
                (DWORD)(uintptr_t)reserved, g_command_count);
#ifdef D9WG_DIAGNOSTIC_TRACE
        trace_window_state();
        trace_last_call("process_detach");
#endif
        TRACE_FLUSH();
        EnterCriticalSection(&g_transport_lock);
        if (g_command_count)
            submit_batch_locked(FALSE);
        close_transport_locked();
        LeaveCriticalSection(&g_transport_lock);
        DeleteCriticalSection(&g_transport_lock);
#ifdef D9WG_DIAGNOSTIC_TRACE
        trace_remove_message_hooks();
        if (g_trace_veh) {
            RemoveVectoredExceptionHandler(g_trace_veh);
            g_trace_veh = NULL;
        }
        trace_close();
#endif
    }
    return TRUE;
}
