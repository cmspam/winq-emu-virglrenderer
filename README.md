# WINQ-EMU virglrenderer

**[Project Page](https://cmspam.github.io/winq-emu/)** | **[Download](https://github.com/cmspam/winq-emu/releases)**

A fork of [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) with a complete Windows port of the **Venus Vulkan forwarding protocol** plus a **D3D11/Media Foundation video decode backend**, enabling Linux VMs to access the host GPU for hardware-accelerated Vulkan rendering and hardware video decode via virtio-gpu.

## What's Changed

All changes are in the `winq-emu-alpha8` branch, applied as a series of commits on top of upstream tag `virglrenderer-1.3.0`.

### Windows Venus Port
- **Win32 external memory shim**: Translates POSIX fd-based memory/fence/semaphore operations to Win32 handle equivalents (`OPAQUE_FD` -> `OPAQUE_WIN32`)
- **Buffer and image creation**: Remaps external memory handle types in creation info chains for Windows compatibility
- **Proxy server threading**: Replaces Unix fork-based server with Windows in-process threading model using Win32 synchronization primitives
- **Ring monitor**: Windows-compatible futex emulation using `WaitOnAddress`/`WakeByAddressSingle`
- **Windows dma-buf shim**: Synthesizes `VK_EXT_external_memory_dma_buf` and `VK_EXT_image_drm_format_modifier` on top of the host Vulkan ICD so guest Wayland compositors and Zink can import Vulkan-allocated surfaces

### Windows D3D11 Video Decode Backend
- **`virgl_video_win32.c`**: New backend (replaces the libva path on Windows) that runs guest VA-API decode requests through `ID3D11VideoDecoder` (DXVA), feeding decoded planes back to the guest via virtio-gpu resources
- Supports H.264 (Baseline / Main / High), HEVC (Main, Main10), VP9 (Profile 0, Profile 2), and AV1 (Main Profile 0)

### Stability Fixes
- **Push descriptor layout fix**: Strips `VkDescriptorSetLayoutBindingFlagsCreateInfo` from push descriptor DSLs to prevent driver crashes on Windows
- **Deferred object destruction**: Delays destruction of lightweight Vulkan objects (descriptor set layouts, pipelines, render passes, etc.) until device teardown to prevent handle reuse issues
- **GL error drain**: Handles `GL_INVALID_ENUM` from `GL_SMOOTH_POINT_SIZE_RANGE`/`GL_SMOOTH_LINE_WIDTH_RANGE` on Windows GL drivers (via ANGLE)
- **Queue creation fix**: Corrected use-after-free in queue info allocation

## Building

### Requirements (MSYS2 UCRT64)

All builds must be done from the MSYS2 UCRT64 shell (not MINGW64 or MSYS).

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-meson \
          mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkg-config \
          mingw-w64-ucrt-x86_64-libepoxy mingw-w64-ucrt-x86_64-vulkan-headers \
          mingw-w64-ucrt-x86_64-vulkan-loader mingw-w64-ucrt-x86_64-python \
          mingw-w64-ucrt-x86_64-python-yaml
```

**Important**: The `mingw-w64-ucrt-x86_64-python-yaml` package is required — the build uses Python YAML processing for gallium format table generation and will fail at configure time without it.

The D3D11 / DXGI / Media Foundation libraries are part of the standard MSYS2 UCRT64 toolchain and resolve via the compiler's `-l<name>` lookup; no extra package is needed.

### Build

```bash
meson setup builddir --prefix=/ucrt64 -Dvenus=true -Dvideo=true -Dtests=false
ninja -C builddir
```

`-Dvideo=true` is required to enable the Windows D3D11 video decode backend. Without it, hardware video decode (and the related dma-buf / format-modifier paths used by Zink-on-Wayland) will not be linked in.

The output is `builddir/src/libvirglrenderer-1.dll`. It will import `d3d11.dll`, `MFPlat.DLL`, `ole32.dll`, etc. — verify with `objdump -p builddir/src/libvirglrenderer-1.dll | grep "DLL Name"`.

### Install

To make the library available to QEMU during its build:

```bash
ninja -C builddir install
```

This installs `libvirglrenderer-1.dll` to `/ucrt64/bin/` and the pkg-config file to `/ucrt64/lib/pkgconfig/`.

## Syncing with Upstream

This fork tracks tagged releases of [virglrenderer upstream](https://gitlab.freedesktop.org/virgl/virglrenderer). The custom changes are a small series of commits on top of an upstream release tag, so rebasing onto a newer tag is straightforward:

```bash
git remote add upstream https://gitlab.freedesktop.org/virgl/virglrenderer.git
git fetch upstream tag virglrenderer-1.3.0 --no-tags
git rebase --onto virglrenderer-1.3.0 <previous-tag> HEAD
```

## Related Projects

- [winq-emu-qemu](https://github.com/cmspam/winq-emu-qemu) - QEMU fork with WHPX and Venus support
- [winq-emu](https://github.com/cmspam/winq-emu) - Installer, launcher, and project page

## License

MIT License (same as upstream virglrenderer)

## Contributing

This is an alpha release. Anyone is welcome to look at, modify, and/or merge these changes into upstream projects.
