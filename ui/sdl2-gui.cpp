/*
 * QEMU SDL display: in-window control menu
 *
 * A menu bar drawn with Dear ImGui at the top of the SDL window, with usage
 * statistics on its right and a window with information about the VM.
 * Shown with the display hotkey modifier plus M (Ctrl-Alt-M by default),
 * it is docked: the guest display is drawn below it and the guest is asked
 * for a display of that size.  Whether it is shown is remembered per VM.
 * While nothing is shown, no frame is built and nothing is drawn.
 *
 * Menu actions do not run while a frame is built: they are queued and run
 * from the main loop afterwards, since some block for a while (snapshots)
 * and some call back into the display (reset, fullscreen).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config-host.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <glib.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#ifdef CONFIG_OPENGL
#include <epoxy/gl.h>
#include "imgui_impl_opengl3.h"
#endif

#include "ui/sdl2-gui.h"

#if !SDL_VERSION_ATLEAST(2, 0, 18)
#define SDL_GetTicks64 SDL_GetTicks
#endif

static const float kFontSize = 16.0f;      /* at 100% */
static const Uint64 kUsbRefresh = 1000;    /* ms between USB list updates */
static const Uint64 kStatsPeriod = 1000;   /* ms between statistics */
static const Uint64 kInfoRefresh = 2000;   /* ms between information updates */
static const size_t kMaxVms = 64;          /* VMs whose state is remembered */
static const float kScales[] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f };

/* Fonts tried before the one built into Dear ImGui */
static const char *const kFontPaths[] = {
    "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",
    "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
};

enum class Op {
    None,
    Pause,
    Resume,
    Reset,
    PowerButton,
    ForceOff,
    SendCtrlAlt,
    Fullscreen,
    SnapshotSave,
    SnapshotRevert,
    SnapshotDelete,
    UsbAttach,
    UsbDetach,
    UsbGrant,
};

/* A menu action, run from the main loop after the frame that asked for it */
struct Request {
    Op op = Op::None;
    std::string name;               /* snapshot */
    bool with_state = false;        /* snapshot with RAM and device state */
    SDL2GuiUsbDevice usb = {};
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    std::string busy;               /* shown while it runs, if not empty */
};

struct Settings {
    bool menu_bar = true;           /* shown for a VM that never ran */
    bool show_stats = true;         /* usage statistics in the menu bar */
    float scale = 0.0f;             /* menu size, 0 for automatic */
};

/* A row of the information window */
typedef std::array<std::string, 3> InfoRow;     /* section, name, value */

struct Gui {
    ImGuiContext *ctx = nullptr;
    SDL_Window *window = nullptr;
    Uint32 window_id = 0;
    SDL_GLContext glctx = nullptr;
    SDL_Renderer *renderer = nullptr;
    void *font_data = nullptr;

    Settings settings;
    float auto_scale = 1.0f;        /* menu size for the desktop */
    float style_scale = 0;          /* menu size the style was made for */

    /* this VM in the settings */
    bool vm_loaded = false;
    std::string vm_id;
    std::string vm_group;
    bool vm_menu_bar = true;        /* shown when the VM starts */

    bool visible = false;           /* menu bar shown */
    bool sticky = false;            /* shown on request (docked), not only
                                       for a dialog */
    bool close_popups = false;      /* menus left open when it was hidden */
    bool drawn = false;             /* the draw data has something */
    uint64_t hash = 0;              /* of the draw data */
    float bar_height = 0;           /* in ImGui coordinates */
    int bar_rows = 0;               /* in window rows, 0 while hidden */

    bool pointer_free = false;      /* not grabbed in relative mode */
    Uint32 guest_buttons = 0;       /* pressed on the guest, released there */
    bool wants_pointer = false;
    bool popup_open = false;

    Request request;                /* queued or running */
    int request_age = 0;            /* frames since it was queued */
    bool scheduled = false;
    Request confirm;                /* waiting for confirmation */
    std::string confirm_title;
    std::string confirm_text;
    std::string error;

    bool snapshot_dialog = false;
    char snapshot_name[256] = "";
    bool snapshot_with_state = false;
    SDL2GuiSnapshotList *snapshots = nullptr;
    bool snapshots_open = false;

    SDL2GuiUsbList *usb = nullptr;
    Uint64 usb_time = 0;
    bool usb_open = false;
    bool usb_waiting = false;       /* for access to usb_waiting_dev */
    SDL2GuiUsbDevice usb_waiting_dev = {};

    SDL2GuiStats stats = {};
    bool stats_valid = false;
    Uint64 stats_time = 0;
    uint64_t stats_frames = 0;
    float fps = 0;

    bool info_open = false;
    bool info_focus = false;
    std::vector<InfoRow> info;
    Uint64 info_time = 0;
};

static Gui gui;

/*
 * Settings, kept in $XDG_CONFIG_HOME/qemu/sdl-gui.ini: those of the menu in
 * [menu], and in a [vm <hash of its ID>] group per VM whether its menu bar
 * was shown, so that it starts that way next time.
 */

static char *settings_path()
{
    return g_build_filename(g_get_user_config_dir(), "qemu", "sdl-gui.ini",
                            nullptr);
}

static GKeyFile *settings_read()
{
    char *path = settings_path();
    GKeyFile *kf = g_key_file_new();

    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, nullptr);
    g_free(path);
    return kf;
}

static bool settings_bool(GKeyFile *kf, const char *group, const char *key,
                          bool def)
{
    GError *err = nullptr;
    bool val = g_key_file_get_boolean(kf, group, key, &err);

    if (err) {
        g_error_free(err);
        return def;
    }
    return val;
}

static void settings_load()
{
    GKeyFile *kf = settings_read();
    Settings &s = gui.settings;
    GError *err = nullptr;
    double scale = g_key_file_get_double(kf, "menu", "scale", &err);

    s.menu_bar = settings_bool(kf, "menu", "menu-bar", s.menu_bar);
    s.show_stats = settings_bool(kf, "menu", "statistics", s.show_stats);
    if (err) {
        g_error_free(err);
    } else if (scale == 0 || (scale >= 0.5 && scale <= 4.0)) {
        s.scale = scale;
    }
    g_key_file_free(kf);
}

/* Forget the VMs that ran longest ago, beyond kMaxVms */
static void settings_prune(GKeyFile *kf)
{
    gchar **groups = g_key_file_get_groups(kf, nullptr);
    std::vector<std::pair<gint64, std::string>> vms;

    for (gchar **g = groups; *g; g++) {
        if (g_str_has_prefix(*g, "vm ")) {
            vms.emplace_back(g_key_file_get_int64(kf, *g, "last-run",
                                                  nullptr), *g);
        }
    }
    g_strfreev(groups);
    if (vms.size() > kMaxVms) {
        std::sort(vms.begin(), vms.end());
        for (size_t i = 0; i < vms.size() - kMaxVms; i++) {
            g_key_file_remove_group(kf, vms[i].second.c_str(), nullptr);
        }
    }
}

static void settings_save()
{
    char *path = settings_path();
    char *dir = g_path_get_dirname(path);
    /* re-read: another QEMU may have saved the state of its VM meanwhile */
    GKeyFile *kf = settings_read();
    const Settings &s = gui.settings;
    const char *vm = gui.vm_group.c_str();

    /* settings of earlier versions */
    g_key_file_remove_key(kf, "menu", "always-show", nullptr);
    g_key_file_remove_key(kf, "menu", "frame-rate", nullptr);
    g_key_file_remove_key(kf, "menu", "reveal-at-top-edge", nullptr);

    g_key_file_set_boolean(kf, "menu", "menu-bar", s.menu_bar);
    g_key_file_set_boolean(kf, "menu", "statistics", s.show_stats);
    g_key_file_set_double(kf, "menu", "scale", s.scale);
    if (*vm) {
        g_key_file_set_string(kf, vm, "id", gui.vm_id.c_str());
        g_key_file_set_boolean(kf, vm, "menu-bar", gui.vm_menu_bar);
        g_key_file_set_int64(kf, vm, "last-run",
                             g_get_real_time() / G_USEC_PER_SEC);
    }
    settings_prune(kf);
    if (g_mkdir_with_parents(dir, 0700) == 0) {
        g_key_file_save_to_file(kf, path, nullptr);
    }
    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

/* Whether the menu bar of this VM was shown when it last ran */
static bool vm_menu_bar_load()
{
    char *id = sdl2_gui_vm_id();
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
    GKeyFile *kf = settings_read();

    gui.vm_id = id;
    gui.vm_group = std::string("vm ") + std::string(hash).substr(0, 16);
    gui.vm_menu_bar = settings_bool(kf, gui.vm_group.c_str(), "menu-bar",
                                    gui.settings.menu_bar);
    g_key_file_free(kf);
    g_free(hash);
    sdl2_gui_free(id);
    return gui.vm_menu_bar;
}

/* The user showed or hid the menu bar: the VM starts that way next time */
static void vm_menu_bar_save(bool shown)
{
    gui.vm_menu_bar = shown;
    gui.settings.menu_bar = shown;      /* and so do new VMs */
    settings_save();
}

/*
 * Scale of the desktop showing the window, e.g. 2 at 200%, which SDL
 * reports as the DPI of the display over 96.  Where SDL windows use scaled
 * units rather than pixels, the compositor already scales the window, menu
 * included; the display bounds, in those units, are then smaller than the
 * display mode, in pixels, and the ratio of the two cancels the scale out.
 */
static float desktop_scale()
{
    int display = SDL_GetWindowDisplayIndex(gui.window);
    SDL_DisplayMode mode;
    SDL_Rect bounds;
    float dpi = 0, scale;

    if (display < 0 ||
        SDL_GetDisplayDPI(display, nullptr, &dpi, nullptr) < 0 || dpi <= 0) {
        return 1;
    }
    scale = dpi / 96;
    if (SDL_GetDisplayBounds(display, &bounds) == 0 &&
        SDL_GetDesktopDisplayMode(display, &mode) == 0 &&
        bounds.w > 0 && mode.w > 0) {
        scale *= (float)bounds.w / mode.w;
    }
    return scale;
}

static float auto_scale()
{
    float scale = desktop_scale();

    if (gui.renderer) {
        int lw, lh, ww, wh;

        /* the 2D display draws the menu at the logical size of the guest */
        SDL_RenderGetLogicalSize(gui.renderer, &lw, &lh);
        SDL_GetWindowSize(gui.window, &ww, &wh);
        if (lw > 0 && ww > 0) {
            scale *= (float)lw / ww;
        }
    }
    scale = roundf(scale * 4) / 4;
    return scale < 0.5f ? 0.5f : scale > 4 ? 4 : scale;
}

static float menu_scale()
{
    return gui.settings.scale > 0 ? gui.settings.scale : gui.auto_scale;
}

static void apply_style(float scale)
{
    ImGuiStyle style;
    ImVec4 *c = style.Colors;

    ImGui::StyleColorsDark(&style);
    style.WindowPadding = ImVec2(12, 10);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(10, 6);
    style.WindowRounding = 6;
    style.PopupRounding = 4;
    style.FrameRounding = 4;
    style.GrabRounding = 4;
    style.PopupBorderSize = 1;
    c[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.10f, 0.11f, 0.94f);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.13f, 0.97f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
    style.ScaleAllSizes(scale);
    style.FontSizeBase = kFontSize;
    style.FontScaleMain = scale;
    ImGui::GetStyle() = style;
}

static void load_font()
{
    ImGuiIO &io = ImGui::GetIO();

    for (const char *path : kFontPaths) {
        ImFontConfig cfg;
        gchar *data;
        gsize len;

        if (!g_file_get_contents(path, &data, &len, nullptr)) {
            continue;
        }
        /* the atlas uses the data for as long as it exists */
        cfg.FontDataOwnedByAtlas = false;
        if (io.Fonts->AddFontFromMemoryTTF(data, (int)len, 0, &cfg)) {
            gui.font_data = data;
            return;
        }
        g_free(data);
    }
    io.Fonts->AddFontDefaultVector();
}

/* Showing and hiding */

static bool dialog_open()
{
    return gui.request.op != Op::None || !gui.error.empty() ||
           gui.confirm.op != Op::None || gui.snapshot_dialog ||
           gui.usb_waiting;
}

static void show(bool sticky)
{
    ImGuiIO &io = ImGui::GetIO();
    int x, y;

    if (!gui.visible) {
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        /* buttons held now belong to the guest */
        gui.guest_buttons = SDL_GetMouseState(&x, &y);
        if (gui.pointer_free) {
            float fx = x, fy = y;

#if SDL_VERSION_ATLEAST(2, 0, 18)
            if (gui.renderer) {
                SDL_RenderWindowToLogical(gui.renderer, x, y, &fx, &fy);
            }
#endif
            io.AddMousePosEvent(fx, fy);
        }
    }
    gui.visible = true;
    gui.sticky = sticky;
}

static void hide()
{
    gui.visible = false;
    gui.sticky = false;
    gui.close_popups = true;
    gui.info_open = false;
}

/*
 * A dialog shows the menu bar while it is hidden, e.g. to report an error
 * that came late; the bar goes away again with the dialog.
 */
static void update_visibility()
{
    if (dialog_open()) {
        show(gui.sticky);
    } else if (gui.visible && !gui.sticky && !gui.popup_open) {
        hide();
    }
}

/* Requests */

static Request make_request(Op op)
{
    Request req;

    req.op = op;
    return req;
}

static void queue(const Request &req)
{
    if (gui.request.op != Op::None) {
        return;     /* one at a time */
    }
    gui.request = req;
    gui.request_age = 0;
    gui.scheduled = false;
}

static void schedule_request()
{
    gui.scheduled = true;
    sdl2_gui_vm_schedule();
}

static void ask(const char *title, const std::string &text,
                const Request &req)
{
    gui.confirm = req;
    gui.confirm_title = title;
    gui.confirm_text = text;
}

static void forget_lists()
{
    sdl2_gui_vm_snapshots_free(gui.snapshots);
    gui.snapshots = nullptr;
    sdl2_gui_vm_usb_devices_free(gui.usb);
    gui.usb = nullptr;
}

void sdl2_gui_run_scheduled(void)
{
    Request req = gui.request;
    char *err = nullptr;
    bool ok = true;

    if (!gui.ctx || req.op == Op::None) {
        return;
    }

    switch (req.op) {
    case Op::None:
        break;
    case Op::Pause:
        ok = sdl2_gui_vm_pause(&err);
        break;
    case Op::Resume:
        ok = sdl2_gui_vm_resume(&err);
        break;
    case Op::Reset:
        sdl2_gui_vm_reset();
        break;
    case Op::PowerButton:
        sdl2_gui_vm_powerdown();
        break;
    case Op::ForceOff:
        sdl2_gui_vm_quit();
        break;
    case Op::SendCtrlAlt:
        sdl2_gui_vm_send_ctrl_alt(req.key);
        break;
    case Op::Fullscreen:
        sdl2_gui_display_toggle_fullscreen();
        break;
    case Op::SnapshotSave:
        ok = sdl2_gui_vm_snapshot_save(req.name.c_str(), req.with_state,
                                       &err);
        break;
    case Op::SnapshotRevert:
        ok = sdl2_gui_vm_snapshot_revert(req.name.c_str(), &err);
        break;
    case Op::SnapshotDelete:
        ok = sdl2_gui_vm_snapshot_delete(req.name.c_str(), &err);
        break;
    case Op::UsbAttach:
        ok = sdl2_gui_vm_usb_attach(&req.usb, &err);
        break;
    case Op::UsbDetach:
        ok = sdl2_gui_vm_usb_detach(&req.usb, &err);
        break;
    case Op::UsbGrant:
        /* answered by sdl2_gui_usb_access_done() */
        ok = sdl2_gui_vm_usb_grant(&req.usb, &err);
        if (ok && gui.ctx) {
            gui.usb_waiting = true;
            gui.usb_waiting_dev = req.usb;
            /* the authentication dialog needs the keyboard and pointer */
            sdl2_gui_display_release_grab();
        }
        break;
    }

    /* the menu may be gone, e.g. if its window was destroyed meanwhile */
    if (gui.ctx) {
        gui.request = Request();
        gui.scheduled = false;
        if (!ok) {
            gui.error = err ? err : "The operation failed.";
        }
        forget_lists();
    }
    sdl2_gui_free(err);
}

void sdl2_gui_usb_access_done(const SDL2GuiUsbDevice *dev, bool granted,
                              const char *errmsg)
{
    Request req = make_request(Op::UsbAttach);

    if (!gui.ctx) {
        return;
    }
    gui.usb_waiting = false;
    forget_lists();
    if (granted) {
        /* what access was asked for */
        req.usb = *dev;
        req.usb.accessible = true;
        queue(req);
    } else if (errmsg) {
        gui.error = errmsg;
    }
}

/* Menus */

static std::string hotkey(const char *key)
{
    return std::string(sdl2_gui_display_hotkey()) + key;
}

static void note(const char *text)
{
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 20);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

static std::string format_date(int64_t date)
{
    GDateTime *dt = g_date_time_new_from_unix_local(date);
    char *str = dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M") : nullptr;
    std::string res = str ? str : "?";

    g_free(str);
    if (dt) {
        g_date_time_unref(dt);
    }
    return res;
}

static void machine_menu()
{
    static const struct {
        const char *label;
        SDL_Scancode key;
    } keys[] = {
        { "Ctrl+Alt+Del", SDL_SCANCODE_DELETE },
        { "Ctrl+Alt+Backspace", SDL_SCANCODE_BACKSPACE },
    };
    bool running;
    const char *state;

    if (!ImGui::BeginMenu("Machine")) {
        return;
    }
    state = sdl2_gui_vm_state(&running);
    if (running) {
        if (ImGui::MenuItem("Pause")) {
            queue(make_request(Op::Pause));
        }
    } else if (ImGui::MenuItem(strcmp(state, "prelaunch") ? "Resume"
                                                          : "Start")) {
        queue(make_request(Op::Resume));
    }

    if (ImGui::BeginMenu("Send Keys")) {
        for (const auto &k : keys) {
            if (ImGui::MenuItem(k.label)) {
                Request req = make_request(Op::SendCtrlAlt);

                req.key = k.key;
                queue(req);
            }
        }
        ImGui::Separator();
        for (int i = 0; i < 12; i++) {
            char label[32];

            snprintf(label, sizeof(label), "Ctrl+Alt+F%d", i + 1);
            if (ImGui::MenuItem(label)) {
                Request req = make_request(Op::SendCtrlAlt);

                req.key = (SDL_Scancode)(SDL_SCANCODE_F1 + i);
                queue(req);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Information")) {
        gui.info_open = true;
        gui.info_focus = true;
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Shut Down")) {
        queue(make_request(Op::PowerButton));
    }
    ImGui::SetItemTooltip("Press the power button, so that the guest shuts "
                          "down cleanly");
    if (ImGui::MenuItem("Reset...")) {
        ask("Reset", "Reset the virtual machine? Anything unsaved in the "
            "guest is lost.", make_request(Op::Reset));
    }
    if (ImGui::MenuItem("Force Off...")) {
        ask("Force Off", "Turn the virtual machine off right away, like "
            "pulling the power plug, and quit? Anything unsaved in the guest "
            "is lost.", make_request(Op::ForceOff));
    }
    ImGui::SetItemTooltip("Pull the plug, for a guest that does not respond "
                          "to Shut Down");
    ImGui::EndMenu();
}

static void open_snapshot_dialog()
{
    GDateTime *now = g_date_time_new_now_local();
    char *name = g_date_time_format(now, "snap-%Y%m%d-%H%M%S");

    g_strlcpy(gui.snapshot_name, name, sizeof(gui.snapshot_name));
    g_free(name);
    g_date_time_unref(now);
    gui.snapshot_with_state = !gui.snapshots->no_state_reason;
    gui.snapshot_dialog = true;
}

static void snapshot_item(const SDL2GuiSnapshot &s)
{
    const SDL2GuiSnapshotList *list = gui.snapshots;
    bool with_state = s.vm_state_size && !list->no_state_reason;
    std::string name = std::string("'") + s.name + "'";

    if (!ImGui::BeginMenu(s.name)) {
        return;
    }
    ImGui::TextDisabled("%s, %s", format_date(s.date).c_str(),
                        s.vm_state_size ? "with memory" : "disks only");
    if (ImGui::MenuItem("Revert...")) {
        Request req = make_request(Op::SnapshotRevert);
        std::string text = "Revert to " + name + "? ";

        if (with_state) {
            text += "The VM goes back to the moment the snapshot was taken.";
        } else {
            text += "The disks go back to the snapshot and the VM restarts "
                    "from them.";
        }
        text += " Its current state is lost.";
        req.name = s.name;
        req.busy = "Reverting to " + name + "...";
        ask("Revert", text, req);
    }
    if (ImGui::MenuItem("Delete...")) {
        Request req = make_request(Op::SnapshotDelete);

        req.name = s.name;
        req.busy = "Deleting " + name + "...";
        ask("Delete", "Delete snapshot " + name + "? This cannot be undone.",
            req);
    }
    ImGui::EndMenu();
}

static void snapshots_menu()
{
    const SDL2GuiSnapshotList *list;

    if (!ImGui::BeginMenu("Snapshots")) {
        gui.snapshots_open = false;
        return;
    }
    if (!gui.snapshots_open || !gui.snapshots) {
        sdl2_gui_vm_snapshots_free(gui.snapshots);
        gui.snapshots = sdl2_gui_vm_snapshots();
    }
    gui.snapshots_open = true;
    list = gui.snapshots;

    if (ImGui::MenuItem("Take Snapshot...", nullptr, false, list->disks)) {
        open_snapshot_dialog();
    }
    if (list->count) {
        ImGui::SeparatorText("Snapshots");
    }
    for (int i = 0; i < list->count; i++) {
        ImGui::PushID(i);
        snapshot_item(list->snapshots[i]);
        ImGui::PopID();
    }

    ImGui::Separator();
    if (!list->disks) {
        note("No writable disk supports snapshots; they need the qcow2 "
             "format.");
    } else {
        std::string text = std::string("Disks: ") + list->disks;

        if (list->skipped) {
            text += std::string("\nNot included: ") + list->skipped;
        }
        if (list->no_state_reason) {
            text += std::string("\nMemory is not saved: ") +
                    list->no_state_reason;
        }
        note(text.c_str());
    }
    ImGui::EndMenu();
}

static void usb_menu()
{
    const SDL2GuiUsbList *list;
    Uint64 now = SDL_GetTicks64();

    if (!ImGui::BeginMenu("USB")) {
        gui.usb_open = false;
        return;
    }
    if (!gui.usb_open || !gui.usb || now - gui.usb_time >= kUsbRefresh) {
        sdl2_gui_vm_usb_devices_free(gui.usb);
        gui.usb = sdl2_gui_vm_usb_devices();
        gui.usb_time = now;
    }
    gui.usb_open = true;
    list = gui.usb;

    if (list->unavailable) {
        note(list->unavailable);
        ImGui::Separator();
    }
    if (!list->count) {
        ImGui::TextDisabled("No USB devices");
    }
    for (int i = 0; i < list->count; i++) {
        const SDL2GuiUsbDevice &dev = list->devices[i];
        char label[sizeof(dev.name) + sizeof(dev.port) + 8];
        char ids[16];

        snprintf(label, sizeof(label), "%s##%s", dev.name, dev.port);
        snprintf(ids, sizeof(ids), "%04x:%04x", dev.vendor_id,
                 dev.product_id);
        if (ImGui::MenuItem(label, ids, dev.attached,
                            dev.attached || !list->unavailable)) {
            Request req = make_request(dev.attached ? Op::UsbDetach
                                                    : Op::UsbAttach);
            std::string name = std::string("'") + dev.name + "'";
            std::string hid = name + " is a keyboard, mouse or similar input "
                              "device. While it is passed through to the "
                              "guest, the host cannot use it.";

            req.usb = dev;
            if (dev.attached) {
                queue(req);
            } else if (!dev.accessible) {
                req.op = Op::UsbGrant;
                ask("Grant Access", "Your user has no access to " + name +
                    " yet. Give it access? Your desktop asks for an "
                    "administrator password; the access lasts until the "
                    "device is unplugged." + (dev.hid ? "\n\n" + hid : ""),
                    req);
            } else if (dev.hid) {
                ask("Pass Through", hid, req);
            } else {
                queue(req);
            }
        }
        if (!dev.attached && !dev.accessible) {
            ImGui::SetItemTooltip("No access to /dev/bus/usb/%03u/%03u yet: "
                                  "you will be asked for it", dev.bus,
                                  dev.addr);
        }
    }
    ImGui::EndMenu();
}

static bool setting(const char *label, bool *value)
{
    if (!ImGui::MenuItem(label, nullptr, *value)) {
        return false;
    }
    *value = !*value;
    settings_save();
    return true;
}

static void view_menu()
{
    bool fullscreen = sdl2_gui_display_is_fullscreen();

    if (!ImGui::BeginMenu("View")) {
        return;
    }
    if (ImGui::MenuItem("Fullscreen", hotkey("F").c_str(), fullscreen)) {
        queue(make_request(Op::Fullscreen));
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Show Menu Bar", hotkey("M").c_str(), gui.sticky)) {
        if (gui.sticky) {
            hide();
        } else {
            show(true);
        }
        vm_menu_bar_save(gui.sticky);
    }
    ImGui::SetItemTooltip("Keep the menu bar shown, above the guest display. "
                          "Each VM starts with the menu bar as you left it.");
    if (setting("Show Statistics", &gui.settings.show_stats)) {
        gui.stats_time = 0;     /* start measuring afresh */
        gui.stats_valid = false;
    }
    ImGui::SetItemTooltip("CPU, GPU, disk and network use and frame rate, on "
                          "the right of the menu bar");
    if (ImGui::BeginMenu("Menu Size")) {
        char label[32];

        snprintf(label, sizeof(label), "Automatic (%.0f%%)",
                 gui.auto_scale * 100);
        if (ImGui::MenuItem(label, nullptr, gui.settings.scale == 0)) {
            gui.settings.scale = 0;
            settings_save();
        }
        ImGui::SetItemTooltip("The scale of the desktop");
        ImGui::Separator();
        for (float scale : kScales) {
            snprintf(label, sizeof(label), "%.0f%%", scale * 100);
            if (ImGui::MenuItem(label, nullptr, gui.settings.scale == scale)) {
                gui.settings.scale = scale;
                settings_save();
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

/* Statistics */

static void update_stats(Uint64 now)
{
    /* rates over a long time, e.g. while the bar was hidden, say little */
    bool recent = gui.stats_time && now - gui.stats_time <= 2 * kStatsPeriod;
    uint64_t frames = sdl2_gui_display_frames();
    SDL2GuiStats stats;

    if (gui.stats_time &&
        now - gui.stats_time < (gui.stats_valid ? kStatsPeriod
                                                : kStatsPeriod / 4)) {
        return;
    }
    gui.stats_valid = sdl2_gui_vm_stats(&stats) && recent;
    if (gui.stats_valid) {
        gui.stats = stats;
        gui.fps = (frames - gui.stats_frames) * 1000.0f /
                  (now - gui.stats_time);
    }
    gui.stats_time = now;
    gui.stats_frames = frames;
}

static std::string format_rate(double bytes)
{
    static const char *const units[] = { "B/s", "KB/s", "MB/s", "GB/s" };
    char buf[32];
    int i = 0;

    for (; bytes >= 999.5 && i < 3; i++) {
        bytes /= 1000;
    }
    snprintf(buf, sizeof(buf), i && bytes < 9.95 ? "%.1f %s" : "%.0f %s",
             bytes, units[i]);
    return buf;
}

static std::string format_percent(double use)
{
    char buf[16];

    snprintf(buf, sizeof(buf), "%.0f%%", use * 100);
    return buf;
}

struct StatusItem {
    const char *label;          /* before the value, or nullptr */
    std::string value;
    const char *widest;         /* the widest value, so that it stays put */
    std::string tooltip;
    int priority;               /* the lowest go first when space is short */
    ImVec4 color;
};

static void stats_items(std::vector<StatusItem> &items)
{
    const SDL2GuiStats &s = gui.stats;
    const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    bool v = gui.stats_valid;
    uint32_t rate = sdl2_gui_display_refresh_rate();
    char tip[512];

    snprintf(tip, sizeof(tip), "vCPUs: %.0f%% of %d, on average\n"
             "QEMU as a whole: %.0f%% of a host CPU", s.cpu * 100, s.vcpus,
             s.process_cpu * 100);
    items.push_back({ "CPU", v ? format_percent(s.cpu) : "-", "100%",
                      v ? tip : "", 90, text });

    if (s.gpu >= 0) {
        snprintf(tip, sizeof(tip), "Use of the host GPU (%s) by QEMU: the "
                 "guest's rendering and the window, on its busiest engine",
                 s.gpu_driver);
    } else if (s.gpu_driver[0]) {
        snprintf(tip, sizeof(tip), "The %s driver of the host GPU does not "
                 "report how busy it is", s.gpu_driver);
    } else {
        snprintf(tip, sizeof(tip), "QEMU does not use a host GPU");
    }
    items.push_back({ "GPU", v && s.gpu >= 0 ? format_percent(s.gpu) : "-",
                      "100%", tip, 80, text });

    snprintf(tip, sizeof(tip), "Guest disks\nRead: %s\nWritten: %s",
             format_rate(s.disk_read).c_str(),
             format_rate(s.disk_write).c_str());
    items.push_back({ "Disk", v ? format_rate(s.disk_read + s.disk_write)
                                : "-", "9.9 MB/s", v ? tip : "", 60, text });

    snprintf(tip, sizeof(tip), "Guest network cards (not with vhost)\n"
             "Received: %s\nSent: %s", format_rate(s.net_rx).c_str(),
             format_rate(s.net_tx).c_str());
    items.push_back({ "Net", v ? format_rate(s.net_rx + s.net_tx) : "-",
                      "9.9 MB/s", v ? tip : "", 50, text });

    snprintf(tip, sizeof(tip), "Guest display frames shown per second");
    if (rate) {
        snprintf(tip + strlen(tip), sizeof(tip) - strlen(tip),
                 "\nThe host display refreshes at %g Hz", rate / 1000.0);
    }
    items.push_back({ nullptr, v ? std::to_string(lroundf(gui.fps)) + " fps"
                                 : "- fps", "000 fps", tip, 70, text });
}

static void status()
{
    static const struct {
        const char *state, *label;
    } labels[] = {
        { "running", "Running" },
        { "paused", "Paused" },
        { "suspended", "Suspended" },
        { "prelaunch", "Not started" },
        { "shutdown", "Shut down" },
        { "guest-panicked", "Guest panicked" },
        { "internal-error", "Internal error" },
        { "io-error", "Disk error" },
        { "save-vm", "Saving" },
        { "restore-vm", "Stopped" },
    };
    std::vector<StatusItem> items;
    float em = ImGui::GetFontSize();
    float avail = ImGui::GetContentRegionAvail().x -
                  ImGui::GetStyle().ItemSpacing.x;
    float x = ImGui::GetCursorPosX();
    bool running;
    const char *state = sdl2_gui_vm_state(&running);
    const char *label = state;

    for (const auto &l : labels) {
        if (!strcmp(state, l.state)) {
            label = l.label;
        }
    }
    if (gui.settings.show_stats) {
        stats_items(items);
    }
    items.push_back({ nullptr, label, label, "", 100,
                      running ? ImVec4(0.45f, 0.85f, 0.45f, 1)
                              : ImVec4(0.95f, 0.75f, 0.30f, 1) });

    /* from the right edge, leaving out what does not fit */
    auto width = [&](const StatusItem &it) {
        float w = std::max(ImGui::CalcTextSize(it.widest).x,
                           ImGui::CalcTextSize(it.value.c_str()).x);

        return it.label ? ImGui::CalcTextSize(it.label).x + em * 0.3f + w
                        : w;
    };
    auto total = [&]() {
        float t = 0;

        for (const StatusItem &it : items) {
            t += width(it) + em;
        }
        return t - em;
    };
    while (!items.empty() && total() > avail) {
        items.erase(std::min_element(items.begin(), items.end(),
                                     [](const StatusItem &a,
                                        const StatusItem &b) {
                                         return a.priority < b.priority;
                                     }));
    }
    if (items.empty()) {
        return;
    }

    x += avail - total();
    for (const StatusItem &it : items) {
        float w = width(it);

        if (it.label) {
            ImGui::SetCursorPosX(x);
            ImGui::TextDisabled("%s", it.label);
            if (!it.tooltip.empty()) {
                ImGui::SetItemTooltip("%s", it.tooltip.c_str());
            }
        }
        ImGui::SetCursorPosX(x + w - ImGui::CalcTextSize(it.value.c_str()).x);
        ImGui::TextColored(it.color, "%s", it.value.c_str());
        if (!it.tooltip.empty()) {
            ImGui::SetItemTooltip("%s", it.tooltip.c_str());
        }
        x += w + em;
    }
}

/* Information window */

static void info_display(std::vector<InfoRow> &rows)
{
    const char *s = "Display";
    uint32_t rate = sdl2_gui_display_refresh_rate();
    const char *driver = SDL_GetCurrentVideoDriver();
    char buf[512];
    SDL_version v;
    int w, h;

    SDL_GetVersion(&v);
    snprintf(buf, sizeof(buf), "SDL %d.%d.%d, %s", v.major, v.minor, v.patch,
             driver ? driver : "?");
    rows.push_back({ s, "Window system", buf });
#ifdef CONFIG_OPENGL
    if (gui.glctx) {
        const char *renderer = (const char *)glGetString(GL_RENDERER);
        const char *version = (const char *)glGetString(GL_VERSION);

        snprintf(buf, sizeof(buf), "%s\n%s %s",
                 renderer ? renderer : "?",
                 epoxy_is_desktop_gl() ? "OpenGL" : "OpenGL ES",
                 version ? version : "?");
    } else
#endif
    {
        SDL_RendererInfo info = {};

        SDL_GetRendererInfo(gui.renderer, &info);
        snprintf(buf, sizeof(buf), "SDL renderer (%s)",
                 info.name ? info.name : "?");
    }
    rows.push_back({ s, "Rendering", buf });

    SDL_GetWindowSize(gui.window, &w, &h);
    snprintf(buf, sizeof(buf), "%dx%d%s, desktop scale %.0f%%", w, h,
             sdl2_gui_display_is_fullscreen() ? ", fullscreen" : "",
             desktop_scale() * 100);
    rows.push_back({ s, "Window", buf });
    if (sdl2_gui_display_guest_size(&w, &h)) {
        snprintf(buf, sizeof(buf), "%dx%d", w, h);
        rows.push_back({ s, "Guest display", buf });
    }
    if (rate) {
        snprintf(buf, sizeof(buf), "%g Hz", rate / 1000.0);
        rows.push_back({ s, "Refresh rate", buf });
    }
}

static void info_refresh()
{
    char **info = sdl2_gui_vm_info();
    bool display = false;

    gui.info.clear();
    for (char **row = info; row && row[0] && row[1] && row[2]; row += 3) {
        /* the display right after the graphics devices */
        if (!display && !gui.info.empty() && gui.info.back()[0] == "Graphics"
            && strcmp(row[0], "Graphics")) {
            info_display(gui.info);
            display = true;
        }
        gui.info.push_back({ row[0], row[1], row[2] });
    }
    if (!display) {
        info_display(gui.info);
    }
    sdl2_gui_vm_info_free(info);
}

static void info_copy()
{
    std::string text, section;

    for (const InfoRow &row : gui.info) {
        std::string value = row[2];
        size_t i = 0;

        if (row[0] != section) {
            section = row[0];
            text += (text.empty() ? "" : "\n") + section + "\n";
        }
        /* indent the lines of multi-line values */
        while ((i = value.find('\n', i)) != std::string::npos) {
            value.insert(++i, "    ");
        }
        text += "  " + row[1] + ": " + value + "\n";
    }
    ImGui::SetClipboardText(text.c_str());
}

static void info_window(Uint64 now)
{
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoCollapse;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    float em = ImGui::GetFontSize();
    std::string section;
    bool table = false;

    if (!gui.info_open) {
        gui.info.clear();
        return;
    }
    if (gui.info.empty() || now - gui.info_time >= kInfoRefresh) {
        info_refresh();
        gui.info_time = now;
    }

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2,
                                   vp->Pos.y + gui.bar_height + em),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(std::min(em * 44, vp->Size.x - 2 * em),
                                    std::min(em * 38, vp->Size.y -
                                             gui.bar_height - 2 * em)),
                             ImGuiCond_Appearing);
    if (gui.info_focus) {
        ImGui::SetNextWindowFocus();
        gui.info_focus = false;
    }
    if (!ImGui::Begin("Virtual Machine Information###info", &gui.info_open,
                      flags)) {
        ImGui::End();
        return;
    }
    for (const InfoRow &row : gui.info) {
        if (row[0] != section) {
            if (table) {
                ImGui::EndTable();
            }
            section = row[0];
            ImGui::SeparatorText(section.c_str());
            table = ImGui::BeginTable(section.c_str(), 2);
            if (table) {
                ImGui::TableSetupColumn("name",
                                        ImGuiTableColumnFlags_WidthFixed,
                                        em * 8);
                ImGui::TableSetupColumn("value",
                                        ImGuiTableColumnFlags_WidthStretch);
            }
        }
        if (table) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, dim);
            ImGui::TextWrapped("%s", row[1].c_str());
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", row[2].c_str());
        }
    }
    if (table) {
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::Button("Copy to Clipboard")) {
        info_copy();
    }
    ImGui::End();
}

static void menu_bar()
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    gui.bar_height = ImGui::GetWindowHeight();
    machine_menu();
    snapshots_menu();
    usb_menu();
    view_menu();
    status();
    ImGui::EndMainMenuBar();
}

/* Dialogs */

/* Begin modal @id while @active holds; true if its contents are wanted */
static bool begin_dialog(const char *id, bool active)
{
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoMove;

    if (active && !ImGui::IsPopupOpen(id)) {
        ImGui::OpenPopup(id);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(id, nullptr, flags)) {
        return false;
    }
    if (!active) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return false;
    }
    return true;
}

static void text_block(const std::string &text)
{
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
}

static void snapshot_dialog()
{
    const SDL2GuiSnapshotList *list = gui.snapshots;
    bool save, valid = gui.snapshot_name[0];

    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
    save = ImGui::InputText("Name", gui.snapshot_name,
                            sizeof(gui.snapshot_name),
                            ImGuiInputTextFlags_EnterReturnsTrue);
    if (!list->no_state_reason) {
        ImGui::Checkbox("Include memory", &gui.snapshot_with_state);
        ImGui::SetItemTooltip("Save RAM and device state too, to go back to "
                              "exactly this moment");
    } else {
        text_block(std::string("Only the disks are saved (") +
                   list->no_state_reason + "). Reverting to the snapshot "
                   "restarts the VM from them.");
    }
    if (list->skipped) {
        text_block(std::string("Not included: ") + list->skipped);
    }

    ImGui::BeginDisabled(!valid);
    save = ImGui::Button("Save") || (save && valid);
    ImGui::EndDisabled();
    if (save) {
        Request req = make_request(Op::SnapshotSave);

        req.name = gui.snapshot_name;
        req.with_state = !list->no_state_reason && gui.snapshot_with_state;
        req.busy = "Saving snapshot '" + req.name + "'...";
        queue(req);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        save) {
        gui.snapshot_dialog = false;
        ImGui::CloseCurrentPopup();
    }
}

static bool busy()
{
    return gui.request.op != Op::None && !gui.request.busy.empty();
}

/* Text of the "please wait" dialog, empty when there is nothing to wait for */
static std::string waiting()
{
    if (busy()) {
        return gui.request.busy;
    }
    if (gui.usb_waiting) {
        return std::string("Waiting for access to '") +
               gui.usb_waiting_dev.name + "': answer the authentication "
               "dialog of your desktop.";
    }
    return "";
}

static void dialogs()
{
    bool wait = !waiting().empty();
    bool error = !wait && !gui.error.empty();
    bool confirm = !wait && !error && gui.confirm.op != Op::None;
    bool snapshot = !wait && !error && !confirm && gui.snapshot_dialog;
    std::string title = gui.confirm_title + "###confirm";

    if (snapshot && !gui.snapshots) {
        gui.snapshots = sdl2_gui_vm_snapshots();
    }
    if (begin_dialog("Error###error", error)) {
        text_block(gui.error);
        if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            gui.error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (begin_dialog(title.c_str(), confirm)) {
        text_block(gui.confirm_text);
        if (ImGui::Button(gui.confirm_title.c_str())) {
            queue(gui.confirm);
            gui.confirm = Request();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            gui.confirm = Request();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (begin_dialog("Take Snapshot###snapshot", snapshot)) {
        snapshot_dialog();
        ImGui::EndPopup();
    }
    /* last, so that it replaces a dialog that queued a request right away */
    if (begin_dialog("Please Wait###busy", !waiting().empty())) {
        text_block(waiting());
        ImGui::EndPopup();
    }
}

/* Frames */

static uint64_t hash_bytes(uint64_t h, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint64_t v;

    for (; len >= sizeof(v); p += sizeof(v), len -= sizeof(v)) {
        memcpy(&v, p, sizeof(v));
        h = (h ^ v) * 0x100000001b3ULL;
        h ^= h >> 32;
    }
    for (; len; p++, len--) {
        h = (h ^ *p) * 0x100000001b3ULL;
    }
    return h;
}

static uint64_t hash_draw_data(const ImDrawData *dd)
{
    uint64_t h = hash_bytes(0xcbf29ce484222325ULL, &dd->DisplaySize,
                            sizeof(dd->DisplaySize));

    for (const ImDrawList *list : dd->CmdLists) {
        h = hash_bytes(h, list->VtxBuffer.Data,
                       list->VtxBuffer.size_in_bytes());
        h = hash_bytes(h, list->IdxBuffer.Data,
                       list->IdxBuffer.size_in_bytes());
        for (const ImDrawCmd &cmd : list->CmdBuffer) {
            h = hash_bytes(h, &cmd.ClipRect, sizeof(cmd.ClipRect));
            h = hash_bytes(h, &cmd.ElemCount, sizeof(cmd.ElemCount));
        }
    }
    return h;
}

/* ImGui coordinates are window coordinates, except with the 2D renderer */
static int to_window_rows(float y)
{
    ImGuiIO &io = ImGui::GetIO();
    int w, h;

    if (!gui.renderer || io.DisplaySize.y <= 0) {
        return (int)ceilf(y);
    }
    SDL_GetWindowSize(gui.window, &w, &h);
    return (int)ceilf(y * h / io.DisplaySize.y);
}

static bool textures_pending(const ImDrawData *dd)
{
    if (dd->Textures) {
        for (const ImTextureData *tex : *dd->Textures) {
            if (tex->Status != ImTextureStatus_OK) {
                return true;
            }
        }
    }
    return false;
}

static void new_frame()
{
    ImGuiIO &io = ImGui::GetIO();
    float scale;

#ifdef CONFIG_OPENGL
    if (gui.glctx) {
        SDL_GL_MakeCurrent(gui.window, gui.glctx);
        ImGui_ImplOpenGL3_NewFrame();
    }
#endif
    if (gui.renderer) {
        ImGui_ImplSDLRenderer2_NewFrame();
    }
    ImGui_ImplSDL2_NewFrame();
    if (gui.renderer) {
        int w, h;

        /*
         * The 2D display draws the guest at its logical size, scaled by SDL,
         * which also scales input: work at that size too.
         */
        SDL_RenderGetLogicalSize(gui.renderer, &w, &h);
        if (w && h) {
            io.DisplaySize = ImVec2(w, h);
            io.DisplayFramebufferScale = ImVec2(1, 1);
        }
    }

    scale = menu_scale();
    if (scale != gui.style_scale) {
        apply_style(scale);
        gui.style_scale = scale;
    }
    ImGui::NewFrame();
    if (gui.close_popups) {
        ImGui::ClosePopupsExceptModals();
        gui.close_popups = false;
    }
}

bool sdl2_gui_update(void)
{
    ImDrawData *dd;
    Uint64 now;
    uint64_t hash;
    bool changed;

    if (!gui.ctx) {
        return false;
    }
    ImGui::SetCurrentContext(gui.ctx);
    now = SDL_GetTicks64();

    /* once the VM is set up, which it is not yet when the window opens */
    if (!gui.vm_loaded) {
        gui.vm_loaded = true;
        if (vm_menu_bar_load()) {
            show(true);
        }
    }
    gui.auto_scale = auto_scale();
    update_visibility();
    if (!gui.visible) {
        changed = gui.drawn;
        gui.drawn = false;
        gui.hash = 0;
        gui.wants_pointer = false;
        gui.popup_open = false;
        gui.bar_rows = 0;
        return changed;     /* erase what was drawn */
    }

    if (gui.settings.show_stats) {
        update_stats(now);
    }
    new_frame();
    menu_bar();
    info_window(now);
    dialogs();
    gui.popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
    ImGui::Render();

    gui.bar_rows = gui.visible ? to_window_rows(gui.bar_height) : 0;
    gui.wants_pointer = gui.visible && ImGui::GetIO().WantCaptureMouse;
    gui.drawn = true;
    dd = ImGui::GetDrawData();
    hash = hash_draw_data(dd);
    changed = hash != gui.hash || textures_pending(dd);
    gui.hash = hash;

    /*
     * A request that shows a "please wait" dialog runs once a frame with
     * the dialog has been presented (see sdl2_gui_render()), since it
     * blocks the main loop.  In case nothing gets presented, e.g. with a
     * minimized window, it runs a few refreshes later anyway.
     */
    if (gui.request.op != Op::None && !gui.scheduled &&
        (!busy() || ++gui.request_age > 3)) {
        schedule_request();
    }
    return changed;
}

void sdl2_gui_render(void)
{
    ImDrawData *dd;

    if (!gui.ctx || !gui.drawn) {
        return;
    }
    ImGui::SetCurrentContext(gui.ctx);
    dd = ImGui::GetDrawData();
    if (!dd || !dd->Valid) {
        return;
    }
#ifdef CONFIG_OPENGL
    if (gui.glctx) {
        ImGui_ImplOpenGL3_RenderDrawData(dd);
    } else
#endif
    {
        ImGui_ImplSDLRenderer2_RenderDrawData(dd, gui.renderer);
    }

    /* this frame shows the dialog of the pending request */
    if (busy() && !gui.scheduled && gui.request_age > 0) {
        schedule_request();
    }
}

/* Input */

static Uint32 event_window(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_MOUSEMOTION:
        return ev->motion.windowID;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        return ev->button.windowID;
    case SDL_MOUSEWHEEL:
        return ev->wheel.windowID;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        return ev->key.windowID;
    case SDL_TEXTINPUT:
        return ev->text.windowID;
    case SDL_WINDOWEVENT:
        return ev->window.windowID;
    default:
        return 0;
    }
}

bool sdl2_gui_process_event(const SDL_Event *ev, bool pointer_free)
{
    ImGuiIO *io;
    Uint32 button;

    if (!gui.ctx || event_window(ev) != gui.window_id) {
        return false;
    }
    ImGui::SetCurrentContext(gui.ctx);
    io = &ImGui::GetIO();

    if (gui.pointer_free && !pointer_free && gui.visible) {
        /* the guest grabbed the pointer: the menu is out of its reach */
        io->AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    gui.pointer_free = pointer_free;

    if (!gui.visible) {
        return false;
    }
    if (!pointer_free && (ev->type == SDL_MOUSEMOTION ||
                          ev->type == SDL_MOUSEBUTTONDOWN ||
                          ev->type == SDL_MOUSEBUTTONUP ||
                          ev->type == SDL_MOUSEWHEEL)) {
        return false;
    }
    ImGui_ImplSDL2_ProcessEvent(ev);

    /* the decisions are based on the last frame */
    switch (ev->type) {
    case SDL_MOUSEMOTION:
        return io->WantCaptureMouse && !gui.guest_buttons;
    case SDL_MOUSEBUTTONDOWN:
        if (io->WantCaptureMouse) {
            return true;
        }
        gui.guest_buttons |= SDL_BUTTON(ev->button.button);
        return false;
    case SDL_MOUSEBUTTONUP:
        button = SDL_BUTTON(ev->button.button);
        if (gui.guest_buttons & button) {
            gui.guest_buttons &= ~button;
            return false;
        }
        return true;
    case SDL_MOUSEWHEEL:
        return io->WantCaptureMouse;
    case SDL_KEYDOWN:
        return io->WantCaptureKeyboard ||
               (gui.popup_open &&
                ev->key.keysym.scancode == SDL_SCANCODE_ESCAPE);
    case SDL_TEXTINPUT:
        return io->WantCaptureKeyboard;
    default:
        /* key releases always reach the guest, so that no key sticks */
        return false;
    }
}

bool sdl2_gui_wants_pointer(void)
{
    return gui.ctx && gui.wants_pointer;
}

int sdl2_gui_bar_height(void)
{
    return gui.ctx ? gui.bar_rows : 0;
}

int sdl2_gui_docked_height(void)
{
    /* the 2D display cannot make room above the guest: it only floats */
    return gui.ctx && gui.glctx && gui.sticky ? gui.bar_rows : 0;
}

bool sdl2_gui_toggle(void)
{
    if (!gui.ctx) {
        return false;
    }
    ImGui::SetCurrentContext(gui.ctx);
    if (!gui.visible) {
        show(true);
    } else if (!dialog_open()) {
        hide();
    } else {
        return true;
    }
    vm_menu_bar_save(gui.visible);
    return gui.visible;
}

/* Setup */

#ifdef CONFIG_OPENGL
static const char *glsl_version()
{
    int version = epoxy_gl_version();

    if (!epoxy_is_desktop_gl()) {
        return version >= 30 ? "#version 300 es" : nullptr;
    }
    if (version >= 32) {
        return "#version 150";
    }
    return version >= 30 ? "#version 130" : nullptr;
}
#endif

/*
 * The SDL platform backend changes these global hints on init, which would
 * change how the display behaves; put them back.
 */
static const char *const kSavedHints[] = {
#ifdef SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH
    SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH,
#endif
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_HINT_IME_SHOW_UI,
#endif
#ifdef SDL_HINT_MOUSE_AUTO_CAPTURE
    SDL_HINT_MOUSE_AUTO_CAPTURE,
#endif
};

static bool init_platform(SDL_Window *window, SDL_GLContext glctx,
                          SDL_Renderer *renderer)
{
    std::vector<std::string> values;
    std::vector<bool> set;
    bool ok;

    for (const char *hint : kSavedHints) {
        const char *value = SDL_GetHint(hint);

        set.push_back(value != nullptr);
        values.push_back(value ? value : "");
    }
    ok = glctx ? ImGui_ImplSDL2_InitForOpenGL(window, glctx)
               : ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    for (size_t i = 0; i < values.size(); i++) {
        SDL_SetHint(kSavedHints[i], set[i] ? values[i].c_str() : nullptr);
    }
    if (ok) {
        /* QEMU does its own pointer grabbing */
        ImGui_ImplSDL2_SetMouseCaptureMode(
            ImGui_ImplSDL2_MouseCaptureMode_Disabled);
    }
    return ok;
}

bool sdl2_gui_init(SDL_Window *window, SDL_GLContext glctx,
                   SDL_Renderer *renderer)
{
    const char *glsl = nullptr;
    bool ok;

    if (gui.ctx || !window || (!glctx && !renderer)) {
        return false;
    }
#ifdef CONFIG_OPENGL
    if (glctx) {
        SDL_GL_MakeCurrent(window, glctx);
        glsl = glsl_version();
        if (!glsl) {
            fprintf(stderr, "sdl: OpenGL (ES) %d.%d is too old for the "
                    "control menu\n", epoxy_gl_version() / 10,
                    epoxy_gl_version() % 10);
            return false;
        }
    }
#else
    if (glctx) {
        return false;
    }
#endif

    IMGUI_CHECKVERSION();
    gui.ctx = ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    /* keys pressed for the guest must not drive the menu: no Ctrl+Tab */
    gui.ctx->ConfigNavWindowingKeyNext = 0;
    gui.ctx->ConfigNavWindowingKeyPrev = 0;

    settings_load();
    load_font();

    ok = init_platform(window, glctx, renderer);
    if (ok) {
#ifdef CONFIG_OPENGL
        if (glctx) {
            ok = ImGui_ImplOpenGL3_Init(glsl);
        } else
#endif
        {
            ok = ImGui_ImplSDLRenderer2_Init(renderer);
        }
        if (!ok) {
            ImGui_ImplSDL2_Shutdown();
        }
    }
    if (!ok) {
        fprintf(stderr, "sdl: cannot set up the control menu\n");
        ImGui::DestroyContext(gui.ctx);
        g_free(gui.font_data);
        gui = Gui();
        return false;
    }

    gui.window = window;
    gui.window_id = SDL_GetWindowID(window);
    gui.glctx = glctx;
    gui.renderer = glctx ? nullptr : renderer;
    return true;
}

void sdl2_gui_fini(void)
{
    if (!gui.ctx) {
        return;
    }
    ImGui::SetCurrentContext(gui.ctx);
#ifdef CONFIG_OPENGL
    if (gui.glctx) {
        SDL_GL_MakeCurrent(gui.window, gui.glctx);
        ImGui_ImplOpenGL3_Shutdown();
    }
#endif
    if (gui.renderer) {
        ImGui_ImplSDLRenderer2_Shutdown();
    }
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(gui.ctx);
    forget_lists();
    g_free(gui.font_data);
    gui = Gui();
}

SDL_Window *sdl2_gui_window(void)
{
    return gui.window;
}
