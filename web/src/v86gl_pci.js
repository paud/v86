// v86 OpenGL PCI DMA command-buffer device
// Put this file in v86/src/v86gl_pci.js and instantiate it after PCI is created.
// Protocol: guest writes a command-buffer descriptor into normal guest RAM,
// then writes descriptor physical address and length to the device's I/O BAR.
// The device reads guest RAM once and emits a browser-side event:
//   v86gl-pci-frame -> { frameId, flags, commandCount, bytes }
// The bytes are the existing GL command stream: [fn:u16][size:u16][payload...]*

import { LOG_PCI } from "./const.js";
import { h } from "./lib.js";
import { dbg_log } from "./log.js";

const V86GL_MAGIC = 0x324C4756; // 'VGL2' little-endian
const V86GL_VERSION = 1;

const DEFAULT_PORT = 0xF100;
const DEFAULT_PCI_ID = 0x13 << 3; // bus 0, device 0x13, function 0
const BAR_SIZE = 0x40;

const REG_MAGIC       = 0x00; // ro: 'VGL2'
const REG_VERSION     = 0x04; // ro: 1
const REG_FEATURES    = 0x08; // ro: bit0=descriptor-v1, bit1=event-submit
const REG_STATUS      = 0x0C; // rw: write 1 bits to clear error flags
const REG_DESC_LO     = 0x10; // rw: descriptor physical address low
const REG_DESC_HI     = 0x14; // rw: descriptor physical address high, must be 0 for now
const REG_DESC_LEN    = 0x18; // rw: descriptor total byte length
const REG_COMMAND     = 0x1C; // wo: bit0=submit, bit1=force-present, bit2=reset
const REG_LAST_FRAME  = 0x20; // ro
const REG_LAST_BYTES  = 0x24; // ro
const REG_ERROR       = 0x28; // ro
const REG_MAX_BYTES   = 0x2C; // ro

const CMD_SUBMIT        = 1 << 0;
const CMD_FORCE_PRESENT = 1 << 1;
const CMD_RESET         = 1 << 2;

const DESC_HEADER_SIZE = 32;
const DESC_FLAG_PRESENT = 1 << 0;

const STATUS_READY       = 1 << 0;
const STATUS_ERROR       = 1 << 1;
const STATUS_BAD_ADDR    = 1 << 2;
const STATUS_BAD_SIZE    = 1 << 3;
const STATUS_BAD_MAGIC   = 1 << 4;
const STATUS_BAD_VERSION = 1 << 5;
const STATUS_BUSY        = 1 << 6;
const STATUS_SUBMITTED   = 1 << 7;

const ERR_NONE = 0;
const ERR_BAD_ADDR = 1;
const ERR_BAD_SIZE = 2;
const ERR_BAD_MAGIC = 3;
const ERR_BAD_VERSION = 4;
const ERR_BAD_COMMAND_BYTES = 5;
const ERR_HANDLER = 6;
const ERR_64BIT_ADDR = 7;

function le32(bytes, off) {
    return (bytes[off] | bytes[off + 1] << 8 | bytes[off + 2] << 16 | bytes[off + 3] << 24) >>> 0;
}

function le16(bytes, off) {
    return bytes[off] | bytes[off + 1] << 8;
}

function command_stream_summary(bytes) {
    const preview = [];
    let offset = 0;
    let commandCount = 0;
    let error = null;

    while(offset + 4 <= bytes.length) {
        const fn = le16(bytes, offset);
        let payloadBytes = le16(bytes, offset + 2);
        let headerBytes = 4;

        if(payloadBytes === 0xFFFF) {
            if(offset + 8 > bytes.length) {
                error = "truncated extended record header at byte " + offset;
                break;
            }
            payloadBytes = le32(bytes, offset + 4);
            headerBytes = 8;
        }

        if(payloadBytes > bytes.length - offset - headerBytes) {
            error = "record " + (commandCount + 1) + " fn=" + fn +
                " requires " + payloadBytes + " bytes, only " +
                (bytes.length - offset - headerBytes) + " remain";
            break;
        }

        commandCount++;
        if(preview.length < 16) {
            preview.push({ index: commandCount, fn, payloadBytes });
        }
        offset += headerBytes + payloadBytes;
    }

    if(!error && offset !== bytes.length) {
        error = "trailing " + (bytes.length - offset) + " byte(s) after record " + commandCount;
    }

    return {
        decodedCommands: commandCount,
        consumedBytes: offset,
        error,
        preview,
    };
}

function build_pci_space(port) {
    // Header type 0, vendor-specific PCI device with one I/O BAR.
    // Vendor/device id are intentionally custom so no real Windows driver claims it.
    return [
        0x34, 0x12, 0x86, 0x56, // vendor 0x1234, device 0x5686
        0x01, 0x00, 0x00, 0x00, // command: I/O space enabled; status
        0x01, 0x00, 0x80, 0xFF, // rev, prog-if, subclass, class 0xFF vendor-specific
        0x00, 0x00, 0x00, 0x00, // cache line, latency, header, bist
        (port & 0xFF) | 1, (port >> 8) & 0xFF, 0x00, 0x00, // BAR0 I/O
        0x00, 0x00, 0x00, 0x00, // BAR1
        0x00, 0x00, 0x00, 0x00, // BAR2
        0x00, 0x00, 0x00, 0x00, // BAR3
        0x00, 0x00, 0x00, 0x00, // BAR4
        0x00, 0x00, 0x00, 0x00, // BAR5
        0x00, 0x00, 0x00, 0x00, // cardbus cis
        0x34, 0x12, 0x86, 0x56, // subsystem vendor/id
        0x00, 0x00, 0x00, 0x00, // expansion rom
        0x00, 0x00, 0x00, 0x00, // capabilities ptr + reserved
        0x00, 0x00, 0x00, 0x00, // reserved
        0x00, 0x01, 0x00, 0x00, // irq line, irq pin INTA, min_grant, max_latency
    ];
}

/** @constructor */
export function V86GLPCI(cpu, bus, options) {
    options = options || {};

    this.name = "v86gl pci dma";
    this.cpu = cpu;
    this.bus = bus || cpu.bus;
    this.pci = cpu.devices.pci;

    this.port = options.port || DEFAULT_PORT;
    this.pci_id = options.pci_id || DEFAULT_PCI_ID;
    this.maxBatchBytes = options.maxBatchBytes || (4 * 1024 * 1024);
    this.onSubmit = options.onSubmit || null;
    this.trace = !!options.trace;

    this.descLo = 0;
    this.descHi = 0;
    this.descLen = 0;
    this.status = STATUS_READY;
    this.error = ERR_NONE;
    this.lastFrameId = 0;
    this.lastBytes = 0;
    this.submitCount = 0;

    this.pci_space = build_pci_space(this.port);
    // The XP transport driver is a legacy kernel service, not a PnP driver.
    // It uses the firmware-assigned port directly, so its BAR must not relocate.
    this.pci_bars = [{ size: BAR_SIZE, fixed: true }];

    this.register_io();
    this.pci.register_device(this);

    dbg_log("v86gl pci dma registered port=" + h(this.port, 4) +
            " bdf=" + h(this.pci_id, 4), LOG_PCI);
}

V86GLPCI.prototype.register_io = function() {
    const io = this.cpu.io;
    const device = this;
    const regs = [
        REG_MAGIC, REG_VERSION, REG_FEATURES, REG_STATUS,
        REG_DESC_LO, REG_DESC_HI, REG_DESC_LEN, REG_COMMAND,
        REG_LAST_FRAME, REG_LAST_BYTES, REG_ERROR, REG_MAX_BYTES,
    ];

    for(const off of regs) {
        io.register_read(this.port + off, this,
            undefined,
            undefined,
            function() {
                return device.read_reg32(off);
            }
        );

        io.register_write(this.port + off, this,
            undefined,
            undefined,
            function(value) {
                device.write_reg32(off, value >>> 0);
            }
        );
    }
};

V86GLPCI.prototype.read_reg32 = function(off) {
    switch(off) {
    case REG_MAGIC:      return V86GL_MAGIC | 0;
    case REG_VERSION:    return V86GL_VERSION | 0;
    case REG_FEATURES:   return 0x00000003; // descriptor-v1 + event-submit
    case REG_STATUS:     return this.status | 0;
    case REG_DESC_LO:    return this.descLo | 0;
    case REG_DESC_HI:    return this.descHi | 0;
    case REG_DESC_LEN:   return this.descLen | 0;
    case REG_LAST_FRAME: return this.lastFrameId | 0;
    case REG_LAST_BYTES: return this.lastBytes | 0;
    case REG_ERROR:      return this.error | 0;
    case REG_MAX_BYTES:  return this.maxBatchBytes | 0;
    default:             return -1;
    }
};

V86GLPCI.prototype.write_reg32 = function(off, value) {
    switch(off) {
    case REG_STATUS: {
        // write-1-to-clear error/submitted flags
        const previousStatus = this.status;
        this.status &= ~value;
        this.status |= STATUS_READY;
        if(!(this.status & STATUS_ERROR)) {
            this.error = ERR_NONE;
        }
        this.trace_log("status acknowledge", {
            clearMask: h(value >>> 0, 8),
            previousStatus: h(previousStatus >>> 0, 8),
            status: h(this.status >>> 0, 8),
            error: this.error,
        });
        break;
    }
    case REG_DESC_LO:
        this.descLo = value >>> 0;
        this.trace_log("descriptor address low", { descAddr: h(this.descLo, 8) });
        break;
    case REG_DESC_HI:
        this.descHi = value >>> 0;
        this.trace_log("descriptor address high", { descHi: h(this.descHi, 8) });
        if(this.descHi) {
            this.fail(ERR_64BIT_ADDR, STATUS_BAD_ADDR);
        }
        break;
    case REG_DESC_LEN:
        this.descLen = value >>> 0;
        this.trace_log("descriptor length", { descLen: this.descLen });
        break;
    case REG_COMMAND:
        this.trace_log("doorbell", {
            command: value >>> 0,
            descAddr: this.descLo >>> 0,
            descLen: this.descLen >>> 0,
        });
        if(value & CMD_RESET) {
            this.reset();
        }
        if(value & CMD_SUBMIT) {
            this.submit(value >>> 0);
        }
        break;
    default:
        break;
    }
};

V86GLPCI.prototype.reset = function() {
    this.descLo = 0;
    this.descHi = 0;
    this.descLen = 0;
    this.status = STATUS_READY;
    this.error = ERR_NONE;
    this.lastFrameId = 0;
    this.lastBytes = 0;
    this.trace_log("reset completed", {});
};

V86GLPCI.prototype.fail = function(error, statusFlag) {
    this.error = error;
    this.status &= ~STATUS_BUSY;
    this.status |= STATUS_READY | STATUS_ERROR | statusFlag;
    dbg_log("v86gl pci dma error=" + error +
            " status=" + h(this.status >>> 0, 8), LOG_PCI);
    this.trace_log("submit failed", {
        error,
        status: this.status >>> 0,
        descAddr: this.descLo >>> 0,
        descLen: this.descLen >>> 0,
    });
};

V86GLPCI.prototype.trace_log = function(message, details) {
    if(this.trace) {
        console.info("[v86gl-pci] " + message, details || "");
    }
};

V86GLPCI.prototype.validate_range = function(addr, len) {
    if(this.descHi) {
        this.fail(ERR_64BIT_ADDR, STATUS_BAD_ADDR);
        return false;
    }
    if(!len || len > this.maxBatchBytes || len < DESC_HEADER_SIZE) {
        this.fail(ERR_BAD_SIZE, STATUS_BAD_SIZE);
        return false;
    }
    const end = (addr + len) >>> 0;
    if(end < addr || end > this.cpu.memory_size[0]) {
        this.fail(ERR_BAD_ADDR, STATUS_BAD_ADDR);
        return false;
    }
    return true;
};

V86GLPCI.prototype.submit = function(command) {
    const addr = this.descLo >>> 0;
    const len = this.descLen >>> 0;
    const submitId = this.submitCount + 1;

    this.trace_log("submit begin", {
        submitCount: submitId,
        command: h(command >>> 0, 8),
        descAddr: h(addr, 8),
        descLen: len,
    });

    if(!this.validate_range(addr, len)) {
        return;
    }

    this.status |= STATUS_BUSY;
    this.status &= ~(STATUS_ERROR | STATUS_SUBMITTED | STATUS_BAD_ADDR | STATUS_BAD_SIZE |
                     STATUS_BAD_MAGIC | STATUS_BAD_VERSION);

    let raw;
    try {
        // Bus listeners run synchronously during the doorbell write, before the guest can reuse this buffer.
        raw = this.cpu.read_blob(addr, len);
    } catch(e) {
        console.warn("[v86gl-pci] read_blob failed", e);
        this.fail(ERR_BAD_ADDR, STATUS_BAD_ADDR);
        return;
    }

    this.trace_log("dma read", {
        descAddr: addr,
        descLen: len,
    });

    const magic = le32(raw, 0);
    const version = le32(raw, 4);
    const flags = le32(raw, 8) | ((command & CMD_FORCE_PRESENT) ? DESC_FLAG_PRESENT : 0);
    const frameId = le32(raw, 12);
    const commandCount = le32(raw, 16);
    const commandBytes = le32(raw, 20);

    this.trace_log("descriptor header", {
        submitCount: submitId,
        magic: h(magic, 8),
        version,
        frameId,
        commandCount,
        commandBytes,
        flags: h(flags >>> 0, 8),
    });

    if(magic !== V86GL_MAGIC) {
        this.fail(ERR_BAD_MAGIC, STATUS_BAD_MAGIC);
        return;
    }
    if(version !== V86GL_VERSION) {
        this.fail(ERR_BAD_VERSION, STATUS_BAD_VERSION);
        return;
    }
    if(commandBytes > len - DESC_HEADER_SIZE) {
        this.fail(ERR_BAD_COMMAND_BYTES, STATUS_BAD_SIZE);
        return;
    }

    const bytes = raw.subarray(DESC_HEADER_SIZE, DESC_HEADER_SIZE + commandBytes);
    const streamSummary = command_stream_summary(bytes);
    this.lastFrameId = frameId >>> 0;
    this.lastBytes = commandBytes >>> 0;
    this.submitCount++;

    this.trace_log("descriptor accepted", {
        frameId: this.lastFrameId,
        commandCount,
        commandBytes: this.lastBytes,
        flags,
        submitCount: this.submitCount,
        stream: streamSummary,
    });

    if(streamSummary.error || streamSummary.decodedCommands !== commandCount) {
        this.trace_log("descriptor command count mismatch", {
            frameId: this.lastFrameId,
            submitCount: this.submitCount,
            descriptorCount: commandCount,
            decodedCount: streamSummary.decodedCommands,
            parseError: streamSummary.error,
        });
    }

    try {
        const event = {
            frameId: this.lastFrameId,
            flags,
            commandCount,
            bytes,
            descAddr: addr,
            descLen: len,
            submitCount: this.submitCount,
        };

        if(this.onSubmit) {
            this.onSubmit(event);
        }
        if(this.bus) {
            this.bus.send("v86gl-pci-frame", event);
        }

        this.trace_log("event emitted", {
            frameId: this.lastFrameId,
            commandBytes: this.lastBytes,
            submitCount: this.submitCount,
        });
    } catch(e) {
        console.warn("[v86gl-pci] submit handler failed", e);
        this.fail(ERR_HANDLER, STATUS_ERROR);
        return;
    }

    this.status &= ~STATUS_BUSY;
    this.status |= STATUS_READY | STATUS_SUBMITTED;
    this.trace_log("submit complete", {
        frameId: this.lastFrameId,
        submitCount: this.submitCount,
        status: h(this.status >>> 0, 8),
        error: this.error,
    });
};

V86GLPCI.prototype.get_state = function() {
    return [
        this.descLo, this.descHi, this.descLen, this.status, this.error,
        this.lastFrameId, this.lastBytes, this.submitCount,
    ];
};

V86GLPCI.prototype.set_state = function(state) {
    if(!state) return;
    this.descLo = state[0] >>> 0;
    this.descHi = state[1] >>> 0;
    this.descLen = state[2] >>> 0;
    this.status = state[3] >>> 0;
    this.error = state[4] >>> 0;
    this.lastFrameId = state[5] >>> 0;
    this.lastBytes = state[6] >>> 0;
    this.submitCount = state[7] >>> 0;
};

export const V86GL_PCI_REGS = {
    REG_MAGIC,
    REG_VERSION,
    REG_FEATURES,
    REG_STATUS,
    REG_DESC_LO,
    REG_DESC_HI,
    REG_DESC_LEN,
    REG_COMMAND,
    REG_LAST_FRAME,
    REG_LAST_BYTES,
    REG_ERROR,
    REG_MAX_BYTES,
    CMD_SUBMIT,
    CMD_FORCE_PRESENT,
    CMD_RESET,
    DESC_HEADER_SIZE,
    DESC_FLAG_PRESENT,
};
