#include "vis.h"

static int vterm_damage(VTermRect rect, void *user) {
    vis_pane_t *pane = (vis_pane_t *)user;
    pane->dirty = true;
    return 1;
}

static int vterm_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user) {
    (void)oldpos; (void)visible;
    return 1;
}

static int vterm_settermprop(VTermProp prop, VTermValue *val, void *user) {
    (void)prop; (void)val; (void)user;
    return 1;
}

static int vterm_bell(void *user) {
    (void)user;
    return 1;
}

static int vterm_sb_pushline(int cols, const VTermScreenCell *cells, void *user) {
    vis_pane_t *pane = (vis_pane_t *)user;
    char line[2048];
    int len = 0;
    for (int i = 0; i < cols && len < (int)sizeof(line) - 1; i++) {
        uint32_t ch = cells[i].chars[0];
        if (ch == 0 || ch == ' ') line[len++] = ' ';
        else if (ch >= 32 && ch < 127) line[len++] = (char)ch;
        else line[len++] = '?';
    }
    while (len > 0 && line[len - 1] == ' ') len--;
    line[len] = '\0';
    vis_scrollback_add(&pane->scrollback, line);
    return 1;
}

static VTermScreenCallbacks screen_callbacks = {
    .damage = vterm_damage,
    .movecursor = vterm_movecursor,
    .settermprop = vterm_settermprop,
    .bell = vterm_bell,
    .sb_pushline = vterm_sb_pushline,
    .sb_popline = NULL,
    .sb_clear = NULL,
    .sb_pushline4 = NULL,
};

static void vterm_output_callback(const char *s, size_t len, void *user) {
    vis_pane_t *pane = (vis_pane_t *)user;
    if (!pane->conn || !pane->conn->channel) return;
    pthread_mutex_lock(&pane->conn->session_lock);
    size_t sent = 0;
    while (sent < len && pane->conn->channel) {
        ssize_t n = libssh2_channel_write(pane->conn->channel, s + sent, len - sent);
        if (n == LIBSSH2_ERROR_EAGAIN || n == LIBSSH2_ERROR_TIMEOUT) continue;
        if (n < 0) break;
        sent += n;
    }
    pthread_mutex_unlock(&pane->conn->session_lock);
}

int vis_vterm_init_pane(vis_pane_t *pane, int cols, int rows) {
    pane->vterm = vterm_new(rows, cols);
    if (!pane->vterm) {
        vis_log(VIS_LOG_ERROR, "Failed to create vterm");
        return -1;
    }
    
    vterm_set_utf8(pane->vterm, 1);
    vterm_output_set_callback(pane->vterm, vterm_output_callback, pane);
    vterm_screen_set_callbacks(vterm_obtain_screen(pane->vterm), &screen_callbacks, pane);
    
    pane->screen = vterm_obtain_screen(pane->vterm);
    if (!pane->screen) {
        vis_log(VIS_LOG_ERROR, "Failed to obtain vterm screen");
        vterm_free(pane->vterm);
        return -1;
    }
    
    vterm_screen_enable_altscreen(pane->screen, 1);
    vterm_screen_reset(pane->screen, 1);
    
    pane->width = cols;
    pane->height = rows;
    
    vis_log(VIS_LOG_INFO, "VTerm initialized: %dx%d", cols, rows);
    return 0;
}

void vis_vterm_destroy_pane(vis_pane_t *pane) {
    if (pane->vterm) {
        vterm_free(pane->vterm);
        pane->vterm = NULL;
    }
    pane->screen = NULL;
}

void vis_vterm_write(vis_pane_t *pane, const char *data, size_t len) {
    if (!pane->vterm) return;
    vterm_input_write(pane->vterm, data, len);
    /* New data arrived; snap back to live view so the user doesn't miss it. */
    pane->scroll_offset = 0;
}

void vis_vterm_resize(vis_pane_t *pane, int cols, int rows) {
    if (!pane->vterm) return;
    vterm_set_size(pane->vterm, rows, cols);
    pane->width = cols;
    pane->height = rows;
}

static int sdl_key_to_vterm(SDL_Keycode key, SDL_Keymod mod, char *out, size_t out_len) {
    switch (key) {
        case SDLK_RETURN: snprintf(out, out_len, "\r"); return 1;
        case SDLK_TAB: snprintf(out, out_len, "\t"); return 1;
        case SDLK_BACKSPACE: snprintf(out, out_len, "\x7f"); return 1;
        case SDLK_ESCAPE: snprintf(out, out_len, "\x1b"); return 1;
        case SDLK_UP: snprintf(out, out_len, "\x1b[A"); return 1;
        case SDLK_DOWN: snprintf(out, out_len, "\x1b[B"); return 1;
        case SDLK_RIGHT: snprintf(out, out_len, "\x1b[C"); return 1;
        case SDLK_LEFT: snprintf(out, out_len, "\x1b[D"); return 1;
        case SDLK_HOME: snprintf(out, out_len, "\x1b[H"); return 1;
        case SDLK_END: snprintf(out, out_len, "\x1b[F"); return 1;
        case SDLK_PAGEUP: snprintf(out, out_len, "\x1b[5~"); return 1;
        case SDLK_PAGEDOWN: snprintf(out, out_len, "\x1b[6~"); return 1;
        case SDLK_INSERT: snprintf(out, out_len, "\x1b[2~"); return 1;
        case SDLK_DELETE: snprintf(out, out_len, "\x1b[3~"); return 1;
        case SDLK_F1: snprintf(out, out_len, "\x1bOP"); return 1;
        case SDLK_F2: snprintf(out, out_len, "\x1bOQ"); return 1;
        case SDLK_F3: snprintf(out, out_len, "\x1bOR"); return 1;
        case SDLK_F4: snprintf(out, out_len, "\x1bOS"); return 1;
        case SDLK_F5: snprintf(out, out_len, "\x1b[15~"); return 1;
        case SDLK_F6: snprintf(out, out_len, "\x1b[17~"); return 1;
        case SDLK_F7: snprintf(out, out_len, "\x1b[18~"); return 1;
        case SDLK_F8: snprintf(out, out_len, "\x1b[19~"); return 1;
        case SDLK_F9: snprintf(out, out_len, "\x1b[20~"); return 1;
        case SDLK_F10: snprintf(out, out_len, "\x1b[21~"); return 1;
        case SDLK_F11: snprintf(out, out_len, "\x1b[23~"); return 1;
        case SDLK_F12: snprintf(out, out_len, "\x1b[24~"); return 1;
    }
    
    if (mod & KMOD_CTRL) {
        if (key >= 'a' && key <= 'z') {
            out[0] = key - 'a' + 1;
            out[1] = '\0';
            return 1;
        }
        if (key >= 'A' && key <= 'Z') {
            out[0] = key - 'A' + 1;
            out[1] = '\0';
            return 1;
        }
        switch (key) {
            case SDLK_LEFTBRACKET: snprintf(out, out_len, "\x1b"); return 1;
            case SDLK_BACKSLASH: snprintf(out, out_len, "\x1c"); return 1;
            case SDLK_RIGHTBRACKET: snprintf(out, out_len, "\x1d"); return 1;
            case SDLK_6: snprintf(out, out_len, "\x1e"); return 1;
            case SDLK_MINUS: snprintf(out, out_len, "\x1f"); return 1;
            case SDLK_SPACE: snprintf(out, out_len, "\x00"); return 1;
        }
    }
    
    return 0;
}

void vis_vterm_handle_key(vis_pane_t *pane, SDL_Keycode key, SDL_Keymod mod, const char *text) {
    if (!pane || !pane->vterm) return;
    if (pane->mode == VIS_MODE_COMMAND || pane->mode == VIS_MODE_SEARCH) return;

    const char *data = NULL;
    size_t data_len = 0;
    char seq[32] = {0};

    if (key && sdl_key_to_vterm(key, mod, seq, sizeof(seq))) {
        data = seq;
        data_len = strlen(seq);
    } else if (key == 0 && text && text[0]) {
        data = text;
        data_len = strlen(text);
    }
    if (!data || !data_len) return;

    /* User keystrokes go DIRECTLY to the SSH channel. The server echoes them
       back and the reader thread feeds that echo into vterm_input_write, so
       the local vterm display stays in sync without us double-echoing. */
    if (!pane->conn || !pane->conn->channel) return;
    pthread_mutex_lock(&pane->conn->session_lock);
    size_t sent = 0;
    while (sent < data_len && pane->conn && pane->conn->channel) {
        ssize_t n = libssh2_channel_write(pane->conn->channel, data + sent, data_len - sent);
        if (n == LIBSSH2_ERROR_EAGAIN || n == LIBSSH2_ERROR_TIMEOUT) continue;
        if (n < 0) break;
        sent += n;
    }
    pthread_mutex_unlock(&pane->conn->session_lock);
}

void vis_vterm_handle_mouse(vis_pane_t *pane, int x, int y, uint32_t buttons) {
    if (!pane->vterm) return;
    
    VTermModifier mod = 0;
    
    if (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) {
        vterm_mouse_button(pane->vterm, 1, 1, mod);
    }
    if (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) {
        vterm_mouse_button(pane->vterm, 3, 1, mod);
    }
    if (buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE)) {
        vterm_mouse_button(pane->vterm, 2, 1, mod);
    }
    
    vterm_mouse_move(pane->vterm, y, x, mod);
}

void vis_vterm_damage_callback(VTermRect rect, void *user) {
    vis_pane_t *pane = (vis_pane_t *)user;
    pane->dirty = true;
}