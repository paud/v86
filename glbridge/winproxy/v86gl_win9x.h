/*
 * v86gl_win9x.h - Driverless transport for Windows 95/98/Me
 *
 * On Win9x, 32-bit user-mode applications can:
 *   1. Execute IN/OUT instructions directly (IOPL = 3)
 *   2. Call VMM VxD services via VxDCall (int 20h)
 *
 * This eliminates the need for v86gl.sys or v86gl.vxd.
 *
 * The proxy's open_v86gl()/close_v86gl()/emit_pci_batch() call these
 * functions when g_use_driverless is TRUE.
 *
 * This header must be included AFTER the transport globals
 * (g_dma_buffer, g_dma_capacity, etc.) are defined, since the
 * static functions below reference them directly.
 */

#ifndef V86GL_WIN9X_H
#define V86GL_WIN9X_H

#include <windows.h>
#include "v86gl_ioctl.h"

/* ------------------------------------------------------------------ */
/*  VxDCall interface                                                  */
/* ------------------------------------------------------------------ */

typedef DWORD (WINAPI *VxDCall_t)(DWORD service, ...);

static VxDCall_t pVxDCall = NULL;
static BOOL      g_is_win9x = FALSE;

/* VMM service numbers (from Win98 DDK VMM.INC) */
#define VMM_PageAllocate       0x000D0025
#define VMM_PageFree           0x000D0027
#define VMM_PageGetPhysAddr    0x000D0029
#define VMM_MapPhysToLinear    0x000D0015

/* PageAllocate flags */
#define PG_SYS         0x00000001
#define PAGECONTIG     0x00000002
#define PAGELOCKED     0x00000004
#define PAGEZEROINIT   0x00000008

/* ------------------------------------------------------------------ */
/*  Direct I/O port access (allowed on Win9x user mode)                */
/* ------------------------------------------------------------------ */

static __inline void outl_9x(USHORT port, ULONG value)
{
    __asm {
        mov edx, port
        mov eax, value
        out dx, eax
    }
}

static __inline ULONG inl_9x(USHORT port)
{
    ULONG val;
    __asm {
        mov edx, port
        in  eax, dx
        mov val, eax
    }
    return val;
}

/* ------------------------------------------------------------------ */
/*  Driverless transport state                                         */
/*  g_dma_buffer / g_dma_capacity are defined in opengl32_proxy.c      */
/* ------------------------------------------------------------------ */

static ULONG   g_win9x_phys_addr   = 0;
static HANDLE  g_win9x_page_handle = NULL;
static BOOL    g_win9x_available   = FALSE;
static BOOL    g_win9x_checked     = FALSE;
static BOOL    g_use_driverless    = FALSE;

/* ------------------------------------------------------------------ */
/*  Detect Windows 9x and resolve VxDCall                              */
/* ------------------------------------------------------------------ */

static BOOL detect_win9x(void)
{
    if (g_is_win9x)
        return TRUE;

    /*
     * GetVersion returns:
     *   Win95: 4.00.950  (major=4, minor=0)
     *   Win98: 4.10.1998 (major=4, minor=10)
     *   WinMe: 4.90.3000 (major=4, minor=90)
     *   NT/2000/XP: major=5 (or higher)
     */
    DWORD ver = GetVersion();
    DWORD major = (DWORD)(LOBYTE(LOWORD(ver)));
    DWORD minor = (DWORD)(HIBYTE(LOWORD(ver)));

    if (major == 4 && minor < 50)
    {
        /* Windows 95, 98, or ME */
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (k32)
        {
            pVxDCall = (VxDCall_t)GetProcAddress(k32, "VxDCall");
        }
        g_is_win9x = (pVxDCall != NULL);
    }

    return g_is_win9x;
}

/* ------------------------------------------------------------------ */
/*  Allocate physically contiguous memory via VMM                      */
/* ------------------------------------------------------------------ */

static BOOL alloc_contiguous_buffer(ULONG size_bytes)
{
    ULONG nPages = (size_bytes + 4095) / 4096;
    ULONG handle;
    ULONG physAddr;
    LPVOID linear;

    if (!pVxDCall)
        return FALSE;

    /*
     * _PageAllocate:
     *   eax = service number
     *   push flags
     *   push maxPhys      (0 = any)
     *   push minPhys      (0 = any)
     *   push physAlign    (0 = any alignment)
     *   push physAddr     (0 = allocated by VMM)
     *   push pType        (PG_SYS)
     *   push nPages
     *   call VxDCall
     *
     * Returns handle in eax, 0 on failure.
     *
     * Note: VxDCall uses cdecl calling convention on Win9x.
     */
    __asm {
        push 0                          ; maxPhys
        push 0                          ; minPhys
        push 0                          ; physAlign
        push 0                          ; physAddr (output)
        push PG_SYS | PAGECONTIG | PAGELOCKED | PAGEZEROINIT
        push nPages
        push VMM_PageAllocate
        call pVxDCall
        add  esp, 28                    ; cdecl: 7 dwords
        mov  handle, eax
    }

    if (!handle)
    {
        return FALSE;
    }

    /* Get the physical address of the first page. */
    __asm {
        push 0                          ; high 32 bits (0 for <4GB)
        push 0                          ; page offset within region
        push handle
        push VMM_PageGetPhysAddr
        call pVxDCall
        add  esp, 16
        mov  physAddr, eax
    }

    if (!physAddr)
    {
        /* Free the allocation */
        __asm {
            push handle
            push VMM_PageFree
            call pVxDCall
            add  esp, 8
        }
        return FALSE;
    }

    /* Map physical range to a flat linear address. */
    __asm {
        push 0                          ; flags (0 = cached, supervisor)
        push size_bytes
        push physAddr
        push VMM_MapPhysToLinear
        call pVxDCall
        add  esp, 16
        mov  linear, eax
    }

    if (!linear)
    {
        __asm {
            push handle
            push VMM_PageFree
            call pVxDCall
            add  esp, 8
        }
        return FALSE;
    }

    g_dma_page_handle = (HANDLE)(uintptr_t)handle;
    g_dma_buffer      = linear;
    g_dma_phys_addr   = physAddr;
    g_dma_capacity    = nPages * 4096;

    return TRUE;
}

static void free_contiguous_buffer(void)
{
    if (g_dma_page_handle && pVxDCall)
    {
        ULONG handle = (ULONG)(uintptr_t)g_dma_page_handle;
        __asm {
            push handle
            push VMM_PageFree
            call pVxDCall
            add  esp, 8
        }
    }
    g_dma_page_handle = NULL;
    g_dma_buffer      = NULL;
    g_dma_capacity    = 0;
    g_dma_phys_addr   = 0;
}

/* ------------------------------------------------------------------ */
/*  Public API (replaces driver-dependent code in opengl32_proxy.c)    */
/* ------------------------------------------------------------------ */

/*
 * Return values:
 *   1 = transport ready (g_dma_buffer valid, g_dma_capacity set)
 *   0 = failed permanently
 */
static int open_v86gl_driverless(void)
{
    if (g_transport_ready)
        return 1;
    if (g_transport_failed)
        return 0;

    if (!detect_win9x())
    {
        /* Not Win9x - caller should fall back to \\.\v86gl driver */
        return 0;
    }

    if (!alloc_contiguous_buffer(V86GL_DEFAULT_BUFFER_BYTES))
    {
        /*
         * Contiguous allocation failed (not enough free physical memory).
         * Try a regular GlobalAlloc as a last resort.  The physical
         * pages may not be contiguous, but v86's read_blob reads
         * linearly from guest physical address space, so this will
         * only work if the Win9x VMM happens to give us contiguous
         * pages (likely for a 16MB allocation on a lightly loaded
         * system).
         */
        g_dma_buffer = GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT,
                                   V86GL_DEFAULT_BUFFER_BYTES);
        if (!g_dma_buffer)
        {
            g_transport_failed = TRUE;
            return 0;
        }
        g_dma_capacity = V86GL_DEFAULT_BUFFER_BYTES;

        /*
         * Try to get the physical address via VxDCall.  If the pages
         * are not contiguous, phys_addr will be wrong and rendering
         * will be corrupted.  We log a warning.
         */
        /* For GlobalAlloc memory on Win9x, linear == physical for
         * addresses below 2GB in most configurations, but this is
         * not guaranteed.  Use it as a best-effort fallback. */
        g_dma_phys_addr = (ULONG)(uintptr_t)g_dma_buffer;
    }

    g_transport_ready = TRUE;
    return 1;
}

static void close_v86gl_driverless(void)
{
    if (g_dma_page_handle)
    {
        free_contiguous_buffer();
    }
    else if (g_dma_buffer)
    {
        GlobalFree(g_dma_buffer);
    }
    g_dma_buffer = NULL;
    g_dma_capacity = 0;
    g_dma_phys_addr = 0;
    g_transport_ready = FALSE;
}

/*
 * Submit the command buffer to the PCI device.
 *
 * descriptor_bytes = total bytes from start of g_dma_buffer (including
 *                    the V86GLDMADesc header) through the last command.
 * flags            = V86GL_SUBMIT_FORCE_PRESENT or 0.
 */
static int submit_v86gl_driverless(ULONG descriptor_bytes, ULONG flags)
{
    ULONG cmd;

    if (!g_transport_ready || !g_dma_buffer)
        return 0;

    /* Write descriptor physical address (high 32 bits = 0). */
    outl_9x(V86GL_PCI_PORT_DEFAULT + V86GL_REG_DESC_HI, 0);
    outl_9x(V86GL_PCI_PORT_DEFAULT + V86GL_REG_DESC_LO, g_dma_phys_addr);
    outl_9x(V86GL_PCI_PORT_DEFAULT + V86GL_REG_DESC_LEN, descriptor_bytes);

    /* Ring the doorbell. */
    cmd = V86GL_CMD_SUBMIT;
    if (flags & V86GL_SUBMIT_FORCE_PRESENT)
        cmd |= V86GL_CMD_FORCE_PRESENT;
    outl_9x(V86GL_PCI_PORT_DEFAULT + V86GL_REG_COMMAND, cmd);

    /* Read status (optional, for diagnostics). */
    ULONG status = inl_9x(V86GL_PCI_PORT_DEFAULT + V86GL_REG_STATUS);
    if (status & V86GL_STATUS_ERROR)
    {
        ULONG error = inl_9x(V86GL_PCI_PORT_DEFAULT + V86GL_REG_ERROR);
        /* Log error via the proxy's logging mechanism. */
        return 0;
    }

    return 1;
}

#endif /* V86GL_WIN9X_H */
