/*
 * QEMU SDL display: in-window control menu
 *
 * The menu is drawn with Dear ImGui by sdl2-gui.cpp.  C++ cannot include
 * most QEMU headers, so this header, which only uses plain C and SDL types,
 * is the whole interface between the menu, the SDL display (sdl2.c,
 * sdl2-gl.c, sdl2-2d.c) and the VM operations behind the menu entries
 * (sdl2-gui-vm.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_SDL2_GUI_H
#define UI_SDL2_GUI_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The menu, implemented in sdl2-gui.cpp.  It lives in a single window, the
 * one of the first graphical console.  All of these run in the main loop.
 */

/* Attach the menu to @window, drawn with @glctx (OpenGL) or @renderer */
bool sdl2_gui_init(SDL_Window *window, SDL_GLContext glctx,
                   SDL_Renderer *renderer);
void sdl2_gui_fini(void);
SDL_Window *sdl2_gui_window(void);
/* Show or hide the menu bar (Ctrl-Alt-M); true if it is now shown */
bool sdl2_gui_toggle(void);
/*
 * Offer an input event to the menu before the display handles it.  Returns
 * true if the menu takes the event and the guest must not see it.
 * @pointer_free: the host pointer is visible and moves freely over the
 * window, i.e. it is not grabbed in relative mode.
 */
bool sdl2_gui_process_event(const SDL_Event *ev, bool pointer_free);
/* The pointer is over the menu: show the host cursor, not the guest's */
bool sdl2_gui_wants_pointer(void);
/* Window rows at the top covered by the menu bar, 0 while it is hidden */
int sdl2_gui_bar_height(void);
/*
 * Window rows to keep free for the menu bar, above the guest display: the
 * bar is docked while it is shown.  0 while it is hidden, and with the 2D
 * renderer, where it floats over the guest display.
 */
int sdl2_gui_docked_height(void);
/*
 * Build the next frame of the menu, once per display refresh.  Returns true
 * if it differs from the previous one and the window needs to be redrawn.
 */
bool sdl2_gui_update(void);
/* Draw the menu on top of the guest display, right before presenting it */
void sdl2_gui_render(void);
/* Runs the queued menu action, called back by sdl2_gui_vm_schedule() */
void sdl2_gui_run_scheduled(void);
/* Result of sdl2_gui_vm_usb_grant(); @errmsg is NULL if the user cancelled */
typedef struct SDL2GuiUsbDevice SDL2GuiUsbDevice;
void sdl2_gui_usb_access_done(const SDL2GuiUsbDevice *dev, bool granted,
                              const char *errmsg);

/* Display operations for the menu, implemented in sdl2.c */
bool sdl2_gui_display_is_fullscreen(void);
void sdl2_gui_display_toggle_fullscreen(void);
/* Let go of the keyboard and pointer, e.g. for another window to use them */
void sdl2_gui_display_release_grab(void);
/* Prefix of the display hotkeys, e.g. "Ctrl+Alt+" */
const char *sdl2_gui_display_hotkey(void);
/* Refresh rate of the host display showing the window, in mHz, or 0 */
uint32_t sdl2_gui_display_refresh_rate(void);
/* Number of guest frames shown so far */
uint64_t sdl2_gui_display_frames(void);
/* Size of the guest display, false if there is none */
bool sdl2_gui_display_guest_size(int *w, int *h);

/*
 * VM operations, implemented in sdl2-gui-vm.c.  Error messages returned
 * through @errmsg are allocated and must be freed with sdl2_gui_free().
 */
void sdl2_gui_free(void *ptr);
/* Run sdl2_gui_run_scheduled() soon, from the main loop */
void sdl2_gui_vm_schedule(void);

/* Run state name, e.g. "running" or "paused" */
const char *sdl2_gui_vm_state(bool *running);

/*
 * What identifies this VM across runs, to remember per-VM settings: its
 * name, else its UUID, else the file of its first writable disk.
 */
char *sdl2_gui_vm_id(void);

typedef struct SDL2GuiStats {
    double cpu;                 /* vCPU use, averaged over the vCPUs, 0-1 */
    int vcpus;
    double process_cpu;         /* CPU time used by QEMU, in host CPUs */
    double disk_read;           /* bytes per second */
    double disk_write;
    double net_rx;              /* bytes per second, received by the guest */
    double net_tx;
    double gpu;                 /* host GPU use by QEMU, 0-1, or -1 */
    char gpu_driver[16];        /* DRM driver of that GPU, e.g. "xe", or "" */
} SDL2GuiStats;

/*
 * Usage since the previous call; false on the first call, which only
 * takes the first sample.
 */
bool sdl2_gui_vm_stats(SDL2GuiStats *stats);

/*
 * Information about the VM: a NULL terminated list of strings, three per
 * row (section, name, value).  Free with sdl2_gui_vm_info_free().
 */
char **sdl2_gui_vm_info(void);
void sdl2_gui_vm_info_free(char **info);
bool sdl2_gui_vm_pause(char **errmsg);
bool sdl2_gui_vm_resume(char **errmsg);
void sdl2_gui_vm_reset(void);
/* Press the (ACPI) power button */
void sdl2_gui_vm_powerdown(void);
/* Turn the VM off and quit, like closing the window */
void sdl2_gui_vm_quit(void);
/* Press and release Ctrl+Alt+@key in the guest */
void sdl2_gui_vm_send_ctrl_alt(SDL_Scancode key);

typedef struct SDL2GuiSnapshot {
    char name[256];
    int64_t date;               /* seconds since the epoch */
    uint64_t vm_state_size;     /* 0 for a disk-only snapshot */
} SDL2GuiSnapshot;

typedef struct SDL2GuiSnapshotList {
    SDL2GuiSnapshot *snapshots; /* newest first */
    int count;
    char *disks;                /* disks in snapshots, NULL if none */
    char *skipped;              /* writable disks that cannot be, or NULL */
    char *no_state_reason;      /* why RAM cannot be saved, or NULL */
} SDL2GuiSnapshotList;

SDL2GuiSnapshotList *sdl2_gui_vm_snapshots(void);
void sdl2_gui_vm_snapshots_free(SDL2GuiSnapshotList *list);
/*
 * With @with_state the snapshot also holds RAM and device state; otherwise
 * it only holds the disks, and reverting to it restarts the guest.
 */
bool sdl2_gui_vm_snapshot_save(const char *name, bool with_state,
                               char **errmsg);
bool sdl2_gui_vm_snapshot_revert(const char *name, char **errmsg);
bool sdl2_gui_vm_snapshot_delete(const char *name, char **errmsg);

struct SDL2GuiUsbDevice {
    char name[128];             /* manufacturer and product */
    char port[32];              /* sysfs name, e.g. "3-2.1" */
    unsigned bus, addr;
    unsigned vendor_id, product_id;
    bool attached;              /* passed through to the guest */
    bool hid;                   /* keyboard, mouse or other HID device */
    bool accessible;            /* the device node can be opened */
};

typedef struct SDL2GuiUsbList {
    SDL2GuiUsbDevice *devices;
    int count;
    char *unavailable;          /* why passthrough is impossible, or NULL */
} SDL2GuiUsbList;

SDL2GuiUsbList *sdl2_gui_vm_usb_devices(void);
void sdl2_gui_vm_usb_devices_free(SDL2GuiUsbList *list);
bool sdl2_gui_vm_usb_attach(const SDL2GuiUsbDevice *dev, char **errmsg);
bool sdl2_gui_vm_usb_detach(const SDL2GuiUsbDevice *dev, char **errmsg);
/*
 * Ask for read/write access to the device node of @dev for the current
 * user: polkit asks for authentication, then an ACL is added to the node,
 * which lasts until the device is unplugged.  Returns false if that cannot
 * be started; the outcome goes to sdl2_gui_usb_access_done().
 */
bool sdl2_gui_vm_usb_grant(const SDL2GuiUsbDevice *dev, char **errmsg);

#ifdef __cplusplus
}
#endif

#endif /* UI_SDL2_GUI_H */
