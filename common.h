/* common.h - shared declarations for the systray application.
 *
 * Every tray icon is implemented as a "Module" (audio.c, display.c,
 * network.c, battery.c).  tray.c owns the X11 plumbing: it docks the icons,
 * creates/positions/grabs the popup windows, shows tooltips and dispatches
 * input to the modules.  Modules only draw (cairo/pango) and react to input
 * in popup-local coordinates.
 */
#ifndef SYSTRAY_COMMON_H
#define SYSTRAY_COMMON_H

#include <stddef.h>
#include <X11/Xlib.h>
#include <cairo/cairo.h>

typedef struct { double r, g, b, a; } Rgba;

extern const Rgba C_BG, C_FG, C_DIM, C_ACCENT, C_ROW, C_LINE, C_ICON, C_RED,
                  C_YELLOW, C_BLUE, C_FILL;

extern Display *g_dpy;
extern Window   g_root;
extern int      g_screen;

/* Draws a glyph centred on (cx,cy), roughly s x s pixels. */
typedef void (*GlyphFn)(cairo_t *cr, double cx, double cy, double s, Rgba c);

typedef struct Module {
    const char *name;
    int  (*init)(void);                       /* 0 = ok, -1 = unavailable   */
    void (*fini)(void);
    int  (*tick)(void);                       /* ~1 Hz, !=0 -> redraw       */
    int  (*icon_width)(int h);                /* desired icon width         */
    void (*draw_icon)(cairo_t *cr, int w, int h);
    int  (*tooltip)(char *buf, size_t n);     /* !=0 -> has tooltip         */
    void (*scroll)(int dir);                  /* +1 up / -1 down over icon  */
    void (*middle_click)(void);
    void (*popup_open)(void);
    void (*popup_close)(void);
    void (*popup_size)(int *w, int *h);
    void (*popup_draw)(cairo_t *cr, int w, int h);
    void (*popup_button)(int x, int y, int button, int pressed);
    void (*popup_motion)(int x, int y);
} Module;

extern const Module audio_module, display_module, network_module, battery_module;

/* ---- scripts (override with environment variables) -------------------- */
#ifndef NET_SCRIPT_DEFAULT
#define NET_SCRIPT_DEFAULT   "/usr/lib/kwort/network-setup.sh"
#endif
#ifndef DISPLAY_SCRIPT_DEFAULT
#define DISPLAY_SCRIPT_DEFAULT "/usr/lib/kwort/display-change.sh"
#endif
#ifndef COMPOSITOR_SCRIPT_DEFAULT
#define COMPOSITOR_SCRIPT_DEFAULT "/usr/lib/kwort/compositor.sh"
#endif
/* compositor is started with X while this symlink exists */
#ifndef COMPOSITOR_LINK
#define COMPOSITOR_LINK      "/etc/X11/xinit/xinitrc.d/0-compositor.sh"
#endif
#ifndef POWER_SCRIPT_DEFAULT
#define POWER_SCRIPT_DEFAULT "/usr/lib/kwort/power-profile.sh"
#endif

/* ---- services provided by tray.c -------------------------------------- */
void app_wake(void);                 /* thread safe: wake the main loop      */
void app_popup_close(void);
void app_popup_redraw(void);
/* fork+exec (PATH lookup); unused trailing args may be NULL. */
void app_run(const char *path, const char *a1, const char *a2, const char *a3);
void app_run_ex(const char *path, const char *const *args,
                const char *env_name, const char *env_val);
/* Modal password dialog (borderless; click outside or Esc cancels).  cb gets
 * the password, or NULL when cancelled.  Opens where the popup was. */
void app_ask_password(const char *title, const char *prompt,
                      void (*cb)(const char *pw, void *ud), void *ud);

/* ---- UI toolkit (tray.c) ---------------------------------------------- */
#define UI_PAD       20
#define UI_HEADER_H  64
#define UI_ROW_H     36
#define UI_TOGGLE_W  44
#define UI_TOGGLE_H  22

enum { UI_LEFT = 0, UI_RIGHT = 1, UI_CENTER = 2, UI_BOLD = 4, UI_CAPS = 8 };

#define IN_RECT(px, py, x, y, w, h) \
    ((px) >= (x) && (px) < (x) + (w) && (py) >= (y) && (py) < (y) + (h))

static inline void ui_color(cairo_t *cr, Rgba c)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

/* x is the left / right / centre edge according to flags, yc the vertical
 * centre.  maxw > 0 ellipsizes.  Returns the drawn width. */
double ui_text(cairo_t *cr, const char *s, double x, double yc, double px,
               int flags, Rgba c, double maxw);
double ui_text_width(const char *s, double px, int flags);
void   ui_fill(cairo_t *cr, double x, double y, double w, double h, Rgba c);
void   ui_frame(cairo_t *cr, double x, double y, double w, double h, Rgba c);
void   ui_sep(cairo_t *cr, double y, double w);
void   ui_header(cairo_t *cr, int w, GlyphFn g, const char *title,
                 const char *sub);
void   ui_toggle(cairo_t *cr, double x, double y, int on);
void   ui_slider(cairo_t *cr, double x, double yc, double w, double frac,
                 int enabled);
double ui_slider_value(int px, int x, int w);          /* 0..1 */
void   ui_button(cairo_t *cr, double x, double y, double w, double h,
                 const char *label, GlyphFn g, int selected);
void   ui_section(cairo_t *cr, double x, double yc, const char *text);

#endif
