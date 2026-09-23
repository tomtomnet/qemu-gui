/*
 * QEMU SDL display: VM operations behind the in-window menu
 *
 * Plain C, so that the menu (sdl2-gui.cpp) does not need QEMU headers.
 * Everything here runs in the main loop with the BQL held.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <link.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include "qemu-version.h"
#include "qemu/accel.h"
#include "qemu/cutils.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "qemu/uuid.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-audio.h"
#include "qapi/qapi-commands-migration.h"
#include "qapi/qapi-commands-misc.h"
#include "qapi/util.h"
#include "qobject/qdict.h"
#include "block/accounting.h"
#include "block/block.h"
#include "block/block_int.h"
#include "block/snapshot.h"
#include "migration/snapshot.h"
#include "monitor/qdev.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev.h"
#include "hw/usb/usb.h"
#include "net/net.h"
#include "system/block-backend.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "system/system.h"
#include "ui/input.h"
#include "standard-headers/linux/input-event-codes.h"
#include "ui/sdl2-gui.h"

#define USB_SYSFS_DEVICES "/sys/bus/usb/devices"

void sdl2_gui_free(void *ptr)
{
    g_free(ptr);
}

/* Hand the message of @err, if any, to the menu */
static bool report(Error *err, char **errmsg)
{
    if (!err) {
        return true;
    }
    *errmsg = g_strdup(error_get_pretty(err));
    error_free(err);
    return false;
}

static gboolean sdl2_gui_idle(gpointer opaque)
{
    sdl2_gui_run_scheduled();
    return G_SOURCE_REMOVE;
}

/*
 * A glib source rather than a bottom half: the menu calls into the block
 * layer while it builds a frame (to list snapshots), which may poll the
 * main AioContext and would run a bottom half in the middle of the frame.
 */
void sdl2_gui_vm_schedule(void)
{
    g_idle_add_full(G_PRIORITY_DEFAULT, sdl2_gui_idle, NULL, NULL);
}

const char *sdl2_gui_vm_state(bool *running)
{
    *running = runstate_is_running();
    return RunState_str(runstate_get());
}

bool sdl2_gui_vm_pause(char **errmsg)
{
    Error *err = NULL;

    qmp_stop(&err);
    return report(err, errmsg);
}

bool sdl2_gui_vm_resume(char **errmsg)
{
    Error *err = NULL;

    qmp_cont(&err);
    return report(err, errmsg);
}

void sdl2_gui_vm_reset(void)
{
    qemu_system_reset_request(SHUTDOWN_CAUSE_HOST_UI);
}

void sdl2_gui_vm_powerdown(void)
{
    qemu_system_powerdown_request();
}

void sdl2_gui_vm_quit(void)
{
    shutdown_action = SHUTDOWN_ACTION_POWEROFF;
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

void sdl2_gui_vm_send_ctrl_alt(SDL_Scancode key)
{
    unsigned int keys[] = { KEY_LEFTCTRL, KEY_LEFTALT, 0 };
    int i;

    if (key <= 0 || key >= qemu_input_map_usb_to_linux_len) {
        return;
    }
    keys[2] = qemu_input_map_usb_to_linux[key];

    for (i = 0; i < ARRAY_SIZE(keys); i++) {
        qemu_input_event_send_key_linux(NULL, keys[i], true);
        qemu_input_event_send_key_delay(0);
    }
    for (i = ARRAY_SIZE(keys) - 1; i >= 0; i--) {
        qemu_input_event_send_key_linux(NULL, keys[i], false);
        qemu_input_event_send_key_delay(0);
    }
}

/*
 * Snapshots are internal snapshots of the writable disks that support them
 * (qcow2), like savevm.  Unlike savevm, other writable disks (typically a
 * raw OVMF_VARS pflash) are left out rather than making every snapshot
 * fail; the menu tells which.
 *
 * With a VM that cannot be migrated (e.g. virtio-gpu with virgl) RAM and
 * device state cannot be saved, so only the disks are, and reverting to such
 * a snapshot puts the disks back and restarts the guest from them.
 */
static strList *snapshot_disks(GString *names, GString *skipped)
{
    strList *disks = NULL, **tail = &disks;
    BlockDriverState *bs;
    BdrvNextIterator it;

    GRAPH_RDLOCK_GUARD_MAINLOOP();

    for (bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
        GString *list = skipped;

        /* the disks bdrv_all_create_snapshot() would include */
        if (!bdrv_is_inserted(bs) || bdrv_is_read_only(bs) ||
            !(bdrv_has_blk(bs) || QLIST_EMPTY(&bs->parents))) {
            continue;
        }
        if (bdrv_can_snapshot(bs)) {
            QAPI_LIST_APPEND(tail, g_strdup(bdrv_get_node_name(bs)));
            list = names;
        }
        if (list) {
            g_string_append_printf(list, "%s%s (%s)", list->len ? ", " : "",
                                   bdrv_get_device_or_node_name(bs),
                                   bdrv_get_format_name(bs) ?: "no format");
        }
    }
    return disks;
}

static char *nonempty(GString *str)
{
    return g_string_free(str, str->len == 0);
}

/* Why RAM and device state cannot be saved, or NULL if they can */
static char *no_state_reason(void)
{
    MigrationInfo *info = qmp_query_migrate(NULL);
    GString *reasons = g_string_new("");
    strList *r;

    for (r = info ? info->blocked_reasons : NULL; r; r = r->next) {
        g_string_append_printf(reasons, "%s%s", reasons->len ? "; " : "",
                               r->value);
    }
    qapi_free_MigrationInfo(info);
    return nonempty(reasons);
}

static int snapshot_newest_first(const void *a, const void *b)
{
    const SDL2GuiSnapshot *sa = a, *sb = b;

    return sa->date < sb->date ? 1 : sa->date > sb->date ? -1 : 0;
}

SDL2GuiSnapshotList *sdl2_gui_vm_snapshots(void)
{
    SDL2GuiSnapshotList *list = g_new0(SDL2GuiSnapshotList, 1);
    GString *names = g_string_new(""), *skipped = g_string_new("");
    g_autoptr(strList) disks = snapshot_disks(names, skipped);
    QEMUSnapshotInfo *sn_tab = NULL;
    BlockDriverState *bs;
    int i, n;

    list->disks = nonempty(names);
    list->skipped = nonempty(skipped);
    list->no_state_reason = no_state_reason();
    if (!disks) {
        return list;
    }

    /* the first disk holds the VM state of full snapshots */
    bs = bdrv_find_node(disks->value);
    n = bs ? bdrv_snapshot_list(bs, &sn_tab) : 0;
    list->snapshots = g_new0(SDL2GuiSnapshot, MAX(n, 0));
    for (i = 0; i < n; i++) {
        SDL2GuiSnapshot *s;

        /* only snapshots present on all the disks can be reverted to */
        if (bdrv_all_has_snapshot(sn_tab[i].name, true, disks, NULL) != 1) {
            continue;
        }
        s = &list->snapshots[list->count++];
        pstrcpy(s->name, sizeof(s->name), sn_tab[i].name);
        s->date = sn_tab[i].date_sec;
        s->vm_state_size = sn_tab[i].vm_state_size;
    }
    g_free(sn_tab);
    qsort(list->snapshots, list->count, sizeof(*list->snapshots),
          snapshot_newest_first);
    return list;
}

void sdl2_gui_vm_snapshots_free(SDL2GuiSnapshotList *list)
{
    if (list) {
        g_free(list->snapshots);
        g_free(list->disks);
        g_free(list->skipped);
        g_free(list->no_state_reason);
        g_free(list);
    }
}

static strList *snapshot_disks_or_error(char **errmsg)
{
    strList *disks = snapshot_disks(NULL, NULL);

    if (!disks) {
        *errmsg = g_strdup("No writable disk supports snapshots "
                           "(they need the qcow2 format).");
    }
    return disks;
}

/* Like save_snapshot(), without the VM state */
static void save_disk_snapshot(const char *name, strList *disks,
                               Error **errp)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    RunState saved_state = runstate_get();
    QEMUSnapshotInfo sn = { 0 };

    /* stop the guest so that all the disks are taken at the same point */
    vm_stop(RUN_STATE_SAVE_VM);
    bdrv_drain_all_begin();

    pstrcpy(sn.name, sizeof(sn.name), name);
    sn.date_sec = g_date_time_to_unix(now);
    sn.date_nsec = g_date_time_get_microsecond(now) * 1000;
    sn.vm_clock_nsec = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    sn.icount = -1ULL;
    if (bdrv_all_create_snapshot(&sn, NULL, 0, true, disks, errp) < 0) {
        bdrv_all_delete_snapshot(name, true, disks, NULL);
    }

    bdrv_drain_all_end();
    vm_resume(saved_state);
}

bool sdl2_gui_vm_snapshot_save(const char *name, bool with_state,
                               char **errmsg)
{
    g_autoptr(strList) disks = snapshot_disks_or_error(errmsg);
    Error *err = NULL;
    int ret;

    if (!disks) {
        return false;
    }
    ret = bdrv_all_has_snapshot(name, true, disks, &err);
    if (ret < 0) {
        return report(err, errmsg);
    }
    if (ret) {
        *errmsg = g_strdup_printf("A snapshot named '%s' already exists.",
                                  name);
        return false;
    }

    if (with_state) {
        save_snapshot(name, false, NULL, true, disks, &err);
    } else {
        save_disk_snapshot(name, disks, &err);
    }
    return report(err, errmsg);
}

bool sdl2_gui_vm_snapshot_revert(const char *name, char **errmsg)
{
    g_autoptr(strList) disks = snapshot_disks_or_error(errmsg);
    g_autofree char *no_state = NULL;
    RunState saved_state = runstate_get();
    BlockDriverState *bs;
    QEMUSnapshotInfo sn;
    Error *err = NULL;

    if (!disks) {
        return false;
    }
    bs = bdrv_find_node(disks->value);
    if (!bs || bdrv_snapshot_find(bs, &sn, name) < 0) {
        *errmsg = g_strdup_printf("Snapshot '%s' not found.", name);
        return false;
    }

    no_state = no_state_reason();
    if (sn.vm_state_size && !no_state) {
        vm_stop(RUN_STATE_RESTORE_VM);
        if (load_snapshot(name, NULL, true, disks, &err)) {
            load_snapshot_resume(saved_state);
        }
        return report(err, errmsg);
    }

    /*
     * Disk-only: put the disks back and restart the guest from them.  The
     * reset must happen before the guest runs again, or it could write data
     * from its old memory to the reverted disks.
     */
    vm_stop(RUN_STATE_RESTORE_VM);
    bdrv_drain_all_begin();
    if (bdrv_all_goto_snapshot(name, true, disks, &err) < 0) {
        bdrv_drain_all_end();
        *errmsg = g_strdup_printf("%s. The VM was left stopped.",
                                  error_get_pretty(err));
        error_free(err);
        return false;
    }
    bdrv_drain_all_end();

    qemu_system_reset(SHUTDOWN_CAUSE_HOST_UI);
    if (runstate_is_live(saved_state)) {
        /* a guest that was suspended is not after the reset */
        vm_set_suspended(false);
        vm_start();
    } else if (runstate_needs_reset()) {
        runstate_set(RUN_STATE_PRELAUNCH);
    }
    return true;
}

bool sdl2_gui_vm_snapshot_delete(const char *name, char **errmsg)
{
    g_autoptr(strList) disks = snapshot_disks_or_error(errmsg);
    Error *err = NULL;

    if (!disks) {
        return false;
    }
    delete_snapshot(name, true, disks, &err);
    return report(err, errmsg);
}

/*
 * USB passthrough.  Host devices are listed from sysfs, so that listing
 * works without opening them, and passed through with usb-host.
 */
static char *usb_sysfs_read(const char *dev, const char *attr)
{
    g_autofree char *path = g_build_filename(USB_SYSFS_DEVICES, dev, attr,
                                             NULL);
    char *contents;

    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        return NULL;
    }
    return g_strstrip(contents);
}

static unsigned int usb_sysfs_read_uint(const char *dev, const char *attr,
                                        int base)
{
    g_autofree char *str = usb_sysfs_read(dev, attr);
    unsigned int val;

    if (!str || qemu_strtoui(str, NULL, base, &val) < 0) {
        return 0;
    }
    return val;
}

static bool usb_sysfs_has_hid(const char *dev)
{
    int i;

    for (i = 0; i < 8; i++) {
        g_autofree char *intf = g_strdup_printf("%s:1.%d", dev, i);

        if (usb_sysfs_read_uint(intf, "bInterfaceClass", 16) ==
            USB_CLASS_HID) {
            return true;
        }
    }
    return false;
}

static char *usb_node(unsigned int bus, unsigned int addr)
{
    return g_strdup_printf("/dev/bus/usb/%03u/%03u", bus, addr);
}

static int usb_find_host_devices(Object *obj, void *opaque)
{
    if (object_dynamic_cast(obj, "usb-host")) {
        g_ptr_array_add(opaque, obj);
    }
    return 0;
}

static GPtrArray *usb_host_devices(void)
{
    GPtrArray *devs = g_ptr_array_new();

    object_child_foreach_recursive(object_get_root(), usb_find_host_devices,
                                   devs);
    return devs;
}

/* Does usb-host device @obj use the host device @dev? */
static bool usb_host_matches(Object *obj, const SDL2GuiUsbDevice *dev)
{
    unsigned int bus = object_property_get_uint(obj, "hostbus", NULL);
    unsigned int addr = object_property_get_uint(obj, "hostaddr", NULL);
    unsigned int vendor = object_property_get_uint(obj, "vendorid", NULL);
    unsigned int product = object_property_get_uint(obj, "productid", NULL);
    g_autofree char *port = object_property_get_str(obj, "hostport", NULL);
    g_autofree char *node = object_property_get_str(obj, "hostdevice", NULL);
    const char *dev_port = strchr(dev->port, '-');

    if (node && *node) {
        g_autofree char *dev_node = usb_node(dev->bus, dev->addr);

        return g_str_equal(node, dev_node);
    }
    if (!bus && !addr && !vendor && !product && !(port && *port)) {
        return false;
    }
    return (!bus || bus == dev->bus) && (!addr || addr == dev->addr) &&
           (!vendor || vendor == dev->vendor_id) &&
           (!product || product == dev->product_id) &&
           (!port || !*port || (dev_port && g_str_equal(port, dev_port + 1)));
}

static char *usb_unavailable_reason(void)
{
    bool ambiguous = false;

    if (!module_object_class_by_name("usb-host")) {
        return g_strdup("This QEMU is built without USB passthrough "
                        "(configure with --enable-libusb).");
    }
    if (!object_resolve_path_type("", TYPE_USB_BUS, &ambiguous) &&
        !ambiguous) {
        return g_strdup("The VM has no USB controller "
                        "(add -device qemu-xhci).");
    }
    return NULL;
}

static gint usb_device_order(gconstpointer a, gconstpointer b)
{
    const SDL2GuiUsbDevice *da = a, *db = b;

    return da->bus != db->bus ? (int)da->bus - (int)db->bus :
                                strcmp(da->port, db->port);
}

SDL2GuiUsbList *sdl2_gui_vm_usb_devices(void)
{
    SDL2GuiUsbList *list = g_new0(SDL2GuiUsbList, 1);
    g_autoptr(GPtrArray) host_devs = usb_host_devices();
    GArray *devs = g_array_new(false, true, sizeof(SDL2GuiUsbDevice));
    g_autoptr(GDir) dir = g_dir_open(USB_SYSFS_DEVICES, 0, NULL);
    const char *name;
    int i;

    list->unavailable = usb_unavailable_reason();

    while (dir && (name = g_dir_read_name(dir))) {
        g_autofree char *manufacturer = NULL, *product = NULL, *node = NULL;
        SDL2GuiUsbDevice dev = { 0 };

        /* skip interfaces ("1-2:1.0") and root hubs ("usb1") */
        if (strchr(name, ':') || g_str_has_prefix(name, "usb")) {
            continue;
        }
        dev.bus = usb_sysfs_read_uint(name, "busnum", 10);
        dev.addr = usb_sysfs_read_uint(name, "devnum", 10);
        if (!dev.bus || !dev.addr ||
            usb_sysfs_read_uint(name, "bDeviceClass", 16) == USB_CLASS_HUB) {
            continue;
        }
        dev.vendor_id = usb_sysfs_read_uint(name, "idVendor", 16);
        dev.product_id = usb_sysfs_read_uint(name, "idProduct", 16);
        pstrcpy(dev.port, sizeof(dev.port), name);

        manufacturer = usb_sysfs_read(name, "manufacturer");
        product = usb_sysfs_read(name, "product");
        if (product && *product) {
            snprintf(dev.name, sizeof(dev.name), "%s%s%s",
                     manufacturer ? manufacturer : "",
                     manufacturer && *manufacturer ? " " : "", product);
        } else {
            snprintf(dev.name, sizeof(dev.name), "USB device %04x:%04x",
                     dev.vendor_id, dev.product_id);
        }

        dev.hid = usb_sysfs_has_hid(name);
        node = usb_node(dev.bus, dev.addr);
        dev.accessible = access(node, R_OK | W_OK) == 0;
        for (i = 0; i < host_devs->len; i++) {
            if (usb_host_matches(g_ptr_array_index(host_devs, i), &dev)) {
                dev.attached = true;
                break;
            }
        }
        g_array_append_val(devs, dev);
    }

    g_array_sort(devs, usb_device_order);
    list->count = devs->len;
    list->devices = (SDL2GuiUsbDevice *)g_array_free(devs, false);
    return list;
}

void sdl2_gui_vm_usb_devices_free(SDL2GuiUsbList *list)
{
    if (list) {
        g_free(list->devices);
        g_free(list->unavailable);
        g_free(list);
    }
}

bool sdl2_gui_vm_usb_attach(const SDL2GuiUsbDevice *dev, char **errmsg)
{
    g_autofree char *node = usb_node(dev->bus, dev->addr);
    g_autofree char *id = g_strdup_printf("sdl-usb-%u-%u", dev->bus,
                                          dev->addr);
    g_autofree char *bus = g_strdup_printf("%u", dev->bus);
    g_autofree char *addr = g_strdup_printf("%u", dev->addr);
    DeviceState *qdev;
    Error *err = NULL;
    QDict *opts;

    if (access(node, R_OK | W_OK) < 0) {
        *errmsg = g_strdup_printf("No permission to open %s. Give your user "
                                  "access to it, e.g. with a udev rule.",
                                  node);
        return false;
    }

    opts = qdict_new();
    qdict_put_str(opts, "driver", "usb-host");
    qdict_put_str(opts, "id", id);
    qdict_put_str(opts, "hostbus", bus);
    qdict_put_str(opts, "hostaddr", addr);
    qdev = qdev_device_add_from_qdict(opts, false, &err);
    qobject_unref(opts);
    if (!qdev) {
        /* like device_add: let a failed device go away completely */
        drain_call_rcu();
        return report(err, errmsg);
    }
    object_unref(OBJECT(qdev));
    return true;
}

/*
 * Access to a USB device node without running QEMU as root: pkexec asks
 * polkit to authenticate the user (the desktop shows its password dialog),
 * then setfacl gives the user read/write access to that one node.  The ACL
 * goes away with the node, when the device is unplugged.
 */
typedef struct UsbGrant {
    SDL2GuiUsbDevice dev;
    char *node;
} UsbGrant;

static void usb_grant_done(GPid pid, gint status, gpointer opaque)
{
    UsbGrant *grant = opaque;
    g_autofree char *msg = NULL;
    bool cancelled = false;

    g_spawn_close_pid(pid);
    if (!WIFEXITED(status)) {
        msg = g_strdup("The access request failed.");
    } else if (WEXITSTATUS(status) == 126) {
        /* pkexec: the user dismissed the authentication dialog */
        cancelled = true;
    } else if (WEXITSTATUS(status) == 127) {
        msg = g_strdup_printf("Not authorized to give access to %s.",
                              grant->node);
    } else if (WEXITSTATUS(status) != 0) {
        msg = g_strdup_printf("Could not give access to %s.", grant->node);
    } else if (access(grant->node, R_OK | W_OK) < 0) {
        msg = g_strdup_printf("Still no access to %s.", grant->node);
    }

    sdl2_gui_usb_access_done(&grant->dev, !msg && !cancelled, msg);
    g_free(grant->node);
    g_free(grant);
}

bool sdl2_gui_vm_usb_grant(const SDL2GuiUsbDevice *dev, char **errmsg)
{
    g_autofree char *pkexec = g_find_program_in_path("pkexec");
    g_autofree char *setfacl = g_find_program_in_path("setfacl");
    g_autofree char *acl = g_strdup_printf("u:%u:rw", (unsigned)getuid());
    char *node = usb_node(dev->bus, dev->addr);
    char *argv[] = { pkexec, setfacl, (char *)"-m", acl, node, NULL };
    GError *err = NULL;
    UsbGrant *grant;
    GPid pid;

    if (!pkexec || !setfacl) {
        *errmsg = g_strdup("Asking for access needs pkexec (polkit) and "
                           "setfacl (acl).");
        g_free(node);
        return false;
    }
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL,
                       NULL, &pid, &err)) {
        *errmsg = g_strdup_printf("Cannot run pkexec: %s", err->message);
        g_error_free(err);
        g_free(node);
        return false;
    }

    grant = g_new0(UsbGrant, 1);
    grant->dev = *dev;
    grant->node = node;
    g_child_watch_add(pid, usb_grant_done, grant);
    return true;
}

bool sdl2_gui_vm_usb_detach(const SDL2GuiUsbDevice *dev, char **errmsg)
{
    g_autoptr(GPtrArray) host_devs = usb_host_devices();
    Error *err = NULL;
    int i;

    for (i = 0; i < host_devs->len; i++) {
        Object *obj = g_ptr_array_index(host_devs, i);

        if (usb_host_matches(obj, dev)) {
            qdev_unplug(DEVICE(obj), &err);
            return report(err, errmsg);
        }
    }
    *errmsg = g_strdup_printf("%s is not passed through.", dev->name);
    return false;
}

/* What identifies this VM across runs, for per-VM settings */
char *sdl2_gui_vm_id(void)
{
    BlockBackend *blk;
    char *id = NULL;
    int pass;

    if (qemu_name) {
        return g_strdup_printf("name:%s", qemu_name);
    }
    if (qemu_uuid_set) {
        g_autofree char *uuid = qemu_uuid_unparse_strdup(&qemu_uuid);

        return g_strdup_printf("uuid:%s", uuid);
    }

    /* the first writable disk, which is this VM's own; UEFI variables last */
    GRAPH_RDLOCK_GUARD_MAINLOOP();
    for (pass = 0; pass < 2 && !id; pass++) {
        for (blk = blk_all_next(NULL); blk && !id; blk = blk_all_next(blk)) {
            BlockDriverState *bs = blk_bs(blk);
            bool pflash = g_str_has_prefix(blk_name(blk), "pflash");
            char *path;

            if (!bs || !blk_is_writable(blk) || pflash != (pass == 1)) {
                continue;
            }
            /* with -snapshot, the image under the temporary overlay */
            while ((bs->open_flags & BDRV_O_TEMPORARY) && bs->backing) {
                bs = bs->backing->bs;
            }
            path = realpath(bs->filename, NULL);
            id = g_strdup_printf("disk:%s", path ? path : bs->filename);
            free(path);
        }
    }
    return id ? id : g_strdup("default");
}

/* Statistics */

/* CPU time used so far by the thread or process of @stat_path, in ticks */
static uint64_t thread_ticks(const char *stat_path)
{
    g_autofree char *stat = NULL;
    unsigned long utime, stime;
    const char *p;

    if (!g_file_get_contents(stat_path, &stat, NULL, NULL) ||
        !(p = strrchr(stat, ')'))) {
        return 0;
    }
    /* fields 14 and 15, counting the name in parentheses as field 2 */
    if (sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
               &utime, &stime) != 2) {
        return 0;
    }
    return utime + stime;
}

/*
 * CPU time used by the vCPU threads so far, in clock ticks.  @threads counts
 * the threads, which the vCPUs share with single-threaded TCG.
 */
static uint64_t vcpu_ticks(int *vcpus, int *threads)
{
    g_autoptr(GHashTable) seen = g_hash_table_new(NULL, NULL);
    CPUState *cpu;
    uint64_t ticks = 0;

    *vcpus = 0;
    CPU_FOREACH(cpu) {
        g_autofree char *path = NULL;

        (*vcpus)++;
        if (!g_hash_table_add(seen, GINT_TO_POINTER(cpu->thread_id))) {
            continue;
        }
        path = g_strdup_printf("/proc/self/task/%d/stat", cpu->thread_id);
        ticks += thread_ticks(path);
    }
    *threads = g_hash_table_size(seen);
    return ticks;
}

static void disk_bytes(uint64_t *rd, uint64_t *wr)
{
    BlockBackend *blk;

    *rd = *wr = 0;
    for (blk = blk_all_next(NULL); blk; blk = blk_all_next(blk)) {
        BlockAcctStats *stats = blk_get_stats(blk);

        *rd += stats->nr_bytes[BLOCK_ACCT_READ];
        *wr += stats->nr_bytes[BLOCK_ACCT_WRITE];
    }
}

typedef struct DrmFile {
    const char *path;           /* e.g. /dev/dri/renderD128 */
    char **fdinfo;              /* lines of its fdinfo */
    const char *driver;         /* e.g. "xe" */
} DrmFile;

/* Name of the DRM driver of @fd, like drmGetVersion() */
static char *drm_driver(int fd)
{
    struct {                    /* struct drm_version */
        int major, minor, patchlevel;
        size_t name_len;
        char *name;
        size_t date_len;
        char *date;
        size_t desc_len;
        char *desc;
    } v = {};
    char name[32];

    v.name = name;
    v.name_len = sizeof(name) - 1;
    if (ioctl(fd, _IOWR('d', 0x00, v), &v) < 0) {
        return NULL;
    }
    name[MIN(v.name_len, sizeof(name) - 1)] = 0;
    return g_strdup(name);
}

/* Value of "@key:\t<value>" in fdinfo @lines, or NULL */
static const char *fdinfo_value(char **lines, const char *key)
{
    size_t len = strlen(key);

    for (; *lines; lines++) {
        if (!strncmp(*lines, key, len) && (*lines)[len] == ':') {
            return *lines + len + 1 + strspn(*lines + len + 1, " \t");
        }
    }
    return NULL;
}

/* Calls @fn for each open DRM file of QEMU */
static void for_each_drm_file(void (*fn)(const DrmFile *file, void *opaque),
                              void *opaque)
{
    g_autoptr(GDir) dir = g_dir_open("/proc/self/fd", 0, NULL);
    const char *fd;

    while (dir && (fd = g_dir_read_name(dir))) {
        g_autofree char *link = g_strdup_printf("/proc/self/fd/%s", fd);
        g_autofree char *target = g_file_read_link(link, NULL);
        g_autofree char *info_path = NULL;
        g_autofree char *info = NULL;
        g_autofree char *driver = NULL;
        g_auto(GStrv) lines = NULL;
        DrmFile file;

        if (!target || !g_str_has_prefix(target, "/dev/dri/")) {
            continue;
        }
        info_path = g_strdup_printf("/proc/self/fdinfo/%s", fd);
        if (!g_file_get_contents(info_path, &info, NULL, NULL)) {
            continue;
        }
        lines = g_strsplit(info, "\n", -1);
        file.path = target;
        file.fdinfo = lines;
        file.driver = fdinfo_value(lines, "drm-driver");
        if (!file.driver) {
            /* drivers without usage statistics, e.g. virtio_gpu */
            driver = drm_driver(atoi(fd));
            file.driver = driver ? driver : "?";
        }
        fn(&file, opaque);
    }
}

/*
 * GPU use comes from the DRM fdinfo of QEMU's own DRM clients, that is its
 * rendering for the guest (virglrenderer) and for the window.  Drivers count
 * the busy time of each engine class ("drm-engine-<class>: <ns> ns"), or its
 * busy cycles against GPU timestamp cycles ("drm-cycles-<class>" and
 * "drm-total-cycles-<class>", xe).
 */
typedef struct GpuSample {
    char driver[16];
    uint64_t ns;                /* busy time */
    uint64_t cycles;            /* busy cycles */
    uint64_t total_cycles;      /* 0 if the driver counts busy time */
    uint64_t capacity;          /* engines in the class */
} GpuSample;

typedef struct GpuScan {
    GHashTable *samples;        /* "<device>\n<class>\n<client>" -> sample */
    char driver[16];            /* the first DRM driver seen */
} GpuScan;

/* Samples of the previous call of sdl2_gui_vm_stats() */
static GHashTable *gpu_prev;

static void gpu_scan_file(const DrmFile *file, void *opaque)
{
    static const char *const counters[] = {
        "drm-engine-capacity-", "drm-engine-", "drm-total-cycles-",
        "drm-cycles-",
    };
    GpuScan *scan = opaque;
    const char *client = fdinfo_value(file->fdinfo, "drm-client-id");
    const char *driver = file->driver;
    const char *device = fdinfo_value(file->fdinfo, "drm-pdev");
    char **l;

    if (!scan->driver[0]) {
        g_strlcpy(scan->driver, driver, sizeof(scan->driver));
    }
    if (!client) {
        return;         /* no usage statistics */
    }
    for (l = file->fdinfo; *l; l++) {
        g_autofree char *key = NULL;
        char name[64];
        uint64_t value;
        GpuSample *s;
        int i;

        if (sscanf(*l, "%63[^:]: %" SCNu64, name, &value) != 2) {
            continue;
        }
        for (i = 0; i < ARRAY_SIZE(counters); i++) {
            if (g_str_has_prefix(name, counters[i])) {
                break;
            }
        }
        if (i == ARRAY_SIZE(counters)) {
            continue;
        }

        key = g_strdup_printf("%s\n%s\n%s", device ?: driver,
                              name + strlen(counters[i]), client);
        s = g_hash_table_lookup(scan->samples, key);
        if (!s) {
            s = g_new0(GpuSample, 1);
            g_strlcpy(s->driver, driver, sizeof(s->driver));
            g_hash_table_insert(scan->samples, g_steal_pointer(&key), s);
        }
        switch (i) {
        case 0:
            s->capacity = value;
            break;
        case 1:
            s->ns = value;
            break;
        case 2:
            s->total_cycles = value;
            break;
        default:
            s->cycles = value;
            break;
        }
    }
}

typedef struct GpuClassUse {
    double use;
    const char *driver;
} GpuClassUse;

/*
 * Use of the busiest engine class of a GPU, summed over QEMU's clients, from
 * 0 to 1, or -1 if unknown.  @driver gets the DRM driver of that GPU.
 */
static double gpu_usage(GHashTable *prev, GHashTable *cur, double seconds,
                        char *driver, size_t driver_len)
{
    g_autoptr(GHashTable) classes = g_hash_table_new_full(g_str_hash,
                                                          g_str_equal,
                                                          g_free, g_free);
    GpuClassUse *busiest = NULL;
    GHashTableIter it;
    gpointer key, value;

    g_hash_table_iter_init(&it, cur);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        GpuSample *now = value, *before = g_hash_table_lookup(prev, key);
        g_autofree char *class = g_strndup(key, strrchr(key, '\n') -
                                           (char *)key);
        GpuClassUse *c;
        double use;

        if (!before) {
            continue;           /* a new client */
        }
        if (now->total_cycles) {
            if (now->total_cycles <= before->total_cycles ||
                now->cycles < before->cycles) {
                continue;
            }
            use = (double)(now->cycles - before->cycles) /
                  (now->total_cycles - before->total_cycles);
        } else {
            if (now->ns < before->ns) {
                continue;
            }
            use = (now->ns - before->ns) / (seconds * 1e9);
        }
        use /= MAX(now->capacity, 1);

        c = g_hash_table_lookup(classes, class);
        if (!c) {
            c = g_new0(GpuClassUse, 1);
            c->driver = now->driver;
            g_hash_table_insert(classes, g_steal_pointer(&class), c);
        }
        c->use += use;
    }

    g_hash_table_iter_init(&it, classes);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        GpuClassUse *c = value;

        if (!busiest || c->use > busiest->use) {
            busiest = c;
        }
    }
    if (!busiest) {
        return -1;
    }
    g_strlcpy(driver, busiest->driver, driver_len);
    return MIN(busiest->use, 1.0);
}

static double rate(uint64_t now, uint64_t before, double seconds)
{
    return now > before ? (now - before) / seconds : 0;
}

bool sdl2_gui_vm_stats(SDL2GuiStats *stats)
{
    static struct {
        bool valid;
        int64_t time;
        uint64_t vcpu_ticks, ticks, rd, wr, rx, tx;
    } prev;
    int64_t time = g_get_monotonic_time();
    double seconds = (time - prev.time) / 1e6;
    bool valid = prev.valid && seconds > 0;
    GpuScan scan = {
        .samples = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, g_free),
    };
    uint64_t vticks, ticks, rd, wr, rx, tx;
    long hz = sysconf(_SC_CLK_TCK);
    int vcpus, threads;

    vticks = vcpu_ticks(&vcpus, &threads);
    ticks = thread_ticks("/proc/self/stat");
    disk_bytes(&rd, &wr);
    qemu_net_guest_bytes(&rx, &tx);
    for_each_drm_file(gpu_scan_file, &scan);

    memset(stats, 0, sizeof(*stats));
    stats->vcpus = vcpus;
    stats->gpu = -1;
    g_strlcpy(stats->gpu_driver, scan.driver, sizeof(stats->gpu_driver));
    if (valid && hz > 0) {
        if (threads) {
            stats->cpu = MIN(rate(vticks, prev.vcpu_ticks, seconds) /
                             (hz * threads), 1.0);
        }
        stats->process_cpu = rate(ticks, prev.ticks, seconds) / hz;
        stats->disk_read = rate(rd, prev.rd, seconds);
        stats->disk_write = rate(wr, prev.wr, seconds);
        stats->net_rx = rate(rx, prev.rx, seconds);
        stats->net_tx = rate(tx, prev.tx, seconds);
        if (gpu_prev) {
            stats->gpu = gpu_usage(gpu_prev, scan.samples, seconds,
                                   stats->gpu_driver,
                                   sizeof(stats->gpu_driver));
        }
    }

    prev.valid = true;
    prev.time = time;
    prev.vcpu_ticks = vticks;
    prev.ticks = ticks;
    prev.rd = rd;
    prev.wr = wr;
    prev.rx = rx;
    prev.tx = tx;
    if (gpu_prev) {
        g_hash_table_unref(gpu_prev);
    }
    gpu_prev = scan.samples;
    return valid;
}

/* Information */

static void info_add(GPtrArray *rows, const char *section, const char *name,
                     char *value)
{
    g_ptr_array_add(rows, g_strdup(section));
    g_ptr_array_add(rows, g_strdup(name));
    g_ptr_array_add(rows, value);
}

static bool prop_bool(Object *obj, const char *name)
{
    return object_property_find(obj, name) &&
           object_property_get_bool(obj, name, NULL);
}

static char *info_firmware(MachineState *ms)
{
    BlockBackend *blk = blk_by_name("pflash0");
    BlockDriverState *bs = blk ? blk_bs(blk) : NULL;
    const char *kernel = ms->kernel_filename;
    char *fw;

    if (bs) {
        fw = g_strdup_printf("UEFI (%s)", bs->filename);
    } else if (object_property_find(OBJECT(ms), "pflash0") &&
               (fw = object_property_get_str(OBJECT(ms), "pflash0", NULL)) &&
               *fw) {
        g_autofree char *node = fw;

        bs = bdrv_find_node(node);
        fw = g_strdup_printf("UEFI (%s)", bs ? bs->filename : node);
    } else if (ms->firmware && (strcasestr(ms->firmware, "ovmf") ||
                                strcasestr(ms->firmware, "edk2") ||
                                strcasestr(ms->firmware, "uefi"))) {
        fw = g_strdup_printf("UEFI (%s)", ms->firmware);
    } else {
        fw = g_strdup_printf("BIOS (%s)", ms->firmware ?: "default");
    }
    if (kernel) {
        g_autofree char *old = fw;

        fw = g_strdup_printf("%s, Linux kernel %s", old, kernel);
    }
    return fw;
}

static void info_machine(GPtrArray *rows)
{
    const char *s = "Virtual machine";
    MachineState *ms = MACHINE(qdev_get_machine());
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CpuTopology *smp = &ms->smp;
    g_autofree char *ram = size_to_str(ms->ram_size);
    char *cpu = g_strdup(ms->cpu_type ? ms->cpu_type : "default");
    char *dash;
    bool running;

    info_add(rows, s, "Name", g_strdup(qemu_name ? qemu_name : "not set"));
    if (qemu_uuid_set) {
        info_add(rows, s, "UUID", qemu_uuid_unparse_strdup(&qemu_uuid));
    }
    info_add(rows, s, "State", g_strdup(sdl2_gui_vm_state(&running)));
    info_add(rows, s, "Machine", g_strdup_printf("%s (%s)",
                                                 mc->desc ?: mc->name,
                                                 mc->name));
    info_add(rows, s, "Accelerator", g_strdup(current_accel_name()));

    /* "host-x86_64-cpu" -> "host" */
    if (g_str_has_suffix(cpu, "-cpu")) {
        cpu[strlen(cpu) - 4] = 0;
        if ((dash = strrchr(cpu, '-'))) {
            *dash = 0;
        }
    }
    info_add(rows, s, "CPU model", cpu);
    info_add(rows, s, "vCPUs",
             g_strdup_printf("%u: %u socket%s, %u core%s, %u thread%s",
                             smp->cpus, smp->sockets,
                             smp->sockets == 1 ? "" : "s", smp->cores,
                             smp->cores == 1 ? "" : "s", smp->threads,
                             smp->threads == 1 ? "" : "s"));
    info_add(rows, s, "Memory",
             ms->memdev ? g_strdup_printf("%s (%s)", ram,
                                          object_get_typename(
                                              OBJECT(ms->memdev)))
                        : g_strdup(ram));
    info_add(rows, s, "Firmware", info_firmware(ms));
    info_add(rows, s, "QEMU", g_strdup_printf("%s, PID %d",
                                              QEMU_FULL_VERSION, getpid()));
}

static bool is_display_device(Object *obj)
{
    return object_dynamic_cast(obj, TYPE_DEVICE) &&
           test_bit(DEVICE_CATEGORY_DISPLAY,
                    DEVICE_GET_CLASS(obj)->categories);
}

typedef struct InfoGraphics {
    GPtrArray *rows;
    bool virgl;                 /* a device renders with virglrenderer */
} InfoGraphics;

static int info_graphics_device(Object *obj, void *opaque)
{
    InfoGraphics *g = opaque;
    GPtrArray *rows = g->rows;
    const char *type = object_get_typename(obj);
    GString *accel;

    if (is_display_device(obj)) {
        BusState *bus = qdev_get_parent_bus(DEVICE(obj));

        /* not the virtio device inside virtio-vga-gl and the like */
        if (!bus || !bus->parent || !is_display_device(OBJECT(bus->parent))) {
            DeviceState *dev = DEVICE(obj);

            info_add(rows, "Graphics", "Device",
                     dev->id ? g_strdup_printf("%s (%s)", type, dev->id)
                             : g_strdup(type));
        }
    }
    if (!object_dynamic_cast(obj, "virtio-gpu-base")) {
        return 0;
    }

    accel = g_string_new("");
    if (strstr(type, "vhost-user")) {
        g_string_append(accel, "in a vhost-user process");
    } else if (strstr(type, "-gl")) {
        g->virgl = true;
        if (prop_bool(obj, "drm_native_context")) {
            g_string_append(accel, "DRM native context and ");
        }
        if (prop_bool(obj, "venus")) {
            g_string_append(accel, "Venus (Vulkan) and ");
        }
        g_string_append(accel, "virgl (OpenGL)");
    } else if (strstr(type, "rutabaga")) {
        g_string_append(accel, "rutabaga");
    } else {
        g_string_append(accel, "none (2D)");
    }
    if (prop_bool(obj, "blob")) {
        g_string_append(accel, ", blob resources");
    }
    if (object_property_find(obj, "hostmem")) {
        uint64_t hostmem = object_property_get_uint(obj, "hostmem", NULL);

        if (hostmem) {
            g_autofree char *size = size_to_str(hostmem);

            g_string_append_printf(accel, ", %s host memory", size);
        }
    }
    info_add(rows, "Graphics", "Acceleration", g_string_free(accel, false));
    return 0;
}

static int find_virglrenderer(struct dl_phdr_info *info, size_t size,
                              void *opaque)
{
    char **path = opaque;

    if (info->dlpi_name && strstr(info->dlpi_name, "libvirglrenderer")) {
        char *real = realpath(info->dlpi_name, NULL);

        *path = g_strdup(real ? real : info->dlpi_name);
        free(real);
        return 1;
    }
    return 0;
}

static void info_drm_driver(const DrmFile *file, void *opaque)
{
    GString *drivers = opaque;
    g_autofree char *entry = g_strdup_printf("%s (%s)", file->driver,
                                             file->path);

    if (!strstr(drivers->str, entry)) {
        g_string_append_printf(drivers, "%s%s", drivers->len ? ", " : "",
                               entry);
    }
}

static void info_graphics(GPtrArray *rows)
{
    InfoGraphics g = { .rows = rows };
    g_autofree char *virgl = NULL;
    GString *drivers = g_string_new("");

    object_child_foreach_recursive(object_get_root(), info_graphics_device,
                                   &g);
    if (g.virgl) {
        dl_iterate_phdr(find_virglrenderer, &virgl);
    }
    if (virgl) {
#ifdef VIRGL_VERSION_MAJOR
        info_add(rows, "Graphics", "virglrenderer",
                 g_strdup_printf("built with %d.%d.%d, using %s",
                                 VIRGL_VERSION_MAJOR, VIRGL_VERSION_MINOR,
                                 VIRGL_VERSION_MICRO, virgl));
#else
        info_add(rows, "Graphics", "virglrenderer", g_strdup(virgl));
#endif
    }
    for_each_drm_file(info_drm_driver, drivers);
    info_add(rows, "Graphics", "Host GPU (DRM)",
             drivers->len ? g_string_free(drivers, false)
                          : (g_string_free(drivers, true),
                             g_strdup("not used")));
}

/* Type of @dev, or of the proxy around it for a virtio device */
static const char *device_type(DeviceState *dev)
{
    BusState *bus = qdev_get_parent_bus(dev);

    if (object_dynamic_cast(OBJECT(dev), "virtio-device") && bus &&
        bus->parent) {
        return object_get_typename(OBJECT(bus->parent));
    }
    return object_get_typename(OBJECT(dev));
}

/*
 * Only cached values and stat(): the menu builds its frames with this, and
 * the block layer calls that could wait for I/O would poll the main loop
 * in the middle of a frame.
 */
static void info_disks(GPtrArray *rows)
{
    BlockBackend *blk;

    GRAPH_RDLOCK_GUARD_MAINLOOP();
    for (blk = blk_all_next(NULL); blk; blk = blk_all_next(blk)) {
        BlockDriverState *bs = blk_bs(blk);
        DeviceState *dev = blk_get_attached_dev(blk);
        const char *name = blk_name(blk);
        g_autofree char *size = NULL;
        QEMUSnapshotInfo *sn = NULL;
        GString *value;
        struct stat st;
        int snapshots;

        if (!*name) {
            name = dev && dev->id ? dev->id : bs ? bdrv_get_node_name(bs) : "";
        }
        if (!*name) {
            continue;
        }
        if (!bs || !bdrv_is_inserted(bs)) {
            info_add(rows, "Disks", name,
                     g_strdup_printf("no medium%s%s", dev ? ", " : "",
                                     dev ? device_type(dev) : ""));
            continue;
        }
        value = g_string_new(bs->filename);
        size = size_to_str(bs->total_sectors * BDRV_SECTOR_SIZE);
        g_string_append_printf(value, "\n%s, %s", bdrv_get_format_name(bs) ?:
                               "?", size);
        if (stat(bs->filename, &st) == 0 && S_ISREG(st.st_mode)) {
            g_autofree char *used = size_to_str((uint64_t)st.st_blocks * 512);

            g_string_append_printf(value, " (%s on the host)", used);
        }
        if (!blk_is_writable(blk)) {
            g_string_append(value, ", read-only");
        }
        if (dev) {
            g_string_append_printf(value, ", %s", device_type(dev));
        }
        snapshots = bdrv_snapshot_list(bs, &sn);
        g_free(sn);
        if (snapshots > 0) {
            g_string_append_printf(value, ", %d snapshot%s", snapshots,
                                   snapshots == 1 ? "" : "s");
        }
        info_add(rows, "Disks", name, g_string_free(value, false));
    }
}

static void info_nic(NICState *nic, void *opaque)
{
    GPtrArray *rows = opaque;
    NetClientState *nc = qemu_get_queue(nic);
    const uint8_t *mac = nic->conf->macaddr.a;
    NetClientState *peer = nc->peer;
    g_autofree char *to = NULL;

    if (!peer) {
        to = g_strdup("not connected");
    } else if (peer->info->type == NET_CLIENT_DRIVER_HUBPORT) {
        to = g_strdup("connected to a hub");
    } else {
        to = g_strdup_printf("%s backend",
                             NetClientDriver_str(peer->info->type));
        if (peer->name[0] != '#') {
            g_autofree char *type = to;

            to = g_strdup_printf("%s \"%s\"", type, peer->name);
        }
    }
    info_add(rows, "Network", nc->model ? nc->model : nc->name,
             g_strdup_printf("%02x:%02x:%02x:%02x:%02x:%02x, %s",
                             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                             to));
}

static void info_network(GPtrArray *rows)
{
    NetClientState *ncs[64];
    int i, n;

    qemu_foreach_nic(info_nic, rows);
    n = qemu_find_net_clients_except(NULL, ncs, NET_CLIENT_DRIVER_NIC,
                                     ARRAY_SIZE(ncs));
    /* the backends that are not connected straight to a NIC */
    for (i = 0; i < n; i++) {
        NetClientState *peer = ncs[i]->peer;

        if (ncs[i]->info->type != NET_CLIENT_DRIVER_HUBPORT &&
            (!peer || peer->info->type != NET_CLIENT_DRIVER_NIC)) {
            const char *name = ncs[i]->name;

            info_add(rows, "Network", "Backend",
                     g_strdup_printf("%s%s%s%s",
                                     NetClientDriver_str(ncs[i]->info->type),
                                     name[0] != '#' ? " \"" : "",
                                     name[0] != '#' ? name : "",
                                     name[0] != '#' ? "\"" : ""));
        }
    }
}

static int info_usb_device(Object *obj, void *opaque)
{
    GPtrArray *rows = opaque;

    if (object_dynamic_cast(obj, TYPE_DEVICE) &&
        test_bit(DEVICE_CATEGORY_USB, DEVICE_GET_CLASS(obj)->categories) &&
        !object_dynamic_cast(obj, TYPE_USB_DEVICE)) {
        info_add(rows, "USB", "Controller", g_strdup(object_get_typename(obj)));
    } else if (object_dynamic_cast(obj, TYPE_USB_DEVICE)) {
        USBDevice *dev = USB_DEVICE(obj);
        const char *desc = usb_device_get_product_desc(dev);
        g_autofree char *port = g_strdup_printf("Port %s", dev->port ?
                                                dev->port->path : "-");

        info_add(rows, "USB", port,
                 g_strdup_printf("%s (%s)", desc && *desc ? desc : "?",
                                 object_get_typename(obj)));
    }
    return 0;
}

static int info_sound_device(Object *obj, void *opaque)
{
    if (object_dynamic_cast(obj, TYPE_DEVICE) &&
        test_bit(DEVICE_CATEGORY_SOUND, DEVICE_GET_CLASS(obj)->categories)) {
        info_add(opaque, "Audio", "Device", g_strdup(object_get_typename(obj)));
    }
    return 0;
}

static void info_audio(GPtrArray *rows)
{
    AudiodevList *list = qmp_query_audiodevs(NULL), *l;

    object_child_foreach_recursive(object_get_root(), info_sound_device, rows);
    for (l = list; l; l = l->next) {
        info_add(rows, "Audio", l->value->id,
                 g_strdup(AudiodevDriver_str(l->value->driver)));
    }
    qapi_free_AudiodevList(list);
}

char **sdl2_gui_vm_info(void)
{
    GPtrArray *rows = g_ptr_array_new();
    struct utsname host;

    info_machine(rows);
    info_graphics(rows);
    info_disks(rows);
    info_network(rows);
    object_child_foreach_recursive(object_get_root(), info_usb_device, rows);
    info_audio(rows);
    if (uname(&host) == 0) {
        info_add(rows, "Host", "Kernel", g_strdup_printf("%s %s %s",
                                                         host.sysname,
                                                         host.release,
                                                         host.machine));
    }
    g_ptr_array_add(rows, NULL);
    return (char **)g_ptr_array_free(rows, false);
}

void sdl2_gui_vm_info_free(char **info)
{
    g_strfreev(info);
}
