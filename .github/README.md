# QEMU with a control menu in the SDL window

QEMU master with a menu bar inside the SDL display window, in the spirit of
[xemu](https://github.com/xemu-project/xemu): power, pause, snapshots, USB
passthrough, usage statistics and VM information, without giving up the fast
SDL + OpenGL display. It also makes the SDL display follow the host monitor's
refresh rate.

This is a fork of [QEMU](https://www.qemu.org): upstream master plus two
commits. The menu needs a Linux host.

## What's added

- **ui/sdl2: follow the host refresh rate, tolerate grab border**
  - The guest is told the refresh rate of the monitor showing the window
    (e.g. 240 Hz), and input is polled at that rate.
  - In absolute pointer mode, the pointer leaves the window reliably at its
    border.
- **ui/sdl2: add an in-window control menu**
  - **Machine:** pause/resume, send Ctrl+Alt+Del and other key combinations,
    information about the VM, shut down, reset, force off.
  - **Snapshots:** take, revert to and delete snapshots of qcow2 disks.
  - **USB:** pass host USB devices through to the guest and back. If your
    user can't access a device yet, the menu asks polkit for access.
  - **View:** fullscreen, menu bar, statistics, menu size.
  - **Statistics:** CPU, GPU, disk and network use and the frame rate, on the
    right of the menu bar.

## Build on Fedora

### Dependencies

    sudo dnf install gcc gcc-c++ make ninja-build python3 git-core pkgconf bzip2 \
        glib2-devel pixman-devel zlib-ng-compat-devel \
        sdl2-compat-devel libepoxy-devel mesa-libgbm-devel libdrm-devel \
        virglrenderer-devel gtk3-devel libusb1-devel libslirp-devel \
        pulseaudio-libs-devel pipewire-devel spice-protocol \
        libzstd-devel libpng-devel libattr-devel

Fedora's virglrenderer is built without DRM native context
(`virtio-gpu-gl,drm_native_context=on`). For that, build virglrenderer
yourself and point `PKG_CONFIG_PATH` at it before running configure.

### Configure and compile

    git clone https://github.com/tomtomnet/qemu-gui.git
    cd qemu-gui
    mkdir build
    cd build
    ../configure --target-list=x86_64-softmmu
    make -j"$(nproc)"

The first configure downloads [Dear ImGui](https://github.com/ocornut/imgui),
which draws the menu, so it needs an internet connection.

QEMU runs straight from the build directory, no install needed:

    ./qemu-system-x86_64 -display sdl,gl=on ...

To install it to `/usr/local`, run `sudo make install`, or pass
`--prefix=<dir>` to configure first.

### Build faster (optional)

By default, QEMU builds every emulator target and every feature it finds.
For x86-64 VMs you can skip most of it:

- **`--target-list=x86_64-softmmu`** builds only the x86-64 system emulator,
  instead of 66 targets (every system and user-mode emulator). This is the
  biggest saving, and the command above already uses it.
- **`--without-default-features`** turns off every optional feature, so you
  enable only what you use. This line covers a typical desktop VM with KVM,
  virtio-gpu with virgl, USB passthrough, PulseAudio/PipeWire sound, 9p
  shares and user networking:

      ../configure --target-list=x86_64-softmmu --without-default-features \
          --enable-kvm --enable-tcg --enable-pixman --enable-attr --enable-virtfs \
          --enable-hmp --enable-malloc-trim \
          --enable-sdl --enable-sdl-gui --enable-gtk --enable-opengl \
          --enable-virglrenderer \
          --enable-libusb --enable-pa --enable-pipewire --enable-spice-protocol \
          --enable-passt --enable-gio --enable-slirp \
          --enable-tpm --enable-vhost-kernel --enable-vhost-net --enable-vhost-user \
          --enable-zstd --enable-png --enable-tools --enable-fdt=internal \
          --disable-docs

  Drop what you don't need: `--enable-gtk` (GTK display),
  `--enable-pa`/`--enable-pipewire` (sound), `--enable-virtfs` (9p folder
  sharing), `--enable-tools` (qemu-img and friends). If you always use KVM,
  `--disable-tcg` instead of `--enable-tcg` saves a lot more, but then the VM
  can't run without KVM.
- **`--disable-debug-info`** makes compiling and linking faster and the
  binaries much smaller.
- **ccache** (`sudo dnf install ccache`) speeds up rebuilds. QEMU picks it up
  on its own.
- **`make qemu-system-x86_64`** builds only the emulator, without the tools.

## Using the menu

- Use `-display sdl,gl=on` (or `gl=off`).
- **Ctrl+Alt+M** (the SDL hotkey modifier plus M) shows or hides the menu
  bar. While shown, the guest display sits below it. Each VM remembers whether
  its bar was shown, identified by `-name`, else `-uuid`, else its first disk.
- **Snapshots** need qcow2 disks. With `virtio-gpu-gl` (virgl), QEMU can't save
  the VM's memory, so snapshots hold the disks only, and reverting to one
  restarts the VM from them.
- **USB passthrough** needs a USB controller in the VM, e.g.
  `-device qemu-xhci`. It uses `pkexec` (polkit) and `setfacl` to get access
  to a device, until it's unplugged.
- The menu settings are in `~/.config/qemu/sdl-gui.ini`.
- To build without the menu, configure with `--disable-sdl-gui`.

## Notes

- The menu code was written with an AI assistant (Claude). QEMU doesn't
  accept AI-generated contributions
  ([docs/devel/code-provenance.rst](../docs/devel/code-provenance.rst)), so
  these changes are not meant for upstream QEMU.
- License: GPL-2.0-or-later, like QEMU (see [COPYING](../COPYING)). Dear ImGui
  is MIT-licensed and downloaded when you configure.
- QEMU's own README is [README.rst](../README.rst).
