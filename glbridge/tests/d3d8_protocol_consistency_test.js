"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const header = fs.readFileSync(path.join(root, "d3d8proxy/d3d8_protocol.h"),
    "utf8");
const executor = fs.readFileSync(
    path.join(root, "d3d8-webgpu/d3d8_executor.js"), "utf8");

function headerNumber(name) {
    const match = header.match(new RegExp("(?:#define\\s+|\\b)" + name +
        "(?:\\s+|\\s*=\\s*)(0x[0-9A-Fa-f]+|[0-9]+)u?"));
    assert.ok(match, "missing C protocol value " + name);
    return Number.parseInt(match[1], 0);
}

function jsNumber(name) {
    const match = executor.match(new RegExp("const\\s+" + name +
        "\\s*=\\s*(0x[0-9A-Fa-f]+|[0-9]+)"));
    assert.ok(match, "missing JavaScript protocol value " + name);
    return Number.parseInt(match[1], 0);
}

function fields(structName) {
    const match = header.match(new RegExp("typedef struct " + structName +
        "\\s*\\{([\\s\\S]*?)\\}\\s*" + structName + ";"));
    assert.ok(match, "missing C protocol struct " + structName);
    return [...match[1].matchAll(/\b(?:u?int(?:16|32)_t|float)\s+(\w+)(?:\[\d+\])?\s*;/g)]
        .map(item => item[1]);
}

const sharedValues = {
    D8WG_VERSION_MAJOR: 1,
    D8WG_VERSION_MINOR: 7,
    D8WG_OP_UPDATE_SURFACE: 8,
    D8WG_OP_CREATE_TEXTURE: 0x110,
    D8WG_OP_UPDATE_TEXTURE: 0x111,
    D8WG_OP_CREATE_VERTEX_SHADER: 0x120,
    D8WG_OP_CREATE_PIXEL_SHADER: 0x121,
    D8WG_OP_SET_TEXTURE: 0x202,
    D8WG_OP_SET_VIEWPORT: 0x203,
    D8WG_OP_SET_TRANSFORM: 0x204,
    D8WG_OP_SET_MATERIAL: 0x205,
    D8WG_OP_SET_LIGHT: 0x206,
    D8WG_OP_LIGHT_ENABLE: 0x207,
    D8WG_OP_SET_INDICES: 0x209,
    D8WG_OP_SET_RENDER_TARGET: 0x20B,
    D8WG_OP_SET_VERTEX_SHADER: 0x20C,
    D8WG_OP_SET_PIXEL_SHADER: 0x20D,
    D8WG_OP_SET_VERTEX_SHADER_CONSTANT: 0x20E,
    D8WG_OP_SET_PIXEL_SHADER_CONSTANT: 0x20F,
    D8WG_OP_DRAW_PRIMITIVE: 0x300,
    D8WG_OP_DRAW_INDEXED_PRIMITIVE: 0x301,
    D8WG_OP_DRAW_PRIMITIVE_UP: 0x302,
    D8WG_OP_DRAW_INDEXED_PRIMITIVE_UP: 0x303,
    D8WG_RESOURCE_BUFFER_VERTEX: 1,
    D8WG_RESOURCE_BUFFER_INDEX: 2,
    D8WG_RESOURCE_TEXTURE_2D: 3,
    D8WG_RESOURCE_VERTEX_SHADER: 4,
    D8WG_RESOURCE_PIXEL_SHADER: 5,
};

for (const [name, expected] of Object.entries(sharedValues)) {
    assert.equal(headerNumber(name), expected, name + " C value changed");
    const jsName = name.startsWith("D8WG_OP_") ?
        name.replace("D8WG_OP_", "OP_") :
        name.replace("D8WG_RESOURCE_", "RESOURCE_");
    assert.equal(jsNumber(jsName), expected, jsName + " JS value drifted");
}

assert.deepEqual(fields("D8WGSetIndices"), [
    "device_handle", "buffer_handle", "base_vertex_index", "reserved",
]);
assert.deepEqual(fields("D8WGBatchHeader"), [
    "magic", "version_major", "version_minor", "frame_id", "flags",
    "command_count", "command_bytes", "session_id_low", "session_id_high",
]);
assert.deepEqual(fields("D8WGHello"), [
    "guest_pointer_bits", "feature_bits", "session_id_low", "session_id_high",
]);
assert.deepEqual(fields("D8WGSetRenderTarget"), [
    "device_handle", "color_texture_handle", "color_level", "depth_enabled",
]);
assert.deepEqual(fields("D8WGCreateDevice"), [
    "device_handle", "hwnd", "x", "y", "width", "height",
    "backbuffer_format", "windowed", "behavior_flags",
    "enable_auto_depth_stencil", "auto_depth_stencil_format",
]);
assert.deepEqual(fields("D8WGResetDevice"), [
    "old_device_handle", "new_device_handle", "hwnd", "x", "y", "width",
    "height", "backbuffer_format", "windowed", "behavior_flags",
    "enable_auto_depth_stencil", "auto_depth_stencil_format",
]);
assert.deepEqual(fields("D8WGPresent"), [
    "device_handle", "hwnd", "x", "y", "width", "height",
]);
assert.deepEqual(fields("D8WGUpdateSurface"), [
    "device_handle", "hwnd", "x", "y", "width", "height",
]);
assert.deepEqual(fields("D8WGCreateTexture"), [
    "device_handle", "resource_handle", "width", "height", "level_count",
    "format", "usage", "pool",
]);
assert.deepEqual(fields("D8WGUpdateBuffer"), [
    "resource_handle", "destination_offset", "byte_count", "data_offset",
    "lock_flags", "reserved",
]);
assert.deepEqual(fields("D8WGUpdateTexture"), [
    "resource_handle", "level", "x", "y", "width", "height", "row_pitch",
    "data_bytes", "data_offset", "reserved",
]);
assert.deepEqual(fields("D8WGSetTexture"), [
    "device_handle", "stage", "texture_handle", "reserved",
]);
assert.deepEqual(fields("D8WGSetViewport"), [
    "device_handle", "x", "y", "width", "height", "min_z", "max_z",
    "reserved",
]);
assert.deepEqual(fields("D8WGSetTransform"), [
    "device_handle", "state", "matrix",
]);
assert.deepEqual(fields("D8WGSetMaterial"), [
    "device_handle", "diffuse", "ambient", "specular", "emissive", "power",
]);
assert.deepEqual(fields("D8WGSetLight"), [
    "device_handle", "index", "type", "diffuse", "specular", "ambient",
    "position", "direction", "range", "falloff", "attenuation", "theta",
    "phi",
]);
assert.deepEqual(fields("D8WGDrawIndexedPrimitive"), [
    "device_handle", "primitive_type", "min_vertex_index", "vertex_count",
    "start_index", "primitive_count",
]);
assert.deepEqual(fields("D8WGDrawPrimitiveUP"), [
    "device_handle", "primitive_type", "primitive_count", "stride",
    "vertex_count", "vertex_bytes", "vertex_data_offset", "reserved",
]);
assert.deepEqual(fields("D8WGDrawIndexedPrimitiveUP"), [
    "device_handle", "primitive_type", "min_vertex_index", "vertex_count",
    "primitive_count", "index_format", "stride", "index_count",
    "index_bytes", "vertex_bytes", "index_data_offset", "vertex_data_offset",
]);
assert.deepEqual(fields("D8WGCreateVertexShader"), [
    "device_handle", "resource_handle", "declaration_token_count",
    "declaration_offset", "instruction_token_count", "code_offset",
]);
assert.deepEqual(fields("D8WGCreatePixelShader"), [
    "device_handle", "resource_handle", "instruction_token_count",
    "code_offset",
]);
assert.deepEqual(fields("D8WGSetShader"), [
    "device_handle", "shader_handle",
]);
assert.deepEqual(fields("D8WGSetShaderConstant"), [
    "device_handle", "start_register", "vector_count", "data_offset",
]);

for (const size of [
    "D8WGAssertBatchHeaderSize",
    "D8WGAssertCommandHeaderSize",
    "D8WGAssertCreateDeviceSize",
    "D8WGAssertPresentSize",
    "D8WGAssertUpdateSurfaceSize",
    "D8WGAssertCreateTextureSize",
    "D8WGAssertUpdateBufferSize",
    "D8WGAssertUpdateTextureSize",
    "D8WGAssertSetTextureSize",
    "D8WGAssertSetViewportSize",
    "D8WGAssertSetTransformSize",
    "D8WGAssertSetMaterialSize",
    "D8WGAssertSetLightSize",
    "D8WGAssertLightEnableSize",
    "D8WGAssertSetIndicesSize",
    "D8WGAssertCreateVertexShaderSize",
    "D8WGAssertCreatePixelShaderSize",
    "D8WGAssertSetShaderSize",
    "D8WGAssertSetShaderConstantSize",
    "D8WGAssertDrawIndexedPrimitiveSize",
    "D8WGAssertDrawPrimitiveUPSize",
    "D8WGAssertDrawIndexedPrimitiveUPSize",
]) {
    assert.match(header, new RegExp("typedef char\\s+" + size + "\\s*\\["),
        "missing compile-time ABI assertion " + size);
}

console.log("d3d8_protocol_consistency_test: ok");
