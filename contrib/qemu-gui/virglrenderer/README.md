# virglrenderer patches of this branch

This branch needs a virglrenderer with these patches. qemu-gui-manager's
File > Build QEMU applies every `*.patch` here to the virglrenderer it builds
for the branch, after its own (cmspam's Xe native context, AMD
write-combining). Branches without this folder build with those alone.

- `0001-virglrenderer-let-the-VMM-attach-a-dma-buf-to-guest-memory-blobs.patch`:
  `virgl_renderer_resource_set_guest_dmabuf()`, which QEMU's zero-copy code
  calls. Without it, QEMU builds with that code left out.
