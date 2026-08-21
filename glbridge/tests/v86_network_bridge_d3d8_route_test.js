"use strict";

const assert = require("node:assert/strict");

require("../v86_network_bridge.js");

const listeners = Object.create(null);
const routed = [];
let d3d8Options;
const style = () => ({
    setProperty(name, value) { this[name] = value; },
});
const screenCanvas = {
    width: 1024,
    height: 768,
    getBoundingClientRect() { return { left: 0, top: 0, width: 1024, height: 768 }; },
};
const d3d8Canvas = { width: 1, height: 1, style: style() };
const canvas = {
    width: 64,
    height: 64,
    style: style(),
    parentElement: {
        getElementsByTagName() { return [screenCanvas, canvas, d3d8Canvas]; },
        getBoundingClientRect() { return { left: 0, top: 0, width: 1024, height: 768 }; },
    },
};
const gl4es = {
    HEAPU8: new Uint8Array(4096),
    _malloc() { return 256; },
    _free() {},
    _v86glResize() {},
};
const bridge = globalThis.installV86GLNetworkBridge({
    add_listener(name, callback) { listeners[name] = callback; },
}, canvas, {
    gl4es,
    d3d8Canvas,
    installD3D8WebGPUExecutor(installedCanvas, options) {
        assert.equal(installedCanvas, d3d8Canvas);
        d3d8Options = options;
        return {
            submit(bytes, metadata) { routed.push({ bytes: Buffer.from(bytes), metadata }); },
        };
    },
});

bridge.lastPresentedFrameId = 999;
const d8wg = Buffer.alloc(32);
d8wg.writeUInt32LE(0x47573844, 0);
d8wg.writeUInt16LE(1, 4);
d8wg.writeUInt16LE(7, 6);
d8wg.writeUInt32LE(0xA0010001, 24);
d8wg.writeUInt32LE(0x20260802, 28);
const envelope = Buffer.alloc(8 + d8wg.length);
envelope.writeUInt16LE(0xFFE0, 0);
envelope.writeUInt16LE(0xFFFF, 2);
envelope.writeUInt32LE(d8wg.length, 4);
d8wg.copy(envelope, 8);

listeners["v86gl-pci-frame"]({
    bytes: envelope,
    frameId: 1,
    submitCount: 7,
    commandCount: 1,
    flags: 0,
});

assert.equal(routed.length, 1,
    "D8WG routing must not be rejected by the OpenGL stale-frame counter");
assert.deepEqual(routed[0].bytes, d8wg);
assert.deepEqual(routed[0].metadata, {
    pciFrameId: 1,
    submitCount: 7,
    descriptorCommandCount: 1,
});

d3d8Options.onSurface({ hwnd: 0x1234, x: 10, y: 20, width: 640,
    height: 480, displayWidth: 640, displayHeight: 480, visible: true }, "create");
d3d8Options.onPresent({ hwnd: 0x1234, x: 10, y: 20, width: 640,
    height: 480, displayWidth: 640, displayHeight: 480, visible: true }, {});
assert.equal(d3d8Canvas.style.display, "block");
assert.equal(d3d8Canvas.style.visibility, "visible");
assert.equal(canvas.style.display, "none");

d3d8Options.onSurface({ hwnd: 0x1234, x: 30, y: 40, width: 640,
    height: 480, displayWidth: 800, displayHeight: 600, visible: true }, "move");
assert.equal(d3d8Canvas.style.left, "30px");
assert.equal(d3d8Canvas.style.top, "40px");
assert.equal(d3d8Canvas.style.width, "800px");
assert.equal(d3d8Canvas.style.height, "600px");

d3d8Options.onSurface({ hwnd: 0x1234, x: 0, y: 0, width: 640,
    height: 480, displayWidth: 800, displayHeight: 600, visible: false }, "hide");
assert.equal(d3d8Canvas.style.display, "none");
assert.equal(d3d8Canvas.style.visibility, "hidden");
d3d8Options.onPresent({ sessionKey: "new-session", hwnd: 0x1234,
    x: 30, y: 40, width: 640, height: 480, displayWidth: 800,
    displayHeight: 600, visible: true }, {});
assert.equal(d3d8Canvas.style.display, "block");

d3d8Options.onDestroy({ sessionKey: "old-session", hwnd: 0x1234,
    x: 30, y: 40, width: 640, height: 480, displayWidth: 800,
    displayHeight: 600, visible: true }, "device");
assert.equal(d3d8Canvas.style.display, "block",
    "late teardown from an old process session must not hide the new owner");
d3d8Options.onDestroy({ sessionKey: "new-session", hwnd: 0x1234,
    x: 30, y: 40, width: 640, height: 480, displayWidth: 800,
    displayHeight: 600, visible: true }, "device");
assert.equal(d3d8Canvas.style.display, "none");
assert.equal(d3d8Canvas.style.visibility, "hidden");

bridge.showOverlayCanvas();
assert.equal(d3d8Canvas.style.display, "none");
assert.equal(canvas.style.display, "block");

console.log("v86_network_bridge_d3d8_route_test: ok");
