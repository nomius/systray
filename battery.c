/* battery.c - battery status, per-battery lines and power profile selection.
 *
 * Icon: battery glyph with the overall percentage next to it (sum of the
 * energy of all batteries over the sum of their full capacity).
 * Popup: overall %, size, state, one line per battery when there are several
 * and the power profile buttons (they run POWER script with the profile name).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "common.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXBAT 8
#define POP_W  420

typedef struct {
    char name[32];
    char path[320];
    double full_wh, now_wh;
    int pct;
    char status[24];
} Bat;

static Bat bats[MAXBAT];
static int nbat;
static double tot_full, tot_now;
static int overall_pct;
static char overall_state[24] = "Unknown";
static int charge_limit = -1;
static char profile[24] = "balanced";

static int g_pct, g_charging;          /* used by glyph_battery */

/* ------------------------------------------------------------------ */
/* sysfs                                                               */
/* ------------------------------------------------------------------ */

static int read_str(const char *dir, const char *file, char *out, size_t n)
{
    char p[400];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    if (!fgets(out, (int)n, f)) { fclose(f); return -1; }
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 0;
}

static int read_ll(const char *dir, const char *file, long long *v)
{
    char b[64];
    if (read_str(dir, file, b, sizeof b) < 0) return -1;
    *v = atoll(b);
    return 0;
}

static void read_bat(Bat *b)
{
    long long full = 0, now = 0, volt = 0, cap = -1;
    int have_e = !read_ll(b->path, "energy_full", &full) &&
                 !read_ll(b->path, "energy_now", &now) && full > 0;
    if (have_e) {
        b->full_wh = full / 1e6;
        b->now_wh = now / 1e6;
    } else if (!read_ll(b->path, "charge_full", &full) &&
               !read_ll(b->path, "charge_now", &now) && full > 0) {
        if (read_ll(b->path, "voltage_min_design", &volt) < 0 || volt <= 0)
            read_ll(b->path, "voltage_now", &volt);
        b->full_wh = full / 1e6 * volt / 1e6;
        b->now_wh = now / 1e6 * volt / 1e6;
    } else {
        b->full_wh = b->now_wh = 0;
    }
    read_ll(b->path, "capacity", &cap);
    if (b->full_wh > 0) b->pct = (int)(100.0 * b->now_wh / b->full_wh + 0.5);
    else b->pct = cap < 0 ? 0 : (int)cap;
    if (read_str(b->path, "status", b->status, sizeof b->status) < 0)
        snprintf(b->status, sizeof b->status, "Unknown");
}

static void scan_bats(void)
{
    nbat = 0;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nbat < MAXBAT) {
        if (e->d_name[0] == '.') continue;
        char p[320], type[32] = "", scope[32] = "";
        snprintf(p, sizeof p, "/sys/class/power_supply/%.255s", e->d_name);
        read_str(p, "type", type, sizeof type);
        read_str(p, "scope", scope, sizeof scope);
        if (strcmp(type, "Battery") || !strcmp(scope, "Device")) continue;
        snprintf(bats[nbat].name, sizeof bats[nbat].name, "%.31s", e->d_name);
        snprintf(bats[nbat].path, sizeof bats[nbat].path, "%s", p);
        nbat++;
    }
    closedir(d);
}

static void update(void)
{
    double sum_full = 0, sum_now = 0, pct_sum = 0;
    int charging = 0, discharging = 0, full = 0;
    for (int i = 0; i < nbat; i++) {
        read_bat(&bats[i]);
        sum_full += bats[i].full_wh;
        sum_now += bats[i].now_wh;
        pct_sum += bats[i].pct;
        if (!strcmp(bats[i].status, "Charging")) charging++;
        else if (!strcmp(bats[i].status, "Discharging")) discharging++;
        else if (!strcmp(bats[i].status, "Full")) full++;
    }
    tot_full = sum_full;
    tot_now = sum_now;
    if (nbat == 0) overall_pct = 0;
    else if (sum_full > 0) overall_pct = (int)(100.0 * sum_now / sum_full + 0.5);
    else overall_pct = (int)(pct_sum / nbat + 0.5);

    if (charging) snprintf(overall_state, sizeof overall_state, "Charging");
    else if (discharging) snprintf(overall_state, sizeof overall_state, "Discharging");
    else if (full == nbat && nbat) snprintf(overall_state, sizeof overall_state, "Full");
    else snprintf(overall_state, sizeof overall_state, "Holding");

    long long lim;
    charge_limit = -1;
    if (nbat && !read_ll(bats[0].path, "charge_control_end_threshold", &lim))
        charge_limit = (int)lim;
}

/* Current profile from sysfs (same logic as `power-profile.sh get`):
 * the ACPI platform profile when present, otherwise cpu0's cpufreq settings. */
static void read_profile(void)
{
    char v[64];
    const char *cur = NULL;

    if (read_str("/sys/firmware/acpi", "platform_profile", v, sizeof v) < 0)
        v[0] = 0;
    if (!strcmp(v, "low-power") || !strcmp(v, "quiet") || !strcmp(v, "cool"))
        cur = "power-saver";
    else if (!strcmp(v, "balanced")) cur = "balanced";
    else if (!strcmp(v, "performance")) cur = "performance";

    if (!cur) {
        char gov[64] = "", epp[64] = "";
        const char *cpu = "/sys/devices/system/cpu/cpu0/cpufreq";
        if (read_str(cpu, "scaling_governor", gov, sizeof gov) == 0) {
            read_str(cpu, "energy_performance_preference", epp, sizeof epp);
            if (!strcmp(gov, "performance")) cur = "performance";
            else if (!strcmp(gov, "powersave")) {
                if (!*epp || !strcmp(epp, "power") || !strcmp(epp, "balance_power"))
                    cur = "power-saver";
                else if (!strcmp(epp, "performance")) cur = "performance";
                else cur = "balanced";
            } else cur = "balanced";
        }
    }
    if (cur) snprintf(profile, sizeof profile, "%s", cur);
}
/* ------------------------------------------------------------------ */
/* Glyphs                                                              */
/* ------------------------------------------------------------------ */

static void glyph_battery(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s / 20.0, s / 20.0);
    ui_color(cr, c);
    cairo_rectangle(cr, -2.5, -11, 5, 2);                       /* nub */
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.8);
    cairo_rectangle(cr, -5.5, -9, 11, 19);                      /* body */
    cairo_stroke(cr);
    double fill = 15.0 * (g_pct < 0 ? 0 : g_pct > 100 ? 100 : g_pct) / 100.0;
    cairo_rectangle(cr, -3.4, 8.4 - fill, 6.8, fill);
    cairo_fill(cr);
    if (g_charging) {                                           /* bolt */
        ui_color(cr, C_BG);
        cairo_move_to(cr, 1.5, -6); cairo_line_to(cr, -3, 2);
        cairo_line_to(cr, 0, 2);    cairo_line_to(cr, -1.5, 8);
        cairo_line_to(cr, 3, 0);    cairo_line_to(cr, 0, 0);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

static void glyph_header_battery(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    g_pct = overall_pct;
    g_charging = !strcmp(overall_state, "Charging");
    glyph_battery(cr, cx, cy, s, c);
}

static void glyph_leaf(cairo_t *cr, double cx, double cy, double s, Rgba c)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s / 20.0, s / 20.0);
    ui_color(cr, c);
    cairo_set_line_width(cr, 1.6);
    cairo_move_to(cr, -8, 8);
    cairo_curve_to(cr, -9, -4, 0, -9, 9, -8);
    cairo_curve_to(cr, 10, 2, 2, 9, -8, 8);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, -8, 8); cairo_line_to(cr, 3, -3);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void gauge(cairo_t *cr, double cx, double cy, double s, Rgba c, double needle)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s / 20.0, s / 20.0);
    ui_color(cr, c);
    cairo_set_line_width(cr, 1.8);
    cairo_arc(cr, 0, 1, 9, 2.35, 0.79 + 6.2832);                /* ~270deg */
    cairo_stroke(cr);
    cairo_move_to(cr, 0, 1);
    cairo_line_to(cr, 6 * cos(needle), 1 + 6 * sin(needle));
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void glyph_balanced(cairo_t *cr, double cx, double cy, double s, Rgba c)
{ gauge(cr, cx, cy, s, c, -1.57); }
static void glyph_perf(cairo_t *cr, double cx, double cy, double s, Rgba c)
{ gauge(cr, cx, cy, s, c, -0.6); }

/* ------------------------------------------------------------------ */
/* Popup                                                               */
/* ------------------------------------------------------------------ */

static const struct { const char *label, *arg; GlyphFn g; } profs[3] = {
    {"Power-saver", "power-saver", glyph_leaf},
    {"Balanced",    "balanced",    glyph_balanced},
    {"Performance", "performance", glyph_perf},
};

#define BTN_W ((POP_W - 2 * UI_PAD - 2 * 10) / 3)
#define BTN_H 40

static int multi(void) { return nbat > 1; }

static int y_profiles(void)
{
    int y = UI_HEADER_H + 20 + 12 + 24 + 28;          /* bar + stats */
    if (multi()) y += nbat * 26 + 10;
    return y + 20;                                    /* section label row */
}

static void popup_size(int *w, int *h)
{
    *w = POP_W;
    *h = y_profiles() + BTN_H + 22;
}

static const char *script(void)
{
    const char *s = getenv("SYSTRAY_POWER_SCRIPT");
    return s && *s ? s : POWER_SCRIPT_DEFAULT;
}

static void popup_draw(cairo_t *cr, int w, int h)
{
    (void)h;
    char buf[64];
    char sub[48];
    if (charge_limit > 0) snprintf(sub, sizeof sub, "Threshold %d%%", charge_limit);
    else snprintf(sub, sizeof sub, "%s", overall_state);
    ui_header(cr, w, glyph_header_battery, "Battery", sub);

    snprintf(buf, sizeof buf, "%d%%", overall_pct);
    ui_text(cr, buf, w - UI_PAD, UI_HEADER_H / 2.0, 34, UI_RIGHT | UI_BOLD, C_DIM, 0);

    int y = UI_HEADER_H + 20;
    ui_fill(cr, UI_PAD, y, w - 2 * UI_PAD, 12, (Rgba){0.12, 0.13, 0.17, 1});
    ui_fill(cr, UI_PAD, y, (w - 2 * UI_PAD) * overall_pct / 100.0, 12, C_FILL);

    y += 12 + 24;
    int col = (w - 2 * UI_PAD - 20) / 2;
    snprintf(buf, sizeof buf, "%.0fWh", tot_full);
    ui_text(cr, "Battery size", UI_PAD, y, 12, 0, C_DIM, 0);
    ui_text(cr, buf, UI_PAD + col, y, 12, UI_RIGHT, C_FG, 0);
    ui_text(cr, "Battery state", UI_PAD + col + 20, y, 12, 0, C_DIM, 0);
    ui_text(cr, overall_state, w - UI_PAD, y, 12, UI_RIGHT, C_FG, 0);

    y += 28;
    if (multi()) {
        for (int i = 0; i < nbat; i++) {
            int yy = y + i * 26 + 8;
            ui_text(cr, bats[i].name, UI_PAD, yy, 11, UI_CAPS, C_DIM, 0);
            snprintf(buf, sizeof buf, "%.0fWh", bats[i].full_wh);
            ui_text(cr, buf, UI_PAD + 100, yy, 12, 0, C_FG, 0);
            ui_text(cr, !strcmp(bats[i].status, "Not charging") ? "Holding" : bats[i].status,
                    UI_PAD + col + 20, yy, 12, 0, C_FG, 0);
            snprintf(buf, sizeof buf, "%d%%", bats[i].pct);
            ui_text(cr, buf, w - UI_PAD, yy, 12, UI_RIGHT, C_FG, 0);
        }
        y += nbat * 26 + 10;
    }

    ui_sep(cr, y - 8, w);
    ui_section(cr, UI_PAD, y_profiles() - 14, "Power profile");
    for (int i = 0; i < 3; i++)
        ui_button(cr, UI_PAD + i * (BTN_W + 10), y_profiles(), BTN_W, BTN_H,
                  profs[i].label, profs[i].g, !strcmp(profile, profs[i].arg));
}

static void popup_button(int x, int y, int button, int pressed)
{
    if (!pressed || button != 1) return;
    if (y < y_profiles() || y >= y_profiles() + BTN_H) return;
    for (int i = 0; i < 3; i++) {
        if (x >= UI_PAD + i * (BTN_W + 10) && x < UI_PAD + i * (BTN_W + 10) + BTN_W) {
            app_run(script(), profs[i].arg, NULL, NULL);
            snprintf(profile, sizeof profile, "%s", profs[i].arg);
        }
    }
}

static void popup_open(void)
{
    update();
    read_profile();
}

/* ------------------------------------------------------------------ */
/* Icon / module                                                       */
/* ------------------------------------------------------------------ */

static void pct_text(char *b, size_t n) { snprintf(b, n, "%d%%", overall_pct); }

static int icon_width(int h)
{
    return h + (int)ui_text_width("100%", 11, 0) + 2;
}

static void draw_icon(cairo_t *cr, int w, int h)
{
    (void)w;
    char b[16];
    g_pct = overall_pct;
    g_charging = !strcmp(overall_state, "Charging");
    glyph_battery(cr, h / 2.0, h / 2.0, h * 0.72, C_ICON);
    pct_text(b, sizeof b);
    ui_text(cr, b, h - 1, h / 2.0, 11, 0, C_ICON, 0);
}

static int tooltip(char *buf, size_t n)
{
    snprintf(buf, n, "Battery: %d%%", overall_pct);
    return 1;
}

static int tick(void)
{
    static int n;
    int old_pct = overall_pct;
    char old_state[sizeof overall_state];
    if (n++ % 5) return 0;
    memcpy(old_state, overall_state, sizeof old_state);
    update();
    return old_pct != overall_pct || strcmp(old_state, overall_state);
}

static int init(void)
{
    scan_bats();
    if (!nbat) return -1;                 /* desktop: no battery icon */
    update();
    read_profile();
    return 0;
}

const Module battery_module = {
    .name = "battery", .init = init, .tick = tick, .icon_width = icon_width,
    .draw_icon = draw_icon, .tooltip = tooltip,
    .popup_open = popup_open, .popup_size = popup_size,
    .popup_draw = popup_draw, .popup_button = popup_button,
};
