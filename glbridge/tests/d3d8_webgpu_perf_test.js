"use strict";

// Stage 7 performance-hardening regression test.
//
// The doc's stage 7 exit conditions are per-frame budgets (section 12.1), not
// wall-clock numbers, so they can be asserted deterministically against a
// counting fake WebGPU device:
//
//   - steady-state pipeline creation  = 0 / frame
//   - steady-state bind group creation = 0 / frame
//   - steady-state GPU buffer creation = 0 / frame
//   - no GPU readback on a normal frame
//   - one render pass and one queue submit per frame
//   - uniform upload traffic proportional to distinct uniform state, not to
//     draw count or to unrelated render-state churn
//
// These are the properties that regress silently, so they are pinned here
// rather than left to a manual browser session.

const assert = require("node:assert/strict");
const {
    D3D8WebGPUExecutor,
    D8WG_MAGIC,
    D8WG_VERSION_MAJOR,
    D8WG_VERSION_MINOR,
} = require("../d3d8-webgpu/d3d8_executor.js");

const OP_HELLO = 1;
const OP_CREATE_DEVICE = 2;
const OP_PRESENT = 4;
const OP_CLEAR = 5;
const OP_BEGIN_SCENE = 6;
const OP_END_SCENE = 7;
const OP_CREATE_BUFFER = 0x100;
const OP_UPDATE_BUFFER = 0x101;
const OP_SET_RENDER_STATE = 0x200;
const OP_SET_STREAM_SOURCE = 0x208;
const OP_SET_VERTEX_FORMAT = 0x20A;
const OP_DRAW_PRIMITIVE = 0x300;

const D3DRS_SRCBLEND = 19;
const D3DRS_DESTBLEND = 20;
const D3DRS_TEXTUREFACTOR = 60;

function countingFake() {
    const n = {
        createBuffer: 0, createBindGroup: 0, createPipeline: 0,
        createTexture: 0, writeBuffer: 0, writeBufferBytes: 0,
        renderPass: 0, submit: 0, mapAsync: 0, copyTextureToBuffer: 0,
        setBindGroup: 0, dynamicOffsets: [],
    };
    const pass = {
        setPipeline() {},
        setBindGroup(index, group, data, start, length) {
            n.setBindGroup++;
            if (data && length) n.dynamicOffsets.push(data[start]);
        },
        setVertexBuffer() {}, setIndexBuffer() {}, setViewport() {},
        setStencilReference() {}, setScissorRect() {},
        draw() {}, drawIndexed() {}, end() {},
    };
    const encoder = {
        beginRenderPass() { n.renderPass++; return pass; },
        copyBufferToBuffer() {},
        copyBufferToTexture() {},
        copyTextureToBuffer() { n.copyTextureToBuffer++; },
        finish() { return {}; },
    };
    const device = {
        lost: new Promise(() => {}),
        queue: {
            writeBuffer(buffer, offset, data) {
                n.writeBuffer++;
                n.writeBufferBytes += data.byteLength;
            },
            writeTexture() {},
            submit() { n.submit++; },
            onSubmittedWorkDone() { return Promise.resolve(); },
        },
        createShaderModule(d) { return { ...d, _code: d.code }; },
        createBuffer(d) {
            n.createBuffer++;
            return { descriptor: d, destroy() {},
                mapAsync() { n.mapAsync++; return Promise.resolve(); },
                getMappedRange() { return new ArrayBuffer(d.size); },
                unmap() {} };
        },
        createTexture(d) {
            n.createTexture++;
            return { descriptor: d, createView: () => ({}), destroy() {} };
        },
        createSampler(d) { return { descriptor: d }; },
        createCommandEncoder() { return encoder; },
        createBindGroupLayout(d) {
            return { descriptor: d,
                bindings: new Set(d.entries.map(e => e.binding)) };
        },
        createPipelineLayout(d) {
            return { descriptor: d, bindGroupLayouts: d.bindGroupLayouts };
        },
        createRenderPipeline(d) {
            n.createPipeline++;
            return { descriptor: d, getBindGroupLayout() {
                return d.layout === "auto"
                    ? { bindings: new Set([0, 1, 2, 3, 4]) }
                    : d.layout.bindGroupLayouts[0];
            } };
        },
        createBindGroup(d) { n.createBindGroup++; return d; },
    };
    const context = {
        configure() {},
        getCurrentTexture() { return { createView: () => ({}) }; },
    };
    const gpu = {
        async requestAdapter() {
            return { async requestDevice() { return device; } };
        },
        getPreferredCanvasFormat() { return "bgra8unorm"; },
    };
    return { n, device, context, gpu };
}

function u32Payload(...values) {
    const payload = Buffer.alloc(values.length * 4);
    values.forEach((v, i) => payload.writeUInt32LE(v >>> 0, i * 4));
    return payload;
}

function createDevicePayload(handle, width, height) {
    const p = Buffer.alloc(44);
    p.writeUInt32LE(handle, 0);
    p.writeUInt32LE(0x1234, 4);
    p.writeInt32LE(0, 8);
    p.writeInt32LE(0, 12);
    p.writeUInt32LE(width, 16);
    p.writeUInt32LE(height, 20);
    p.writeUInt32LE(22, 24);
    p.writeUInt32LE(1, 28);
    p.writeUInt32LE(0x20, 32);
    p.writeUInt32LE(1, 36);
    p.writeUInt32LE(75, 40);
    return p;
}

function surfacePayload(handle, width, height) {
    const p = Buffer.alloc(24);
    p.writeUInt32LE(handle, 0);
    p.writeUInt32LE(0x1234, 4);
    p.writeInt32LE(0, 8);
    p.writeInt32LE(0, 12);
    p.writeUInt32LE(width, 16);
    p.writeUInt32LE(height, 20);
    return p;
}

function batch(commands, frameId) {
    let commandBytes = 0;
    for (const c of commands) {
        c.size = (16 + c.payload.length +
            (c.blob ? c.blob.length : 0) + 7) & ~7;
        c.offset = 32 + commandBytes;
        commandBytes += c.size;
    }
    const result = Buffer.alloc(32 + commandBytes);
    result.writeUInt32LE(D8WG_MAGIC, 0);
    result.writeUInt16LE(D8WG_VERSION_MAJOR, 4);
    result.writeUInt16LE(D8WG_VERSION_MINOR, 6);
    result.writeUInt32LE(frameId, 8);
    result.writeUInt32LE(1, 12);
    result.writeUInt32LE(commands.length, 16);
    result.writeUInt32LE(commandBytes, 20);
    result.writeUInt32LE(0xA0010001, 24);
    result.writeUInt32LE(0x20260802, 28);
    let sequence = 1;
    for (const c of commands) {
        result.writeUInt16LE(c.opcode, c.offset);
        result.writeUInt32LE(c.size, c.offset + 4);
        result.writeUInt32LE(sequence++, c.offset + 8);
        c.payload.copy(result, c.offset + 16);
        if (c.blob) {
            const blobOffset = c.offset + 16 + c.payload.length;
            result.writeUInt32LE(blobOffset, c.offset + 16 + 12);
            c.blob.copy(result, blobOffset);
        }
    }
    return result;
}

const command = (opcode, payload, blob) => ({ opcode, payload, blob });

const DEVICE = 0x00100002;
const VERTEX_BUFFER = 0x00100003;

async function makeReadyExecutor() {
    const fake = countingFake();
    const canvas = { width: 1, height: 1, getContext: () => fake.context };
    const executor = new D3D8WebGPUExecutor(canvas, { gpu: fake.gpu });

    const vertices = Buffer.alloc(4 * 20);
    for (let i = 0; i < 4; i++) {
        vertices.writeFloatLE(i * 10, i * 20);
        vertices.writeFloatLE(i * 10, i * 20 + 4);
        vertices.writeFloatLE(0.5, i * 20 + 8);
        vertices.writeFloatLE(1, i * 20 + 12);
        vertices.writeUInt32LE(0xFFFFFFFF, i * 20 + 16);
    }
    const createBuffer = Buffer.alloc(32);
    createBuffer.writeUInt32LE(DEVICE, 0);
    createBuffer.writeUInt32LE(VERTEX_BUFFER, 4);
    createBuffer.writeUInt32LE(1, 8);
    createBuffer.writeUInt32LE(vertices.length, 12);
    createBuffer.writeUInt32LE(0x200, 16);
    createBuffer.writeUInt32LE(0x44, 20);
    const update = Buffer.alloc(24);
    update.writeUInt32LE(VERTEX_BUFFER, 0);
    update.writeUInt32LE(0, 4);
    update.writeUInt32LE(vertices.length, 8);

    await executor.submit(batch([
        command(OP_HELLO, u32Payload(32, 0, 0xA0010001, 0x20260802)),
        command(OP_CREATE_DEVICE, createDevicePayload(DEVICE, 640, 480)),
        command(OP_CREATE_BUFFER, createBuffer),
        command(OP_UPDATE_BUFFER, update, vertices),
        command(OP_SET_STREAM_SOURCE, u32Payload(DEVICE, 0, VERTEX_BUFFER, 20)),
        command(OP_SET_VERTEX_FORMAT, u32Payload(DEVICE, 0x44)),
    ], 1), {});
    assert.equal(executor.failed, null);
    return { fake, executor };
}

// A Maple-shaped frame: many sprite draws, each preceded by pipeline-state
// churn (blend factors) that must NOT invalidate the uniform block.
function spriteFrame(draws) {
    const commands = [
        command(OP_CLEAR,
            u32Payload(DEVICE, 1, 0xFF000000, 0x3F800000, 0, 0)),
        command(OP_BEGIN_SCENE, u32Payload(DEVICE, 0)),
    ];
    for (let i = 0; i < draws; i++) {
        commands.push(command(OP_SET_RENDER_STATE,
            u32Payload(DEVICE, D3DRS_SRCBLEND, (i % 2) ? 2 : 5, 0)));
        commands.push(command(OP_SET_RENDER_STATE,
            u32Payload(DEVICE, D3DRS_DESTBLEND, (i % 2) ? 1 : 6, 0)));
        commands.push(command(OP_DRAW_PRIMITIVE, u32Payload(DEVICE, 4, 0, 1)));
    }
    commands.push(command(OP_END_SCENE, u32Payload(DEVICE, 0)));
    commands.push(command(OP_PRESENT, surfacePayload(DEVICE, 640, 480)));
    return commands;
}

async function main() {
    const { fake, executor } = await makeReadyExecutor();
    const DRAWS = 150;
    const FRAMES = 10;

    // Two warm-up frames populate the pipeline/bind-group/sampler caches.
    await executor.submit(batch(spriteFrame(DRAWS), 2), {});
    await executor.submit(batch(spriteFrame(DRAWS), 3), {});
    assert.equal(executor.failed, null);

    const before = { ...fake.n };
    for (let frame = 0; frame < FRAMES; frame++) {
        await executor.submit(batch(spriteFrame(DRAWS), 4 + frame), {});
        assert.equal(executor.failed, null);
    }
    // Snapshot immediately: later verification frames also move these
    // counters, and a summary computed against live counters would misreport
    // the steady-state numbers this test exists to pin.
    const after = { ...fake.n };
    const delta = key => after[key] - before[key];

    // --- Stage 7 steady-state budgets (doc 12.1) ---
    assert.equal(delta("createPipeline"), 0,
        "steady-state pipeline creation must be 0/frame");
    assert.equal(delta("createBindGroup"), 0,
        "steady-state bind group creation must be 0/frame: the uniform slot " +
        "is addressed by dynamic offset and must not be part of bind group " +
        "identity");
    assert.equal(delta("createBuffer"), 0,
        "steady-state GPU buffer creation must be 0/frame: uniforms come " +
        "from the persistent ring, not a fresh buffer per state change");
    assert.equal(delta("createTexture"), 0,
        "no texture should be created on a steady-state frame");
    assert.equal(delta("renderPass"), FRAMES,
        "a frame of compatible draws must merge into one render pass");
    assert.equal(delta("submit"), FRAMES,
        "a normal frame must cost exactly one queue submit");

    // --- no readback on a normal frame (doc 10.4 / 12.1) ---
    assert.equal(delta("copyTextureToBuffer"), 0,
        "a normal frame must not read back from the GPU");
    assert.equal(delta("mapAsync"), 0,
        "a normal frame must not map a GPU buffer");

    // --- uniform traffic tracks distinct uniform state, not draw count ---
    assert.equal(delta("writeBuffer"), FRAMES,
        "blend-factor churn must not repack the uniform block: only one " +
        "uniform upload per frame is expected");
    assert.ok(delta("writeBufferBytes") <= FRAMES * 2048,
        "uniform upload bytes should stay near one block per frame, saw " +
        delta("writeBufferBytes"));

    // Every draw still binds, and every bind carries a valid dynamic offset.
    assert.equal(delta("setBindGroup"), FRAMES * DRAWS,
        "every draw must still bind its resources");
    for (const offset of fake.n.dynamicOffsets) {
        assert.equal(offset % 256, 0,
            "a dynamic uniform offset must respect the 256-byte alignment " +
            "limit, saw " + offset);
    }

    const stats = executor.getStats();
    assert.equal(stats.uniformRingOverflows, 0,
        "the uniform ring should absorb a normal frame without overflowing");
    assert.ok(stats.bindGroupHits > 0, "bind group cache should be hitting");
    assert.ok(stats.redundantStateWrites >= 0);

    // --- correctness guard: a render state the uniform block DOES contain
    // must still force a repack, or dead-store elimination would render with
    // stale uniforms. This is the failure mode the optimisation could cause.
    const beforeFactor = fake.n.writeBuffer;
    const factorFrame = [
        command(OP_BEGIN_SCENE, u32Payload(DEVICE, 0)),
        command(OP_DRAW_PRIMITIVE, u32Payload(DEVICE, 4, 0, 1)),
        command(OP_SET_RENDER_STATE,
            u32Payload(DEVICE, D3DRS_TEXTUREFACTOR, 0xFF00FF00, 0)),
        command(OP_DRAW_PRIMITIVE, u32Payload(DEVICE, 4, 0, 1)),
        command(OP_END_SCENE, u32Payload(DEVICE, 0)),
        command(OP_PRESENT, surfacePayload(DEVICE, 640, 480)),
    ];
    await executor.submit(batch(factorFrame, 100), {});
    assert.equal(executor.failed, null);
    assert.equal(fake.n.writeBuffer - beforeFactor, 2,
        "changing D3DRS_TEXTUREFACTOR must repack the uniform block: it is " +
        "read by the fixed-function shader");

    // --- second scenario: uniform state that genuinely changes every draw.
    // This is what proves the dynamic-offset design: the uniform slot moves
    // per draw, so if the slot offset (or the uniform buffer identity) were
    // part of bind group identity, bind group creation would scale with the
    // draw count instead of staying at zero.
    const varyingFrame = (frameIndex) => {
        const commands = [command(OP_BEGIN_SCENE, u32Payload(DEVICE, 0))];
        for (let i = 0; i < DRAWS; i++) {
            commands.push(command(OP_SET_RENDER_STATE, u32Payload(DEVICE,
                D3DRS_TEXTUREFACTOR, (frameIndex * 977 + i * 131) >>> 0, 0)));
            commands.push(command(OP_DRAW_PRIMITIVE,
                u32Payload(DEVICE, 4, 0, 1)));
        }
        commands.push(command(OP_END_SCENE, u32Payload(DEVICE, 0)));
        commands.push(command(OP_PRESENT, surfacePayload(DEVICE, 640, 480)));
        return commands;
    };
    await executor.submit(batch(varyingFrame(1), 200), {});
    const beforeVarying = { ...fake.n };
    const VARYING_FRAMES = 5;
    for (let f = 0; f < VARYING_FRAMES; f++) {
        await executor.submit(batch(varyingFrame(2 + f), 201 + f), {});
        assert.equal(executor.failed, null);
    }
    const varyingDelta = key => fake.n[key] - beforeVarying[key];
    assert.equal(varyingDelta("createBindGroup"), 0,
        "bind group creation must stay at 0/frame even when the uniform " +
        "block changes on every draw: the slot is reached by dynamic offset " +
        "and must not appear in bind group identity");
    assert.equal(varyingDelta("createBuffer"), 0,
        "per-draw uniform changes must come out of the ring, not new buffers");
    assert.equal(varyingDelta("writeBuffer"), VARYING_FRAMES * DRAWS,
        "each genuinely distinct uniform state needs exactly one upload");
    const distinctOffsets = new Set(
        fake.n.dynamicOffsets.slice(-DRAWS));
    assert.ok(distinctOffsets.size > 1,
        "per-draw uniform changes must land at distinct ring offsets");
    // The strongest form of the invariant: the bind group cache must stay
    // proportional to the number of distinct (texture, sampler) combinations,
    // NOT to the number of distinct uniform states. Per-frame creation counts
    // alone cannot catch a uniform-in-key regression, because the ring cursor
    // restarts each frame and so the same offsets recur and simply refill a
    // large cache once. Cache occupancy does catch it.
    assert.ok(executor.getStats().bindGroupsCached <= 4,
        "bind group cache must not scale with uniform state; " +
        DRAWS + " distinct per-draw uniform states produced " +
        executor.getStats().bindGroupsCached + " cached bind groups");

    // And a genuinely redundant write must be dropped rather than repacked.
    const beforeRedundant = fake.n.writeBuffer;
    await executor.submit(batch([
        command(OP_BEGIN_SCENE, u32Payload(DEVICE, 0)),
        command(OP_SET_RENDER_STATE,
            u32Payload(DEVICE, D3DRS_TEXTUREFACTOR, 0xFF00FF00, 0)),
        command(OP_DRAW_PRIMITIVE, u32Payload(DEVICE, 4, 0, 1)),
        command(OP_END_SCENE, u32Payload(DEVICE, 0)),
        command(OP_PRESENT, surfacePayload(DEVICE, 640, 480)),
    ], 101), {});
    assert.equal(fake.n.writeBuffer - beforeRedundant, 1,
        "re-setting a render state to its current value must not repack");

    console.log("d3d8_webgpu_perf_test: ok");
    console.log("  steady state per frame: " +
        (delta("createBuffer") / FRAMES) + " buffers, " +
        (delta("createBindGroup") / FRAMES) + " bind groups, " +
        (delta("createPipeline") / FRAMES) + " pipelines, " +
        (delta("writeBuffer") / FRAMES) + " uniform uploads (" +
        (delta("writeBufferBytes") / FRAMES) + " bytes), " +
        (delta("renderPass") / FRAMES) + " pass, " +
        (delta("submit") / FRAMES) + " submit, for " + DRAWS + " draws");
}

main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
