/* tray.c - system tray host/client, popup windows, tooltips and UI toolkit.
 *
 * Icon order: network, display, audio, battery.
 * Each icon is a separate XEmbed client window docked into the system tray
 * (_NET_SYSTEM_TRAY_Sn).  Popups are override-redirect (borderless) windows
 * with an active pointer grab, so a click anywhere outside closes them.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "common.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <cairo/cairo-xlib.h>
#include <glib.h>
#include <pango/pangocairo.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Globals / theme                                                     */
/* ------------------------------------------------------------------ */

Display *g_dpy;
Window   g_root;
int      g_screen;

const Rgba C_BG     = {0.075, 0.082, 0.105, 1.0};
const Rgba C_FG     = {0.84, 0.84, 0.90, 1.0};
const Rgba C_DIM    = {0.42, 0.44, 0.51, 1.0};
const Rgba C_ACCENT = {0.43, 0.52, 0.86, 1.0};
const Rgba C_ROW    = {0.165, 0.175, 0.225, 1.0};
const Rgba C_LINE   = {0.16, 0.17, 0.21, 1.0};
const Rgba C_ICON   = {0.62, 0.63, 0.68, 1.0};
const Rgba C_RED    = {0.92, 0.25, 0.25, 1.0};
const Rgba C_YELLOW = {0.96, 0.80, 0.18, 1.0};
const Rgba C_BLUE   = {0.30, 0.60, 1.00, 1.0};
const Rgba C_FILL   = {0.68, 0.68, 0.78, 1.0};

#define UI_FONT "JetBrains Mono, DejaVu Sans Mono, monospace"

static int wake_pipe[2] = {-1, -1};

void app_wake(void)
{
    char b = 1;
    if (write(wake_pipe[1], &b, 1) < 0) { /* pipe full: a wake is pending */ }
}

void app_run(const char *path, const char *a1, const char *a2, const char *a3)
{
    const char *args[4] = {a1, a2, a3, NULL};
    app_run_ex(path, args, NULL, NULL);
}

/* args: NULL-terminated, at most 62.  If env_name is set it is exported to the
 * child with env_val.  A value equal to env_val is masked in the debug log. */
void app_run_ex(const char *path, const char *const *args,
                const char *env_name, const char *env_val)
{
    const char *argv[64];
    int n = 0;
    argv[n++] = path;
    for (int k = 0; args && args[k] && n < 63; k++) argv[n++] = args[k];
    argv[n] = NULL;

    fprintf(stderr, "systray: app_run: exec '%s'", path);
    for (int k = 1; k < n; k++)
        fprintf(stderr, " [arg%d='%s']", k, argv[k]);
    if (env_name) fprintf(stderr, " [env %s=***]", env_name);
    if (strchr(path, '/')) {
        if (access(path, F_OK) != 0)
            fprintf(stderr, " -- WARNING: file not found (%s)", strerror(errno));
        else if (access(path, X_OK) != 0)
            fprintf(stderr, " -- WARNING: not executable (%s)", strerror(errno));
    } else {
        fprintf(stderr, " (looked up in PATH)");
    }
    fputc('\n', stderr);

    /* Double fork: the grandchild is re-parented to init, so we never need
     * SIGCHLD ignored/handled (which would break pclose() on popen children). */
    pid_t p = fork();
    if (p < 0) {
        fprintf(stderr, "systray: app_run: fork failed: %s\n", strerror(errno));
    } else if (p == 0) {
        if (fork() != 0) _exit(0);
        setsid();
        if (env_name && env_val) setenv(env_name, env_val, 1);
        execvp(path, (char *const *)argv);
        fprintf(stderr, "systray: app_run: execvp('%s') failed: %s (errno %d)\n",
                path, strerror(errno), errno);
        _exit(127);
    } else {
        int st;
        waitpid(p, &st, 0);
        fprintf(stderr, "systray: app_run: launched '%s'\n", path);
    }
}
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ------------------------------------------------------------------ */
/* UI toolkit                                                          */
/* ------------------------------------------------------------------ */

static PangoLayout *make_layout(cairo_t *cr, const char *s, double px, int fl)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(UI_FONT);
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    if (fl & UI_BOLD)
        pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    if (fl & UI_CAPS) {
        PangoAttrList *al = pango_attr_list_new();
        pango_attr_list_insert(al, pango_attr_letter_spacing_new(1.2 * PANGO_SCALE));
        pango_layout_set_attributes(l, al);
        pango_attr_list_unref(al);
    }
    pango_layout_set_text(l, s, -1);
    return l;
}

double ui_text(cairo_t *cr, const char *s, double x, double yc, double px,
               int fl, Rgba c, double maxw)
{
    if (!s || !*s) return 0;
    char *up = (fl & UI_CAPS) ? g_utf8_strup(s, -1) : NULL;
    PangoLayout *l = make_layout(cr, up ? up : s, px, fl);
    if (maxw > 0) {
        pango_layout_set_width(l, (int)(maxw * PANGO_SCALE));
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    }
    int lw, lh;
    pango_layout_get_pixel_size(l, &lw, &lh);
    double tx = x;
    if (fl & UI_RIGHT) tx = x - lw;
    else if (fl & UI_CENTER) tx = x - lw / 2.0;
    ui_color(cr, c);
    cairo_move_to(cr, tx, yc - lh / 2.0);
    pango_cairo_show_layout(cr, l);
    cairo_new_path(cr);        /* move_to leaves a current point behind */
    g_object_unref(l);
    g_free(up);
    return lw;
}

double ui_text_width(const char *s, double px, int fl)
{
    static cairo_surface_t *ms;
    if (!ms) ms = cairo_image_surface_create(CAIRO_FORMAT_A8, 8, 8);
    cairo_t *cr = cairo_create(ms);
    char *up = (fl & UI_CAPS) ? g_utf8_strup(s, -1) : NULL;
    PangoLayout *l = make_layout(cr, up ? up : s, px, fl);
    int lw, lh;
    pango_layout_get_pixel_size(l, &lw, &lh);
    g_object_unref(l);
    g_free(up);
    cairo_destroy(cr);
    return lw;
}

void ui_fill(cairo_t *cr, double x, double y, double w, double h, Rgba c)
{
    ui_color(cr, c);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

void ui_frame(cairo_t *cr, double x, double y, double w, double h, Rgba c)
{
    ui_color(cr, c);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, x + .5, y + .5, w - 1, h - 1);
    cairo_stroke(cr);
}

void ui_sep(cairo_t *cr, double y, double w)
{
    ui_fill(cr, UI_PAD, y, w - 2 * UI_PAD, 1, C_LINE);
}

void ui_header(cairo_t *cr, int w, GlyphFn g, const char *title, const char *sub)
{
    if (g) g(cr, UI_PAD + 16, UI_HEADER_H / 2.0, 30, C_DIM);
    ui_text(cr, title, UI_PAD + 52, 24, 17, UI_BOLD, C_FG, w - UI_PAD * 2 - 52 - 70);
    ui_text(cr, sub, UI_PAD + 52, 46, 11, UI_CAPS, C_DIM, w - UI_PAD * 2 - 52 - 70);
    ui_sep(cr, UI_HEADER_H, w);
}

void ui_toggle(cairo_t *cr, double x, double y, int on)
{
    Rgba bg = {0.10, 0.11, 0.14, 1};
    ui_fill(cr, x, y, UI_TOGGLE_W, UI_TOGGLE_H, bg);
    double k = UI_TOGGLE_H - 6;
    ui_fill(cr, on ? x + UI_TOGGLE_W - k - 3 : x + 3, y + 3, k, k,
            on ? C_FILL : C_DIM);
}

void ui_slider(cairo_t *cr, double x, double yc, double w, double frac, int en)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    Rgba tr = {0.21, 0.22, 0.27, 1};
    Rgba fg = en ? C_FILL : C_DIM;
    ui_fill(cr, x, yc - 1.5, w, 3, tr);
    ui_fill(cr, x, yc - 1.5, w * frac, 3, fg);
    ui_color(cr, fg);
    cairo_arc(cr, x + w * frac, yc, 6.5, 0, 2 * G_PI);
    cairo_fill(cr);
}

double ui_slider_value(int px, int x, int w)
{
    double v = (double)(px - x) / (double)w;
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

void ui_button(cairo_t *cr, double x, double y, double w, double h,
               const char *label, GlyphFn g, int sel)
{
    if (sel) ui_fill(cr, x, y, w, h, C_ROW);
    ui_frame(cr, x, y, w, h, (Rgba){0.33, 0.34, 0.41, 1});
    double tw = ui_text_width(label, 12, 0), gw = g ? 22 : 0;
    double sx = x + (w - tw - gw) / 2;
    Rgba c = sel ? C_FG : (Rgba){0.62, 0.63, 0.70, 1};
    if (g) g(cr, sx + 7, y + h / 2.0, 15, c);
    ui_text(cr, label, sx + gw, y + h / 2.0, 12, 0, c, 0);
}

void ui_section(cairo_t *cr, double x, double yc, const char *text)
{
    ui_text(cr, text, x, yc, 11, UI_CAPS | UI_BOLD, C_DIM, 0);
}

/* ------------------------------------------------------------------ */
/* Tray icons                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    const Module *m;
    Window win;
    int w, h;
} Icon;

static const Module *const mods[] = {
    &network_module, &display_module, &audio_module, &battery_module
};
static Icon icons[4];
static int  nicons;

static Atom a_sel, a_opcode, a_manager, a_xembed_info;
static Window last_owner;

static Icon *icon_for(Window w)
{
    for (int i = 0; i < nicons; i++)
        if (icons[i].win && icons[i].win == w) return &icons[i];
    return NULL;
}

static void icon_draw(Icon *i)
{
    if (!i->win) return;
    XClearArea(g_dpy, i->win, 0, 0, 0, 0, False);   /* ParentRelative bg */
    cairo_surface_t *s = cairo_xlib_surface_create(
        g_dpy, i->win, DefaultVisual(g_dpy, g_screen), i->w, i->h);
    cairo_t *cr = cairo_create(s);
    i->m->draw_icon(cr, i->w, i->h);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    cairo_surface_destroy(s);
    XFlush(g_dpy);
}

static void icon_create(Icon *i)
{
    int h = 24;
    int w = i->m->icon_width ? i->m->icon_width(h) : h;
    Window win = XCreateSimpleWindow(g_dpy, g_root, 0, 0, w, h, 0, 0, 0);
    XSetWindowBackgroundPixmap(g_dpy, win, ParentRelative);
    XSelectInput(g_dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 StructureNotifyMask | EnterWindowMask | LeaveWindowMask);

    XSizeHints sh;
    memset(&sh, 0, sizeof sh);
    sh.flags = PMinSize | PBaseSize;
    sh.min_width = sh.base_width = w;
    sh.min_height = sh.base_height = h;
    XSetWMNormalHints(g_dpy, win, &sh);

    XClassHint ch = {(char *)"systray", (char *)"Systray"};
    XSetClassHint(g_dpy, win, &ch);

    long xe[2] = {0, 1};                      /* version 0, XEMBED_MAPPED */
    XChangeProperty(g_dpy, win, a_xembed_info, a_xembed_info, 32,
                    PropModeReplace, (unsigned char *)xe, 2);
    i->win = win;
    i->w = w;
    i->h = h;
}

static void icon_dock(Icon *i, Window owner)
{
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = owner;
    e.xclient.message_type = a_opcode;
    e.xclient.format = 32;
    e.xclient.data.l[0] = CurrentTime;
    e.xclient.data.l[1] = 0;                  /* SYSTEM_TRAY_REQUEST_DOCK */
    e.xclient.data.l[2] = i->win;
    XSendEvent(g_dpy, owner, False, NoEventMask, &e);
    XSync(g_dpy, False);
}

/* (Re)create and dock icons whenever a tray manager (re)appears. */
static void check_tray(void)
{
    Window o = XGetSelectionOwner(g_dpy, a_sel);
    if (o != last_owner) {
        for (int i = 0; i < nicons; i++)
            if (icons[i].win) {
                XDestroyWindow(g_dpy, icons[i].win);
                icons[i].win = 0;
            }
        last_owner = o;
    }
    if (!o) return;
    for (int i = 0; i < nicons; i++) {
        if (icons[i].win) continue;
        icon_create(&icons[i]);
        icon_dock(&icons[i], o);
        usleep(60000);                        /* keep the dock order */
    }
}

static void redraw_icons(void)
{
    for (int i = 0; i < nicons; i++) icon_draw(&icons[i]);
}

/* ------------------------------------------------------------------ */
/* Placement                                                           */
/* ------------------------------------------------------------------ */

static void monitor_for(int cx, int cy, int *mx, int *my, int *mw, int *mh)
{
    *mx = 0; *my = 0;
    *mw = DisplayWidth(g_dpy, g_screen);
    *mh = DisplayHeight(g_dpy, g_screen);
    int n = 0;
    XRRMonitorInfo *mi = XRRGetMonitors(g_dpy, g_root, True, &n);
    if (!mi) return;
    for (int i = 0; i < n; i++)
        if (cx >= mi[i].x && cx < mi[i].x + mi[i].width &&
            cy >= mi[i].y && cy < mi[i].y + mi[i].height) {
            *mx = mi[i].x; *my = mi[i].y; *mw = mi[i].width; *mh = mi[i].height;
            break;
        }
    XRRFreeMonitors(mi);
}

static void place(Window anchor, int pw, int ph, int *ox, int *oy,
                  int *below, int *anchor_y)
{
    int ax = 0, ay = 0;
    Window c;
    XWindowAttributes wa;
    XTranslateCoordinates(g_dpy, anchor, g_root, 0, 0, &ax, &ay, &c);
    XGetWindowAttributes(g_dpy, anchor, &wa);
    int mx, my, mw, mh;
    monitor_for(ax + wa.width / 2, ay + wa.height / 2, &mx, &my, &mw, &mh);

    int x = ax + wa.width / 2 - pw / 2;
    if (x > mx + mw - pw - 4) x = mx + mw - pw - 4;
    if (x < mx + 4) x = mx + 4;
    *ox = x;
    *below = (ay + wa.height / 2 < my + mh / 2);
    if (*below) { *anchor_y = ay + wa.height + 4; *oy = *anchor_y; }
    else        { *anchor_y = ay - 4;             *oy = *anchor_y - ph; }
}

/* ------------------------------------------------------------------ */
/* Popup                                                               */
/* ------------------------------------------------------------------ */

static struct {
    const Module *m;
    Window win;
    int x, y, w, h, below, anchor_y;
    cairo_surface_t *surf;
} pop;

static void popup_paint(void)
{
    if (!pop.m) return;
    int w, h;
    pop.m->popup_size(&w, &h);
    if (w != pop.w || h != pop.h) {
        pop.w = w; pop.h = h;
        pop.y = pop.below ? pop.anchor_y : pop.anchor_y - h;
        XMoveResizeWindow(g_dpy, pop.win, pop.x, pop.y, w, h);
        cairo_xlib_surface_set_size(pop.surf, w, h);
    }
    cairo_t *cr = cairo_create(pop.surf);
    cairo_push_group(cr);
    ui_fill(cr, 0, 0, w, h, C_BG);
    pop.m->popup_draw(cr, w, h);
    ui_frame(cr, 0, 0, w, h, (Rgba){C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, 0.75});
    cairo_pop_group_to_source(cr);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(pop.surf);
    XFlush(g_dpy);
}

void app_popup_redraw(void) { popup_paint(); }

void app_popup_close(void)
{
    if (!pop.m) return;
    XUngrabPointer(g_dpy, CurrentTime);
    if (pop.surf) cairo_surface_destroy(pop.surf);
    XDestroyWindow(g_dpy, pop.win);
    if (pop.m->popup_close) pop.m->popup_close();
    memset(&pop, 0, sizeof pop);
    XFlush(g_dpy);
}

static void popup_open(Icon *ic)
{
    const Module *m = ic->m;
    if (!m->popup_size || !m->popup_draw) return;
    if (m->popup_open) m->popup_open();

    int w, h, x, y, below, ay;
    m->popup_size(&w, &h);
    place(ic->win, w, h, &x, &y, &below, &ay);

    XSetWindowAttributes at;
    at.override_redirect = True;               /* borderless, no WM */
    at.background_pixmap = None;
    at.save_under = True;
    at.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                    PointerMotionMask;
    Window win = XCreateWindow(g_dpy, g_root, x, y, w, h, 0, CopyFromParent,
                               InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixmap | CWSaveUnder |
                               CWEventMask, &at);
    XMapRaised(g_dpy, win);

    int ok = 0;
    for (int tries = 0; tries < 20 && !ok; tries++) {
        int r = XGrabPointer(g_dpy, win, True,
                             ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                             GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        if (r == GrabSuccess) ok = 1; else usleep(10000);
    }
    if (!ok) {
        XDestroyWindow(g_dpy, win);
        if (m->popup_close) m->popup_close();
        return;
    }
    pop.m = m; pop.win = win;
    pop.x = x; pop.y = y; pop.w = w; pop.h = h;
    pop.below = below; pop.anchor_y = ay;
    pop.surf = cairo_xlib_surface_create(g_dpy, win,
                                         DefaultVisual(g_dpy, g_screen), w, h);
    popup_paint();
}

/* ------------------------------------------------------------------ */
/* Tooltip                                                             */
/* ------------------------------------------------------------------ */

static struct { Window win; int pending, icon; long due; char text[128]; } tip;

static void tooltip_hide(void)
{
    tip.pending = 0;
    if (tip.win) {
        XDestroyWindow(g_dpy, tip.win);
        tip.win = 0;
        XFlush(g_dpy);
    }
}

static void tooltip_paint(int w, int h)
{
    cairo_surface_t *s = cairo_xlib_surface_create(
        g_dpy, tip.win, DefaultVisual(g_dpy, g_screen), w, h);
    cairo_t *cr = cairo_create(s);
    ui_fill(cr, 0, 0, w, h, C_BG);
    ui_frame(cr, 0, 0, w, h, C_ACCENT);
    ui_text(cr, tip.text, w / 2.0, h / 2.0, 12, UI_CENTER, C_FG, 0);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    cairo_surface_destroy(s);
    XFlush(g_dpy);
}

static void tooltip_show(void)
{
    tip.pending = 0;
    if (pop.m || tip.icon >= nicons || !icons[tip.icon].win) return;
    Icon *ic = &icons[tip.icon];
    if (!ic->m->tooltip || !ic->m->tooltip(tip.text, sizeof tip.text)) return;

    int w = (int)ui_text_width(tip.text, 12, 0) + 20, h = 26;
    int x, y, below, ay;
    place(ic->win, w, h, &x, &y, &below, &ay);

    XSetWindowAttributes at;
    at.override_redirect = True;
    at.background_pixmap = None;
    at.event_mask = ExposureMask;
    tip.win = XCreateWindow(g_dpy, g_root, x, y, w, h, 0, CopyFromParent,
                            InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackPixmap | CWEventMask, &at);
    XMapRaised(g_dpy, tip.win);
    tooltip_paint(w, h);
}

/* ------------------------------------------------------------------ */
/* Password dialog                                                     */
/* ------------------------------------------------------------------ */

#define DLG_W    420
#define DLG_H    190
#define DLG_BW   110
#define DLG_BH   34
#define DLG_BY   122
#define DLG_MAXVIS 40

static struct {
    Window win;
    int x, y;
    cairo_surface_t *surf;
    char title[96], prompt[96], buf[128];
    int len;
    void (*cb)(const char *, void *);
    void *ud;
} dlg;

static void dialog_paint(void)
{
    if (!dlg.win) return;
    cairo_t *cr = cairo_create(dlg.surf);
    cairo_push_group(cr);
    ui_fill(cr, 0, 0, DLG_W, DLG_H, C_BG);
    ui_text(cr, dlg.title, UI_PAD, 28, 16, UI_BOLD, C_FG, DLG_W - 2 * UI_PAD);
    ui_text(cr, dlg.prompt, UI_PAD, 54, 11, UI_CAPS, C_DIM, DLG_W - 2 * UI_PAD);

    ui_fill(cr, UI_PAD, 72, DLG_W - 2 * UI_PAD, 36, (Rgba){0.055, 0.06, 0.08, 1});
    ui_frame(cr, UI_PAD, 72, DLG_W - 2 * UI_PAD, 36, C_ACCENT);
    char dots[DLG_MAXVIS * 3 + 1];
    int vis = dlg.len < DLG_MAXVIS ? dlg.len : DLG_MAXVIS;
    for (int i = 0; i < vis; i++) memcpy(dots + i * 3, "\xE2\x80\xA2", 3);
    dots[vis * 3] = 0;
    double tw = ui_text(cr, dots, UI_PAD + 12, 90, 14, 0, C_FG, 0);
    ui_fill(cr, UI_PAD + 13 + tw, 80, 1.5, 20, C_FG);

    ui_button(cr, DLG_W - UI_PAD - 2 * DLG_BW - 10, DLG_BY, DLG_BW, DLG_BH, "Cancel", NULL, 0);
    ui_button(cr, DLG_W - UI_PAD - DLG_BW, DLG_BY, DLG_BW, DLG_BH, "Connect", NULL, dlg.len > 0);
    ui_text(cr, "Enter: connect   Esc: cancel", UI_PAD, DLG_H - 16, 10, UI_CAPS, C_DIM, 0);

    ui_frame(cr, 0, 0, DLG_W, DLG_H, (Rgba){C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, 0.75});
    cairo_pop_group_to_source(cr);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(dlg.surf);
    XFlush(g_dpy);
}

static void dialog_finish(int ok)
{
    void (*cb)(const char *, void *) = dlg.cb;
    void *ud = dlg.ud;
    char pw[sizeof dlg.buf];
    memcpy(pw, dlg.buf, sizeof pw);

    XUngrabKeyboard(g_dpy, CurrentTime);
    XUngrabPointer(g_dpy, CurrentTime);
    if (dlg.surf) cairo_surface_destroy(dlg.surf);
    XDestroyWindow(g_dpy, dlg.win);
    XFlush(g_dpy);
    explicit_bzero(&dlg, sizeof dlg);

    if (cb) cb(ok ? pw : NULL, ud);
    explicit_bzero(pw, sizeof pw);
}

void app_ask_password(const char *title, const char *prompt,
                      void (*cb)(const char *, void *), void *ud)
{
    if (dlg.win) dialog_finish(0);

    int x, y;
    if (pop.m) { x = pop.x + (pop.w - DLG_W) / 2; y = pop.y; }
    else {
        Window r, c; int wx, wy; unsigned m;
        XQueryPointer(g_dpy, g_root, &r, &c, &x, &y, &wx, &wy, &m);
        x -= DLG_W / 2;
    }
    int mx, my, mw, mh;
    monitor_for(x + DLG_W / 2, y + 10, &mx, &my, &mw, &mh);
    if (x > mx + mw - DLG_W - 4) x = mx + mw - DLG_W - 4;
    if (x < mx + 4) x = mx + 4;
    if (y > my + mh - DLG_H - 4) y = my + mh - DLG_H - 4;
    if (y < my + 4) y = my + 4;
    app_popup_close();

    XSetWindowAttributes at;
    at.override_redirect = True;
    at.background_pixmap = None;
    at.save_under = True;
    at.event_mask = ExposureMask | ButtonPressMask | KeyPressMask;
    Window win = XCreateWindow(g_dpy, g_root, x, y, DLG_W, DLG_H, 0, CopyFromParent,
                               InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixmap | CWSaveUnder |
                               CWEventMask, &at);
    XMapRaised(g_dpy, win);

    int ok = 0;
    for (int t = 0; t < 20 && !ok; t++) {
        if (XGrabPointer(g_dpy, win, True, ButtonPressMask, GrabModeAsync,
                         GrabModeAsync, None, None, CurrentTime) == GrabSuccess &&
            XGrabKeyboard(g_dpy, win, True, GrabModeAsync, GrabModeAsync,
                          CurrentTime) == GrabSuccess)
            ok = 1;
        else { XUngrabPointer(g_dpy, CurrentTime); usleep(10000); }
    }
    if (!ok) {
        XDestroyWindow(g_dpy, win);
        fprintf(stderr, "systray: password dialog: could not grab input\n");
        if (cb) cb(NULL, ud);
        return;
    }

    memset(&dlg, 0, sizeof dlg);
    dlg.win = win; dlg.x = x; dlg.y = y; dlg.cb = cb; dlg.ud = ud;
    snprintf(dlg.title, sizeof dlg.title, "%.95s", title ? title : "");
    snprintf(dlg.prompt, sizeof dlg.prompt, "%.95s", prompt ? prompt : "Password");
    dlg.surf = cairo_xlib_surface_create(g_dpy, win, DefaultVisual(g_dpy, g_screen),
                                         DLG_W, DLG_H);
    dialog_paint();
}

static void dialog_key(XKeyEvent *k)
{
    char b[32];
    KeySym ks;
    int n = XLookupString(k, b, sizeof b, &ks, NULL);

    if (ks == XK_Escape) { dialog_finish(0); return; }
    if (ks == XK_Return || ks == XK_KP_Enter) {
        if (dlg.len > 0) dialog_finish(1);
        return;
    }
    if (ks == XK_BackSpace) {
        if (dlg.len > 0) dlg.buf[--dlg.len] = 0;
    } else {
        for (int i = 0; i < n; i++)
            if ((unsigned char)b[i] >= 0x20 && (unsigned char)b[i] < 0x7f &&
                dlg.len < (int)sizeof dlg.buf - 1) {
                dlg.buf[dlg.len++] = b[i];
                dlg.buf[dlg.len] = 0;
            }
    }
    dialog_paint();
}

static void dialog_button(int rx, int ry)
{
    int x = rx - dlg.x, y = ry - dlg.y;
    if (!IN_RECT(x, y, 0, 0, DLG_W, DLG_H)) { dialog_finish(0); return; }   /* outside */
    if (IN_RECT(x, y, DLG_W - UI_PAD - 2 * DLG_BW - 10, DLG_BY, DLG_BW, DLG_BH))
        dialog_finish(0);
    else if (IN_RECT(x, y, DLG_W - UI_PAD - DLG_BW, DLG_BY, DLG_BW, DLG_BH) && dlg.len > 0)
        dialog_finish(1);
}

/* ------------------------------------------------------------------ */
/* Event handling                                                      */
/* ------------------------------------------------------------------ */

static int x_error(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

static void redraw_all(void)
{
    redraw_icons();
    popup_paint();
}

static void handle_event(XEvent *ev)
{
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count) break;
        if (dlg.win && ev->xexpose.window == dlg.win) dialog_paint();
        else if (pop.m && ev->xexpose.window == pop.win) popup_paint();
        else if (tip.win && ev->xexpose.window == tip.win) {
            XWindowAttributes wa;
            XGetWindowAttributes(g_dpy, tip.win, &wa);
            tooltip_paint(wa.width, wa.height);
        } else {
            Icon *i = icon_for(ev->xexpose.window);
            if (i) icon_draw(i);
        }
        break;

    case ConfigureNotify: {
        Icon *i = icon_for(ev->xconfigure.window);
        if (i) {
            i->w = ev->xconfigure.width;
            i->h = ev->xconfigure.height;
            icon_draw(i);
        }
        break;
    }

    case DestroyNotify: {
        Icon *i = icon_for(ev->xdestroywindow.window);
        if (i) i->win = 0;
        break;
    }

    case ClientMessage:
        if (ev->xclient.message_type == a_manager &&
            (Atom)ev->xclient.data.l[1] == a_sel)
            check_tray();
        break;

    case EnterNotify: {
        Icon *i = icon_for(ev->xcrossing.window);
        if (i && !pop.m && i->m->tooltip) {
            tip.icon = (int)(i - icons);
            tip.pending = 1;
            tip.due = now_ms() + 350;
        }
        break;
    }

    case KeyPress:
        if (dlg.win) dialog_key(&ev->xkey);
        break;

    case LeaveNotify:
        if (icon_for(ev->xcrossing.window)) tooltip_hide();
        break;

    case ButtonPress: {
        XButtonEvent *b = &ev->xbutton;
        if (dlg.win) { dialog_button(b->x_root, b->y_root); break; }
        Icon *i = icon_for(b->window);
        if (i) {
            tooltip_hide();
            if (b->button == 4 || b->button == 5) {
                if (i->m->scroll) i->m->scroll(b->button == 4 ? 1 : -1);
                redraw_all();
            } else if (b->button == 2) {
                if (i->m->middle_click) i->m->middle_click();
                redraw_all();
            } else if (b->button == 1) {
                int same = pop.m == i->m;
                app_popup_close();
                if (!same) popup_open(i);
            }
            break;
        }
        if (pop.m) {
            int x = b->x_root - pop.x, y = b->y_root - pop.y;
            if (IN_RECT(x, y, 0, 0, pop.w, pop.h)) {
                if (pop.m->popup_button) pop.m->popup_button(x, y, b->button, 1);
                popup_paint();
            } else {
                app_popup_close();          /* click outside */
            }
        }
        break;
    }

    case ButtonRelease:
        if (pop.m) {
            XButtonEvent *b = &ev->xbutton;
            if (!icon_for(b->window) && pop.m->popup_button)
                pop.m->popup_button(b->x_root - pop.x, b->y_root - pop.y,
                                    b->button, 0);
            popup_paint();
        }
        break;

    case MotionNotify:
        if (pop.m && pop.m->popup_motion) {
            while (XCheckTypedEvent(g_dpy, MotionNotify, ev)) { }
            pop.m->popup_motion(ev->xmotion.x_root - pop.x,
                                ev->xmotion.y_root - pop.y);
            popup_paint();
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "systray: cannot open display\n");
        return 1;
    }
    XSetErrorHandler(x_error);
    g_screen = DefaultScreen(g_dpy);
    g_root = RootWindow(g_dpy, g_screen);

    if (pipe(wake_pipe) < 0) { perror("pipe"); return 1; }
    for (int k = 0; k < 2; k++)
        fcntl(wake_pipe[k], F_SETFL, fcntl(wake_pipe[k], F_GETFL) | O_NONBLOCK);

    char name[64];
    snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", g_screen);
    a_sel = XInternAtom(g_dpy, name, False);
    a_opcode = XInternAtom(g_dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    a_manager = XInternAtom(g_dpy, "MANAGER", False);
    a_xembed_info = XInternAtom(g_dpy, "_XEMBED_INFO", False);
    XSelectInput(g_dpy, g_root, StructureNotifyMask);

    for (size_t k = 0; k < sizeof mods / sizeof mods[0]; k++) {
        if (mods[k]->init && mods[k]->init() != 0) {
            fprintf(stderr, "systray: %s unavailable, icon skipped\n", mods[k]->name);
            continue;
        }
        icons[nicons++].m = mods[k];
    }

    check_tray();
    redraw_icons();

    int xfd = ConnectionNumber(g_dpy);
    long last_tick = now_ms();

    for (;;) {
        while (XPending(g_dpy)) {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            handle_event(&ev);
        }
        XFlush(g_dpy);

        struct pollfd fds[2] = {{xfd, POLLIN, 0}, {wake_pipe[0], POLLIN, 0}};
        if (poll(fds, 2, 100) < 0 && errno != EINTR) break;

        int dirty = 0;
        if (fds[1].revents & POLLIN) {
            char buf[64];
            while (read(wake_pipe[0], buf, sizeof buf) > 0) { }
            dirty = 1;
        }

        long t = now_ms();
        if (tip.pending && t >= tip.due) tooltip_show();

        if (t - last_tick >= 1000) {
            last_tick = t;
            check_tray();
            for (int i = 0; i < nicons; i++)
                if (icons[i].m->tick && icons[i].m->tick()) dirty = 1;
        }
        if (dirty) redraw_all();
    }

    for (int i = 0; i < nicons; i++)
        if (icons[i].m->fini) icons[i].m->fini();
    XCloseDisplay(g_dpy);
    return 0;
}
