# Minimal ddraw.dll proxy for dxdiag

This is a deliberately tiny fake `ddraw.dll` for Windows XP/9x guests in v86.

## Why it exists

`dxdiag.exe` calls `DirectDrawCreate` to enumerate the primary display adapter,
then queries `IDirectDraw7::GetDeviceIdentifier` and `IDirectDraw7::GetCaps`
to fill its Display tab. The real system `ddraw.dll` talks to the VBE/Bochs VGA
driver, which does not support DirectDraw acceleration, so dxdiag either
crashes or reports "No DirectDraw acceleration available".

This proxy returns an honest device identity (matching the d3d9 proxy's
default NVIDIA GeForce FX 5200 so dxdiag's DirectDraw and Direct3D tabs agree)
and conservative caps that say "no 3D acceleration" rather than lying about
capabilities the rest of the glbridge stack does not route through ddraw.dll.

It deliberately does not implement `CreateSurface`/`Blt`/`Flip` - those return
`DDERR_UNSUPPORTED`, which dxdiag handles gracefully.

## What it supports

- `DirectDrawCreate` / `DirectDrawCreateEx`
- `DirectDrawEnumerateA`/`W` / `DirectDrawEnumerateExA`/`W`
- `DirectDrawCreateClipper` (returns `DDERR_UNSUPPORTED`)
- `IDirectDraw7` COM vtable (fully populated):
  - `QueryInterface` / `AddRef` / `Release`
  - `GetDeviceIdentifier` - returns the NVIDIA GeForce FX 5200 identity
  - `GetCaps` - honest caps: no 3D acceleration, 16 MB video memory
  - `GetDisplayMode` - 640x480x16@60
  - `EnumDisplayModes` - one 640x480x16@60 mode
  - `SetCooperativeLevel` / `TestCooperativeLevel` - succeed
  - All other methods return `DDERR_UNSUPPORTED` or `DD_OK` as appropriate

## Build

```sh
./glbridge/ddrawproxy/build.sh /private/tmp/ddraw.dll
```

The build enforces `-Wall -Wextra -Werror` and asserts the output imports
only `KERNEL32.dll` - no MSVCRT/UCRT dependency, matching the d3d8/d3d9
proxies' XP-compatibility requirement.

Install the resulting DLL beside the target executable (or in `C:\Windows\System32`
for system-wide dxdiag support). Use a separate game deployment profile from
`d3d8.dll`/`d3d9.dll`/`opengl32.dll` proxy: all four frontends share the same
app-local proxy discipline.

### Automated installation

`tools/install.bat` (packaged onto the `v86gl_install.iso` CD by
`tools/make_install_iso.mjs`) copies `opengl32.dll` and `ddraw.dll` to the
correct system directory and backs up the originals on NT/2000/XP.  Run it from
the guest after mounting the CD:

```cmd
D:\INSTALL.BAT
```

(replace `D:` with the guest's CD-ROM drive letter).  Reboot the guest
afterwards for the new DLLs to take effect.
