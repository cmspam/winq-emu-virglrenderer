# WINQ-EMU virglrenderer

**[Project Page](https://cmspam.github.io/winq-emu/)** | **[Download](https://github.com/cmspam/winq-emu/releases)**

A fork of [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) with a complete Windows port of the **Venus Vulkan forwarding protocol**, enabling Linux VMs to access the host GPU for hardware-accelerated Vulkan rendering via virtio-gpu.

## What's Changed

All changes are in the `winq-emu-alpha1` branch, applied as a single commit on top of upstream `main`.

### Windows Venus Port
- **Win32 external memory shim**: Translates POSIX fd-based memory/fence/semaphore operations to Win32 handle equivalents (`OPAQUE_FD` -> `OPAQUE_WIN32`)
- **Buffer and image creation**: Remaps external memory handle types in creation info chains for Windows compatibility
- **Proxy server threading**: Replaces Unix fork-based server with Windows in-process threading model using Win32 synchronization primitives
- **Ring monitor**: Windows-compatible futex emulation using `WaitOnAddress`/`WakeByAddressSingle`

### Stability Fixes
- **Push descriptor layout fix**: Strips `VkDescriptorSetLayoutBindingFlagsCreateInfo` from push descriptor DSLs to prevent driver crashes on Windows
- **Deferred object destruction**: Delays destruction of lightweight Vulkan objects (descriptor set layouts, pipelines, render passes, etc.) until device teardown to prevent handle reuse issues
- **GL error drain**: Handles `GL_INVALID_ENUM` from `GL_SMOOTH_POINT_SIZE_RANGE`/`GL_SMOOTH_LINE_WIDTH_RANGE` on Windows GL drivers (via ANGLE)
- **Queue creation fix**: Corrected use-after-free in queue info allocation

## Building

### Requirements (MSYS2 UCRT64)

```bash
pacman -S mingw-w64-ucrt64-x86_64-meson mingw-w64-ucrt64-x86_64-ninja \
          mingw-w64-ucrt64-x86_64-gcc mingw-w64-ucrt64-x86_64-pkg-config \
          mingw-w64-ucrt64-x86_64-libepoxy mingw-w64-ucrt64-x86_64-vulkan-headers \
          mingw-w64-ucrt64-x86_64-vulkan-loader mingw-w64-ucrt64-x86_64-angleproject
```

### Build

```bash
meson setup builddir --prefix=/ucrt64
ninja -C builddir
```

The output is `builddir/src/libvirglrenderer-1.dll`.

## Syncing with Upstream

This fork tracks [virglrenderer upstream](https://gitlab.freedesktop.org/virgl/virglrenderer). To sync:

```bash
git remote add upstream https://gitlab.freedesktop.org/virgl/virglrenderer.git
git fetch upstream
git checkout main
git merge upstream/main
git checkout winq-emu-alpha1
git rebase main
```

## Related Projects

- [winq-emu-qemu](https://github.com/cmspam/winq-emu-qemu) - QEMU fork with WHPX and Venus support
- [winq-emu](https://github.com/cmspam/winq-emu) - Installer, launcher, and project page

## License

MIT License (same as upstream virglrenderer)

## Contributing

This is an alpha release. Anyone is welcome to look at, modify, and/or merge these changes into upstream projects.
