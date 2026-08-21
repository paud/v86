/*
 * App-local Direct3D 8 frontend for Windows XP guests running in v86.
 *
 * This DLL deliberately does not load WineD3D or opengl32.dll.  It keeps D3D8
 * COM/state/resource semantics in the guest, batches high-level commands in
 * the existing v86gl.sys DMA arena, and sends one D8WG stream to WebGPU.
 *
 * The fixed-function path implements Maple's XYZRHW and XYZ geometry,
 * transforms, material/lights, fog, depth/stencil, texture stages and dynamic
 * resources. Unsupported resource and programmable paths return
 * D3DERR_INVALIDCALL rather than pretending that rendering succeeded.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d8.h>
#include <stdint.h>
#include "../winproxy/v86gl_ioctl.h"
#include "d3d8_protocol.h"

/* These legacy D3D shade-cap bits are omitted by some d3d8.h variants. */
#ifndef D3DPSHADECAPS_COLORFLATRGB
#define D3DPSHADECAPS_COLORFLATRGB 0x00000002u
#endif
#ifndef D3DPSHADECAPS_ALPHAFLATBLEND
#define D3DPSHADECAPS_ALPHAFLATBLEND 0x00001000u
#endif

#define D8WG_LOG_PREFIX "[d3d8-webgpu] "
#define D8WG_MAX_RENDER_STATES 256u
#define D8WG_MAX_TEXTURE_STAGES 8u
#define D8WG_MAX_TEXTURE_STAGE_STATES 32u
#define D8WG_MAX_STREAMS 16u
#define D8WG_MAX_LIGHTS 8u
#define D8WG_TRANSFORM_VIEW_SLOT 0u
#define D8WG_TRANSFORM_PROJECTION_SLOT 1u
#define D8WG_TRANSFORM_TEXTURE_SLOT 2u
#define D8WG_TRANSFORM_WORLD_SLOT 10u
#define D8WG_MAX_TRANSFORMS (D8WG_TRANSFORM_WORLD_SLOT + 256u)
#define D8WG_VGL2_RECORD_HEADER_BYTES 8u
#define D8WG_HANDLE_GENERATION_ONE (1u << 20)
/* Stage 6: D3D8 shader model 1.x. MaxVertexShaderConst/MaxPixelShaderValue
 * were already pre-staged in fill_caps() ahead of Stage 6 landing. */
#define D8WG_MAX_VS_CONSTANTS 96u
#define D8WG_MAX_PS_CONSTANTS 8u
#define D8WG_MAX_SHADER_TOKENS 8192u

typedef struct D8Direct3D D8Direct3D;
typedef struct D8Device D8Device;
typedef struct D8VertexBuffer D8VertexBuffer;
typedef struct D8IndexBuffer D8IndexBuffer;
typedef struct D8Texture D8Texture;
typedef struct D8Surface D8Surface;
typedef struct D8StateBlock D8StateBlock;
typedef struct D8SwapChain D8SwapChain;
typedef struct D8Shader D8Shader;

typedef struct D8TextureLevel {
    BYTE *shadow;
    UINT width;
    UINT height;
    UINT row_pitch;
    UINT row_count;
    UINT byte_count;
    RECT lock_rect;
    DWORD lock_flags;
    BOOL locked;
} D8TextureLevel;

typedef struct D8StreamBinding {
    D8VertexBuffer *buffer;
    UINT stride;
} D8StreamBinding;

typedef struct D8StateSnapshot {
    BYTE render_mask[D8WG_MAX_RENDER_STATES];
    BYTE texture_stage_mask[D8WG_MAX_TEXTURE_STAGES]
                           [D8WG_MAX_TEXTURE_STAGE_STATES];
    BYTE texture_mask[D8WG_MAX_TEXTURE_STAGES];
    BYTE stream_mask[D8WG_MAX_STREAMS];
    BYTE transform_mask[D8WG_MAX_TRANSFORMS];
    BYTE light_mask[D8WG_MAX_LIGHTS];
    BYTE light_enable_mask[D8WG_MAX_LIGHTS];
    BOOL viewport_mask;
    BOOL material_mask;
    BOOL indices_mask;
    BOOL vertex_shader_mask;
    BOOL pixel_shader_mask;
    DWORD render_states[D8WG_MAX_RENDER_STATES];
    DWORD texture_stage_states[D8WG_MAX_TEXTURE_STAGES]
                                      [D8WG_MAX_TEXTURE_STAGE_STATES];
    D8Texture *textures[D8WG_MAX_TEXTURE_STAGES];
    D8StreamBinding streams[D8WG_MAX_STREAMS];
    D8IndexBuffer *index_buffer;
    UINT base_vertex_index;
    DWORD vertex_shader;
    DWORD pixel_shader;
    D3DVIEWPORT8 viewport;
    D3DMATRIX transforms[D8WG_MAX_TRANSFORMS];
    D3DMATERIAL8 material;
    D3DLIGHT8 lights[D8WG_MAX_LIGHTS];
    BOOL light_set[D8WG_MAX_LIGHTS];
    BOOL light_enabled[D8WG_MAX_LIGHTS];
} D8StateSnapshot;

struct D8StateBlock {
    D8StateBlock *next;
    DWORD token;
    D3DSTATEBLOCKTYPE type;
    D8StateSnapshot state;
};

struct D8Direct3D {
    IDirect3D8 iface;
    LONG refcount;
};

struct D8Device {
    IDirect3DDevice8 iface;
    LONG refcount;
    LONG child_parent_refs;
    BOOL releasing_owned_refs;
    D8Direct3D *parent;
    uint32_t handle;
    D3DDEVICE_CREATION_PARAMETERS creation;
    D3DPRESENT_PARAMETERS present;
    D3DDISPLAYMODE display_mode;
    D3DVIEWPORT8 viewport;
    D3DMATRIX transforms[D8WG_MAX_TRANSFORMS];
    D3DMATERIAL8 material;
    D3DLIGHT8 lights[D8WG_MAX_LIGHTS];
    BOOL light_set[D8WG_MAX_LIGHTS];
    BOOL light_enabled[D8WG_MAX_LIGHTS];
    DWORD render_states[D8WG_MAX_RENDER_STATES];
    DWORD texture_stage_states[D8WG_MAX_TEXTURE_STAGES]
                                      [D8WG_MAX_TEXTURE_STAGE_STATES];
    D8StreamBinding streams[D8WG_MAX_STREAMS];
    D8IndexBuffer *index_buffer;
    D8Texture *textures[D8WG_MAX_TEXTURE_STAGES];
    UINT base_vertex_index;
    DWORD vertex_shader;
    DWORD pixel_shader;
    BOOL in_scene;
    HWND tracked_window;
    WNDPROC original_window_proc;
    BOOL window_subclassed;
    BOOL window_unicode;
    D8WGUpdateSurface last_surface;
    BOOL has_last_surface;
    D8StateBlock *state_blocks;
    D8StateBlock *recording_state_block;
    DWORD next_state_block_token;
    D8VertexBuffer *vertex_buffers;
    D8IndexBuffer *index_buffers;
    D8Texture *texture_resources;
    D8Shader *vertex_shaders;
    D8Shader *pixel_shaders;
    float vs_constants[D8WG_MAX_VS_CONSTANTS][4];
    float ps_constants[D8WG_MAX_PS_CONSTANTS][4];
    uint32_t reset_epoch;
    D8Texture *render_target_texture;
    UINT render_target_level;
    BOOL depth_surface_enabled;
    BYTE *front_shadow;
    UINT front_shadow_pitch;
    UINT additional_swap_chain_count;
    UINT implicit_surface_count;
    D8Surface *depth_surface;
};

struct D8VertexBuffer {
    IDirect3DVertexBuffer8 iface;
    LONG refcount;
    D8Device *device;
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
    D8VertexBuffer *next_device_resource;
};

struct D8IndexBuffer {
    IDirect3DIndexBuffer8 iface;
    LONG refcount;
    D8Device *device;
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
    D8IndexBuffer *next_device_resource;
};

struct D8Texture {
    IDirect3DTexture8 iface;
    LONG refcount;
    D8Device *device;
    uint32_t handle;
    UINT width;
    UINT height;
    UINT level_count;
    DWORD usage;
    D3DFORMAT format;
    D3DPOOL pool;
    DWORD priority;
    DWORD lod;
    D8TextureLevel *levels;
    D8Texture *next_device_resource;
    BOOL lockable_render_target;
};

/*
 * D3D8 vertex/pixel shaders are not COM objects: CreateVertexShader and
 * CreatePixelShader hand back a raw DWORD handle (bit 0 always set, which is
 * how SetVertexShader distinguishes a real shader handle from an FVF code),
 * and the app must call Delete{Vertex,Pixel}Shader explicitly. Keep a shadow
 * copy of the declaration/bytecode token streams so Get*Declaration/
 * Get*Function can answer from guest memory and so Reset can resubmit
 * CREATE_VERTEX_SHADER/CREATE_PIXEL_SHADER for the host executor to
 * re-translate under the new device epoch.
 */
struct D8Shader {
    D8Shader *next;
    uint32_t handle;
    BOOL is_pixel_shader;
    DWORD *declaration_tokens;      /* vertex shaders only; NULL for pixel */
    UINT declaration_token_count;
    DWORD *code_tokens;             /* version token + body, excludes END */
    UINT code_token_count;
    UINT major_version;
    UINT minor_version;
};

struct D8Surface {
    IDirect3DSurface8 iface;
    LONG refcount;
    D8Texture *texture;
    D8Device *device;
    UINT level;
    D3DSURFACE_DESC desc;
    BYTE *shadow;
    UINT row_pitch;
    RECT lock_rect;
    DWORD lock_flags;
    BOOL locked;
    BOOL backbuffer;
    BOOL depth_stencil;
    BOOL reset_blocker;
};

struct D8SwapChain {
    IDirect3DSwapChain8 iface;
    LONG refcount;
    D8Device *device;
    D3DPRESENT_PARAMETERS present;
};

static HANDLE g_transport = INVALID_HANDLE_VALUE;
static uint8_t *g_dma_buffer;
static uint32_t g_dma_capacity;
static uint32_t g_batch_bytes;
static uint32_t g_command_count;
static uint32_t g_frame_id = 1;
static uint32_t g_sequence = 1;
static uint32_t g_next_handle = D8WG_HANDLE_GENERATION_ONE;
static uint32_t g_session_id_low;
static uint32_t g_session_id_high;
static BOOL g_transport_failed;
static BOOL g_hello_emitted;
static CRITICAL_SECTION g_transport_lock;

static IDirect3D8Vtbl g_d3d_vtbl;
static IDirect3DDevice8Vtbl g_device_vtbl;
static IDirect3DVertexBuffer8Vtbl g_vb_vtbl;
static IDirect3DIndexBuffer8Vtbl g_ib_vtbl;
static IDirect3DTexture8Vtbl g_texture_vtbl;
static IDirect3DSurface8Vtbl g_surface_vtbl;
static IDirect3DSwapChain8Vtbl g_swapchain_vtbl;

static void state_block_release_references(D8StateBlock *block);
static BOOL recreate_device_resources(D8Device *device);
static BOOL device_has_reset_blockers(D8Device *device);
static void device_clear_bindings(D8Device *device);
static HRESULT WINAPI device_set_pixel_shader(IDirect3DDevice8 *iface,
        DWORD shader);
static void device_release_owned_references(D8Device *device);

static D8Direct3D *d3d_from_iface(IDirect3D8 *iface)
{
    return (D8Direct3D *)iface;
}

static D8Device *device_from_iface(IDirect3DDevice8 *iface)
{
    return (D8Device *)iface;
}

static D8VertexBuffer *vb_from_iface(IDirect3DVertexBuffer8 *iface)
{
    return (D8VertexBuffer *)iface;
}

static D8IndexBuffer *ib_from_iface(IDirect3DIndexBuffer8 *iface)
{
    return (D8IndexBuffer *)iface;
}

static D8Texture *texture_from_iface(IDirect3DTexture8 *iface)
{
    return (D8Texture *)iface;
}

static D8Surface *surface_from_iface(IDirect3DSurface8 *iface)
{
    return (D8Surface *)iface;
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

static void d8wg_log(const char *text)
{
    OutputDebugStringA(D8WG_LOG_PREFIX);
    OutputDebugStringA(text);
    OutputDebugStringA("\r\n");
}

static uint32_t allocate_handle(void)
{
    uint32_t handle = (uint32_t)InterlockedIncrement((LONG *)&g_next_handle);
    if (!handle)
        handle = (uint32_t)InterlockedIncrement((LONG *)&g_next_handle);
    return handle;
}

/* Shader handles live in a namespace disjoint from allocate_handle()'s
 * buffer/texture counter (see D8WG_SHADER_HANDLE_BASE) and always carry bit 0
 * set, matching the real D3D8 convention that distinguishes a genuine shader
 * handle from a raw FVF token passed to SetVertexShader. */
static uint32_t g_next_shader_handle = D8WG_SHADER_HANDLE_BASE;

static uint32_t allocate_shader_handle(void)
{
    uint32_t handle = (uint32_t)InterlockedExchangeAdd(
            (LONG *)&g_next_shader_handle, 2);
    if (!handle) {
        handle = (uint32_t)InterlockedExchangeAdd(
                (LONG *)&g_next_shader_handle, 2);
    }
    return handle | 1u;
}

static uint8_t *batch_base(void)
{
    return g_dma_buffer + sizeof(V86GLDMADesc)
            + D8WG_VGL2_RECORD_HEADER_BYTES;
}

static uint32_t batch_capacity(void)
{
    if (g_dma_capacity <= sizeof(V86GLDMADesc)
            + D8WG_VGL2_RECORD_HEADER_BYTES)
        return 0;
    return g_dma_capacity - (uint32_t)sizeof(V86GLDMADesc)
            - D8WG_VGL2_RECORD_HEADER_BYTES;
}

static void reset_batch_locked(void)
{
    D8WGBatchHeader *header;

    g_batch_bytes = sizeof(D8WGBatchHeader);
    g_command_count = 0;
    if (!g_dma_buffer || batch_capacity() < sizeof(D8WGBatchHeader))
        return;

    header = (D8WGBatchHeader *)batch_base();
    ZeroMemory(header, sizeof(*header));
    header->magic = D8WG_MAGIC;
    header->version_major = D8WG_VERSION_MAJOR;
    header->version_minor = D8WG_VERSION_MINOR;
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
        d8wg_log("could not open \\.\\v86gl");
        return FALSE;
    }

    ZeroMemory(&mapping, sizeof(mapping));
    if (!DeviceIoControl(g_transport, V86GL_IOCTL_MAP_BUFFER,
            NULL, 0, &mapping, sizeof(mapping), &returned, NULL)
            || returned != sizeof(mapping)
            || !mapping.user_address
            || mapping.buffer_bytes < sizeof(V86GLDMADesc)
                    + D8WG_VGL2_RECORD_HEADER_BYTES
                    + sizeof(D8WGBatchHeader)
                    + sizeof(D8WGCommandHeader)) {
        d8wg_log("v86gl MAP_BUFFER failed");
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
    D8WGBatchHeader *batch;
    V86GLSubmit submit;
    uint8_t *outer;
    uint32_t outer_bytes;
    DWORD returned = 0;

    if (!open_transport_locked())
        return FALSE;
    if (g_command_count == 0)
        return TRUE;

    batch = (D8WGBatchHeader *)batch_base();
    batch->frame_id = g_frame_id;
    batch->flags = present ? D8WG_BATCH_FLAG_PRESENT : 0;
    batch->command_count = g_command_count;
    batch->command_bytes = g_batch_bytes - sizeof(*batch);
    batch->session_id_low = g_session_id_low;
    batch->session_id_high = g_session_id_high;

    outer = g_dma_buffer + sizeof(V86GLDMADesc);
    outer[0] = (uint8_t)(V86GL_CTRL_D3D8_BATCH & 0xFFu);
    outer[1] = (uint8_t)(V86GL_CTRL_D3D8_BATCH >> 8);
    outer[2] = 0xFF;
    outer[3] = 0xFF;
    outer[4] = (uint8_t)(g_batch_bytes & 0xFFu);
    outer[5] = (uint8_t)((g_batch_bytes >> 8) & 0xFFu);
    outer[6] = (uint8_t)((g_batch_bytes >> 16) & 0xFFu);
    outer[7] = (uint8_t)((g_batch_bytes >> 24) & 0xFFu);

    outer_bytes = D8WG_VGL2_RECORD_HEADER_BYTES + g_batch_bytes;
    descriptor = (V86GLDMADesc *)g_dma_buffer;
    descriptor->magic = V86GL_MAGIC;
    descriptor->version = V86GL_VERSION;
    /* WebGPU Present is an inner command.  Do not ask the GL bridge to swap. */
    descriptor->flags = 0;
    descriptor->frame_id = g_frame_id;
    descriptor->command_count = 1;
    descriptor->command_bytes = outer_bytes;
    descriptor->reserved0 = D8WG_MAGIC;
    descriptor->reserved1 = 0;

    submit.descriptor_bytes = (uint32_t)sizeof(*descriptor) + outer_bytes;
    submit.flags = 0;
    if (!DeviceIoControl(g_transport, V86GL_IOCTL_SUBMIT,
            &submit, sizeof(submit), NULL, 0, &returned, NULL)) {
        d8wg_log("D8WG batch submit failed");
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
        uint32_t extra_bytes, D8WGCommandHeader **command_out,
        uint8_t **payload_out, uint8_t **extra_out)
{
    uint32_t raw_size;
    uint32_t record_size;
    D8WGCommandHeader *command;

    if (!open_transport_locked())
        return FALSE;
    if (payload_bytes > 0xFFFFFFFFu - sizeof(*command) - extra_bytes)
        return FALSE;
    raw_size = (uint32_t)sizeof(*command) + payload_bytes + extra_bytes;
    record_size = D8WG_ALIGN8(raw_size);
    if (record_size > batch_capacity() - sizeof(D8WGBatchHeader))
        return FALSE;
    if (g_batch_bytes + record_size > batch_capacity()
            && !submit_batch_locked(FALSE))
        return FALSE;

    command = (D8WGCommandHeader *)(batch_base() + g_batch_bytes);
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

static BOOL emit_buffer_update(uint32_t handle, uint32_t destination_offset,
        const void *data, uint32_t byte_count, uint32_t lock_flags)
{
    D8WGUpdateBuffer update;
    D8WGCommandHeader *command;
    uint8_t *payload;
    uint8_t *blob;
    BOOL result;

    ZeroMemory(&update, sizeof(update));
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D8WG_OP_UPDATE_BUFFER,
            sizeof(update), byte_count, &command, &payload, &blob);
    if (result) {
        update.resource_handle = handle;
        update.destination_offset = destination_offset;
        update.byte_count = byte_count;
        update.data_offset = (uint32_t)((uint8_t *)blob - batch_base());
        update.lock_flags = lock_flags;
        CopyMemory(payload, &update, sizeof(update));
        if (byte_count)
            CopyMemory(blob, data, byte_count);
        (void)command;
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

/* ---- Stage 6: D3D8 shader model 1.x bytecode validation ---- */

static BOOL shader_regtype_valid(DWORD register_type, BOOL is_pixel_shader)
{
    switch (register_type) {
    case D3DSPR_TEMP:
    case D3DSPR_INPUT:
    case D3DSPR_CONST:
    case D3DSPR_ADDR: /* == D3DSPR_TEXTURE; d3d8types.h aliases the value. */
        return TRUE;
    case D3DSPR_RASTOUT:
    case D3DSPR_ATTROUT:
    case D3DSPR_TEXCRDOUT:
        return !is_pixel_shader;
    default:
        return FALSE;
    }
}

/*
 * Returns FALSE for any opcode outside the Stage 6 supported instruction set
 * for this shader type/version. Unimplemented-but-legal D3D8 opcodes (matrix
 * macros, bump-mapping texture ops, control flow) are rejected the same way
 * as truly malformed data: the doc's parser safety rule is "never guess at
 * semantics for an instruction you do not implement." On success,
 * *operand_words is the number of DWORD parameter tokens following the
 * opcode token (D3DSIO_DEF's trailing four are raw float32 immediates, not
 * register-encoded operands, but are still counted here).
 */
static BOOL shader_opcode_supported(WORD opcode, BOOL is_pixel_shader,
        UINT minor, UINT *operand_words)
{
    switch (opcode) {
    case D3DSIO_NOP:
        *operand_words = 0; return TRUE;
    case D3DSIO_MOV:
        *operand_words = 2; return TRUE;
    case D3DSIO_ADD:
    case D3DSIO_SUB:
    case D3DSIO_MUL:
    case D3DSIO_MIN:
    case D3DSIO_MAX:
    case D3DSIO_DP3:
    case D3DSIO_DP4:
        *operand_words = 3; return TRUE;
    case D3DSIO_MAD:
        *operand_words = 4; return TRUE;
    case D3DSIO_RCP:
    case D3DSIO_RSQ:
    case D3DSIO_EXP:
    case D3DSIO_LOG:
    case D3DSIO_LIT:
    case D3DSIO_FRC:
    case D3DSIO_EXPP:
    case D3DSIO_LOGP:
        if (is_pixel_shader) return FALSE;
        *operand_words = 2; return TRUE;
    case D3DSIO_SLT:
    case D3DSIO_SGE:
    case D3DSIO_DST:
        if (is_pixel_shader) return FALSE;
        *operand_words = 3; return TRUE;
    case D3DSIO_DEF:
        *operand_words = 5; return TRUE;
    case D3DSIO_LRP:
        if (!is_pixel_shader) return FALSE;
        *operand_words = 4; return TRUE;
    case D3DSIO_CND:
        if (!is_pixel_shader || minor > 3u) return FALSE;
        *operand_words = 4; return TRUE;
    case D3DSIO_CMP:
        if (!is_pixel_shader || minor < 2u) return FALSE;
        *operand_words = 4; return TRUE;
    case D3DSIO_TEXCOORD:
        if (!is_pixel_shader) return FALSE;
        *operand_words = 1; return TRUE;
    case D3DSIO_TEX:
        if (!is_pixel_shader) return FALSE;
        *operand_words = (minor >= 4u) ? 2u : 1u; return TRUE;
    case D3DSIO_TEXKILL:
        if (!is_pixel_shader) return FALSE;
        *operand_words = 1; return TRUE;
    case D3DSIO_PHASE:
        if (!is_pixel_shader || minor != 4u) return FALSE;
        *operand_words = 0; return TRUE;
    default:
        return FALSE;
    }
}

/*
 * Walk one D3D8 SM1.x token stream (the app-supplied pFunction, starting
 * right after the version token) until D3DSIO_END. Rejects anything that
 * would read past `max_tokens`, any opcode outside shader_opcode_supported,
 * and any register-type field outside the recognized D3DSPR_* set, so a
 * corrupt or hostile token stream can never desync the parser or run
 * unbounded. On success *body_token_count is the token count from index 0
 * through (but excluding) the END token.
 */
static BOOL validate_shader_body(const DWORD *tokens, UINT max_tokens,
        BOOL is_pixel_shader, UINT minor, UINT *body_token_count)
{
    UINT offset = 0;
    while (offset < max_tokens) {
        DWORD token = tokens[offset];
        WORD opcode;
        UINT operand_words;

        if (token == (DWORD)D3DVS_END()) {
            *body_token_count = offset;
            return TRUE;
        }
        opcode = (WORD)(token & D3DSI_OPCODE_MASK);
        if (opcode == D3DSIO_COMMENT) {
            UINT comment_words = (UINT)((token & D3DSI_COMMENTSIZE_MASK)
                    >> D3DSI_COMMENTSIZE_SHIFT);
            if (offset + 1u + comment_words > max_tokens)
                return FALSE;
            offset += 1u + comment_words;
            continue;
        }
        if (!shader_opcode_supported(opcode, is_pixel_shader, minor,
                &operand_words))
            return FALSE;
        if (offset + 1u + operand_words > max_tokens)
            return FALSE;
        if (opcode != D3DSIO_NOP && opcode != D3DSIO_PHASE) {
            DWORD dst_regtype = tokens[offset + 1u] & D3DSP_REGTYPE_MASK;
            if (!shader_regtype_valid(dst_regtype, is_pixel_shader))
                return FALSE;
        }
        if (opcode != D3DSIO_DEF && opcode != D3DSIO_NOP
                && opcode != D3DSIO_PHASE) {
            UINT src_count = operand_words - 1u;
            UINT src;
            for (src = 0; src < src_count; ++src) {
                DWORD src_regtype =
                        tokens[offset + 2u + src] & D3DSP_REGTYPE_MASK;
                if (!shader_regtype_valid(src_regtype, is_pixel_shader))
                    return FALSE;
            }
        }
        offset += 1u + operand_words;
    }
    return FALSE; /* ran past max_tokens without ever finding D3DSIO_END */
}

/*
 * Walk one D3DVSD_* vertex declaration token stream until D3DVSD_END()
 * (0xFFFFFFFF), bounded by max_tokens. This only checks structural safety
 * (each token's type nibble is one of the seven recognized D3DVSD_TOKEN_*
 * values and END is reached within the bound); the host executor is
 * responsible for interpreting D3DVSD_REG entries into a vertex input
 * layout when it translates the paired vertex shader.
 */
static BOOL validate_vertex_declaration(const DWORD *tokens, UINT max_tokens,
        UINT *token_count)
{
    UINT offset = 0;
    while (offset < max_tokens) {
        DWORD token = tokens[offset];
        DWORD token_type;
        if (token == 0xFFFFFFFFu) {
            *token_count = offset;
            return TRUE;
        }
        token_type = (token >> D3DVSD_TOKENTYPESHIFT) & 0x7u;
        if (token_type > (DWORD)D3DVSD_TOKEN_END)
            return FALSE;
        ++offset;
    }
    return FALSE;
}

static D8Shader *find_shader(D8Shader *list, DWORD handle)
{
    D8Shader *shader;
    for (shader = list; shader; shader = shader->next) {
        if (shader->handle == handle) return shader;
    }
    return NULL;
}

static void free_shader_list(D8Shader *list)
{
    while (list) {
        D8Shader *next = list->next;
        HeapFree(GetProcessHeap(), 0, list->declaration_tokens);
        HeapFree(GetProcessHeap(), 0, list->code_tokens);
        HeapFree(GetProcessHeap(), 0, list);
        list = next;
    }
}

static BOOL emit_create_vertex_shader(D8Device *device, D8Shader *shader)
{
    D8WGCreateVertexShader command;
    uint8_t *payload;
    uint8_t *decl_blob;
    uint8_t *code_blob;
    UINT decl_bytes = shader->declaration_token_count * (UINT)sizeof(DWORD);
    UINT code_bytes = shader->code_token_count * (UINT)sizeof(DWORD);
    BOOL result;

    ZeroMemory(&command, sizeof(command));
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D8WG_OP_CREATE_VERTEX_SHADER,
            sizeof(command), decl_bytes + code_bytes, NULL, &payload,
            &decl_blob);
    if (result) {
        code_blob = decl_blob + decl_bytes;
        command.device_handle = device->handle;
        command.resource_handle = shader->handle;
        command.declaration_token_count = shader->declaration_token_count;
        command.declaration_offset = (uint32_t)(decl_blob - batch_base());
        command.instruction_token_count = shader->code_token_count;
        command.code_offset = (uint32_t)(code_blob - batch_base());
        CopyMemory(payload, &command, sizeof(command));
        if (decl_bytes)
            CopyMemory(decl_blob, shader->declaration_tokens, decl_bytes);
        if (code_bytes)
            CopyMemory(code_blob, shader->code_tokens, code_bytes);
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

static BOOL emit_create_pixel_shader(D8Device *device, D8Shader *shader)
{
    D8WGCreatePixelShader command;
    uint8_t *payload;
    uint8_t *code_blob;
    UINT code_bytes = shader->code_token_count * (UINT)sizeof(DWORD);
    BOOL result;

    ZeroMemory(&command, sizeof(command));
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D8WG_OP_CREATE_PIXEL_SHADER,
            sizeof(command), code_bytes, NULL, &payload, &code_blob);
    if (result) {
        command.device_handle = device->handle;
        command.resource_handle = shader->handle;
        command.instruction_token_count = shader->code_token_count;
        command.code_offset = (uint32_t)(code_blob - batch_base());
        CopyMemory(payload, &command, sizeof(command));
        if (code_bytes)
            CopyMemory(code_blob, shader->code_tokens, code_bytes);
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

static BOOL emit_set_shader(uint16_t opcode, D8Device *device,
        DWORD shader_handle)
{
    D8WGSetShader command;
    command.device_handle = device->handle;
    command.shader_handle = shader_handle;
    return emit_command(opcode, &command, sizeof(command));
}

static BOOL emit_set_shader_constant(uint16_t opcode, D8Device *device,
        UINT start_register, const float *data, UINT vector_count)
{
    D8WGSetShaderConstant command;
    uint8_t *payload;
    uint8_t *blob;
    UINT data_bytes = vector_count * 16u;
    BOOL result;

    ZeroMemory(&command, sizeof(command));
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

static BOOL emit_destroy_shader(D8Shader *shader)
{
    D8WGDestroyResource destroy;
    destroy.resource_handle = shader->handle;
    destroy.resource_kind = shader->is_pixel_shader
            ? D8WG_RESOURCE_PIXEL_SHADER : D8WG_RESOURCE_VERTEX_SHADER;
    return emit_command(D8WG_OP_DESTROY_RESOURCE, &destroy, sizeof(destroy));
}

static BOOL emit_draw_primitive_up(const D8WGDrawPrimitiveUP *draw,
        const void *vertices)
{
    D8WGDrawPrimitiveUP payload_value = *draw;
    uint8_t *payload;
    uint8_t *blob;
    BOOL result;

    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D8WG_OP_DRAW_PRIMITIVE_UP,
            sizeof(payload_value), payload_value.vertex_bytes, NULL,
            &payload, &blob);
    if (result) {
        payload_value.vertex_data_offset =
                (uint32_t)(blob - batch_base());
        CopyMemory(payload, &payload_value, sizeof(payload_value));
        CopyMemory(blob, vertices, payload_value.vertex_bytes);
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

static BOOL emit_draw_indexed_primitive_up(
        const D8WGDrawIndexedPrimitiveUP *draw, const void *indices,
        const void *vertices)
{
    D8WGDrawIndexedPrimitiveUP payload_value = *draw;
    uint32_t extra_bytes;
    uint8_t *payload;
    uint8_t *blob;
    BOOL result;

    if (payload_value.index_bytes >
            0xFFFFFFFFu - payload_value.vertex_bytes)
        return FALSE;
    extra_bytes = payload_value.index_bytes + payload_value.vertex_bytes;
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D8WG_OP_DRAW_INDEXED_PRIMITIVE_UP,
            sizeof(payload_value), extra_bytes, NULL, &payload, &blob);
    if (result) {
        payload_value.index_data_offset =
                (uint32_t)(blob - batch_base());
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
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        *block_bytes = 4;
        return TRUE;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
        *block_bytes = 2;
        return TRUE;
    case D3DFMT_L8:
    case D3DFMT_A8:
        *block_bytes = 1;
        return TRUE;
    case D3DFMT_DXT1:
        *block_width = 4;
        *block_height = 4;
        *block_bytes = 8;
        return TRUE;
    case D3DFMT_DXT3:
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

static void write_surface_color(BYTE *destination, D3DFORMAT format,
        D3DCOLOR color)
{
    UINT a = (color >> 24) & 0xffu;
    UINT r = (color >> 16) & 0xffu;
    UINT g = (color >> 8) & 0xffu;
    UINT b = color & 0xffu;
    WORD packed;
    switch (format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        *(DWORD *)destination = color;
        break;
    case D3DFMT_R5G6B5:
        packed = (WORD)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        *(WORD *)destination = packed;
        break;
    case D3DFMT_X1R5G5B5:
        packed = (WORD)(0x8000u | (r >> 3) << 10 | (g >> 3) << 5
                | (b >> 3));
        *(WORD *)destination = packed;
        break;
    case D3DFMT_A1R5G5B5:
        packed = (WORD)((a >> 7) << 15 | (r >> 3) << 10
                | (g >> 3) << 5 | (b >> 3));
        *(WORD *)destination = packed;
        break;
    case D3DFMT_A4R4G4B4:
        packed = (WORD)((a >> 4) << 12 | (r >> 4) << 8
                | (g >> 4) << 4 | (b >> 4));
        *(WORD *)destination = packed;
        break;
    case D3DFMT_L8:
        *destination = (BYTE)((r * 77u + g * 150u + b * 29u) >> 8);
        break;
    case D3DFMT_A8:
        *destination = (BYTE)a;
        break;
    default:
        break;
    }
}

static BOOL emit_texture_update(D8Texture *texture, UINT level,
        const RECT *rect)
{
    D8TextureLevel *level_data = &texture->levels[level];
    D8WGUpdateTexture update;
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
    update.width = (uint32_t)(rect->right - rect->left);
    update.height = (uint32_t)(rect->bottom - rect->top);
    update.row_pitch = row_bytes;
    update.data_bytes = data_bytes;

    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D8WG_OP_UPDATE_TEXTURE,
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

static BOOL capture_surface(D8Device *device, HWND window,
        D8WGUpdateSurface *surface)
{
    RECT client;
    POINT origin;

    ZeroMemory(surface, sizeof(*surface));
    surface->device_handle = device->handle;
    surface->hwnd = (uint32_t)(uintptr_t)window;
    if (!window || !IsWindow(window) || IsIconic(window))
        return FALSE;
    if (!GetClientRect(window, &client))
        return FALSE;
    origin.x = 0;
    origin.y = 0;
    if (!ClientToScreen(window, &origin))
        return FALSE;
    surface->x = origin.x;
    surface->y = origin.y;
    surface->width = (uint32_t)(client.right - client.left);
    surface->height = (uint32_t)(client.bottom - client.top);
    return surface->width != 0 && surface->height != 0;
}

static BOOL same_surface(const D8WGUpdateSurface *left,
        const D8WGUpdateSurface *right)
{
    return left->device_handle == right->device_handle
            && left->hwnd == right->hwnd
            && left->x == right->x
            && left->y == right->y
            && left->width == right->width
            && left->height == right->height;
}

static BOOL emit_surface_update_and_flush(D8Device *device, HWND window,
        BOOL hidden, BOOL force)
{
    D8WGUpdateSurface surface;
    uint8_t *payload;
    BOOL result;

    if (hidden) {
        ZeroMemory(&surface, sizeof(surface));
        surface.device_handle = device->handle;
        surface.hwnd = (uint32_t)(uintptr_t)window;
    } else {
        capture_surface(device, window, &surface);
    }
    if (!force && device->has_last_surface
            && same_surface(&device->last_surface, &surface))
        return TRUE;

    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D8WG_OP_UPDATE_SURFACE, sizeof(surface), 0,
            NULL, &payload, NULL);
    if (result) {
        CopyMemory(payload, &surface, sizeof(surface));
        result = submit_batch_locked(FALSE);
    }
    LeaveCriticalSection(&g_transport_lock);
    if (result) {
        device->last_surface = surface;
        device->has_last_surface = TRUE;
    }
    return result;
}

static BOOL emit_present_and_flush(D8Device *device, HWND override_window)
{
    D8WGUpdateSurface surface;
    D8WGPresent present;
    HWND window = override_window ? override_window : device->tracked_window;
    uint8_t *payload;
    BOOL result;

    capture_surface(device, window, &surface);
    present.device_handle = surface.device_handle;
    present.hwnd = surface.hwnd;
    present.x = surface.x;
    present.y = surface.y;
    present.width = surface.width;
    present.height = surface.height;
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D8WG_OP_PRESENT, sizeof(present), 0,
            NULL, &payload, NULL);
    if (result) {
        CopyMemory(payload, &present, sizeof(present));
        result = submit_batch_locked(TRUE);
    }
    LeaveCriticalSection(&g_transport_lock);
    if (result && !override_window) {
        device->last_surface = surface;
        device->has_last_surface = TRUE;
    }
    return result;
}

static BOOL emit_device_destroy_and_flush(uint32_t device_handle)
{
    D8WGDestroyResource destroy;
    uint8_t *payload;
    BOOL result;

    destroy.resource_handle = device_handle;
    destroy.resource_kind = 0;
    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D8WG_OP_DESTROY_RESOURCE,
            sizeof(destroy), 0, NULL, &payload, NULL);
    if (result) {
        CopyMemory(payload, &destroy, sizeof(destroy));
        result = submit_batch_locked(FALSE);
    }
    LeaveCriticalSection(&g_transport_lock);
    return result;
}

#define D8WG_WINDOW_PROPERTY "D8WG.Device.20260801"

static LRESULT CALLBACK d8wg_window_proc(HWND window, UINT message,
        WPARAM wparam, LPARAM lparam)
{
    D8Device *device = (D8Device *)GetPropA(window, D8WG_WINDOW_PROPERTY);
    WNDPROC original = device ? device->original_window_proc : NULL;
    BOOL unicode = device ? device->window_unicode : FALSE;

    if (!original)
        return DefWindowProcA(window, message, wparam, lparam);

    if (message == WM_WINDOWPOSCHANGED || message == WM_MOVE
            || message == WM_SIZE || message == WM_SHOWWINDOW) {
        BOOL hidden = (message == WM_SHOWWINDOW && !wparam)
                || (message == WM_SIZE && wparam == SIZE_MINIMIZED);
        emit_surface_update_and_flush(device, window, hidden, FALSE);
    } else if (message == WM_NCDESTROY) {
        emit_surface_update_and_flush(device, window, TRUE, TRUE);
        RemovePropA(window, D8WG_WINDOW_PROPERTY);
        device->tracked_window = NULL;
        device->window_subclassed = FALSE;
    }

    return unicode
            ? CallWindowProcW(original, window, message, wparam, lparam)
            : CallWindowProcA(original, window, message, wparam, lparam);
}

static void detach_device_window(D8Device *device)
{
    HWND window = device->tracked_window;

    if (!device->window_subclassed || !window)
        return;
    if ((D8Device *)GetPropA(window, D8WG_WINDOW_PROPERTY) == device) {
        RemovePropA(window, D8WG_WINDOW_PROPERTY);
        LONG current = device->window_unicode
                ? GetWindowLongW(window, GWL_WNDPROC)
                : GetWindowLongA(window, GWL_WNDPROC);
        if ((WNDPROC)(LONG_PTR)current
                == d8wg_window_proc) {
            if (device->window_unicode) {
                SetWindowLongW(window, GWL_WNDPROC,
                        (LONG)(LONG_PTR)device->original_window_proc);
            } else {
                SetWindowLongA(window, GWL_WNDPROC,
                        (LONG)(LONG_PTR)device->original_window_proc);
            }
        }
    }
    device->window_subclassed = FALSE;
    device->original_window_proc = NULL;
    device->window_unicode = FALSE;
}

static void attach_device_window(D8Device *device, HWND window)
{
    LONG previous;

    device->tracked_window = window;
    if (!window || !IsWindow(window))
        return;
    if (GetPropA(window, D8WG_WINDOW_PROPERTY))
        return;
    device->window_unicode = IsWindowUnicode(window);
    previous = device->window_unicode
            ? GetWindowLongW(window, GWL_WNDPROC)
            : GetWindowLongA(window, GWL_WNDPROC);
    if (!previous)
        return;
    device->original_window_proc = (WNDPROC)(LONG_PTR)previous;
    if (!SetPropA(window, D8WG_WINDOW_PROPERTY, (HANDLE)device)) {
        device->original_window_proc = NULL;
        return;
    }
    SetLastError(0);
    previous = device->window_unicode
            ? SetWindowLongW(window, GWL_WNDPROC,
                    (LONG)(LONG_PTR)d8wg_window_proc)
            : SetWindowLongA(window, GWL_WNDPROC,
                    (LONG)(LONG_PTR)d8wg_window_proc);
    if (!previous && GetLastError()) {
        RemovePropA(window, D8WG_WINDOW_PROPERTY);
        device->original_window_proc = NULL;
        device->window_unicode = FALSE;
        return;
    }
    device->window_subclassed = TRUE;
}

static void emit_hello_once(void)
{
    D8WGHello hello;

    if (InterlockedCompareExchange((LONG *)&g_hello_emitted, TRUE, FALSE))
        return;
    hello.guest_pointer_bits = 32;
    hello.feature_bits = 0;
    hello.session_id_low = g_session_id_low;
    hello.session_id_high = g_session_id_high;
    emit_command(D8WG_OP_HELLO, &hello, sizeof(hello));
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
        g_session_id_high = 0xD8A80001u;
}

static void fill_display_mode(D3DDISPLAYMODE *mode, UINT width, UINT height,
        D3DFORMAT format)
{
    mode->Width = width;
    mode->Height = height;
    mode->RefreshRate = 60;
    mode->Format = format;
}

static void fill_caps(D3DCAPS8 *caps)
{
    ZeroMemory(caps, sizeof(*caps));
    caps->DeviceType = D3DDEVTYPE_HAL;
    caps->AdapterOrdinal = D3DADAPTER_DEFAULT;
    caps->Caps2 = D3DCAPS2_CANRENDERWINDOWED;
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
            | D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_BLENDOP;
    caps->RasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_ZBIAS
            | D3DPRASTERCAPS_FOGVERTEX
            | D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_WFOG
            | D3DPRASTERCAPS_FOGRANGE;
    caps->ZCmpCaps = 0xFFu;
    caps->SrcBlendCaps = 0x1FFFu;
    caps->DestBlendCaps = 0x1FFFu;
    caps->AlphaCmpCaps = 0xFFu;
    caps->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO
            | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT
            | D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT
            | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR;
    caps->ShadeCaps = D3DPSHADECAPS_COLORFLATRGB
            | D3DPSHADECAPS_COLORGOURAUDRGB
            | D3DPSHADECAPS_SPECULARGOURAUDRGB
            | D3DPSHADECAPS_FOGGOURAUD
            | D3DPSHADECAPS_ALPHAFLATBLEND
            | D3DPSHADECAPS_ALPHAGOURAUDBLEND;
    caps->TextureCaps = D3DPTEXTURECAPS_ALPHA
            | D3DPTEXTURECAPS_MIPMAP
            | D3DPTEXTURECAPS_PERSPECTIVE;
    caps->TextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT
            | D3DPTFILTERCAPS_MINFLINEAR
            | D3DPTFILTERCAPS_MAGFPOINT
            | D3DPTFILTERCAPS_MAGFLINEAR
            | D3DPTFILTERCAPS_MIPFPOINT
            | D3DPTFILTERCAPS_MIPFLINEAR;
    caps->CubeTextureFilterCaps = 0;
    caps->VolumeTextureFilterCaps = 0;
    caps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP
            | D3DPTADDRESSCAPS_MIRROR
            | D3DPTADDRESSCAPS_CLAMP;
    caps->TextureOpCaps = D3DTEXOPCAPS_DISABLE
            | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2
            | D3DTEXOPCAPS_MODULATE | D3DTEXOPCAPS_MODULATE2X
            | D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD
            | D3DTEXOPCAPS_ADDSIGNED | D3DTEXOPCAPS_ADDSIGNED2X
            | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH
            | D3DTEXOPCAPS_BLENDDIFFUSEALPHA
            | D3DTEXOPCAPS_BLENDTEXTUREALPHA
            | D3DTEXOPCAPS_BLENDFACTORALPHA
            | D3DTEXOPCAPS_BLENDTEXTUREALPHAPM
            | D3DTEXOPCAPS_BLENDCURRENTALPHA
            | D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_MULTIPLYADD
            | D3DTEXOPCAPS_LERP;
    caps->VolumeTextureAddressCaps = 0;
    caps->MaxTextureWidth = 4096;
    caps->MaxTextureHeight = 4096;
    caps->MaxVolumeExtent = 0;
    caps->MaxTextureRepeat = 8192;
    caps->MaxTextureAspectRatio = 4096;
    caps->MaxAnisotropy = 1;
    caps->MaxVertexW = 1.0e10f;
    caps->MaxPrimitiveCount = 0xFFFFFu;
    caps->MaxVertexIndex = 0xFFFFFFu;
    caps->MaxStreams = 1;
    caps->MaxStreamStride = 255;
    caps->VertexShaderVersion = (DWORD)D3DVS_VERSION(1, 1);
    caps->MaxVertexShaderConst = D8WG_MAX_VS_CONSTANTS;
    caps->PixelShaderVersion = (DWORD)D3DPS_VERSION(1, 4);
    caps->MaxPixelShaderValue = 8.0f;
    caps->FVFCaps = 2u & D3DFVFCAPS_TEXCOORDCOUNTMASK;
    caps->MaxTextureBlendStages = 2;
    caps->MaxSimultaneousTextures = 2;
    caps->MaxActiveLights = D8WG_MAX_LIGHTS;
    caps->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN
            | D3DVTXPCAPS_MATERIALSOURCE7
            | D3DVTXPCAPS_DIRECTIONALLIGHTS
            | D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
}

static HRESULT WINAPI d3d_query_interface(IDirect3D8 *iface, REFIID iid,
        void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid) && !guid_equal(iid, &IID_IDirect3D8)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3D8_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI d3d_add_ref(IDirect3D8 *iface)
{
    return (ULONG)InterlockedIncrement(&d3d_from_iface(iface)->refcount);
}

static ULONG WINAPI d3d_release(IDirect3D8 *iface)
{
    D8Direct3D *d3d = d3d_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&d3d->refcount);
    if (!refs)
        HeapFree(GetProcessHeap(), 0, d3d);
    return refs;
}

static HRESULT WINAPI d3d_register_software_device(IDirect3D8 *iface,
        void *initialize)
{
    (void)iface;
    (void)initialize;
    return D3DERR_INVALIDCALL;
}

static UINT WINAPI d3d_get_adapter_count(IDirect3D8 *iface)
{
    (void)iface;
    return 1;
}

static HRESULT WINAPI d3d_get_adapter_identifier(IDirect3D8 *iface,
        UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8 *identifier)
{
    (void)iface;
    (void)flags;
    if (adapter || !identifier)
        return D3DERR_INVALIDCALL;
    ZeroMemory(identifier, sizeof(*identifier));
    lstrcpynA(identifier->Driver, "d3d8-webgpu", sizeof(identifier->Driver));
    lstrcpynA(identifier->Description,
            "v86 Direct3D 8 WebGPU Adapter", sizeof(identifier->Description));
    identifier->VendorId = 0x1234;
    identifier->DeviceId = 0x5686;
    identifier->SubSysId = 0x56861234;
    identifier->Revision = 1;
    identifier->WHQLLevel = 0;
    return D3D_OK;
}

static UINT WINAPI d3d_get_adapter_mode_count(IDirect3D8 *iface, UINT adapter)
{
    (void)iface;
    return adapter ? 0 : 6;
}

static HRESULT WINAPI d3d_enum_adapter_modes(IDirect3D8 *iface, UINT adapter,
        UINT index, D3DDISPLAYMODE *mode)
{
    static const struct {
        UINT width;
        UINT height;
        D3DFORMAT format;
    } modes[] = {
        { 640, 480, D3DFMT_R5G6B5 },
        { 640, 480, D3DFMT_X8R8G8B8 },
        { 800, 600, D3DFMT_R5G6B5 },
        { 800, 600, D3DFMT_X8R8G8B8 },
        { 1024, 768, D3DFMT_R5G6B5 },
        { 1024, 768, D3DFMT_X8R8G8B8 }
    };
    (void)iface;
    if (adapter || !mode || index >= sizeof(modes) / sizeof(modes[0]))
        return D3DERR_INVALIDCALL;
    fill_display_mode(mode, modes[index].width, modes[index].height,
            modes[index].format);
    return D3D_OK;
}

static HRESULT WINAPI d3d_get_adapter_display_mode(IDirect3D8 *iface,
        UINT adapter, D3DDISPLAYMODE *mode)
{
    (void)iface;
    if (adapter || !mode)
        return D3DERR_INVALIDCALL;
    fill_display_mode(mode, 1024, 768, D3DFMT_X8R8G8B8);
    return D3D_OK;
}

static BOOL supported_backbuffer_format(D3DFORMAT format)
{
    return format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8
            || format == D3DFMT_R5G6B5;
}

static HRESULT WINAPI d3d_check_device_type(IDirect3D8 *iface, UINT adapter,
        D3DDEVTYPE type, D3DFORMAT display_format,
        D3DFORMAT backbuffer_format, WINBOOL windowed)
{
    (void)iface;
    (void)windowed;
    if (adapter || type != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;
    if ((display_format != D3DFMT_X8R8G8B8
            && display_format != D3DFMT_R5G6B5)
            || !supported_backbuffer_format(backbuffer_format))
        return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

static BOOL supported_texture_format(D3DFORMAT format)
{
    switch (format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_L8:
    case D3DFMT_A8:
    case D3DFMT_DXT1:
    case D3DFMT_DXT3:
    case D3DFMT_DXT5:
        return TRUE;
    default:
        return FALSE;
    }
}

static HRESULT WINAPI d3d_check_device_format(IDirect3D8 *iface,
        UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format,
        DWORD usage, D3DRESOURCETYPE resource_type, D3DFORMAT format)
{
    (void)iface;
    (void)adapter_format;
    if (adapter || type != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;
    if (resource_type == D3DRTYPE_TEXTURE
            && !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
            && supported_texture_format(format))
        return D3D_OK;
    if (resource_type == D3DRTYPE_TEXTURE
            && usage == D3DUSAGE_RENDERTARGET
            && (format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8))
        return D3D_OK;
    if (resource_type == D3DRTYPE_SURFACE
            && (usage & D3DUSAGE_DEPTHSTENCIL)
            && (format == D3DFMT_D16 || format == D3DFMT_D24S8))
        return D3D_OK;
    return D3DERR_NOTAVAILABLE;
}

static HRESULT WINAPI d3d_check_multisample(IDirect3D8 *iface, UINT adapter,
        D3DDEVTYPE type, D3DFORMAT format, WINBOOL windowed,
        D3DMULTISAMPLE_TYPE multisample)
{
    (void)iface;
    (void)format;
    (void)windowed;
    return !adapter && type == D3DDEVTYPE_HAL
            && multisample == D3DMULTISAMPLE_NONE
            ? D3D_OK : D3DERR_NOTAVAILABLE;
}

static HRESULT WINAPI d3d_check_depth_stencil(IDirect3D8 *iface,
        UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format,
        D3DFORMAT render_format, D3DFORMAT depth_format)
{
    (void)iface;
    (void)adapter_format;
    (void)render_format;
    if (adapter || type != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;
    return depth_format == D3DFMT_D16 || depth_format == D3DFMT_D24S8
            ? D3D_OK : D3DERR_NOTAVAILABLE;
}

static HRESULT WINAPI d3d_get_device_caps(IDirect3D8 *iface, UINT adapter,
        D3DDEVTYPE type, D3DCAPS8 *caps)
{
    (void)iface;
    if (adapter || type != D3DDEVTYPE_HAL || !caps)
        return D3DERR_INVALIDCALL;
    fill_caps(caps);
    return D3D_OK;
}

static HMONITOR WINAPI d3d_get_adapter_monitor(IDirect3D8 *iface,
        UINT adapter)
{
    (void)iface;
    if (adapter)
        return NULL;
    return MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
}

static void device_init_states(D8Device *device)
{
    UINT stage;
    UINT transform;
    ZeroMemory(device->render_states, sizeof(device->render_states));
    ZeroMemory(device->texture_stage_states,
            sizeof(device->texture_stage_states));
    device->render_states[D3DRS_ZENABLE] = D3DZB_TRUE;
    device->render_states[D3DRS_ZWRITEENABLE] = TRUE;
    device->render_states[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    device->render_states[D3DRS_CULLMODE] = D3DCULL_CCW;
    device->render_states[D3DRS_LIGHTING] = TRUE;
    device->render_states[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    device->render_states[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    device->render_states[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    device->render_states[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    device->render_states[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    device->render_states[D3DRS_COLORWRITEENABLE] = 0xFu;
    device->render_states[D3DRS_FOGEND] = 0x3F800000u;
    device->render_states[D3DRS_FOGDENSITY] = 0x3F800000u;
    device->render_states[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
    device->render_states[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    device->render_states[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    device->render_states[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    device->render_states[D3DRS_STENCILMASK] = 0xFFFFFFFFu;
    device->render_states[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFFu;
    device->render_states[D3DRS_COLORVERTEX] = TRUE;
    device->render_states[D3DRS_LOCALVIEWER] = TRUE;
    device->render_states[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    device->render_states[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    device->render_states[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
    device->render_states[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
    ZeroMemory(&device->material, sizeof(device->material));
    device->material.Diffuse.r = device->material.Diffuse.g =
            device->material.Diffuse.b = device->material.Diffuse.a = 1.0f;
    device->material.Ambient = device->material.Diffuse;
    ZeroMemory(device->lights, sizeof(device->lights));
    ZeroMemory(device->light_set, sizeof(device->light_set));
    ZeroMemory(device->light_enabled, sizeof(device->light_enabled));
    for (transform = 0; transform < D8WG_MAX_TRANSFORMS; ++transform) {
        ZeroMemory(&device->transforms[transform], sizeof(D3DMATRIX));
        device->transforms[transform].m[0][0] = 1.0f;
        device->transforms[transform].m[1][1] = 1.0f;
        device->transforms[transform].m[2][2] = 1.0f;
        device->transforms[transform].m[3][3] = 1.0f;
    }
    for (stage = 0; stage < D8WG_MAX_TEXTURE_STAGES; ++stage) {
        device->texture_stage_states[stage][D3DTSS_COLOROP] =
                stage == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        device->texture_stage_states[stage][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        device->texture_stage_states[stage][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        device->texture_stage_states[stage][D3DTSS_ALPHAOP] =
                stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        device->texture_stage_states[stage][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        device->texture_stage_states[stage][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        device->texture_stage_states[stage][D3DTSS_TEXCOORDINDEX] = stage;
        device->texture_stage_states[stage][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
        device->texture_stage_states[stage][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
        device->texture_stage_states[stage][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
        device->texture_stage_states[stage][D3DTSS_MINFILTER] = D3DTEXF_POINT;
        device->texture_stage_states[stage][D3DTSS_MIPFILTER] = D3DTEXF_NONE;
        device->texture_stage_states[stage][D3DTSS_MAXANISOTROPY] = 1;
        device->texture_stage_states[stage][D3DTSS_RESULTARG] = D3DTA_CURRENT;
    }
}

static BOOL emit_vertex_buffer_create(D8Device *device,
        D8VertexBuffer *buffer)
{
    D8WGCreateBuffer command;
    command.device_handle = device->handle;
    command.resource_handle = buffer->handle;
    command.resource_kind = D8WG_RESOURCE_BUFFER_VERTEX;
    command.byte_count = buffer->length;
    command.usage = buffer->usage;
    command.fvf = buffer->fvf;
    command.pool = buffer->pool;
    command.reserved = 0;
    return emit_command(D8WG_OP_CREATE_BUFFER, &command, sizeof(command));
}

static BOOL emit_index_buffer_create(D8Device *device,
        D8IndexBuffer *buffer)
{
    D8WGCreateBuffer command;
    command.device_handle = device->handle;
    command.resource_handle = buffer->handle;
    command.resource_kind = D8WG_RESOURCE_BUFFER_INDEX;
    command.byte_count = buffer->length;
    command.usage = buffer->usage;
    command.fvf = buffer->format;
    command.pool = buffer->pool;
    command.reserved = 0;
    return emit_command(D8WG_OP_CREATE_BUFFER, &command, sizeof(command));
}

static BOOL emit_texture_create(D8Device *device, D8Texture *texture)
{
    D8WGCreateTexture command;
    command.device_handle = device->handle;
    command.resource_handle = texture->handle;
    command.width = texture->width;
    command.height = texture->height;
    command.level_count = texture->level_count;
    command.format = texture->format;
    command.usage = texture->usage;
    command.pool = texture->pool;
    return emit_command(D8WG_OP_CREATE_TEXTURE, &command, sizeof(command));
}

static BOOL device_has_reset_blockers(D8Device *device)
{
    D8VertexBuffer *vb;
    D8IndexBuffer *ib;
    D8Texture *texture;
    UINT level;
    if (device->in_scene || device->recording_state_block
            || device->additional_swap_chain_count
            || device->implicit_surface_count)
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
    return FALSE;
}

static void device_clear_bindings(D8Device *device)
{
    UINT index;
    for (index = 0; index < D8WG_MAX_STREAMS; ++index) {
        D8VertexBuffer *buffer = device->streams[index].buffer;
        device->streams[index].buffer = NULL;
        device->streams[index].stride = 0;
        if (buffer) IDirect3DVertexBuffer8_Release(&buffer->iface);
    }
    if (device->index_buffer) {
        D8IndexBuffer *buffer = device->index_buffer;
        device->index_buffer = NULL;
        device->base_vertex_index = 0;
        IDirect3DIndexBuffer8_Release(&buffer->iface);
    }
    for (index = 0; index < D8WG_MAX_TEXTURE_STAGES; ++index) {
        D8Texture *texture = device->textures[index];
        device->textures[index] = NULL;
        if (texture) IDirect3DTexture8_Release(&texture->iface);
    }
    device->vertex_shader = 0;
    device->pixel_shader = 0;
    if (device->render_target_texture) {
        D8Texture *texture = device->render_target_texture;
        device->render_target_texture = NULL;
        device->render_target_level = 0;
        IDirect3DTexture8_Release(&texture->iface);
    }
    if (device->depth_surface) {
        D8Surface *surface = device->depth_surface;
        device->depth_surface = NULL;
        IDirect3DSurface8_Release(&surface->iface);
    }
    device->depth_surface_enabled = device->present.EnableAutoDepthStencil;
}

/*
 * D3D8 resources retain their parent device, while device state retains the
 * currently bound resources.  Track the parent-side references separately so
 * that releasing the last public device reference can drop device-owned state
 * and break that otherwise-uncollectable COM cycle.  Externally held resources
 * continue to keep the device alive and may still return it from GetDevice().
 */
static void device_release_owned_references(D8Device *device)
{
    D8StateBlock *block;
    D8StateBlock *recording;

    device_clear_bindings(device);

    block = device->state_blocks;
    device->state_blocks = NULL;
    while (block) {
        D8StateBlock *next = block->next;
        state_block_release_references(block);
        HeapFree(GetProcessHeap(), 0, block);
        block = next;
    }
    recording = device->recording_state_block;
    device->recording_state_block = NULL;
    if (recording) {
        state_block_release_references(recording);
        HeapFree(GetProcessHeap(), 0, recording);
    }
}

static BOOL recreate_device_resources(D8Device *device)
{
    D8VertexBuffer *vb;
    D8IndexBuffer *ib;
    D8Texture *texture;
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
    {
        D8Shader *shader;
        /* Unlike buffers and textures -- whose handles are private and hidden
         * behind COM pointers, so Reset can freely renumber them -- a shader
         * handle is the opaque DWORD the application itself holds and will
         * pass back to SetVertexShader after Reset. Keep it stable and let
         * the host re-establish the same handle under the new device epoch. */
        for (shader = device->vertex_shaders; shader; shader = shader->next) {
            if (!emit_create_vertex_shader(device, shader)) return FALSE;
        }
        for (shader = device->pixel_shaders; shader; shader = shader->next) {
            if (!emit_create_pixel_shader(device, shader)) return FALSE;
        }
        /* Host-side constant registers live under the old device_handle and
         * do not survive Reset; resend the guest shadow banks so a shader
         * bound again after Reset sees the same constants it had before. */
        if (!emit_set_shader_constant(D8WG_OP_SET_VERTEX_SHADER_CONSTANT,
                device, 0, &device->vs_constants[0][0],
                D8WG_MAX_VS_CONSTANTS))
            return FALSE;
        if (!emit_set_shader_constant(D8WG_OP_SET_PIXEL_SHADER_CONSTANT,
                device, 0, &device->ps_constants[0][0], D8WG_MAX_PS_CONSTANTS))
            return FALSE;
    }
    return TRUE;
}

static HRESULT WINAPI d3d_create_device(IDirect3D8 *iface, UINT adapter,
        D3DDEVTYPE type, HWND focus_window, DWORD behavior,
        D3DPRESENT_PARAMETERS *parameters, IDirect3DDevice8 **device_out)
{
    D8Direct3D *d3d = d3d_from_iface(iface);
    D8Device *device;
    D8WGCreateDevice command;
    HWND window;
    RECT client;
    POINT origin;
    UINT front_row_count;
    UINT front_byte_count;

    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = NULL;
    if (adapter || type != D3DDEVTYPE_HAL || !parameters)
        return D3DERR_INVALIDCALL;
    if (parameters->MultiSampleType != D3DMULTISAMPLE_NONE)
        return D3DERR_NOTAVAILABLE;
    if (parameters->EnableAutoDepthStencil
            && parameters->AutoDepthStencilFormat != D3DFMT_D16
            && parameters->AutoDepthStencilFormat != D3DFMT_D24S8)
        return D3DERR_NOTAVAILABLE;
    if (parameters->BackBufferFormat != D3DFMT_UNKNOWN
            && !supported_backbuffer_format(parameters->BackBufferFormat))
        return D3DERR_NOTAVAILABLE;
    if (parameters->BackBufferWidth > 8192
            || parameters->BackBufferHeight > 8192)
        return D3DERR_INVALIDCALL;
    /* D3D8 permits 0..3 back buffers (0 meaning 1). The host always presents
     * through a single WebGPU surface, so extra back buffers are a
     * presentation detail we can satisfy with one target; only a request
     * beyond the D3D8 maximum is a genuine error. */
    if (parameters->BackBufferCount > 3)
        return D3DERR_INVALIDCALL;

    device = (D8Device *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*device));
    if (!device)
        return E_OUTOFMEMORY;
    device->iface.lpVtbl = &g_device_vtbl;
    device->refcount = 1;
    device->parent = d3d;
    IDirect3D8_AddRef(iface);
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
    device->depth_surface_enabled = parameters->EnableAutoDepthStencil;
    if (!texture_level_layout(device->display_mode.Format,
            device->display_mode.Width, device->display_mode.Height,
            &device->front_shadow_pitch, &front_row_count,
            &front_byte_count)
            || front_row_count != device->display_mode.Height) {
        IDirect3D8_Release(iface);
        HeapFree(GetProcessHeap(), 0, device);
        return D3DERR_NOTAVAILABLE;
    }
    device->front_shadow = (BYTE *)HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, front_byte_count);
    if (!device->front_shadow) {
        IDirect3D8_Release(iface);
        HeapFree(GetProcessHeap(), 0, device);
        return E_OUTOFMEMORY;
    }

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
    if (!command.width)
        command.width = 640;
    if (!command.height)
        command.height = 480;
    command.backbuffer_format = device->display_mode.Format;
    command.windowed = parameters->Windowed;
    command.behavior_flags = behavior;
    command.enable_auto_depth_stencil = parameters->EnableAutoDepthStencil;
    command.auto_depth_stencil_format = parameters->AutoDepthStencilFormat;
    if (!emit_command(D8WG_OP_CREATE_DEVICE, &command, sizeof(command))) {
        IDirect3D8_Release(iface);
        HeapFree(GetProcessHeap(), 0, device->front_shadow);
        HeapFree(GetProcessHeap(), 0, device);
        return D3DERR_DRIVERINTERNALERROR;
    }
    attach_device_window(device, window);
    capture_surface(device, window, &device->last_surface);
    device->has_last_surface = TRUE;

    *device_out = &device->iface;
    return D3D_OK;
}

static HRESULT WINAPI device_query_interface(IDirect3DDevice8 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DDevice8)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DDevice8_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI device_add_ref(IDirect3DDevice8 *iface)
{
    return (ULONG)InterlockedIncrement(&device_from_iface(iface)->refcount);
}

static ULONG WINAPI device_release(IDirect3DDevice8 *iface)
{
    D8Device *device = device_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&device->refcount);

    if (device->releasing_owned_refs)
        return refs;

    if ((LONG)refs == InterlockedCompareExchange(
            &device->child_parent_refs, 0, 0)) {
        device->releasing_owned_refs = TRUE;
        device_release_owned_references(device);
        device->releasing_owned_refs = FALSE;
        refs = (ULONG)InterlockedCompareExchange(&device->refcount, 0, 0);
    }
    if (!refs) {
        detach_device_window(device);
        emit_device_destroy_and_flush(device->handle);
        IDirect3D8_Release(&device->parent->iface);
        /* Shaders are not COM objects (no per-object refcount/Release), so
         * unlike vertex_buffers/index_buffers/texture_resources -- which are
         * always already empty here because each resource unlinks and frees
         * itself from its own Release() -- any shader the app never called
         * Delete{Vertex,Pixel}Shader on must be freed here. */
        free_shader_list(device->vertex_shaders);
        free_shader_list(device->pixel_shaders);
        HeapFree(GetProcessHeap(), 0, device->front_shadow);
        HeapFree(GetProcessHeap(), 0, device);
    }
    return refs;
}

static void device_child_add_ref(D8Device *device)
{
    InterlockedIncrement(&device->child_parent_refs);
    IDirect3DDevice8_AddRef(&device->iface);
}

static void device_child_release(D8Device *device)
{
    InterlockedDecrement(&device->child_parent_refs);
    IDirect3DDevice8_Release(&device->iface);
}

static HRESULT WINAPI device_test_cooperative_level(IDirect3DDevice8 *iface)
{
    (void)iface;
    return D3D_OK;
}

static UINT WINAPI device_get_available_texture_mem(IDirect3DDevice8 *iface)
{
    (void)iface;
    return 128u * 1024u * 1024u;
}

static HRESULT WINAPI device_discard_bytes(IDirect3DDevice8 *iface, DWORD bytes)
{
    (void)iface;
    (void)bytes;
    return D3D_OK;
}

static HRESULT WINAPI device_get_direct3d(IDirect3DDevice8 *iface,
        IDirect3D8 **d3d_out)
{
    D8Device *device = device_from_iface(iface);
    if (!d3d_out)
        return D3DERR_INVALIDCALL;
    *d3d_out = &device->parent->iface;
    IDirect3D8_AddRef(*d3d_out);
    return D3D_OK;
}

static HRESULT WINAPI device_get_caps(IDirect3DDevice8 *iface, D3DCAPS8 *caps)
{
    (void)iface;
    if (!caps)
        return D3DERR_INVALIDCALL;
    fill_caps(caps);
    return D3D_OK;
}

static HRESULT WINAPI device_get_display_mode(IDirect3DDevice8 *iface,
        D3DDISPLAYMODE *mode)
{
    if (!mode)
        return D3DERR_INVALIDCALL;
    *mode = device_from_iface(iface)->display_mode;
    return D3D_OK;
}

static HRESULT WINAPI device_get_creation_parameters(IDirect3DDevice8 *iface,
        D3DDEVICE_CREATION_PARAMETERS *parameters)
{
    if (!parameters)
        return D3DERR_INVALIDCALL;
    *parameters = device_from_iface(iface)->creation;
    return D3D_OK;
}

static HRESULT WINAPI device_reset(IDirect3DDevice8 *iface,
        D3DPRESENT_PARAMETERS *parameters)
{
    D8Device *device = device_from_iface(iface);
    D8WGResetDevice reset;
    RECT client;
    POINT origin;
    HWND window;
    BYTE *new_front_shadow;
    UINT new_front_pitch;
    UINT new_front_rows;
    UINT new_front_bytes;

    if (!parameters || parameters->MultiSampleType != D3DMULTISAMPLE_NONE
            || parameters->BackBufferCount > 1
            || device_has_reset_blockers(device))
        return D3DERR_INVALIDCALL;
    if (parameters->EnableAutoDepthStencil
            && parameters->AutoDepthStencilFormat != D3DFMT_D16
            && parameters->AutoDepthStencilFormat != D3DFMT_D24S8)
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
    if (!reset.width)
        reset.width = device->display_mode.Width;
    if (!reset.height)
        reset.height = device->display_mode.Height;
    reset.backbuffer_format = parameters->BackBufferFormat == D3DFMT_UNKNOWN
            ? device->display_mode.Format : parameters->BackBufferFormat;
    reset.windowed = parameters->Windowed;
    reset.behavior_flags = device->creation.BehaviorFlags;
    reset.enable_auto_depth_stencil = parameters->EnableAutoDepthStencil;
    reset.auto_depth_stencil_format = parameters->AutoDepthStencilFormat;
    if (reset.width > 8192 || reset.height > 8192)
        return D3DERR_INVALIDCALL;
    if (!texture_level_layout((D3DFORMAT)reset.backbuffer_format,
            reset.width, reset.height, &new_front_pitch, &new_front_rows,
            &new_front_bytes) || new_front_rows != reset.height)
        return D3DERR_NOTAVAILABLE;
    new_front_shadow = (BYTE *)HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, new_front_bytes);
    if (!new_front_shadow)
        return E_OUTOFMEMORY;
    if (!emit_command(D8WG_OP_RESET, &reset, sizeof(reset)))
    {
        HeapFree(GetProcessHeap(), 0, new_front_shadow);
        return D3DERR_DRIVERINTERNALERROR;
    }
    device_clear_bindings(device);
    HeapFree(GetProcessHeap(), 0, device->front_shadow);
    device->front_shadow = new_front_shadow;
    device->front_shadow_pitch = new_front_pitch;
    device->handle = reset.new_device_handle;
    ++device->reset_epoch;
    device->present = *parameters;
    fill_display_mode(&device->display_mode, reset.width, reset.height,
            reset.backbuffer_format);
    device_init_states(device);
    device->viewport.X = device->viewport.Y = 0;
    device->viewport.Width = reset.width;
    device->viewport.Height = reset.height;
    device->viewport.MinZ = 0.0f;
    device->viewport.MaxZ = 1.0f;
    device->depth_surface_enabled = parameters->EnableAutoDepthStencil;
    if (!recreate_device_resources(device))
        return D3DERR_DRIVERINTERNALERROR;
    if (window != device->tracked_window) {
        detach_device_window(device);
        attach_device_window(device, window);
    }
    capture_surface(device, window, &device->last_surface);
    device->has_last_surface = TRUE;
    return D3D_OK;
}

static HRESULT WINAPI device_present(IDirect3DDevice8 *iface,
        const RECT *source, const RECT *destination, HWND override_window,
        const RGNDATA *dirty_region)
{
    (void)source;
    (void)destination;
    (void)dirty_region;
    return emit_present_and_flush(device_from_iface(iface), override_window)
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_begin_scene(IDirect3DDevice8 *iface)
{
    D8Device *device = device_from_iface(iface);
    D8WGDeviceOnly command;
    if (device->in_scene)
        return D3DERR_INVALIDCALL;
    device->in_scene = TRUE;
    command.device_handle = device->handle;
    command.reserved = 0;
    return emit_command(D8WG_OP_BEGIN_SCENE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_end_scene(IDirect3DDevice8 *iface)
{
    D8Device *device = device_from_iface(iface);
    D8WGDeviceOnly command;
    if (!device->in_scene)
        return D3DERR_INVALIDCALL;
    device->in_scene = FALSE;
    command.device_handle = device->handle;
    command.reserved = 0;
    return emit_command(D8WG_OP_END_SCENE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_clear(IDirect3DDevice8 *iface, DWORD rect_count,
        const D3DRECT *rects, DWORD flags, D3DCOLOR color, float depth,
        DWORD stencil)
{
    D8Device *device = device_from_iface(iface);
    D8WGClear command;
    D8WGCommandHeader *header;
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
    command.depth = depth;
    command.stencil = stencil;
    command.rect_count = rect_count;

    EnterCriticalSection(&g_transport_lock);
    result = reserve_command_locked(D8WG_OP_CLEAR, sizeof(command), rect_bytes,
            &header, &payload, &rect_data);
    if (result) {
        CopyMemory(payload, &command, sizeof(command));
        if (rect_bytes)
            CopyMemory(rect_data, rects, rect_bytes);
        (void)header;
    }
    LeaveCriticalSection(&g_transport_lock);
    if (result && (flags & D3DCLEAR_TARGET)) {
        BYTE *shadow = device->front_shadow;
        UINT pitch = device->front_shadow_pitch;
        UINT width = device->display_mode.Width;
        UINT height = device->display_mode.Height;
        D3DFORMAT format = device->display_mode.Format;
        UINT block_width;
        UINT block_height;
        UINT pixel_bytes;
        UINT rect_index;
        if (device->render_target_texture) {
            D8TextureLevel *level = &device->render_target_texture->levels[
                    device->render_target_level];
            shadow = level->shadow;
            pitch = level->row_pitch;
            width = level->width;
            height = level->height;
            format = device->render_target_texture->format;
        }
        if (!texture_format_layout(format, &block_width, &block_height,
                &pixel_bytes) || block_width != 1 || block_height != 1)
            return D3DERR_DRIVERINTERNALERROR;
        for (rect_index = 0; rect_index < (rect_count ? rect_count : 1u);
                ++rect_index) {
            LONG left = rect_count ? rects[rect_index].x1 : 0;
            LONG top = rect_count ? rects[rect_index].y1 : 0;
            LONG right = rect_count ? rects[rect_index].x2 : (LONG)width;
            LONG bottom = rect_count ? rects[rect_index].y2 : (LONG)height;
            LONG y;
            if (left < 0) left = 0;
            if (top < 0) top = 0;
            if (right > (LONG)width) right = (LONG)width;
            if (bottom > (LONG)height) bottom = (LONG)height;
            for (y = top; y < bottom; ++y) {
                BYTE *row = shadow + y * pitch;
                LONG x;
                for (x = left; x < right; ++x)
                    write_surface_color(row + x * pixel_bytes, format, color);
            }
        }
    }
    return result ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_set_viewport(IDirect3DDevice8 *iface,
        const D3DVIEWPORT8 *viewport)
{
    D8Device *device = device_from_iface(iface);
    D8WGSetViewport command;
    if (!viewport || !viewport->Width || !viewport->Height)
        return D3DERR_INVALIDCALL;
    if (viewport->X > device->display_mode.Width
            || viewport->Y > device->display_mode.Height
            || viewport->Width > device->display_mode.Width - viewport->X
            || viewport->Height > device->display_mode.Height - viewport->Y
            || viewport->MinZ < 0.0f || viewport->MaxZ > 1.0f
            || viewport->MinZ > viewport->MaxZ)
        return D3DERR_INVALIDCALL;
    if (device->recording_state_block)
        device->recording_state_block->state.viewport_mask = TRUE;
    if (device->viewport.X == viewport->X
            && device->viewport.Y == viewport->Y
            && device->viewport.Width == viewport->Width
            && device->viewport.Height == viewport->Height
            && device->viewport.MinZ == viewport->MinZ
            && device->viewport.MaxZ == viewport->MaxZ)
        return D3D_OK;
    device->viewport = *viewport;
    command.device_handle = device->handle;
    command.x = viewport->X;
    command.y = viewport->Y;
    command.width = viewport->Width;
    command.height = viewport->Height;
    command.min_z = viewport->MinZ;
    command.max_z = viewport->MaxZ;
    command.reserved = 0;
    return emit_command(D8WG_OP_SET_VIEWPORT, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_viewport(IDirect3DDevice8 *iface,
        D3DVIEWPORT8 *viewport)
{
    if (!viewport)
        return D3DERR_INVALIDCALL;
    *viewport = device_from_iface(iface)->viewport;
    return D3D_OK;
}

static HRESULT WINAPI device_set_render_state(IDirect3DDevice8 *iface,
        D3DRENDERSTATETYPE state, DWORD value)
{
    D8Device *device = device_from_iface(iface);
    D8WGSetRenderState command;
    if ((UINT)state >= D8WG_MAX_RENDER_STATES)
        return D3DERR_INVALIDCALL;
    if (device->recording_state_block)
        device->recording_state_block->state.render_mask[state] = 1;
    if (device->render_states[state] == value)
        return D3D_OK;
    device->render_states[state] = value;
    command.device_handle = device->handle;
    command.state = state;
    command.value = value;
    command.reserved = 0;
    return emit_command(D8WG_OP_SET_RENDER_STATE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_render_state(IDirect3DDevice8 *iface,
        D3DRENDERSTATETYPE state, DWORD *value)
{
    if (!value || (UINT)state >= D8WG_MAX_RENDER_STATES)
        return D3DERR_INVALIDCALL;
    *value = device_from_iface(iface)->render_states[state];
    return D3D_OK;
}

static HRESULT WINAPI device_set_texture_stage_state(IDirect3DDevice8 *iface,
        DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD value)
{
    D8Device *device = device_from_iface(iface);
    D8WGSetTextureStageState command;
    if (stage >= D8WG_MAX_TEXTURE_STAGES
            || (UINT)state >= D8WG_MAX_TEXTURE_STAGE_STATES)
        return D3DERR_INVALIDCALL;
    if (device->recording_state_block)
        device->recording_state_block->state.texture_stage_mask[stage][state]
                = 1;
    if (device->texture_stage_states[stage][state] == value)
        return D3D_OK;
    device->texture_stage_states[stage][state] = value;
    command.device_handle = device->handle;
    command.stage = stage;
    command.state = state;
    command.value = value;
    return emit_command(D8WG_OP_SET_TEXTURE_STAGE_STATE,
            &command, sizeof(command)) ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_texture_stage_state(IDirect3DDevice8 *iface,
        DWORD stage, D3DTEXTURESTAGESTATETYPE state, DWORD *value)
{
    if (!value || stage >= D8WG_MAX_TEXTURE_STAGES
            || (UINT)state >= D8WG_MAX_TEXTURE_STAGE_STATES)
        return D3DERR_INVALIDCALL;
    *value = device_from_iface(iface)->texture_stage_states[stage][state];
    return D3D_OK;
}

static HRESULT WINAPI device_create_vertex_buffer(IDirect3DDevice8 *iface,
        UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
        IDirect3DVertexBuffer8 **buffer_out)
{
    D8Device *device = device_from_iface(iface);
    D8VertexBuffer *buffer;
    if (!buffer_out)
        return D3DERR_INVALIDCALL;
    *buffer_out = NULL;
    if (!length || pool > D3DPOOL_SCRATCH)
        return D3DERR_INVALIDCALL;
    buffer = (D8VertexBuffer *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*buffer));
    if (!buffer)
        return E_OUTOFMEMORY;
    buffer->shadow = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            length);
    if (!buffer->shadow) {
        HeapFree(GetProcessHeap(), 0, buffer);
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
        device_child_release(device);
        HeapFree(GetProcessHeap(), 0, buffer->shadow);
        HeapFree(GetProcessHeap(), 0, buffer);
        return D3DERR_DRIVERINTERNALERROR;
    }
    buffer->next_device_resource = device->vertex_buffers;
    device->vertex_buffers = buffer;
    *buffer_out = &buffer->iface;
    return D3D_OK;
}

static HRESULT WINAPI device_create_index_buffer(IDirect3DDevice8 *iface,
        UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
        IDirect3DIndexBuffer8 **buffer_out)
{
    D8Device *device = device_from_iface(iface);
    D8IndexBuffer *buffer;
    UINT index_size;

    if (!buffer_out)
        return D3DERR_INVALIDCALL;
    *buffer_out = NULL;
    if (!length || pool > D3DPOOL_SCRATCH)
        return D3DERR_INVALIDCALL;
    if (format == D3DFMT_INDEX16)
        index_size = 2;
    else if (format == D3DFMT_INDEX32)
        index_size = 4;
    else
        return D3DERR_INVALIDCALL;
    if (length % index_size)
        return D3DERR_INVALIDCALL;

    buffer = (D8IndexBuffer *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*buffer));
    if (!buffer)
        return E_OUTOFMEMORY;
    buffer->shadow = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            length);
    if (!buffer->shadow) {
        HeapFree(GetProcessHeap(), 0, buffer);
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
        device_child_release(device);
        HeapFree(GetProcessHeap(), 0, buffer->shadow);
        HeapFree(GetProcessHeap(), 0, buffer);
        return D3DERR_DRIVERINTERNALERROR;
    }
    buffer->next_device_resource = device->index_buffers;
    device->index_buffers = buffer;
    *buffer_out = &buffer->iface;
    return D3D_OK;
}

static HRESULT WINAPI device_set_stream_source(IDirect3DDevice8 *iface,
        UINT stream, IDirect3DVertexBuffer8 *buffer_iface, UINT stride)
{
    D8Device *device = device_from_iface(iface);
    D8VertexBuffer *buffer = buffer_iface ? vb_from_iface(buffer_iface) : NULL;
    D8WGSetStreamSource command;
    if (stream >= D8WG_MAX_STREAMS)
        return D3DERR_INVALIDCALL;
    if (buffer && (buffer_iface->lpVtbl != &g_vb_vtbl
            || buffer->device != device))
        return D3DERR_INVALIDCALL;
    if (device->recording_state_block)
        device->recording_state_block->state.stream_mask[stream] = 1;
    if (device->streams[stream].buffer == buffer
            && device->streams[stream].stride == stride)
        return D3D_OK;
    if (buffer)
        IDirect3DVertexBuffer8_AddRef(buffer_iface);
    if (device->streams[stream].buffer)
        IDirect3DVertexBuffer8_Release(
                &device->streams[stream].buffer->iface);
    device->streams[stream].buffer = buffer;
    device->streams[stream].stride = stride;
    command.device_handle = device->handle;
    command.stream = stream;
    command.buffer_handle = buffer ? buffer->handle : 0;
    command.stride = stride;
    return emit_command(D8WG_OP_SET_STREAM_SOURCE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_stream_source(IDirect3DDevice8 *iface,
        UINT stream, IDirect3DVertexBuffer8 **buffer_out, UINT *stride_out)
{
    D8Device *device = device_from_iface(iface);
    if (stream >= D8WG_MAX_STREAMS || !buffer_out || !stride_out)
        return D3DERR_INVALIDCALL;
    *stride_out = device->streams[stream].stride;
    *buffer_out = device->streams[stream].buffer
            ? &device->streams[stream].buffer->iface : NULL;
    if (*buffer_out)
        IDirect3DVertexBuffer8_AddRef(*buffer_out);
    return D3D_OK;
}

static HRESULT WINAPI device_set_indices(IDirect3DDevice8 *iface,
        IDirect3DIndexBuffer8 *buffer_iface, UINT base_vertex_index)
{
    D8Device *device = device_from_iface(iface);
    D8IndexBuffer *buffer = buffer_iface ? ib_from_iface(buffer_iface) : NULL;
    D8WGSetIndices command;

    if (base_vertex_index > 0x7FFFFFFFu
            || (buffer && (buffer_iface->lpVtbl != &g_ib_vtbl
                || buffer->device != device)))
        return D3DERR_INVALIDCALL;
    if (device->recording_state_block)
        device->recording_state_block->state.indices_mask = TRUE;
    if (device->index_buffer == buffer
            && device->base_vertex_index == base_vertex_index)
        return D3D_OK;
    if (buffer)
        IDirect3DIndexBuffer8_AddRef(buffer_iface);
    if (device->index_buffer)
        IDirect3DIndexBuffer8_Release(&device->index_buffer->iface);
    device->index_buffer = buffer;
    device->base_vertex_index = base_vertex_index;
    command.device_handle = device->handle;
    command.buffer_handle = buffer ? buffer->handle : 0;
    command.base_vertex_index = base_vertex_index;
    command.reserved = 0;
    return emit_command(D8WG_OP_SET_INDICES, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_indices(IDirect3DDevice8 *iface,
        IDirect3DIndexBuffer8 **buffer_out, UINT *base_vertex_index_out)
{
    D8Device *device = device_from_iface(iface);
    if (!buffer_out || !base_vertex_index_out)
        return D3DERR_INVALIDCALL;
    *buffer_out = device->index_buffer ? &device->index_buffer->iface : NULL;
    *base_vertex_index_out = device->base_vertex_index;
    if (*buffer_out)
        IDirect3DIndexBuffer8_AddRef(*buffer_out);
    return D3D_OK;
}

static HRESULT WINAPI device_set_vertex_shader(IDirect3DDevice8 *iface,
        DWORD handle)
{
    D8Device *device = device_from_iface(iface);
    /* D3D8 shader handles always carry bit 0; raw FVF tokens never do. */
    if (handle & 1u) {
        if (!find_shader(device->vertex_shaders, handle))
            return D3DERR_INVALIDCALL;
        if (device->recording_state_block)
            device->recording_state_block->state.vertex_shader_mask = TRUE;
        if (device->vertex_shader == handle)
            return D3D_OK;
        device->vertex_shader = handle;
        return emit_set_shader(D8WG_OP_SET_VERTEX_SHADER, device, handle)
                ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
    }
    {
        D8WGSetVertexFormat command;
        if (device->recording_state_block)
            device->recording_state_block->state.vertex_shader_mask = TRUE;
        if (device->vertex_shader == handle)
            return D3D_OK;
        device->vertex_shader = handle;
        command.device_handle = device->handle;
        command.fvf = handle;
        return emit_command(D8WG_OP_SET_VERTEX_FORMAT, &command,
                sizeof(command)) ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
    }
}

static HRESULT WINAPI device_get_vertex_shader(IDirect3DDevice8 *iface,
        DWORD *handle)
{
    if (!handle)
        return D3DERR_INVALIDCALL;
    *handle = device_from_iface(iface)->vertex_shader;
    return D3D_OK;
}

static HRESULT WINAPI device_draw_primitive(IDirect3DDevice8 *iface,
        D3DPRIMITIVETYPE primitive_type, UINT start_vertex,
        UINT primitive_count)
{
    D8Device *device = device_from_iface(iface);
    D8WGDrawPrimitive command;
    UINT vertex_count;
    UINT available_vertices;
    if (!device->streams[0].buffer || !device->streams[0].stride
            || device->streams[0].buffer->locked
            || !device->vertex_shader || !primitive_count
            || !primitive_element_count(primitive_type, primitive_count,
                    &vertex_count))
        return D3DERR_INVALIDCALL;
    available_vertices = device->streams[0].buffer->length
            / device->streams[0].stride;
    if (start_vertex > available_vertices
            || vertex_count > available_vertices - start_vertex)
        return D3DERR_INVALIDCALL;
    command.device_handle = device->handle;
    command.primitive_type = primitive_type;
    command.start_vertex = start_vertex;
    command.primitive_count = primitive_count;
    return emit_command(D8WG_OP_DRAW_PRIMITIVE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_draw_indexed(IDirect3DDevice8 *iface,
        D3DPRIMITIVETYPE primitive_type, UINT min_vertex_index,
        UINT vertex_count, UINT start_index, UINT primitive_count)
{
    D8Device *device = device_from_iface(iface);
    D8WGDrawIndexedPrimitive command;
    UINT index_count;
    UINT index_size;
    UINT available_indices;
    UINT available_vertices;
    UINT first_vertex;

    if (!device->streams[0].buffer || !device->streams[0].stride
            || device->streams[0].buffer->locked || !device->index_buffer
            || device->index_buffer->locked || !device->vertex_shader
            || !primitive_count || !vertex_count
            || !primitive_element_count(primitive_type, primitive_count,
                    &index_count))
        return D3DERR_INVALIDCALL;
    index_size = device->index_buffer->format == D3DFMT_INDEX16 ? 2u : 4u;
    available_indices = device->index_buffer->length / index_size;
    if (start_index > available_indices
            || index_count > available_indices - start_index)
        return D3DERR_INVALIDCALL;
    available_vertices = device->streams[0].buffer->length
            / device->streams[0].stride;
    if (device->base_vertex_index > 0xFFFFFFFFu - min_vertex_index)
        return D3DERR_INVALIDCALL;
    first_vertex = device->base_vertex_index + min_vertex_index;
    if (first_vertex > available_vertices
            || vertex_count > available_vertices - first_vertex)
        return D3DERR_INVALIDCALL;

    command.device_handle = device->handle;
    command.primitive_type = primitive_type;
    command.min_vertex_index = min_vertex_index;
    command.vertex_count = vertex_count;
    command.start_index = start_index;
    command.primitive_count = primitive_count;
    return emit_command(D8WG_OP_DRAW_INDEXED_PRIMITIVE,
            &command, sizeof(command)) ? D3D_OK
            : D3DERR_DRIVERINTERNALERROR;
}

static void clear_stream_zero_after_up(D8Device *device)
{
    D8VertexBuffer *old = device->streams[0].buffer;
    device->streams[0].buffer = NULL;
    device->streams[0].stride = 0;
    if (old)
        IDirect3DVertexBuffer8_Release(&old->iface);
}

static void clear_indices_after_indexed_up(D8Device *device)
{
    D8IndexBuffer *old = device->index_buffer;
    device->index_buffer = NULL;
    device->base_vertex_index = 0;
    if (old)
        IDirect3DIndexBuffer8_Release(&old->iface);
}

static HRESULT WINAPI device_draw_up(IDirect3DDevice8 *iface,
        D3DPRIMITIVETYPE primitive_type, UINT primitive_count,
        const void *vertex_data, UINT stride)
{
    D8Device *device = device_from_iface(iface);
    D8WGDrawPrimitiveUP command;
    UINT vertex_count;
    UINT vertex_bytes;
    BOOL result;

    if (!vertex_data || !stride || !device->vertex_shader || !primitive_count
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
    if (result)
        clear_stream_zero_after_up(device);
    return result ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_draw_indexed_up(IDirect3DDevice8 *iface,
        D3DPRIMITIVETYPE primitive_type, UINT min_vertex_index,
        UINT vertex_count, UINT primitive_count, const void *index_data,
        D3DFORMAT index_format, const void *vertex_data, UINT stride)
{
    D8Device *device = device_from_iface(iface);
    D8WGDrawIndexedPrimitiveUP command;
    UINT index_count;
    UINT index_size;
    UINT vertex_elements;
    BOOL result;

    if (!index_data || !vertex_data || !stride || !device->vertex_shader
            || !primitive_count || !vertex_count
            || !primitive_element_count(primitive_type, primitive_count,
                    &index_count))
        return D3DERR_INVALIDCALL;
    if (index_format == D3DFMT_INDEX16)
        index_size = 2;
    else if (index_format == D3DFMT_INDEX32)
        index_size = 4;
    else
        return D3DERR_INVALIDCALL;
    if (min_vertex_index > 0xFFFFFFFFu - vertex_count)
        return D3DERR_INVALIDCALL;
    vertex_elements = min_vertex_index + vertex_count;
    ZeroMemory(&command, sizeof(command));
    if (!multiply_u32(index_count, index_size, &command.index_bytes)
            || !multiply_u32(vertex_elements, stride,
                    &command.vertex_bytes))
        return D3DERR_INVALIDCALL;
    command.device_handle = device->handle;
    command.primitive_type = primitive_type;
    command.min_vertex_index = min_vertex_index;
    command.vertex_count = vertex_count;
    command.primitive_count = primitive_count;
    command.index_format = index_format;
    command.stride = stride;
    command.index_count = index_count;
    result = emit_draw_indexed_primitive_up(&command, index_data,
            vertex_data);
    if (result) {
        clear_stream_zero_after_up(device);
        clear_indices_after_indexed_up(device);
    }
    return result ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_validate(IDirect3DDevice8 *iface, DWORD *passes)
{
    (void)iface;
    if (!passes)
        return D3DERR_INVALIDCALL;
    *passes = 1;
    return D3D_OK;
}

static HRESULT WINAPI vb_query_interface(IDirect3DVertexBuffer8 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DResource8)
            && !guid_equal(iid, &IID_IDirect3DVertexBuffer8)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DVertexBuffer8_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI vb_add_ref(IDirect3DVertexBuffer8 *iface)
{
    return (ULONG)InterlockedIncrement(&vb_from_iface(iface)->refcount);
}

static ULONG WINAPI vb_release(IDirect3DVertexBuffer8 *iface)
{
    D8VertexBuffer *buffer = vb_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&buffer->refcount);
    if (!refs) {
        D8VertexBuffer **link = &buffer->device->vertex_buffers;
        D8WGDestroyResource destroy;
        while (*link && *link != buffer)
            link = &(*link)->next_device_resource;
        if (*link) *link = buffer->next_device_resource;
        destroy.resource_handle = buffer->handle;
        destroy.resource_kind = D8WG_RESOURCE_BUFFER_VERTEX;
        emit_command(D8WG_OP_DESTROY_RESOURCE, &destroy, sizeof(destroy));
        HeapFree(GetProcessHeap(), 0, buffer->shadow);
        device_child_release(buffer->device);
        HeapFree(GetProcessHeap(), 0, buffer);
    }
    return refs;
}

static HRESULT WINAPI vb_get_device(IDirect3DVertexBuffer8 *iface,
        IDirect3DDevice8 **device_out)
{
    D8VertexBuffer *buffer = vb_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &buffer->device->iface;
    IDirect3DDevice8_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI vb_set_private_data(IDirect3DVertexBuffer8 *iface,
        REFGUID guid, const void *data, DWORD size, DWORD flags)
{
    (void)iface; (void)guid; (void)data; (void)size; (void)flags;
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI vb_get_private_data(IDirect3DVertexBuffer8 *iface,
        REFGUID guid, void *data, DWORD *size)
{
    (void)iface; (void)guid; (void)data; (void)size;
    return D3DERR_NOTFOUND;
}

static HRESULT WINAPI vb_free_private_data(IDirect3DVertexBuffer8 *iface,
        REFGUID guid)
{
    (void)iface; (void)guid;
    return D3DERR_NOTFOUND;
}

static DWORD WINAPI vb_set_priority(IDirect3DVertexBuffer8 *iface,
        DWORD priority)
{
    D8VertexBuffer *buffer = vb_from_iface(iface);
    DWORD old = buffer->priority;
    buffer->priority = priority;
    return old;
}

static DWORD WINAPI vb_get_priority(IDirect3DVertexBuffer8 *iface)
{
    return vb_from_iface(iface)->priority;
}

static void WINAPI vb_preload(IDirect3DVertexBuffer8 *iface)
{
    (void)iface;
}

static D3DRESOURCETYPE WINAPI vb_get_type(IDirect3DVertexBuffer8 *iface)
{
    (void)iface;
    return D3DRTYPE_VERTEXBUFFER;
}

static HRESULT WINAPI vb_lock(IDirect3DVertexBuffer8 *iface, UINT offset,
        UINT size, BYTE **data_out, DWORD flags)
{
    D8VertexBuffer *buffer = vb_from_iface(iface);
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

static HRESULT WINAPI vb_unlock(IDirect3DVertexBuffer8 *iface)
{
    D8VertexBuffer *buffer = vb_from_iface(iface);
    BOOL result;
    if (!buffer->locked)
        return D3DERR_INVALIDCALL;
    result = (buffer->lock_flags & D3DLOCK_READONLY) ||
            emit_buffer_update(buffer->handle, buffer->lock_offset,
            buffer->shadow + buffer->lock_offset, buffer->lock_size,
            buffer->lock_flags);
    buffer->locked = FALSE;
    buffer->lock_offset = 0;
    buffer->lock_size = 0;
    buffer->lock_flags = 0;
    return result ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI vb_get_desc(IDirect3DVertexBuffer8 *iface,
        D3DVERTEXBUFFER_DESC *desc)
{
    D8VertexBuffer *buffer = vb_from_iface(iface);
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

static HRESULT WINAPI ib_query_interface(IDirect3DIndexBuffer8 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DResource8)
            && !guid_equal(iid, &IID_IDirect3DIndexBuffer8)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DIndexBuffer8_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI ib_add_ref(IDirect3DIndexBuffer8 *iface)
{
    return (ULONG)InterlockedIncrement(&ib_from_iface(iface)->refcount);
}

static ULONG WINAPI ib_release(IDirect3DIndexBuffer8 *iface)
{
    D8IndexBuffer *buffer = ib_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&buffer->refcount);
    if (!refs) {
        D8IndexBuffer **link = &buffer->device->index_buffers;
        D8WGDestroyResource destroy;
        while (*link && *link != buffer)
            link = &(*link)->next_device_resource;
        if (*link) *link = buffer->next_device_resource;
        destroy.resource_handle = buffer->handle;
        destroy.resource_kind = D8WG_RESOURCE_BUFFER_INDEX;
        emit_command(D8WG_OP_DESTROY_RESOURCE, &destroy, sizeof(destroy));
        HeapFree(GetProcessHeap(), 0, buffer->shadow);
        device_child_release(buffer->device);
        HeapFree(GetProcessHeap(), 0, buffer);
    }
    return refs;
}

static HRESULT WINAPI ib_get_device(IDirect3DIndexBuffer8 *iface,
        IDirect3DDevice8 **device_out)
{
    D8IndexBuffer *buffer = ib_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &buffer->device->iface;
    IDirect3DDevice8_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI ib_set_private_data(IDirect3DIndexBuffer8 *iface,
        REFGUID guid, const void *data, DWORD size, DWORD flags)
{
    (void)iface; (void)guid; (void)data; (void)size; (void)flags;
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI ib_get_private_data(IDirect3DIndexBuffer8 *iface,
        REFGUID guid, void *data, DWORD *size)
{
    (void)iface; (void)guid; (void)data; (void)size;
    return D3DERR_NOTFOUND;
}

static HRESULT WINAPI ib_free_private_data(IDirect3DIndexBuffer8 *iface,
        REFGUID guid)
{
    (void)iface; (void)guid;
    return D3DERR_NOTFOUND;
}

static DWORD WINAPI ib_set_priority(IDirect3DIndexBuffer8 *iface,
        DWORD priority)
{
    D8IndexBuffer *buffer = ib_from_iface(iface);
    DWORD old = buffer->priority;
    buffer->priority = priority;
    return old;
}

static DWORD WINAPI ib_get_priority(IDirect3DIndexBuffer8 *iface)
{
    return ib_from_iface(iface)->priority;
}

static void WINAPI ib_preload(IDirect3DIndexBuffer8 *iface)
{
    (void)iface;
}

static D3DRESOURCETYPE WINAPI ib_get_type(IDirect3DIndexBuffer8 *iface)
{
    (void)iface;
    return D3DRTYPE_INDEXBUFFER;
}

static HRESULT WINAPI ib_lock(IDirect3DIndexBuffer8 *iface, UINT offset,
        UINT size, BYTE **data_out, DWORD flags)
{
    D8IndexBuffer *buffer = ib_from_iface(iface);
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

static HRESULT WINAPI ib_unlock(IDirect3DIndexBuffer8 *iface)
{
    D8IndexBuffer *buffer = ib_from_iface(iface);
    BOOL result;
    if (!buffer->locked)
        return D3DERR_INVALIDCALL;
    result = (buffer->lock_flags & D3DLOCK_READONLY) ||
            emit_buffer_update(buffer->handle, buffer->lock_offset,
            buffer->shadow + buffer->lock_offset, buffer->lock_size,
            buffer->lock_flags);
    buffer->locked = FALSE;
    buffer->lock_offset = 0;
    buffer->lock_size = 0;
    buffer->lock_flags = 0;
    return result ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI ib_get_desc(IDirect3DIndexBuffer8 *iface,
        D3DINDEXBUFFER_DESC *desc)
{
    D8IndexBuffer *buffer = ib_from_iface(iface);
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

static UINT full_mip_level_count(UINT width, UINT height)
{
    UINT levels = 1;
    while (width > 1 || height > 1) {
        if (width > 1)
            width >>= 1;
        if (height > 1)
            height >>= 1;
        ++levels;
    }
    return levels;
}

static HRESULT WINAPI device_create_texture(IDirect3DDevice8 *iface,
        UINT width, UINT height, UINT levels, DWORD usage,
        D3DFORMAT format, D3DPOOL pool, IDirect3DTexture8 **texture_out)
{
    D8Device *device = device_from_iface(iface);
    D8Texture *texture;
    UINT full_levels;
    UINT level;
    UINT level_width;
    UINT level_height;
    HRESULT failure = E_OUTOFMEMORY;

    if (!texture_out)
        return D3DERR_INVALIDCALL;
    *texture_out = NULL;
    if (!width || !height || width > 4096 || height > 4096
            || !supported_texture_format(format)
            || (usage & D3DUSAGE_DEPTHSTENCIL)
            || (usage & D3DUSAGE_RENDERTARGET
                && (pool != D3DPOOL_DEFAULT || levels != 1
                    || (format != D3DFMT_A8R8G8B8
                        && format != D3DFMT_X8R8G8B8)))
            || pool > D3DPOOL_SCRATCH)
        return D3DERR_INVALIDCALL;
    full_levels = full_mip_level_count(width, height);
    if (!levels)
        levels = full_levels;
    if (levels > full_levels)
        return D3DERR_INVALIDCALL;

    texture = (D8Texture *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*texture));
    if (!texture)
        return E_OUTOFMEMORY;
    texture->levels = (D8TextureLevel *)HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, levels * sizeof(*texture->levels));
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
    texture->level_count = levels;
    texture->usage = usage;
    texture->format = format;
    texture->pool = pool;
    device_child_add_ref(device);

    level_width = width;
    level_height = height;
    for (level = 0; level < levels; ++level) {
        D8TextureLevel *level_data = &texture->levels[level];
        level_data->width = level_width;
        level_data->height = level_height;
        if (!texture_level_layout(format, level_width, level_height,
                &level_data->row_pitch, &level_data->row_count,
                &level_data->byte_count))
            goto allocation_failed;
        level_data->shadow = (BYTE *)HeapAlloc(GetProcessHeap(),
                HEAP_ZERO_MEMORY, level_data->byte_count);
        if (!level_data->shadow)
            goto allocation_failed;
        if (level_width > 1)
            level_width >>= 1;
        if (level_height > 1)
            level_height >>= 1;
    }

    if (!emit_texture_create(device, texture)) {
        failure = D3DERR_DRIVERINTERNALERROR;
        goto allocation_failed;
    }
    texture->next_device_resource = device->texture_resources;
    device->texture_resources = texture;
    *texture_out = &texture->iface;
    return D3D_OK;

allocation_failed:
    for (level = 0; level < levels; ++level) {
        if (texture->levels[level].shadow)
            HeapFree(GetProcessHeap(), 0, texture->levels[level].shadow);
    }
    device_child_release(device);
    HeapFree(GetProcessHeap(), 0, texture->levels);
    HeapFree(GetProcessHeap(), 0, texture);
    return failure;
}

static HRESULT texture_lock_level(D8Texture *texture, UINT level,
        D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags)
{
    D8TextureLevel *level_data;
    RECT area;
    UINT block_width;
    UINT block_height;
    UINT block_bytes;
    UINT block_x;
    UINT block_y;

    if (!locked_rect || level >= texture->level_count
            || ((texture->usage & D3DUSAGE_RENDERTARGET)
                && !texture->lockable_render_target))
        return D3DERR_INVALIDCALL;
    level_data = &texture->levels[level];
    if (level_data->locked)
        return D3DERR_INVALIDCALL;
    if (rect) {
        area = *rect;
    } else {
        SetRect(&area, 0, 0, (int)level_data->width,
                (int)level_data->height);
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

static HRESULT texture_unlock_level(D8Texture *texture, UINT level)
{
    D8TextureLevel *level_data;
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

static HRESULT WINAPI texture_query_interface(IDirect3DTexture8 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DResource8)
            && !guid_equal(iid, &IID_IDirect3DBaseTexture8)
            && !guid_equal(iid, &IID_IDirect3DTexture8)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DTexture8_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI texture_add_ref(IDirect3DTexture8 *iface)
{
    return (ULONG)InterlockedIncrement(&texture_from_iface(iface)->refcount);
}

static ULONG WINAPI texture_release(IDirect3DTexture8 *iface)
{
    D8Texture *texture = texture_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&texture->refcount);
    if (!refs) {
        D8Texture **link = &texture->device->texture_resources;
        D8WGDestroyResource destroy;
        UINT level;
        while (*link && *link != texture)
            link = &(*link)->next_device_resource;
        if (*link) *link = texture->next_device_resource;
        destroy.resource_handle = texture->handle;
        destroy.resource_kind = D8WG_RESOURCE_TEXTURE_2D;
        emit_command(D8WG_OP_DESTROY_RESOURCE, &destroy, sizeof(destroy));
        for (level = 0; level < texture->level_count; ++level)
            HeapFree(GetProcessHeap(), 0, texture->levels[level].shadow);
        HeapFree(GetProcessHeap(), 0, texture->levels);
        device_child_release(texture->device);
        HeapFree(GetProcessHeap(), 0, texture);
    }
    return refs;
}

static HRESULT WINAPI texture_get_device(IDirect3DTexture8 *iface,
        IDirect3DDevice8 **device_out)
{
    D8Texture *texture = texture_from_iface(iface);
    if (!device_out)
        return D3DERR_INVALIDCALL;
    *device_out = &texture->device->iface;
    IDirect3DDevice8_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI texture_set_private_data(IDirect3DTexture8 *iface,
        REFGUID guid, const void *data, DWORD size, DWORD flags)
{
    (void)iface; (void)guid; (void)data; (void)size; (void)flags;
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI texture_get_private_data(IDirect3DTexture8 *iface,
        REFGUID guid, void *data, DWORD *size)
{
    (void)iface; (void)guid; (void)data; (void)size;
    return D3DERR_NOTFOUND;
}

static HRESULT WINAPI texture_free_private_data(IDirect3DTexture8 *iface,
        REFGUID guid)
{
    (void)iface; (void)guid;
    return D3DERR_NOTFOUND;
}

static DWORD WINAPI texture_set_priority(IDirect3DTexture8 *iface,
        DWORD priority)
{
    D8Texture *texture = texture_from_iface(iface);
    DWORD old = texture->priority;
    texture->priority = priority;
    return old;
}

static DWORD WINAPI texture_get_priority(IDirect3DTexture8 *iface)
{
    return texture_from_iface(iface)->priority;
}

static void WINAPI texture_preload(IDirect3DTexture8 *iface)
{
    (void)iface;
}

static D3DRESOURCETYPE WINAPI texture_get_type(IDirect3DTexture8 *iface)
{
    (void)iface;
    return D3DRTYPE_TEXTURE;
}

static DWORD WINAPI texture_set_lod(IDirect3DTexture8 *iface, DWORD lod)
{
    D8Texture *texture = texture_from_iface(iface);
    DWORD old = texture->lod;
    if (lod >= texture->level_count)
        lod = texture->level_count - 1;
    texture->lod = lod;
    return old;
}

static DWORD WINAPI texture_get_lod(IDirect3DTexture8 *iface)
{
    return texture_from_iface(iface)->lod;
}

static DWORD WINAPI texture_get_level_count(IDirect3DTexture8 *iface)
{
    return texture_from_iface(iface)->level_count;
}

static HRESULT WINAPI texture_get_level_desc(IDirect3DTexture8 *iface,
        UINT level, D3DSURFACE_DESC *desc)
{
    D8Texture *texture = texture_from_iface(iface);
    D8TextureLevel *level_data;
    if (!desc || level >= texture->level_count)
        return D3DERR_INVALIDCALL;
    level_data = &texture->levels[level];
    ZeroMemory(desc, sizeof(*desc));
    desc->Format = texture->format;
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = texture->usage;
    desc->Pool = texture->pool;
    desc->Size = level_data->byte_count;
    desc->MultiSampleType = D3DMULTISAMPLE_NONE;
    desc->Width = level_data->width;
    desc->Height = level_data->height;
    return D3D_OK;
}

static HRESULT WINAPI texture_get_surface_level(IDirect3DTexture8 *iface,
        UINT level, IDirect3DSurface8 **surface_out)
{
    D8Texture *texture = texture_from_iface(iface);
    D8Surface *surface;
    if (!surface_out)
        return D3DERR_INVALIDCALL;
    *surface_out = NULL;
    if (level >= texture->level_count)
        return D3DERR_INVALIDCALL;
    surface = (D8Surface *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*surface));
    if (!surface)
        return E_OUTOFMEMORY;
    surface->iface.lpVtbl = &g_surface_vtbl;
    surface->refcount = 1;
    surface->texture = texture;
    surface->device = texture->device;
    surface->level = level;
    IDirect3DTexture8_AddRef(iface);
    *surface_out = &surface->iface;
    return D3D_OK;
}

static HRESULT WINAPI texture_lock_rect(IDirect3DTexture8 *iface, UINT level,
        D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags)
{
    return texture_lock_level(texture_from_iface(iface), level, locked_rect,
            rect, flags);
}

static HRESULT WINAPI texture_unlock_rect(IDirect3DTexture8 *iface,
        UINT level)
{
    return texture_unlock_level(texture_from_iface(iface), level);
}

static HRESULT WINAPI texture_add_dirty_rect(IDirect3DTexture8 *iface,
        const RECT *rect)
{
    D8Texture *texture = texture_from_iface(iface);
    if (rect && (rect->left < 0 || rect->top < 0
            || rect->right <= rect->left || rect->bottom <= rect->top
            || (UINT)rect->right > texture->width
            || (UINT)rect->bottom > texture->height))
        return D3DERR_INVALIDCALL;
    return D3D_OK;
}

static HRESULT WINAPI surface_query_interface(IDirect3DSurface8 *iface,
        REFIID iid, void **object)
{
    if (!object)
        return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DResource8)
            && !guid_equal(iid, &IID_IDirect3DSurface8)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DSurface8_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI surface_add_ref(IDirect3DSurface8 *iface)
{
    return (ULONG)InterlockedIncrement(&surface_from_iface(iface)->refcount);
}

static ULONG WINAPI surface_release(IDirect3DSurface8 *iface)
{
    D8Surface *surface = surface_from_iface(iface);
    ULONG refs = (ULONG)InterlockedDecrement(&surface->refcount);
    if (!refs) {
        if (surface->reset_blocker && surface->device
                && surface->device->implicit_surface_count)
            --surface->device->implicit_surface_count;
        if (surface->texture)
            IDirect3DTexture8_Release(&surface->texture->iface);
        else if (surface->device)
            device_child_release(surface->device);
        HeapFree(GetProcessHeap(), 0, surface->shadow);
        HeapFree(GetProcessHeap(), 0, surface);
    }
    return refs;
}

static HRESULT WINAPI surface_get_device(IDirect3DSurface8 *iface,
        IDirect3DDevice8 **device_out)
{
    D8Surface *surface = surface_from_iface(iface);
    if (!device_out) return D3DERR_INVALIDCALL;
    *device_out = surface->texture ? &surface->texture->device->iface
            : &surface->device->iface;
    IDirect3DDevice8_AddRef(*device_out);
    return D3D_OK;
}

static HRESULT WINAPI surface_set_private_data(IDirect3DSurface8 *iface,
        REFGUID guid, const void *data, DWORD size, DWORD flags)
{
    (void)iface; (void)guid; (void)data; (void)size; (void)flags;
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI surface_get_private_data(IDirect3DSurface8 *iface,
        REFGUID guid, void *data, DWORD *size)
{
    (void)iface; (void)guid; (void)data; (void)size;
    return D3DERR_NOTFOUND;
}

static HRESULT WINAPI surface_free_private_data(IDirect3DSurface8 *iface,
        REFGUID guid)
{
    (void)iface; (void)guid;
    return D3DERR_NOTFOUND;
}

static HRESULT WINAPI surface_get_container(IDirect3DSurface8 *iface,
        REFIID iid, void **container)
{
    D8Surface *surface = surface_from_iface(iface);
    if (!container)
        return E_POINTER;
    *container = NULL;
    if (surface->texture) {
        if (!iid || (!iid_is_unknown(iid)
                && !guid_equal(iid, &IID_IDirect3DResource8)
                && !guid_equal(iid, &IID_IDirect3DBaseTexture8)
                && !guid_equal(iid, &IID_IDirect3DTexture8)))
            return E_NOINTERFACE;
        *container = &surface->texture->iface;
        IDirect3DTexture8_AddRef(&surface->texture->iface);
    } else {
        if (!iid || (!iid_is_unknown(iid)
                && !guid_equal(iid, &IID_IDirect3DDevice8)))
            return E_NOINTERFACE;
        *container = &surface->device->iface;
        IDirect3DDevice8_AddRef(&surface->device->iface);
    }
    return S_OK;
}

static HRESULT WINAPI surface_get_desc(IDirect3DSurface8 *iface,
        D3DSURFACE_DESC *desc)
{
    D8Surface *surface = surface_from_iface(iface);
    if (!desc) return D3DERR_INVALIDCALL;
    if (surface->texture)
        return texture_get_level_desc(&surface->texture->iface,
                surface->level, desc);
    *desc = surface->desc;
    return D3D_OK;
}

static HRESULT WINAPI surface_lock_rect(IDirect3DSurface8 *iface,
        D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags)
{
    D8Surface *surface = surface_from_iface(iface);
    RECT area;
    if (surface->texture)
        return texture_lock_level(surface->texture, surface->level,
                locked_rect, rect, flags);
    if (!locked_rect || !surface->shadow || surface->locked
            || surface->backbuffer || surface->depth_stencil)
        return D3DERR_INVALIDCALL;
    if (rect) area = *rect;
    else SetRect(&area, 0, 0, (int)surface->desc.Width,
            (int)surface->desc.Height);
    if (area.left < 0 || area.top < 0 || area.right <= area.left
            || area.bottom <= area.top
            || (UINT)area.right > surface->desc.Width
            || (UINT)area.bottom > surface->desc.Height)
        return D3DERR_INVALIDCALL;
    surface->locked = TRUE;
    surface->lock_rect = area;
    surface->lock_flags = flags;
    locked_rect->Pitch = surface->row_pitch;
    locked_rect->pBits = surface->shadow + area.top * surface->row_pitch
            + area.left * (surface->row_pitch / surface->desc.Width);
    return D3D_OK;
}

static HRESULT WINAPI surface_unlock_rect(IDirect3DSurface8 *iface)
{
    D8Surface *surface = surface_from_iface(iface);
    if (surface->texture)
        return texture_unlock_level(surface->texture, surface->level);
    if (!surface->locked) return D3DERR_INVALIDCALL;
    surface->locked = FALSE;
    surface->lock_flags = 0;
    ZeroMemory(&surface->lock_rect, sizeof(surface->lock_rect));
    return D3D_OK;
}

static HRESULT WINAPI device_get_texture(IDirect3DDevice8 *iface,
        DWORD stage, IDirect3DBaseTexture8 **texture_out)
{
    D8Device *device = device_from_iface(iface);
    if (!texture_out || stage >= D8WG_MAX_TEXTURE_STAGES)
        return D3DERR_INVALIDCALL;
    *texture_out = device->textures[stage]
            ? (IDirect3DBaseTexture8 *)&device->textures[stage]->iface : NULL;
    if (*texture_out)
        IDirect3DBaseTexture8_AddRef(*texture_out);
    return D3D_OK;
}

static HRESULT WINAPI device_set_texture(IDirect3DDevice8 *iface,
        DWORD stage, IDirect3DBaseTexture8 *texture_iface)
{
    D8Device *device = device_from_iface(iface);
    D8Texture *texture = texture_iface ? (D8Texture *)texture_iface : NULL;
    D8WGSetTexture command;

    if (stage >= D8WG_MAX_TEXTURE_STAGES
            || (texture && (texture->iface.lpVtbl != &g_texture_vtbl
                || texture->device != device)))
        return D3DERR_INVALIDCALL;
    if (device->recording_state_block)
        device->recording_state_block->state.texture_mask[stage] = 1;
    if (device->textures[stage] == texture)
        return D3D_OK;
    if (texture)
        IDirect3DTexture8_AddRef(&texture->iface);
    if (device->textures[stage])
        IDirect3DTexture8_Release(&device->textures[stage]->iface);
    device->textures[stage] = texture;
    command.device_handle = device->handle;
    command.stage = stage;
    command.texture_handle = texture ? texture->handle : 0;
    command.reserved = 0;
    return emit_command(D8WG_OP_SET_TEXTURE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_update_texture(IDirect3DDevice8 *iface,
        IDirect3DBaseTexture8 *source_iface,
        IDirect3DBaseTexture8 *destination_iface)
{
    D8Device *device = device_from_iface(iface);
    D8Texture *source = (D8Texture *)source_iface;
    D8Texture *destination = (D8Texture *)destination_iface;
    UINT level;

    if (!source || !destination
            || source->iface.lpVtbl != &g_texture_vtbl
            || destination->iface.lpVtbl != &g_texture_vtbl
            || source->device != device || destination->device != device
            || source->format != destination->format
            || source->width != destination->width
            || source->height != destination->height)
        return D3DERR_INVALIDCALL;
    for (level = 0; level < source->level_count
            && level < destination->level_count; ++level) {
        RECT full;
        if (source->levels[level].locked || destination->levels[level].locked)
            return D3DERR_INVALIDCALL;
        CopyMemory(destination->levels[level].shadow,
                source->levels[level].shadow,
                destination->levels[level].byte_count);
        SetRect(&full, 0, 0, (int)destination->levels[level].width,
                (int)destination->levels[level].height);
        if (!emit_texture_update(destination, level, &full))
            return D3DERR_DRIVERINTERNALERROR;
    }
    return D3D_OK;
}

static HRESULT create_standalone_surface(D8Device *device, UINT width,
        UINT height, D3DFORMAT format, DWORD usage, D3DPOOL pool,
        BOOL lockable, BOOL backbuffer, BOOL depth_stencil,
        BOOL reset_blocker, IDirect3DSurface8 **surface_out)
{
    D8Surface *surface;
    UINT block_width;
    UINT block_height;
    UINT pixel_bytes;
    UINT row_pitch;
    UINT row_count;
    UINT byte_count;
    if (!surface_out)
        return D3DERR_INVALIDCALL;
    *surface_out = NULL;
    if (!width || !height || width > 8192 || height > 8192)
        return D3DERR_INVALIDCALL;
    if (!depth_stencil) {
        if (!texture_format_layout(format, &block_width, &block_height,
                &pixel_bytes)
                || (lockable && (block_width != 1 || block_height != 1))
                || !texture_level_layout(format, width, height,
                    &row_pitch, &row_count, &byte_count))
            return D3DERR_INVALIDCALL;
    }
    if (depth_stencil) {
        row_pitch = width * 4u;
        row_count = height;
        byte_count = row_pitch * row_count;
    }
    surface = (D8Surface *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*surface));
    if (!surface) return E_OUTOFMEMORY;
    if (lockable) {
        surface->shadow = (BYTE *)HeapAlloc(GetProcessHeap(),
                HEAP_ZERO_MEMORY, byte_count);
        if (!surface->shadow) {
            HeapFree(GetProcessHeap(), 0, surface);
            return E_OUTOFMEMORY;
        }
    }
    surface->iface.lpVtbl = &g_surface_vtbl;
    surface->refcount = 1;
    surface->device = device;
    device_child_add_ref(device);
    surface->row_pitch = row_pitch;
    surface->backbuffer = backbuffer;
    surface->depth_stencil = depth_stencil;
    surface->reset_blocker = reset_blocker;
    if (reset_blocker) ++device->implicit_surface_count;
    ZeroMemory(&surface->desc, sizeof(surface->desc));
    surface->desc.Format = format;
    surface->desc.Type = D3DRTYPE_SURFACE;
    surface->desc.Usage = usage;
    surface->desc.Pool = pool;
    surface->desc.Size = byte_count;
    surface->desc.MultiSampleType = D3DMULTISAMPLE_NONE;
    surface->desc.Width = width;
    surface->desc.Height = height;
    *surface_out = &surface->iface;
    return D3D_OK;
}

static HRESULT create_backbuffer_surface(D8Device *device,
        const D3DPRESENT_PARAMETERS *present, BOOL reset_blocker,
        IDirect3DSurface8 **surface_out)
{
    UINT width = present->BackBufferWidth ? present->BackBufferWidth
            : device->display_mode.Width;
    UINT height = present->BackBufferHeight ? present->BackBufferHeight
            : device->display_mode.Height;
    D3DFORMAT format = present->BackBufferFormat == D3DFMT_UNKNOWN
            ? device->display_mode.Format : present->BackBufferFormat;
    return create_standalone_surface(device, width, height, format,
            D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, FALSE, TRUE, FALSE,
            reset_blocker, surface_out);
}

static BYTE *surface_pixels(D8Surface *surface, UINT *pitch_out)
{
    if (surface->texture) {
        D8TextureLevel *level = &surface->texture->levels[surface->level];
        *pitch_out = level->row_pitch;
        return level->shadow;
    }
    if (surface->backbuffer) {
        *pitch_out = surface->device->front_shadow_pitch;
        return surface->device->front_shadow;
    }
    *pitch_out = surface->row_pitch;
    return surface->shadow;
}

static HRESULT copy_surface_rect(D8Surface *source, const RECT *source_rect,
        D8Surface *destination, const POINT *destination_point)
{
    D3DSURFACE_DESC source_desc;
    D3DSURFACE_DESC destination_desc;
    RECT area;
    POINT point;
    BYTE *source_data;
    BYTE *destination_data;
    UINT source_pitch;
    UINT destination_pitch;
    UINT block_width, block_height, pixel_bytes;
    UINT row;
    if (FAILED(surface_get_desc(&source->iface, &source_desc))
            || FAILED(surface_get_desc(&destination->iface,
                    &destination_desc))
            || (source_desc.Format != destination_desc.Format
                && !((source_desc.Format == D3DFMT_A8R8G8B8
                        || source_desc.Format == D3DFMT_X8R8G8B8)
                    && (destination_desc.Format == D3DFMT_A8R8G8B8
                        || destination_desc.Format == D3DFMT_X8R8G8B8)))
            || !texture_format_layout(source_desc.Format, &block_width,
                    &block_height, &pixel_bytes)
            || block_width != 1 || block_height != 1)
        return D3DERR_INVALIDCALL;
    if (source_rect) area = *source_rect;
    else SetRect(&area, 0, 0, (int)source_desc.Width,
            (int)source_desc.Height);
    if (destination_point) point = *destination_point;
    else point.x = point.y = 0;
    if (area.left < 0 || area.top < 0 || area.right <= area.left
            || area.bottom <= area.top
            || (UINT)area.right > source_desc.Width
            || (UINT)area.bottom > source_desc.Height || point.x < 0
            || point.y < 0
            || (UINT)(point.x + area.right - area.left)
                    > destination_desc.Width
            || (UINT)(point.y + area.bottom - area.top)
                    > destination_desc.Height)
        return D3DERR_INVALIDCALL;
    source_data = surface_pixels(source, &source_pitch);
    destination_data = surface_pixels(destination, &destination_pitch);
    if (!source_data || !destination_data) return D3DERR_INVALIDCALL;
    for (row = 0; row < (UINT)(area.bottom - area.top); ++row) {
        CopyMemory(destination_data + (point.y + row) * destination_pitch
                + point.x * pixel_bytes,
                source_data + (area.top + row) * source_pitch
                + area.left * pixel_bytes,
                (area.right - area.left) * pixel_bytes);
    }
    if (destination->texture) {
        RECT dirty;
        SetRect(&dirty, point.x, point.y,
                point.x + area.right - area.left,
                point.y + area.bottom - area.top);
        if (!emit_texture_update(destination->texture, destination->level,
                &dirty))
            return D3DERR_DRIVERINTERNALERROR;
    }
    return D3D_OK;
}

/* Typed unsupported methods keep stdcall stack cleanup correct on 32-bit XP. */
#define DEV_STUB0(name) \
    static HRESULT WINAPI device_##name(IDirect3DDevice8 *iface) \
    { (void)iface; return D3DERR_INVALIDCALL; }
#define DEV_STUB(name, ...) \
    static HRESULT WINAPI device_##name(IDirect3DDevice8 *iface, __VA_ARGS__)

DEV_STUB(set_cursor_properties, UINT x, UINT y, IDirect3DSurface8 *surface)
{ (void)iface; (void)x; (void)y; (void)surface; return D3DERR_INVALIDCALL; }
static void WINAPI device_set_cursor_position(IDirect3DDevice8 *iface,
        UINT x, UINT y, DWORD flags)
{ (void)iface; (void)x; (void)y; (void)flags; }
static WINBOOL WINAPI device_show_cursor(IDirect3DDevice8 *iface, WINBOOL show)
{ (void)iface; (void)show; return FALSE; }

static HRESULT WINAPI swapchain_query_interface(IDirect3DSwapChain8 *iface,
        REFIID iid, void **object)
{
    if (!object) return E_POINTER;
    *object = NULL;
    if (!iid || (!iid_is_unknown(iid)
            && !guid_equal(iid, &IID_IDirect3DSwapChain8)))
        return E_NOINTERFACE;
    *object = iface;
    IDirect3DSwapChain8_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI swapchain_add_ref(IDirect3DSwapChain8 *iface)
{
    return (ULONG)InterlockedIncrement(&((D8SwapChain *)iface)->refcount);
}

static ULONG WINAPI swapchain_release(IDirect3DSwapChain8 *iface)
{
    D8SwapChain *chain = (D8SwapChain *)iface;
    ULONG refs = (ULONG)InterlockedDecrement(&chain->refcount);
    if (!refs) {
        if (chain->device->additional_swap_chain_count)
            --chain->device->additional_swap_chain_count;
        device_child_release(chain->device);
        HeapFree(GetProcessHeap(), 0, chain);
    }
    return refs;
}

static HRESULT WINAPI swapchain_present(IDirect3DSwapChain8 *iface,
        const RECT *source, const RECT *destination, HWND override_window,
        const RGNDATA *dirty_region)
{
    D8SwapChain *chain = (D8SwapChain *)iface;
    HWND window = override_window ? override_window : chain->present.hDeviceWindow;
    (void)source; (void)destination; (void)dirty_region;
    return emit_present_and_flush(chain->device, window)
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI swapchain_get_backbuffer(IDirect3DSwapChain8 *iface,
        UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8 **surface_out)
{
    D8SwapChain *chain = (D8SwapChain *)iface;
    if (!surface_out)
        return D3DERR_INVALIDCALL;
    *surface_out = NULL;
    if (index || type != D3DBACKBUFFER_TYPE_MONO)
        return D3DERR_INVALIDCALL;
    return create_backbuffer_surface(chain->device, &chain->present, TRUE,
            surface_out);
}

static HRESULT WINAPI device_create_swapchain(IDirect3DDevice8 *iface,
        D3DPRESENT_PARAMETERS *parameters, IDirect3DSwapChain8 **chain_out)
{
    D8Device *device = device_from_iface(iface);
    D8SwapChain *chain;
    if (!chain_out)
        return D3DERR_INVALIDCALL;
    *chain_out = NULL;
    if (!parameters || !parameters->Windowed
            || parameters->MultiSampleType != D3DMULTISAMPLE_NONE
            || parameters->BackBufferCount > 1
            || parameters->BackBufferWidth > 8192
            || parameters->BackBufferHeight > 8192)
        return D3DERR_INVALIDCALL;
    if (parameters->BackBufferFormat != D3DFMT_UNKNOWN
            && !supported_backbuffer_format(parameters->BackBufferFormat))
        return D3DERR_NOTAVAILABLE;
    chain = (D8SwapChain *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*chain));
    if (!chain) return E_OUTOFMEMORY;
    chain->iface.lpVtbl = &g_swapchain_vtbl;
    chain->refcount = 1;
    chain->device = device;
    chain->present = *parameters;
    device_child_add_ref(device);
    ++device->additional_swap_chain_count;
    *chain_out = &chain->iface;
    return D3D_OK;
}

static HRESULT WINAPI device_get_backbuffer(IDirect3DDevice8 *iface, UINT index,
        D3DBACKBUFFER_TYPE type, IDirect3DSurface8 **surface_out)
{
    D8Device *device = device_from_iface(iface);
    if (!surface_out)
        return D3DERR_INVALIDCALL;
    *surface_out = NULL;
    if (index || type != D3DBACKBUFFER_TYPE_MONO)
        return D3DERR_INVALIDCALL;
    return create_backbuffer_surface(device, &device->present, TRUE,
            surface_out);
}
DEV_STUB(get_raster_status, D3DRASTER_STATUS *s)
{ (void)iface; (void)s; return D3DERR_INVALIDCALL; }
static void WINAPI device_set_gamma(IDirect3DDevice8 *iface, DWORD flags,
        const D3DGAMMARAMP *ramp)
{ (void)iface; (void)flags; (void)ramp; }
static void WINAPI device_get_gamma(IDirect3DDevice8 *iface, D3DGAMMARAMP *ramp)
{ (void)iface; if (ramp) ZeroMemory(ramp, sizeof(*ramp)); }
DEV_STUB(create_volume_texture, UINT w, UINT h, UINT d, UINT levels, DWORD usage,
        D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture8 **out)
{ (void)iface;(void)w;(void)h;(void)d;(void)levels;(void)usage;(void)format;(void)pool; if(out)*out=NULL; return D3DERR_INVALIDCALL; }
DEV_STUB(create_cube_texture, UINT edge, UINT levels, DWORD usage,
        D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture8 **out)
{ (void)iface;(void)edge;(void)levels;(void)usage;(void)format;(void)pool; if(out)*out=NULL; return D3DERR_INVALIDCALL; }
static HRESULT WINAPI device_create_render_target(IDirect3DDevice8 *iface,
        UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
        WINBOOL lockable, IDirect3DSurface8 **surface_out)
{
    IDirect3DTexture8 *texture_iface = NULL;
    HRESULT hr;
    if (!surface_out || ms != D3DMULTISAMPLE_NONE)
        return D3DERR_INVALIDCALL;
    *surface_out = NULL;
    hr = device_create_texture(iface, width, height, 1,
            D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &texture_iface);
    if (FAILED(hr)) return hr;
    texture_from_iface(texture_iface)->lockable_render_target = !!lockable;
    hr = IDirect3DTexture8_GetSurfaceLevel(texture_iface, 0, surface_out);
    IDirect3DTexture8_Release(texture_iface);
    return hr;
}

static HRESULT WINAPI device_create_depth_surface(IDirect3DDevice8 *iface,
        UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
        IDirect3DSurface8 **surface_out)
{
    if (!surface_out || (format != D3DFMT_D16 && format != D3DFMT_D24S8)
            || ms != D3DMULTISAMPLE_NONE)
        return D3DERR_INVALIDCALL;
    *surface_out = NULL;
    return create_standalone_surface(device_from_iface(iface), width, height,
            format, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, FALSE, FALSE,
            TRUE, TRUE, surface_out);
}

static HRESULT WINAPI device_create_image_surface(IDirect3DDevice8 *iface,
        UINT width, UINT height, D3DFORMAT format,
        IDirect3DSurface8 **surface_out)
{
    if (!surface_out) return D3DERR_INVALIDCALL;
    *surface_out = NULL;
    return create_standalone_surface(device_from_iface(iface), width, height,
            format, 0, D3DPOOL_SYSTEMMEM, TRUE, FALSE, FALSE, FALSE,
            surface_out);
}

static HRESULT WINAPI device_copy_rects(IDirect3DDevice8 *iface,
        IDirect3DSurface8 *source_iface, const RECT *rects, UINT count,
        IDirect3DSurface8 *destination_iface, const POINT *points)
{
    D8Device *device = device_from_iface(iface);
    D8Surface *source;
    D8Surface *destination;
    UINT index;
    if (!source_iface || !destination_iface
            || source_iface->lpVtbl != &g_surface_vtbl
            || destination_iface->lpVtbl != &g_surface_vtbl
            || (count && (!rects || !points)))
        return D3DERR_INVALIDCALL;
    source = surface_from_iface(source_iface);
    destination = surface_from_iface(destination_iface);
    if ((source->texture ? source->texture->device : source->device) != device
            || (destination->texture ? destination->texture->device
                    : destination->device) != device)
        return D3DERR_INVALIDCALL;
    if (!count) return copy_surface_rect(source, NULL, destination, NULL);
    for (index = 0; index < count; ++index) {
        HRESULT hr = copy_surface_rect(source, &rects[index], destination,
                &points[index]);
        if (FAILED(hr)) return hr;
    }
    return D3D_OK;
}

static HRESULT WINAPI device_get_front_buffer(IDirect3DDevice8 *iface,
        IDirect3DSurface8 *destination_iface)
{
    D8Device *device = device_from_iface(iface);
    IDirect3DSurface8 *source_iface = NULL;
    HRESULT hr;
    if (!destination_iface || destination_iface->lpVtbl != &g_surface_vtbl)
        return D3DERR_INVALIDCALL;
    if ((surface_from_iface(destination_iface)->texture
            ? surface_from_iface(destination_iface)->texture->device
            : surface_from_iface(destination_iface)->device) != device)
        return D3DERR_INVALIDCALL;
    hr = create_backbuffer_surface(device, &device->present, FALSE,
            &source_iface);
    if (SUCCEEDED(hr)) {
        hr = copy_surface_rect(surface_from_iface(source_iface), NULL,
                surface_from_iface(destination_iface), NULL);
        IDirect3DSurface8_Release(source_iface);
    }
    return hr;
}

static HRESULT WINAPI device_set_render_target(IDirect3DDevice8 *iface,
        IDirect3DSurface8 *render_target_iface,
        IDirect3DSurface8 *depth_iface)
{
    D8Device *device = device_from_iface(iface);
    D8Surface *render_target;
    D8Surface *depth = depth_iface ? surface_from_iface(depth_iface) : NULL;
    D8Texture *texture;
    D8WGSetRenderTarget command;
    if (!render_target_iface
            || render_target_iface->lpVtbl != &g_surface_vtbl
            || (depth_iface && depth_iface->lpVtbl != &g_surface_vtbl))
        return D3DERR_INVALIDCALL;
    render_target = surface_from_iface(render_target_iface);
    if ((render_target->texture ? render_target->texture->device
            : render_target->device) != device || render_target->depth_stencil)
        return D3DERR_INVALIDCALL;
    texture = render_target->backbuffer ? NULL : render_target->texture;
    if (texture && !(texture->usage & D3DUSAGE_RENDERTARGET))
        return D3DERR_INVALIDCALL;
    if (depth && ((depth->texture ? depth->texture->device : depth->device)
                != device || !depth->depth_stencil))
        return D3DERR_INVALIDCALL;
    if (depth && (depth->desc.Width != (render_target->texture
                    ? render_target->texture->levels[render_target->level].width
                    : render_target->desc.Width)
            || depth->desc.Height != (render_target->texture
                    ? render_target->texture->levels[render_target->level].height
                    : render_target->desc.Height)))
        return D3DERR_INVALIDCALL;
    if (texture) IDirect3DTexture8_AddRef(&texture->iface);
    if (depth) IDirect3DSurface8_AddRef(&depth->iface);
    if (device->render_target_texture)
        IDirect3DTexture8_Release(&device->render_target_texture->iface);
    if (device->depth_surface)
        IDirect3DSurface8_Release(&device->depth_surface->iface);
    device->render_target_texture = texture;
    device->render_target_level = texture ? render_target->level : 0;
    device->depth_surface = depth;
    device->depth_surface_enabled = depth != NULL;
    command.device_handle = device->handle;
    command.color_texture_handle = texture ? texture->handle : 0;
    command.color_level = texture ? render_target->level : 0;
    command.depth_enabled = depth != NULL;
    return emit_command(D8WG_OP_SET_RENDER_TARGET, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_render_target(IDirect3DDevice8 *iface,
        IDirect3DSurface8 **surface_out)
{
    D8Device *device = device_from_iface(iface);
    if (!surface_out) return D3DERR_INVALIDCALL;
    *surface_out = NULL;
    if (device->render_target_texture)
        return texture_get_surface_level(&device->render_target_texture->iface,
                device->render_target_level, surface_out);
    return create_backbuffer_surface(device, &device->present, TRUE,
            surface_out);
}

static HRESULT WINAPI device_get_depth_surface(IDirect3DDevice8 *iface,
        IDirect3DSurface8 **surface_out)
{
    D8Device *device = device_from_iface(iface);
    if (!surface_out)
        return D3DERR_INVALIDCALL;
    *surface_out = NULL;
    if (!device->depth_surface_enabled)
        return D3DERR_INVALIDCALL;
    if (device->depth_surface) {
        *surface_out = &device->depth_surface->iface;
        IDirect3DSurface8_AddRef(*surface_out);
        return D3D_OK;
    }
    return create_standalone_surface(device, device->display_mode.Width,
            device->display_mode.Height,
            device->present.AutoDepthStencilFormat,
            D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, FALSE, FALSE, TRUE,
            TRUE, surface_out);
}
static BOOL transform_slot(D3DTRANSFORMSTATETYPE state, UINT *slot)
{
    UINT value = (UINT)state;
    if (state == D3DTS_VIEW) {
        *slot = D8WG_TRANSFORM_VIEW_SLOT;
        return TRUE;
    }
    if (state == D3DTS_PROJECTION) {
        *slot = D8WG_TRANSFORM_PROJECTION_SLOT;
        return TRUE;
    }
    if (value >= (UINT)D3DTS_TEXTURE0 && value <= (UINT)D3DTS_TEXTURE7) {
        *slot = D8WG_TRANSFORM_TEXTURE_SLOT + value - (UINT)D3DTS_TEXTURE0;
        return TRUE;
    }
    if (value >= (UINT)D3DTS_WORLD && value < (UINT)D3DTS_WORLD + 256u) {
        *slot = D8WG_TRANSFORM_WORLD_SLOT + value - (UINT)D3DTS_WORLD;
        return TRUE;
    }
    return FALSE;
}

static BOOL matrix_equal(const D3DMATRIX *left, const D3DMATRIX *right)
{
    UINT row;
    UINT column;
    for (row = 0; row < 4; ++row) {
        for (column = 0; column < 4; ++column) {
            if (left->m[row][column] != right->m[row][column])
                return FALSE;
        }
    }
    return TRUE;
}

static void matrix_multiply(D3DMATRIX *result, const D3DMATRIX *left,
        const D3DMATRIX *right)
{
    D3DMATRIX temporary;
    UINT row;
    UINT column;
    UINT inner;
    for (row = 0; row < 4; ++row) {
        for (column = 0; column < 4; ++column) {
            float value = 0.0f;
            for (inner = 0; inner < 4; ++inner)
                value += left->m[row][inner] * right->m[inner][column];
            temporary.m[row][column] = value;
        }
    }
    *result = temporary;
}

static HRESULT emit_transform(D8Device *device,
        D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix)
{
    D8WGSetTransform command;
    command.device_handle = device->handle;
    command.state = (UINT)state;
    CopyMemory(command.matrix, matrix, sizeof(command.matrix));
    return emit_command(D8WG_OP_SET_TRANSFORM, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_set_transform(IDirect3DDevice8 *iface,
        D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix)
{
    D8Device *device = device_from_iface(iface);
    UINT slot;
    if (!matrix || !transform_slot(state, &slot))
        return D3DERR_INVALIDCALL;
    if (device->recording_state_block)
        device->recording_state_block->state.transform_mask[slot] = 1;
    if (matrix_equal(&device->transforms[slot], matrix))
        return D3D_OK;
    device->transforms[slot] = *matrix;
    return emit_transform(device, state, matrix);
}

static HRESULT WINAPI device_get_transform(IDirect3DDevice8 *iface,
        D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
    UINT slot;
    if (!matrix || !transform_slot(state, &slot))
        return D3DERR_INVALIDCALL;
    *matrix = device_from_iface(iface)->transforms[slot];
    return D3D_OK;
}

static HRESULT WINAPI device_multiply_transform(IDirect3DDevice8 *iface,
        D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix)
{
    D8Device *device = device_from_iface(iface);
    D3DMATRIX product;
    UINT slot;
    if (!matrix || !transform_slot(state, &slot))
        return D3DERR_INVALIDCALL;
    if (device->recording_state_block)
        device->recording_state_block->state.transform_mask[slot] = 1;
    matrix_multiply(&product, &device->transforms[slot], matrix);
    if (matrix_equal(&device->transforms[slot], &product))
        return D3D_OK;
    device->transforms[slot] = product;
    return emit_transform(device, state, &product);
}
static BOOL bytes_equal(const void *left, const void *right, UINT size)
{
    const BYTE *a = (const BYTE *)left;
    const BYTE *b = (const BYTE *)right;
    UINT index;
    for (index = 0; index < size; ++index) {
        if (a[index] != b[index]) return FALSE;
    }
    return TRUE;
}

static HRESULT WINAPI device_set_material(IDirect3DDevice8 *iface,
        const D3DMATERIAL8 *material)
{
    D8Device *device = device_from_iface(iface);
    D8WGSetMaterial command;
    if (!material) return D3DERR_INVALIDCALL;
    if (device->recording_state_block)
        device->recording_state_block->state.material_mask = TRUE;
    if (bytes_equal(&device->material, material, sizeof(*material)))
        return D3D_OK;
    device->material = *material;
    command.device_handle = device->handle;
    CopyMemory(command.diffuse, &material->Diffuse, sizeof(command.diffuse));
    CopyMemory(command.ambient, &material->Ambient, sizeof(command.ambient));
    CopyMemory(command.specular, &material->Specular, sizeof(command.specular));
    CopyMemory(command.emissive, &material->Emissive, sizeof(command.emissive));
    command.power = material->Power;
    return emit_command(D8WG_OP_SET_MATERIAL, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_material(IDirect3DDevice8 *iface,
        D3DMATERIAL8 *material)
{
    if (!material) return D3DERR_INVALIDCALL;
    *material = device_from_iface(iface)->material;
    return D3D_OK;
}

static HRESULT WINAPI device_set_light(IDirect3DDevice8 *iface, DWORD index,
        const D3DLIGHT8 *light)
{
    D8Device *device = device_from_iface(iface);
    D8WGSetLight command;
    if (!light || index >= D8WG_MAX_LIGHTS || light->Type < D3DLIGHT_POINT
            || light->Type > D3DLIGHT_DIRECTIONAL)
        return D3DERR_INVALIDCALL;
    if (device->recording_state_block)
        device->recording_state_block->state.light_mask[index] = 1;
    if (device->light_set[index] &&
            bytes_equal(&device->lights[index], light, sizeof(*light)))
        return D3D_OK;
    device->lights[index] = *light;
    device->light_set[index] = TRUE;
    command.device_handle = device->handle;
    command.index = index;
    command.type = light->Type;
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
    return emit_command(D8WG_OP_SET_LIGHT, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_light(IDirect3DDevice8 *iface, DWORD index,
        D3DLIGHT8 *light)
{
    D8Device *device = device_from_iface(iface);
    if (!light || index >= D8WG_MAX_LIGHTS || !device->light_set[index])
        return D3DERR_INVALIDCALL;
    *light = device->lights[index];
    return D3D_OK;
}

static HRESULT WINAPI device_light_enable(IDirect3DDevice8 *iface,
        DWORD index, WINBOOL enable)
{
    D8Device *device = device_from_iface(iface);
    D8WGLightEnable command;
    if (index >= D8WG_MAX_LIGHTS) return D3DERR_INVALIDCALL;
    if (device->recording_state_block)
        device->recording_state_block->state.light_enable_mask[index] = 1;
    enable = !!enable;
    if (device->light_enabled[index] == enable) return D3D_OK;
    device->light_enabled[index] = enable;
    command.device_handle = device->handle;
    command.index = index;
    command.enable = enable;
    command.reserved = 0;
    return emit_command(D8WG_OP_LIGHT_ENABLE, &command, sizeof(command))
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_light_enable(IDirect3DDevice8 *iface,
        DWORD index, WINBOOL *enable)
{
    if (!enable || index >= D8WG_MAX_LIGHTS) return D3DERR_INVALIDCALL;
    *enable = device_from_iface(iface)->light_enabled[index];
    return D3D_OK;
}
DEV_STUB(set_clip_plane, DWORD index, const float *plane)
{ (void)iface;(void)index;(void)plane; return D3DERR_INVALIDCALL; }
DEV_STUB(get_clip_plane, DWORD index, float *plane)
{ (void)iface;(void)index;(void)plane; return D3DERR_INVALIDCALL; }

static void state_block_set_scope(D8StateBlock *block,
        D3DSTATEBLOCKTYPE type)
{
    D8StateSnapshot *state = &block->state;
    if (type == D3DSBT_ALL || type == D3DSBT_PIXELSTATE) {
        FillMemory(state->render_mask, sizeof(state->render_mask), 1);
        FillMemory(state->texture_stage_mask,
                sizeof(state->texture_stage_mask), 1);
        FillMemory(state->texture_mask, sizeof(state->texture_mask), 1);
        state->pixel_shader_mask = TRUE;
    }
    if (type == D3DSBT_ALL || type == D3DSBT_VERTEXSTATE) {
        /* D3D8 has render states in both predefined state-block classes. */
        FillMemory(state->render_mask, sizeof(state->render_mask), 1);
        FillMemory(state->stream_mask, sizeof(state->stream_mask), 1);
        FillMemory(state->transform_mask, sizeof(state->transform_mask), 1);
        FillMemory(state->light_mask, sizeof(state->light_mask), 1);
        FillMemory(state->light_enable_mask,
                sizeof(state->light_enable_mask), 1);
        state->viewport_mask = TRUE;
        state->material_mask = TRUE;
        state->indices_mask = TRUE;
        state->vertex_shader_mask = TRUE;
    }
}

static void state_block_release_references(D8StateBlock *block)
{
    UINT index;
    for (index = 0; index < D8WG_MAX_TEXTURE_STAGES; ++index) {
        if (block->state.textures[index]) {
            IDirect3DTexture8_Release(&block->state.textures[index]->iface);
            block->state.textures[index] = NULL;
        }
    }
    for (index = 0; index < D8WG_MAX_STREAMS; ++index) {
        if (block->state.streams[index].buffer) {
            IDirect3DVertexBuffer8_Release(
                    &block->state.streams[index].buffer->iface);
            block->state.streams[index].buffer = NULL;
        }
    }
    if (block->state.index_buffer) {
        IDirect3DIndexBuffer8_Release(&block->state.index_buffer->iface);
        block->state.index_buffer = NULL;
    }
}

static void state_block_capture(D8Device *device, D8StateBlock *block)
{
    D8StateSnapshot *state = &block->state;
    UINT index;
    UINT stage;

    for (index = 0; index < D8WG_MAX_RENDER_STATES; ++index) {
        if (state->render_mask[index])
            state->render_states[index] = device->render_states[index];
    }
    for (stage = 0; stage < D8WG_MAX_TEXTURE_STAGES; ++stage) {
        for (index = 0; index < D8WG_MAX_TEXTURE_STAGE_STATES; ++index) {
            if (state->texture_stage_mask[stage][index])
                state->texture_stage_states[stage][index] =
                        device->texture_stage_states[stage][index];
        }
        if (state->texture_mask[stage]) {
            D8Texture *texture = device->textures[stage];
            if (texture)
                IDirect3DTexture8_AddRef(&texture->iface);
            if (state->textures[stage])
                IDirect3DTexture8_Release(&state->textures[stage]->iface);
            state->textures[stage] = texture;
        }
    }
    for (index = 0; index < D8WG_MAX_STREAMS; ++index) {
        if (state->stream_mask[index]) {
            D8VertexBuffer *buffer = device->streams[index].buffer;
            if (buffer)
                IDirect3DVertexBuffer8_AddRef(&buffer->iface);
            if (state->streams[index].buffer)
                IDirect3DVertexBuffer8_Release(
                        &state->streams[index].buffer->iface);
            state->streams[index] = device->streams[index];
            state->streams[index].buffer = buffer;
        }
    }
    if (state->indices_mask) {
        D8IndexBuffer *buffer = device->index_buffer;
        if (buffer)
            IDirect3DIndexBuffer8_AddRef(&buffer->iface);
        if (state->index_buffer)
            IDirect3DIndexBuffer8_Release(&state->index_buffer->iface);
        state->index_buffer = buffer;
        state->base_vertex_index = device->base_vertex_index;
    }
    if (state->vertex_shader_mask)
        state->vertex_shader = device->vertex_shader;
    if (state->pixel_shader_mask)
        state->pixel_shader = device->pixel_shader;
    if (state->viewport_mask)
        state->viewport = device->viewport;
    if (state->material_mask)
        state->material = device->material;
    for (index = 0; index < D8WG_MAX_TRANSFORMS; ++index) {
        if (state->transform_mask[index])
            state->transforms[index] = device->transforms[index];
    }
    for (index = 0; index < D8WG_MAX_LIGHTS; ++index) {
        if (state->light_mask[index]) {
            state->lights[index] = device->lights[index];
            state->light_set[index] = device->light_set[index];
        }
        if (state->light_enable_mask[index])
            state->light_enabled[index] = device->light_enabled[index];
    }
}

static D8StateBlock *device_find_state_block(D8Device *device, DWORD token,
        D8StateBlock ***link_out)
{
    D8StateBlock **link = &device->state_blocks;
    while (*link) {
        if ((*link)->token == token) {
            if (link_out) *link_out = link;
            return *link;
        }
        link = &(*link)->next;
    }
    return NULL;
}

static DWORD device_allocate_state_block_token(D8Device *device)
{
    DWORD token;
    do {
        token = ++device->next_state_block_token;
        if (!token) token = ++device->next_state_block_token;
    } while (device_find_state_block(device, token, NULL));
    return token;
}

static D3DTRANSFORMSTATETYPE transform_state_from_slot(UINT slot)
{
    if (slot == D8WG_TRANSFORM_VIEW_SLOT) return D3DTS_VIEW;
    if (slot == D8WG_TRANSFORM_PROJECTION_SLOT) return D3DTS_PROJECTION;
    if (slot >= D8WG_TRANSFORM_TEXTURE_SLOT
            && slot < D8WG_TRANSFORM_WORLD_SLOT)
        return (D3DTRANSFORMSTATETYPE)((UINT)D3DTS_TEXTURE0
                + slot - D8WG_TRANSFORM_TEXTURE_SLOT);
    return (D3DTRANSFORMSTATETYPE)((UINT)D3DTS_WORLD
            + slot - D8WG_TRANSFORM_WORLD_SLOT);
}

static HRESULT state_block_apply(D8Device *device, D8StateBlock *block)
{
    D8StateSnapshot *state = &block->state;
    HRESULT hr;
    UINT index;
    UINT stage;
#define APPLY_STATE(call) do { hr = (call); if (FAILED(hr)) return hr; } while (0)
    for (index = 0; index < D8WG_MAX_RENDER_STATES; ++index) {
        if (state->render_mask[index])
            APPLY_STATE(device_set_render_state(&device->iface,
                    (D3DRENDERSTATETYPE)index, state->render_states[index]));
    }
    for (stage = 0; stage < D8WG_MAX_TEXTURE_STAGES; ++stage) {
        for (index = 0; index < D8WG_MAX_TEXTURE_STAGE_STATES; ++index) {
            if (state->texture_stage_mask[stage][index])
                APPLY_STATE(device_set_texture_stage_state(&device->iface,
                        stage, (D3DTEXTURESTAGESTATETYPE)index,
                        state->texture_stage_states[stage][index]));
        }
        if (state->texture_mask[stage])
            APPLY_STATE(device_set_texture(&device->iface, stage,
                    state->textures[stage]
                    ? (IDirect3DBaseTexture8 *)&state->textures[stage]->iface
                    : NULL));
    }
    if (state->viewport_mask)
        APPLY_STATE(device_set_viewport(&device->iface, &state->viewport));
    if (state->material_mask)
        APPLY_STATE(device_set_material(&device->iface, &state->material));
    for (index = 0; index < D8WG_MAX_TRANSFORMS; ++index) {
        if (state->transform_mask[index])
            APPLY_STATE(device_set_transform(&device->iface,
                    transform_state_from_slot(index),
                    &state->transforms[index]));
    }
    for (index = 0; index < D8WG_MAX_LIGHTS; ++index) {
        if (state->light_mask[index] && state->light_set[index])
            APPLY_STATE(device_set_light(&device->iface, index,
                    &state->lights[index]));
        if (state->light_enable_mask[index])
            APPLY_STATE(device_light_enable(&device->iface, index,
                    state->light_enabled[index]));
    }
    for (index = 0; index < D8WG_MAX_STREAMS; ++index) {
        if (state->stream_mask[index])
            APPLY_STATE(device_set_stream_source(&device->iface, index,
                    state->streams[index].buffer
                    ? &state->streams[index].buffer->iface : NULL,
                    state->streams[index].stride));
    }
    if (state->indices_mask)
        APPLY_STATE(device_set_indices(&device->iface,
                state->index_buffer ? &state->index_buffer->iface : NULL,
                state->base_vertex_index));
    if (state->vertex_shader_mask)
        APPLY_STATE(device_set_vertex_shader(&device->iface,
                state->vertex_shader));
    if (state->pixel_shader_mask)
        APPLY_STATE(device_set_pixel_shader(&device->iface,
                state->pixel_shader));
#undef APPLY_STATE
    return D3D_OK;
}

static HRESULT WINAPI device_begin_state_block(IDirect3DDevice8 *iface)
{
    D8Device *device = device_from_iface(iface);
    D8StateBlock *block;
    if (device->recording_state_block)
        return D3DERR_INVALIDCALL;
    block = (D8StateBlock *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*block));
    if (!block) return E_OUTOFMEMORY;
    device->recording_state_block = block;
    return D3D_OK;
}

static HRESULT WINAPI device_end_state_block(IDirect3DDevice8 *iface,
        DWORD *token)
{
    D8Device *device = device_from_iface(iface);
    D8StateBlock *block = device->recording_state_block;
    if (!token || !block)
        return D3DERR_INVALIDCALL;
    block->token = device_allocate_state_block_token(device);
    state_block_capture(device, block);
    block->next = device->state_blocks;
    device->state_blocks = block;
    device->recording_state_block = NULL;
    *token = block->token;
    return D3D_OK;
}

static HRESULT WINAPI device_apply_state_block(IDirect3DDevice8 *iface,
        DWORD token)
{
    D8Device *device = device_from_iface(iface);
    D8StateBlock *block;
    if (device->recording_state_block)
        return D3DERR_INVALIDCALL;
    block = device_find_state_block(device, token, NULL);
    return block ? state_block_apply(device, block) : D3DERR_INVALIDCALL;
}

static HRESULT WINAPI device_capture_state_block(IDirect3DDevice8 *iface,
        DWORD token)
{
    D8Device *device = device_from_iface(iface);
    D8StateBlock *block;
    if (device->recording_state_block)
        return D3DERR_INVALIDCALL;
    block = device_find_state_block(device, token, NULL);
    if (!block) return D3DERR_INVALIDCALL;
    state_block_capture(device, block);
    return D3D_OK;
}

static HRESULT WINAPI device_delete_state_block(IDirect3DDevice8 *iface,
        DWORD token)
{
    D8Device *device = device_from_iface(iface);
    D8StateBlock **link;
    D8StateBlock *block;
    if (device->recording_state_block)
        return D3DERR_INVALIDCALL;
    block = device_find_state_block(device, token, &link);
    if (!block) return D3DERR_INVALIDCALL;
    *link = block->next;
    state_block_release_references(block);
    HeapFree(GetProcessHeap(), 0, block);
    return D3D_OK;
}

static HRESULT WINAPI device_create_state_block(IDirect3DDevice8 *iface,
        D3DSTATEBLOCKTYPE type, DWORD *token)
{
    D8Device *device = device_from_iface(iface);
    D8StateBlock *block;
    if (!token || device->recording_state_block
            || (type != D3DSBT_ALL && type != D3DSBT_PIXELSTATE
                && type != D3DSBT_VERTEXSTATE))
        return D3DERR_INVALIDCALL;
    block = (D8StateBlock *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*block));
    if (!block) return E_OUTOFMEMORY;
    block->type = type;
    block->token = device_allocate_state_block_token(device);
    state_block_set_scope(block, type);
    state_block_capture(device, block);
    block->next = device->state_blocks;
    device->state_blocks = block;
    *token = block->token;
    return D3D_OK;
}
DEV_STUB(set_clip_status, const D3DCLIPSTATUS8 *status)
{ (void)iface;(void)status; return D3DERR_INVALIDCALL; }
DEV_STUB(get_clip_status, D3DCLIPSTATUS8 *status)
{ (void)iface;(void)status; return D3DERR_INVALIDCALL; }
DEV_STUB(get_info, DWORD id, void *info, DWORD size)
{ (void)iface;(void)id;(void)info;(void)size; return D3DERR_NOTAVAILABLE; }
DEV_STUB(set_palette, UINT index, const PALETTEENTRY *entries)
{ (void)iface;(void)index;(void)entries; return D3DERR_INVALIDCALL; }
DEV_STUB(get_palette, UINT index, PALETTEENTRY *entries)
{ (void)iface;(void)index;(void)entries; return D3DERR_INVALIDCALL; }
DEV_STUB(set_current_palette, UINT index)
{ (void)iface;(void)index; return D3DERR_INVALIDCALL; }
DEV_STUB(get_current_palette, UINT *index)
{ (void)iface;(void)index; return D3DERR_INVALIDCALL; }
DEV_STUB(process_vertices, UINT src, UINT dst, UINT count,
        IDirect3DVertexBuffer8 *buffer, DWORD flags)
{ (void)iface;(void)src;(void)dst;(void)count;(void)buffer;(void)flags; return D3DERR_INVALIDCALL; }
/*
 * Stage 6: D3D8 shader model 1.x. CreateVertexShader/CreatePixelShader parse
 * and reject anything outside the supported instruction set up front (see
 * validate_shader_body); a shader that fails validation never gets a handle
 * and never reaches the D8WG protocol, matching "illegal bytecode is
 * rejected, not silently mistranslated."
 */
static HRESULT WINAPI device_create_vertex_shader(IDirect3DDevice8 *iface,
        const DWORD *declaration, const DWORD *function, DWORD *shader,
        DWORD usage)
{
    D8Device *device = device_from_iface(iface);
    D8Shader *entry;
    UINT decl_count = 0;
    UINT body_count = 0;
    (void)usage;

    if (shader) *shader = 0;
    if (!declaration || !function || !shader)
        return D3DERR_INVALIDCALL;
    if (function[0] != (DWORD)D3DVS_VERSION(1, 1))
        return D3DERR_INVALIDCALL;
    if (!validate_vertex_declaration(declaration, D8WG_MAX_SHADER_TOKENS,
            &decl_count))
        return D3DERR_INVALIDCALL;
    if (!validate_shader_body(function + 1, D8WG_MAX_SHADER_TOKENS, FALSE,
            1u, &body_count))
        return D3DERR_INVALIDCALL;

    entry = (D8Shader *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*entry));
    if (!entry) return E_OUTOFMEMORY;
    entry->declaration_tokens = decl_count
            ? (DWORD *)HeapAlloc(GetProcessHeap(), 0,
                    decl_count * sizeof(DWORD))
            : NULL;
    entry->code_tokens = (DWORD *)HeapAlloc(GetProcessHeap(), 0,
            (body_count + 1u) * sizeof(DWORD));
    if ((decl_count && !entry->declaration_tokens) || !entry->code_tokens) {
        HeapFree(GetProcessHeap(), 0, entry->declaration_tokens);
        HeapFree(GetProcessHeap(), 0, entry->code_tokens);
        HeapFree(GetProcessHeap(), 0, entry);
        return E_OUTOFMEMORY;
    }
    if (decl_count)
        CopyMemory(entry->declaration_tokens, declaration,
                decl_count * sizeof(DWORD));
    entry->declaration_token_count = decl_count;
    entry->code_tokens[0] = function[0];
    if (body_count)
        CopyMemory(entry->code_tokens + 1, function + 1,
                body_count * sizeof(DWORD));
    entry->code_token_count = body_count + 1u;
    entry->is_pixel_shader = FALSE;
    entry->major_version = 1;
    entry->minor_version = 1;
    entry->handle = allocate_shader_handle();

    if (!emit_create_vertex_shader(device, entry)) {
        HeapFree(GetProcessHeap(), 0, entry->declaration_tokens);
        HeapFree(GetProcessHeap(), 0, entry->code_tokens);
        HeapFree(GetProcessHeap(), 0, entry);
        return D3DERR_DRIVERINTERNALERROR;
    }
    entry->next = device->vertex_shaders;
    device->vertex_shaders = entry;
    *shader = entry->handle;
    return D3D_OK;
}

static HRESULT WINAPI device_delete_vertex_shader(IDirect3DDevice8 *iface,
        DWORD shader)
{
    D8Device *device = device_from_iface(iface);
    D8Shader **link = &device->vertex_shaders;
    D8Shader *entry;
    if (!(shader & 1u))
        return D3DERR_INVALIDCALL;
    while (*link && (*link)->handle != shader) link = &(*link)->next;
    entry = *link;
    /* Real D3D8 refuses to delete the currently bound shader; the app must
     * rebind (0 or another handle) first. */
    if (!entry || device->vertex_shader == shader)
        return D3DERR_INVALIDCALL;
    *link = entry->next;
    emit_destroy_shader(entry);
    HeapFree(GetProcessHeap(), 0, entry->declaration_tokens);
    HeapFree(GetProcessHeap(), 0, entry->code_tokens);
    HeapFree(GetProcessHeap(), 0, entry);
    return D3D_OK;
}

static HRESULT WINAPI device_set_vs_constant(IDirect3DDevice8 *iface,
        DWORD reg, const void *data, DWORD count)
{
    D8Device *device = device_from_iface(iface);
    if (!data || !count
            || (uint64_t)reg + count > D8WG_MAX_VS_CONSTANTS)
        return D3DERR_INVALIDCALL;
    CopyMemory(device->vs_constants[reg], data, count * 16u);
    return emit_set_shader_constant(D8WG_OP_SET_VERTEX_SHADER_CONSTANT,
            device, reg, (const float *)data, count)
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_vs_constant(IDirect3DDevice8 *iface,
        DWORD reg, void *data, DWORD count)
{
    D8Device *device = device_from_iface(iface);
    if (!data || !count
            || (uint64_t)reg + count > D8WG_MAX_VS_CONSTANTS)
        return D3DERR_INVALIDCALL;
    CopyMemory(data, device->vs_constants[reg], count * 16u);
    return D3D_OK;
}

static HRESULT WINAPI device_get_vs_decl(IDirect3DDevice8 *iface,
        DWORD shader, void *data, DWORD *size)
{
    D8Device *device = device_from_iface(iface);
    D8Shader *entry;
    DWORD needed;
    if (!(shader & 1u) || !size) return D3DERR_INVALIDCALL;
    entry = find_shader(device->vertex_shaders, shader);
    if (!entry) return D3DERR_INVALIDCALL;
    needed = (entry->declaration_token_count + 1u) * (DWORD)sizeof(DWORD);
    if (!data) { *size = needed; return D3D_OK; }
    if (*size < needed) return D3DERR_INVALIDCALL;
    if (entry->declaration_token_count)
        CopyMemory(data, entry->declaration_tokens,
                entry->declaration_token_count * sizeof(DWORD));
    ((DWORD *)data)[entry->declaration_token_count] = 0xFFFFFFFFu;
    *size = needed;
    return D3D_OK;
}

static HRESULT WINAPI device_get_vs_function(IDirect3DDevice8 *iface,
        DWORD shader, void *data, DWORD *size)
{
    D8Device *device = device_from_iface(iface);
    D8Shader *entry;
    DWORD needed;
    if (!(shader & 1u) || !size) return D3DERR_INVALIDCALL;
    entry = find_shader(device->vertex_shaders, shader);
    if (!entry) return D3DERR_INVALIDCALL;
    needed = (entry->code_token_count + 1u) * (DWORD)sizeof(DWORD);
    if (!data) { *size = needed; return D3D_OK; }
    if (*size < needed) return D3DERR_INVALIDCALL;
    CopyMemory(data, entry->code_tokens,
            entry->code_token_count * sizeof(DWORD));
    ((DWORD *)data)[entry->code_token_count] = 0x0000FFFFu;
    *size = needed;
    return D3D_OK;
}

static HRESULT WINAPI device_create_pixel_shader(IDirect3DDevice8 *iface,
        const DWORD *function, DWORD *shader)
{
    D8Device *device = device_from_iface(iface);
    D8Shader *entry;
    UINT body_count = 0;
    UINT major;
    UINT minor;
    DWORD version;

    if (shader) *shader = 0;
    if (!function || !shader)
        return D3DERR_INVALIDCALL;
    version = function[0];
    if ((version & 0xFFFF0000u) != 0xFFFF0000u)
        return D3DERR_INVALIDCALL;
    major = (UINT)D3DSHADER_VERSION_MAJOR(version);
    minor = (UINT)D3DSHADER_VERSION_MINOR(version);
    if (major != 1u || minor < 1u || minor > 4u)
        return D3DERR_INVALIDCALL;
    if (!validate_shader_body(function + 1, D8WG_MAX_SHADER_TOKENS, TRUE,
            minor, &body_count))
        return D3DERR_INVALIDCALL;

    entry = (D8Shader *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*entry));
    if (!entry) return E_OUTOFMEMORY;
    entry->code_tokens = (DWORD *)HeapAlloc(GetProcessHeap(), 0,
            (body_count + 1u) * sizeof(DWORD));
    if (!entry->code_tokens) {
        HeapFree(GetProcessHeap(), 0, entry);
        return E_OUTOFMEMORY;
    }
    entry->code_tokens[0] = version;
    if (body_count)
        CopyMemory(entry->code_tokens + 1, function + 1,
                body_count * sizeof(DWORD));
    entry->code_token_count = body_count + 1u;
    entry->is_pixel_shader = TRUE;
    entry->major_version = major;
    entry->minor_version = minor;
    entry->handle = allocate_shader_handle();

    if (!emit_create_pixel_shader(device, entry)) {
        HeapFree(GetProcessHeap(), 0, entry->code_tokens);
        HeapFree(GetProcessHeap(), 0, entry);
        return D3DERR_DRIVERINTERNALERROR;
    }
    entry->next = device->pixel_shaders;
    device->pixel_shaders = entry;
    *shader = entry->handle;
    return D3D_OK;
}

static HRESULT WINAPI device_set_pixel_shader(IDirect3DDevice8 *iface,
        DWORD shader)
{
    D8Device *device = device_from_iface(iface);
    if (shader && (!(shader & 1u)
            || !find_shader(device->pixel_shaders, shader)))
        return D3DERR_INVALIDCALL;
    if (device->recording_state_block)
        device->recording_state_block->state.pixel_shader_mask = TRUE;
    if (device->pixel_shader == shader)
        return D3D_OK;
    device->pixel_shader = shader;
    return emit_set_shader(D8WG_OP_SET_PIXEL_SHADER, device, shader)
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_pixel_shader(IDirect3DDevice8 *iface,
        DWORD *shader)
{
    if (!shader) return D3DERR_INVALIDCALL;
    *shader = device_from_iface(iface)->pixel_shader;
    return D3D_OK;
}

static HRESULT WINAPI device_delete_pixel_shader(IDirect3DDevice8 *iface,
        DWORD shader)
{
    D8Device *device = device_from_iface(iface);
    D8Shader **link = &device->pixel_shaders;
    D8Shader *entry;
    if (!(shader & 1u))
        return D3DERR_INVALIDCALL;
    while (*link && (*link)->handle != shader) link = &(*link)->next;
    entry = *link;
    if (!entry || device->pixel_shader == shader)
        return D3DERR_INVALIDCALL;
    *link = entry->next;
    emit_destroy_shader(entry);
    HeapFree(GetProcessHeap(), 0, entry->code_tokens);
    HeapFree(GetProcessHeap(), 0, entry);
    return D3D_OK;
}

static HRESULT WINAPI device_set_ps_constant(IDirect3DDevice8 *iface,
        DWORD reg, const void *data, DWORD count)
{
    D8Device *device = device_from_iface(iface);
    if (!data || !count
            || (uint64_t)reg + count > D8WG_MAX_PS_CONSTANTS)
        return D3DERR_INVALIDCALL;
    CopyMemory(device->ps_constants[reg], data, count * 16u);
    return emit_set_shader_constant(D8WG_OP_SET_PIXEL_SHADER_CONSTANT,
            device, reg, (const float *)data, count)
            ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI device_get_ps_constant(IDirect3DDevice8 *iface,
        DWORD reg, void *data, DWORD count)
{
    D8Device *device = device_from_iface(iface);
    if (!data || !count
            || (uint64_t)reg + count > D8WG_MAX_PS_CONSTANTS)
        return D3DERR_INVALIDCALL;
    CopyMemory(data, device->ps_constants[reg], count * 16u);
    return D3D_OK;
}

static HRESULT WINAPI device_get_ps_function(IDirect3DDevice8 *iface,
        DWORD shader, void *data, DWORD *size)
{
    D8Device *device = device_from_iface(iface);
    D8Shader *entry;
    DWORD needed;
    if (!(shader & 1u) || !size) return D3DERR_INVALIDCALL;
    entry = find_shader(device->pixel_shaders, shader);
    if (!entry) return D3DERR_INVALIDCALL;
    needed = (entry->code_token_count + 1u) * (DWORD)sizeof(DWORD);
    if (!data) { *size = needed; return D3D_OK; }
    if (*size < needed) return D3DERR_INVALIDCALL;
    CopyMemory(data, entry->code_tokens,
            entry->code_token_count * sizeof(DWORD));
    ((DWORD *)data)[entry->code_token_count] = 0x0000FFFFu;
    *size = needed;
    return D3D_OK;
}
DEV_STUB(draw_rect_patch, UINT handle, const float *segments,
        const D3DRECTPATCH_INFO *info)
{ (void)iface;(void)handle;(void)segments;(void)info; return D3DERR_INVALIDCALL; }
DEV_STUB(draw_tri_patch, UINT handle, const float *segments,
        const D3DTRIPATCH_INFO *info)
{ (void)iface;(void)handle;(void)segments;(void)info; return D3DERR_INVALIDCALL; }
DEV_STUB(delete_patch, UINT handle)
{ (void)iface;(void)handle; return D3DERR_INVALIDCALL; }

static IDirect3D8Vtbl g_d3d_vtbl = {
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
    .GetDeviceCaps = d3d_get_device_caps,
    .GetAdapterMonitor = d3d_get_adapter_monitor,
    .CreateDevice = d3d_create_device
};

static IDirect3DDevice8Vtbl g_device_vtbl = {
    .QueryInterface = device_query_interface,
    .AddRef = device_add_ref,
    .Release = device_release,
    .TestCooperativeLevel = device_test_cooperative_level,
    .GetAvailableTextureMem = device_get_available_texture_mem,
    .ResourceManagerDiscardBytes = device_discard_bytes,
    .GetDirect3D = device_get_direct3d,
    .GetDeviceCaps = device_get_caps,
    .GetDisplayMode = device_get_display_mode,
    .GetCreationParameters = device_get_creation_parameters,
    .SetCursorProperties = device_set_cursor_properties,
    .SetCursorPosition = device_set_cursor_position,
    .ShowCursor = device_show_cursor,
    .CreateAdditionalSwapChain = device_create_swapchain,
    .Reset = device_reset,
    .Present = device_present,
    .GetBackBuffer = device_get_backbuffer,
    .GetRasterStatus = device_get_raster_status,
    .SetGammaRamp = device_set_gamma,
    .GetGammaRamp = device_get_gamma,
    .CreateTexture = device_create_texture,
    .CreateVolumeTexture = device_create_volume_texture,
    .CreateCubeTexture = device_create_cube_texture,
    .CreateVertexBuffer = device_create_vertex_buffer,
    .CreateIndexBuffer = device_create_index_buffer,
    .CreateRenderTarget = device_create_render_target,
    .CreateDepthStencilSurface = device_create_depth_surface,
    .CreateImageSurface = device_create_image_surface,
    .CopyRects = device_copy_rects,
    .UpdateTexture = device_update_texture,
    .GetFrontBuffer = device_get_front_buffer,
    .SetRenderTarget = device_set_render_target,
    .GetRenderTarget = device_get_render_target,
    .GetDepthStencilSurface = device_get_depth_surface,
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
    .BeginStateBlock = device_begin_state_block,
    .EndStateBlock = device_end_state_block,
    .ApplyStateBlock = device_apply_state_block,
    .CaptureStateBlock = device_capture_state_block,
    .DeleteStateBlock = device_delete_state_block,
    .CreateStateBlock = device_create_state_block,
    .SetClipStatus = device_set_clip_status,
    .GetClipStatus = device_get_clip_status,
    .GetTexture = device_get_texture,
    .SetTexture = device_set_texture,
    .GetTextureStageState = device_get_texture_stage_state,
    .SetTextureStageState = device_set_texture_stage_state,
    .ValidateDevice = device_validate,
    .GetInfo = device_get_info,
    .SetPaletteEntries = device_set_palette,
    .GetPaletteEntries = device_get_palette,
    .SetCurrentTexturePalette = device_set_current_palette,
    .GetCurrentTexturePalette = device_get_current_palette,
    .DrawPrimitive = device_draw_primitive,
    .DrawIndexedPrimitive = device_draw_indexed,
    .DrawPrimitiveUP = device_draw_up,
    .DrawIndexedPrimitiveUP = device_draw_indexed_up,
    .ProcessVertices = device_process_vertices,
    .CreateVertexShader = device_create_vertex_shader,
    .SetVertexShader = device_set_vertex_shader,
    .GetVertexShader = device_get_vertex_shader,
    .DeleteVertexShader = device_delete_vertex_shader,
    .SetVertexShaderConstant = device_set_vs_constant,
    .GetVertexShaderConstant = device_get_vs_constant,
    .GetVertexShaderDeclaration = device_get_vs_decl,
    .GetVertexShaderFunction = device_get_vs_function,
    .SetStreamSource = device_set_stream_source,
    .GetStreamSource = device_get_stream_source,
    .SetIndices = device_set_indices,
    .GetIndices = device_get_indices,
    .CreatePixelShader = device_create_pixel_shader,
    .SetPixelShader = device_set_pixel_shader,
    .GetPixelShader = device_get_pixel_shader,
    .DeletePixelShader = device_delete_pixel_shader,
    .SetPixelShaderConstant = device_set_ps_constant,
    .GetPixelShaderConstant = device_get_ps_constant,
    .GetPixelShaderFunction = device_get_ps_function,
    .DrawRectPatch = device_draw_rect_patch,
    .DrawTriPatch = device_draw_tri_patch,
    .DeletePatch = device_delete_patch
};

static IDirect3DVertexBuffer8Vtbl g_vb_vtbl = {
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

static IDirect3DIndexBuffer8Vtbl g_ib_vtbl = {
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

static IDirect3DTexture8Vtbl g_texture_vtbl = {
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
    .GetLevelDesc = texture_get_level_desc,
    .GetSurfaceLevel = texture_get_surface_level,
    .LockRect = texture_lock_rect,
    .UnlockRect = texture_unlock_rect,
    .AddDirtyRect = texture_add_dirty_rect
};

static IDirect3DSurface8Vtbl g_surface_vtbl = {
    .QueryInterface = surface_query_interface,
    .AddRef = surface_add_ref,
    .Release = surface_release,
    .GetDevice = surface_get_device,
    .SetPrivateData = surface_set_private_data,
    .GetPrivateData = surface_get_private_data,
    .FreePrivateData = surface_free_private_data,
    .GetContainer = surface_get_container,
    .GetDesc = surface_get_desc,
    .LockRect = surface_lock_rect,
    .UnlockRect = surface_unlock_rect
};

static IDirect3DSwapChain8Vtbl g_swapchain_vtbl = {
    .QueryInterface = swapchain_query_interface,
    .AddRef = swapchain_add_ref,
    .Release = swapchain_release,
    .Present = swapchain_present,
    .GetBackBuffer = swapchain_get_backbuffer
};

/*
 * Both SDK version tokens seen in the wild must be accepted, exactly as the
 * real d3d8.dll does. Titles compiled against the DirectX 8.0 SDK pass 120
 * and titles compiled against 8.1 pass 220 (D3D_SDK_VERSION); rejecting 120
 * makes Direct3DCreate8 return NULL at the very first call, which a game can
 * only report as a generic "unable to initialize DirectX".
 */
#define D3D_SDK_VERSION_DX80 120u
#define D3D_SDK_VERSION_DX81 220u

IDirect3D8 *WINAPI Direct3DCreate8(UINT sdk_version)
{
    D8Direct3D *d3d;
    BOOL transport_ready;

    if (sdk_version != D3D_SDK_VERSION_DX80
            && sdk_version != D3D_SDK_VERSION_DX81)
        return NULL;
    EnterCriticalSection(&g_transport_lock);
    transport_ready = open_transport_locked();
    LeaveCriticalSection(&g_transport_lock);
    if (!transport_ready)
        return NULL;

    d3d = (D8Direct3D *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(*d3d));
    if (!d3d)
        return NULL;
    d3d->iface.lpVtbl = &g_d3d_vtbl;
    d3d->refcount = 1;
    emit_hello_once();
    return &d3d->iface;
}

/*
 * Secondary d3d8.dll exports.
 *
 * These exist only so that a title which statically imports them can load at
 * all; omitting them makes the loader reject the DLL before Direct3DCreate8 is
 * ever reached. The shader validators accept everything: real validation
 * already happens in CreateVertexShader/CreatePixelShader, which parse the
 * token stream against the Stage 6 supported-instruction table and reject
 * anything they cannot translate.
 */
HRESULT WINAPI ValidateVertexShader(const DWORD *shader, const DWORD *declaration,
        const D3DCAPS8 *caps, WINBOOL return_error, char **errors)
{
    (void)shader;
    (void)declaration;
    (void)caps;
    (void)return_error;
    if (errors) *errors = NULL;
    return S_OK;
}

HRESULT WINAPI ValidatePixelShader(const DWORD *shader, const D3DCAPS8 *caps,
        WINBOOL return_error, char **errors)
{
    (void)shader;
    (void)caps;
    (void)return_error;
    if (errors) *errors = NULL;
    return S_OK;
}

void WINAPI DebugSetMute(void)
{
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        initialize_session_id(instance);
        InitializeCriticalSection(&g_transport_lock);
    } else if (reason == DLL_PROCESS_DETACH) {
        EnterCriticalSection(&g_transport_lock);
        if (g_command_count)
            submit_batch_locked(FALSE);
        close_transport_locked();
        LeaveCriticalSection(&g_transport_lock);
        DeleteCriticalSection(&g_transport_lock);
    }
    return TRUE;
}
