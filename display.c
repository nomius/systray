/* display.c - display brightness (/sys/class/backlight), monitor arrangement
 * by drag & drop and primary display selection (RandR + the xrandr tool).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "common.h"

#include <X11/extensions/Xrandr.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define POP_W    420
#define MAXMON   8
#define AREA_H   140
#define SL_X     UI_PAD
#define SL_W     (POP_W - 2 * UI_PAD)

typedef struct {
    char name[32];
    int active, primary;
    int x, y, w, h;
} Mon;

static Mon mons[MAXMON];
static int nmon;
static char focused[32];

static char bl_dir[320];
static long bl_max;
static int  bl_pct = 100;

static int  slider_drag;
static int  mon_drag = -1;               /* index being dragged               */
static int  drag_x, drag_y;              /* world position of dragged monitor */
static int  press_px, press_py, press_wx, press_wy;
static double m_scale, m_ox, m_oy;       /* world -> area mapping (frozen)    */
static int  refresh_soon;

/* ------------------------------------------------------------------ */
/* Brightness                                                          */
/* ------------------------------------------------------------------ */

static long read_long(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (!f) return -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void find_backlight(void)
{
    DIR *d = opendir("/sys/class/backlight");
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[320];
        snprintf(p, sizeof p, "/sys/class/backlight/%s/max_brightness", e->d_name);
        long m = read_long(p);
        if (m > 0) {
            snprintf(bl_dir, sizeof bl_dir, "/sys/class/backlight/%.255s", e->d_name);
            bl_max = m;
            break;
        }
    }
    closedir(d);
}

static void read_brightness(void)
{
    if (!bl_dir[0]) return;
    char p[360];
    snprintf(p, sizeof p, "%s/brightness", bl_dir);
    long v = read_long(p);
    if (v >= 0) bl_pct = (int)(100.0 * v / bl_max + 0.5);
}

static void set_brightness(int pct)
{
    if (!bl_dir[0]) return;
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    bl_pct = pct;
    long v = bl_max * pct / 100;
    if (v < 1) v = 1;

    char p[360];
    snprintf(p, sizeof p, "%s/brightness", bl_dir);
    FILE *f = fopen(p, "w");
    if (!f) {
        fprintf(stderr, "systray: cannot open %s for writing: %s "
                "(needs a udev rule or membership of the 'video' group)\n",
                p, strerror(errno));
        return;
    }
    fprintf(f, "%ld\n", v);
    if (fclose(f) != 0)
        fprintf(stderr, "systray: writing %s failed: %s\n", p, strerror(errno));
}

/* ------------------------------------------------------------------ */
/* Monitors                                                            */
/* ------------------------------------------------------------------ */

static void refresh_monitors(void)
{
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(g_dpy, g_root);
    if (!res) return;
    RROutput prim = XRRGetOutputPrimary(g_dpy, g_root);
    nmon = 0;
    for (int i = 0; i < res->noutput && nmon < MAXMON; i++) {
        XRROutputInfo *oi = XRRGetOutputInfo(g_dpy, res, res->outputs[i]);
        if (!oi) continue;
        if (oi->connection == RR_Connected) {
            Mon *m = &mons[nmon];
            memset(m, 0, sizeof *m);
            snprintf(m->name, sizeof m->name, "%.31s", oi->name);
            m->primary = res->outputs[i] == prim;
            if (oi->crtc) {
                XRRCrtcInfo *ci = XRRGetCrtcInfo(g_dpy, res, oi->crtc);
                if (ci) {
                    m->active = 1;
                    m->x = ci->x; m->y = ci->y;
                    m->w = ci->width; m->h = ci->height;
                    XRRFreeCrtcInfo(ci);
                }
            }
            nmon++;
        }
        XRRFreeOutputInfo(oi);
    }
    XRRFreeScreenResources(res);

    focused[0] = 0;
    Window r, c;
    int rx, ry, wx, wy;
    unsigned mask;
    if (XQueryPointer(g_dpy, g_root, &r, &c, &rx, &ry, &wx, &wy, &mask))
        for (int i = 0; i < nmon; i++)
            if (mons[i].active && IN_RECT(rx, ry, mons[i].x, mons[i].y, mons[i].w, mons[i].h))
                snprintf(focused, sizeof focused, "%.31s", mons[i].name);
}

static void run_argv(char **argv)
{
    app_run_ex(argv[0], (const char *const *)&argv[1], NULL, NULL);
    refresh_soon = 2;
}
static int overlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Snap the dragged monitor next to one of the others (left/right/above/below)
 * choosing the non-overlapping placement nearest to the drop position. */
static void snap(int di, int *ox, int *oy)
{
    Mon *d = &mons[di];
    long long best = -1;
    int bx = d->x, by = d->y;
    int dx = *ox, dy = *oy;

    for (int o = 0; o < nmon; o++) {
        if (o == di || !mons[o].active) continue;
        Mon *O = &mons[o];
        int cx[16], cy[16], n = 0;
        for (int side = 0; side < 2; side++) {
            int x = side ? O->x + O->w : O->x - d->w;
            int ys[4] = { O->y, O->y + O->h - d->h, O->y + (O->h - d->h) / 2,
                          clampi(dy, O->y - d->h + 64, O->y + O->h - 64) };
            for (int k = 0; k < 4; k++) { cx[n] = x; cy[n] = ys[k]; n++; }
        }
        for (int side = 0; side < 2; side++) {
            int y = side ? O->y + O->h : O->y - d->h;
            int xs[4] = { O->x, O->x + O->w - d->w, O->x + (O->w - d->w) / 2,
                          clampi(dx, O->x - d->w + 64, O->x + O->w - 64) };
            for (int k = 0; k < 4; k++) { cx[n] = xs[k]; cy[n] = y; n++; }
        }
        for (int k = 0; k < n; k++) {
            int bad = 0;
            for (int q = 0; q < nmon; q++) {
                if (q == di || !mons[q].active) continue;
                if (overlaps(cx[k], cy[k], d->w, d->h, mons[q].x, mons[q].y, mons[q].w, mons[q].h))
                    bad = 1;
            }
            if (bad) continue;
            long long ddx = cx[k] - dx, ddy = cy[k] - dy, dist = ddx * ddx + ddy * ddy;
            if (best < 0 || dist < best) { best = dist; bx = cx[k]; by = cy[k]; }
        }
    }
    *ox = bx;
    *oy = by;
}

static void apply_layout(void)
{
    char *argv[4 + MAXMON * 4 + 1];
    char pos[MAXMON][24];
    int n = 0, minx = INT_MAX, miny = INT_MAX;

    for (int i = 0; i < nmon; i++)
        if (mons[i].active) {
            if (mons[i].x < minx) minx = mons[i].x;
            if (mons[i].y < miny) miny = mons[i].y;
        }
    if (minx == INT_MAX) return;

    argv[n++] = "xrandr";
    for (int i = 0; i < nmon; i++) {
        if (!mons[i].active) continue;
        snprintf(pos[i], sizeof pos[i], "%dx%d", mons[i].x - minx, mons[i].y - miny);
        argv[n++] = "--output";
        argv[n++] = mons[i].name;
        argv[n++] = "--pos";
        argv[n++] = pos[i];
    }
    argv[n] = NULL;
    run_argv(argv);
}

static void set_primary(Mon *m)
{
    if (m->active) {
        char *argv[] = {"xrandr", "--output", m->name, "--primary", NULL};
        run_argv(argv);
    } else {
        const char *right_of = NULL;
        for (int i = 0; i < nmon; i++) if (mons[i].active) { right_of = mons[i].name; break; }
        char *argv[] = {"xrandr", "--output", m->name, "--auto", "--primary",
                        (char *)(right_of ? "--right-of" : NULL), (char *)right_of, NULL};
        run_argv(argv);
    }
}

/* ------------------------------------------------------------------ */
/* Glyphs                                                              */
/* ------------------------------------------------------------------ */

static void glyph_monitor(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s / 20.0, s / 20.0);
    ui_color(cr, c);
    cairo_set_line_width(cr, 1.8);
    cairo_rectangle(cr, -9, -8, 18, 12);
    cairo_stroke(cr);
    cairo_move_to(cr, -4, 9); cairo_line_to(cr, 4, 9);
    cairo_move_to(cr, 0, 4);  cairo_line_to(cr, 0, 9);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void glyph_monitors(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    glyph_monitor(cr, cx - s * 0.1, cy + s * 0.08, s * 0.85, c);
    cairo_save(cr);
    ui_color(cr, c);
    cairo_set_line_width(cr, 1.6);
    cairo_move_to(cr, cx - s * 0.2, cy - s * 0.32);
    cairo_line_to(cr, cx + s * 0.42, cy - s * 0.32);
    cairo_line_to(cr, cx + s * 0.42, cy + s * 0.2);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void draw_check(cairo_t *cr, double cx, double cy, Rgba c)
{
    ui_color(cr, c);
    cairo_set_line_width(cr, 1.8);
    cairo_move_to(cr, cx - 5, cy);
    cairo_line_to(cr, cx - 1.5, cy + 4);
    cairo_line_to(cr, cx + 5, cy - 4);
    cairo_stroke(cr);
}

/* ------------------------------------------------------------------ */
/* Popup layout                                                        */
/* ------------------------------------------------------------------ */

typedef struct { int b_label, b_slider, a_label, a_y, l_label, l_rows, h; } Lay;

static Lay layout(void)
{
    Lay L;
    L.b_label = UI_HEADER_H + 24;
    L.b_slider = L.b_label + 26;
    L.a_label = L.b_slider + 40;
    L.a_y = L.a_label + 18;
    L.l_label = L.a_y + AREA_H + 26;
    L.l_rows = L.l_label + 18;
    L.h = L.l_rows + nmon * UI_ROW_H + 12;
    return L;
}

static void compute_map(double *scale, double *ox, double *oy)
{
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN, maxw = 0, maxh = 0;
    for (int i = 0; i < nmon; i++) {
        if (!mons[i].active) continue;
        if (mons[i].x < x0) x0 = mons[i].x;
        if (mons[i].y < y0) y0 = mons[i].y;
        if (mons[i].x + mons[i].w > x1) x1 = mons[i].x + mons[i].w;
        if (mons[i].y + mons[i].h > y1) y1 = mons[i].y + mons[i].h;
        if (mons[i].w > maxw) maxw = mons[i].w;
        if (mons[i].h > maxh) maxh = mons[i].h;
    }
    if (x0 == INT_MAX) { *scale = 0.05; *ox = *oy = 0; return; }
    double ew = (x1 - x0) + 1.2 * maxw, eh = (y1 - y0) + 1.2 * maxh;
    double aw = POP_W - 2 * UI_PAD - 12, ah = AREA_H - 12;
    double s = aw / ew < ah / eh ? aw / ew : ah / eh;
    *scale = s;
    *ox = UI_PAD + 6 + aw / 2 - s * (x0 + x1) / 2.0;
    *oy = 0 + ah / 2 + 6 - s * (y0 + y1) / 2.0;       /* relative to area top */
}

static void mon_rect(int i, double scale, double ox, double oy, double ay,
                     double *x, double *y, double *w, double *h)
{
    int wx = (i == mon_drag) ? drag_x : mons[i].x;
    int wy = (i == mon_drag) ? drag_y : mons[i].y;
    *x = ox + wx * scale;
    *y = ay + oy + wy * scale;
    *w = mons[i].w * scale;
    *h = mons[i].h * scale;
}

static void popup_size(int *w, int *h)
{
    Lay L = layout();
    *w = POP_W;
    *h = L.h;
}

static void popup_draw(cairo_t *cr, int w, int h)
{
    (void)h;
    char buf[64];
    Lay L = layout();

    snprintf(buf, sizeof buf, "%d display%s", nmon, nmon == 1 ? "" : "s");
    ui_header(cr, w, glyph_monitors, "Display", buf);

    ui_section(cr, UI_PAD, L.b_label, "Brightness");
    if (bl_dir[0]) snprintf(buf, sizeof buf, "%d%%", bl_pct);
    else snprintf(buf, sizeof buf, "N/A");
    ui_text(cr, buf, w - UI_PAD, L.b_label, 11, UI_RIGHT | UI_CAPS, C_DIM, 0);
    ui_slider(cr, SL_X, L.b_slider, SL_W, bl_pct / 100.0, bl_dir[0] != 0);
    ui_sep(cr, L.b_slider + 20, w);

    /* arrangement area */
    ui_section(cr, UI_PAD, L.a_label, "Arrangement");
    ui_text(cr, "drag to move", w - UI_PAD, L.a_label, 10, UI_RIGHT | UI_CAPS, C_DIM, 0);
    ui_fill(cr, UI_PAD, L.a_y, w - 2 * UI_PAD, AREA_H, (Rgba){0.055, 0.06, 0.08, 1});
    ui_frame(cr, UI_PAD, L.a_y, w - 2 * UI_PAD, AREA_H, C_LINE);

    double scale, ox, oy;
    if (mon_drag >= 0) { scale = m_scale; ox = m_ox; oy = m_oy; }
    else compute_map(&scale, &ox, &oy);

    cairo_save(cr);
    cairo_rectangle(cr, UI_PAD + 1, L.a_y + 1, w - 2 * UI_PAD - 2, AREA_H - 2);
    cairo_clip(cr);
    int order[MAXMON], n = 0;
    for (int i = 0; i < nmon; i++) if (mons[i].active && i != mon_drag) order[n++] = i;
    if (mon_drag >= 0) order[n++] = mon_drag;          /* dragged one on top */
    for (int k = 0; k < n; k++) {
        int i = order[k];
        double x, y, mw, mh;
        mon_rect(i, scale, ox, oy, L.a_y, &x, &y, &mw, &mh);
        ui_fill(cr, x, y, mw, mh, i == mon_drag ? (Rgba){0.24, 0.26, 0.34, 1} : C_ROW);
        ui_frame(cr, x, y, mw, mh, mons[i].primary ? C_ACCENT : (Rgba){0.35, 0.36, 0.43, 1});
        ui_text(cr, mons[i].name, x + mw / 2, y + mh / 2 - 6, 11, UI_CENTER, C_FG, mw - 4);
        snprintf(buf, sizeof buf, "%dx%d", mons[i].w, mons[i].h);
        ui_text(cr, buf, x + mw / 2, y + mh / 2 + 8, 9, UI_CENTER, C_DIM, mw - 4);
    }
    cairo_restore(cr);

    ui_sep(cr, L.a_y + AREA_H + 12, w);
    ui_section(cr, UI_PAD, L.l_label, "Displays");
    for (int i = 0; i < nmon; i++) {
        int y = L.l_rows + i * UI_ROW_H;
        int foc = !strcmp(focused, mons[i].name);
        if (mons[i].primary) ui_fill(cr, UI_PAD - 10, y, w - 2 * (UI_PAD - 10), UI_ROW_H - 2, C_ROW);
        glyph_monitor(cr, UI_PAD + 8, y + UI_ROW_H / 2.0 - 1, 16, mons[i].active ? C_FG : C_DIM);
        double tw = ui_text(cr, mons[i].name, UI_PAD + 32, y + UI_ROW_H / 2.0 - 1, 13, 0,
                            mons[i].active ? C_FG : C_DIM, 0);
        const char *extra = !mons[i].active ? " . off" : foc ? " . focused" : "";
        ui_text(cr, extra, UI_PAD + 32 + tw, y + UI_ROW_H / 2.0 - 1, 13, 0, C_DIM, 0);
        if (mons[i].primary) draw_check(cr, w - UI_PAD - 6, y + UI_ROW_H / 2.0 - 1, C_DIM);
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void popup_motion(int x, int y)
{
    if (slider_drag) {
        int pct = (int)(ui_slider_value(x, SL_X, SL_W) * 100 + 0.5);
        set_brightness(pct);
    } else if (mon_drag >= 0) {
        drag_x = press_wx + (int)((x - press_px) / m_scale);
        drag_y = press_wy + (int)((y - press_py) / m_scale);
    }
}

static void popup_button(int x, int y, int button, int pressed)
{
    Lay L = layout();

    if (!pressed) {
        if (slider_drag) {
            popup_motion(x, y);
            set_brightness(bl_pct);
            slider_drag = 0;
        }
        if (mon_drag >= 0) {
            int di = mon_drag, nx = drag_x, ny = drag_y;
            mon_drag = -1;
            snap(di, &nx, &ny);
            mons[di].x = nx;
            mons[di].y = ny;
            apply_layout();
        }
        return;
    }
    if (button == 4 || button == 5) {
        if (bl_dir[0] && y < L.a_label - 10)
            set_brightness(bl_pct + (button == 4 ? 5 : -5));
        return;
    }
    if (button != 1) return;

    if (bl_dir[0] && abs(y - L.b_slider) <= 12) {
        slider_drag = 1;
        popup_motion(x, y);
        return;
    }

    if (IN_RECT(x, y, UI_PAD, L.a_y, POP_W - 2 * UI_PAD, AREA_H)) {
        double scale, ox, oy;
        compute_map(&scale, &ox, &oy);
        for (int i = nmon - 1; i >= 0; i--) {
            double rx, ry, rw, rh;
            if (!mons[i].active) continue;
            mon_rect(i, scale, ox, oy, L.a_y, &rx, &ry, &rw, &rh);
            if (nmon > 1 && IN_RECT(x, y, rx, ry, rw, rh)) {
                mon_drag = i;
                m_scale = scale; m_ox = ox; m_oy = oy;
                press_px = x; press_py = y;
                press_wx = drag_x = mons[i].x;
                press_wy = drag_y = mons[i].y;
                return;
            }
        }
        return;
    }

    if (y >= L.l_rows && y < L.l_rows + nmon * UI_ROW_H) {
        int i = (y - L.l_rows) / UI_ROW_H;
        if (i >= 0 && i < nmon) set_primary(&mons[i]);
    }
}

static int is_open;

static void popup_open(void)
{
    is_open = 1;
    slider_drag = 0;
    mon_drag = -1;
    refresh_monitors();
    read_brightness();
}

static void popup_close(void)
{
    is_open = 0;
    slider_drag = 0;
    mon_drag = -1;
}

/* ------------------------------------------------------------------ */
/* Icon / module                                                       */
/* ------------------------------------------------------------------ */

static void draw_icon(cairo_t *cr, int w, int h)
{
    glyph_monitor(cr, w / 2.0, h / 2.0, h * 0.78, C_ICON);
}

static int tick(void)
{
    static int n;
    n++;
    if (refresh_soon > 0 && --refresh_soon == 0) { refresh_monitors(); return 1; }
    if (is_open && mon_drag < 0 && !slider_drag && (n % 2) == 0) {
        /* cheap refresh so an open popup follows external changes */
        refresh_monitors();
        read_brightness();
        return 1;
    }
    return 0;
}

static int init(void)
{
    find_backlight();
    read_brightness();
    refresh_monitors();
    return 0;
}

const Module display_module = {
    .name = "display", .init = init, .tick = tick, .draw_icon = draw_icon,
    .popup_open = popup_open, .popup_close = popup_close,
    .popup_size = popup_size, .popup_draw = popup_draw,
    .popup_button = popup_button, .popup_motion = popup_motion,
};
