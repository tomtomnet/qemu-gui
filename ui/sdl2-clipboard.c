/*
 * QEMU SDL display: clipboard sharing with the guest
 *
 * The host clipboard, as SDL sees it, and the QEMU clipboard, which the
 * qemu-vdagent chardev shares with spice-vdagent in the guest, exchange
 * text.
 *
 * SDL 2 can only put the text itself in the host clipboard: there is no
 * way to produce it when an application pastes.  So the text of a guest
 * copy is fetched right away and put in the host clipboard, while a host
 * copy is only announced to the guest, which asks for the text when
 * something pastes it there.
 *
 * Text is compared with what was last exchanged, so that nothing goes
 * back where it came from: putting guest text in the host clipboard
 * changes the host clipboard, and guests that bridge their X11 and
 * Wayland clipboards copy again the text they got.  On Wayland, the host
 * also reports the clipboard again each time the window gets the focus:
 * announcing the same text again would take the clipboard back from the
 * guest.
 *
 * Wayland lets a window set the clipboard only in response to input: SDL
 * needs the serial of a key or button press for it, and without one, it
 * silently keeps the text to itself and stops seeing the host clipboard.
 * So there guest text waits for a key or button press in a window that
 * has the focus, which is when one copies in the guest anyway, unless
 * the host clipboard changes meanwhile.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "ui/clipboard.h"
#include "ui/sdl2.h"
#include "trace.h"

static QemuClipboardPeer sdl2_cbpeer;
static char *host_text;         /* in the host clipboard, as far as we know */
static char *guest_text;        /* given to the guest, until either copies */
static char *pending_text;      /* from the guest, waiting for input */
static bool input;              /* key or button pressed since the focus came */

static void set_text(char **dst, const char *text)
{
    g_free(*dst);
    *dst = g_strdup(text);
}

/* Offer the text of the host clipboard to the guest */
static void sdl2_clipboard_announce(bool again)
{
    char *text = SDL_HasClipboardText() ? SDL_GetClipboardText() : NULL;
    QemuClipboardInfo *info;

    if (!text || !*text) {
        /* nothing to share, e.g. an image */
        trace_sdl2_clipboard_host("no text", 0);
        g_clear_pointer(&host_text, g_free);
        qemu_clipboard_peer_release(&sdl2_cbpeer,
                                    QEMU_CLIPBOARD_SELECTION_CLIPBOARD);
    } else if (!again && !g_strcmp0(text, host_text)) {
        trace_sdl2_clipboard_host("unchanged", strlen(text));
    } else {
        trace_sdl2_clipboard_host(again ? "offered again" : "offered",
                                  strlen(text));
        set_text(&host_text, text);
        g_clear_pointer(&guest_text, g_free);
        g_clear_pointer(&pending_text, g_free);     /* older than this */
        info = qemu_clipboard_info_new(&sdl2_cbpeer,
                                       QEMU_CLIPBOARD_SELECTION_CLIPBOARD);
        info->types[QEMU_CLIPBOARD_TYPE_TEXT].available = true;
        qemu_clipboard_update(info);
        qemu_clipboard_info_unref(info);
    }
    SDL_free(text);
}

void sdl2_clipboard_update(void)
{
    sdl2_clipboard_announce(false);
}

/* Put guest text in the host clipboard */
static void sdl2_clipboard_set(const char *text)
{
    const char *driver = SDL_GetCurrentVideoDriver();

    if (driver && !strcmp(driver, "wayland") &&
        !(input && SDL_GetKeyboardFocus())) {
        trace_sdl2_clipboard_guest("waiting for input", strlen(text));
        set_text(&pending_text, text);
        return;
    }
    g_clear_pointer(&pending_text, g_free);
    if (SDL_SetClipboardText(text) == 0) {
        trace_sdl2_clipboard_guest("put in the host clipboard", strlen(text));
        set_text(&host_text, text);
    } else {
        trace_sdl2_clipboard_guest(SDL_GetError(), strlen(text));
    }
}

void sdl2_clipboard_focus(void)
{
    input = false;
}

void sdl2_clipboard_input(void)
{
    g_autofree char *text = g_steal_pointer(&pending_text);

    input = true;
    if (text) {
        sdl2_clipboard_set(text);
    }
}

/* The guest pastes the host clipboard */
static void sdl2_clipboard_request(QemuClipboardInfo *info,
                                   QemuClipboardType type)
{
    char *text = NULL;

    if (type == QEMU_CLIPBOARD_TYPE_TEXT && SDL_HasClipboardText()) {
        text = SDL_GetClipboardText();
    }
    trace_sdl2_clipboard_host("pasted in the guest", text ? strlen(text) : 0);
    /* empty if the text is gone meanwhile: the guest waits for an answer */
    qemu_clipboard_set_data(&sdl2_cbpeer, info, type,
                            text ? strlen(text) : 0, text, true);
    if (text && *text) {
        set_text(&guest_text, text);
    }
    SDL_free(text);
}

static gboolean sdl2_clipboard_reannounce(gpointer opaque)
{
    sdl2_clipboard_announce(true);
    return G_SOURCE_REMOVE;
}

static void sdl2_clipboard_update_info(QemuClipboardInfo *info)
{
    QemuClipboardContent *content = &info->types[QEMU_CLIPBOARD_TYPE_TEXT];
    g_autofree char *text = NULL;

    if (info->owner == &sdl2_cbpeer || !info->owner ||
        info->selection != QEMU_CLIPBOARD_SELECTION_CLIPBOARD) {
        return;
    }
    if (info != qemu_clipboard_info(info->selection)) {
        /* the guest copied: fetch the text now, see above */
        if (content->available) {
            qemu_clipboard_request(info, QEMU_CLIPBOARD_TYPE_TEXT);
        }
        return;
    }

    /* the text of the guest copy */
    if (!content->data || !content->size) {
        return;
    }
    text = g_strndup(content->data, content->size);
    if (!*text || !g_strcmp0(text, host_text)) {
        trace_sdl2_clipboard_guest("already in the host clipboard",
                                   strlen(text));
        return;
    }
    if (!g_strcmp0(text, guest_text)) {
        /* what the guest got from us, copied again */
        trace_sdl2_clipboard_guest("the host text back", strlen(text));
        return;
    }
    g_clear_pointer(&guest_text, g_free);
    sdl2_clipboard_set(text);
}

static void sdl2_clipboard_notify(Notifier *notifier, void *data)
{
    QemuClipboardNotify *notify = data;

    switch (notify->type) {
    case QEMU_CLIPBOARD_UPDATE_INFO:
        sdl2_clipboard_update_info(notify->info);
        break;
    case QEMU_CLIPBOARD_RESET_SERIAL:
        /*
         * A guest agent connected, e.g. at login: offer it the host
         * clipboard, once qemu-vdagent listens, right after this.
         */
        g_idle_add(sdl2_clipboard_reannounce, NULL);
        break;
    }
}

void sdl2_clipboard_init(void)
{
    sdl2_cbpeer.name = "sdl2";
    sdl2_cbpeer.notifier.notify = sdl2_clipboard_notify;
    sdl2_cbpeer.request = sdl2_clipboard_request;
    qemu_clipboard_peer_register(&sdl2_cbpeer);
}
