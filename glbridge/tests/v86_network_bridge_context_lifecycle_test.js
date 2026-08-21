"use strict";

const assert = require("node:assert/strict");

require("../v86_network_bridge.js");

const V86GL_CTRL_DESTROY_CONTEXT = 0xFFF2;

function glRecord(fn, payload = Buffer.alloc(0)) {
    const record = Buffer.alloc(4 + payload.length);
    record.writeUInt16LE(fn, 0);
    record.writeUInt16LE(payload.length, 2);
    payload.copy(record, 4);
    return record;
}

function makeModule(name, calls) {
    return {
        name,
        HEAPU8: new Uint8Array(4096),
        _malloc() { return 256; },
        _free() {},
        _v86glResize(width, height) {
            calls.push([name, "resize", width, height]);
        },
        _v86glDestroyRenderer() {
            calls.push([name, "destroy"]);
        },
    };
}

async function main() {
    const calls = [];
    const listeners = Object.create(null);
    let running = true;
    let resolveStop;
    let resolveFresh;
    const freshModule = makeModule("fresh", calls);
    const emulator = {
        add_listener(name, listener) {
            listeners[name] = listener;
        },
        is_running() {
            return running;
        },
        stop() {
            calls.push(["emulator", "stop"]);
            running = false;
            return new Promise(resolve => {
                resolveStop = resolve;
            });
        },
        run() {
            calls.push(["emulator", "run"]);
            running = true;
            return Promise.resolve();
        },
    };
    const canvas = {
        width: 640,
        height: 480,
        style: { setProperty() {} },
        parentElement: null,
    };
    const bridge = globalThis.installV86GLNetworkBridge(emulator, canvas, {
        gl4es: makeModule("initial", calls),
        resetGL4ESRenderer() {
            calls.push(["factory", "start"]);
            return new Promise(resolve => {
                resolveFresh = () => resolve(freshModule);
            });
        },
    });

    listeners["v86gl-pci-frame"]({
        bytes: glRecord(V86GL_CTRL_DESTROY_CONTEXT),
        frameId: 0,
        submitCount: 1,
        commandCount: 1,
        flags: 0,
    });

    assert.equal(running, false,
        "v86 must stop before the asynchronous replacement is ready");
    assert.equal(bridge.renderer.module.name, "initial",
        "the renderer must remain installed until the CPU slice has stopped");
    assert.equal(bridge.rendererRebuildInProgress, true);
    assert.deepEqual(calls.slice(1), [["emulator", "stop"]],
        "renderer teardown must wait for the real stop promise");
    assert.equal(calls.some(call => call[1] === "run"), false,
        "v86 must remain stopped while gl4es is unresolved");

    resolveStop();
    await Promise.resolve();
    assert.equal(bridge.renderer, null,
        "the stopped VM must not issue calls while the replacement is pending");
    assert.deepEqual(calls.slice(2, 4), [
        ["initial", "destroy"],
        ["factory", "start"],
    ]);

    resolveFresh();
    await bridge.contextRebuildPromise;

    assert.equal(bridge.renderer.module, freshModule);
    assert.equal(bridge.rendererRebuildInProgress, false);
    assert.equal(running, true,
        "v86 must resume after the replacement renderer is installed");
    const resizeIndex = calls.findIndex(call => call[0] === "fresh" && call[1] === "resize");
    const runIndex = calls.findIndex(call => call[0] === "emulator" && call[1] === "run");
    assert.ok(resizeIndex >= 0 && runIndex > resizeIndex,
        "the fresh renderer must be ready before v86 resumes");

    const factoryCount = calls.filter(call => call[0] === "factory").length;
    bridge.destroyContext();
    assert.equal(calls.filter(call => call[0] === "factory").length, factoryCount,
        "a duplicate destroy notification must not start a second rebuild");

    console.log("v86_network_bridge_context_lifecycle_test: ok");
}

main().catch(err => {
    console.error(err);
    process.exitCode = 1;
});
