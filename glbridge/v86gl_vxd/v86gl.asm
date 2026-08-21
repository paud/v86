; =============================================================================
; v86gl.vxd - Virtual Device Driver for Windows 95/98/Me
;
; Allocates 16 MB of physically contiguous guest RAM below 4 GB and maps it
; into the calling process.  The companion opengl32.dll proxy writes GL command
; records into this buffer, then submits the descriptor through the PCI I/O
; BAR at 0xF100.
;
; Unlike Windows NT/2000/XP, Windows 9x allows user-mode code to execute
; IN/OUT instructions directly (IOPL = 3 for Win32 apps).  Therefore the
; opengl32 proxy can perform the PCI doorbell writes itself; this VxD only
; needs to provide physically-contiguous memory.
;
; Build with the Windows 98 DDK + MASM 6.11:
;   ml -coff -c -Cx -DMASM6 v86gl.asm
;   link /VXD v86gl.obj
;
; Or place in the Win98 DDK and use:  nmake
; =============================================================================

    .386p
    .xlist
    include vmm.inc
    include vwin32.inc
    include shell.inc
    include debug.inc
    .list

; ---------------------------------------------------------------------------
; Constants  (must stay in sync with ../winproxy/v86gl_ioctl.h)
; ---------------------------------------------------------------------------
V86GL_BUFFER_BYTES   EQU  16 * 1024 * 1024   ; 16 MiB
V86GL_IO_BASE        EQU  0F100h

V86GL_REG_STATUS     EQU  0Ch
V86GL_REG_DESC_LO    EQU  10h
V86GL_REG_DESC_HI    EQU  14h
V86GL_REG_DESC_LEN   EQU  18h
V86GL_REG_COMMAND    EQU  1Ch
V86GL_REG_LAST_FRAME EQU  20h
V86GL_REG_LAST_BYTES EQU  24h
V86GL_REG_ERROR      EQU  28h

V86GL_CMD_SUBMIT        EQU  1
V86GL_CMD_FORCE_PRESENT EQU  2

; DeviceIoControl codes  (CTL_CODE(FILE_DEVICE_UNKNOWN, ..., METHOD_BUFFERED, FILE_ANY_ACCESS))
IOCTL_MAP_BUFFER    EQU  00222014h   ; function 0x805
IOCTL_SUBMIT        EQU  00222018h   ; function 0x806
IOCTL_UNMAP_BUFFER  EQU  0022201Ch   ; function 0x807

; ---------------------------------------------------------------------------
; Virtual device declaration
; ---------------------------------------------------------------------------
Declare_Virtual_Device V86GL, 1, 0, V86GL_Control, \
    Undefined_Device_ID, Undefined_Init_Order, \
    V86GL_W32IOCTL, V86GL_W32IOCTL

; ---------------------------------------------------------------------------
; Locked data
; ---------------------------------------------------------------------------
VxD_LOCKED_DATA_SEG

    align 4
v86gl_buffer_linear   dd  0          ; linear address of 16 MB buffer
v86gl_buffer_phys     dd  0          ; physical address
v86gl_buffer_handle   dd  0          ; PageAllocate handle
v86gl_mapped_thread   dd  0          ; thread that currently owns the mapping
v86gl_submit_count    dd  0

VxD_LOCKED_DATA_ENDS

; ---------------------------------------------------------------------------
; Real-mode / init code
; ---------------------------------------------------------------------------
VxD_REAL_INIT_SEG

V86GL_Real_Init:
    mov bx, 0                       ; no pages to exclude
    mov si, 0                       ; no instance data
    mov edx, 0                      ; return success, load at any TSR
    ret

VxD_REAL_INIT_ENDS

; ---------------------------------------------------------------------------
; Init code (runs at system boot, ring 0)
; ---------------------------------------------------------------------------
VxD_ICODE_SEG

BeginProc V86GL_Init
    ;
    ; Allocate physically contiguous, page-aligned system memory.
    ; _PageAllocate(nPages, pType, physAddr, physAlignBits, minPhys, maxPhys, flags)
    ;
    mov eax, V86GL_BUFFER_BYTES
    shr eax, 12                     ; convert to 4 KB pages
    push eax
    VMMCall _PageAllocate, <PG_SYS, eax, 0, 0, 0, 0, PAGECONTIG OR PAGELOCKED>
    add esp, 4
    test eax, eax
    jz short alloc_failed
    mov [v86gl_buffer_handle], eax

    ; Retrieve the physical address of the first page.
    VMMCall _PageGetPhysAddr, <eax, 0>
    test eax, eax
    jz short alloc_failed
    mov [v86gl_buffer_phys], eax

    ; Map the physical range into a flat linear address.
    VMMCall _MapPhysToLinear, <eax, V86GL_BUFFER_BYTES, 0>
    test eax, eax
    jz short alloc_failed
    mov [v86gl_buffer_linear], eax

    ; Zero the buffer.
    mov edi, eax
    mov ecx, V86GL_BUFFER_BYTES / 4
    xor eax, eax
    cld
    rep stosd

    IFDEF DEBUG
    _Dbg_Out "V86GL: buffer allocated", 0
    ENDIF

    clc
    ret

alloc_failed:
    IFDEF DEBUG
    _Dbg_Out "V86GL: buffer allocation FAILED", 0
    ENDIF
    stc
    ret
EndProc V86GL_Init

VxD_ICODE_ENDS

; ---------------------------------------------------------------------------
; Main device control procedure
; ---------------------------------------------------------------------------
VxD_LOCKED_CODE_SEG

BeginProc V86GL_Control
    Control_Dispatch DEVICE_INIT, V86GL_Init
    Control_Dispatch Sys_Dynamic_Terminate, V86GL_Uninit
    clc
    ret
EndProc V86GL_Control

BeginProc V86GL_Uninit
    mov eax, [v86gl_buffer_handle]
    test eax, eax
    jz short no_free
    VMMCall _PageFree, <eax>
    mov [v86gl_buffer_handle], 0
    mov [v86gl_buffer_linear], 0
    mov [v86gl_buffer_phys], 0
no_free:
    clc
    ret
EndProc V86GL_Uninit

; ---------------------------------------------------------------------------
; Win32 DeviceIoControl handler
;
; On entry:
;   esi -> DIOCParams (see VWIN32.H)
;
; DIOCParams layout (relevant fields):
;   +0  dwService   (DIOC_OPEN, DIOC_CLOSEHANDLE, or IOCTL code)
;   +4  dwDDB       (reserved)
;   +8  hDevice
;   +C  hEvent
;   +10 lpUserOutBuffer
;   +14 cbOutBuffer
;   +18 lpUserInBuffer
;   +1C cbInBuffer
;   +20 overlapped
;   +24 hThread
; ---------------------------------------------------------------------------
BeginProc V86GL_W32IOCTL

    mov eax, [esi.dwService]

    ;
    ; CreateFile / CloseHandle
    ;
    cmp eax, DIOC_OPEN
    je short ioctl_success
    cmp eax, DIOC_CLOSEHANDLE
    je short ioctl_close

    ;
    ; IOCTL_MAP_BUFFER
    ;   OutBuffer: V86GLMapBuffer { user_address, buffer_bytes }
    ;
    cmp eax, IOCTL_MAP_BUFFER
    jne check_submit

    mov edi, [esi.lpUserOutBuffer]
    test edi, edi
    jz ioctl_fail
    mov ecx, [esi.cbOutBuffer]
    cmp ecx, 8                       ; sizeof(V86GLMapBuffer)
    jb ioctl_fail

    mov eax, [v86gl_buffer_linear]
    mov [edi], eax                   ; user_address
    mov dword ptr [edi+4], V86GL_BUFFER_BYTES

    ; Record the calling thread so we can unmap on close.
    mov eax, [esi.hThread]
    mov [v86gl_mapped_thread], eax

    jmp ioctl_success_return8

    ;
    ; IOCTL_SUBMIT
    ;   InBuffer: V86GLSubmit { descriptor_bytes, flags }
    ;
check_submit:
    cmp eax, IOCTL_SUBMIT
    jne check_unmap

    mov esi, [esi.lpUserInBuffer]
    test esi, esi
    jz ioctl_fail

    mov eax, [esi]                   ; descriptor_bytes
    mov edx, [esi+4]                 ; flags

    ; Write descriptor physical address + length to PCI I/O BAR.
    mov ecx, V86GL_IO_BASE + V86GL_REG_DESC_HI
    xor eax, eax                     ; high 32 bits = 0
    out dx, eax

    mov eax, [v86gl_buffer_phys]
    mov edx, V86GL_IO_BASE + V86GL_REG_DESC_LO
    out dx, eax

    mov eax, [esi]                   ; descriptor_bytes
    mov edx, V86GL_IO_BASE + V86GL_REG_DESC_LEN
    out dx, eax

    ; Build COMMAND register value.
    mov eax, V86GL_CMD_SUBMIT
    test edx, 1                     ; flags & V86GL_SUBMIT_FORCE_PRESENT
    jz no_force_present
    or eax, V86GL_CMD_FORCE_PRESENT
no_force_present:
    mov edx, V86GL_IO_BASE + V86GL_REG_COMMAND
    out dx, eax

    ; Read back status.
    mov edx, V86GL_IO_BASE + V86GL_REG_STATUS
    in eax, dx

    inc [v86gl_submit_count]
    jmp ioctl_success

    ;
    ; IOCTL_UNMAP_BUFFER
    ;
check_unmap:
    cmp eax, IOCTL_UNMAP_BUFFER
    jne ioctl_fail

    mov [v86gl_mapped_thread], 0
    jmp ioctl_success

ioctl_close:
    mov [v86gl_mapped_thread], 0
    ; fall through

ioctl_success:
    xor eax, eax                     ; return 0 = success
    ret

ioctl_success_return8:
    mov eax, 8                       ; 8 bytes returned
    ret

ioctl_fail:
    mov eax, 1                       ; non-zero = failure
    ret

EndProc V86GL_W32IOCTL

VxD_LOCKED_CODE_ENDS

end
