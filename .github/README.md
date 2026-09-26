# QEMU with a control menu in the SDL window

QEMU master with a menu bar inside the SDL display window, in the spirit of
[xemu](https://github.com/xemu-project/xemu): power, pause, snapshots, USB
passthrough, usage statistics and VM information, without giving up the fast
SDL + OpenGL display. It also makes the SDL display follow the host monitor's
refresh rate.

This is a fork of [QEMU](https://www.qemu.org): upstream master plus a few
commits. The menu needs a Linux host.

## This branch: zero-copy (experimental)

`zero-copy` is master plus one experiment, not for everyday use yet:
**virtio-gpu/virgl: give guest-memory blobs a udmabuf for DRM native
contexts**.

- **What it does.** A guest compositor can hand the GPU a client's buffer in
  the guest's own memory. KWin does it for `wl_shm` clients, such as most Qt
  and GTK apps drawn by the CPU: it wraps the buffer in a udmabuf, and the
  guest's Mesa imports that. The guest driver passes it to the host as a
  guest-memory blob. A DRM native context can only use dma-bufs, so today it
  rejects the blob ("invalid res_id") and the guest copies the pixels
  instead. A full 4K buffer is about 30 MB per upload, about 3 ms on the GPU
  it was measured on. With this branch, QEMU gives such a blob a host udmabuf
  over the same guest pages, and the host GPU reads them directly.
- **When anything is missing it copies, as master does.** QEMU only warns once
  if the kernel refuses a udmabuf.
- **Host needs:**
  - DRM native context on the device (`drm_native_context=on`);
  - guest RAM from `memory-backend-memfd` (`-object
    memory-backend-memfd,id=mem,size=...,share=on -machine memory-backend=mem`),
    which udmabuf requires;
  - `/dev/udmabuf` usable by the user running QEMU (on Fedora desktops, the
    logged-in user through the `uaccess` tag);
  - udmabuf's limits: before Linux 7.3 a udmabuf may be 64 MB at most
    (`udmabuf.size_limit_mb`), and a blob in more than 1024 pieces is refused
    (`udmabuf.list_limit`);
  - a virglrenderer with `virgl_renderer_resource_set_guest_dmabuf()`: the
    patch in `contrib/qemu-gui/virglrenderer/`, which qemu-gui-manager's
    File > Build QEMU applies for this branch.
- **Guest needs:**
  - a guest kernel whose virtio-gpu driver attaches imported dma-bufs to the
    3D context. It is not upstream yet: "drm/virtio: attach imported dma-bufs
    to the 3D context", a two-line change, identical to one in Val Packett's
    pending `F_CREATE_GUEST_HANDLE` series. Without it the guest never offers
    the buffer, and nothing changes;
  - a compositor that imports `wl_shm` buffers through udmabuf: KWin 6.7 does.
- **Risks, why it is a branch:**
  - The pages of such buffers stay pinned while the host holds the udmabuf.
  - The guest side depends on kernel behaviour upstream may change: a pending
    revert of PRIME import with 3D, and a proposed udmabuf memlock limit, would
    both turn it off silently (back to copies).
  - The virglrenderer call is ours, not upstream's. Upstream is heading for a
    guest-negotiated flag (`VIRTIO_GPU_F_CREATE_GUEST_HANDLE`) instead, which
    would replace it.

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
- **ui/sdl2: share the clipboard with the guest**: copy and paste text
  between the host and the guest, see [Clipboard sharing](#clipboard-sharing).
- **hw/display/virtio-gpu-gl: tell whether the guest uses native context**:
  read-only properties of the `virtio-vga-gl` and `virtio-gpu-gl` devices,
  read with QMP `qom-get`. `x-drm-offered` says whether the guest is offered
  DRM native context, and `x-drm-contexts`, `x-virgl-contexts` and
  `x-venus-contexts` count the contexts of each kind it created since it
  booted. A guest offered native context that only creates virgl contexts
  has a Mesa without native context support for the GPU.
  [qemu-gui-manager](https://github.com/tomtomnet/qemu-gui-manager) shows it.
- **virtio-gpu-gl: disable the scanouts on reset in the main thread**: a
  fix. A guest reset of `virtio-gpu-gl` ran display work in a vCPU thread,
  and with `-display dbus,gl=on` QEMU aborted there
  (`dbus_scanout_texture: Assertion 'tex_id' failed`). UEFI resets the GPU
  when the bootloader hands over to the kernel, so VMs with `-vga none`
  crashed at boot. Upstream has the same bug.

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

## Clipboard sharing

Text copied on the host can be pasted in the guest, and the other way
around. QEMU's vdagent talks to spice-vdagent in the guest. On the host side
it works with any desktop; I tested it with KDE Plasma, Hyprland and niri.

1. Give the VM a vdagent channel:

       -device virtio-serial-pci \
       -chardev qemu-vdagent,id=vdagent,name=vdagent,clipboard=on,mouse=off \
       -device virtserialport,chardev=vdagent,name=com.redhat.spice.0

2. In the guest, install spice-vdagent: `sudo dnf install spice-vdagent`.
   GNOME and KDE Plasma start it on their own.
3. Wayland guests other than GNOME also need the clipboard bridge.
   spice-vdagent only sees the X11 (XWayland) clipboard: KDE Plasma passes
   only host-to-guest copies on to Wayland apps, and Hyprland and niri pass on
   neither direction. The bridge copies text between the two clipboards:

       sudo dnf install wl-clipboard xclip
       sudo install -m 755 contrib/vdagent-clipboard-bridge/vdagent-clipboard-bridge /usr/local/bin/

   Then start it with the session:
   - **KDE Plasma, niri and other desktops that run autostart entries**
     (niri does when started as `niri-session`, the usual way):

         cp contrib/vdagent-clipboard-bridge/vdagent-clipboard-bridge.desktop ~/.config/autostart/

     spice-vdagent starts through its own autostart entry. niri also needs
     `xwayland-satellite` installed for XWayland.
   - **Hyprland** runs no autostart entries, so start both spice-vdagent and
     the bridge in its config. With a Lua config (`hyprland.lua`):

         hl.on("hyprland.start", function ()
             hl.exec_cmd("spice-vdagent")
             hl.exec_cmd("/usr/local/bin/vdagent-clipboard-bridge")
         end)

     With a `hyprland.conf`:

         exec-once = spice-vdagent
         exec-once = /usr/local/bin/vdagent-clipboard-bridge

Good to know:
- Only text is shared.
- **Guest to host, on a Wayland host:** the text reaches the host clipboard at
  your next key press or click in the VM window, since Wayland lets a window
  set the clipboard only in response to input. Copying with Ctrl+C or a click
  in the guest already counts.
- **Host to guest, on a Wayland host:** a host copy is offered to the guest
  once the VM window has the focus, since Wayland only shows the clipboard to
  that window.
- **Debugging:** `-trace 'sdl2_clipboard*'` shows what the display does with the
  clipboard, and `-trace 'vdagent*'` shows the traffic with the guest.

## Notes

- The menu, the clipboard sharing and the clipboard bridge were written with
  an AI assistant (Claude). QEMU doesn't accept AI-generated contributions
  ([docs/devel/code-provenance.rst](../docs/devel/code-provenance.rst)), so
  these changes are not meant for upstream QEMU.
- License: GPL-2.0-or-later, like QEMU (see [COPYING](../COPYING)). Dear ImGui
  is MIT-licensed and downloaded when you configure.
- QEMU's own README is [README.rst](../README.rst).
