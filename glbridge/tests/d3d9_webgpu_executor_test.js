#!/usr/bin/env node
// Executor-level tests for glbridge/d3d9-webgpu/d3d9_executor.js against a
// fake WebGPU device.
//
// These drive real D9WG batches (built byte-for-byte the way d3d9_proxy.c
// emits them) through the executor and assert on the WebGPU calls that come
// out: which shader modules, which pipeline topology, which bind group
// entries, and what actually lands in the shader constant buffer. The fake
// device reproduces the two validation rules that bite hardest in practice --
// a bind group must supply exactly the bindings its layout declares, and
// writeBuffer offsets/sizes must be multiples of 4 -- so a wiring mistake
// fails here rather than as a silent black screen inside v86.

"use strict";

const assert = require("node:assert/strict");
const { D3D9WebGPUExecutor, buildFixedFunctionPixelShader } =
    require("../d3d9-webgpu/d3d9_executor.js");
const shaderPipeline = require("../d3d9-webgpu/d3d9_shader_pipeline.js");

const OP = {
    HELLO: 1, CREATE_DEVICE: 2, RESET: 3, PRESENT: 4, CLEAR: 5, COLOR_FILL: 9,
    BEGIN_SCENE: 6, END_SCENE: 7, GUEST_LOG: 11,
    CREATE_BUFFER: 0x100, UPDATE_BUFFER: 0x101, DESTROY_RESOURCE: 0x103,
    CREATE_TEXTURE_2D: 0x110, UPDATE_TEXTURE: 0x113,
    CREATE_VERTEX_DECLARATION: 0x120,
    CREATE_VERTEX_SHADER: 0x121, CREATE_PIXEL_SHADER: 0x122,
    SET_RENDER_STATE: 0x200, SET_SAMPLER_STATE: 0x201,
    SET_TEXTURE: 0x203, SET_VIEWPORT: 0x204, SET_TRANSFORM: 0x206,
    SET_MATERIAL: 0x207, SET_LIGHT: 0x208, LIGHT_ENABLE: 0x209,
    SET_STREAM_SOURCE: 0x20A, SET_INDICES: 0x20C,
    SET_VERTEX_DECLARATION: 0x20D, SET_FVF: 0x20E,
    SET_VERTEX_SHADER: 0x211, SET_PIXEL_SHADER: 0x212,
    SET_VS_CONST_F: 0x213, SET_VS_CONST_I: 0x214, SET_VS_CONST_B: 0x215,
    SET_PS_CONST_F: 0x216, SET_PS_CONST_I: 0x217, SET_PS_CONST_B: 0x218,
    DRAW_PRIMITIVE: 0x300, DRAW_INDEXED_PRIMITIVE: 0x301,
    DRAW_PRIMITIVE_UP: 0x302, DRAW_INDEXED_PRIMITIVE_UP: 0x303,
};

const D9WG_MAGIC = 0x47573944;
const BATCH_FLAG_PRESENT = 1;
const DECLUSAGE = { POSITION: 0, BLENDWEIGHT: 1, BLENDINDICES: 2, NORMAL: 3,
    PSIZE: 4, TEXCOORD: 5, TANGENT: 6, BINORMAL: 7, POSITIONT: 9, COLOR: 10 };
const DECLTYPE = { FLOAT1: 0, FLOAT2: 1, FLOAT3: 2, FLOAT4: 3, D3DCOLOR: 4,
    UBYTE4: 5, SHORT2: 6, SHORT4: 7, UBYTE4N: 8, UDEC3: 13, DEC3N: 14 };
const DEVICE = 0x00100002;

// ---- D9WG batch builder ----
//
// `blob` is the trailing variable-length payload some commands carry (shader
// bytecode, vertex data, constant values); the builder patches the recorded
// offset once the command's position in the batch is known, exactly as
// reserve_command_locked() does on the guest.

function command(opcode, payload, blob, blobOffsetField) {
    return { opcode, payload, blob: blob || null, blobOffsetField };
}

function buildBatch(commands, options = {}) {
    let commandBytes = 0;
    for (const item of commands) {
        const raw = 16 + item.payload.length + (item.blob ? item.blob.length : 0);
        item.size = (raw + 7) & ~7;
        item.offset = 32 + commandBytes;
        commandBytes += item.size;
    }
    const batch = Buffer.alloc(32 + commandBytes);
    batch.writeUInt32LE(D9WG_MAGIC, 0);
    batch.writeUInt16LE(1, 4);
    batch.writeUInt16LE(0, 6);
    batch.writeUInt32LE(options.frameId || 1, 8);
    batch.writeUInt32LE(options.present ? BATCH_FLAG_PRESENT : 0, 12);
    batch.writeUInt32LE(commands.length, 16);
    batch.writeUInt32LE(commandBytes, 20);
    let sequence = 1;
    for (const item of commands) {
        batch.writeUInt16LE(item.opcode, item.offset);
        batch.writeUInt32LE(item.size, item.offset + 4);
        batch.writeUInt32LE(sequence++, item.offset + 8);
        if (item.blob) {
            const blobOffset = item.offset + 16 + item.payload.length;
            item.payload.writeUInt32LE(blobOffset, item.blobOffsetField);
            item.blob.copy(batch, blobOffset);
        }
        item.payload.copy(batch, item.offset + 16);
    }
    return batch;
}

function u32(...values) {
    const buffer = Buffer.alloc(values.length * 4);
    values.forEach((value, index) => buffer.writeUInt32LE(value >>> 0, index * 4));
    return buffer;
}

function createDevicePayload(width, height, autoDepth = 1) {
    const payload = Buffer.alloc(44);
    payload.writeUInt32LE(DEVICE, 0);
    payload.writeUInt32LE(0x1234, 4);
    payload.writeUInt32LE(width, 16);
    payload.writeUInt32LE(height, 20);
    payload.writeUInt32LE(22, 24);
    payload.writeUInt32LE(1, 28);
    payload.writeUInt32LE(autoDepth, 36);
    return payload;
}

function element(stream, offset, type, usage, usageIndex = 0) {
    const buffer = Buffer.alloc(8);
    buffer.writeUInt16LE(stream, 0);
    buffer.writeUInt16LE(offset, 2);
    buffer.writeUInt8(type, 4);
    buffer.writeUInt8(0, 5);
    buffer.writeUInt8(usage, 6);
    buffer.writeUInt8(usageIndex, 7);
    return buffer;
}

function declarationPayload(handle, elements) {
    return Buffer.concat([u32(DEVICE, handle, elements.length, 0), ...elements]);
}

function fvfPayload(fvf, elements) {
    return Buffer.concat([u32(DEVICE, fvf, elements.length, 0), ...elements]);
}

function createBufferPayload(handle, kind, byteCount, format = 0) {
    return u32(DEVICE, handle, kind, byteCount, 0, format, 0, 0);
}

function setStreamSourcePayload(stream, handle, stride, offsetInBytes = 0) {
    return u32(DEVICE, stream, handle, stride, offsetInBytes, 0);
}

function drawPrimitivePayload(type, startVertex, primitiveCount) {
    return u32(DEVICE, type, startVertex, primitiveCount);
}

function drawIndexedPayload(type, baseVertex, startIndex, primitiveCount) {
    const payload = Buffer.alloc(28);
    payload.writeUInt32LE(DEVICE, 0);
    payload.writeUInt32LE(type, 4);
    payload.writeInt32LE(baseVertex, 8);
    payload.writeUInt32LE(0, 12);
    payload.writeUInt32LE(0, 16);
    payload.writeUInt32LE(startIndex, 20);
    payload.writeUInt32LE(primitiveCount, 24);
    return payload;
}

function shaderCreatePayload(handle, tokens) {
    const payload = Buffer.alloc(24);
    payload.writeUInt32LE(DEVICE, 0);
    payload.writeUInt32LE(handle, 4);
    payload.writeUInt32LE(tokens.length, 8);
    const hash = shaderPipeline.hashTokens(tokens);
    payload.writeUInt32LE(hash.low, 16);
    payload.writeUInt32LE(hash.high, 20);
    const blob = Buffer.alloc(tokens.length * 4);
    tokens.forEach((token, index) => blob.writeUInt32LE(token >>> 0, index * 4));
    return { payload, blob, blobOffsetField: 12 };
}

function constantPayload(startRegister, vectorCount, values, writer) {
    const payload = Buffer.alloc(16);
    payload.writeUInt32LE(DEVICE, 0);
    payload.writeUInt32LE(startRegister, 4);
    payload.writeUInt32LE(vectorCount, 8);
    const stride = values.length / vectorCount;
    const blob = Buffer.alloc(values.length * 4);
    values.forEach((value, index) => writer(blob, value, index * 4));
    void stride;
    return { payload, blob, blobOffsetField: 12 };
}

const floatConstants = (start, values) => constantPayload(start, values.length / 4,
    values, (buffer, value, at) => buffer.writeFloatLE(value, at));
const intConstants = (start, values) => constantPayload(start, values.length / 4,
    values, (buffer, value, at) => buffer.writeInt32LE(value, at));
const boolConstants = (start, values) => constantPayload(start, values.length,
    values, (buffer, value, at) => buffer.writeUInt32LE(value, at));

// ---- shader bytecode fixtures ----

const VS = (major, minor) => (0xfffe0000 | (major << 8) | minor) >>> 0;
const PS = (major, minor) => (0xffff0000 | (major << 8) | minor) >>> 0;
const END = 0x0000ffff;
const REG = shaderPipeline.REGISTER;
const SIO = shaderPipeline.OP;
const regTypeBits = type => (((type & 0x7) << 28) | ((type & 0x18) << 8)) >>> 0;
const instr = (opcode, length = 0) =>
    ((opcode & 0xffff) | ((length & 0xf) << 24)) >>> 0;
const dst = (type, index, mask = 0xf) =>
    (0x80000000 | (index & 0x7ff) | regTypeBits(type) | (mask << 16)) >>> 0;
const src = (type, index) =>
    (0x80000000 | (index & 0x7ff) | regTypeBits(type) | (0xe4 << 16)) >>> 0;
const dcl = (usage, usageIndex = 0, textureType = 0) =>
    (0x80000000 | usage | (usageIndex << 16) | (textureType << 27)) >>> 0;

// vs_2_0: dcl_position v0 / dcl_color0 v1 / m4x4 oPos, v0, c0 / mov oD0, v1
const VS_BYTECODE = [
    VS(2, 0),
    instr(SIO.DCL, 2), dcl(DECLUSAGE.POSITION), dst(REG.INPUT, 0),
    instr(SIO.DCL, 2), dcl(DECLUSAGE.COLOR, 0), dst(REG.INPUT, 1),
    instr(SIO.M4x4, 3), dst(REG.RASTOUT, 0), src(REG.INPUT, 0), src(REG.CONST, 0),
    instr(SIO.MOV, 2), dst(REG.ATTROUT, 0), src(REG.INPUT, 1),
    END,
];

// M5 skeletal-layout fixture. The maths is intentionally small; the important
// contract here is that BLENDWEIGHT/BLENDINDICES and the compact auxiliary
// semantics all reach v# with D3D9 float4 values, ready for a real matrix
// palette shader to index the c# register file.
const VS_M5_SKINNING_INPUTS = [
    VS(2, 0),
    instr(SIO.DCL, 2), dcl(DECLUSAGE.POSITION), dst(REG.INPUT, 0),
    instr(SIO.DCL, 2), dcl(DECLUSAGE.BLENDWEIGHT), dst(REG.INPUT, 1),
    instr(SIO.DCL, 2), dcl(DECLUSAGE.BLENDINDICES), dst(REG.INPUT, 2),
    instr(SIO.DCL, 2), dcl(DECLUSAGE.TEXCOORD), dst(REG.INPUT, 3),
    instr(SIO.DCL, 2), dcl(DECLUSAGE.TANGENT), dst(REG.INPUT, 4),
    instr(SIO.DCL, 2), dcl(DECLUSAGE.NORMAL), dst(REG.INPUT, 5),
    instr(SIO.DCL, 2), dcl(DECLUSAGE.BINORMAL), dst(REG.INPUT, 6),
    instr(SIO.M4x4, 3), dst(REG.RASTOUT, 0), src(REG.INPUT, 0), src(REG.CONST, 0),
    instr(SIO.ADD, 3), dst(REG.ATTROUT, 0), src(REG.INPUT, 1), src(REG.INPUT, 2),
    END,
];

// ps_2_0: dcl_2d s0 / dcl t0 / texld r0, t0, s0 / mul oC0, r0, c1
const PS_BYTECODE = [
    PS(2, 0),
    instr(SIO.DCL, 2), dcl(0, 0, 2), dst(REG.SAMPLER, 0),
    instr(SIO.DCL, 2), dcl(DECLUSAGE.TEXCOORD, 0), dst(REG.TEXTURE, 0),
    instr(SIO.TEX, 3), dst(REG.TEMP, 0), src(REG.TEXTURE, 0), src(REG.SAMPLER, 0),
    instr(SIO.MUL, 3), dst(REG.COLOROUT, 0), src(REG.TEMP, 0), src(REG.CONST, 1),
    END,
];

// A shader the translator refuses (ps_1_x bump environment mapping).
const PS_UNSUPPORTED = [
    PS(1, 1),
    instr(SIO.TEX), dst(REG.TEXTURE, 0),
    instr(SIO.TEXBEM), dst(REG.TEXTURE, 1), src(REG.TEXTURE, 0),
    END,
];

// ---- fake WebGPU ----

function makeFakeWebGPU() {
    const calls = [];
    const submittedWorkResolvers = [];
    class FakeBuffer {
        constructor(descriptor) { this.descriptor = descriptor; this.size = descriptor.size; }
        destroy() { this.destroyed = true; }
    }
    class FakeTexture {
        constructor(descriptor) { this.descriptor = descriptor; }
        createView(descriptor) {
            const view = { texture: this, descriptor: descriptor || null };
            calls.push(["createView", this, descriptor || null, view]);
            return view;
        }
        destroy() { this.destroyed = true; }
    }
    class FakePass {
        constructor(descriptor) { this.descriptor = descriptor; this.ops = []; }
        setPipeline(p) { this.ops.push(["pipeline", p]); }
        setBindGroup(i, g) { this.ops.push(["bindGroup", i, g]); }
        setViewport(...a) { this.ops.push(["viewport", ...a]); }
        setScissorRect(...a) {
            this.ops.push(["scissor", ...a]);
            calls.push(["setScissorRect", ...a]);
        }
        setBlendConstant(value) {
            this.ops.push(["blendConstant", value]);
            calls.push(["setBlendConstant", value]);
        }
        setStencilReference(value) {
            this.ops.push(["stencilReference", value]);
            calls.push(["setStencilReference", value]);
        }
        setVertexBuffer(slot, buffer, offset) {
            this.ops.push(["vertexBuffer", slot, buffer, offset]);
        }
        setIndexBuffer(buffer, format, offset) {
            this.ops.push(["indexBuffer", buffer, format, offset]);
        }
        draw(...a) { this.ops.push(["draw", ...a]); }
        drawIndexed(...a) { this.ops.push(["drawIndexed", ...a]); }
        end() { this.ended = true; }
    }
    class FakeEncoder {
        constructor() { this.passes = []; }
        beginRenderPass(descriptor) {
            const pass = new FakePass(descriptor);
            this.passes.push(pass);
            calls.push(["beginRenderPass", descriptor, pass]);
            return pass;
        }
        copyTextureToTexture(...args) {
            calls.push(["copyTextureToTexture", ...args]);
        }
        finish() { return { encoder: this }; }
    }
    const queue = {
        writeBuffer(buffer, offset, data, dataOffset, size) {
            const length = size !== undefined ? size
                : (data.byteLength !== undefined ? data.byteLength : data.length);
            assert.equal(offset % 4, 0, "writeBuffer destination offset must be 4-aligned");
            assert.equal(length % 4, 0, "writeBuffer size must be a multiple of 4");
            // WebGPU copies the source at call time. Snapshotting here rather
            // than holding the caller's view matters: the executor writes
            // straight out of a buffer's CPU shadow, which keeps mutating, so
            // a live reference would make every recorded write appear to
            // contain the frame's final contents.
            const view = ArrayBuffer.isView(data)
                ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
                : new Uint8Array(data);
            const start = ArrayBuffer.isView(data) ? (dataOffset || 0) * 1 : (dataOffset || 0);
            const snapshot = view.slice(start, start + length);
            calls.push(["writeBuffer", buffer, offset, data, dataOffset, size, snapshot]);
        },
        writeTexture(...a) { calls.push(["writeTexture", ...a]); },
        submit(buffers) { calls.push(["submit", buffers]); },
        onSubmittedWorkDone() {
            return new Promise(resolve => submittedWorkResolvers.push(resolve));
        },
    };
    const device = {
        queue,
        lost: new Promise(() => {}),
        createShaderModule(descriptor) {
            const module = { descriptor, code: descriptor.code,
                getCompilationInfo: async () => ({ messages: [] }) };
            calls.push(["createShaderModule", descriptor, module]);
            return module;
        },
        createBuffer(descriptor) {
            const buffer = new FakeBuffer(descriptor);
            calls.push(["createBuffer", descriptor, buffer]);
            return buffer;
        },
        createTexture(descriptor) {
            const texture = new FakeTexture(descriptor);
            calls.push(["createTexture", descriptor, texture]);
            return texture;
        },
        createSampler(descriptor) {
            const sampler = { descriptor };
            calls.push(["createSampler", descriptor, sampler]);
            return sampler;
        },
        createCommandEncoder() {
            const encoder = new FakeEncoder();
            calls.push(["createCommandEncoder", encoder]);
            return encoder;
        },
        createBindGroupLayout(descriptor) {
            const layout = { descriptor,
                bindings: new Set(descriptor.entries.map(e => e.binding)) };
            calls.push(["createBindGroupLayout", descriptor, layout]);
            return layout;
        },
        createPipelineLayout(descriptor) {
            calls.push(["createPipelineLayout", descriptor]);
            return { descriptor };
        },
        createRenderPipeline(descriptor) {
            const pipeline = { descriptor };
            calls.push(["createRenderPipeline", descriptor, pipeline]);
            return pipeline;
        },
        createBindGroup(descriptor) {
            const declared = descriptor.layout.bindings;
            const supplied = new Set(descriptor.entries.map(e => e.binding));
            for (const binding of supplied)
                assert.ok(declared.has(binding),
                    "bind group supplies binding " + binding +
                    " which its layout does not declare");
            for (const binding of declared)
                assert.ok(supplied.has(binding),
                    "bind group layout declares binding " + binding +
                    " but the bind group does not supply it");
            calls.push(["createBindGroup", descriptor]);
            return descriptor;
        },
    };
    const context = {
        configure(descriptor) { calls.push(["configure", descriptor]); },
        getCurrentTexture() {
            return { width: 640, height: 480, createView: () => ({ swapchain: true }) };
        },
    };
    const gpu = {
        async requestAdapter() {
            return {
                // A real adapter advertises optional features here, and a device
                // gets none of them unless it names them in requiredFeatures.
                features: new Set(["texture-compression-bc"]),
                async requestDevice(descriptor) {
                    calls.push(["requestDevice", descriptor || null]);
                    return device;
                },
            };
        },
        getPreferredCanvasFormat() { return "bgra8unorm"; },
    };
    return { calls, device, context, gpu,
        completeSubmittedWork() {
            for (const resolve of submittedWorkResolvers.splice(0)) resolve();
        } };
}

function makeExecutor(options = {}) {
    const fake = makeFakeWebGPU();
    const canvas = { width: 1, height: 1, getContext: () => fake.context };
    const executor = new D3D9WebGPUExecutor(canvas, { gpu: fake.gpu, ...options });
    return { fake, executor,
        find: name => fake.calls.filter(call => call[0] === name),
        last: name => {
            const matches = fake.calls.filter(call => call[0] === name);
            return matches[matches.length - 1];
        } };
}

// ---- harness ----

const failures = [];
let passed = 0;

async function test(name, body) {
    try {
        await body();
        ++passed;
    } catch (error) {
        failures.push({ name, error });
    }
}

// ---- tests ----

async function main() {

await test("fixed-function FVF triangle still renders (M1 regression guard)", async () => {
    const { executor, fake, find } = makeExecutor();
    const vertices = Buffer.alloc(3 * 16);
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(0, 12, DECLTYPE.D3DCOLOR, DECLUSAGE.COLOR),
    ];
    const create = shaderCreatePayload; void create;
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, vertices.length)),
        command(OP.SET_FVF, fvfPayload(0x142, elements)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 16)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.drawCalls, 1, "the draw was not recorded");
    assert.equal(executor.stats.droppedDraws, 0);
    const pipeline = find("createRenderPipeline").pop()[1];
    assert.equal(pipeline.primitive.topology, "triangle-list");
    assert.equal(pipeline.vertex.entryPoint, "d9_vs_main");
    assert.equal(pipeline.fragment.entryPoint, "d9_ps_main");
    // Position at location 0, diffuse at location 1, both from stream 0.
    assert.equal(pipeline.vertex.buffers.length, 1);
    assert.equal(pipeline.vertex.buffers[0].arrayStride, 16);
    assert.deepEqual(pipeline.vertex.buffers[0].attributes, [
        { shaderLocation: 0, offset: 0, format: "float32x3" },
        { shaderLocation: 1, offset: 12, format: "unorm8x4" },
    ]);
    const passes = fake.calls.filter(c => c[0] === "beginRenderPass");
    assert.equal(passes.length, 1);
    assert.deepEqual(passes[0][2].ops.filter(op => op[0] === "draw"), [["draw", 3]]);
});

await test("point sprites expand one source point into an instanced textured quad", async () => {
    const D3DRS_POINTSIZE = 154, D3DRS_POINTSPRITEENABLE = 156;
    const D3DRS_POINTSCALEENABLE = 157;
    const { executor, fake, find } = makeExecutor();
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(0, 12, DECLTYPE.FLOAT1, DECLUSAGE.PSIZE),
        element(0, 16, DECLTYPE.D3DCOLOR, DECLUSAGE.COLOR),
    ];
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 20 * 8)),
        command(OP.SET_FVF, fvfPayload(0x142, elements)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_POINTSIZE,
            0x41000000, 0)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_POINTSPRITEENABLE, 1, 0)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_POINTSCALEENABLE, 1, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(1, 0, 8)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();

    assert.equal(executor.stats.droppedDraws, 0);
    assert.equal(executor.stats.pointSpriteDraws, 1);
    assert.equal(executor.stats.pointSpriteInstances, 8);
    const pipeline = find("createRenderPipeline").pop()[1];
    assert.equal(pipeline.primitive.topology, "triangle-list");
    assert.equal(pipeline.primitive.cullMode, "none",
        "expanded points must not inherit triangle culling");
    assert.equal(pipeline.vertex.buffers[0].stepMode, "instance");
    assert.ok(pipeline.vertex.buffers[0].attributes.some(attribute =>
        attribute.shaderLocation === 12 && attribute.format === "float32"),
        "D3DDECLUSAGE_PSIZE must feed the point-size input");
    assert.ok(pipeline.vertex.module.code.includes("d9_point_uvs"));
    assert.ok(pipeline.vertex.module.code.includes("inverseSqrt(d9_point_denom)"),
        "D3DRS_POINTSCALEENABLE must apply A/B/C distance attenuation");
    assert.ok(pipeline.vertex.module.code.includes("result.varying2 = vec4<f32>(d9_point_uv"),
        "point-sprite UVs must replace TEXCOORD0");
    const pass = fake.calls.filter(call => call[0] === "beginRenderPass").pop()[2];
    assert.deepEqual(pass.ops.filter(op => op[0] === "draw"), [["draw", 6, 8]],
        "eight points should be one six-vertex instanced draw");
});

await test("fixed-function attribute locations follow semantics, not element order", async () => {
    // TEXCOORD declared before COLOR. M1 assigned locations by iteration
    // order while the WGSL hardcoded colour at location 1, so this
    // declaration fed texcoord bytes into the colour attribute.
    const { executor, find } = makeExecutor();
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(0, 12, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD),
        element(0, 20, DECLTYPE.D3DCOLOR, DECLUSAGE.COLOR),
    ];
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.CREATE_VERTEX_DECLARATION, declarationPayload(0x301, elements)),
        command(OP.SET_VERTEX_DECLARATION, u32(DEVICE, 0x301)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 24)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    const pipeline = find("createRenderPipeline").pop()[1];
    const byLocation = new Map(pipeline.vertex.buffers[0].attributes
        .map(a => [a.shaderLocation, a]));
    assert.equal(byLocation.get(1).offset, 20, "COLOR0 must stay at location 1");
    assert.equal(byLocation.get(1).format, "unorm8x4");
    // M3 widened the fixed-function location table to make room for NORMAL and
    // COLOR1 (lighting) plus all eight coordinate sets, so TEXCOORD0 moved from
    // 2 to 4. The property under test is unchanged: the location follows the
    // semantic, not the element's position in the declaration.
    assert.equal(byLocation.get(4).offset, 12, "TEXCOORD0 must stay at location 4");
    assert.equal(byLocation.get(4).format, "float32x2");
});

await test("programmable vs+ps: modules, bindings and constants all line up", async () => {
    const { executor, find } = makeExecutor();
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(0, 12, DECLTYPE.D3DCOLOR, DECLUSAGE.COLOR),
    ];
    const vs = shaderCreatePayload(0x40000001, VS_BYTECODE);
    const ps = shaderCreatePayload(0x40000003, PS_BYTECODE);
    const vsConst = floatConstants(0, [
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1,
    ]);
    const psConst = floatConstants(1, [0.25, 0.5, 0.75, 1]);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x401, 4, 4, 1, 21, 0, 1)),
        command(OP.CREATE_VERTEX_DECLARATION, declarationPayload(0x301, elements)),
        command(OP.CREATE_VERTEX_SHADER, vs.payload, vs.blob, vs.blobOffsetField),
        command(OP.CREATE_PIXEL_SHADER, ps.payload, ps.blob, ps.blobOffsetField),
        command(OP.SET_VERTEX_DECLARATION, u32(DEVICE, 0x301)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 16)),
        command(OP.SET_VERTEX_SHADER, u32(DEVICE, 0x40000001)),
        command(OP.SET_PIXEL_SHADER, u32(DEVICE, 0x40000003)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x401, 0)),
        command(OP.SET_VS_CONST_F, vsConst.payload, vsConst.blob, vsConst.blobOffsetField),
        command(OP.SET_PS_CONST_F, psConst.payload, psConst.blob, psConst.blobOffsetField),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();

    assert.equal(executor.stats.shadersTranslated, 2);
    assert.equal(executor.stats.shaderTranslationFailures, 0);
    // One extra translation for the D3DCOLOR-corrected vertex variant.
    assert.equal(executor.stats.shaderVariantsTranslated, 1);
    assert.equal(executor.stats.droppedDraws, 0, "the programmable draw was dropped");
    assert.equal(executor.stats.programmableDraws, 1);

    // Vertex and fragment must come from two different modules.
    const pipeline = find("createRenderPipeline").pop()[1];
    assert.notEqual(pipeline.vertex.module, pipeline.fragment.module);
    assert.ok(pipeline.vertex.module.code.includes("@vertex"));
    assert.ok(pipeline.fragment.module.code.includes("@fragment"));
    // The v1 COLOR0 input is D3DCOLOR, so the module must swizzle it.
    assert.ok(/vin1 = in1\.bgra;/.test(pipeline.vertex.module.code),
        "D3DCOLOR vertex input was not corrected to RGBA:\n" +
        pipeline.vertex.module.code);
    // Locations follow the shader's own v# register numbers.
    assert.deepEqual(pipeline.vertex.buffers[0].attributes, [
        { shaderLocation: 0, offset: 0, format: "float32x3" },
        { shaderLocation: 1, offset: 12, format: "unorm8x4" },
    ]);

    // Bind group layout: vertex constants at 0, pixel constants at 1,
    // sampler 0's texture/sampler pair at 2/3.
    const layout = find("createBindGroupLayout").pop()[1];
    assert.deepEqual(layout.entries.map(e => e.binding).sort((a, b) => a - b),
        [0, 1, 2, 3]);
    const bindGroup = find("createBindGroup").pop()[1];
    const entries = new Map(bindGroup.entries.map(e => [e.binding, e]));
    assert.ok(entries.get(0).resource.buffer, "vertex constants are not a buffer");
    assert.equal(entries.get(0).resource.offset, 0);
    assert.equal(entries.get(1).resource.offset % 256, 0,
        "the pixel constant region must start on a 256-byte boundary");

    // And the values themselves: c0..c3 for the vertex stage, c1 for the
    // pixel stage at its own offset.
    const write = find("writeBuffer").filter(
        call => call[1] === entries.get(0).resource.buffer).pop();
    const data = new DataView(write[3]);
    assert.equal(data.getFloat32(12 * 4, true), 5, "vs c3.x");
    assert.equal(data.getFloat32(13 * 4, true), 6, "vs c3.y");
    const pixelBase = entries.get(1).resource.offset;
    assert.equal(data.getFloat32(pixelBase + 16, true), 0.25, "ps c1.x");
    assert.equal(data.getFloat32(pixelBase + 28, true), 1, "ps c1.w");
});

await test("persistent WGSL cache is restored before CREATE_SHADER executes", async () => {
    const cache = new shaderPipeline.D3D9ShaderCache();
    const stream = new Uint32Array(VS_BYTECODE);
    const hash = shaderPipeline.hashTokens(stream);
    cache.compile(stream, hash.low, hash.high);
    const payload = cache.exportEntries();
    let loads = 0;
    const storage = {
        async load() { ++loads; return payload; },
        async save() { throw new Error("a restored hit must not schedule a save"); },
    };
    const { executor } = makeExecutor({ shaderCacheStorage: storage });
    const vs = shaderCreatePayload(0x40000100, VS_BYTECODE);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_VERTEX_SHADER, vs.payload, vs.blob, vs.blobOffsetField),
    ]));
    await executor.idle();
    const stats = executor.getStats();
    assert.equal(loads, 1);
    assert.equal(stats.shaderCachePersistentLoads, 1);
    assert.equal(stats.shaderCachePersistentBackend, "injected");
    assert.equal(stats.shaderCacheMisses, 0);
    assert.equal(stats.shaderCacheHits, 1);
    assert.equal(stats.shadersCached, 1);
    assert.ok(stats.shaderWGSLBytesCached > 0);
    assert.deepEqual(stats.occlusionQueries, { mode: "guest-conservative",
        slotsUsed: 0, slotsCapacity: 0, slotExhaustionFallbacks: 0 });
});

await test("CREATE_SHADER translation can run through the M6 Worker path", async () => {
    class CompileWorker {
        postMessage(message) {
            queueMicrotask(() => this.onmessage({ data: { id: message.id,
                result: shaderPipeline.compileShader(
                    new Uint32Array(message.tokens)) } }));
        }
        terminate() {}
    }
    const { executor } = makeExecutor({ Worker: CompileWorker,
        shaderWorkerUrl: "fake://d3d9_shader_worker.js" });
    const vs = shaderCreatePayload(0x40000101, VS_BYTECODE);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_VERTEX_SHADER, vs.payload, vs.blob, vs.blobOffsetField),
    ]));
    await executor.idle();
    const stats = executor.getStats();
    assert.equal(stats.shaderWorkerCompiles, 1);
    assert.equal(stats.shaderWorkerFallbacks, 0);
    assert.equal(stats.shaderCacheMisses, 1);
    assert.equal(stats.shaderCompileLatencyMs.samples, 1);
});

await test("shader `def` literals override app-set constants for that register", async () => {
    const { executor, find } = makeExecutor();
    // ps_2_0 with `def c0, 0.5, 0.25, 0, 1` and mov oC0, c0.
    const bytecode = [
        PS(2, 0),
        instr(SIO.DEF, 5), dst(REG.CONST, 0),
        0x3f000000, 0x3e800000, 0x00000000, 0x3f800000,
        instr(SIO.MOV, 2), dst(REG.COLOROUT, 0), src(REG.CONST, 0),
        END,
    ];
    const ps = shaderCreatePayload(0x40000005, bytecode);
    const psConst = floatConstants(0, [9, 9, 9, 9]);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.CREATE_PIXEL_SHADER, ps.payload, ps.blob, ps.blobOffsetField),
        command(OP.SET_PIXEL_SHADER, u32(DEVICE, 0x40000005)),
        command(OP.SET_PS_CONST_F, psConst.payload, psConst.blob, psConst.blobOffsetField),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    const bindGroup = find("createBindGroup").pop()[1];
    const pixelEntry = bindGroup.entries.find(e => e.binding === 1);
    const write = find("writeBuffer").filter(
        call => call[1] === pixelEntry.resource.buffer).pop();
    const data = new DataView(write[3]);
    const base = pixelEntry.resource.offset;
    assert.equal(data.getFloat32(base, true), 0.5,
        "def c0 must win over SetPixelShaderConstantF");
    assert.equal(data.getFloat32(base + 4, true), 0.25);
});

await test("int and bool constant registers land after the float region", async () => {
    const { executor, find } = makeExecutor();
    // vs_2_0 with rep i0 { add r0, r0, c0 } and if b0.
    const bytecode = [
        VS(2, 0),
        instr(SIO.DCL, 2), dcl(DECLUSAGE.POSITION), dst(REG.INPUT, 0),
        instr(SIO.REP, 1), src(REG.CONSTINT, 0),
        instr(SIO.ADD, 3), dst(REG.TEMP, 0), src(REG.TEMP, 0), src(REG.CONST, 0),
        instr(SIO.ENDREP),
        instr(SIO.IF, 1), src(REG.CONSTBOOL, 0),
        instr(SIO.MOV, 2), dst(REG.TEMP, 0), src(REG.CONST, 1),
        instr(SIO.ENDIF),
        instr(SIO.MOV, 2), dst(REG.RASTOUT, 0), src(REG.TEMP, 0),
        END,
    ];
    const vs = shaderCreatePayload(0x40000007, bytecode);
    const ints = intConstants(0, [3, 0, 1, 0]);
    const bools = boolConstants(0, [1]);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.CREATE_VERTEX_SHADER, vs.payload, vs.blob, vs.blobOffsetField),
        command(OP.SET_VERTEX_SHADER, u32(DEVICE, 0x40000007)),
        command(OP.SET_VS_CONST_I, ints.payload, ints.blob, ints.blobOffsetField),
        command(OP.SET_VS_CONST_B, bools.payload, bools.blob, bools.blobOffsetField),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    const bindGroup = find("createBindGroup").pop()[1];
    const write = find("writeBuffer").filter(
        call => call[1] === bindGroup.entries[0].resource.buffer).pop();
    const data = new DataView(write[3]);
    // The shader reads c0 and c1, so the float region is two vec4s (32 bytes),
    // then i0 (16 bytes), then the bool vector.
    assert.equal(data.getInt32(32, true), 3, "i0.x");
    assert.equal(data.getInt32(40, true), 1, "i0.z");
    assert.equal(data.getUint32(48, true), 1, "b0");
});

await test("a shader the translator refuses skips its draws and keeps the batch alive", async () => {
    const { executor } = makeExecutor();
    const ps = shaderCreatePayload(0x40000009, PS_UNSUPPORTED);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.CREATE_PIXEL_SHADER, ps.payload, ps.blob, ps.blobOffsetField),
        command(OP.SET_PIXEL_SHADER, u32(DEVICE, 0x40000009)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.SET_PIXEL_SHADER, u32(DEVICE, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.failed, null, "the batch must not fail as a whole");
    assert.equal(executor.stats.shaderTranslationFailures, 1);
    assert.equal(executor.stats.droppedDraws, 1, "only the shader-bound draw is skipped");
    assert.equal(executor.stats.drawsSkippedForBadShader, 1,
        "the skip must be attributed to the shader, not to missing geometry");
    assert.equal(executor.stats.drawCalls, 1, "the fixed-function draw still ran");
});

await test("independent sampler state drives the GPUSampler, not the texture", async () => {
    const { executor, find } = makeExecutor();
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(0, 12, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD),
    ];
    const D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV = 2;
    const D3DSAMP_MAGFILTER = 5, D3DSAMP_MINFILTER = 6, D3DSAMP_MIPFILTER = 7;
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x401, 4, 4, 1, 21, 0, 1)),
        command(OP.SET_FVF, fvfPayload(0x102, elements)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x401, 0)),
        command(OP.SET_SAMPLER_STATE, u32(DEVICE, 0, D3DSAMP_ADDRESSU, 3)), // CLAMP
        command(OP.SET_SAMPLER_STATE, u32(DEVICE, 0, D3DSAMP_ADDRESSV, 2)), // MIRROR
        command(OP.SET_SAMPLER_STATE, u32(DEVICE, 0, D3DSAMP_MAGFILTER, 2)), // LINEAR
        command(OP.SET_SAMPLER_STATE, u32(DEVICE, 0, D3DSAMP_MINFILTER, 2)),
        command(OP.SET_SAMPLER_STATE, u32(DEVICE, 0, D3DSAMP_MIPFILTER, 2)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    const samplers = find("createSampler");
    assert.equal(samplers.length, 1, "expected exactly one sampler to be created");
    assert.deepEqual({
        u: samplers[0][1].addressModeU, v: samplers[0][1].addressModeV,
        mag: samplers[0][1].magFilter, min: samplers[0][1].minFilter,
        mip: samplers[0][1].mipmapFilter,
    }, { u: "clamp-to-edge", v: "mirror-repeat", mag: "linear",
        min: "linear", mip: "linear" });
    assert.equal(executor.stats.samplersCreated, 1);
});

await test("a second draw with the same sampler state reuses the cached sampler", async () => {
    const { executor } = makeExecutor();
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(0, 12, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD),
    ];
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x401, 4, 4, 1, 21, 0, 1)),
        command(OP.SET_FVF, fvfPayload(0x102, elements)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x401, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.samplersCreated, 1);
    assert.equal(executor.stats.samplerHits, 1);
});

await test("fixed-function BORDER sampler state reaches the generated shader",
        async () => {
    const { executor, find } = makeExecutor();
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(0, 12, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD),
    ];
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x401, 4, 4, 1, 21, 0, 1)),
        command(OP.SET_FVF, fvfPayload(0x102, elements)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x401, 0)),
        command(OP.SET_SAMPLER_STATE, u32(DEVICE, 0, 1, 4)),
        command(OP.SET_SAMPLER_STATE, u32(DEVICE, 0, 2, 4)),
        command(OP.SET_SAMPLER_STATE, u32(DEVICE, 0, 4, 0x80402010)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    const wgsl = find("createRenderPipeline").pop()[1].fragment.module.code;
    assert.ok(wgsl.includes("let tex0 = select(vec4<f32>("));
    assert.ok(wgsl.includes("0.25098039") && wgsl.includes("0.50196078"));
    assert.ok(!executor.stats.unreadStateIds ||
        !(executor.stats.unreadStateIds.samplerStates || []).includes(4),
    "BORDERCOLOR is consumed together with BORDER addressing");
});

await test("multi-stream declarations bind one vertex buffer per stream", async () => {
    const { executor, fake, find } = makeExecutor();
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(1, 0, DECLTYPE.D3DCOLOR, DECLUSAGE.COLOR),
        element(1, 4, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD),
    ];
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x202, 1, 96)),
        command(OP.CREATE_VERTEX_DECLARATION, declarationPayload(0x301, elements)),
        command(OP.SET_VERTEX_DECLARATION, u32(DEVICE, 0x301)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(1, 0x202, 12, 32)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    const pipeline = find("createRenderPipeline").pop()[1];
    assert.equal(pipeline.vertex.buffers.length, 2, "expected two vertex buffer layouts");
    assert.deepEqual(pipeline.vertex.buffers[0].attributes,
        [{ shaderLocation: 0, offset: 0, format: "float32x3" }]);
    assert.equal(pipeline.vertex.buffers[1].arrayStride, 12);
    const pass = fake.calls.filter(c => c[0] === "beginRenderPass").pop()[2];
    const binds = pass.ops.filter(op => op[0] === "vertexBuffer");
    assert.equal(binds.length, 2);
    assert.equal(binds[1][3], 32, "stream 1's OffsetInBytes was lost");
});

await test("triangle strips use strip topology instead of being reinterpreted as a list", async () => {
    const { executor, find } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(5, 0, 4)), // 4 tris
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    const pipeline = find("createRenderPipeline").pop()[1];
    assert.equal(pipeline.primitive.topology, "triangle-strip");
    assert.equal(pipeline.primitive.stripIndexFormat, undefined,
        "a non-indexed strip must not declare stripIndexFormat");
});

await test("indexed triangle strips declare the strip index format", async () => {
    const { executor, find } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x202, 2, 64, 101)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.SET_INDICES, u32(DEVICE, 0x202)),
        command(OP.DRAW_INDEXED_PRIMITIVE, drawIndexedPayload(5, 0, 0, 4)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    const pipeline = find("createRenderPipeline").pop()[1];
    assert.equal(pipeline.primitive.topology, "triangle-strip");
    assert.equal(pipeline.primitive.stripIndexFormat, "uint16");
});

await test("triangle fans become an indexed triangle list", async () => {
    const { executor, fake, find } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(6, 0, 3)), // fan, 3 tris
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    const pipeline = find("createRenderPipeline").pop()[1];
    assert.equal(pipeline.primitive.topology, "triangle-list");
    const pass = fake.calls.filter(c => c[0] === "beginRenderPass").pop()[2];
    const drawIndexed = pass.ops.filter(op => op[0] === "drawIndexed");
    assert.equal(drawIndexed.length, 1);
    assert.equal(drawIndexed[0][1], 9, "3 fan triangles == 9 list indices");
    // (0,1,2) (0,2,3) (0,3,4)
    const indexWrite = find("writeBuffer").find(
        call => call[3] instanceof Uint32Array && call[3].length === 9);
    assert.ok(indexWrite, "the generated fan index buffer was not uploaded");
    assert.deepEqual([...indexWrite[3]], [0, 1, 2, 0, 2, 3, 0, 3, 4]);
});

await test("DrawIndexedPrimitiveUP works (M1 threw a ReferenceError on every call)", async () => {
    const { executor, fake } = makeExecutor();
    const vertexBytes = 4 * 12;
    const indexBytes = 6 * 2;
    const blob = Buffer.alloc(indexBytes + vertexBytes);
    for (let i = 0; i < 6; ++i) blob.writeUInt16LE([0, 1, 2, 0, 2, 3][i], i * 2);
    const payload = Buffer.alloc(48);
    payload.writeUInt32LE(DEVICE, 0);
    payload.writeUInt32LE(4, 4);       // D3DPT_TRIANGLELIST
    payload.writeUInt32LE(0, 8);       // min_vertex_index
    payload.writeUInt32LE(4, 12);      // vertex_count
    payload.writeUInt32LE(2, 16);      // primitive_count
    payload.writeUInt32LE(101, 20);    // D3DFMT_INDEX16
    payload.writeUInt32LE(12, 24);     // stride
    payload.writeUInt32LE(6, 28);      // index_count
    payload.writeUInt32LE(indexBytes, 32);
    payload.writeUInt32LE(vertexBytes, 36);
    // index_data_offset / vertex_data_offset are patched below.
    const built = buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.DRAW_INDEXED_PRIMITIVE_UP, payload, blob, 40),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true });
    // buildBatch patched index_data_offset (field 40); the vertex data sits
    // straight after the index data in the same blob, so field 44 is patched
    // here, in the assembled batch, to point past it.
    const blobOffset = payload.readUInt32LE(40);
    const commandOffset = built.indexOf(payload, 32);
    built.writeUInt32LE(blobOffset + indexBytes, commandOffset + 44);

    await executor.submit(built);
    await executor.idle();
    assert.equal(executor.failed, null,
        "DrawIndexedPrimitiveUP must not blow up the batch: " + executor.failed);
    assert.equal(executor.stats.upDrawCalls, 1);
    assert.equal(executor.stats.droppedDraws, 0);
    const pass = fake.calls.filter(c => c[0] === "beginRenderPass").pop()[2];
    assert.equal(pass.ops.filter(op => op[0] === "drawIndexed").length, 1);
});

await test("identical bytecode is translated once and shares one shader module", async () => {
    const { executor, find } = makeExecutor();
    const first = shaderCreatePayload(0x40000011, VS_BYTECODE);
    const second = shaderCreatePayload(0x40000013, VS_BYTECODE);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_VERTEX_SHADER, first.payload, first.blob, first.blobOffsetField),
        command(OP.CREATE_VERTEX_SHADER, second.payload, second.blob, second.blobOffsetField),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.shaderCache.stats.compiles, 1);
    assert.equal(executor.shaderCache.stats.hits, 1);
    assert.equal(find("createShaderModule").length, 0,
        "modules are only created when a pipeline needs them");
});

await test("HELLO's feature bits report which guest DLL is loaded", async () => {
    const { executor } = makeExecutor();
    // guest_pointer_bits / feature_bits / session_id_low / session_id_high.
    await executor.submit(buildBatch([
        command(OP.HELLO, u32(32, 1 /* D9WG_FEATURE_SHADER_MODEL_2 */, 0, 0)),
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.guestShaderModel2, true);

    const stale = makeExecutor();
    await stale.executor.submit(buildBatch([
        command(OP.HELLO, u32(32, 0, 0, 0)),
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await stale.executor.idle();
    assert.equal(stale.executor.stats.guestShaderModel2, false,
        "a pre-M2 guest must be distinguishable from one that simply drew " +
        "no shaders");
});

await test("an empty client rect on Present keeps the last known surface size", async () => {
    const { executor } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CLEAR, u32(DEVICE, 1, 0xff102030, 0x3f800000, 0, 0)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    const surfaceChangesAfterFirst = executor.stats.surfaceChanges;
    // Fullscreen War3 reports 0x0 here; letting that through would resize the
    // overlay canvas every other frame.
    await executor.submit(buildBatch([
        command(OP.CLEAR, u32(DEVICE, 1, 0xff102030, 0x3f800000, 0, 0)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 0, 0)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.emptySurfaceReports, 1);
    assert.equal(executor.stats.surfaceChanges, surfaceChangesAfterFirst,
        "an empty rect must not count as a surface change");
    const state = executor.devices.get(DEVICE);
    assert.equal(state.surface.width, 640);
    assert.equal(state.surface.height, 480);
});

await test("frames that never clear the colour target are counted", async () => {
    const { executor } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        // A draw with no preceding Clear: WebGPU does not preserve the
        // canvas across Present, so this composites over an undefined buffer.
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.framesWithoutColorClear, 1);
    assert.equal(executor.stats.framesWithNoOps, 0);
});

await test("a dynamic buffer rewritten between draws does not corrupt the earlier draw", async () => {
    // The exact idiom that made War3's scene geometry explode: one shared
    // dynamic vertex buffer, refilled and drawn twice inside a single frame.
    // Draws are recorded and replayed at Present, while writeBuffer takes
    // effect in queue order -- so without renaming, both draws would read the
    // second batch of vertices.
    const { executor, fake, find } = makeExecutor();
    const batchA = Buffer.alloc(36, 0x11);
    const batchB = Buffer.alloc(36, 0x22);
    const updatePayload = (handle, byteCount) => {
        const payload = Buffer.alloc(24);
        payload.writeUInt32LE(handle, 0);
        payload.writeUInt32LE(0, 4);
        payload.writeUInt32LE(byteCount, 8);
        return payload;
    };
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 36)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.UPDATE_BUFFER, updatePayload(0x201, 36), batchA, 12),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.UPDATE_BUFFER, updatePayload(0x201, 36), batchB, 12),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();

    assert.equal(executor.stats.drawCalls, 2);
    assert.equal(executor.stats.bufferRenames, 1,
        "the second write must rename, not overwrite what draw 1 reads");

    // The two draws must end up bound to two different GPUBuffers.
    const pass = fake.calls.filter(c => c[0] === "beginRenderPass").pop()[2];
    const bound = pass.ops.filter(op => op[0] === "vertexBuffer").map(op => op[2]);
    assert.equal(bound.length, 2);
    assert.notEqual(bound[0], bound[1],
        "both draws are reading the same buffer, so the first one renders " +
        "the second one's vertices");

    // And each buffer must hold the batch its draw was issued with.
    const contentsOf = buffer => {
        const write = find("writeBuffer").filter(call => call[1] === buffer).pop();
        assert.ok(write, "no upload for a bound vertex buffer");
        return write[6][0]; // the snapshot taken at writeBuffer time
    };
    assert.equal(contentsOf(bound[0]), 0x11, "draw 1 lost its vertex data");
    assert.equal(contentsOf(bound[1]), 0x22, "draw 2 got the wrong vertex data");
});

await test("a buffer rewritten with no draw in between is updated in place", async () => {
    // Renaming must stay off the ordinary path: upload once, draw many.
    const { executor } = makeExecutor();
    const payload = Buffer.alloc(24);
    payload.writeUInt32LE(0x201, 0);
    payload.writeUInt32LE(0, 4);
    payload.writeUInt32LE(36, 8);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 36)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.UPDATE_BUFFER, payload, Buffer.alloc(36, 0x11), 12),
        command(OP.UPDATE_BUFFER, payload, Buffer.alloc(36, 0x22), 12),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.bufferRenames, 0,
        "no draw observed the first contents, so nothing needed renaming");
});

await test("a buffer rewritten in a later frame is updated in place", async () => {
    const { executor } = makeExecutor();
    const payload = Buffer.alloc(24);
    payload.writeUInt32LE(0x201, 0);
    payload.writeUInt32LE(0, 4);
    payload.writeUInt32LE(36, 8);
    const frame = data => buildBatch([
        command(OP.UPDATE_BUFFER, payload, data, 12),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true });
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 36)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
    ]));
    await executor.submit(frame(Buffer.alloc(36, 0x11)));
    await executor.submit(frame(Buffer.alloc(36, 0x22)));
    await executor.idle();
    assert.equal(executor.stats.drawCalls, 2);
    assert.equal(executor.stats.bufferRenames, 0,
        "the previous frame was already submitted; its draws cannot be " +
        "affected by this frame's writes");
});

await test("alpha test becomes a discard in both fixed-function and translated shaders", async () => {
    const D3DRS_ALPHATESTENABLE = 15, D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25;
    const D3DCMP_GREATEREQUAL = 7;
    const { executor, find } = makeExecutor();
    const ps = shaderCreatePayload(0x40000021, PS_BYTECODE);
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(0, 12, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD),
    ];
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x401, 4, 4, 1, 21, 0, 1)),
        command(OP.SET_FVF, fvfPayload(0x102, elements)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x401, 0)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_ALPHATESTENABLE, 1, 0)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL, 0)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_ALPHAREF, 128, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        // Same draw with a translated pixel shader bound.
        command(OP.CREATE_PIXEL_SHADER, ps.payload, ps.blob, ps.blobOffsetField),
        command(OP.SET_PIXEL_SHADER, u32(DEVICE, 0x40000021)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);

    const pipelines = find("createRenderPipeline").map(call => call[1]);
    assert.equal(pipelines.length, 2);
    for (const pipeline of pipelines) {
        const code = pipeline.fragment.module.code;
        assert.ok(code.includes("discard;"),
            "alpha test did not emit a discard:\n" + code);
        // GREATEREQUAL passes when a >= ref, so the discard is its negation.
        assert.ok(code.includes("0.501961"),
            "alpha reference 128 should normalise to ~0.501961:\n" + code);
    }
});

await test("turning alpha test off again returns to the untested shader", async () => {
    const D3DRS_ALPHATESTENABLE = 15;
    const { executor, find } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_ALPHATESTENABLE, 1, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_ALPHATESTENABLE, 0, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    const pipelines = find("createRenderPipeline").map(call => call[1]);
    assert.equal(pipelines.length, 2, "the two states must not share a pipeline");
    // D3DCMP_ALWAYS is the default, so enabling alpha test without setting a
    // function is still a no-op -- the first draw must not gain a discard.
    assert.ok(!pipelines[0].fragment.module.code.includes("discard;"),
        "ALPHAFUNC defaults to ALWAYS, which tests nothing");
    assert.ok(!pipelines[1].fragment.module.code.includes("discard;"));
});

await test("the D3D9 hardware cursor is uploaded and composited over the frame", async () => {
    const { executor, fake, find } = makeExecutor();
    const size = 8;
    const bitmap = Buffer.alloc(size * size * 4, 0x80);
    const cursorProps = Buffer.alloc(32);
    cursorProps.writeUInt32LE(DEVICE, 0);
    cursorProps.writeUInt32LE(2, 4);   // hotspot x
    cursorProps.writeUInt32LE(3, 8);   // hotspot y
    cursorProps.writeUInt32LE(size, 12);
    cursorProps.writeUInt32LE(size, 16);
    cursorProps.writeUInt32LE(bitmap.length, 20);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CLEAR, u32(DEVICE, 1, 0xff102030, 0x3f800000, 0, 0)),
        command(0x21A, cursorProps, bitmap, 24),
        command(0x21B, u32(DEVICE, 100, 50, 0)),   // SET_CURSOR_POSITION
        command(0x21C, u32(DEVICE, 1)),            // SHOW_CURSOR
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();

    assert.equal(executor.stats.cursorUploads, 1);
    assert.equal(executor.stats.cursorDraws, 1);
    // The cursor gets its own final pass, loading the frame underneath.
    const passes = fake.calls.filter(c => c[0] === "beginRenderPass");
    const cursorPass = passes[passes.length - 1];
    assert.equal(cursorPass[1].colorAttachments[0].loadOp, "load",
        "the cursor pass must not clear the frame it sits on");
    assert.equal(cursorPass[1].depthStencilAttachment, undefined,
        "the cursor must not be depth-tested against the game's scene");
    assert.deepEqual(cursorPass[2].ops.filter(op => op[0] === "draw"),
        [["draw", 6]]);

    // Position is placed by the hotspot, in normalised back-buffer space.
    const rectWrite = find("writeBuffer")
        .filter(call => call[6] && call[6].byteLength === 16).pop();
    const rect = new Float32Array(rectWrite[6].buffer, rectWrite[6].byteOffset, 4);
    assert.ok(Math.abs(rect[0] - (100 - 2) / 640) < 1e-6, "cursor origin x");
    assert.ok(Math.abs(rect[1] - (50 - 3) / 480) < 1e-6, "cursor origin y");
    assert.ok(Math.abs(rect[2] - size / 640) < 1e-6, "cursor width");
});

await test("a hidden cursor is not composited", async () => {
    const { executor } = makeExecutor();
    const size = 4;
    const bitmap = Buffer.alloc(size * size * 4, 0xff);
    const cursorProps = Buffer.alloc(32);
    cursorProps.writeUInt32LE(DEVICE, 0);
    cursorProps.writeUInt32LE(size, 12);
    cursorProps.writeUInt32LE(size, 16);
    cursorProps.writeUInt32LE(bitmap.length, 20);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CLEAR, u32(DEVICE, 1, 0xff102030, 0x3f800000, 0, 0)),
        command(0x21A, cursorProps, bitmap, 24),
        command(0x21C, u32(DEVICE, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.submit(buildBatch([
        command(OP.CLEAR, u32(DEVICE, 1, 0xff102030, 0x3f800000, 0, 0)),
        command(0x21C, u32(DEVICE, 0)),  // ShowCursor(FALSE)
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.cursorUploads, 1);
    assert.equal(executor.stats.cursorDraws, 1, "only the visible frame draws it");
});

await test("the lock flags decide whether a mid-frame write has to rename", async () => {
    const D3DLOCK_NOOVERWRITE = 0x1000, D3DLOCK_DISCARD = 0x2000;
    const run = async lockFlags => {
        const { executor } = makeExecutor();
        const payload = Buffer.alloc(24);
        payload.writeUInt32LE(0x201, 0);
        payload.writeUInt32LE(0, 4);
        payload.writeUInt32LE(36, 8);
        payload.writeUInt32LE(lockFlags, 16);
        await executor.submit(buildBatch([
            command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
            command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 36)),
            command(OP.SET_FVF, fvfPayload(0x2,
                [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
            command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
            command(OP.UPDATE_BUFFER, payload, Buffer.alloc(36, 0x11), 12),
            command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
            command(OP.UPDATE_BUFFER, payload, Buffer.alloc(36, 0x22), 12),
            command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
            command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
        ], { present: true }));
        await executor.idle();
        return executor.stats;
    };

    // NOOVERWRITE is the application promising it is not touching bytes an
    // issued draw reads -- exactly the guarantee the hazard needs. Renaming
    // there is pure waste, and it is the idiom that made War3 rename ~277
    // times a frame.
    const noOverwrite = await run(D3DLOCK_NOOVERWRITE);
    assert.equal(noOverwrite.bufferRenames, 0);
    assert.equal(noOverwrite.bufferNoOverwriteWrites, 1);

    // DISCARD renames, but the replacement only carries the bytes being
    // written now: the rest is contents the application has abandoned.
    const discard = await run(D3DLOCK_DISCARD);
    assert.equal(discard.bufferRenames, 1);
    assert.equal(discard.bufferFullCopyRenames, 0);

    // A plain lock keeps the old contents readable, so the whole shadow has
    // to be copied forward. Correct, and the only case that costs that.
    const plain = await run(0);
    assert.equal(plain.bufferRenames, 1);
    assert.equal(plain.bufferFullCopyRenames, 1);
});

await test("window state reports a game whose window cannot receive input", async () => {
    const { executor } = makeExecutor();
    const windowState = (flags) => {
        const payload = Buffer.alloc(40);
        payload.writeUInt32LE(DEVICE, 0);
        payload.writeUInt32LE(0xa0180, 4);   // hwnd
        payload.writeUInt32LE(0xb1234, 8);   // foreground hwnd (someone else)
        payload.writeUInt32LE(flags, 12);
        payload.writeUInt32LE(800, 24);
        payload.writeUInt32LE(600, 28);
        return payload;
    };
    // IS_WINDOW | VISIBLE | FULLSCREEN, but not FOREGROUND.
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(0x21D, windowState(1 | 2 | 16)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    const stats = executor.getStats();
    assert.equal(stats.windowStateChanges, 1);
    assert.equal(stats.window.isWindow, true);
    assert.equal(stats.window.fullscreen, true);
    assert.equal(stats.window.foreground, false,
        "a game that is not the foreground window is exactly the case this " +
        "report exists to make visible");
    assert.equal(stats.window.hwnd, 0xa0180);
    assert.notEqual(stats.window.foregroundHwnd, stats.window.hwnd);
});

await test("the stage-0 texture matrix transforms fixed-function texcoords", async () => {
    const D3DTSS_TEXTURETRANSFORMFLAGS = 24, D3DTTFF_COUNT2 = 2;
    const D3DTS_TEXTURE0 = 16;
    const { executor, find } = makeExecutor();
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(0, 12, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD),
    ];
    // A scrolling matrix: D3D9 games put the offset in row 3 (_31/_32) for
    // COUNT2, because the coordinate enters as the row vector (u, v, 1, 1).
    const scroll = [1, 0, 0, 0, 0, 1, 0, 0, 0.25, 0.5, 1, 0, 0, 0, 0, 1];
    const transform = Buffer.alloc(72);
    transform.writeUInt32LE(DEVICE, 0);
    transform.writeUInt32LE(D3DTS_TEXTURE0, 4);
    scroll.forEach((value, index) => transform.writeFloatLE(value, 8 + index * 4));
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x401, 4, 4, 1, 21, 0, 1)),
        command(OP.SET_FVF, fvfPayload(0x102, elements)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x401, 0)),
        command(OP.SET_TRANSFORM, transform),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(0x202, u32(DEVICE, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);

    const pipelines = find("createRenderPipeline").map(call => call[1]);
    assert.equal(pipelines.length, 2,
        "enabling the transform must not reuse the untransformed pipeline");
    assert.ok(!pipelines[0].vertex.module.code.includes("texture_transform0 *"),
        "the first draw has D3DTTFF_DISABLE and must pass texcoords through");
    assert.ok(pipelines[1].vertex.module.code.includes("texture_transform0 *"),
        "the second draw must apply the matrix:\n" + pipelines[1].vertex.module.code);
    // Entering as (u, v, 1, 1) is what puts the game's offset in row 3.
    assert.ok(pipelines[1].vertex.module.code.includes(".xy, 1.0, 1.0)"),
        "the coordinate must enter the matrix as (u, v, 1, 1)");

    // And the matrix has to actually reach the uniform, after the WVP,
    // viewport and padding.
    const bindGroup = find("createBindGroup").pop()[1];
    const write = find("writeBuffer")
        .filter(call => call[1] === bindGroup.entries[0].resource.buffer).pop();
    const data = new Float32Array(write[6].buffer, write[6].byteOffset, 36);
    assert.deepEqual([...data.slice(20, 36)], scroll);
});

await test("fixed-function fog tints the fragment towards D3DRS_FOGCOLOR", async () => {
    const D3DRS_FOGENABLE = 28, D3DRS_FOGCOLOR = 34, D3DRS_FOGTABLEMODE = 35;
    const D3DRS_FOGSTART = 36, D3DRS_FOGEND = 37;
    const D3DFOG_LINEAR = 3;
    const floatBitsOf = value => {
        const buffer = new ArrayBuffer(4);
        new Float32Array(buffer)[0] = value;
        return new Uint32Array(buffer)[0];
    };
    const { executor, find } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_FOGENABLE, 1, 0)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_FOGTABLEMODE, D3DFOG_LINEAR, 0)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_FOGCOLOR, 0x00405060, 0)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_FOGSTART, floatBitsOf(10), 0)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_FOGEND, floatBitsOf(200), 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);

    const pipelines = find("createRenderPipeline").map(call => call[1]);
    assert.equal(pipelines.length, 2, "fog must not reuse the unfogged pipeline");
    assert.ok(!pipelines[0].fragment.module.code.includes("mix(uniforms.fog_color"),
        "the pre-fog draw must not blend");
    assert.ok(pipelines[1].vertex.module.code.includes("fog_distance"),
        "the fog factor is computed in the vertex stage:\n" +
        pipelines[1].vertex.module.code);
    assert.ok(pipelines[1].fragment.module.code.includes("mix(uniforms.fog_color"),
        "the pixel stage must blend towards the fog colour");

    // The fixed-function pixel stage has no register file, so binding 1 exists
    // only to carry the fog colour -- and it has to be declared and supplied.
    const layout = find("createBindGroupLayout").pop()[1];
    assert.ok(layout.entries.some(entry => entry.binding === 1),
        "the fog colour needs its own pixel-stage uniform binding");

    const bindGroup = find("createBindGroup").pop()[1];
    const pixelEntry = bindGroup.entries.find(entry => entry.binding === 1);
    const write = find("writeBuffer")
        .filter(call => call[1] === pixelEntry.resource.buffer).pop();
    const data = new Float32Array(write[6].buffer, write[6].byteOffset);
    const fog = data.subarray(pixelEntry.resource.offset / 4,
        pixelEntry.resource.offset / 4 + 3);
    assert.deepEqual([...fog].map(v => Math.round(v * 255)), [0x40, 0x50, 0x60],
        "D3DRS_FOGCOLOR is 0x00RRGGBB and must reach the shader as RGB");

    // FOGSTART/FOGEND are float bits inside a DWORD, not integers.
    const vertexData = new Float32Array(
        find("writeBuffer").filter(c => c[1] === bindGroup.entries[0].resource.buffer)
            .pop()[6].buffer);
    // The vertex block's shape follows the signature (M3), so with no lighting
    // and no texture transform fog_params sits right after the WVP and viewport.
    assert.equal(vertexData[20], 10, "FOGSTART decoded as float bits");
    assert.equal(vertexData[21], 200, "FOGEND decoded as float bits");
});

await test("a malformed batch is rejected rather than half-executed", async () => {
    const { executor } = makeExecutor();
    const batch = buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
    ]);
    batch.writeUInt32LE(0xffffffff, 20); // command_bytes past the record
    await executor.submit(batch);
    await executor.idle();
    assert.ok(executor.failed, "an overrunning command_bytes must fail the batch");
    assert.equal(executor.stats.malformedBatches, 1);
});

await test("shader bytecode that overruns the batch is rejected", async () => {
    const { executor } = makeExecutor();
    const vs = shaderCreatePayload(0x40000015, VS_BYTECODE);
    const batch = buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_VERTEX_SHADER, vs.payload, vs.blob, vs.blobOffsetField),
    ]);
    // Claim far more tokens than the batch can hold.
    const commandOffset = batch.indexOf(vs.payload, 32);
    batch.writeUInt32LE(0x10000, commandOffset + 8);
    await executor.submit(batch);
    await executor.idle();
    assert.ok(executor.failed, "an overrunning token count must fail the batch");
});

// ---- M3: fixed-function lighting and the texture-blending cascade ----

const D3DRS = {
    LIGHTING: 137, AMBIENT: 139, SPECULARENABLE: 29, COLORVERTEX: 141,
    NORMALIZENORMALS: 143, TEXTUREFACTOR: 60, SCISSORTESTENABLE: 174,
    DIFFUSEMATERIALSOURCE: 145,
};
const D3DTSS = {
    COLOROP: 1, COLORARG1: 2, COLORARG2: 3, ALPHAOP: 4, ALPHAARG1: 5,
    ALPHAARG2: 6, TEXCOORDINDEX: 11, TEXTURETRANSFORMFLAGS: 24,
    COLORARG0: 26, RESULTARG: 28, CONSTANT: 32,
};
const D3DTOP = {
    DISABLE: 1, SELECTARG1: 2, SELECTARG2: 3, MODULATE: 4, ADD: 7,
    ADDSIGNED: 8, BLENDTEXTUREALPHA: 13, DOTPRODUCT3: 24, MULTIPLYADD: 25,
    LERP: 26,
};
const D3DTA = { DIFFUSE: 0, CURRENT: 1, TEXTURE: 2, TFACTOR: 3, SPECULAR: 4,
    TEMP: 5, CONSTANT: 6, COMPLEMENT: 0x10, ALPHAREPLICATE: 0x20 };

function materialPayload(diffuse, ambient, specular, emissive, power) {
    const payload = Buffer.alloc(72);
    payload.writeUInt32LE(DEVICE, 0);
    [...diffuse, ...ambient, ...specular, ...emissive].forEach((value, index) =>
        payload.writeFloatLE(value, 4 + index * 4));
    payload.writeFloatLE(power, 68);
    return payload;
}

function lightPayload(index, type, options = {}) {
    const payload = Buffer.alloc(112);
    payload.writeUInt32LE(DEVICE, 0);
    payload.writeUInt32LE(index, 4);
    payload.writeUInt32LE(type, 8);
    const diffuse = options.diffuse || [1, 1, 1, 1];
    const specular = options.specular || [1, 1, 1, 1];
    const ambient = options.ambient || [0, 0, 0, 0];
    [...diffuse, ...specular, ...ambient].forEach((value, i) =>
        payload.writeFloatLE(value, 12 + i * 4));
    (options.position || [0, 0, 0]).forEach((value, i) =>
        payload.writeFloatLE(value, 60 + i * 4));
    (options.direction || [0, 0, 1]).forEach((value, i) =>
        payload.writeFloatLE(value, 72 + i * 4));
    payload.writeFloatLE(options.range === undefined ? 1000 : options.range, 84);
    payload.writeFloatLE(options.falloff === undefined ? 1 : options.falloff, 88);
    (options.attenuation || [1, 0, 0]).forEach((value, i) =>
        payload.writeFloatLE(value, 92 + i * 4));
    payload.writeFloatLE(options.theta === undefined ? 0.5 : options.theta, 104);
    payload.writeFloatLE(options.phi === undefined ? 1.0 : options.phi, 108);
    return payload;
}

function transformPayload(state, matrix) {
    const payload = Buffer.alloc(72);
    payload.writeUInt32LE(DEVICE, 0);
    payload.writeUInt32LE(state, 4);
    matrix.forEach((value, index) => payload.writeFloatLE(value, 8 + index * 4));
    return payload;
}

await test("fixed-function lighting reaches the shader and the uniform block",
        async () => {
    const { executor, find } = makeExecutor();
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(0, 12, DECLTYPE.FLOAT3, DECLUSAGE.NORMAL),
    ];
    // A view matrix with a translation, so "the light was transformed into view
    // space" is distinguishable from "the light was passed through untouched" --
    // an identity view would make the two indistinguishable, which is exactly
    // the mistake the M1 WVP-order bug was.
    const view = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1];
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 240)),
        command(OP.SET_FVF, fvfPayload(0x102, elements)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 24)),
        command(OP.SET_TRANSFORM, transformPayload(2, view)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS.LIGHTING, 1, 0)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS.AMBIENT, 0x00204060, 0)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS.NORMALIZENORMALS, 1, 0)),
        command(OP.SET_MATERIAL, materialPayload(
            [0.5, 0.25, 0.125, 0.75], [1, 1, 1, 1], [0, 0, 0, 0], [0, 0, 0, 0], 16)),
        command(OP.SET_LIGHT, lightPayload(0, 1 /* POINT */,
            { position: [1, 2, 3], attenuation: [1, 0, 0] })),
        command(OP.LIGHT_ENABLE, u32(DEVICE, 0, 1, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    assert.equal(executor.stats.drawsWithUnappliedLighting, 0,
        "a declaration with a NORMAL must actually be lit");

    const pipeline = find("createRenderPipeline").pop()[1];
    const wgsl = pipeline.vertex.module.code;
    assert.ok(wgsl.includes("struct D9Light"),
        "the light array has to be declared:\n" + wgsl);
    assert.ok(wgsl.includes("normal_matrix"),
        "normals need the inverse-transpose matrix");
    assert.ok(wgsl.includes("uniforms.lights[0]"), "light 0 must be read");
    assert.ok(wgsl.includes("light.range_falloff.x"),
        "a point light must honour its range");
    // The normal has to reach the pipeline as its own attribute.
    const byLocation = new Map(pipeline.vertex.buffers[0].attributes
        .map(a => [a.shaderLocation, a]));
    assert.equal(byLocation.get(3).offset, 12, "NORMAL belongs at location 3");

    // And the light's position must arrive already multiplied by the view
    // matrix. (1,2,3) * view = (11,22,33).
    const bindGroup = find("createBindGroup").pop()[1];
    const write = find("writeBuffer")
        .filter(call => call[1] === bindGroup.entries[0].resource.buffer).pop();
    const data = new Float32Array(write[6].buffer, write[6].byteOffset);
    // world_view_projection(16) viewport(4) world_view(16) normal_matrix(16)
    // material diffuse/ambient/specular/emissive(16) ambient_power(4) lights...
    const materialDiffuse = 16 + 4 + 16 + 16;
    assert.deepEqual([...data.slice(materialDiffuse, materialDiffuse + 4)],
        [0.5, 0.25, 0.125, 0.75], "material diffuse must reach the block");
    const ambientPower = materialDiffuse + 16;
    assert.deepEqual([...data.slice(ambientPower, ambientPower + 4)]
        .map(v => Math.round(v * 255) / 255),
        [0x20 / 255, 0x40 / 255, 0x60 / 255, 16 / 255].map(v =>
            Math.round(v * 255) / 255).slice(0, 3).concat([
                Math.round(16 * 255) / 255]),
        "D3DRS_AMBIENT is 0x00RRGGBB and the material power shares the vec4");
    const lightBase = ambientPower + 4;
    const position = [...data.slice(lightBase + 12, lightBase + 15)];
    assert.deepEqual(position, [11, 22, 33],
        "the light position must be transformed into view space");
});

await test("D3DRS_LIGHTING with no NORMAL passes the vertex colour through",
        async () => {
    // D3DRS_LIGHTING defaults to TRUE, so most draws with a plain pre-coloured
    // vertex format arrive with lighting nominally on. Running the maths on a
    // zero normal would leave only ambient+emissive, and with D3DRS_AMBIENT
    // defaulting to 0 that renders every such draw black.
    const { executor, find } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x42, [
            element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
            element(0, 12, DECLTYPE.D3DCOLOR, DECLUSAGE.COLOR)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 16)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS.LIGHTING, 1, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    assert.equal(executor.stats.drawsWithUnappliedLighting, 1,
        "skipping lighting for a normal-less draw has to be counted");
    const wgsl = find("createRenderPipeline").pop()[1].vertex.module.code;
    assert.ok(!wgsl.includes("struct D9Light"),
        "no lighting maths should be generated:\n" + wgsl);
    assert.ok(wgsl.includes("let out_diffuse = vertex_diffuse;"),
        "the vertex colour must reach the rasteriser unchanged");
});

await test("a multi-stage texture cascade generates one blend per stage",
        async () => {
    // Terrain splatting in miniature: stage 0 selects its texture, stage 1
    // blends a second texture over it by the first's alpha, stage 2 modulates
    // the result with the diffuse colour.
    const { executor, find } = makeExecutor();
    const tss = (stage, state, value) =>
        command(0x202, u32(DEVICE, stage, state, value));
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 240)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x401, 4, 4, 1, 21, 0, 1)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x402, 4, 4, 1, 21, 0, 1)),
        command(OP.SET_FVF, fvfPayload(0x144, [
            element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
            element(0, 12, DECLTYPE.D3DCOLOR, DECLUSAGE.COLOR),
            element(0, 16, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD, 0),
            element(0, 24, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD, 1)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 32)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x401, 0)),
        command(OP.SET_TEXTURE, u32(DEVICE, 1, 0x402, 0)),
        tss(0, D3DTSS.COLOROP, D3DTOP.SELECTARG1),
        tss(0, D3DTSS.COLORARG1, D3DTA.TEXTURE),
        tss(1, D3DTSS.COLOROP, D3DTOP.BLENDTEXTUREALPHA),
        tss(1, D3DTSS.COLORARG1, D3DTA.TEXTURE),
        tss(1, D3DTSS.COLORARG2, D3DTA.CURRENT),
        tss(1, D3DTSS.TEXCOORDINDEX, 1),
        tss(2, D3DTSS.COLOROP, D3DTOP.MODULATE),
        tss(2, D3DTSS.COLORARG1, D3DTA.CURRENT),
        tss(2, D3DTSS.COLORARG2, D3DTA.DIFFUSE),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    assert.equal(executor.stats.drawsWithUnsupportedTextureOp, 0,
        "every operation used here is inside TextureOpCaps");

    const pipeline = find("createRenderPipeline").pop()[1];
    const wgsl = pipeline.fragment.module.code;
    assert.ok(wgsl.includes("d9_tex0") && wgsl.includes("d9_tex1"),
        "both sampled stages need their own texture binding:\n" + wgsl);
    assert.ok(!wgsl.includes("d9_tex2"),
        "stage 2 samples nothing and must not declare a texture");
    assert.ok(wgsl.includes("mix("),
        "BLENDTEXTUREALPHA is a mix by the stage's texture alpha");
    // Stage 2 reads the running result, so `current` has to thread through.
    assert.ok(/current = vec4<f32>\(stage_rgb, stage_a\);/.test(wgsl),
        "each stage must write the cascade register");

    // Two textures bound means two texture/sampler binding pairs.
    const layout = find("createBindGroupLayout").pop()[1];
    for (const binding of [2, 3, 4, 5])
        assert.ok(layout.entries.some(entry => entry.binding === binding),
            "binding " + binding + " must be declared for two sampled stages");
    assert.ok(!layout.entries.some(entry => entry.binding === 6),
        "stage 2 samples nothing, so no third pair");
});

await test("D3DTSS_RESULTARG threads a stage result through the temp register",
        async () => {
    const { executor, find } = makeExecutor();
    const tss = (stage, state, value) =>
        command(0x202, u32(DEVICE, stage, state, value));
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 240)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x401, 4, 4, 1, 21, 0, 1)),
        command(OP.SET_FVF, fvfPayload(0x104, [
            element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
            element(0, 12, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD, 0)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x401, 0)),
        tss(0, D3DTSS.COLOROP, D3DTOP.SELECTARG1),
        tss(0, D3DTSS.COLORARG1, D3DTA.TEXTURE),
        tss(0, D3DTSS.RESULTARG, D3DTA.TEMP),
        tss(1, D3DTSS.COLOROP, D3DTOP.MULTIPLYADD),
        tss(1, D3DTSS.COLORARG0, D3DTA.TEMP),
        tss(1, D3DTSS.COLORARG1, D3DTA.DIFFUSE),
        tss(1, D3DTSS.COLORARG2, D3DTA.TFACTOR),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS.TEXTUREFACTOR, 0x80402010, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    const wgsl = find("createRenderPipeline").pop()[1].fragment.module.code;
    assert.ok(/temp = vec4<f32>\(stage_rgb, stage_a\);/.test(wgsl),
        "stage 0 must write the temp register:\n" + wgsl);
    assert.ok(wgsl.includes("temp.rgb"), "stage 1 must read it back");
    assert.ok(wgsl.includes("uniforms.texture_factor"),
        "D3DTA_TFACTOR needs the texture factor uniform");

    // The texture factor has to actually be uploaded, as 0xAARRGGBB.
    const bindGroup = find("createBindGroup").pop()[1];
    const pixelEntry = bindGroup.entries.find(entry => entry.binding === 1);
    const write = find("writeBuffer")
        .filter(call => call[1] === pixelEntry.resource.buffer).pop();
    const data = new Float32Array(write[6].buffer, write[6].byteOffset);
    const factor = [...data.slice(pixelEntry.resource.offset / 4,
        pixelEntry.resource.offset / 4 + 4)].map(v => Math.round(v * 255));
    assert.deepEqual(factor, [0x40, 0x20, 0x10, 0x80],
        "D3DRS_TEXTUREFACTOR is 0xAARRGGBB");
});

await test("a texture stage op outside TextureOpCaps is counted, not invented",
        async () => {
    const { executor } = makeExecutor();
    const D3DTOP_BUMPENVMAP = 22;
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 240)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x401, 4, 4, 1, 21, 0, 1)),
        command(OP.SET_FVF, fvfPayload(0x104, [
            element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
            element(0, 12, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD, 0)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x401, 0)),
        command(0x202, u32(DEVICE, 0, D3DTSS.COLOROP, D3DTOP_BUMPENVMAP)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.drawsWithUnsupportedTextureOp, 1,
        "D3DTOP_BUMPENVMAP is not in TextureOpCaps and must be reported");
    assert.equal(executor.stats.droppedDraws, 0,
        "the draw still renders, with the stage falling back to its arg1");
});

await test("a render target redirects the pass and keys its own pipeline",
        async () => {
    const D3DUSAGE_RENDERTARGET = 1;
    const { executor, find } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        // A render target arrives as a CREATE_TEXTURE_2D carrying the usage.
        command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, 0x501, 256, 256, 1, 21, D3DUSAGE_RENDERTARGET, 0)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        // Into the texture...
        command(0x20F, u32(DEVICE, 0, 0x501, 0)),
        command(0x210, u32(DEVICE, 0, 0, 0)), // no depth surface with it
        command(OP.SET_VIEWPORT, u32(DEVICE, 0, 0, 256, 256, 0, 0x3f800000, 0)),
        command(OP.CLEAR, u32(DEVICE, 1, 0xff112233, 0, 0, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        // ...and back to the back buffer, restoring the implicit depth surface
        // (D9WG_AUTO_DEPTH_STENCIL_HANDLE) the way an app that saved it does.
        command(0x20F, u32(DEVICE, 0, 0, 0)),
        command(0x210, u32(DEVICE, 0xffffffff, 640, 480)),
        command(OP.SET_VIEWPORT, u32(DEVICE, 0, 0, 640, 480, 0, 0x3f800000, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    assert.equal(executor.stats.renderTargetsCreated, 1);
    assert.equal(executor.stats.renderTargetBinds, 2);
    // The texture pass has no depth attachment and the back-buffer pass does,
    // so they cannot share a pipeline -- that is the whole point of baking
    // hasDepth into the key.
    const pipelines = find("createRenderPipeline").map(call => call[1]);
    assert.equal(pipelines.length, 2,
        "a target with a different depth configuration needs its own pipeline");
    assert.ok(!pipelines[0].depthStencil,
        "the render-to-texture pass was given no depth surface");
    assert.ok(pipelines[1].depthStencil,
        "the back-buffer pass still has the auto depth-stencil");
    // Two distinct targets means at least two passes, and the first must not be
    // pointed at the swap chain.
    const passes = find("beginRenderPass").map(call => call[1]);
    assert.ok(passes.length >= 2, "each target needs its own pass");
    assert.notEqual(passes[0].colorAttachments[0].view,
        passes[passes.length - 1].colorAttachments[0].view,
        "the texture pass must not render into the back buffer");
    assert.equal(executor.stats.renderPasses, passes.length);
});

await test("a cube texture binds as a cube view and uploads per face",
        async () => {
    const { executor, find } = makeExecutor();
    const face = (index, level) => {
        const payload = Buffer.alloc(48);
        payload.writeUInt32LE(0x601, 0);
        payload.writeUInt32LE(level, 4);
        payload.writeUInt32LE(0, 8);   // x
        payload.writeUInt32LE(0, 12);  // y
        payload.writeUInt32LE(index, 16); // z == cube face
        payload.writeUInt32LE(4, 20);  // width
        payload.writeUInt32LE(4, 24);  // height
        payload.writeUInt32LE(1, 28);  // depth
        payload.writeUInt32LE(16, 32); // row pitch
        payload.writeUInt32LE(0, 36);
        payload.writeUInt32LE(64, 40); // data bytes
        return { payload, blob: Buffer.alloc(64, index + 1), field: 44 };
    };
    const uploads = [0, 1, 2, 3, 4, 5].map(index => face(index, 0));
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 240)),
        command(0x111, u32(DEVICE, 0x601, 4, 1, 21, 0, 1, 0)),
        ...uploads.map(upload =>
            command(OP.UPDATE_TEXTURE, upload.payload, upload.blob, upload.field)),
        command(OP.SET_FVF, fvfPayload(0x104, [
            element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
            element(0, 12, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD, 0)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x601, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    assert.equal(executor.stats.cubeTexturesCreated, 1);
    assert.equal(executor.stats.textureUploads, 6, "one upload per face");
    assert.equal(executor.stats.drawsWithIncompleteMipChain, 0,
        "all six faces of the single level were uploaded");

    const created = find("createTexture").pop()[1];
    assert.equal(created.size.depthOrArrayLayers, 6,
        "a cube is six array layers");
    assert.ok(find("createView").some(call =>
        call[2] && call[2].dimension === "cube"),
        "the sampled view has to be a cube view");
    // Each face has to land on its own layer, or five of them overwrite one.
    const layers = find("writeTexture")
        .filter(call => call[1] && call[1].origin)
        .map(call => call[1].origin.z);
    assert.deepEqual(layers, [0, 1, 2, 3, 4, 5]);
    // And the fixed-function cascade has to sample it as a cube.
    const wgsl = find("createRenderPipeline").pop()[1].fragment.module.code;
    assert.ok(wgsl.includes("texture_cube<f32>"),
        "the cascade must declare a cube sampler:\n" + wgsl);
});

await test("legacy D3D9 texture formats preserve colour and signed bump values",
        async () => {
    const { executor, find } = makeExecutor();
    const halfMinusOne = [0x00, 0xbc];
    const halfZero = [0x00, 0x00];
    const halfHalf = [0x00, 0x38];
    const halfOne = [0x00, 0x3c];
    const cases = [
        { format: 20, gpu: "rgba8unorm", source: [0x33, 0x22, 0x11],
          expected: [0x11, 0x22, 0x33, 0xff] }, // R8G8B8: B,G,R
        { format: 27, gpu: "rgba8unorm", source: [0xe3],
          expected: [0xff, 0x00, 0xff, 0xff] },
        { format: 29, gpu: "rgba8unorm", source: [0xe3, 0x80],
          expected: [0xff, 0x00, 0xff, 0x80] },
        { format: 30, gpu: "rgba8unorm", source: [0x23, 0xf1],
          expected: [0x11, 0x22, 0x33, 0xff] },
        { format: 32, gpu: "rgba8unorm", source: [0x11, 0x22, 0x33, 0x44],
          expected: [0x11, 0x22, 0x33, 0x44] },
        { format: 33, gpu: "rgba8unorm", source: [0x11, 0x22, 0x33, 0x44],
          expected: [0x11, 0x22, 0x33, 0xff] },
        { format: 28, gpu: "rgba8unorm", source: [0x40],
          expected: [0x00, 0x00, 0x00, 0x40] }, // A8 missing RGB is zero
        { format: 51, gpu: "rgba8unorm", source: [0x40, 0x80],
          expected: [0x40, 0x40, 0x40, 0x80] },
        { format: 52, gpu: "rgba8unorm", source: [0xa5],
          expected: [0x55, 0x55, 0x55, 0xaa] },
        { format: 81, gpu: "rgba16float", source: [0x00, 0x80],
          expected: [...halfHalf, ...halfHalf, ...halfHalf, ...halfOne] },
        { format: 60, gpu: "rgba8snorm", source: [0x80, 0x7f],
          expected: [0x80, 0x7f, 0x7f, 0x7f] },
        { format: 63, gpu: "rgba8snorm", source: [0x80, 0xc0, 0x40, 0x7f],
          expected: [0x80, 0xc0, 0x40, 0x7f] },
        { format: 61, gpu: "rgba16float", source: [0xf0, 0xfd],
          expected: [...halfMinusOne, ...halfOne, ...halfOne, ...halfOne] },
        { format: 62, gpu: "rgba16float", source: [0x80, 0x7f, 0xff, 0],
          expected: [...halfMinusOne, ...halfOne, ...halfOne, ...halfOne] },
        { format: 64, gpu: "rgba16float", source: [0x00, 0x80, 0xff, 0x7f],
          expected: [...halfMinusOne, ...halfOne, ...halfOne, ...halfOne] },
        { format: 67, gpu: "rgba16float", source: [0x00, 0xfe, 0x07, 0xc0],
          expected: [...halfMinusOne, ...halfOne, ...halfZero, ...halfOne] },
        { format: 117, gpu: "rgba16float", source: [0x80, 0x00],
          expected: [...halfMinusOne, ...halfZero, ...halfZero, ...halfOne] },
    ];
    const commands = [command(OP.CREATE_DEVICE, createDevicePayload(640, 480))];
    cases.forEach((item, index) => {
        const handle = 0x700 + index;
        const source = Buffer.from(item.source);
        const update = Buffer.alloc(48);
        update.writeUInt32LE(handle, 0);
        update.writeUInt32LE(1, 20); // width
        update.writeUInt32LE(1, 24); // height
        update.writeUInt32LE(1, 28); // depth
        update.writeUInt32LE(source.length, 32); // source row pitch
        update.writeUInt32LE(source.length, 40);
        commands.push(command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, handle, 1, 1, 1, item.format, 0, 1)));
        commands.push(command(OP.UPDATE_TEXTURE, update, source, 44));
    });
    await executor.submit(buildBatch(commands));
    await executor.idle();
    assert.equal(executor.stats.texturesRejected, 0);
    const descriptors = find("createTexture").slice(-cases.length)
        .map(call => call[1]);
    const writes = find("writeTexture").slice(-cases.length);
    cases.forEach((item, index) => {
        assert.equal(descriptors[index].format, item.gpu,
            "wrong GPU format for D3DFMT " + item.format);
        if (item.gpu === "rgba8snorm")
            assert.equal(descriptors[index].usage & 0x10, 0,
                "SNORM textures must not request RENDER_ATTACHMENT");
        assert.deepEqual(Array.from(writes[index][2]), item.expected,
            "wrong texel conversion for D3DFMT " + item.format);
        assert.equal(writes[index][3].bytesPerRow, item.expected.length);
    });
});

await test("signed textures reject render-target use but keep direct copies",
        async () => {
    const { executor, find } = makeExecutor();
    const stretch = (destinationSize) => {
        const payload = Buffer.alloc(56);
        payload.writeUInt32LE(DEVICE, 0);
        payload.writeUInt32LE(0x771, 4);
        payload.writeUInt32LE(0, 8);
        [0, 0, 4, 4].forEach((value, index) =>
            payload.writeInt32LE(value, 12 + index * 4));
        payload.writeUInt32LE(0x772, 28);
        payload.writeUInt32LE(0, 32);
        [0, 0, destinationSize, destinationSize].forEach((value, index) =>
            payload.writeInt32LE(value, 36 + index * 4));
        return payload;
    };
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        // A stale or malformed guest must not make the host create an illegal
        // rgba8snorm RENDER_ATTACHMENT descriptor.
        command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, 0x770, 4, 4, 1, 60, 1, 0)),
        command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, 0x771, 4, 4, 1, 60, 0, 1)),
        command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, 0x772, 4, 4, 1, 60, 0, 1)),
        command(0x8, stretch(4)),
        command(0x8, stretch(2)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.texturesRejected, 1);
    assert.equal(executor.stats.blits, 1,
        "same-size same-format signed textures can use a GPU copy");
    assert.equal(executor.stats.blitsSkipped, 1,
        "scaling cannot render into an rgba8snorm texture");
    assert.equal(find("copyTextureToTexture").length, 1);
    assert.ok(!find("createTexture").some(call =>
        call[1].format === "rgba8snorm" && (call[1].usage & 0x10)),
        "rgba8snorm must never request RENDER_ATTACHMENT");
});

await test("R8G8B8 upload honours a padded source pitch", async () => {
    const { executor, find } = makeExecutor();
    const source = Buffer.from([
        0x30, 0x20, 0x10, 0xee,
        0x60, 0x50, 0x40, 0xee,
    ]);
    const update = Buffer.alloc(48);
    update.writeUInt32LE(0x750, 0);
    update.writeUInt32LE(1, 20);
    update.writeUInt32LE(2, 24);
    update.writeUInt32LE(1, 28);
    update.writeUInt32LE(4, 32);
    update.writeUInt32LE(source.length, 40);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x750, 1, 2, 1, 20, 0, 1)),
        command(OP.UPDATE_TEXTURE, update, source, 44),
    ]));
    await executor.idle();
    const write = find("writeTexture").pop();
    assert.deepEqual(Array.from(write[2]), [
        0x10, 0x20, 0x30, 0xff,
        0x40, 0x50, 0x60, 0xff,
    ]);
    assert.equal(write[3].bytesPerRow, 4);
    assert.equal(write[3].rowsPerImage, 2);
});

await test("DXT2 and DXT4 reuse BC2 and BC3 block storage", async () => {
    const { executor, find } = makeExecutor();
    const commands = [command(OP.CREATE_DEVICE, createDevicePayload(640, 480))];
    [
        { handle: 0x760, format: 0x32545844, gpu: "bc2-rgba-unorm" },
        { handle: 0x761, format: 0x34545844, gpu: "bc3-rgba-unorm" },
    ].forEach(item => {
        const update = Buffer.alloc(48);
        update.writeUInt32LE(item.handle, 0);
        update.writeUInt32LE(4, 20);
        update.writeUInt32LE(4, 24);
        update.writeUInt32LE(1, 28);
        update.writeUInt32LE(16, 32);
        update.writeUInt32LE(16, 40);
        commands.push(command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, item.handle, 4, 4, 1, item.format, 0, 1)));
        commands.push(command(OP.UPDATE_TEXTURE, update,
            Buffer.alloc(16, item.handle & 0xff), 44));
    });
    await executor.submit(buildBatch(commands));
    await executor.idle();
    assert.deepEqual(find("createTexture").slice(-2).map(call => call[1].format),
        ["bc2-rgba-unorm", "bc3-rgba-unorm"]);
    assert.deepEqual(find("writeTexture").slice(-2).map(call => call[3]), [
        { bytesPerRow: 16, rowsPerImage: 1 },
        { bytesPerRow: 16, rowsPerImage: 1 },
    ]);
});

await test("exhausting the debug preview budget never drops a game texture upload",
        async () => {
    const { executor, find } = makeExecutor();
    executor.previewBudget = 0;
    const payload = Buffer.alloc(48);
    payload.writeUInt32LE(0x611, 0);
    payload.writeUInt32LE(0, 4);
    payload.writeUInt32LE(0, 8);
    payload.writeUInt32LE(0, 12);
    payload.writeUInt32LE(0, 16);
    payload.writeUInt32LE(4, 20);
    payload.writeUInt32LE(4, 24);
    payload.writeUInt32LE(1, 28);
    payload.writeUInt32LE(16, 32);
    payload.writeUInt32LE(0, 36);
    payload.writeUInt32LE(64, 40);
    await executor.submit(buildBatch([
        command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, 0x611, 4, 4, 1, 21, 0, 1)),
        command(OP.UPDATE_TEXTURE, payload, Buffer.alloc(64, 0x7f), 44),
    ]));
    await executor.idle();
    assert.equal(executor.stats.texturePreviewsSkipped, 1,
        "the bounded diagnostic copy should be skipped");
    assert.equal(executor.stats.textureUploads, 1,
        "the actual upload must still be counted");
    assert.ok(find("writeTexture").some(call =>
        call[1].texture && call[1].texture.descriptor.size.width === 4),
        "preview exhaustion must not bypass queue.writeTexture");
});

await test("a texture updated after a recorded draw is renamed", async () => {
    const { executor, find } = makeExecutor();
    const upload = fill => {
        const payload = Buffer.alloc(48);
        payload.writeUInt32LE(0x612, 0);
        payload.writeUInt32LE(0, 4);
        payload.writeUInt32LE(0, 8);
        payload.writeUInt32LE(0, 12);
        payload.writeUInt32LE(0, 16);
        payload.writeUInt32LE(4, 20);
        payload.writeUInt32LE(4, 24);
        payload.writeUInt32LE(1, 28);
        payload.writeUInt32LE(16, 32);
        payload.writeUInt32LE(0, 36);
        payload.writeUInt32LE(64, 40);
        return command(OP.UPDATE_TEXTURE, payload, Buffer.alloc(64, fill), 44);
    };
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(0, 12, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD),
    ];
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 60)),
        command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, 0x612, 4, 4, 1, 21, 0, 1)),
        upload(0x11),
        command(OP.SET_FVF, fvfPayload(0x102, elements)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x612, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        upload(0x22),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.textureUpdateHazards, 1);
    assert.equal(executor.stats.textureRenames, 1);
    const groups = find("createBindGroup").filter(call =>
        call[1].entries.some(entry => entry.binding === 2));
    assert.equal(groups.length, 2);
    const firstView = groups[0][1].entries.find(entry => entry.binding === 2).resource;
    const secondView = groups[1][1].entries.find(entry => entry.binding === 2).resource;
    assert.notEqual(firstView.texture, secondView.texture,
        "the earlier bind group must retain the old GPU texture");
});

await test("D3DRS_SCISSORTESTENABLE gates the scissor rect", async () => {
    const { executor, find } = makeExecutor();
    const scissor = Buffer.alloc(20);
    scissor.writeUInt32LE(DEVICE, 0);
    scissor.writeInt32LE(10, 4);
    scissor.writeInt32LE(20, 8);
    scissor.writeInt32LE(110, 12);
    scissor.writeInt32LE(220, 16);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(0x205, scissor),
        // Rect set but the test disabled: D3D9 ignores it.
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS.SCISSORTESTENABLE, 1, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    assert.equal(executor.stats.drawsWithScissor, 1,
        "only the draw with the test enabled is scissored");
    // Every draw carries a clip rect, because a D3D9 viewport clips and a
    // WebGPU one does not. With the test off that rect is the full viewport;
    // with it on it is the viewport intersected with the app's rect, which D3D9
    // also applies on top of the viewport rather than instead of it.
    const calls = find("setScissorRect");
    assert.equal(calls.length, 2, "each draw sets its own clip rect");
    assert.deepEqual(calls[0].slice(1), [0, 0, 640, 480],
        "with the test disabled the clip rect is the whole viewport");
    assert.deepEqual(calls[1].slice(1), [10, 20, 100, 200]);
});

// A D3D9 viewport clips geometry; WebGPU's setViewport only maps NDC to pixels.
// Nothing else would cut a draw off at the viewport edge, so a game that
// restricts a small panel with SetViewport alone had its geometry drawn across
// the whole target instead.
await test("a viewport clips, and carries its D3D9 depth range", async () => {
    const { executor, find } = makeExecutor();
    const viewport = Buffer.alloc(32);
    viewport.writeUInt32LE(DEVICE, 0);
    viewport.writeUInt32LE(64, 4);    // x
    viewport.writeUInt32LE(48, 8);    // y
    viewport.writeUInt32LE(128, 12);  // width
    viewport.writeUInt32LE(96, 16);   // height
    viewport.writeFloatLE(0.25, 20);  // MinZ
    viewport.writeFloatLE(0.5, 24);   // MaxZ
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.SET_VIEWPORT, viewport),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    assert.deepEqual(find("setScissorRect").pop().slice(1), [64, 48, 128, 96],
        "the clip rect has to follow the viewport with no app scissor set");
    // MinZ/MaxZ have always been on the wire; they used to be dropped here.
    const pass = find("beginRenderPass").pop()[2];
    assert.deepEqual(pass.ops.find(op => op[0] === "viewport").slice(1),
        [64, 48, 128, 96, 0.25, 0.5]);
});

await test("a new guest session releases the previous process's resources",
        async () => {
    const { executor } = makeExecutor();
    const hello = session => {
        const payload = Buffer.alloc(16);
        payload.writeUInt32LE(32, 0);
        payload.writeUInt32LE(1, 4); // D9WG_FEATURE_SHADER_MODEL_2
        payload.writeUInt32LE(session, 8);
        payload.writeUInt32LE(0, 12);
        return command(OP.HELLO, payload);
    };
    await executor.submit(buildBatch([
        hello(0xabcd),
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
    ]));
    await executor.idle();
    assert.equal(executor.resources.size, 1);
    assert.equal(executor.stats.sessionChanges, 0);

    // A different process reuses the same numeric handles for different
    // objects; keeping the old entries would let one process draw with the
    // other's geometry.
    await executor.submit(buildBatch([hello(0x1234)]));
    await executor.idle();
    assert.equal(executor.stats.sessionChanges, 1);
    assert.equal(executor.resources.size, 0,
        "the departing process's resources must be released");
    assert.equal(executor.devices.size, 0);
});

await test("the device requests texture-compression-bc so DXT textures work",
        async () => {
    // Without this, every createTexture for a DXT format throws
    // ("requires the 'texture-compression-bc' feature") and takes the whole
    // batch -- and therefore the whole frame -- down with it. DXT1/3/5 is where
    // a D3D9 game of this era keeps nearly all of its art, so the symptom is a
    // blank screen, not a missing texture.
    const { executor, find, fake } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, 0x401, 16, 16, 1, 0x33545844 /* DXT3 */, 0, 1)),
    ]));
    await executor.idle();
    const request = find("requestDevice").pop();
    assert.ok(request, "requestDevice must be observed");
    assert.deepEqual(request[1] && request[1].requiredFeatures,
        ["texture-compression-bc"],
        "the adapter advertises BC, so the device has to ask for it");
    const created = find("createTexture").map(call => call[1]);
    assert.ok(created.some(descriptor => descriptor.format === "bc2-rgba-unorm"),
        "DXT3 must reach WebGPU as bc2-rgba-unorm");
    assert.equal(executor.stats.texturesRejected, 0);
    assert.equal(executor.stats.malformedBatches, 0);
    assert.ok(!executor.failed, "a DXT texture must not fail the batch");
});

await test("a texture format the device refuses costs one texture, not the frame",
        async () => {
    const { executor, fake } = makeExecutor();
    const realCreateTexture = fake.device.createTexture.bind(fake.device);
    fake.device.createTexture = descriptor => {
        if (descriptor.format === "bc1-rgba-unorm")
            throw new Error("simulated: unsupported format");
        return realCreateTexture(descriptor);
    };
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 240)),
        command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, 0x401, 16, 16, 1, 0x31545844 /* DXT1 */, 0, 1)),
        command(OP.SET_FVF, fvfPayload(0x104, [
            element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
            element(0, 12, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD, 0)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x401, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.ok(!executor.failed, "the batch must survive one refused texture");
    assert.equal(executor.stats.texturesRejected, 1);
    assert.equal(executor.stats.droppedDraws, 0,
        "the draw still renders, with the white fallback bound");
    assert.equal(executor.stats.drawsWithFallbackTexture, 1);
    assert.equal(executor.stats.presents, 1);
});

await test("StretchRect from the back buffer becomes a deferred blit", async () => {
    // The back buffer has no view until Present (the swap chain texture is only
    // valid inside the task that acquired it), so this cannot be submitted where
    // the command arrives. Doing it eagerly is what produced "the host cannot
    // address this surface" -- and grabbing the frame into a texture is how a
    // D3D9 game does full-screen post-processing, so it is not a rare path.
    const D3DUSAGE_RENDERTARGET = 1;
    const stretch = (sourceHandle, destinationHandle) => {
        const payload = Buffer.alloc(56);
        payload.writeUInt32LE(DEVICE, 0);
        payload.writeUInt32LE(sourceHandle, 4);
        payload.writeUInt32LE(0, 8);
        [0, 0, 640, 480].forEach((v, i) => payload.writeInt32LE(v, 12 + i * 4));
        payload.writeUInt32LE(destinationHandle, 28);
        payload.writeUInt32LE(0, 32);
        [0, 0, 256, 256].forEach((v, i) => payload.writeInt32LE(v, 36 + i * 4));
        payload.writeUInt32LE(0, 52); // linear filter
        return payload;
    };
    const { executor, find } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, 0x501, 256, 256, 1, 21, D3DUSAGE_RENDERTARGET, 0)),
        command(OP.CLEAR, u32(DEVICE, 1, 0xff000000, 0, 0, 0)),
        // Source handle 0 == the back buffer.
        command(0x8, stretch(0, 0x501)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.ok(!executor.failed, executor.failed && executor.failed.message);
    assert.equal(executor.stats.blitsSkipped, 0,
        "a back-buffer StretchRect must no longer be skipped");
    assert.equal(executor.stats.blits, 1);
    assert.equal(executor.stats.blitsThroughBackBuffer, 1);

    // It has to run as its own pass, drawing the six-vertex quad, into the
    // texture -- not into the back buffer.
    const passes = find("beginRenderPass").map(call => call[2]);
    const blitPass = passes.find(pass =>
        pass.ops.some(op => op[0] === "draw" && op[1] === 6));
    assert.ok(blitPass, "the blit must draw its quad in a pass of its own");
    const viewport = blitPass.ops.find(op => op[0] === "viewport");
    assert.deepEqual(viewport.slice(1, 5), [0, 0, 256, 256],
        "the destination rect becomes the viewport");
    // The source rect reaches the shader normalised against the source size.
    const uniformWrite = find("writeBuffer").pop();
    assert.deepEqual([...new Float32Array(uniformWrite[6].buffer,
        uniformWrite[6].byteOffset, 4)], [0, 0, 1, 1]);
});

await test("D3DSAMP_SRGBTEXTURE samples through an -srgb view", async () => {
    // Ignoring it hands the shader values substantially brighter than the app
    // intends (sRGB 0.5 is linear 0.21), which on an additive environment
    // reflection reads as blown-out white rather than as a gamma difference.
    const D3DSAMP_SRGBTEXTURE = 11;
    const { executor, find } = makeExecutor();
    const draw = extra => [
        command(OP.SET_FVF, fvfPayload(0x104, [
            element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
            element(0, 12, DECLTYPE.FLOAT2, DECLUSAGE.TEXCOORD, 0)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.SET_TEXTURE, u32(DEVICE, 0, 0x401, 0)),
        ...extra,
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
    ];
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 240)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x401, 4, 4, 1, 21, 0, 1)),
        ...draw([]),
        ...draw([command(OP.SET_SAMPLER_STATE,
            u32(DEVICE, 0, D3DSAMP_SRGBTEXTURE, 1))]),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    assert.equal(executor.stats.srgbTextureSamples, 1,
        "only the second draw asked for sRGB decoding");
    assert.equal(executor.stats.srgbViewsCreated, 1);
    assert.equal(executor.stats.srgbTextureUnavailable, 0);

    // The texture has to declare the view format up front, or the view is
    // invalid however it is requested later.
    const created = find("createTexture")
        .map(call => call[1]).find(d => d.size.width === 4);
    assert.deepEqual(created.viewFormats, ["rgba8unorm-srgb"]);
    assert.ok(find("createView").some(call =>
        call[2] && call[2].format === "rgba8unorm-srgb"),
        "the second draw must sample through the -srgb view");
});

await test("D3DRS_SRGBWRITEENABLE renders through an -srgb target view", async () => {
    const { executor, find } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, 194, 1, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.srgbWriteRequests, 1);
    assert.equal(executor.stats.srgbWriteUnavailable, 0);
    assert.deepEqual(find("configure").pop()[1].viewFormats,
        ["bgra8unorm-srgb"]);
    assert.equal(find("createRenderPipeline").pop()[1]
        .fragment.targets[0].format, "bgra8unorm-srgb");
});

await test("a state nothing reads is listed rather than silently dropped",
        async () => {
    // The expensive failures on this path have all been silent: a state the app
    // clearly cares about that the renderer never looks at, producing a picture
    // that is wrong in a plausible way with nothing saying so.
    const D3DRS_WRAP0 = 128, D3DSAMP_MIPMAPLODBIAS = 8;
    const { executor } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, D3DRS_WRAP0, 3, 0)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, 22 /* CULLMODE, read */, 1, 0)),
        command(OP.SET_SAMPLER_STATE, u32(DEVICE, 0, D3DSAMP_MIPMAPLODBIAS, 1)),
        command(OP.SET_SAMPLER_STATE, u32(DEVICE, 0, 5 /* MAGFILTER, read */, 2)),
    ]));
    await executor.idle();
    assert.deepEqual(executor.stats.unreadStateIds, {
        renderStates: [D3DRS_WRAP0],
        samplerStates: [D3DSAMP_MIPMAPLODBIAS],
    }, "only the unread ones, and each listed once");
});

await test("M5 compact declarations feed skeletal shader inputs without CPU repacking",
        async () => {
    const { executor, find } = makeExecutor();
    const shader = shaderCreatePayload(0x40000021, VS_M5_SKINNING_INPUTS);
    const elements = [
        element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
        element(0, 12, DECLTYPE.UBYTE4N, DECLUSAGE.BLENDWEIGHT),
        element(0, 16, DECLTYPE.UBYTE4, DECLUSAGE.BLENDINDICES),
        element(0, 20, DECLTYPE.SHORT2, DECLUSAGE.TEXCOORD),
        element(0, 24, DECLTYPE.SHORT4, DECLUSAGE.TANGENT),
        element(0, 32, DECLTYPE.DEC3N, DECLUSAGE.NORMAL),
        element(0, 36, DECLTYPE.UDEC3, DECLUSAGE.BINORMAL),
    ];
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 400)),
        command(OP.CREATE_VERTEX_DECLARATION,
            declarationPayload(0x30000021, elements)),
        command(OP.CREATE_VERTEX_SHADER, shader.payload, shader.blob,
            shader.blobOffsetField),
        command(OP.SET_VERTEX_DECLARATION, u32(DEVICE, 0x30000021)),
        command(OP.SET_VERTEX_SHADER, u32(DEVICE, 0x40000021)),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 40)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.droppedDraws, 0);
    assert.equal(executor.stats.drawsWithCompactVertexInputs, 1);
    const pipeline = find("createRenderPipeline").pop()[1];
    assert.deepEqual(pipeline.vertex.buffers[0].attributes.map(a => a.format),
        ["float32x3", "unorm8x4", "uint8x4", "sint16x2", "sint16x4",
            "uint32", "uint32"]);
    const wgsl = pipeline.vertex.module.code;
    assert.ok(wgsl.includes("@location(2) in2: vec4<u32>"));
    assert.ok(wgsl.includes("d9_unpack_dec3n(in5)"));
    assert.ok(wgsl.includes("d9_unpack_udec3(in6)"));
});

await test("shadow render states map to signed projection, exact blend and depth-stencil",
        async () => {
    const floatBitsOf = value => {
        const bits = new ArrayBuffer(4);
        new Float32Array(bits)[0] = value;
        return new Uint32Array(bits)[0];
    };
    const R = { SRCBLEND: 19, DESTBLEND: 20, ALPHABLENDENABLE: 27,
        STENCILENABLE: 52, STENCILFAIL: 53, STENCILZFAIL: 54,
        STENCILPASS: 55, STENCILFUNC: 56, STENCILREF: 57,
        STENCILMASK: 58, STENCILWRITEMASK: 59, BLENDFACTOR: 193,
        DEPTHBIAS: 195, SLOPE: 175, SEPARATEALPHA: 206,
        SRCBLENDALPHA: 207, DESTBLENDALPHA: 208, BLENDOPALPHA: 209 };
    const { executor, find } = makeExecutor();
    const states = [
        [R.ALPHABLENDENABLE, 1], [R.SRCBLEND, 14], [R.DESTBLEND, 15],
        [R.BLENDFACTOR, 0x80402010], [R.SEPARATEALPHA, 1],
        [R.SRCBLENDALPHA, 2], [R.DESTBLENDALPHA, 1],
        [R.BLENDOPALPHA, 1], [R.STENCILENABLE, 1], [R.STENCILFAIL, 3],
        [R.STENCILZFAIL, 4], [R.STENCILPASS, 5], [R.STENCILFUNC, 7],
        [R.STENCILREF, 0x55], [R.STENCILMASK, 0xff],
        [R.STENCILWRITEMASK, 0x0f], [R.DEPTHBIAS, floatBitsOf(1 / 0x1000000)],
        [R.SLOPE, floatBitsOf(1.5)],
    ].map(([id, value]) => command(OP.SET_RENDER_STATE,
        u32(DEVICE, id, value, 0)));
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        ...states,
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    const pipeline = find("createRenderPipeline").pop()[1];
    assert.deepEqual(pipeline.fragment.targets[0].blend, {
        color: { srcFactor: "constant", dstFactor: "one-minus-constant",
            operation: "add" },
        alpha: { srcFactor: "one", dstFactor: "zero", operation: "add" },
    });
    assert.equal(pipeline.depthStencil.depthBias, 1);
    assert.equal(pipeline.depthStencil.depthBiasSlopeScale, 1.5);
    assert.deepEqual(pipeline.depthStencil.stencilFront, {
        compare: "greater-equal", failOp: "replace",
        depthFailOp: "increment-clamp", passOp: "decrement-clamp",
    });
    assert.equal(pipeline.depthStencil.stencilReadMask, 0xff);
    assert.equal(pipeline.depthStencil.stencilWriteMask, 0x0f);
    assert.deepEqual(find("setBlendConstant").pop()[1], {
        r: 0x40 / 255, g: 0x20 / 255, b: 0x10 / 255, a: 0x80 / 255,
    });
    assert.equal(find("setStencilReference").pop()[1], 0x55);

    const projected = buildFixedFunctionPixelShader({
        usesTextureFactor: false, specularEnable: false, fogMode: 0,
        alphaTest: { enabled: false, func: 8, reference: 0 },
        stages: [{ index: 0, colorOp: 2, colorArg0: 1, colorArg1: 2,
            colorArg2: 1, alphaOp: 2, alphaArg0: 1, alphaArg1: 2,
            alphaArg2: 1, resultArg: 1, samplesTexture: true,
            textureType: "2d", coordVarying: 0, projected: true,
            transformCount: 3, usesConstant: false }],
    }, null);
    assert.ok(projected.includes("select(-max(abs("),
        "projected shadows must preserve a negative q divisor");

    const bordered = buildFixedFunctionPixelShader({
        usesTextureFactor: false, specularEnable: false, fogMode: 0,
        alphaTest: { enabled: false, func: 8, reference: 0 },
        stages: [{ index: 0, colorOp: 2, colorArg0: 1, colorArg1: 2,
            colorArg2: 1, alphaOp: 2, alphaArg0: 1, alphaArg1: 2,
            alphaArg2: 1, resultArg: 1, samplesTexture: true,
            textureType: "2d", coordVarying: 0, projected: true,
            transformCount: 3, usesConstant: false, addressU: 4,
            addressV: 4, addressW: 1, borderColor: 0x80402010 }],
    }, null);
    assert.ok(bordered.includes("let tex0 = select(vec4<f32>("),
        "BORDER addressing must select the D3D border colour in WGSL");
    assert.ok(bordered.includes(".x >= 0.0") &&
        bordered.includes(".y <= 1.0"),
        "both BORDER axes must reject projected coordinates outside [0,1]");
    assert.ok(bordered.includes("0.25098039") &&
        bordered.includes("0.50196078"),
        "D3DCOLOR border bytes must be converted from ARGB to RGBA");
});

await test("D3D9 default blending is ONE/ZERO when blending is first enabled",
        async () => {
    const { executor, find } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 96)),
        command(OP.SET_FVF, fvfPayload(0x2,
            [element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.SET_RENDER_STATE, u32(DEVICE, 27, 1, 0)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.deepEqual(find("createRenderPipeline").pop()[1]
        .fragment.targets[0].blend, {
        color: { srcFactor: "one", dstFactor: "zero", operation: "add" },
        alpha: { srcFactor: "one", dstFactor: "zero", operation: "add" },
    });
});

await test("M4 ColorFill preserves pixels outside a partial rectangle", async () => {
    const D3DUSAGE_RENDERTARGET = 1;
    const payload = Buffer.alloc(32);
    payload.writeUInt32LE(DEVICE, 0);
    payload.writeUInt32LE(0x501, 4);
    payload.writeUInt32LE(0, 8);
    payload.writeUInt32LE(0x80402010, 12);
    [4, 6, 20, 22].forEach((value, index) =>
        payload.writeInt32LE(value, 16 + index * 4));
    const { executor, find } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, 0x501, 64, 64, 1, 21, D3DUSAGE_RENDERTARGET, 0)),
        command(OP.COLOR_FILL, payload),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.blitsSkipped, 0);
    assert.equal(executor.stats.colorFills, 1);
    const pass = find("beginRenderPass").pop()[2];
    assert.equal(pass.descriptor.colorAttachments[0].loadOp, "load",
        "a sub-rect fill must retain the rest of the attachment");
    assert.deepEqual(pass.ops.find(op => op[0] === "viewport").slice(1, 5),
        [4, 6, 16, 16]);
    assert.deepEqual(pass.ops.find(op => op[0] === "draw"), ["draw", 3]);
});

await test("M4 target fills and copies retain D3D command order", async () => {
    const D3DUSAGE_RENDERTARGET = 1;
    const fill = Buffer.alloc(32);
    fill.writeUInt32LE(DEVICE, 0);
    fill.writeUInt32LE(0x501, 4);
    fill.writeUInt32LE(0, 8);
    fill.writeUInt32LE(0xff204060, 12);
    [0, 0, 64, 64].forEach((value, index) =>
        fill.writeInt32LE(value, 16 + index * 4));
    const stretch = Buffer.alloc(56);
    stretch.writeUInt32LE(DEVICE, 0);
    stretch.writeUInt32LE(0x501, 4);
    stretch.writeUInt32LE(0, 8);
    [0, 0, 64, 64].forEach((value, index) =>
        stretch.writeInt32LE(value, 12 + index * 4));
    stretch.writeUInt32LE(0x502, 28);
    stretch.writeUInt32LE(0, 32);
    [0, 0, 64, 64].forEach((value, index) =>
        stretch.writeInt32LE(value, 36 + index * 4));
    stretch.writeUInt32LE(1, 52);
    const { executor, fake } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, 0x501, 64, 64, 1, 21, D3DUSAGE_RENDERTARGET, 0)),
        command(OP.CREATE_TEXTURE_2D,
            u32(DEVICE, 0x502, 64, 64, 1, 21, D3DUSAGE_RENDERTARGET, 0)),
        command(OP.COLOR_FILL, fill),
        command(0x8, stretch),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    const fillIndex = fake.calls.findIndex(call => call[0] === "beginRenderPass");
    const copyIndex = fake.calls.findIndex(call => call[0] ===
        "copyTextureToTexture");
    assert.ok(fillIndex >= 0 && copyIndex > fillIndex,
        "ColorFill must be encoded before the following StretchRect copy");
    assert.equal(executor.stats.queueSubmits, 1,
        "ordered target operations share the Present submission");
});

await test("M4 Clear honours its rectangle list", async () => {
    const clear = Buffer.alloc(40);
    clear.writeUInt32LE(DEVICE, 0);
    clear.writeUInt32LE(1, 4); // D3DCLEAR_TARGET
    clear.writeUInt32LE(0xff336699, 8);
    clear.writeFloatLE(1, 12);
    clear.writeUInt32LE(0, 16);
    clear.writeUInt32LE(1, 20);
    [8, 10, 28, 34].forEach((value, index) =>
        clear.writeInt32LE(value, 24 + index * 4));
    const { executor, find } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CLEAR, clear),
        command(OP.PRESENT, u32(DEVICE, 0x1234, 0, 0, 640, 480)),
    ], { present: true }));
    await executor.idle();
    assert.equal(executor.stats.partialClears, 1);
    const pass = find("beginRenderPass").pop()[2];
    assert.equal(pass.descriptor.colorAttachments[0].loadOp, "load");
    assert.deepEqual(pass.ops.find(op => op[0] === "viewport").slice(1, 5),
        [8, 10, 20, 24]);
    assert.deepEqual(pass.ops.find(op => op[0] === "draw"), ["draw", 3]);
});

// WebGPU counts a block-compressed copy in whole 4x4 blocks, and a mip level's
// physical extent is its logical size rounded up to that grid. The tail of a
// DXT mip chain is logically 2x2 and 1x1, so passing the logical size makes
// writeTexture fail validation. That failure arrives as an uncaptured device
// error rather than an exception, so the only symptom is that the smallest mips
// keep whatever the texture was created with -- which is how Kart Rider's UI
// atlases sampled as garbage while the console filled with "copySize.width (1)
// is not a multiple of compressed texture format block width (4)".
await test("a DXT mip chain's sub-block levels upload as whole 4x4 blocks",
        async () => {
    const { executor, find } = makeExecutor();
    const DXT1 = 0x31545844;
    // 8x8 DXT1 with a full chain: 8x8, 4x4, 2x2, 1x1. Every level occupies at
    // least one 8-byte block, and the last two are smaller than one block.
    const level = (index, size) => {
        const blockRow = Math.ceil(size / 4) * 8;
        const bytes = blockRow * Math.ceil(size / 4);
        const payload = Buffer.alloc(48);
        payload.writeUInt32LE(0x401, 0);
        payload.writeUInt32LE(index, 4);
        payload.writeUInt32LE(0, 8);       // x
        payload.writeUInt32LE(0, 12);      // y
        payload.writeUInt32LE(0, 16);      // z
        payload.writeUInt32LE(size, 20);   // logical width
        payload.writeUInt32LE(size, 24);   // logical height
        payload.writeUInt32LE(1, 28);      // depth
        payload.writeUInt32LE(blockRow, 32); // row pitch, in bytes per block row
        payload.writeUInt32LE(0, 36);
        payload.writeUInt32LE(bytes, 40);
        return { payload, blob: Buffer.alloc(bytes, index + 1), field: 44 };
    };
    const uploads = [[0, 8], [1, 4], [2, 2], [3, 1]]
        .map(([index, size]) => level(index, size));
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_TEXTURE_2D, u32(DEVICE, 0x401, 8, 8, 4, DXT1, 0, 1)),
        ...uploads.map(upload =>
            command(OP.UPDATE_TEXTURE, upload.payload, upload.blob, upload.field)),
    ]));
    await executor.idle();

    // The executor also writes its 1x1 fallback texture, which carries no
    // mipLevel; only the mip uploads are of interest here.
    const writes = find("writeTexture")
        .filter(call => call[1].mipLevel !== undefined);
    assert.equal(writes.length, 4, "every level has to be written");
    for (const write of writes) {
        const mipLevel = write[1].mipLevel;
        const size = write[4];
        assert.equal(size.width % 4, 0,
            "level " + mipLevel + " copy width " + size.width +
            " is not a whole number of 4x4 blocks");
        assert.equal(size.height % 4, 0,
            "level " + mipLevel + " copy height " + size.height +
            " is not a whole number of 4x4 blocks");
        // Rounding up must not overshoot the level's physical extent either.
        const physical = Math.max(4, Math.ceil((8 >> mipLevel) / 4) * 4);
        assert.equal(size.width, physical);
        assert.equal(size.height, physical);
    }
});

// PRESENT carries the window's *client rect* so the page can place the overlay
// canvas; emit_present_and_flush fills it from GetClientRect. It is not the back
// buffer's size, and a windowed game's client area is shorter than the back
// buffer it hosts. Treating it as the render size made the swap-chain colour
// attachment look like it disagreed with the auto depth target created beside
// it, and the mismatch path then dropped depth for every pass -- depth testing
// off for the whole game, from a window border.
await test("a client rect smaller than the back buffer keeps depth attached",
        async () => {
    const { executor, find } = makeExecutor();
    // 640x467 client rect for a 640x480 back buffer: the window has a title bar.
    const clientRect = u32(DEVICE, 0x1234, 0, 0, 640, 467);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.PRESENT, clientRect),
    ], { present: true }));
    await executor.idle();

    await executor.submit(buildBatch([
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 240)),
        command(OP.SET_FVF, fvfPayload(0x104, [
            element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 12)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
        command(OP.PRESENT, clientRect),
    ], { present: true }));
    await executor.idle();

    assert.equal(executor.stats.depthTargetSizeMismatches, 0,
        "the client rect must not be mistaken for the back buffer's size");
    const pass = find("beginRenderPass").pop()[1];
    assert.ok(pass.depthStencilAttachment,
        "the back-buffer pass keeps its auto depth-stencil");
});

// D3D9 rasterises with the sample point at a pixel's integer corner; WebGPU
// samples at the pixel centre. A title that blits UI 1:1 has already subtracted
// that half pixel itself, so replaying its geometry unchanged lands every
// sample on a texel boundary and bilinear filtering returns the mean of two
// texels -- invisible on 3D art, ruinous on small text. XYZRHW UI is the case
// that shows it, but the offset belongs on every fixed-function draw.
await test("fixed-function draws carry the D3D9 half-pixel offset", async () => {
    const { executor, find } = makeExecutor();
    const drawWith = position => buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 240)),
        command(OP.SET_FVF, fvfPayload(0x104, [
            element(0, 0, DECLTYPE.FLOAT4, position),
            element(0, 16, DECLTYPE.D3DCOLOR, DECLUSAGE.COLOR)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
    ]);

    // POSITIONT: pre-transformed UI, the path the shop text goes through.
    await executor.submit(drawWith(DECLUSAGE.POSITIONT));
    await executor.idle();
    // POSITION: ordinary world-space geometry, through world_view_projection.
    await executor.submit(drawWith(DECLUSAGE.POSITION));
    await executor.idle();

    const vertexShaders = find("createShaderModule")
        .map(call => call[1].code)
        .filter(code => code.includes("d9_vs_main"));
    assert.ok(vertexShaders.length >= 2,
        "both a screen-space and a world-space vertex shader were built");
    for (const code of vertexShaders) {
        assert.ok(code.includes(
            "result.position.x + result.position.w / uniforms.viewport.x"),
            "a fixed-function vertex shader is missing the half-pixel offset");
        // Screen y grows downward, NDC y grows upward: the y term is negated.
        assert.ok(code.includes(
            "result.position.y - result.position.w / uniforms.viewport.y"),
            "the half-pixel offset must negate y");
    }
});

// The proxy sends BLENDWEIGHT/BLENDINDICES through, but only D3DTS_WORLD is
// ever consumed, so a fixed-function skinned mesh is posed by world matrix 0
// alone. That renders as a collapsed or contorted model with no other trace,
// so it has to be reported rather than drawn silently wrong.
await test("a fixed-function skinned declaration is reported, not drawn silently",
        async () => {
    const { executor } = makeExecutor();
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 240)),
        command(OP.SET_FVF, fvfPayload(0x104, [
            element(0, 0, DECLTYPE.FLOAT3, DECLUSAGE.POSITION),
            element(0, 12, DECLTYPE.FLOAT4, DECLUSAGE.BLENDWEIGHT),
            element(0, 28, DECLTYPE.UBYTE4, DECLUSAGE.BLENDINDICES),
            element(0, 32, DECLTYPE.FLOAT3, DECLUSAGE.NORMAL)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 44)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
    ]));
    await executor.idle();
    assert.equal(executor.stats.drawsWithUnappliedVertexBlend, 1,
        "a declaration carrying blend weights has to be counted");
});

// XYZRHW coordinates are absolute render-target pixels. setViewport already
// puts the viewport's origin back when it maps NDC into the viewport rect, so
// the shader has to take that origin off first. Getting this wrong cancels out
// exactly when the viewport sits at 0,0 -- which is every full-screen UI pass,
// and is why it stayed invisible until a game drew pre-transformed geometry
// through a small offset viewport (Kart Rider's shop item panels) and the
// geometry landed several viewport-widths outside the box.
await test("pre-transformed geometry subtracts the viewport origin",
        async () => {
    const { executor, find } = makeExecutor();
    const viewport = Buffer.alloc(32);
    viewport.writeUInt32LE(DEVICE, 0);
    viewport.writeUInt32LE(368, 4);   // x
    viewport.writeUInt32LE(104, 8);   // y
    viewport.writeUInt32LE(110, 12);  // width
    viewport.writeUInt32LE(109, 16);  // height
    viewport.writeFloatLE(0, 20);
    viewport.writeFloatLE(1, 24);
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(800, 600)),
        command(OP.CREATE_BUFFER, createBufferPayload(0x201, 1, 240)),
        command(OP.SET_VIEWPORT, viewport),
        command(OP.SET_FVF, fvfPayload(0x104, [
            element(0, 0, DECLTYPE.FLOAT4, DECLUSAGE.POSITIONT),
            element(0, 16, DECLTYPE.D3DCOLOR, DECLUSAGE.COLOR)])),
        command(OP.SET_STREAM_SOURCE, setStreamSourcePayload(0, 0x201, 20)),
        command(OP.DRAW_PRIMITIVE, drawPrimitivePayload(4, 0, 1)),
    ]));
    await executor.idle();

    const vertexShader = find("createShaderModule").map(call => call[1].code)
        .filter(code => code.includes("d9_vs_main")).pop();
    assert.ok(vertexShader, "a screen-space vertex shader was built");
    assert.ok(vertexShader.includes("- viewport.z") &&
        vertexShader.includes("- viewport.w"),
        "the XYZRHW path must subtract the viewport origin:\n" + vertexShader);

    // The origin has to actually reach the uniform, not just the WGSL.
    const writes = find("writeBuffer");
    assert.ok(writes.length > 0, "constants were uploaded");
    const carriesOrigin = writes.some(call => {
        const data = call[3];
        if (!data) return false;
        const floats = new Float32Array(data.buffer || data, data.byteOffset || 0,
            Math.floor((data.byteLength || data.length || 0) / 4));
        for (let i = 0; i + 3 < floats.length; ++i) {
            if (floats[i] === 110 && floats[i + 1] === 109 &&
                    floats[i + 2] === 368 && floats[i + 3] === 104)
                return true;
        }
        return false;
    });
    assert.ok(carriesOrigin,
        "the viewport uniform must carry size in xy and origin in zw");
});

// Guest-to-host diagnostics. Everything the guest DLL refuses used to be
// invisible from the page -- the console sees only valid commands, and the
// guest's trace file is inside a VM whose filesystem the developer cannot
// reach -- which repeatedly turned "the picture is wrong" into guesswork.
await test("a guest log command reaches the console with its text intact",
        async () => {
    const { executor } = makeExecutor();
    const text = "CreateVertexBuffer refused: length=0 usage=00000008";
    const payload = Buffer.alloc(8 + text.length);
    payload.writeUInt32LE(2, 0);            // severity: failed
    payload.writeUInt32LE(text.length, 4);
    payload.write(text, 8, "ascii");

    const errors = [];
    const realError = console.error;
    console.error = (...args) => errors.push(args.join(" "));
    try {
        await executor.submit(buildBatch([
            command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
            command(OP.GUEST_LOG, payload),
        ]));
        await executor.idle();
    } finally {
        console.error = realError;
    }
    assert.equal(executor.stats.guestReports, 1);
    assert.equal(executor.stats.unsupportedCommands, 0,
        "the opcode has to be handled, not counted as unknown");
    assert.ok(errors.some(line => line === "[d3d9-guest] " + text),
        "the guest's text has to arrive verbatim, got: " + errors.join(" | "));
});

// The guest identifies itself at startup so that a session with no other
// guest messages means "nothing was refused" rather than "the DLL inside the
// disk image predates this channel and cannot say anything". Info severity has
// to reach the console like the rest, just not as a warning.
await test("an info-severity guest log is reported without being a warning",
        async () => {
    const { executor } = makeExecutor();
    const text = "proxy build guest-log-20260816 loaded";
    const payload = Buffer.alloc(8 + text.length);
    payload.writeUInt32LE(0, 0);            // severity: info
    payload.writeUInt32LE(text.length, 4);
    payload.write(text, 8, "ascii");

    const logs = [];
    const warnings = [];
    const realLog = console.log;
    const realWarn = console.warn;
    console.log = (...args) => logs.push(args.join(" "));
    console.warn = (...args) => warnings.push(args.join(" "));
    try {
        await executor.submit(buildBatch([
            command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
            command(OP.GUEST_LOG, payload),
        ]));
        await executor.idle();
    } finally {
        console.log = realLog;
        console.warn = realWarn;
    }
    assert.equal(executor.stats.guestReports, 1);
    assert.ok(logs.some(line => line === "[d3d9-guest] " + text),
        "the identification line has to reach the console");
    assert.ok(!warnings.some(line => line.includes(text)),
        "identification is not a warning");
});

// A truncated length field must not read past the command into whatever
// follows it in the batch.
await test("a guest log claiming more text than it carries is rejected",
        async () => {
    const { executor } = makeExecutor();
    const payload = Buffer.alloc(12);
    payload.writeUInt32LE(1, 0);
    payload.writeUInt32LE(0xffff, 4); // far more than the 4 bytes present
    await executor.submit(buildBatch([
        command(OP.CREATE_DEVICE, createDevicePayload(640, 480)),
        command(OP.GUEST_LOG, payload),
    ]));
    await executor.idle();
    assert.equal(executor.stats.malformedBatches, 1);
    assert.equal(executor.stats.guestReports, 0);
});

// ---- report ----

if (failures.length) {
    for (const failure of failures) {
        console.error("FAIL " + failure.name);
        console.error("  " + (failure.error && failure.error.message));
        if (process.env.D9_TEST_STACK) console.error(failure.error);
    }
    console.error("\n" + failures.length + " failed, " + passed + " passed");
    process.exit(1);
}
console.log(passed + " executor tests passed");
}

main().catch(error => { console.error(error); process.exit(1); });
