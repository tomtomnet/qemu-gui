/*
 * QEMU SDL display: frame statistics
 *
 * For x-query-display-stats: how many guest frames reach the screen, how
 * long they take, and how long input takes to change them.
 *
 * - A frame's latency runs from QEMU receiving the guest's flush to the
 *   compositor showing the frame.  On Wayland the compositor says when, in
 *   its presentation feedback, asked for on SDL's surface right before each
 *   buffer swap, so that the swap's commit carries it.  Elsewhere the end of
 *   the swap is as far as QEMU sees.
 * - Every flush is numbered.  A frame shows the newest flush QEMU had drawn
 *   when it swapped, so the flushes between two frames on screen that the
 *   second does not show were dropped: replaced by a newer one before being
 *   shown, in QEMU or in the compositor.
 * - Input latency runs from a key press, click or wheel turn to the first
 *   frame on screen that holds a flush made after the event reached the
 *   guest, when the guest was idle before, so that the flush is likely its
 *   answer.  On Wayland SDL gives the time the compositor sent the event,
 *   so the latency includes the wait for the display's next refresh, when
 *   it reads input.  Elsewhere, or when those times prove wrong, it starts
 *   when the display read the event.
 *
 * All of it runs in the main thread, like the rest of the SDL display, and
 * costs a few clock reads per frame and input event, plus one feedback
 * request per swap on Wayland.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qapi/qapi-types-ui.h"
#include "ui/console.h"
#include "ui/sdl2.h"
#include "trace.h"

#ifdef CONFIG_SDL_WAYLAND
#include <wayland-client.h>
#include "ui/presentation-time-client-protocol.h"
#endif

/* What the rates and the frame latencies cover */
#define STATS_WINDOW_NS     NANOSECONDS_PER_SECOND
/* The input latencies are fewer: a longer look back */
#define INPUT_WINDOW_NS     (10 * NANOSECONDS_PER_SECOND)
/* A guest idle for that long before an input event answers it */
#define INPUT_IDLE_NS       (50 * SCALE_MS)
/* ... and within that long, else it did not */
#define INPUT_TIMEOUT_NS    NANOSECONDS_PER_SECOND
/* Counts go by buckets, more of them than the window needs */
#define BUCKET_NS           (50 * SCALE_MS)
#define NBUCKETS            32
/* Samples of each duration: a second of frames at 1000 Hz and more */
#define NSAMPLES            2048
#define NINPUT_SAMPLES      64
/* Frames waiting for the compositor's feedback */
#define NPRESENTS           64
/* A feedback that never came */
#define PRESENT_TIMEOUT_NS  (2 * NANOSECONDS_PER_SECOND)

typedef enum {
    COUNT_FLUSH,
    COUNT_SHOWN,
    COUNT_DROPPED,
    COUNT_DIRECT,
    COUNT_REDRAW,
    COUNT__MAX,
} Count;

typedef struct {
    int64_t start;              /* 0 if unused */
    uint32_t n[COUNT__MAX];
} Bucket;

typedef struct {
    int64_t t;                  /* when, 0 if unused */
    int64_t ns;
} Sample;

typedef struct {
    Sample *s;
    unsigned size;              /* a power of two */
    unsigned next;
} Samples;

typedef struct Present Present;

struct sdl2_stats {
    struct sdl2_console *scon;

    uint64_t flush_seq;         /* the guest's flushes so far */
    int64_t flush_ns;           /* when the newest came */
    bool in_refresh;            /* 2D updates count once per refresh */
    bool refresh_flushed;

    uint64_t swapped_seq;       /* the newest flush a swap has shown */
    uint64_t shown_seq;         /* the newest flush on screen */
    int64_t shown_ns;           /* when it got there */
    bool swapped;               /* a frame was swapped */
    bool feedback;              /* the swap in progress gets feedback */
    Present *present;           /* its request, if there was room for one */

    int64_t input_ns;           /* an input waiting for the guest's answer */
    int64_t input_given_ns;     /* when the guest got it */
    int64_t carried_input_ns;   /* its answer was discarded: the next frame */

    bool wm_checked;            /* looked for the Wayland surface */
#ifdef CONFIG_SDL_WAYLAND
    struct wl_surface *surface; /* SDL's, while the window lives */
#endif
    uint32_t refresh_ns;        /* the compositor's refresh period */

    Bucket buckets[NBUCKETS];
    Samples frame_latency;
    Samples qemu_latency;
    Samples frame_interval;
    Samples input_latency;
    Samples input_wait;
};

typedef struct sdl2_stats SDL2Stats;

struct Present {
    SDL2Stats *st;              /* NULL if free */
#ifdef CONFIG_SDL_WAYLAND
    struct wp_presentation_feedback *feedback;
#endif
    uint64_t flush_seq;         /* the newest flush it shows */
    int64_t flush_ns;
    int64_t swap_ns;            /* when its swap ended */
    int64_t input_ns;           /* the input it answers, if any */
    int64_t requested_ns;
};

static struct sdl2_console *stats_consoles;
static int stats_count;
static Present presents[NPRESENTS];
static bool on_wayland;         /* SDL's event times come from the compositor */
static int64_t last_poll_ns;    /* when the display last read events */

/*
 * SDL's event times, per kind of input, unless they proved wrong: KWin's
 * fake input devices, for one, send key events with a stale time
 */
typedef enum {
    INPUT_KEY,
    INPUT_POINTER,
    INPUT__MAX,
} InputKind;

typedef struct {
    uint32_t stamp;             /* SDL's time of the previous event, in ms */
    int64_t read_ns;            /* when the display read it */
    bool wrong;
} InputTimes;

static InputTimes input_times[INPUT__MAX];

/* An event read at @now, @age ms after SDL's time for it */
static bool input_time_ok(const SDL_Event *ev, uint32_t age, int64_t now)
{
    InputKind kind = ev->type == SDL_KEYDOWN || ev->type == SDL_KEYUP ?
                     INPUT_KEY : INPUT_POINTER;
    InputTimes *it = &input_times[kind];

    if (!on_wayland) {
        return false;           /* SDL stamps X11 events as it reads them */
    }
    /* it came after the previous read, else that read would have got it */
    if ((int64_t)age * SCALE_MS >
        (last_poll_ns ? now - last_poll_ns : 0) + 100 * SCALE_MS) {
        it->wrong = true;
    }
    /* the same time as an event read long before: a stale time */
    if (it->read_ns && ev->common.timestamp == it->stamp &&
        now - it->read_ns > 100 * SCALE_MS) {
        it->wrong = true;
    }
    it->stamp = ev->common.timestamp;
    it->read_ns = now;
    return !it->wrong;
}

/* CLOCK_MONOTONIC on Linux, the clock of Wayland compositors' times */
static int64_t now_ns(void)
{
    return get_clock();
}

static void samples_init(Samples *s, unsigned size)
{
    s->s = g_new0(Sample, size);
    s->size = size;
    s->next = 0;
}

static void samples_add(Samples *s, int64_t t, int64_t ns)
{
    s->s[s->next++ & (s->size - 1)] = (Sample) { t, ns };
}

static void count(SDL2Stats *st, Count what, int64_t t, uint32_t n)
{
    int64_t start = t - t % BUCKET_NS;
    Bucket *b = &st->buckets[(start / BUCKET_NS) % NBUCKETS];

    if (b->start != start) {
        if (b->start > start) {
            return;             /* older than the buckets kept */
        }
        memset(b, 0, sizeof(*b));
        b->start = start;
    }
    b->n[what] += n;
}

/* Per second, over the complete buckets of the last second */
static double rate(SDL2Stats *st, Count what, int64_t now)
{
    int64_t end = now - now % BUCKET_NS;
    int64_t begin = end - STATS_WINDOW_NS;
    uint64_t sum = 0;
    int i;

    for (i = 0; i < NBUCKETS; i++) {
        if (st->buckets[i].start >= begin && st->buckets[i].start < end) {
            sum += st->buckets[i].n[what];
        }
    }
    return (double)sum * NANOSECONDS_PER_SECOND / STATS_WINDOW_NS;
}

static int compare_ns(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

    return x < y ? -1 : x > y;
}

/* Nearest rank, in microseconds */
static int64_t percentile(const int64_t *v, unsigned n, unsigned pct)
{
    unsigned rank = (n * pct + 99) / 100;

    return (v[MAX(rank, 1) - 1] + 500) / 1000;
}

static DisplayStatsLatency *latency(Samples *s, int64_t since)
{
    DisplayStatsLatency *l = g_new0(DisplayStatsLatency, 1);
    g_autofree int64_t *v = g_new(int64_t, s->size);
    unsigned i, n = 0;

    for (i = 0; i < s->size; i++) {
        if (s->s[i].t && s->s[i].t >= since) {
            v[n++] = s->s[i].ns;
        }
    }
    if (n) {
        qsort(v, n, sizeof(v[0]), compare_ns);
        l->samples = n;
        l->median = percentile(v, n, 50);
        l->p99 = percentile(v, n, 99);
        l->max = (v[n - 1] + 500) / 1000;
    }
    return l;
}

/*
 * A frame reached the screen at @t, showing flushes up to @seq.  @input
 * is the time of the input event it answers, if any.
 */
static void frame_shown(SDL2Stats *st, uint64_t seq, int64_t flush_ns,
                        int64_t input, int64_t t, bool direct)
{
    if (st->carried_input_ns) {
        input = input ? MIN(input, st->carried_input_ns) : st->carried_input_ns;
        st->carried_input_ns = 0;
    }
    if (seq > st->shown_seq) {
        count(st, COUNT_SHOWN, t, 1);
        if (st->shown_seq && seq - st->shown_seq > 1) {
            count(st, COUNT_DROPPED, t, seq - st->shown_seq - 1);
        }
        if (direct) {
            count(st, COUNT_DIRECT, t, 1);
        }
        if (t >= flush_ns) {
            samples_add(&st->frame_latency, t, t - flush_ns);
        }
        if (st->shown_ns && t > st->shown_ns) {
            samples_add(&st->frame_interval, t, t - st->shown_ns);
        }
        st->shown_seq = seq;
        st->shown_ns = t;
    } else {
        count(st, COUNT_REDRAW, t, 1);
    }
    if (input && t > input) {
        samples_add(&st->input_latency, t, t - input);
    }
}

/* The input the frame about to be swapped answers, if any */
static int64_t take_input(SDL2Stats *st, int64_t now)
{
    int64_t input = 0;

    if (!st->input_ns) {
        return 0;
    }
    if (st->flush_ns > st->input_given_ns) {
        input = st->input_ns;
        st->input_ns = 0;
    } else if (now - st->input_given_ns > INPUT_TIMEOUT_NS) {
        st->input_ns = 0;       /* the guest did not answer */
    }
    return input;
}

static void present_free(Present *p)
{
#ifdef CONFIG_SDL_WAYLAND
    if (p->feedback) {
        wp_presentation_feedback_destroy(p->feedback);
        p->feedback = NULL;
    }
#endif
    if (p->st && p->st->present == p) {
        p->st->present = NULL;
    }
    p->st = NULL;
}

#ifdef CONFIG_SDL_WAYLAND
/*
 * The presentation feedback, on an event queue of its own: SDL reads the
 * socket as it polls for events, and sdl2_stats_dispatch() dispatches what
 * came for the queue.
 */
static struct {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wp_presentation *presentation;
    clockid_t clock;
    bool tried;
} wl = {
    .clock = CLOCK_MONOTONIC,
};

/* The compositor's times are those of its clock, QEMU's CLOCK_MONOTONIC */
static int64_t to_monotonic(int64_t t)
{
    struct timespec a, b;

    if (wl.clock == CLOCK_MONOTONIC ||
        clock_gettime(wl.clock, &a) || clock_gettime(CLOCK_MONOTONIC, &b)) {
        return t;
    }
    return t - (a.tv_sec - b.tv_sec) * NANOSECONDS_PER_SECOND -
           (a.tv_nsec - b.tv_nsec);
}

static void feedback_sync_output(void *data,
                                 struct wp_presentation_feedback *feedback,
                                 struct wl_output *output)
{
}

static void feedback_presented(void *data,
                               struct wp_presentation_feedback *feedback,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                               uint32_t tv_nsec, uint32_t refresh,
                               uint32_t seq_hi, uint32_t seq_lo,
                               uint32_t flags)
{
    Present *p = data;
    SDL2Stats *st = p->st;
    int64_t sec = (int64_t)tv_sec_hi << 32 | tv_sec_lo;
    int64_t t = sec * NANOSECONDS_PER_SECOND + tv_nsec;

    t = to_monotonic(t);
    if (refresh) {
        st->refresh_ns = refresh;
    }
    trace_sdl2_stats_presented(st->scon->idx, p->flush_seq, t,
                               p->swap_ns ? t - p->swap_ns : 0, flags);
    frame_shown(st, p->flush_seq, p->flush_ns, p->input_ns, t,
                flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY);
    present_free(p);
}

static void feedback_discarded(void *data,
                               struct wp_presentation_feedback *feedback)
{
    Present *p = data;
    SDL2Stats *st = p->st;

    trace_sdl2_stats_discarded(st->scon->idx, p->flush_seq);
    /* its content, the answer included, shows with the next frame */
    if (p->input_ns && (!st->carried_input_ns ||
                        p->input_ns < st->carried_input_ns)) {
        st->carried_input_ns = p->input_ns;
    }
    present_free(p);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    .sync_output = feedback_sync_output,
    .presented = feedback_presented,
    .discarded = feedback_discarded,
};

static void presentation_clock_id(void *data,
                                  struct wp_presentation *presentation,
                                  uint32_t clk_id)
{
    wl.clock = clk_id;
}

static const struct wp_presentation_listener presentation_listener = {
    .clock_id = presentation_clock_id,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    if (!strcmp(interface, wp_presentation_interface.name)) {
        wl.presentation = wl_registry_bind(registry, name,
                                           &wp_presentation_interface, 1);
        wp_presentation_add_listener(wl.presentation, &presentation_listener,
                                     NULL);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/*
 * Once, when the first window shows: a round trip for the globals, then
 * one for the clock
 */
static void wayland_init(struct wl_display *display)
{
    struct wl_display *wrapper;
    struct wl_registry *registry;

    wl.tried = true;
    wl.display = display;
    wl.queue = wl_display_create_queue(display);
    wrapper = wl_proxy_create_wrapper(display);
    wl_proxy_set_queue((struct wl_proxy *)wrapper, wl.queue);
    registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip_queue(display, wl.queue);
    if (wl.presentation) {
        wl_display_roundtrip_queue(display, wl.queue);
    }
    wl_registry_destroy(registry);
    trace_sdl2_stats_presentation(wl.presentation != NULL, wl.clock);
}
#endif

/* The Wayland surface of the window, whose commits can carry feedback */
static void find_surface(SDL2Stats *st)
{
    SDL_SysWMinfo info;

    st->wm_checked = true;
    SDL_VERSION(&info.version);
    if (!st->scon->real_window ||
        !SDL_GetWindowWMInfo(st->scon->real_window, &info)) {
        return;
    }
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    if (info.subsystem != SDL_SYSWM_WAYLAND) {
        return;
    }
    on_wayland = true;
#ifdef CONFIG_SDL_WAYLAND
    if (!wl.tried) {
        wayland_init(info.info.wl.display);
    }
    if (wl.presentation && info.info.wl.display == wl.display) {
        st->surface = info.info.wl.surface;
    }
#endif
#endif
}

static DisplayStatsConsole *console_stats(SDL2Stats *st, int64_t now)
{
    struct sdl2_console *scon = st->scon;
    DisplayStatsConsole *c = g_new0(DisplayStatsConsole, 1);
    int64_t since = now - STATS_WINDOW_NS;

    c->console = qemu_console_get_index(scon->dcl.con);
#ifdef CONFIG_OPENGL
    if (scon->scanout_mode) {
        c->width = scon->w;
        c->height = scon->h;
    } else
#endif
    if (scon->surface) {
        c->width = surface_width(scon->surface);
        c->height = surface_height(scon->surface);
    }
    c->refresh_rate = st->refresh_ns ?
        (int64_t)(1e12 / st->refresh_ns + 0.5) : scon->refresh_rate;
#ifdef CONFIG_SDL_WAYLAND
    if (st->surface) {
        c->method = DISPLAY_STATS_METHOD_PRESENTATION;
    } else
#endif
    {
        c->method = st->swapped ? DISPLAY_STATS_METHOD_SWAP
                                : DISPLAY_STATS_METHOD_NONE;
    }
    c->flushes = rate(st, COUNT_FLUSH, now);
    c->presented = rate(st, COUNT_SHOWN, now);
    c->dropped = rate(st, COUNT_DROPPED, now);
    c->direct = rate(st, COUNT_DIRECT, now);
    c->redraws = rate(st, COUNT_REDRAW, now);
    c->frame_latency = latency(&st->frame_latency, since);
    c->qemu_latency = latency(&st->qemu_latency, since);
    c->frame_interval = latency(&st->frame_interval, since);
    c->input_latency = latency(&st->input_latency, now - INPUT_WINDOW_NS);
    if (on_wayland) {
        c->input_wait = latency(&st->input_wait, since);
    }
    return c;
}

/* The consoles of x-query-display-stats */
static void sdl2_stats_query(DisplayStats *stats)
{
    DisplayStatsConsoleList **tail = &stats->consoles;
    int64_t now = now_ns();
    int i;

    for (i = 0; i < stats_count; i++) {
        struct sdl2_console *scon = &stats_consoles[i];

        if (!scon->stats || !scon->dcl.con ||
            !qemu_console_is_graphic(scon->dcl.con) ||
            (!scon->real_window && !scon->stats->flush_seq)) {
            continue;
        }
        QAPI_LIST_APPEND(tail, console_stats(scon->stats, now));
    }
}

void sdl2_stats_init(struct sdl2_console *consoles, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        SDL2Stats *st = g_new0(SDL2Stats, 1);

        st->scon = &consoles[i];
        samples_init(&st->frame_latency, NSAMPLES);
        samples_init(&st->qemu_latency, NSAMPLES);
        samples_init(&st->frame_interval, NSAMPLES);
        samples_init(&st->input_latency, NINPUT_SAMPLES);
        samples_init(&st->input_wait, NSAMPLES);
        consoles[i].stats = st;
    }
    stats_consoles = consoles;
    stats_count = count;
    qemu_display_set_stats(sdl2_stats_query);
}

void sdl2_stats_fini(void)
{
    int i;

    qemu_display_set_stats(NULL);
    for (i = 0; i < NPRESENTS; i++) {
        present_free(&presents[i]);
    }
    for (i = 0; i < stats_count; i++) {
        SDL2Stats *st = stats_consoles[i].stats;

        if (st) {
            g_free(st->frame_latency.s);
            g_free(st->qemu_latency.s);
            g_free(st->frame_interval.s);
            g_free(st->input_latency.s);
            g_free(st->input_wait.s);
            g_free(st);
            stats_consoles[i].stats = NULL;
        }
    }
    stats_consoles = NULL;
    stats_count = 0;
#ifdef CONFIG_SDL_WAYLAND
    g_clear_pointer(&wl.presentation, wp_presentation_destroy);
    g_clear_pointer(&wl.queue, wl_event_queue_destroy);
    wl.display = NULL;
    wl.tried = false;
#endif
}

void sdl2_stats_window_changed(struct sdl2_console *scon)
{
    SDL2Stats *st = scon->stats;

    if (!st) {
        return;
    }
    st->wm_checked = false;
#ifdef CONFIG_SDL_WAYLAND
    st->surface = NULL;
#endif
}

void sdl2_stats_flush(struct sdl2_console *scon)
{
    SDL2Stats *st = scon->stats;

    if (!st) {
        return;
    }
    if (st->in_refresh) {
        if (st->refresh_flushed) {
            return;
        }
        st->refresh_flushed = true;
    }
    st->flush_ns = now_ns();
    st->flush_seq++;
    count(st, COUNT_FLUSH, st->flush_ns, 1);
    trace_sdl2_stats_flush(scon->idx, st->flush_seq, st->flush_ns);
}

void sdl2_stats_refresh_begin(struct sdl2_console *scon)
{
    if (scon->stats) {
        scon->stats->in_refresh = true;
        scon->stats->refresh_flushed = false;
    }
}

void sdl2_stats_refresh_end(struct sdl2_console *scon)
{
    if (scon->stats) {
        scon->stats->in_refresh = false;
    }
}

void sdl2_stats_present_begin(struct sdl2_console *scon)
{
    SDL2Stats *st = scon->stats;

    if (!st) {
        return;
    }
    if (!st->wm_checked) {
        find_surface(st);
    }
    st->present = NULL;
    st->feedback = false;
#ifdef CONFIG_SDL_WAYLAND
    if (st->surface) {
        int64_t now = now_ns();
        Present *p = NULL;
        int i;

        st->feedback = true;
        for (i = 0; i < NPRESENTS; i++) {
            if (!presents[i].st) {
                p = &presents[i];
                break;
            }
        }
        if (p) {
            p->st = st;
            p->flush_seq = st->flush_seq;
            p->flush_ns = st->flush_ns;
            p->swap_ns = 0;
            p->input_ns = take_input(st, now);
            p->requested_ns = now;
            p->feedback = wp_presentation_feedback(wl.presentation,
                                                   st->surface);
            wp_presentation_feedback_add_listener(p->feedback,
                                                  &feedback_listener, p);
            st->present = p;
        }
    }
#endif
}

void sdl2_stats_present_end(struct sdl2_console *scon)
{
    SDL2Stats *st = scon->stats;
    int64_t now;

    if (!st) {
        return;
    }
    now = now_ns();
    st->swapped = true;
    trace_sdl2_stats_swap(scon->idx, st->flush_seq, st->flush_ns, now);
    if (st->flush_seq > st->swapped_seq) {
        samples_add(&st->qemu_latency, now, now - st->flush_ns);
        st->swapped_seq = st->flush_seq;
    }
    if (st->feedback) {
        /* the compositor tells, unless no request was free for this frame */
        if (st->present) {
            st->present->swap_ns = now;
            st->present = NULL;
        }
        return;
    }
    /* no feedback: the swap is as far as QEMU sees */
    frame_shown(st, st->flush_seq, st->flush_ns, take_input(st, now), now,
                false);
}

void sdl2_stats_input(struct sdl2_console *scon, const SDL_Event *ev,
                      bool answered)
{
    SDL2Stats *st = scon->stats;
    int64_t now, wait = 0;
    uint32_t ticks, age;

    if (!st) {
        return;
    }
    now = now_ns();
    /* both in SDL's milliseconds */
    ticks = SDL_GetTicks();
    age = ticks - ev->common.timestamp;
    if (input_time_ok(ev, age, now)) {
        wait = (int64_t)age * SCALE_MS;
        samples_add(&st->input_wait, now, wait);
    }
    if (answered && !st->input_ns && now - st->flush_ns > INPUT_IDLE_NS) {
        st->input_ns = now - wait;
        st->input_given_ns = now;
    }
    trace_sdl2_stats_input(scon->idx, ev->type, now, wait, answered);
}

void sdl2_stats_dispatch(void)
{
    int64_t now;
    int i;

#ifdef CONFIG_SDL_WAYLAND
    if (wl.queue) {
        wl_display_dispatch_queue_pending(wl.display, wl.queue);
    }
#endif
    now = now_ns();
    for (i = 0; i < NPRESENTS; i++) {
        if (presents[i].st && presents[i].st->present != &presents[i] &&
            now - presents[i].requested_ns > PRESENT_TIMEOUT_NS) {
            present_free(&presents[i]);
        }
    }
    last_poll_ns = now;
}

