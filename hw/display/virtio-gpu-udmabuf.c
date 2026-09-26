/*
 * Virtio GPU Device
 *
 * Copyright Red Hat, Inc. 2013-2014
 *
 * Authors:
 *     Dave Airlie <airlied@redhat.com>
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qemu/iov.h"
#include "ui/console.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-pixman.h"
#include "trace.h"
#include "system/ramblock.h"
#include "system/hostmem.h"
#include <sys/ioctl.h>
#include <linux/memfd.h>
#include "qemu/memfd.h"
#include "standard-headers/linux/udmabuf.h"
#include "standard-headers/drm/drm_fourcc.h"

/*
 * Creates a udmabuf aliasing the guest RAM pages backing @res and returns its
 * fd, or -1.  @ioctl_errno is set when the UDMABUF_CREATE_LIST ioctl itself
 * fails and is 0 otherwise.
 */
static int
virtio_gpu_udmabuf_create_list(struct virtio_gpu_simple_resource *res,
                               int *ioctl_errno)
{
    struct udmabuf_create_list *list;
    RAMBlock *rb;
    ram_addr_t offset;
    int udmabuf, fd, i;

    *ioctl_errno = 0;

    udmabuf = udmabuf_fd();
    if (udmabuf < 0) {
        return -1;
    }

    list = g_try_malloc0(sizeof(struct udmabuf_create_list) +
                         sizeof(struct udmabuf_create_item) * res->iov_cnt);
    if (!list) {
        return -1;
    }

    for (i = 0; i < res->iov_cnt; i++) {
        rcu_read_lock();
        rb = qemu_ram_block_from_host(res->iov[i].iov_base, false, &offset);
        rcu_read_unlock();

        if (!rb || rb->fd < 0) {
            g_free(list);
            return -1;
        }

        list->list[i].memfd  = rb->fd;
        list->list[i].offset = offset;
        list->list[i].size   = res->iov[i].iov_len;
    }

    list->count = res->iov_cnt;
    list->flags = UDMABUF_FLAGS_CLOEXEC;

    fd = ioctl(udmabuf, UDMABUF_CREATE_LIST, list);
    if (fd < 0) {
        *ioctl_errno = errno;
    }
    g_free(list);

    return fd;
}

static void virtio_gpu_create_udmabuf(struct virtio_gpu_simple_resource *res)
{
    int err;

    res->dmabuf_fd = virtio_gpu_udmabuf_create_list(res, &err);
    if (err) {
        warn_report("%s: UDMABUF_CREATE_LIST: %s", __func__, strerror(err));
    }
}

/*
 * Returns a udmabuf aliasing the guest pages of @res without mapping it, for
 * resources that are handed over to the renderer.  Failures, such as hitting
 * the udmabuf list_limit or size_limit_mb module parameters, are reported
 * once.
 */
int virtio_gpu_create_udmabuf_fd(struct virtio_gpu_simple_resource *res)
{
    int err, fd;

    if (!res->iov_cnt) {
        return -1;
    }

    fd = virtio_gpu_udmabuf_create_list(res, &err);
    if (err) {
        warn_report_once("%s: UDMABUF_CREATE_LIST: %s", __func__,
                         strerror(err));
    }

    return fd;
}

static void virtio_gpu_remap_udmabuf(struct virtio_gpu_simple_resource *res)
{
    res->remapped = mmap(NULL, res->blob_size, PROT_READ,
                         MAP_SHARED, res->dmabuf_fd, 0);
    if (res->remapped == MAP_FAILED) {
        warn_report("%s: dmabuf mmap failed: %s", __func__,
                    strerror(errno));
        res->remapped = NULL;
    }
}

void virtio_gpu_fini_udmabuf(struct virtio_gpu_simple_resource *res)
{
    if (res->remapped) {
        munmap(res->remapped, res->blob_size);
        res->remapped = NULL;
    }
    if (res->dmabuf_fd >= 0) {
        close(res->dmabuf_fd);
        res->dmabuf_fd = -1;
        res->share_handle = SHAREABLE_NONE;
    }
    res->blob = NULL;
}

static int find_memory_backend_type(Object *obj, void *opaque)
{
    bool *memfd_backend = opaque;
    int ret;

    if (object_dynamic_cast(obj, TYPE_MEMORY_BACKEND)) {
        HostMemoryBackend *backend = MEMORY_BACKEND(obj);
        RAMBlock *rb = backend->mr.ram_block;

        if (rb && rb->fd > 0) {
            ret = fcntl(rb->fd, F_GET_SEALS);
            if (ret > 0) {
                *memfd_backend = true;
            }
        }
    }

    return 0;
}

bool virtio_gpu_have_udmabuf(void)
{
    Object *memdev_root;
    int udmabuf;
    bool memfd_backend = false;

    udmabuf = udmabuf_fd();
    if (udmabuf < 0) {
        return false;
    }

    memdev_root = object_resolve_path("/objects", NULL);
    object_child_foreach(memdev_root, find_memory_backend_type, &memfd_backend);

    return memfd_backend;
}

bool virtio_gpu_init_udmabuf(struct virtio_gpu_simple_resource *res)
{
    void *pdata = NULL;

    res->dmabuf_fd = -1;
    if (res->iov_cnt == 1 &&
        res->iov[0].iov_len < 4096) {
        pdata = res->iov[0].iov_base;
    } else if (res->blob_size) {
        virtio_gpu_create_udmabuf(res);
        if (res->dmabuf_fd < 0) {
            return false;
        }
        virtio_gpu_remap_udmabuf(res);
        if (!res->remapped) {
            virtio_gpu_fini_udmabuf(res);
            return false;
        }
        res->share_handle = res->dmabuf_fd;
        pdata = res->remapped;
    }

    res->blob = pdata;

    return true;
}

static QemuDmaBuf *
virtio_gpu_create_dmabuf(struct virtio_gpu_simple_resource *res,
                         struct virtio_gpu_framebuffer *fb,
                         struct virtio_gpu_rect *r)
{
    uint32_t offset = 0;
    int fd;

    if (res->dmabuf_fd < 0) {
        return NULL;
    }

    fd = qemu_dup(res->dmabuf_fd);
    if (fd < 0) {
        return NULL;
    }

    return qemu_dmabuf_new(r->width, r->height,
                           &offset, &fb->stride,
                           r->x, r->y, fb->width, fb->height,
                           qemu_pixman_to_drm_format(fb->format),
                           DRM_FORMAT_MOD_INVALID, &fd,
                           1, true, false);
}

int virtio_gpu_update_dmabuf(VirtIOGPU *g,
                             uint32_t scanout_id,
                             struct virtio_gpu_simple_resource *res,
                             struct virtio_gpu_framebuffer *fb,
                             struct virtio_gpu_rect *r)
{
    struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[scanout_id];
    QemuDmaBuf *new_primary, *old_primary;
    uint32_t width, height;

    new_primary = virtio_gpu_create_dmabuf(res, fb, r);
    if (!new_primary) {
        return -EINVAL;
    }

    old_primary = scanout->dmabuf;

    width = qemu_dmabuf_get_width(new_primary);
    height = qemu_dmabuf_get_height(new_primary);
    scanout->dmabuf = new_primary;
    qemu_console_resize(scanout->con, width, height);
    qemu_console_gl_scanout_dmabuf(scanout->con, new_primary);

    if (old_primary) {
        qemu_console_gl_release_dmabuf(scanout->con, old_primary);
        qemu_dmabuf_free(old_primary);
    }

    return 0;
}
