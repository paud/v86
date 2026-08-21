# v86gl VxD - Windows 95/98/Me 驱动

## 与 XP 驱动的区别

| | Windows XP/2000 (`v86gl.sys`) | Windows 95/98/Me (`v86gl.vxd`) |
|---|---|---|
| 驱动模型 | WDM (内核模式) | VxD (虚拟设备驱动) |
| I/O 端口访问 | 必须通过内核驱动 | **用户态可直接 IN/OUT** (IOPL=3) |
| 物理内存分配 | `MmAllocateContiguousMemory` | `_PageAllocate` (VMM 服务) |
| 用户态接口 | `\\.\v86gl` + IOCTL | `\\.\v86gl` + IOCTL (相同) |

## 架构

```
游戏/应用
   │
   ▼
opengl32.dll (代理)
   │  ① MAP_BUFFER IOCTL → 获取 16MB 连续物理内存的线性地址
   │  ② 写入 GL 命令到共享缓冲区
   │  ③ SUBMIT IOCTL → VxD 写 PCI I/O BAR (0xF100)
   │     (或 opengl32.dll 直接 OUT 指令，因为 Win9x IOPL=3)
   ▼
v86gl.vxd
   │  分配 16MB 物理连续内存
   │  写 DESC_LO/DESC_LEN/COMMAND 到 PCI 端口
   ▼
v86 虚拟 PCI 设备 (00:13.0, I/O 0xF100)
   │
   ▼
浏览器端 v86_network_bridge.js → gl4es → WebGL
```

## 编译

需要 Windows 98 DDK + MASM 6.11：

```cmd
cd C:\v86gl_vxd
nmake
```

产出 `v86gl.vxd`。

## 安装

### 方法 1：注册表加载（推荐）

把 `v86gl.vxd` 复制到 `C:\Windows\System\`，然后运行：

```cmd
reg add "HKLM\System\CurrentControlSet\Services\VxD\v86gl" /v StaticVxD /d v86gl.vxd /f
reg add "HKLM\System\CurrentControlSet\Services\VxD\v86gl" /v Start /t REG_DWORD /d 0 /f
```

重启后驱动自动加载。

### 方法 2：System.ini

把 `v86gl.vxd` 复制到 `C:\Windows\System\`，编辑 `C:\Windows\System.ini`：

```ini
[386Enh]
device=v86gl.vxd
```

重启。

## 验证

驱动加载后，`opengl32.dll` 代理通过 `\\.\v86gl` 打开设备：

```c
HANDLE h = CreateFile("\\\\.\\v86gl", GENERIC_READ|GENERIC_WRITE,
                      0, NULL, OPEN_EXISTING, 0, NULL);
```

如果成功，说明 VxD 已加载。

## IOCTL 接口

与 XP 驱动完全相同（`v86gl_ioctl.h`）：

| IOCTL | 输入 | 输出 | 说明 |
|-------|------|------|------|
| `MAP_BUFFER` | — | `{user_address, buffer_bytes}` | 获取共享缓冲区地址 |
| `SUBMIT` | `{descriptor_bytes, flags}` | — | 提交命令缓冲区到 PCI |
| `UNMAP_BUFFER` | — | — | 释放映射 |

## 注意事项

- Windows 9x 用户态程序可以直接执行 `IN`/`OUT` 指令，所以即使没有 VxD，
  `opengl32.dll` 也可以直接写 PCI 端口。VxD 的唯一作用是分配物理连续内存。
- 16 MB 缓冲区在系统启动时分配，确保物理地址低于 4 GB。
- 如果 VxD 加载失败，`opengl32.dll` 可以回退到 `GlobalAlloc` 分配的内存，
  但物理地址不连续，可能导致 PCI DMA 读取错误。
