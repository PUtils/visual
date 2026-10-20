#include "vis.h"
#include <SDL2/SDL_ttf.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>

static TTF_Font *g_font = NULL;
static TTF_Font *g_ui_font = NULL;
static TTF_Font *g_icon_font = NULL;

typedef struct { SDL_Texture *tex; int w, h; } glyph_t;
static glyph_t g_mono_glyphs[128];
static glyph_t g_ui_glyphs[128];

static void build_glyph_cache(SDL_Renderer *r, TTF_Font *font, glyph_t out[128]) {
    SDL_Color white = {255, 255, 255, 255};
    for (int c = 0; c < 128; c++) { out[c].tex = NULL; out[c].w = 0; out[c].h = 0; }
    for (int c = 32; c < 127; c++) {
        char s[2] = {(char)c, '\0'};
        SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s, white);
        if (!surf) continue;
        SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
        if (tex) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            out[c].tex = tex;
            out[c].w = surf->w;
            out[c].h = surf->h;
        }
        SDL_FreeSurface(surf);
    }
}

static void destroy_glyph_cache(glyph_t out[128]) {
    for (int c = 0; c < 128; c++) {
        if (out[c].tex) { SDL_DestroyTexture(out[c].tex); out[c].tex = NULL; }
    }
}

static inline void draw_glyph(SDL_Renderer *r, const glyph_t *gset, int ch,
                              int x, int y, SDL_Color col) {
    if (ch < 32 || ch >= 127) return;
    const glyph_t *g = &gset[ch];
    if (!g->tex) return;
    SDL_SetTextureColorMod(g->tex, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(g->tex, col.a);
    SDL_Rect dst = {x, y, g->w, g->h};
    SDL_RenderCopy(r, g->tex, NULL, &dst);
}

static void draw_text_mono_line(SDL_Renderer *r, const char *text, int x, int y,
                                int cell_w, SDL_Color col) {
    for (int i = 0; text[i]; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch >= 32 && ch < 127) draw_glyph(r, g_mono_glyphs, ch, x + i * cell_w, y, col);
    }
}

#define DESK_BG_R        32
#define DESK_BG_G        48
#define DESK_BG_B        78
#define DESK_ACCENT_R    58
#define DESK_ACCENT_G    82
#define DESK_ACCENT_B    124

#define TITLEBAR_H       26
#define TASKBAR_H        30
#define BORDER           2
#define ICON_W           72
#define ICON_H           80
#define DBLCLICK_MS      400
#define DOT_SIZE         10
#define DOT_SPACING      16
#define DOT_X0           10
#define DOT_Y0           6
#define RESIZE_GRIP      16

static int sdl_init_fonts(vis_state_t *state) {
    const char *font_path = "/System/Library/Fonts/Menlo.ttc";
    if (access(font_path, R_OK) != 0)
        font_path = "/opt/homebrew/share/fonts/MesloLGS-NF-Regular.ttf";
    if (access(font_path, R_OK) != 0) {
        vis_log(VIS_LOG_ERROR, "Font not found");
        return -1;
    }
    int font_size = state->config.terminal.font_size > 0 ? state->config.terminal.font_size : 14;
    g_font = TTF_OpenFont(font_path, font_size);
    if (!g_font) { vis_log(VIS_LOG_ERROR, "TTF_OpenFont failed: %s", TTF_GetError()); return -1; }
    g_ui_font = TTF_OpenFont(font_path, 12);
    g_icon_font = TTF_OpenFont(font_path, 11);
    int w, h;
    TTF_SizeText(g_font, "M", &w, &h);
    state->font_width = w > 0 ? w : 8;
    state->font_height = h > 0 ? h : 16;
    return 0;
}

static void sdl_cleanup_fonts(void) {
    if (g_font) { TTF_CloseFont(g_font); g_font = NULL; }
    if (g_ui_font) { TTF_CloseFont(g_ui_font); g_ui_font = NULL; }
    if (g_icon_font) { TTF_CloseFont(g_icon_font); g_icon_font = NULL; }
}

int vis_sdl_init(vis_state_t *state) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        vis_log(VIS_LOG_ERROR, "SDL_Init failed: %s", SDL_GetError());
        return -1;
    }
    if (TTF_Init() != 0) {
        vis_log(VIS_LOG_ERROR, "TTF_Init failed: %s", TTF_GetError());
        SDL_Quit();
        return -1;
    }
    state->window_width = 1280;
    state->window_height = 840;
    state->window = SDL_CreateWindow(
        "vis — remote desktop",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        state->window_width, state->window_height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);
    if (!state->window) { TTF_Quit(); SDL_Quit(); return -1; }
    SDL_ShowWindow(state->window);
    SDL_RaiseWindow(state->window);
    state->renderer = SDL_CreateRenderer(state->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!state->renderer) { SDL_DestroyWindow(state->window); TTF_Quit(); SDL_Quit(); return -1; }
    if (sdl_init_fonts(state) != 0) { vis_sdl_cleanup(state); return -1; }

    state->cols = state->config.terminal.cols;
    state->rows = state->config.terminal.rows;
    state->cell_width = state->font_width;
    state->cell_height = state->font_height;

    build_glyph_cache(state->renderer, g_font, g_mono_glyphs);
    if (g_ui_font) build_glyph_cache(state->renderer, g_ui_font, g_ui_glyphs);

    vis_desktop_init(state);
    vis_update_layout(state);
    SDL_StartTextInput();
    vis_log(VIS_LOG_INFO, "SDL initialized: %dx%d", state->window_width, state->window_height);
    return 0;
}

void vis_sdl_cleanup(vis_state_t *state) {
    destroy_glyph_cache(g_mono_glyphs);
    destroy_glyph_cache(g_ui_glyphs);
    sdl_cleanup_fonts();
    while (state->windows) {
        vis_window_t *w = state->windows;
        state->windows = w->next;
        vis_window_destroy(w);
    }
    free(state->icons);
    if (state->renderer) SDL_DestroyRenderer(state->renderer);
    if (state->window) SDL_DestroyWindow(state->window);
    TTF_Quit();
    SDL_Quit();
}

void vis_update_layout(vis_state_t *state) {
    int w, h;
    SDL_GetWindowSize(state->window, &w, &h);
    state->window_width = w;
    state->window_height = h;

    for (vis_window_t *win = state->windows; win; win = win->next) {
        if (win->maximized) {
            win->x = 0; win->y = 0;
            win->w = w; win->h = h - state->desktop.taskbar_height;
        }
        if (win->x + win->w > w) win->x = w - win->w;
        if (win->x < 0) win->x = 0;
        int max_y = h - state->desktop.taskbar_height - win->h;
        if (max_y < 0) max_y = 0;
        if (win->y > max_y) win->y = max_y;
        if (win->y < 0) win->y = 0;
        if (win->kind == VIS_WIN_TERMINAL && win->pane) {
            win->pane->x = win->x + BORDER;
            win->pane->y = win->y + TITLEBAR_H;
        }
    }
    vis_desktop_layout_icons(state);
}

static void fill_rect(SDL_Renderer *r, SDL_Rect rect, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r, &rect);
}

static void draw_text(SDL_Renderer *r, TTF_Font *font, const char *text, int x, int y, SDL_Color c) {
    if (!text || !*text || !font) return;
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, c);
    if (!s) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
    if (t) {
        SDL_Rect dst = {x, y, s->w, s->h};
        SDL_RenderCopy(r, t, NULL, &dst);
        SDL_DestroyTexture(t);
    }
    SDL_FreeSurface(s);
}

static void draw_text_clipped(SDL_Renderer *r, TTF_Font *font, const char *text,
                              int x, int y, int max_w, SDL_Color c) {
    if (!text || !*text || !font) return;
    char buf[512];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    int w = 0, h = 0;
    TTF_SizeUTF8(font, buf, &w, &h);
    if (w > max_w) {
        size_t len = strlen(buf);
        while (len > 3 && w > max_w) {
            buf[--len] = '\0';
            TTF_SizeUTF8(font, buf, &w, &h);
        }
        if (len > 3) buf[len - 1] = buf[len - 2] = buf[len - 3] = '.';
    }
    draw_text(r, font, buf, x, y, c);
}

static void draw_text_centered(SDL_Renderer *r, TTF_Font *font, const char *text,
                               int x, int y, int w, int h, SDL_Color c) {
    if (!text || !*text || !font) return;
    int tw = 0, th = 0;
    TTF_SizeUTF8(font, text, &tw, &th);
    if (tw > w - 8) {
        draw_text_clipped(r, font, text, x + 4, y + (h - th) / 2, w - 8, c);
        return;
    }
    draw_text(r, font, text, x + (w - tw) / 2, y + (h - th) / 2, c);
}

static void draw_wallpaper(vis_state_t *state) {
    for (int y = 0; y < state->window_height; y++) {
        float t = (float)y / (float)state->window_height;
        Uint8 r = (Uint8)(DESK_BG_R + (DESK_ACCENT_R - DESK_BG_R) * t);
        Uint8 g = (Uint8)(DESK_BG_G + (DESK_ACCENT_G - DESK_BG_G) * t);
        Uint8 b = (Uint8)(DESK_BG_B + (DESK_ACCENT_B - DESK_BG_B) * t);
        SDL_SetRenderDrawColor(state->renderer, r, g, b, 255);
        SDL_RenderDrawLine(state->renderer, 0, y, state->window_width, y);
    }
}

static void draw_icon_glyph(SDL_Renderer *r, int x, int y, bool is_dir, bool selected) {
    SDL_Rect card = {x + 12, y, 48, 48};
    if (selected) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        fill_rect(r, (SDL_Rect){x - 2, y - 4, ICON_W + 4, ICON_H + 4}, (SDL_Color){255, 255, 255, 60});
    }
    if (is_dir) {
        fill_rect(r, (SDL_Rect){card.x, card.y + 10, card.w, card.h - 10}, (SDL_Color){240, 200, 90, 255});
        fill_rect(r, (SDL_Rect){card.x, card.y, 20, 10}, (SDL_Color){240, 200, 90, 255});
        SDL_SetRenderDrawColor(r, 180, 140, 50, 255);
        SDL_RenderDrawRect(r, &card);
    } else {
        fill_rect(r, card, (SDL_Color){240, 240, 245, 255});
        fill_rect(r, (SDL_Rect){card.x + card.w - 14, card.y, 14, 14}, (SDL_Color){200, 200, 210, 255});
        SDL_SetRenderDrawColor(r, 80, 80, 90, 255);
        SDL_RenderDrawRect(r, &card);
        SDL_SetRenderDrawColor(r, 160, 160, 170, 255);
        for (int i = 0; i < 3; i++) {
            SDL_RenderDrawLine(r, card.x + 6, card.y + 24 + i * 6,
                               card.x + card.w - 6, card.y + 24 + i * 6);
        }
    }
}

static void draw_desktop_icons(vis_state_t *state) {
    for (int i = 0; i < state->icon_count; i++) {
        vis_desktop_icon_t *ic = &state->icons[i];
        draw_icon_glyph(state->renderer, ic->grid_x, ic->grid_y, ic->is_dir,
                        i == state->icon_focus);
        SDL_Color label = {240, 242, 250, 255};
        if (g_icon_font) {
            int tw = 0, th = 0;
            char buf[64];
            strncpy(buf, ic->name, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            TTF_SizeUTF8(g_icon_font, buf, &tw, &th);
            if (tw > ICON_W) {
                size_t len = strlen(buf);
                while (len > 3 && tw > ICON_W) {
                    buf[--len] = '\0';
                    TTF_SizeUTF8(g_icon_font, buf, &tw, &th);
                }
            }
            int lx = ic->grid_x + (ICON_W - tw) / 2;
            draw_text(state->renderer, g_icon_font, buf, lx, ic->grid_y + 52, label);
        }
    }
}

static SDL_Color lighten(SDL_Color c, int amt) {
    int r = c.r + amt; if (r > 255) r = 255;
    int g = c.g + amt; if (g > 255) g = 255;
    int b = c.b + amt; if (b > 255) b = 255;
    return (SDL_Color){(Uint8)r, (Uint8)g, (Uint8)b, c.a};
}

static void draw_dot(SDL_Renderer *r, int cx, int cy, SDL_Color c, bool hovered,
                     const char *glyph) {
    SDL_Color draw = hovered ? lighten(c, 40) : c;
    SDL_SetRenderDrawColor(r, draw.r, draw.g, draw.b, 255);
    SDL_Rect d = {cx, cy, DOT_SIZE, DOT_SIZE};
    SDL_RenderFillRect(r, &d);
    if (hovered && glyph && g_icon_font) {
        SDL_Color text = {0, 0, 0, 255};
        int tw = 0, th = 0;
        TTF_SizeUTF8(g_icon_font, glyph, &tw, &th);
        draw_text(r, g_icon_font, glyph,
                  cx + (DOT_SIZE - tw) / 2, cy + (DOT_SIZE - th) / 2 - 1, text);
    }
}

static void draw_window_frame(vis_state_t *state, vis_window_t *win, bool focused) {
    SDL_SetRenderDrawBlendMode(state->renderer, SDL_BLENDMODE_BLEND);
    fill_rect(state->renderer, (SDL_Rect){win->x + 4, win->y + 6, win->w, win->h},
              (SDL_Color){0, 0, 0, 90});
    fill_rect(state->renderer, (SDL_Rect){win->x, win->y, win->w, win->h},
              (SDL_Color){26, 28, 34, 255});
    SDL_Color tb = focused ? (SDL_Color){62, 94, 150, 255} : (SDL_Color){60, 60, 72, 255};
    fill_rect(state->renderer, (SDL_Rect){win->x, win->y, win->w, TITLEBAR_H}, tb);

    bool hover_group = (state->desktop.hover_win == win && state->desktop.hover_titlebar_btn >= 0);
    SDL_Color dot_colors[3] = {{235, 92, 92, 255}, {240, 190, 70, 255}, {80, 200, 110, 255}};
    const char *dot_glyph[3] = {"×", "–", win->maximized ? "❐" : "+"};
    for (int i = 0; i < 3; i++) {
        int dx = win->x + DOT_X0 + i * DOT_SPACING;
        int dy = win->y + DOT_Y0;
        bool hovered = hover_group;
        draw_dot(state->renderer, dx, dy, dot_colors[i], hovered, dot_glyph[i]);
    }

    SDL_Color white = {230, 230, 235, 255};
    draw_text_clipped(state->renderer, g_ui_font ? g_ui_font : g_font,
                      win->title, win->x + 64, win->y + 6, win->w - 80, white);
}

static void draw_resize_grip(vis_state_t *state, vis_window_t *win) {
    if (win->maximized) return;
    bool hovered = state->desktop.hover_resize && state->desktop.hover_win == win;
    SDL_Color c = hovered ? (SDL_Color){200, 220, 255, 255} : (SDL_Color){120, 140, 170, 255};
    SDL_SetRenderDrawColor(state->renderer, c.r, c.g, c.b, 255);
    int gx = win->x + win->w - RESIZE_GRIP;
    int gy = win->y + win->h - RESIZE_GRIP;
    for (int i = 0; i < 3; i++) {
        SDL_RenderDrawLine(state->renderer,
            gx + 4 + i * 4, gy + RESIZE_GRIP - 2,
            gx + RESIZE_GRIP - 2, gy + 4 + i * 4);
    }
}

static void render_terminal_body(vis_state_t *state, vis_window_t *win) {
    if (!win->pane || !win->pane->vterm || !win->pane->screen) return;
    int ox = win->x + BORDER;
    int oy = win->y + TITLEBAR_H;
    int body_w = win->w - 2 * BORDER;
    int body_h = win->h - TITLEBAR_H - BORDER;
    fill_rect(state->renderer, (SDL_Rect){ox, oy, body_w, body_h}, (SDL_Color){16, 16, 20, 255});

    int cell_w = state->cell_width;
    int cell_h = state->cell_height;
    int cols = body_w / cell_w;
    int rows = body_h / cell_h;
    if (cols > state->cols) cols = state->cols;
    if (rows > state->rows) rows = state->rows;

    SDL_Color fg_default = {220, 220, 220, 255};
    SDL_Color bg_default = {16, 16, 20, 255};
    SDL_Color scrollback_fg = {170, 180, 200, 255};

    vis_pane_t *pane = win->pane;
    int sb_count = vis_scrollback_count(&pane->scrollback);
    int offset = pane->scroll_offset;
    if (offset < 0) offset = 0;
    if (offset > sb_count) offset = sb_count;

    VTermScreen *screen = pane->screen;
    VTermPos pos;
    VTermScreenCell cell;
    for (int disp_row = 0; disp_row < rows; disp_row++) {
        int src = sb_count - offset + disp_row;
        int py = oy + disp_row * cell_h;
        if (src < 0) continue;
        if (src < sb_count) {
            /* Historical line from scrollback — monochrome text. */
            char line[1024];
            vis_scrollback_get(&pane->scrollback, src, line, sizeof(line));
            draw_text_mono_line(state->renderer, line, ox, py, cell_w, scrollback_fg);
            continue;
        }
        int screen_row = src - sb_count;
        if (screen_row < 0 || screen_row >= state->rows) continue;

        for (int col = 0; col < cols; col++) {
            pos.row = screen_row; pos.col = col;
            if (!vterm_screen_get_cell(screen, pos, &cell)) continue;
            uint32_t ch0 = cell.chars[0];
            int visible = ch0 >= 32 && ch0 < 127;
            SDL_Color fg = fg_default;
            SDL_Color bgc = bg_default;
            if (cell.attrs.reverse) { SDL_Color t = fg; fg = bgc; bgc = t; }
            int px = ox + col * cell_w;
            if (bgc.r != bg_default.r || bgc.g != bg_default.g || bgc.b != bg_default.b) {
                fill_rect(state->renderer, (SDL_Rect){px, py, cell_w, cell_h}, bgc);
            }
            if (visible) draw_glyph(state->renderer, g_mono_glyphs, (int)ch0, px, py, fg);
        }
    }

    /* Cursor — only when live view. */
    if (offset == 0) {
        VTermState *vs = vterm_obtain_state(pane->vterm);
        if (vs) {
            VTermPos cp;
            vterm_state_get_cursorpos(vs, &cp);
            if (cp.row >= 0 && cp.row < rows && cp.col >= 0 && cp.col < cols) {
                int cx = ox + cp.col * cell_w;
                int cy = oy + cp.row * cell_h;
                SDL_SetRenderDrawBlendMode(state->renderer, SDL_BLENDMODE_BLEND);
                Uint8 alpha = (win == state->focused) ? 200 : 90;
                SDL_SetRenderDrawColor(state->renderer, 255, 255, 255, alpha);
                SDL_Rect cur = {cx, cy, cell_w, cell_h};
                SDL_RenderFillRect(state->renderer, &cur);
            }
        }
    } else {
        /* Show "scrollback" indicator in top-right corner. */
        char tag[32];
        snprintf(tag, sizeof(tag), "scrollback -%d", offset);
        SDL_Color hint = {255, 220, 120, 240};
        int tw = 0, th = 0;
        if (g_ui_font) TTF_SizeUTF8(g_ui_font, tag, &tw, &th);
        draw_text(state->renderer, g_ui_font ? g_ui_font : g_font, tag,
                  ox + body_w - tw - 12, oy + 4, hint);
    }

    if (pane->channel_closed) {
        SDL_Color hint = {240, 120, 120, 240};
        draw_text(state->renderer, g_ui_font ? g_ui_font : g_font,
                  "[shell exited — close this window or open a new terminal]",
                  ox + 10, oy + body_h - 20, hint);
    }
}

static void draw_button(SDL_Renderer *r, int x, int y, int w, int h,
                        const char *label, SDL_Color face, SDL_Color text,
                        bool hovered) {
    SDL_Color f = hovered ? lighten(face, 30) : face;
    fill_rect(r, (SDL_Rect){x, y, w, h}, f);
    SDL_SetRenderDrawColor(r, 20, 24, 32, 180);
    SDL_RenderDrawRect(r, &(SDL_Rect){x, y, w, h});
    draw_text_centered(r, g_ui_font ? g_ui_font : g_font, label, x, y, w, h, text);
}

static void render_files_body(vis_state_t *state, vis_window_t *win) {
    int bx = win->x + BORDER;
    int by = win->y + TITLEBAR_H;
    int bw = win->w - 2 * BORDER;
    int bh = win->h - TITLEBAR_H - BORDER;

    fill_rect(state->renderer, (SDL_Rect){bx, by, bw, bh}, (SDL_Color){28, 30, 36, 255});
    fill_rect(state->renderer, (SDL_Rect){bx, by, bw, 28}, (SDL_Color){42, 46, 56, 255});

    SDL_Color btn_face = {60, 72, 96, 255};
    SDL_Color btn_text = {230, 232, 240, 255};
    int n = vis_files_btn_total(state);
    int window_right = win->x + win->w - 6;
    int rightmost = win->x + 10;
    for (int i = 0; i < n; i++) {
        int gx, gy, gw, gh;
        vis_files_btn_geom(state, win, i, &gx, &gy, &gw, &gh);
        if (gx + gw > window_right) break;
        bool hovered = (state->desktop.hover_win == win && state->desktop.hover_toolbar_btn == i);
        draw_button(state->renderer, gx, gy, gw, gh,
                    vis_files_btn_label(i), btn_face, btn_text, hovered);
        rightmost = gx + gw;
    }

    int path_x = rightmost + 10;
    int path_w = window_right - path_x;
    if (path_w > 20) {
        draw_text_clipped(state->renderer, g_ui_font ? g_ui_font : g_font,
                          win->path, path_x, win->y + TITLEBAR_H + 7, path_w,
                          (SDL_Color){200, 205, 220, 255});
    }

    int list_y = by + 30;
    if (win->prompting) {
        const char *lbl =
            win->prompt_action == 1 ? "New file:" :
            win->prompt_action == 2 ? "New folder:" :
            win->prompt_action == 3 ? "Rename to:" : "";
        fill_rect(state->renderer, (SDL_Rect){bx, list_y, bw, 26}, (SDL_Color){40, 56, 92, 255});
        TTF_Font *uf = g_ui_font ? g_ui_font : g_font;
        draw_text(state->renderer, uf, lbl, bx + 8, list_y + 6, (SDL_Color){210, 220, 240, 255});
        int lw = 0, lh = 0;
        TTF_SizeUTF8(uf, lbl, &lw, &lh);
        int tx = bx + 16 + lw;
        char with_cursor[VIS_MAX_PATH_LEN + 2];
        snprintf(with_cursor, sizeof(with_cursor), "%s|", win->prompt_buffer);
        draw_text(state->renderer, uf, with_cursor, tx, list_y + 6, (SDL_Color){255, 255, 255, 255});
        draw_text(state->renderer, uf, "Enter = ok · Esc = cancel",
                  bx + bw - 190, list_y + 6, (SDL_Color){180, 190, 210, 220});
        list_y += 28;
    }

    int list_h = (by + bh) - list_y;
    int row_h = state->font_height + 4;
    int rows_shown = list_h / row_h;

    SDL_Color fg = {210, 215, 225, 255};
    SDL_Color dir_fg = {140, 200, 255, 255};
    SDL_Color sel_bg = {80, 120, 190, 255};
    SDL_Color sel_fg = {255, 255, 255, 255};

    for (int i = 0; i < rows_shown && win->entry_scroll + i < win->entry_count; i++) {
        int idx = win->entry_scroll + i;
        vis_file_entry_t *e = &win->entries[idx];
        int ry = list_y + i * row_h;
        bool sel = idx == win->entry_selected;
        if (sel) fill_rect(state->renderer, (SDL_Rect){bx, ry, bw, row_h}, sel_bg);
        char line[320];
        const char *tag = (strcmp(e->name, "..") == 0) ? "[^]"
                        : (e->is_dir ? "[d]" : "   ");
        snprintf(line, sizeof(line), "%s  %s", tag, e->name);
        draw_text_clipped(state->renderer, g_ui_font ? g_ui_font : g_font,
                          line, bx + 10, ry + 2, bw - 20,
                          sel ? sel_fg : (e->is_dir ? dir_fg : fg));
    }
}

static void render_editor_body(vis_state_t *state, vis_window_t *win) {
    int bx = win->x + BORDER;
    int by = win->y + TITLEBAR_H;
    int bw = win->w - 2 * BORDER;
    int bh = win->h - TITLEBAR_H - BORDER;

    fill_rect(state->renderer, (SDL_Rect){bx, by, bw, bh}, (SDL_Color){22, 24, 30, 255});
    fill_rect(state->renderer, (SDL_Rect){bx, by, bw, 28}, (SDL_Color){42, 46, 56, 255});

    SDL_Color save_face = win->edit_dirty ? (SDL_Color){170, 90, 60, 255}
                                          : (SDL_Color){60, 110, 80, 255};
    SDL_Color btn_text = {230, 232, 240, 255};
    bool hover = state->desktop.hover_win == win && state->desktop.hover_toolbar_btn == 0;
    draw_button(state->renderer, win->x + 10, win->y + TITLEBAR_H + 3, 90, 22,
                win->edit_dirty ? "Save *" : "Saved", save_face, btn_text, hover);

    draw_text_clipped(state->renderer, g_ui_font ? g_ui_font : g_font,
                      win->path, win->x + 110, win->y + TITLEBAR_H + 7, win->w - 128,
                      (SDL_Color){200, 205, 220, 255});

    int text_y = by + 30;
    int text_x = bx + 6;
    int text_w = bw - 12;
    int text_h = bh - 30;
    SDL_Color fg = {220, 225, 235, 255};
    int line_h = state->font_height + 2;
    int rows_shown = text_h / line_h;

    const char *buf = win->edit_buffer ? win->edit_buffer : "";
    size_t len = win->edit_len;

    int line_no = 0;
    size_t pos = 0;
    while (pos < len && line_no < win->edit_scroll_row) {
        if (buf[pos] == '\n') line_no++;
        pos++;
    }

    int row = 0;
    int cursor_px = -1, cursor_py = -1;
    while (pos <= len && row < rows_shown) {
        char line[1024];
        size_t ll = 0;
        size_t line_start = pos;
        while (pos < len && buf[pos] != '\n' && ll < sizeof(line) - 1) {
            line[ll++] = buf[pos++];
        }
        line[ll] = '\0';
        if ((size_t)win->edit_cursor >= line_start && (size_t)win->edit_cursor <= pos) {
            int col = win->edit_cursor - line_start;
            if (col > (int)ll) col = ll;
            char tmp[1024];
            if (col > 0) {
                memcpy(tmp, line, col);
                tmp[col] = '\0';
                int cw = 0, ch = 0;
                TTF_SizeUTF8(g_font, tmp, &cw, &ch);
                cursor_px = text_x + cw;
            } else {
                cursor_px = text_x;
            }
            cursor_py = text_y + row * line_h;
        }
        draw_text_clipped(state->renderer, g_font, line, text_x, text_y + row * line_h, text_w, fg);
        if (pos < len && buf[pos] == '\n') pos++;
        else if (pos >= len) break;
        row++;
    }
    if (cursor_px >= 0) {
        SDL_SetRenderDrawColor(state->renderer, 255, 255, 255, 255);
        SDL_Rect cur = {cursor_px, cursor_py, 2, line_h};
        SDL_RenderFillRect(state->renderer, &cur);
    }
}

static void render_window(vis_state_t *state, vis_window_t *win, bool focused) {
    if (!win->visible || win->minimized) return;
    draw_window_frame(state, win, focused);
    switch (win->kind) {
        case VIS_WIN_TERMINAL: render_terminal_body(state, win); break;
        case VIS_WIN_FILES:    render_files_body(state, win); break;
        case VIS_WIN_EDITOR:   render_editor_body(state, win); break;
    }
    draw_resize_grip(state, win);
}

static int count_taskbar_entries(vis_state_t *state) {
    int n = 0;
    for (vis_window_t *w = state->windows; w; w = w->next) n++;
    return n;
}

static int taskbar_entry_width(vis_state_t *state) {
    int n = count_taskbar_entries(state);
    if (n == 0) return 160;
    int budget = state->window_width - 80 - 90;
    int w = budget / n - 4;
    if (w > 180) w = 180;
    if (w < 80) w = 80;
    return w;
}

static void draw_taskbar(vis_state_t *state) {
    int y = state->window_height - TASKBAR_H;
    fill_rect(state->renderer, (SDL_Rect){0, y, state->window_width, TASKBAR_H},
              (SDL_Color){18, 20, 28, 240});
    SDL_SetRenderDrawColor(state->renderer, 70, 90, 130, 255);
    SDL_RenderDrawLine(state->renderer, 0, y, state->window_width, y);

    TTF_Font *uf = g_ui_font ? g_ui_font : g_font;
    SDL_Color fg = {220, 225, 235, 255};
    SDL_Color launcher_face = state->desktop.launcher_open
        ? (SDL_Color){80, 120, 190, 255} : (SDL_Color){48, 60, 92, 255};
    draw_button(state->renderer, 6, y + 4, 64, TASKBAR_H - 8, "apps",
                launcher_face, fg, state->desktop.hover_launcher_btn);

    int bx = 76;
    int bw = taskbar_entry_width(state);
    int idx = 0;
    for (vis_window_t *w = state->windows; w; w = w->next) {
        bool f = state->focused == w && !w->minimized;
        bool hover = (state->desktop.hover_taskbar_btn == idx);
        SDL_Color face = f ? (SDL_Color){62, 94, 150, 255}
                           : (w->minimized ? (SDL_Color){32, 38, 54, 255}
                                           : (SDL_Color){40, 48, 68, 255});
        char label[300];
        snprintf(label, sizeof(label), "%s%s", w->minimized ? "- " : "", w->title);
        draw_button(state->renderer, bx, y + 4, bw, TASKBAR_H - 8, label, face, fg, hover);
        bx += bw + 4;
        idx++;
        if (bx + bw > state->window_width - 90) break;
    }

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%a %d %b  %H:%M:%S", tm);
    int tw = 0, th = 0;
    TTF_SizeUTF8(uf, ts, &tw, &th);
    draw_text(state->renderer, uf, ts, state->window_width - tw - 12, y + 8, fg);
}

static void draw_context_menu(vis_state_t *state) {
    vis_context_menu_t *c = &state->desktop.ctx;
    if (!c->open) return;
    fill_rect(state->renderer, (SDL_Rect){c->x + 3, c->y + 4, c->w, c->h},
              (SDL_Color){0, 0, 0, 120});
    fill_rect(state->renderer, (SDL_Rect){c->x, c->y, c->w, c->h},
              (SDL_Color){36, 40, 54, 250});
    SDL_SetRenderDrawColor(state->renderer, 90, 110, 160, 255);
    SDL_RenderDrawRect(state->renderer, &(SDL_Rect){c->x, c->y, c->w, c->h});
    TTF_Font *uf = g_ui_font ? g_ui_font : g_font;
    SDL_Color fg = {230, 232, 240, 255};
    int row_h = 22;
    for (int i = 0; i < c->item_count; i++) {
        int ry = c->y + 3 + i * row_h;
        if (i == c->hover_item) {
            fill_rect(state->renderer, (SDL_Rect){c->x + 2, ry, c->w - 4, row_h},
                      (SDL_Color){62, 94, 150, 220});
        }
        draw_text(state->renderer, uf, c->items[i].label, c->x + 10, ry + 4, fg);
    }
}

static void draw_launcher(vis_state_t *state) {
    if (!state->desktop.launcher_open) return;
    int lx = state->desktop.launcher_x, ly = state->desktop.launcher_y;
    int lw = state->desktop.launcher_w, lh = state->desktop.launcher_h;
    fill_rect(state->renderer, (SDL_Rect){lx, ly, lw, lh}, (SDL_Color){32, 36, 48, 245});
    SDL_SetRenderDrawColor(state->renderer, 90, 110, 160, 255);
    SDL_RenderDrawRect(state->renderer, &(SDL_Rect){lx, ly, lw, lh});
    const char *items[4] = {"New Terminal", "Open Home", "Open /", "Quit"};
    TTF_Font *uf = g_ui_font ? g_ui_font : g_font;
    SDL_Color fg = {230, 232, 240, 255};
    int item_h = lh / 4;
    for (int i = 0; i < 4; i++) {
        bool hover = (state->desktop.hover_launcher_item == i);
        if (hover) fill_rect(state->renderer, (SDL_Rect){lx + 2, ly + i * item_h, lw - 4, item_h},
                             (SDL_Color){62, 94, 150, 220});
        draw_text(state->renderer, uf, items[i], lx + 14, ly + i * item_h + (item_h - 12) / 2, fg);
    }
}

void vis_sdl_render(vis_state_t *state) {
    if (!state->renderer || !g_font) return;
    vis_windows_tick(state);
    draw_wallpaper(state);
    draw_desktop_icons(state);
    /* Head of list is TOPMOST — draw tail-first so head paints over. */
    vis_window_t *tail = state->windows;
    if (tail) while (tail->next) tail = tail->next;
    for (vis_window_t *w = tail; w; w = w->prev) {
        render_window(state, w, w == state->focused);
    }
    draw_taskbar(state);
    draw_launcher(state);
    draw_context_menu(state);
    if (state->focused && state->focused->kind == VIS_WIN_TERMINAL &&
        state->focused->pane &&
        (state->focused->pane->mode == VIS_MODE_COMMAND ||
         state->focused->pane->mode == VIS_MODE_SEARCH)) {
        vis_sdl_draw_command_line(state, state->focused->pane);
    }
    SDL_RenderPresent(state->renderer);
}

void vis_sdl_draw_pane(vis_state_t *state, vis_pane_t *pane) { (void)state; (void)pane; }
void vis_sdl_draw_file_manager(vis_state_t *state) { (void)state; }
void vis_sdl_draw_status_bar(vis_state_t *state, vis_pane_t *pane) { (void)state; (void)pane; }

void vis_sdl_draw_command_line(vis_state_t *state, vis_pane_t *pane) {
    char *buffer = (pane->mode == VIS_MODE_COMMAND) ? pane->command_buffer : pane->search_buffer;
    char prompt = (pane->mode == VIS_MODE_COMMAND) ? ':' : '/';
    char text[VIS_MAX_BUFFER + 2];
    snprintf(text, sizeof(text), "%c%s", prompt, buffer);
    int y = state->window_height - TASKBAR_H - 24;
    SDL_Rect bar = {40, y, state->window_width - 80, 22};
    fill_rect(state->renderer, bar, (SDL_Color){10, 10, 30, 240});
    SDL_Color fg = {230, 230, 240, 255};
    draw_text(state->renderer, g_ui_font ? g_ui_font : g_font, text, bar.x + 8, bar.y + 2, fg);
}

static bool in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void update_hover(vis_state_t *state, int mx, int my) {
    vis_desktop_t *d = &state->desktop;
    d->mouse_x = mx; d->mouse_y = my;
    d->hover_win = NULL;
    d->hover_titlebar_btn = -1;
    d->hover_toolbar_btn = -1;
    d->hover_taskbar_btn = -1;
    d->hover_resize = false;
    d->hover_launcher_btn = false;
    d->hover_launcher_item = -1;
    d->ctx.hover_item = -1;
    if (d->ctx.open) {
        int ci = vis_context_hit(state, mx, my);
        if (ci >= 0) { d->ctx.hover_item = ci; return; }
    }

    int taskbar_y = state->window_height - TASKBAR_H;
    if (my >= taskbar_y) {
        if (in_rect(mx, my, 6, taskbar_y + 4, 64, TASKBAR_H - 8)) d->hover_launcher_btn = true;
        int bx = 76;
        int bw = taskbar_entry_width(state);
        int idx = 0;
        for (vis_window_t *w = state->windows; w; w = w->next) {
            if (in_rect(mx, my, bx, taskbar_y + 4, bw, TASKBAR_H - 8)) {
                d->hover_taskbar_btn = idx;
                break;
            }
            bx += bw + 4; idx++;
            if (bx + bw > state->window_width - 90) break;
        }
        return;
    }

    if (d->launcher_open) {
        d->hover_launcher_item = vis_launcher_hit(state, mx, my);
        if (d->hover_launcher_item >= 0) return;
    }

    vis_window_t *hit = vis_window_at(state, mx, my);
    if (hit) {
        d->hover_win = hit;
        int dot = vis_window_dot_at(hit, mx, my);
        if (dot >= 0) d->hover_titlebar_btn = dot;
        if (vis_window_resize_at(hit, mx, my)) d->hover_resize = true;
        if (hit->kind == VIS_WIN_FILES) {
            int tb = vis_files_toolbar_hit(state, hit, mx, my);
            if (tb >= 0) d->hover_toolbar_btn = tb;
        } else if (hit->kind == VIS_WIN_EDITOR) {
            if (in_rect(mx, my, hit->x + 10, hit->y + TITLEBAR_H + 3, 90, 22))
                d->hover_toolbar_btn = 0;
        }
    }
}

void vis_sdl_handle_event(vis_state_t *state, SDL_Event *event) {
    vis_desktop_t *d = &state->desktop;
    switch (event->type) {
        case SDL_QUIT:
            state->running = false;
            break;
        case SDL_WINDOWEVENT:
            if (event->window.event == SDL_WINDOWEVENT_RESIZED ||
                event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                vis_update_layout(state);
            break;
        case SDL_MOUSEBUTTONDOWN: {
            int mx = event->button.x, my = event->button.y;
            int taskbar_y = state->window_height - TASKBAR_H;
            bool right = event->button.button == SDL_BUTTON_RIGHT;

            if (d->ctx.open) {
                int ci = vis_context_hit(state, mx, my);
                if (ci >= 0) { vis_context_activate(state, ci); return; }
                vis_context_close(state);
            }

            if (right) {
                /* Build the right-click menu for whatever we're over. */
                if (my < taskbar_y) {
                    vis_window_t *hit = vis_window_at(state, mx, my);
                    if (hit && hit->kind == VIS_WIN_FILES && my > hit->y + TITLEBAR_H + 30) {
                        int row_h = state->font_height + 4;
                        int body_y = hit->y + TITLEBAR_H + 30 + (hit->prompting ? 28 : 0);
                        int row = (my - body_y) / row_h + hit->entry_scroll;
                        if (row >= 0 && row < hit->entry_count) hit->entry_selected = row;
                        vis_window_focus(state, hit);
                        vis_context_open_for_files(state, hit,
                            (row >= 0 && row < hit->entry_count) ? row : -1, mx, my);
                        return;
                    }
                    int icon = vis_desktop_icon_at(state, mx, my);
                    if (icon >= 0) {
                        state->icon_focus = icon;
                        vis_context_open_for_icon(state, icon, mx, my);
                        return;
                    }
                    if (!hit) { vis_context_open_for_desktop(state, mx, my); return; }
                }
                /* Right-click on titlebar/body of non-files window → just focus. */
            }

            if (d->launcher_open) {
                int item = vis_launcher_hit(state, mx, my);
                if (item >= 0) { vis_launcher_activate(state, item); return; }
                d->launcher_open = false;
            }

            if (my >= taskbar_y) {
                if (in_rect(mx, my, 6, taskbar_y + 4, 64, TASKBAR_H - 8)) {
                    vis_launcher_toggle(state);
                    return;
                }
                int bx = 76;
                int bw = taskbar_entry_width(state);
                for (vis_window_t *w = state->windows; w; w = w->next) {
                    if (in_rect(mx, my, bx, taskbar_y + 4, bw, TASKBAR_H - 8)) {
                        if (state->focused == w && !w->minimized) {
                            vis_window_minimize(state, w);
                        } else {
                            vis_window_focus(state, w);
                        }
                        return;
                    }
                    bx += bw + 4;
                    if (bx + bw > state->window_width - 90) break;
                }
                return;
            }

            vis_window_t *hit = vis_window_at(state, mx, my);
            if (hit) {
                int dot = vis_window_dot_at(hit, mx, my);
                if (dot == 0) { vis_window_close(state, hit); return; }
                if (dot == 1) { vis_window_minimize(state, hit); return; }
                if (dot == 2) { vis_window_toggle_max(state, hit); vis_update_layout(state); return; }
                if (vis_window_resize_at(hit, mx, my)) {
                    vis_window_focus(state, hit);
                    d->resizing = hit;
                    d->resize_start_w = hit->w;
                    d->resize_start_h = hit->h;
                    d->resize_start_mx = mx;
                    d->resize_start_my = my;
                    return;
                }
                if (vis_window_hit_titlebar(state, hit, mx, my)) {
                    vis_window_focus(state, hit);
                    if (!hit->maximized) {
                        d->dragging = hit;
                        d->drag_dx = mx - hit->x;
                        d->drag_dy = my - hit->y;
                    }
                    return;
                }
                Uint32 now = SDL_GetTicks();
                bool dbl = (hit == state->focused && now - state->last_bg_click_ms < DBLCLICK_MS);
                vis_window_handle_click(state, hit, mx, my);
                if (dbl) vis_window_handle_dblclick(state, hit, mx, my);
                state->last_bg_click_ms = now;
                return;
            }

            int icon = vis_desktop_icon_at(state, mx, my);
            if (icon >= 0) {
                Uint32 now = SDL_GetTicks();
                bool dbl = (icon == state->icon_focus &&
                            now - state->icons[icon].last_click_ms < DBLCLICK_MS);
                state->icon_focus = icon;
                state->icons[icon].last_click_ms = now;
                if (dbl) vis_desktop_handle_icon_dblclick(state, icon);
                return;
            }
            state->icon_focus = -1;
            /* Deliberately do NOT clear state->focused here. Clicking empty
               wallpaper used to deselect the current window, after which
               keystrokes (including Ctrl+C) went nowhere until the user
               explicitly re-focused a window. Keep the last focus so the
               terminal keeps receiving input. */
            break;
        }
        case SDL_MOUSEBUTTONUP:
            d->dragging = NULL;
            d->resizing = NULL;
            break;
        case SDL_MOUSEMOTION: {
            int mx = event->motion.x, my = event->motion.y;
            if (d->dragging) {
                d->dragging->x = mx - d->drag_dx;
                d->dragging->y = my - d->drag_dy;
                vis_update_layout(state);
            } else if (d->resizing) {
                int new_w = d->resize_start_w + (mx - d->resize_start_mx);
                int new_h = d->resize_start_h + (my - d->resize_start_my);
                if (new_w < d->resizing->min_w) new_w = d->resizing->min_w;
                if (new_h < d->resizing->min_h) new_h = d->resizing->min_h;
                d->resizing->w = new_w;
                d->resizing->h = new_h;
                vis_update_layout(state);
            } else {
                update_hover(state, mx, my);
            }
            break;
        }
        case SDL_MOUSEWHEEL: {
            int mx, my;
            SDL_GetMouseState(&mx, &my);
            vis_window_t *under = vis_window_at(state, mx, my);
            if (!under) break;
            int dy = event->wheel.y;
            if (dy == 0) break;
            if (under->kind == VIS_WIN_FILES) {
                under->entry_scroll -= dy * 3;
                if (under->entry_scroll < 0) under->entry_scroll = 0;
                int max_scroll = under->entry_count - 1;
                if (max_scroll < 0) max_scroll = 0;
                if (under->entry_scroll > max_scroll) under->entry_scroll = max_scroll;
            } else if (under->kind == VIS_WIN_EDITOR) {
                under->edit_scroll_row -= dy * 3;
                if (under->edit_scroll_row < 0) under->edit_scroll_row = 0;
            } else if (under->kind == VIS_WIN_TERMINAL && under->pane) {
                int sb = vis_scrollback_count(&under->pane->scrollback);
                under->pane->scroll_offset += dy * 3;
                if (under->pane->scroll_offset < 0) under->pane->scroll_offset = 0;
                if (under->pane->scroll_offset > sb) under->pane->scroll_offset = sb;
            }
            break;
        }
        case SDL_KEYDOWN:
            if (state->focused) {
                vis_window_handle_key(state, state->focused,
                                      event->key.keysym.sym, event->key.keysym.mod);
            }
            break;
        case SDL_TEXTINPUT:
            if (state->focused) {
                vis_window_handle_text(state, state->focused, event->text.text);
            }
            break;
    }
}
