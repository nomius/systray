/* audio.c - PulseAudio volume / device widget and tray icon.
 *
 * Icon: gray, yellow while the default output is muted.
 * Scroll over icon: output volume +/-5%.  Middle click: toggle mute.
 * Popup: mute toggle, output device + volume, input device + volume.
 *
 * PulseAudio runs on its own thread (pa_threaded_mainloop); the cached state
 * is only touched with the mainloop lock held.  Changes wake the UI loop.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "common.h"

#include <pulse/pulseaudio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXDEV   16
#define POP_W    340
#define SCROLL_STEP 5

typedef struct {
    int used;
    uint32_t idx;
    char name[256];
    char desc[128];
    pa_cvolume cv;
    int vol;                  /* 0..100 */
    int mute;
} Dev;

static struct {
    Dev sink[MAXDEV], src[MAXDEV];
    char def_sink[256], def_src[256];
} st;

static pa_threaded_mainloop *ml;
static pa_context *ctx;
static volatile int ready;
static int popup_is_open, drag;           /* drag: 0 none, 1 output, 2 input */

#define LOCK()   pa_threaded_mainloop_lock(ml)
#define UNLOCK() pa_threaded_mainloop_unlock(ml)
#define OP(x) do { pa_operation *o_ = (x); if (o_) pa_operation_unref(o_); } while (0)

/* ------------------------------------------------------------------ */
/* Glyphs                                                              */
/* ------------------------------------------------------------------ */

static int g_muted;

static void glyph_speaker(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s / 20.0, s / 20.0);
    ui_color(cr, c);
    cairo_move_to(cr, -9, -4);
    cairo_line_to(cr, -4, -4);
    cairo_line_to(cr, 2, -9);
    cairo_line_to(cr, 2, 9);
    cairo_line_to(cr, -4, 4);
    cairo_line_to(cr, -9, 4);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.8);
    if (!g_muted) {
        cairo_arc(cr, 2, 0, 5.5, -0.9, 0.9);
        cairo_stroke(cr);
        cairo_arc(cr, 2, 0, 9.5, -0.8, 0.8);
        cairo_stroke(cr);
    } else {
        cairo_move_to(cr, 6, -4); cairo_line_to(cr, 12, 4);
        cairo_move_to(cr, 12, -4); cairo_line_to(cr, 6, 4);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void glyph_mic(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s / 20.0, s / 20.0);
    ui_color(cr, c);
    cairo_arc(cr, 0, -4, 3.6, 3.14159265, 0);          /* capsule */
    cairo_line_to(cr, 3.6, 3);
    cairo_arc_negative(cr, 0, 3, 3.6, 0, 3.14159265);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.6);
    cairo_arc(cr, 0, 2, 6.4, 0, 3.14159265);           /* cradle */
    cairo_stroke(cr);
    cairo_move_to(cr, 0, 8.4); cairo_line_to(cr, 0, 11);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void glyph_box(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s / 20.0, s / 20.0);
    ui_color(cr, c);
    cairo_set_line_width(cr, 1.8);
    cairo_rectangle(cr, -6, -9, 12, 18);
    cairo_stroke(cr);
    cairo_arc(cr, 0, 3, 2.8, 0, 6.2832);
    cairo_stroke(cr);
    cairo_arc(cr, 0, -4, 1.1, 0, 6.2832);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* ------------------------------------------------------------------ */
/* Device cache (pulse thread, mainloop lock held)                     */
/* ------------------------------------------------------------------ */

static int to_pct(pa_volume_t v)
{
    return (int)(((uint64_t)v * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
}

static Dev *upsert(Dev *arr, uint32_t idx)
{
    Dev *free_slot = NULL;
    for (int i = 0; i < MAXDEV; i++) {
        if (arr[i].used && arr[i].idx == idx) return &arr[i];
        if (!arr[i].used && !free_slot) free_slot = &arr[i];
    }
    if (free_slot) { memset(free_slot, 0, sizeof *free_slot); free_slot->used = 1; free_slot->idx = idx; }
    return free_slot;
}

static void sink_cb(pa_context *c, const pa_sink_info *i, int eol, void *u)
{
    (void)c; (void)u;
    if (eol || !i) return;
    Dev *d = upsert(st.sink, i->index);
    if (!d) return;
    snprintf(d->name, sizeof d->name, "%s", i->name);
    snprintf(d->desc, sizeof d->desc, "%s", i->description ? i->description : i->name);
    d->cv = i->volume;
    d->vol = to_pct(pa_cvolume_avg(&i->volume));
    d->mute = i->mute;
    app_wake();
}

static void source_cb(pa_context *c, const pa_source_info *i, int eol, void *u)
{
    (void)c; (void)u;
    if (eol || !i) return;
    if (i->monitor_of_sink != PA_INVALID_INDEX) return;      /* hide monitors */
    Dev *d = upsert(st.src, i->index);
    if (!d) return;
    snprintf(d->name, sizeof d->name, "%s", i->name);
    snprintf(d->desc, sizeof d->desc, "%s", i->description ? i->description : i->name);
    d->cv = i->volume;
    d->vol = to_pct(pa_cvolume_avg(&i->volume));
    d->mute = i->mute;
    app_wake();
}

static void server_cb(pa_context *c, const pa_server_info *i, void *u)
{
    (void)c; (void)u;
    if (!i) return;
    snprintf(st.def_sink, sizeof st.def_sink, "%s", i->default_sink_name ? i->default_sink_name : "");
    snprintf(st.def_src, sizeof st.def_src, "%s", i->default_source_name ? i->default_source_name : "");
    app_wake();
}

static void refresh_all(void)
{
    OP(pa_context_get_server_info(ctx, server_cb, NULL));
    OP(pa_context_get_sink_info_list(ctx, sink_cb, NULL));
    OP(pa_context_get_source_info_list(ctx, source_cb, NULL));
}

static void subscribe_cb(pa_context *c, pa_subscription_event_type_t t,
                         uint32_t idx, void *u)
{
    (void)u;
    int fac = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    int rem = (t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE;
    if (fac == PA_SUBSCRIPTION_EVENT_SINK) {
        if (rem) { Dev *d = upsert(st.sink, idx); if (d) d->used = 0; app_wake(); }
        else OP(pa_context_get_sink_info_by_index(c, idx, sink_cb, NULL));
    } else if (fac == PA_SUBSCRIPTION_EVENT_SOURCE) {
        if (rem) { Dev *d = upsert(st.src, idx); if (d) d->used = 0; app_wake(); }
        else OP(pa_context_get_source_info_by_index(c, idx, source_cb, NULL));
    } else if (fac == PA_SUBSCRIPTION_EVENT_SERVER) {
        OP(pa_context_get_server_info(c, server_cb, NULL));
    }
}

static void state_cb(pa_context *c, void *u)
{
    (void)u;
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        ready = 1;
        pa_context_set_subscribe_callback(c, subscribe_cb, NULL);
        OP(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK |
                                   PA_SUBSCRIPTION_MASK_SOURCE |
                                   PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL));
        refresh_all();
        app_wake();
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        ready = 0;
        app_wake();
        break;
    default:
        break;
    }
}

static void connect_ctx(void)
{
    ctx = pa_context_new(pa_threaded_mainloop_get_api(ml), "systray");
    pa_context_set_state_callback(ctx, state_cb, NULL);
    pa_context_connect(ctx, NULL, PA_CONTEXT_NOFAIL, NULL);
}

/* ------------------------------------------------------------------ */
/* Actions (called from the main thread)                               */
/* ------------------------------------------------------------------ */

static Dev *find_by_name(Dev *arr, const char *name)
{
    for (int i = 0; i < MAXDEV; i++)
        if (arr[i].used && !strcmp(arr[i].name, name)) return &arr[i];
    for (int i = 0; i < MAXDEV; i++)            /* fallback: first device */
        if (arr[i].used) return &arr[i];
    return NULL;
}

static Dev *cur_sink(void) { return find_by_name(st.sink, st.def_sink); }
static Dev *cur_src(void)  { return find_by_name(st.src, st.def_src); }

static void set_volume(Dev *d, int is_sink, int pct)
{
    if (!d || !ready) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    pa_volume_t v = (pa_volume_t)((uint64_t)PA_VOLUME_NORM * pct / 100);
    pa_cvolume cv = d->cv;
    if (cv.channels == 0) pa_cvolume_set(&cv, 2, v);
    else pa_cvolume_scale(&cv, v);
    d->cv = cv;
    d->vol = pct;
    if (is_sink) OP(pa_context_set_sink_volume_by_index(ctx, d->idx, &cv, NULL, NULL));
    else         OP(pa_context_set_source_volume_by_index(ctx, d->idx, &cv, NULL, NULL));
}

static void toggle_mute(void)
{
    Dev *d = cur_sink();
    if (!d || !ready) return;
    d->mute = !d->mute;
    OP(pa_context_set_sink_mute_by_index(ctx, d->idx, d->mute, NULL, NULL));
}

static uint32_t move_target;

static void move_si_cb(pa_context *c, const pa_sink_input_info *i, int eol, void *u)
{
    (void)u;
    if (eol || !i) return;
    OP(pa_context_move_sink_input_by_index(c, i->index, move_target, NULL, NULL));
}

static void move_so_cb(pa_context *c, const pa_source_output_info *i, int eol, void *u)
{
    (void)u;
    if (eol || !i) return;
    OP(pa_context_move_source_output_by_index(c, i->index, move_target, NULL, NULL));
}

static void make_default(Dev *d, int is_sink)
{
    if (!d || !ready) return;
    move_target = d->idx;
    if (is_sink) {
        snprintf(st.def_sink, sizeof st.def_sink, "%s", d->name);
        OP(pa_context_set_default_sink(ctx, d->name, NULL, NULL));
        OP(pa_context_get_sink_input_info_list(ctx, move_si_cb, NULL));
    } else {
        snprintf(st.def_src, sizeof st.def_src, "%s", d->name);
        OP(pa_context_set_default_source(ctx, d->name, NULL, NULL));
        OP(pa_context_get_source_output_info_list(ctx, move_so_cb, NULL));
    }
}

/* ------------------------------------------------------------------ */
/* Popup layout                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int out_label, out_slider, out_rows, n_out;
    int in_label, in_slider, in_rows, n_in;
    int h;
} Lay;

static int count(Dev *a) { int n = 0; for (int i = 0; i < MAXDEV; i++) n += a[i].used; return n; }

static Lay layout(void)
{
    Lay L;
    L.n_out = count(st.sink);
    L.n_in = count(st.src);
    L.out_label = UI_HEADER_H + 24;
    L.out_slider = L.out_label + 26;
    L.out_rows = L.out_slider + 18;
    L.in_label = L.out_rows + L.n_out * UI_ROW_H + 22;
    L.in_slider = L.in_label + 26;
    L.in_rows = L.in_slider + 18;
    L.h = L.in_rows + L.n_in * UI_ROW_H + 14;
    return L;
}

#define SL_X UI_PAD
#define SL_W (POP_W - 2 * UI_PAD)

static void draw_devs(cairo_t *cr, Dev *arr, const char *def, int y0, GlyphFn g)
{
    Dev *cur = find_by_name(arr, def);
    int row = 0;
    for (int i = 0; i < MAXDEV; i++) {
        if (!arr[i].used) continue;
        int y = y0 + row * UI_ROW_H;
        if (&arr[i] == cur) ui_fill(cr, UI_PAD - 10, y, POP_W - 2 * (UI_PAD - 10), UI_ROW_H - 2, C_ROW);
        g(cr, UI_PAD + 8, y + UI_ROW_H / 2.0 - 1, 15, C_DIM);
        ui_text(cr, arr[i].desc, UI_PAD + 32, y + UI_ROW_H / 2.0 - 1, 13, 0,
                &arr[i] == cur ? C_FG : (Rgba){0.7, 0.7, 0.76, 1}, POP_W - 2 * UI_PAD - 36);
        row++;
    }
}

static void popup_size(int *w, int *h)
{
    LOCK();
    Lay L = layout();
    UNLOCK();
    *w = POP_W;
    *h = L.h;
}

static void popup_draw(cairo_t *cr, int w, int h)
{
    (void)h;
    char buf[32];
    LOCK();
    Lay L = layout();
    Dev *so = cur_sink(), *si = cur_src();
    g_muted = so ? so->mute : 0;
    ui_header(cr, w, glyph_speaker, "Audio", ready ? (g_muted ? "Muted" : "Easy listening") : "Connecting...");
    ui_toggle(cr, w - UI_PAD - UI_TOGGLE_W, 21, !g_muted);

    ui_section(cr, UI_PAD, L.out_label, "Output");
    snprintf(buf, sizeof buf, "%d%%", so ? so->vol : 0);
    ui_text(cr, buf, w - UI_PAD, L.out_label, 11, UI_RIGHT | UI_CAPS, C_DIM, 0);
    ui_slider(cr, SL_X, L.out_slider, SL_W, so ? so->vol / 100.0 : 0, so != NULL);
    draw_devs(cr, st.sink, st.def_sink, L.out_rows, glyph_box);

    ui_sep(cr, L.in_label - 14, w);
    ui_section(cr, UI_PAD, L.in_label, "Input");
    snprintf(buf, sizeof buf, "%d%%", si ? si->vol : 0);
    ui_text(cr, buf, w - UI_PAD, L.in_label, 11, UI_RIGHT | UI_CAPS, C_DIM, 0);
    ui_slider(cr, SL_X, L.in_slider, SL_W, si ? si->vol / 100.0 : 0, si != NULL);
    draw_devs(cr, st.src, st.def_src, L.in_rows, glyph_mic);
    UNLOCK();
}

static Dev *nth(Dev *arr, int n)
{
    for (int i = 0; i < MAXDEV; i++)
        if (arr[i].used && n-- == 0) return &arr[i];
    return NULL;
}

static void popup_motion(int x, int y)
{
    (void)y;
    if (!drag) return;
    LOCK();
    int pct = (int)(ui_slider_value(x, SL_X, SL_W) * 100 + 0.5);
    if (drag == 1) set_volume(cur_sink(), 1, pct);
    else           set_volume(cur_src(), 0, pct);
    UNLOCK();
}

static void popup_button(int x, int y, int button, int pressed)
{
    if (!pressed) { drag = 0; return; }
    LOCK();
    Lay L = layout();
    int out_zone = y < L.in_label - 14;

    if (button == 4 || button == 5) {
        int dir = button == 4 ? SCROLL_STEP : -SCROLL_STEP;
        Dev *d = out_zone ? cur_sink() : cur_src();
        if (d) set_volume(d, out_zone, d->vol + dir);
    } else if (button == 1) {
        if (IN_RECT(x, y, POP_W - UI_PAD - UI_TOGGLE_W, 21, UI_TOGGLE_W, UI_TOGGLE_H)) {
            toggle_mute();
        } else if (abs(y - L.out_slider) <= 12) {
            drag = 1;
            UNLOCK();
            popup_motion(x, y);
            return;
        } else if (abs(y - L.in_slider) <= 12) {
            drag = 2;
            UNLOCK();
            popup_motion(x, y);
            return;
        } else if (y >= L.out_rows && y < L.out_rows + L.n_out * UI_ROW_H) {
            make_default(nth(st.sink, (y - L.out_rows) / UI_ROW_H), 1);
        } else if (y >= L.in_rows && y < L.in_rows + L.n_in * UI_ROW_H) {
            make_default(nth(st.src, (y - L.in_rows) / UI_ROW_H), 0);
        }
    }
    UNLOCK();
}

static void popup_open(void)  { popup_is_open = 1; drag = 0; }
static void popup_close(void) { popup_is_open = 0; drag = 0; }

/* ------------------------------------------------------------------ */
/* Icon / module                                                       */
/* ------------------------------------------------------------------ */

static void draw_icon(cairo_t *cr, int w, int h)
{
    LOCK();
    Dev *d = cur_sink();
    g_muted = d ? d->mute : 0;
    UNLOCK();
    glyph_speaker(cr, w / 2.0, h / 2.0, h * 0.72, g_muted ? C_YELLOW : C_ICON);
}

static int tooltip(char *buf, size_t n)
{
    LOCK();
    Dev *d = cur_sink();
    if (!d) snprintf(buf, n, "No audio device");
    else if (d->mute) snprintf(buf, n, "Muted");
    else snprintf(buf, n, "Volume %d%%", d->vol);
    UNLOCK();
    return 1;
}

static void scroll(int dir)
{
    LOCK();
    Dev *d = cur_sink();
    if (d) set_volume(d, 1, d->vol + dir * SCROLL_STEP);
    UNLOCK();
}

static void middle_click(void)
{
    LOCK();
    toggle_mute();
    UNLOCK();
}

static int tick(void)
{
    /* reconnect if the pulse server went away */
    LOCK();
    if (!ready && ctx) {
        pa_context_state_t s = pa_context_get_state(ctx);
        if (s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED) {
            pa_context_unref(ctx);
            ctx = NULL;
            memset(&st, 0, sizeof st);
            connect_ctx();
        }
    }
    UNLOCK();
    return 0;
}

static int init(void)
{
    ml = pa_threaded_mainloop_new();
    if (!ml) return -1;
    connect_ctx();
    if (pa_threaded_mainloop_start(ml) < 0) return -1;
    return 0;
}

static void fini(void)
{
    if (!ml) return;
    pa_threaded_mainloop_stop(ml);
    if (ctx) pa_context_unref(ctx);
    pa_threaded_mainloop_free(ml);
}

const Module audio_module = {
    .name = "audio", .init = init, .fini = fini, .tick = tick,
    .draw_icon = draw_icon, .tooltip = tooltip, .scroll = scroll,
    .middle_click = middle_click,
    .popup_open = popup_open, .popup_close = popup_close,
    .popup_size = popup_size, .popup_draw = popup_draw,
    .popup_button = popup_button, .popup_motion = popup_motion,
};
