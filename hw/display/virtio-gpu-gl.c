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
#include "qemu/iov.h"
#include "qemu/mmap-alloc.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "system/system.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"
#include "hw/virtio/virtio-gpu-pixman.h"
#include "hw/core/qdev-properties.h"

#include <virglrenderer.h>

static void virtio_gpu_gl_update_cursor_data(VirtIOGPU *g,
                                             struct virtio_gpu_scanout *s,
                                             uint32_t resource_id)
{
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    uint32_t width, height;
    uint32_t pixels, *data;

    if (gl->renderer_state != RS_INITED) {
        return;
    }

    data = virgl_renderer_get_cursor_data(resource_id, &width, &height);
    if (!data) {
        return;
    }

    if (width != s->current_cursor->width ||
        height != s->current_cursor->height) {
        free(data);
        return;
    }

    pixels = s->current_cursor->width * s->current_cursor->height;
    memcpy(s->current_cursor->data, data, pixels * sizeof(uint32_t));
    free(data);
}

static void virtio_gpu_gl_flushed(VirtIOGPUBase *b)
{
    VirtIOGPU *g = VIRTIO_GPU(b);

    virtio_gpu_process_cmdq(g);
}

static void virtio_gpu_gl_handle_ctrl(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    struct virtio_gpu_ctrl_command *cmd;

    if (!virtio_queue_ready(vq)) {
        return;
    }

    if (!virtio_gpu_virgl_update_render_state(g)) {
        return;
    }

    cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    while (cmd) {
        cmd->vq = vq;
        cmd->error = 0;
        cmd->finished = false;
        QTAILQ_INSERT_TAIL(&g->cmdq, cmd, next);
        cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    }

    virtio_gpu_process_cmdq(g);
    virtio_gpu_virgl_fence_poll(g);
}

static void virtio_gpu_gl_reset(VirtIODevice *vdev)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(vdev);

    virtio_gpu_reset(vdev);
    memset(gl->contexts_created, 0, sizeof(gl->contexts_created));

    /*
     * GL functions must be called with the associated GL context in main
     * thread, and when the renderer is unblocked.
     */
    if (gl->renderer_state == RS_INITED) {
        virtio_gpu_virgl_reset_scanout(g);
        gl->renderer_state = RS_RESET;
    }
}

static void virtio_gpu_gl_device_realize(DeviceState *qdev, Error **errp)
{
    ERRP_GUARD();
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(qdev);
    VirtIOGPU *g = VIRTIO_GPU(b);
#if !defined(CONFIG_WIN32)
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    void *map;
#endif

#if HOST_BIG_ENDIAN
    error_setg(errp, "virgl is not supported on bigendian platforms");
    return;
#endif

    if (!object_resolve_path_type("", TYPE_VIRTIO_GPU_GL, NULL)) {
        error_setg(errp, "at most one %s device is permitted", TYPE_VIRTIO_GPU_GL);
        return;
    }

    if (!display_opengl) {
        error_setg(errp,
                   "The display backend does not have OpenGL support enabled");
        error_append_hint(errp,
                          "It can be enabled with '-display BACKEND,gl=on' "
                          "where BACKEND is the name of the display backend "
                          "to use.\n");
        return;
    }

    g->parent_obj.conf.flags |= (1 << VIRTIO_GPU_FLAG_VIRGL_ENABLED);
    g->capset_ids = virtio_gpu_virgl_get_capsets(g);
    VIRTIO_GPU_BASE(g)->virtio_config.num_capsets = g->capset_ids->len;

#if VIRGL_VERSION_MAJOR >= 1
    g->parent_obj.conf.flags |= 1 << VIRTIO_GPU_FLAG_CONTEXT_INIT_ENABLED;
#endif

#if !defined(CONFIG_WIN32)
    if (virtio_gpu_hostmem_enabled(b->conf)) {
        map = qemu_ram_mmap(-1, b->conf.hostmem, qemu_real_host_page_size(),
                            0, 0);
        if (map == MAP_FAILED) {
            error_setg_errno(errp, errno,
                             "virgl hostmem region could not be initialized");
            return;
        }

        gl->hostmem_mmap = map;
        memory_region_init_ram_ptr(&gl->hostmem_background, NULL,
                                   "hostmem-background", b->conf.hostmem,
                                   gl->hostmem_mmap);
        memory_region_add_subregion(&b->hostmem, 0, &gl->hostmem_background);
    }
#endif

    virtio_gpu_device_realize(qdev, errp);
}

static const Property virtio_gpu_gl_properties[] = {
    DEFINE_PROP_BIT("stats", VirtIOGPU, parent_obj.conf.flags,
                    VIRTIO_GPU_FLAG_STATS_ENABLED, false),
    DEFINE_PROP_BIT("venus", VirtIOGPU, parent_obj.conf.flags,
                    VIRTIO_GPU_FLAG_VENUS_ENABLED, false),
    DEFINE_PROP_BIT("drm_native_context", VirtIOGPU, parent_obj.conf.flags,
                    VIRTIO_GPU_FLAG_DRM_ENABLED, false),
};

static void virtio_gpu_gl_device_unrealize(DeviceState *qdev)
{
    VirtIOGPU *g = VIRTIO_GPU(qdev);
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(qdev);

    if (gl->renderer_state >= RS_INITED) {
#if VIRGL_VERSION_MAJOR >= 1
        qemu_bh_delete(gl->cmdq_resume_bh);

        if (gl->async_fence_bh) {
            virtio_gpu_virgl_reset_async_fences(g);
            qemu_bh_delete(gl->async_fence_bh);
        }
#endif
        if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
            timer_free(gl->print_stats);
        }
        timer_free(gl->fence_poll);
        virgl_renderer_cleanup(NULL);
    }

    gl->renderer_state = RS_START;

    g_array_unref(g->capset_ids);

    /*
     * It is not guaranteed that the memory region will be finalized
     * immediately with memory_region_del_subregion(), there can be
     * a remaining reference to gl->hostmem_mmap. VirtIO-GPU is not
     * hotpluggable, hence no need to worry about the leaked mapping.
     *
     * The memory_region_del_subregion(gl->hostmem_background) is unnecessary
     * because b->hostmem  and gl->hostmem_background belong to the same
     * device and will be gone at the same time.
     */
}

/*
 * Read-only, for management tools: whether the guest actually uses DRM
 * native context, which needs Mesa built with it in the guest, or falls
 * back to virgl.  The counts are of the contexts created since the guest
 * reset the device, so since it booted.
 */
static void virtio_gpu_gl_get_contexts(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(obj);
    uint32_t capset_id = GPOINTER_TO_UINT(opaque);
    uint32_t count = gl->contexts_created[capset_id];

    /* virgl, with or without context_init */
    if (capset_id == VIRTIO_GPU_CAPSET_VIRGL) {
        count += gl->contexts_created[VIRTIO_GPU_CAPSET_VIRGL2];
    }
    visit_type_uint32(v, name, &count, errp);
}

/* drm_native_context=on, and virglrenderer has a renderer for the host GPU */
static bool virtio_gpu_gl_get_drm_offered(Object *obj, Error **errp)
{
    VirtIOGPU *g = VIRTIO_GPU(obj);

    for (guint i = 0; g->capset_ids && i < g->capset_ids->len; i++) {
        uint32_t capset_id = g_array_index(g->capset_ids, uint32_t, i);

        if (capset_id == VIRTIO_GPU_CAPSET_DRM) {
            return true;
        }
    }
    return false;
}

static void virtio_gpu_gl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    VirtIOGPUBaseClass *vbc = VIRTIO_GPU_BASE_CLASS(klass);
    VirtIOGPUClass *vgc = VIRTIO_GPU_CLASS(klass);

    vbc->gl_flushed = virtio_gpu_gl_flushed;
    vgc->handle_ctrl = virtio_gpu_gl_handle_ctrl;
    vgc->process_cmd = virtio_gpu_virgl_process_cmd;
    vgc->update_cursor_data = virtio_gpu_gl_update_cursor_data;

    vgc->resource_destroy = virtio_gpu_virgl_resource_destroy;
    vdc->realize = virtio_gpu_gl_device_realize;
    vdc->unrealize = virtio_gpu_gl_device_unrealize;
    vdc->reset = virtio_gpu_gl_reset;
    device_class_set_props(dc, virtio_gpu_gl_properties);

    object_class_property_add(klass, "x-virgl-contexts", "uint32",
                              virtio_gpu_gl_get_contexts, NULL, NULL,
                              GUINT_TO_POINTER(VIRTIO_GPU_CAPSET_VIRGL));
    object_class_property_set_description(klass, "x-virgl-contexts",
        "virgl contexts the guest created since it booted");
    object_class_property_add(klass, "x-venus-contexts", "uint32",
                              virtio_gpu_gl_get_contexts, NULL, NULL,
                              GUINT_TO_POINTER(VIRTIO_GPU_CAPSET_VENUS));
    object_class_property_set_description(klass, "x-venus-contexts",
        "Venus contexts the guest created since it booted");
    object_class_property_add(klass, "x-drm-contexts", "uint32",
                              virtio_gpu_gl_get_contexts, NULL, NULL,
                              GUINT_TO_POINTER(VIRTIO_GPU_CAPSET_DRM));
    object_class_property_set_description(klass, "x-drm-contexts",
        "DRM native contexts the guest created since it booted");
    object_class_property_add_bool(klass, "x-drm-offered",
                                   virtio_gpu_gl_get_drm_offered, NULL);
    object_class_property_set_description(klass, "x-drm-offered",
        "Whether the guest is offered DRM native context");
}

static const TypeInfo virtio_gpu_gl_info = {
    .name = TYPE_VIRTIO_GPU_GL,
    .parent = TYPE_VIRTIO_GPU,
    .instance_size = sizeof(VirtIOGPUGL),
    .class_init = virtio_gpu_gl_class_init,
};
module_obj(TYPE_VIRTIO_GPU_GL);
module_kconfig(VIRTIO_GPU);

static void virtio_register_types(void)
{
    type_register_static(&virtio_gpu_gl_info);
}

type_init(virtio_register_types)

module_dep("hw-display-virtio-gpu");
module_dep("ui-opengl");
