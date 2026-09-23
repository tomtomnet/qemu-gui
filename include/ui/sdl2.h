#ifndef SDL2_H
#define SDL2_H

/* Avoid compiler warning because macro is redefined in SDL_syswm.h. */
#undef WIN32_LEAN_AND_MEAN

#include <SDL.h>

/* with Alpine / muslc SDL headers pull in directfb headers
 * which in turn trigger warning about redundant decls for
 * direct_waitqueue_deinit.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"

#include <SDL_syswm.h>

#pragma GCC diagnostic pop

#ifdef CONFIG_SDL_IMAGE
# include <SDL_image.h>
#endif

#include "ui/kbd-state.h"
#ifdef CONFIG_OPENGL
# include "ui/egl-helpers.h"
#endif

struct sdl2_console {
    DisplayGLCtx dgc;
    DisplayChangeListener dcl;
    DisplaySurface *surface;
    DisplayOptions *opts;
    SDL_Texture *texture;
    SDL_Window *real_window;
    SDL_Renderer *real_renderer;
    int idx;
    int last_vm_running; /* per console for caption reasons */
    int x, y, w, h;
    int hidden;
    int opengl;
    int updates;
    int idle_counter;
    int busy_interval;      /* refresh interval while busy, in ms */
    uint32_t refresh_rate;  /* host display refresh rate, in mHz */
    int ignore_hotkeys;
    bool gui_keysym;
    SDL_GLContext winctx;
    QKbdState *kbd;
    bool has_dmabuf;
    uint64_t frames;        /* guest frames shown */
    bool menu_presented;    /* a frame was presented since the last refresh */
    bool menu_stale;        /* the menu changed and is not on screen yet */
    int menu_top;           /* window rows taken by the docked menu bar */
#ifdef CONFIG_OPENGL
    QemuGLShader *gls;
    egl_fb guest_fb;
    egl_fb win_fb;
    bool y0_top;
    bool scanout_mode;
#endif
};

void sdl2_window_create(struct sdl2_console *scon);
void sdl2_window_destroy(struct sdl2_console *scon);
void sdl2_window_resize(struct sdl2_console *scon);
/* Draw the control menu, if any, right before presenting the window */
void sdl2_draw_menu(struct sdl2_console *scon);

/* Rows of a window @h rows high that show the guest, below the menu bar */
static inline int sdl2_guest_height(struct sdl2_console *scon, int h)
{
    return MAX(h - scon->menu_top, 1);
}
void sdl2_poll_events(struct sdl2_console *scon);

/* Clipboard sharing with the guest, see sdl2-clipboard.c */
void sdl2_clipboard_init(void);
/* The host clipboard changed (SDL_CLIPBOARDUPDATE) */
void sdl2_clipboard_update(void);
/* A window of the display got or lost the keyboard focus */
void sdl2_clipboard_focus(void);
/* A key or mouse button was pressed in a window of the display */
void sdl2_clipboard_input(void);

void sdl2_process_key(struct sdl2_console *scon,
                      SDL_KeyboardEvent *ev);
void sdl2_release_modifiers(struct sdl2_console *scon);

void sdl2_2d_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h);
void sdl2_2d_switch(DisplayChangeListener *dcl,
                    DisplaySurface *new_surface);
void sdl2_2d_refresh(DisplayChangeListener *dcl);
void sdl2_2d_redraw(struct sdl2_console *scon);
bool sdl2_2d_check_format(DisplayChangeListener *dcl,
                          pixman_format_code_t format);

void sdl2_gl_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h);
void sdl2_gl_switch(DisplayChangeListener *dcl,
                    DisplaySurface *new_surface);
void sdl2_gl_refresh(DisplayChangeListener *dcl);
void sdl2_gl_redraw(struct sdl2_console *scon);

QEMUGLContext sdl2_gl_create_context(DisplayGLCtx *dgc,
                                     QEMUGLParams *params);
void sdl2_gl_destroy_context(DisplayGLCtx *dgc, QEMUGLContext ctx);
int sdl2_gl_make_context_current(DisplayGLCtx *dgc,
                                 QEMUGLContext ctx);

void sdl2_gl_scanout_disable(DisplayChangeListener *dcl);
void sdl2_gl_scanout_texture(DisplayChangeListener *dcl,
                             uint32_t backing_id,
                             bool backing_y_0_top,
                             uint32_t backing_width,
                             uint32_t backing_height,
                             uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h,
                             void *d3d_tex2d);
void sdl2_gl_scanout_flush(DisplayChangeListener *dcl,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void sdl2_gl_scanout_dmabuf(DisplayChangeListener *dcl,
                            QemuDmaBuf *dmabuf);
void sdl2_gl_release_dmabuf(DisplayChangeListener *dcl,
                            QemuDmaBuf *dmabuf);
bool sdl2_gl_has_dmabuf(DisplayChangeListener *dcl);
void sdl2_gl_console_init(struct sdl2_console *scon);

#endif /* SDL2_H */
