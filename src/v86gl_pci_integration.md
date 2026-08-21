# v86gl PCI DMA command-buffer integration

## 1. Device integration

`v86gl_pci.js` is imported by `cpu.js` and is created whenever the emulator
option `v86gl_pci` is truthy. The default BDF is `00:13.0`; `00:12.0` is
already occupied by v86's VGA device.

## 2. Enable the device

Pass the option when constructing the browser emulator:

```js
const emulator = new V86({
    v86gl_pci: {
        port: 0xF100,
        maxBatchBytes: 16 * 1024 * 1024,
    },
    net_device: { type: "none" },
});
```

v86's PCI framework uses `pci_space`, `pci_id`, and `pci_bars`; I/O BAR entries
are captured from `cpu.io.ports`, so the device registers its I/O ports before
calling `pci.register_device(this)`.

## 3. Connect to the existing OpenGL bridge

`V86GLNetworkBridge` subscribes to `v86gl-pci-frame` and sends
`event.bytes` directly to `executeGLCommands`. It does not subscribe to
`net0-send`.

## 4. Guest side

The XP `v86gl.sys` helper allocates physically contiguous guest RAM and maps
it into the proxy process. The proxy writes this memory layout:

```c
V86GLDMADesc header;
uint8_t command_stream[command_bytes];
```

The command stream is exactly your existing batch command format:

```text
[fn:u16][payload_size:u16][payload bytes] ...
```

The driver then writes:

```c
outl(io_base + V86GL_REG_DESC_HI, 0);
outl(io_base + V86GL_REG_DESC_LO, desc_phys);
outl(io_base + V86GL_REG_DESC_LEN, desc_len);
outl(io_base + V86GL_REG_COMMAND, V86GL_CMD_SUBMIT | V86GL_CMD_FORCE_PRESENT);
```

On Windows XP, `opengl32.dll` must not execute `outl` directly. The helper
driver exposes `\\.\v86gl`, maps the contiguous buffer with `MAP_BUFFER`, and
performs the I/O BAR writes in response to `SUBMIT`.
