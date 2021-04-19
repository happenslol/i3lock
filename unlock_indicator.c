/*
 * vim:ts=4:sw=4:expandtab
 *
 * © 2010 Michael Stapelberg
 *
 * See LICENSE for licensing information
 *
 */
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <xcb/xcb.h>
#include <ev.h>
#include <cairo.h>
#include <cairo/cairo-xcb.h>

#include "i3lock.h"
#include "xcb.h"
#include "unlock_indicator.h"
#include "randr.h"
#include "dpi.h"

#define BUTTON_RADIUS 90
#define BUTTON_SPACE (BUTTON_RADIUS + 5)
#define BUTTON_CENTER (BUTTON_RADIUS + 5)
#define BUTTON_DIAMETER (2 * BUTTON_SPACE)

/*******************************************************************************
 * Variables defined in i3lock.c.
 ******************************************************************************/

extern bool debug_mode;

/* The current position in the input buffer. Useful to determine if any
 * characters of the password have already been entered or not. */
extern int input_position;
static int last_input_position = 0;

/* The lock window. */
extern xcb_window_t win;

/* The current resolution of the X11 root window. */
extern uint32_t last_resolution[2];

/* Whether the unlock indicator is enabled (defaults to true). */
extern bool unlock_indicator;

/* List of pressed modifiers, or NULL if none are pressed. */
extern char *modifier_string;

/* A Cairo surface containing the specified image (-i), if any. */
extern cairo_surface_t *img;

/* Whether the image should be tiled. */
extern bool tile;
/* The background color to use (in hex). */
extern char color[7];

extern int show_on_screen;

/* Whether the failed attempts should be displayed. */
extern bool show_failed_attempts;
/* Number of failed unlock attempts. */
extern int failed_attempts;

/*******************************************************************************
 * Variables defined in xcb.c.
 ******************************************************************************/

/* The root screen, to determine the DPI. */
extern xcb_screen_t *screen;

/*******************************************************************************
 * Local variables.
 ******************************************************************************/

/* Cache the screen’s visual, necessary for creating a Cairo context. */
static xcb_visualtype_t *vistype;

/* Maintain the current unlock/PAM state to draw the appropriate unlock
 * indicator. */
unlock_state_t unlock_state;
auth_state_t auth_state;

static void string_repeat(char *dest, const char *str, int n) {
    if (n <= 0) return;
    if (n >= 64) n = 64;

    char *pa, *pb;
    int slen = strlen(str);

    pa = dest + (n-1)*slen;
    strcpy(pa, str);
    pb = --pa + slen; 
    while (pa>=dest) *pa-- = *pb--;
}

/*
 * Draws global image with fill color onto a pixmap with the given
 * resolution and returns it.
 *
 */
void draw_image(xcb_pixmap_t bg_pixmap, uint32_t *resolution) {
    const double scaling_factor = get_dpi_value() / 96.0;

    if (!vistype)
        vistype = get_root_visual_type(screen);

    /* Initialize cairo: Create one in-memory surface to render the unlock
     * indicator on, create one XCB surface to actually draw (one or more,
     * depending on the amount of screens) unlock indicators on. */
    cairo_surface_t *output = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, resolution[0], resolution[1]);
    cairo_t *ctx = cairo_create(output);

    cairo_surface_t *xcb_output = cairo_xcb_surface_create(conn, bg_pixmap, vistype, resolution[0], resolution[1]);
    cairo_t *xcb_ctx = cairo_create(xcb_output);

    /* After the first iteration, the pixmap will still contain the previous
     * contents. Explicitly clear the entire pixmap with the background color
     * first to get back into a defined state: */
    char strgroups[3][3] = {{color[0], color[1], '\0'},
                            {color[2], color[3], '\0'},
                            {color[4], color[5], '\0'}};
    uint32_t rgb16[3] = {(strtol(strgroups[0], NULL, 16)),
                         (strtol(strgroups[1], NULL, 16)),
                         (strtol(strgroups[2], NULL, 16))};
    cairo_set_source_rgb(xcb_ctx, rgb16[0] / 255.0, rgb16[1] / 255.0, rgb16[2] / 255.0);
    cairo_rectangle(xcb_ctx, 0, 0, resolution[0], resolution[1]);
    cairo_fill(xcb_ctx);

    if (img) {
        if (!tile) {
            cairo_set_source_surface(xcb_ctx, img, 0, 0);
            cairo_paint(xcb_ctx);
        } else {
            /* create a pattern and fill a rectangle as big as the screen */
            cairo_pattern_t *pattern;
            pattern = cairo_pattern_create_for_surface(img);
            cairo_set_source(xcb_ctx, pattern);
            cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
            cairo_rectangle(xcb_ctx, 0, 0, resolution[0], resolution[1]);
            cairo_fill(xcb_ctx);
            cairo_pattern_destroy(pattern);
        }
    }

    if (unlock_indicator &&
        (unlock_state >= STATE_KEY_PRESSED || auth_state > STATE_AUTH_IDLE)) {
        if (input_position > 0)
            last_input_position = input_position;

        cairo_scale(ctx, scaling_factor, scaling_factor);

        /* Display a (centered) text of the current PAM state. */
        char *text = malloc(256);
        strcpy(text, "");

        if (auth_state == STATE_AUTH_WRONG || auth_state == STATE_I3LOCK_LOCK_FAILED)
            string_repeat(text, "•", last_input_position);
        else
            string_repeat(text, "•", input_position);

        cairo_set_source_rgb(ctx, 0, 0, 0);
        cairo_select_font_face(ctx, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(ctx, 80.0);

        switch (auth_state) {
            case STATE_AUTH_VERIFY:
                cairo_set_source_rgb(ctx, 84.0f / 255, 110.0f / 255, 122.0f / 255);
                break;
            case STATE_AUTH_LOCK:
                cairo_set_source_rgb(ctx, 84.0f / 255, 110.0f / 255, 122.0f / 255);
                break;
            case STATE_AUTH_WRONG:
                if (unlock_state < STATE_KEY_PRESSED)
                    cairo_set_source_rgb(ctx, 255.0f / 255, 83.0f / 255, 112.0f / 255);
                else
                    cairo_set_source_rgb(ctx, 1, 1, 1);
                break;
            case STATE_I3LOCK_LOCK_FAILED:
                cairo_set_source_rgb(ctx, 255.0f / 255, 83.0f / 255, 112.0f / 255);
                break;
            default:
                if (unlock_state == STATE_NOTHING_TO_DELETE) strcpy(text, "");
                cairo_set_source_rgb(ctx, 1, 1, 1);
                break;
        }

        cairo_text_extents_t extents;
        double x, y;

        int screen_center_x, screen_center_y, screen_offset_x, screen_offset_y;

        if (xr_screens > 0) {
            int selected_screen = 0;
            // Check if a specific screen was requested
            if (show_on_screen >= 0 && show_on_screen < xr_screens)
                selected_screen = show_on_screen;
            else if (show_on_screen >= 0 && show_on_screen >= xr_screens)
                DEBUG("screen index was %d out of bounds, found %d screens, drawing on 0\n", show_on_screen, xr_screens);
            else
                DEBUG("no screen index given, drawing on 0\n");

            screen_center_x = xr_resolutions[selected_screen].width / 2;
            screen_center_y = xr_resolutions[selected_screen].height / 2;
            screen_offset_x = xr_resolutions[selected_screen].x;
            screen_offset_y = xr_resolutions[selected_screen].y;
        } else {
            /* We have no information about the screen sizes/positions, so we just
             * place the unlock indicator in the middle of the X root window and
             * hope for the best. */
            screen_center_x = (last_resolution[0] / 2);
            screen_center_y = (last_resolution[1] / 2);
            screen_offset_x = 0;
            screen_offset_y = 0;
        }

        cairo_text_extents(ctx, text, &extents);
        x = screen_offset_x + screen_center_x - ((extents.width / 2) + extents.x_bearing);
        y = screen_offset_y + screen_center_y - ((extents.height / 2) + extents.y_bearing);

        cairo_move_to(ctx, x, y);
        cairo_show_text(ctx, text);
        cairo_close_path(ctx);
    }

    cairo_set_source_surface(xcb_ctx, output, 0, 0);
    cairo_rectangle(xcb_ctx, 0, 0, resolution[0], resolution[1]);
    cairo_fill(xcb_ctx);

    cairo_surface_destroy(xcb_output);
    cairo_surface_destroy(output);
    cairo_destroy(ctx);
    cairo_destroy(xcb_ctx);
}

static xcb_pixmap_t bg_pixmap = XCB_NONE;

/*
 * Releases the current background pixmap so that the next redraw_screen() call
 * will allocate a new one with the updated resolution.
 *
 */
void free_bg_pixmap(void) {
    xcb_free_pixmap(conn, bg_pixmap);
    bg_pixmap = XCB_NONE;
}

/*
 * Calls draw_image on a new pixmap and swaps that with the current pixmap
 *
 */
void redraw_screen(void) {
    DEBUG("redraw_screen(unlock_state = %d, auth_state = %d)\n", unlock_state, auth_state);
    if (bg_pixmap == XCB_NONE) {
        DEBUG("allocating pixmap for %d x %d px\n", last_resolution[0], last_resolution[1]);
        bg_pixmap = create_bg_pixmap(conn, screen, last_resolution, color);
    }

    draw_image(bg_pixmap, last_resolution);
    xcb_change_window_attributes(conn, win, XCB_CW_BACK_PIXMAP, (uint32_t[1]){bg_pixmap});
    /* XXX: Possible optimization: Only update the area in the middle of the
     * screen instead of the whole screen. */
    xcb_clear_area(conn, 0, win, 0, 0, last_resolution[0], last_resolution[1]);
    xcb_flush(conn);
}

/*
 * Hides the unlock indicator completely when there is no content in the
 * password buffer.
 *
 */
void clear_indicator(void) {
    if (input_position == 0) {
        unlock_state = STATE_STARTED;
    } else
        unlock_state = STATE_KEY_PRESSED;
    redraw_screen();
}
