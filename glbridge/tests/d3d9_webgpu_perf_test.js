#!/usr/bin/env node
// M6 steady-state budget guard for the D3D9 WebGPU executor. This is a
// counting WebGPU device rather than a timing benchmark: it pins the resource
// creation/submission invariants that make frame time predictable on real GPU
// drivers.

"use strict";

const assert = require("node:assert/strict");
const { D3D9WebGPUExecutor } = require("../d3d9-webgpu/d3d9_executor.js");

const DEVICE = 0x100002;
const OP = {
    CREATE_DEVICE: 2, PRESENT: 4, CREATE_BUFFER: 0x100,
    SET_RENDER_STATE: 0x200, SET_STREAM_SOURCE: 0x20a, SET_FVF: 0x20e,
    DRAW_PRIMITIVE: 0x300,
};

function u32(...values) {
    const out = Buffer.alloc(values.length * 4);
    values.forEach((value, index) => out.writeUInt32LE(value >>> 0, index * 4));
    return out;
}

function command(opcode, payload) { return { opcode, payload }; }

function batch(commands, present) {
    let commandBytes = 0;
    for (const item of commands) {
        item.size = (16 + item.payload.length + 7) & ~7;
        item.offset = 32 + commandBytes;
        commandBytes += item.size;
    }
    const out = Buffer.alloc(32 + commandBytes);
    out.writeUInt32LE(0x47573944, 0);
    out.writeUInt16LE(1, 4);
    out.writeUInt16LE(0, 6);
    out.writeUInt32LE(present ? 1 : 0, 12);
    out.writeUInt32LE(commands.length, 16);
    out.writeUInt32LE(commandBytes, 20);
    commands.forEach((item, index) => {
        out.writeUInt16LE(item.opcode, item.offset);
        out.writeUInt32LE(item.size, item.offset + 4);
        out.writeUInt32LE(index + 1, item.offset + 8);
        item.payload.copy(out, item.offset + 16);
    });
    return out;
}

function createDevice() {
    const out = Buffer.alloc(44);
    out.writeUInt32LE(DEVICE, 0);
    out.writeUInt32LE(0x1234, 4);
    out.writeUInt32LE(800, 16);
    out.writeUInt32LE(600, 20);
    out.writeUInt32LE(22, 24);
    out.writeUInt32LE(1, 28);
    return out;
}

function createBuffer(handle, bytes) {
    return u32(DEVICE, handle, 1, bytes, 0, 0);
}

function fvf() {
    const out = Buffer.alloc(24);
    out.writeUInt32LE(DEVICE, 0);
    out.writeUInt32LE(0x002, 4);
    out.writeUInt32LE(1, 8);
    // D9WGVertexElement: stream 0, offset 0, FLOAT3, POSITION0.
    out.writeUInt16LE(0, 16);
    out.writeUInt16LE(0, 18);
    out.writeUInt8(2, 20);
    out.writeUInt8(0, 21);
    out.writeUInt8(0, 22);
    out.writeUInt8(0, 23);
    return out;
}

function frameCommands(includeSetup) {
    const commands = [];
    if (includeSetup) {
        commands.push(command(OP.CREATE_DEVICE, createDevice()));
        commands.push(command(OP.CREATE_BUFFER, createBuffer(0x201, 4096)));
        commands.push(command(OP.SET_FVF, fvf()));
        commands.push(command(OP.SET_STREAM_SOURCE,
            u32(DEVICE, 0, 0x201, 12, 0)));
    }
    // 150 draws with 300 state writes. CULLMODE alternates between two real
    // pipeline states; TEXTUREFACTOR changes twice per draw but is not read by
    // this untextured cascade, so it must not force a GPU resource allocation.
    for (let draw = 0; draw < 150; ++draw) {
        commands.push(command(OP.SET_RENDER_STATE,
            u32(DEVICE, 22, draw & 1 ? 1 : 3, 0)));
        commands.push(command(OP.SET_RENDER_STATE,
            u32(DEVICE, 60, 0xff000000 | draw, 0)));
        commands.push(command(OP.DRAW_PRIMITIVE,
            u32(DEVICE, 4, 0, 1)));
    }
    commands.push(command(OP.PRESENT,
        u32(DEVICE, 0x1234, 0, 0, 800, 600, 0, 0)));
    return commands;
}

function countingWebGPU() {
    const counts = new Map();
    const hit = name => counts.set(name, (counts.get(name) || 0) + 1);
    class BufferObject {
        constructor(descriptor) { this.size = descriptor.size; }
        destroy() { hit("destroyBuffer"); }
    }
    class TextureObject {
        constructor(descriptor) { this.descriptor = descriptor; }
        createView() { return { texture: this }; }
        destroy() { hit("destroyTexture"); }
    }
    class Pass {
        setPipeline() {}
        setBindGroup() {}
        setBlendConstant() {}
        setStencilReference() {}
        setViewport() {}
        setScissorRect() {}
        setVertexBuffer() {}
        setIndexBuffer() {}
        draw() { hit("draw"); }
        drawIndexed() { hit("drawIndexed"); }
        end() {}
    }
    const queue = {
        writeBuffer() { hit("writeBuffer"); },
        writeTexture() { hit("writeTexture"); },
        submit() { hit("submit"); },
        onSubmittedWorkDone() { return Promise.resolve(); },
    };
    const device = {
        queue, lost: new Promise(() => {}),
        createBuffer(descriptor) { hit("createBuffer"); return new BufferObject(descriptor); },
        createTexture(descriptor) { hit("createTexture"); return new TextureObject(descriptor); },
        createShaderModule(descriptor) {
            hit("createShaderModule");
            return { code: descriptor.code,
                getCompilationInfo: async () => ({ messages: [] }) };
        },
        createSampler() { hit("createSampler"); return {}; },
        createBindGroupLayout(descriptor) {
            hit("createBindGroupLayout");
            return { bindings: new Set(descriptor.entries.map(entry => entry.binding)) };
        },
        createPipelineLayout(descriptor) {
            hit("createPipelineLayout"); return { bindGroupLayouts: descriptor.bindGroupLayouts };
        },
        createRenderPipeline(descriptor) {
            hit("createRenderPipeline"); return { descriptor };
        },
        createBindGroup(descriptor) { hit("createBindGroup"); return descriptor; },
        createCommandEncoder() {
            hit("createCommandEncoder");
            return { beginRenderPass() { hit("beginRenderPass"); return new Pass(); },
                finish() { return {}; } };
        },
    };
    const context = {
        configure() {},
        getCurrentTexture() {
            return { width: 800, height: 600, createView: () => ({ swapchain: true }) };
        },
    };
    return { device, context, counts,
        snapshot: () => Object.fromEntries(counts) };
}

function delta(after, before, key) {
    return (after[key] || 0) - (before[key] || 0);
}

async function main() {
    const fake = countingWebGPU();
    const executor = new D3D9WebGPUExecutor({ getContext: () => fake.context }, {
        device: fake.device, context: fake.context, format: "bgra8unorm",
        uniformRingBytes: 4 * 1024 * 1024,
    });

    // Two warm-up frames populate both alternating pipeline/bind-group keys.
    await executor.submit(batch(frameCommands(true), true));
    await executor.submit(batch(frameCommands(false), true));
    await executor.idle();
    const before = fake.snapshot();
    const statsBefore = executor.getStats();

    await executor.submit(batch(frameCommands(false), true));
    await executor.idle();
    const after = fake.snapshot();
    const stats = executor.getStats();

    assert.equal(delta(after, before, "createRenderPipeline"), 0,
        "steady frame created a pipeline");
    assert.equal(delta(after, before, "createBindGroup"), 0,
        "steady frame created a bind group");
    assert.equal(delta(after, before, "createBuffer"), 0,
        "steady frame created a GPU buffer");
    assert.equal(delta(after, before, "createTexture"), 0,
        "steady frame created a texture");
    assert.equal(delta(after, before, "submit"), 1,
        "one D3D frame should be one WebGPU queue submit");
    assert.equal(delta(after, before, "beginRenderPass"), 1,
        "compatible draws should merge into one render pass");
    assert.equal(delta(after, before, "copyTextureToBuffer"), 0,
        "normal rendering must not read the GPU back");
    assert.equal(delta(after, before, "mapAsync"), 0,
        "normal rendering must not map GPU results");
    assert.equal(stats.lastFrame.pipelineCreations, 0);
    assert.equal(stats.lastFrame.bindGroupCreations, 0);
    assert.ok(stats.bindGroupHits - statsBefore.bindGroupHits >= 150,
        "every steady draw should hit the bind-group cache");
    assert.ok(stats.uniformSlotReuses - statsBefore.uniformSlotReuses >= 149,
        "identical constants should upload once for a run of draws");
    assert.equal(stats.uniformRingOverflows, 0);
    assert.ok(stats.lastFrame.queueSubmits <= 5, "M6 submit budget exceeded");

    console.log("PASS d3d9 M6 steady-state: 150 draws, 0 pipeline/bind-group/buffer " +
        "creations, 1 pass, 1 submit");
}

main().catch(error => {
    console.error(error);
    process.exit(1);
});
