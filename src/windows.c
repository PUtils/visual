#include "vis.h"
#include <ctype.h>

#define TITLEBAR_H 26
#define BORDER     2
#define DOT_SIZE   10
#define DOT_SPACING 16
#define DOT_X0     10
#define DOT_Y0     6

static vis_ssh_connection_t *active_conn(vis_state_t *state) {
    if (state->active_pane && state->active_pane->conn) return state->active_pane->conn;
    if (state->panes && state->panes->conn) return state->panes->conn;
    return NULL;
}

/* The list is ordered so that HEAD is the TOPMOST window:
 *   - new windows prepend to head
 *   - bring_to_front moves to head
 *   - sdl.c iterates tail -> head when drawing (head drawn last = on top)
 *   - sdl.c iterates head -> tail for hit-testing (head matches first)
 */
static void window_push_front(vis_state_t *state, vis_window_t *win) {
    win->prev = NULL;
    win->next = state->windows;
    if (state->windows) state->windows->prev = win;
    state->windows = win;
}

static void window_detach(vis_state_t *state, vis_window_t *win) {
    if (win->prev) win->prev->next = win->next;
    else state->windows = win->next;
    if (win->next) win->next->prev = win->prev;
    win->next = win->prev = NULL;
}

void vis_window_bring_to_front(vis_state_t *state, vis_window_t *win) {
    if (!win || state->windows == win) return;
    window_detach(state, win);
    window_push_front(state, win);
}

void vis_window_focus(vis_state_t *state, vis_window_t *win) {
    if (!win) return;
    win->visible = true;
    win->minimized = false;
    state->focused = win;
    vis_window_bring_to_front(state, win);
    if (win->kind == VIS_WIN_TERMINAL && win->pane) {
        state->active_pane = win->pane;
    }
}

static int next_cascade(vis_state_t *state) {
    int base = 60;
    int count = 0;
    for (vis_window_t *w = state->windows; w; w = w->next) count++;
    return base + (count % 8) * 24;
}

vis_window_t *vis_window_new_terminal(vis_state_t *state) {
    vis_window_t *win = calloc(1, sizeof(*win));
    win->id = ++state->next_window_id;
    win->kind = VIS_WIN_TERMINAL;
    win->visible = true;
    win->pane = state->active_pane ? state->active_pane : (state->panes ? state->panes : NULL);
    win->w = state->cols * state->cell_width + 2 * BORDER;
    win->h = state->rows * state->cell_height + TITLEBAR_H + BORDER;
    win->min_w = 420;
    win->min_h = 220;
    win->x = next_cascade(state);
    win->y = next_cascade(state);
    const char *host = win->pane && win->pane->conn ? win->pane->conn->host : state->config.ssh.host;
    const char *user = win->pane && win->pane->conn ? win->pane->conn->username : state->config.ssh.user;
    snprintf(win->title, sizeof(win->title), "Terminal — %s@%s", user, host);
    window_push_front(state, win);
    vis_window_focus(state, win);
    return win;
}

static int entry_cmp(const void *a, const void *b) {
    const vis_file_entry_t *ea = a, *eb = b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    return strcasecmp(ea->name, eb->name);
}

static void files_apply_entries(vis_window_t *win, vis_file_entry_t *entries, int count) {
    if (count > 0) qsort(entries, count, sizeof(*entries), entry_cmp);
    bool add_parent = (win->path[0] == '/' && win->path[1] != '\0');
    int final_count = count + (add_parent ? 1 : 0);
    vis_file_entry_t *final_entries = calloc(final_count > 0 ? final_count : 1,
                                             sizeof(vis_file_entry_t));
    int idx = 0;
    if (add_parent) {
        vis_file_entry_t *p = &final_entries[idx++];
        strncpy(p->name, "..", sizeof(p->name) - 1);
        snprintf(p->path, sizeof(p->path), "%s/..", win->path);
        p->is_dir = true;
    }
    for (int i = 0; i < count; i++) final_entries[idx++] = entries[i];
    free(entries);
    free(win->entries);
    win->entries = final_entries;
    win->entry_count = final_count;
    if (win->entry_selected >= win->entry_count) win->entry_selected = win->entry_count - 1;
    if (win->entry_selected < 0) win->entry_selected = 0;
    snprintf(win->title, sizeof(win->title), "Files - %s", win->path);
}

void vis_files_refresh(vis_window_t *win, vis_ssh_connection_t *conn) {
    if (!conn) return;
    vis_file_entry_t *entries = NULL;
    int count = 0;
    if (vis_sftp_list(conn, win->path, &entries, &count) != 0) return;
    files_apply_entries(win, entries, count);
}

typedef struct {
    vis_window_t *win;
    vis_ssh_connection_t *conn;
    char path[VIS_MAX_PATH_LEN];
} files_refresh_task_t;

static void *files_refresh_worker(void *arg) {
    files_refresh_task_t *task = (files_refresh_task_t *)arg;
    vis_file_entry_t *entries = NULL;
    int count = 0;
    int rc = vis_sftp_list(task->conn, task->path, &entries, &count);
    task->win->refresh_result_entries = entries;
    task->win->refresh_result_count = count;
    task->win->refresh_result_status = rc;
    task->win->refresh_done = true;
    free(task);
    return NULL;
}

void vis_files_refresh_async(vis_window_t *win, vis_ssh_connection_t *conn, const char *path) {
    if (!conn || !win) return;
    if (win->refresh_in_progress) return;

    if (path && path != win->path) {
        strncpy(win->path, path, sizeof(win->path) - 1);
        win->path[sizeof(win->path) - 1] = '\0';
    }
    strncpy(win->refresh_target_path, win->path, sizeof(win->refresh_target_path) - 1);
    win->refresh_target_path[sizeof(win->refresh_target_path) - 1] = '\0';

    snprintf(win->title, sizeof(win->title), "Files - %s  (loading...)", win->path);

    files_refresh_task_t *task = calloc(1, sizeof(*task));
    task->win = win;
    task->conn = conn;
    strncpy(task->path, win->refresh_target_path, sizeof(task->path) - 1);

    win->refresh_in_progress = true;
    win->refresh_done = false;
    if (pthread_create(&win->refresh_thread, NULL, files_refresh_worker, task) != 0) {
        win->refresh_in_progress = false;
        free(task);
        vis_files_refresh(win, conn);
        return;
    }
    pthread_detach(win->refresh_thread);
    win->refresh_thread_valid = true;
}

void vis_windows_tick(vis_state_t *state) {
    for (vis_window_t *w = state->windows; w; w = w->next) {
        if (w->kind != VIS_WIN_FILES) continue;

        if (w->refresh_done) {
            if (w->refresh_result_status == 0 && w->refresh_result_entries) {
                files_apply_entries(w, w->refresh_result_entries, w->refresh_result_count);
            } else {
                snprintf(w->title, sizeof(w->title), "Files - %s  (error)", w->path);
                free(w->refresh_result_entries);
            }
            w->refresh_result_entries = NULL;
            w->refresh_result_count = 0;
            w->refresh_done = false;
            w->refresh_in_progress = false;
        }

        /* A background write finished and wants a refresh. */
        if (w->refresh_requested && !w->refresh_in_progress) {
            w->refresh_requested = false;
            vis_files_refresh_async(w, active_conn(state), NULL);
        }
    }
}

/* ---------- async SFTP writes (rm / rename / mkdir) ---------- */

typedef enum { SFTP_WOP_RM, SFTP_WOP_RENAME, SFTP_WOP_MKDIR } sftp_wop_t;

typedef struct {
    sftp_wop_t op;
    vis_window_t *refresh_win;
    vis_ssh_connection_t *conn;
    char path[VIS_MAX_PATH_LEN];
    char path2[VIS_MAX_PATH_LEN];
} sftp_write_task_t;

static void *sftp_write_worker(void *arg) {
    sftp_write_task_t *t = (sftp_write_task_t *)arg;
    switch (t->op) {
        case SFTP_WOP_RM:     vis_sftp_rm(t->conn, t->path); break;
        case SFTP_WOP_RENAME: vis_sftp_rename(t->conn, t->path, t->path2); break;
        case SFTP_WOP_MKDIR:  vis_sftp_mkdir(t->conn, t->path); break;
    }
    if (t->refresh_win) {
        t->refresh_win->refresh_requested = true;
        __sync_fetch_and_sub(&t->refresh_win->write_in_progress, 1);
    }
    free(t);
    return NULL;
}

static void queue_sftp_write(sftp_wop_t op, vis_window_t *refresh_win,
                             vis_ssh_connection_t *conn,
                             const char *path, const char *path2) {
    if (!conn || !path) return;
    sftp_write_task_t *t = calloc(1, sizeof(*t));
    t->op = op;
    t->refresh_win = refresh_win;
    t->conn = conn;
    strncpy(t->path, path, sizeof(t->path) - 1);
    if (path2) strncpy(t->path2, path2, sizeof(t->path2) - 1);
    if (refresh_win) __sync_fetch_and_add(&refresh_win->write_in_progress, 1);
    pthread_t th;
    if (pthread_create(&th, NULL, sftp_write_worker, t) != 0) {
        if (refresh_win) __sync_fetch_and_sub(&refresh_win->write_in_progress, 1);
        /* Fall back to synchronous. */
        sftp_write_worker(t);
        return;
    }
    pthread_detach(th);
}

vis_window_t *vis_window_new_files(vis_state_t *state, const char *path) {
    vis_window_t *win = calloc(1, sizeof(*win));
    win->id = ++state->next_window_id;
    win->kind = VIS_WIN_FILES;
    win->visible = true;
    strncpy(win->path, path && *path ? path : "/", sizeof(win->path) - 1);
    win->x = next_cascade(state) + 200;
    win->y = next_cascade(state);
    win->w = 520;
    win->h = 520;
    win->min_w = 340;
    win->min_h = 220;
    snprintf(win->title, sizeof(win->title), "Files - %s", win->path);
    window_push_front(state, win);
    vis_window_focus(state, win);
    vis_files_refresh_async(win, active_conn(state), NULL);
    return win;
}

int vis_editor_load(vis_window_t *win, vis_ssh_connection_t *conn) {
    if (!conn) return -1;
    if (vis_sftp_init(conn) != 0) return -1;

    pthread_mutex_lock(&conn->session_lock);
    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(conn->sftp_session, win->path,
                                               LIBSSH2_FXF_READ, 0);
    if (!h) {
        pthread_mutex_unlock(&conn->session_lock);
        vis_log(VIS_LOG_WARN, "editor: cannot open %s (new file)", win->path);
        win->edit_cap = 1024;
        win->edit_buffer = calloc(1, win->edit_cap);
        win->edit_len = 0;
        win->edit_dirty = true;
        return -1;
    }
    size_t cap = 4096;
    char *buf = malloc(cap);
    size_t len = 0;
    char tmp[4096];
    ssize_t n;
    while ((n = libssh2_sftp_read(h, tmp, sizeof(tmp))) > 0) {
        if (len + n + 1 > cap) {
            while (len + n + 1 > cap) cap *= 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    buf[len] = '\0';
    libssh2_sftp_close(h);
    pthread_mutex_unlock(&conn->session_lock);

    free(win->edit_buffer);
    win->edit_buffer = buf;
    win->edit_len = len;
    win->edit_cap = cap;
    win->edit_cursor = 0;
    win->edit_scroll_row = 0;
    win->edit_dirty = false;
    return 0;
}

int vis_editor_save(vis_window_t *win, vis_ssh_connection_t *conn) {
    if (!conn || !win->edit_buffer) return -1;
    if (vis_sftp_init(conn) != 0) return -1;

    pthread_mutex_lock(&conn->session_lock);
    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(conn->sftp_session, win->path,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!h) {
        pthread_mutex_unlock(&conn->session_lock);
        vis_log(VIS_LOG_ERROR, "editor: cannot write %s", win->path);
        return -1;
    }
    size_t written = 0;
    while (written < win->edit_len) {
        ssize_t n = libssh2_sftp_write(h, win->edit_buffer + written, win->edit_len - written);
        if (n <= 0) break;
        written += n;
    }
    libssh2_sftp_close(h);
    pthread_mutex_unlock(&conn->session_lock);
    win->edit_dirty = false;
    vis_log(VIS_LOG_INFO, "editor: saved %s (%zu bytes)", win->path, written);
    return 0;
}

vis_window_t *vis_window_new_editor(vis_state_t *state, const char *path) {
    vis_window_t *win = calloc(1, sizeof(*win));
    win->id = ++state->next_window_id;
    win->kind = VIS_WIN_EDITOR;
    win->visible = true;
    strncpy(win->path, path, sizeof(win->path) - 1);
    win->x = next_cascade(state) + 100;
    win->y = next_cascade(state) + 40;
    win->w = 720;
    win->h = 560;
    win->min_w = 360;
    win->min_h = 240;
    const char *bn = strrchr(path, '/');
    bn = bn ? bn + 1 : path;
    snprintf(win->title, sizeof(win->title), "Edit — %s", bn);
    window_push_front(state, win);
    vis_window_focus(state, win);
    vis_editor_load(win, active_conn(state));
    return win;
}

void vis_window_destroy(vis_window_t *win) {
    if (!win) return;
    /* Spin until any in-flight async refresh or SFTP write has finished
       touching the window — the threads were detached so we can't join. */
    while ((win->refresh_in_progress && !win->refresh_done) ||
           win->write_in_progress > 0) {
        struct timespec ts = {0, 5 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    free(win->refresh_result_entries);
    free(win->entries);
    free(win->edit_buffer);
    free(win);
}

void vis_window_close(vis_state_t *state, vis_window_t *win) {
    if (!win) return;
    window_detach(state, win);
    if (state->focused == win) state->focused = state->windows;
    vis_window_destroy(win);
}

static void window_toggle_maximize(vis_state_t *state, vis_window_t *win) {
    if (win->maximized) {
        win->x = win->prev_x; win->y = win->prev_y;
        win->w = win->prev_w; win->h = win->prev_h;
        win->maximized = false;
    } else {
        win->prev_x = win->x; win->prev_y = win->y;
        win->prev_w = win->w; win->prev_h = win->h;
        win->x = 0; win->y = 0;
        win->w = state->window_width;
        win->h = state->window_height - state->desktop.taskbar_height;
        win->maximized = true;
    }
}

int vis_window_hit_titlebar(vis_state_t *state, vis_window_t *win, int x, int y) {
    (void)state;
    return x >= win->x && x < win->x + win->w &&
           y >= win->y && y < win->y + TITLEBAR_H;
}

static int titlebar_dot_at(vis_window_t *win, int x, int y) {
    for (int i = 0; i < 3; i++) {
        int dx = win->x + DOT_X0 + i * DOT_SPACING;
        int dy = win->y + DOT_Y0;
        if (x >= dx - 3 && x < dx + DOT_SIZE + 3 &&
            y >= dy - 3 && y < dy + DOT_SIZE + 3) return i;
    }
    return -1;
}

int vis_window_hit_close(vis_state_t *state, vis_window_t *win, int x, int y) {
    (void)state;
    return titlebar_dot_at(win, x, y) == 0;
}

static int hit_resize_grip(vis_window_t *win, int x, int y) {
    if (win->maximized) return 0;
    int gx = win->x + win->w - 16;
    int gy = win->y + win->h - 16;
    return x >= gx && x < gx + 16 && y >= gy && y < gy + 16;
}

vis_window_t *vis_window_at(vis_state_t *state, int x, int y) {
    /* Head is topmost — return the first match in forward iteration. */
    for (vis_window_t *w = state->windows; w; w = w->next) {
        if (!w->visible || w->minimized) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) return w;
    }
    return NULL;
}

static void files_navigate_into(vis_state_t *state, vis_window_t *win, int idx) {
    if (idx < 0 || idx >= win->entry_count) return;
    vis_file_entry_t *e = &win->entries[idx];
    if (e->is_dir) {
        char newpath[VIS_MAX_PATH_LEN];
        if (strcmp(e->name, "..") == 0) {
            strncpy(newpath, win->path, sizeof(newpath) - 1);
            newpath[sizeof(newpath) - 1] = '\0';
            char *slash = strrchr(newpath, '/');
            if (slash && slash != newpath) *slash = '\0';
            else strcpy(newpath, "/");
        } else {
            if (strcmp(win->path, "/") == 0)
                snprintf(newpath, sizeof(newpath), "/%s", e->name);
            else
                snprintf(newpath, sizeof(newpath), "%s/%s", win->path, e->name);
        }
        vis_files_refresh_async(win, active_conn(state), newpath);
    } else {
        vis_window_new_editor(state, e->path);
    }
}

/* ---------- Files toolbar + prompt ---------- */

typedef struct {
    const char *label;
    int w;
} files_btn_t;

/* buttons: 0=up 1=refresh 2=new file 3=new dir 4=rename 5=delete */
static const files_btn_t files_btn_defs[6] = {
    {"up",         50},
    {"refresh",    76},
    {"new file",   84},
    {"new folder", 100},
    {"rename",     72},
    {"delete",     68},
};

static int files_btn_count(vis_state_t *state) {
    return (state && state->read_only) ? 2 : 6;
}

static void files_btn_rect(vis_state_t *state, vis_window_t *win, int idx,
                           int *x, int *y, int *w, int *h) {
    int bx = win->x + 10;
    int by = win->y + TITLEBAR_H + 3;
    int n = files_btn_count(state);
    for (int i = 0; i < idx && i < n; i++) bx += files_btn_defs[i].w + 6;
    *x = bx; *y = by; *w = files_btn_defs[idx].w; *h = 22;
}

int vis_files_toolbar_hit(vis_state_t *state, vis_window_t *win, int x, int y) {
    int n = files_btn_count(state);
    for (int i = 0; i < n; i++) {
        int bx, by, bw, bh;
        files_btn_rect(state, win, i, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) return i;
    }
    return -1;
}

const char *vis_files_btn_label(int idx) {
    if (idx < 0 || idx >= 6) return "";
    return files_btn_defs[idx].label;
}

void vis_files_btn_geom(vis_state_t *state, vis_window_t *win, int idx,
                        int *x, int *y, int *w, int *h) {
    files_btn_rect(state, win, idx, x, y, w, h);
}

int vis_files_btn_total(vis_state_t *state) {
    return files_btn_count(state);
}

static void files_start_prompt(vis_window_t *win, int action, const char *initial) {
    win->prompting = true;
    win->prompt_action = action;
    win->prompt_buffer[0] = '\0';
    win->prompt_pos = 0;
    if (initial) {
        strncpy(win->prompt_buffer, initial, sizeof(win->prompt_buffer) - 1);
        win->prompt_pos = strlen(win->prompt_buffer);
    }
}

static void files_cancel_prompt(vis_window_t *win) {
    win->prompting = false;
    win->prompt_buffer[0] = '\0';
    win->prompt_pos = 0;
}

static void files_apply_prompt(vis_state_t *state, vis_window_t *win) {
    vis_ssh_connection_t *conn = active_conn(state);
    if (!conn || !win->prompt_buffer[0]) { files_cancel_prompt(win); return; }

    char target[VIS_MAX_PATH_LEN];
    if (win->prompt_action == 1 || win->prompt_action == 2) {
        if (strcmp(win->path, "/") == 0)
            snprintf(target, sizeof(target), "/%s", win->prompt_buffer);
        else
            snprintf(target, sizeof(target), "%s/%s", win->path, win->prompt_buffer);
    }

    switch (win->prompt_action) {
        case 1: {
            vis_window_t *ed = vis_window_new_editor(state, target);
            if (ed) { ed->edit_dirty = true; }
            break;
        }
        case 2:
            queue_sftp_write(SFTP_WOP_MKDIR, win, conn, target, NULL);
            break;
        case 3: {
            if (win->entry_selected < 0 || win->entry_selected >= win->entry_count) break;
            vis_file_entry_t *e = &win->entries[win->entry_selected];
            char newpath[VIS_MAX_PATH_LEN];
            if (strcmp(win->path, "/") == 0)
                snprintf(newpath, sizeof(newpath), "/%s", win->prompt_buffer);
            else
                snprintf(newpath, sizeof(newpath), "%s/%s", win->path, win->prompt_buffer);
            queue_sftp_write(SFTP_WOP_RENAME, win, conn, e->path, newpath);
            break;
        }
    }
    files_cancel_prompt(win);
}

static void files_delete_selected(vis_state_t *state, vis_window_t *win) {
    if (win->entry_selected < 0 || win->entry_selected >= win->entry_count) return;
    vis_file_entry_t *e = &win->entries[win->entry_selected];
    if (strcmp(e->name, "..") == 0) return;
    vis_ssh_connection_t *conn = active_conn(state);
    if (!conn) return;
    queue_sftp_write(SFTP_WOP_RM, win, conn, e->path, NULL);
}

static void files_activate_toolbar(vis_state_t *state, vis_window_t *win, int btn) {
    switch (btn) {
        case 0: {
            char newpath[VIS_MAX_PATH_LEN];
            strncpy(newpath, win->path, sizeof(newpath) - 1);
            newpath[sizeof(newpath) - 1] = '\0';
            char *slash = strrchr(newpath, '/');
            if (slash && slash != newpath) *slash = '\0';
            else strcpy(newpath, "/");
            vis_files_refresh_async(win, active_conn(state), newpath);
            break;
        }
        case 1: vis_files_refresh_async(win, active_conn(state), NULL); break;
        case 2: if (!state->read_only) files_start_prompt(win, 1, "untitled.txt"); break;
        case 3: if (!state->read_only) files_start_prompt(win, 2, "new_folder"); break;
        case 4:
            if (!state->read_only && win->entry_selected >= 0 && win->entry_selected < win->entry_count) {
                files_start_prompt(win, 3, win->entries[win->entry_selected].name);
            }
            break;
        case 5: if (!state->read_only) files_delete_selected(state, win); break;
    }
}

/* ---------- clicks & keys ---------- */

void vis_window_handle_dblclick(vis_state_t *state, vis_window_t *win, int x, int y) {
    if (!win) return;
    if (win->kind == VIS_WIN_FILES) {
        if (y < win->y + TITLEBAR_H + 30) return;
        int body_y = win->y + TITLEBAR_H + 30 + (win->prompting ? 28 : 0);
        int row_h = state->font_height + 4;
        int row = (y - body_y) / row_h + win->entry_scroll;
        files_navigate_into(state, win, row);
    }
}

void vis_window_handle_click(vis_state_t *state, vis_window_t *win, int x, int y) {
    if (!win) return;
    vis_window_focus(state, win);
    if (win->kind == VIS_WIN_FILES) {
        int toolbar_btn = vis_files_toolbar_hit(state, win, x, y);
        if (toolbar_btn >= 0) { files_activate_toolbar(state, win, toolbar_btn); return; }
        int body_y = win->y + TITLEBAR_H + 30 + (win->prompting ? 28 : 0);
        int row_h = state->font_height + 4;
        int row = (y - body_y) / row_h + win->entry_scroll;
        if (row >= 0 && row < win->entry_count) win->entry_selected = row;
    } else if (win->kind == VIS_WIN_EDITOR) {
        int tb_y = win->y + TITLEBAR_H;
        if (y >= tb_y && y < tb_y + 26 && x >= win->x + 10 && x < win->x + 100) {
            vis_editor_save(win, active_conn(state));
            return;
        }
        /* Click in text area → position cursor. */
        int text_x = win->x + BORDER + 6;
        int text_y = win->y + TITLEBAR_H + 30;
        int line_h = state->font_height + 2;
        if (y < text_y) return;
        int clicked_row = (y - text_y) / line_h + win->edit_scroll_row;
        int clicked_col_px = x - text_x;
        if (clicked_col_px < 0) clicked_col_px = 0;
        int approx_col = state->cell_width > 0 ? clicked_col_px / state->cell_width : 0;
        /* Walk buffer to find offset of clicked (row, col). */
        size_t pos = 0;
        int row = 0;
        while (pos < win->edit_len && row < clicked_row) {
            if (win->edit_buffer[pos] == '\n') row++;
            pos++;
        }
        size_t line_start = pos;
        size_t line_end = line_start;
        while (line_end < win->edit_len && win->edit_buffer[line_end] != '\n') line_end++;
        int col = approx_col;
        if (col > (int)(line_end - line_start)) col = line_end - line_start;
        if (col < 0) col = 0;
        win->edit_cursor = line_start + col;
    } else if (win->kind == VIS_WIN_TERMINAL && win->pane) {
        int body_x = win->x + BORDER;
        int body_y = win->y + TITLEBAR_H;
        int cx = (x - body_x) / state->cell_width;
        int cy = (y - body_y) / state->cell_height;
        vis_vterm_handle_mouse(win->pane, cx, cy, 1);
    }
}

static void edit_insert(vis_window_t *win, const char *text, size_t n) {
    if (!win->edit_buffer) {
        win->edit_cap = 4096;
        win->edit_buffer = calloc(1, win->edit_cap);
    }
    if (win->edit_len + n + 1 > win->edit_cap) {
        while (win->edit_len + n + 1 > win->edit_cap) win->edit_cap *= 2;
        win->edit_buffer = realloc(win->edit_buffer, win->edit_cap);
    }
    if (win->edit_cursor < 0) win->edit_cursor = 0;
    if ((size_t)win->edit_cursor > win->edit_len) win->edit_cursor = win->edit_len;
    memmove(win->edit_buffer + win->edit_cursor + n,
            win->edit_buffer + win->edit_cursor,
            win->edit_len - win->edit_cursor + 1);
    memcpy(win->edit_buffer + win->edit_cursor, text, n);
    win->edit_len += n;
    win->edit_cursor += n;
    win->edit_dirty = true;
}

static void edit_backspace(vis_window_t *win) {
    if (!win->edit_buffer || win->edit_cursor <= 0) return;
    memmove(win->edit_buffer + win->edit_cursor - 1,
            win->edit_buffer + win->edit_cursor,
            win->edit_len - win->edit_cursor + 1);
    win->edit_cursor--;
    win->edit_len--;
    win->edit_dirty = true;
}

static void edit_move(vis_window_t *win, int d) {
    win->edit_cursor += d;
    if (win->edit_cursor < 0) win->edit_cursor = 0;
    if ((size_t)win->edit_cursor > win->edit_len) win->edit_cursor = win->edit_len;
}

static void prompt_insert(vis_window_t *win, const char *text) {
    size_t n = strlen(text);
    if (win->prompt_pos + n >= sizeof(win->prompt_buffer) - 1) return;
    memmove(win->prompt_buffer + win->prompt_pos + n,
            win->prompt_buffer + win->prompt_pos,
            strlen(win->prompt_buffer) - win->prompt_pos + 1);
    memcpy(win->prompt_buffer + win->prompt_pos, text, n);
    win->prompt_pos += n;
}

static void prompt_backspace(vis_window_t *win) {
    if (win->prompt_pos <= 0) return;
    size_t len = strlen(win->prompt_buffer);
    memmove(win->prompt_buffer + win->prompt_pos - 1,
            win->prompt_buffer + win->prompt_pos,
            len - win->prompt_pos + 1);
    win->prompt_pos--;
}

void vis_window_handle_text(vis_state_t *state, vis_window_t *win, const char *text) {
    (void)state;
    if (!win || !text || !*text) return;
    if (win->kind == VIS_WIN_FILES && win->prompting) {
        prompt_insert(win, text);
        return;
    }
    if (win->kind == VIS_WIN_EDITOR) {
        edit_insert(win, text, strlen(text));
    } else if (win->kind == VIS_WIN_TERMINAL && win->pane) {
        vis_vterm_handle_key(win->pane, 0, 0, text);
    }
}

void vis_window_handle_key(vis_state_t *state, vis_window_t *win, SDL_Keycode key, SDL_Keymod mod) {
    if (!win) return;

    if (win->kind == VIS_WIN_FILES && win->prompting) {
        switch (key) {
            case SDLK_ESCAPE: files_cancel_prompt(win); return;
            case SDLK_RETURN: files_apply_prompt(state, win); return;
            case SDLK_BACKSPACE: prompt_backspace(win); return;
        }
        return;
    }

    if (win->kind == VIS_WIN_EDITOR) {
        if ((mod & KMOD_GUI) || (mod & KMOD_CTRL)) {
            if (key == SDLK_s) { vis_editor_save(win, active_conn(state)); return; }
            if (key == SDLK_w) { vis_window_close(state, win); return; }
        }
        switch (key) {
            case SDLK_RETURN: edit_insert(win, "\n", 1); return;
            case SDLK_BACKSPACE: edit_backspace(win); return;
            case SDLK_LEFT: edit_move(win, -1); return;
            case SDLK_RIGHT: edit_move(win, 1); return;
            case SDLK_UP:
                win->edit_scroll_row = win->edit_scroll_row > 0 ? win->edit_scroll_row - 1 : 0;
                return;
            case SDLK_DOWN: win->edit_scroll_row++; return;
            case SDLK_TAB: edit_insert(win, "    ", 4); return;
            case SDLK_HOME: win->edit_cursor = 0; return;
            case SDLK_END: win->edit_cursor = win->edit_len; return;
        }
    } else if (win->kind == VIS_WIN_FILES) {
        switch (key) {
            case SDLK_UP:
                if (win->entry_selected > 0) win->entry_selected--;
                if (win->entry_selected < win->entry_scroll) win->entry_scroll = win->entry_selected;
                return;
            case SDLK_DOWN:
                if (win->entry_selected < win->entry_count - 1) win->entry_selected++;
                return;
            case SDLK_RETURN:
                files_navigate_into(state, win, win->entry_selected);
                return;
            case SDLK_BACKSPACE: {
                char newpath[VIS_MAX_PATH_LEN];
                strncpy(newpath, win->path, sizeof(newpath) - 1);
                newpath[sizeof(newpath) - 1] = '\0';
                char *slash = strrchr(newpath, '/');
                if (slash && slash != newpath) *slash = '\0';
                else strcpy(newpath, "/");
                vis_files_refresh_async(win, active_conn(state), newpath);
                return;
            }
            case SDLK_DELETE:
                if (!state->read_only) files_delete_selected(state, win);
                return;
            case SDLK_F2:
                if (!state->read_only && win->entry_selected >= 0 && win->entry_selected < win->entry_count)
                    files_start_prompt(win, 3, win->entries[win->entry_selected].name);
                return;
            case SDLK_n:
                if (!state->read_only && (mod & (KMOD_GUI | KMOD_CTRL)))
                    files_start_prompt(win, 1, "untitled.txt");
                return;
        }
    } else if (win->kind == VIS_WIN_TERMINAL && win->pane) {
        /* Only forward special/modified keys; plain chars come via TEXTINPUT. */
        vis_vterm_handle_key(win->pane, key, mod, NULL);
    }
}

/* ---------- desktop icons & launcher ---------- */

void vis_desktop_init(vis_state_t *state) {
    state->desktop.taskbar_height = 30;
    state->desktop.titlebar_height = TITLEBAR_H;
    state->desktop.border = BORDER;
    state->desktop.hover_titlebar_btn = -1;
    state->desktop.hover_toolbar_btn = -1;
    state->desktop.hover_taskbar_btn = -1;
    state->desktop.hover_launcher_item = -1;
    state->icon_cap = 64;
    state->icons = calloc(state->icon_cap, sizeof(*state->icons));
    state->icon_count = 0;
    state->icon_focus = -1;
}

void vis_desktop_layout_icons(vis_state_t *state) {
    int col_w = 96, row_h = 90;
    int cols = (state->window_width - 40) / col_w;
    if (cols < 1) cols = 1;
    for (int i = 0; i < state->icon_count; i++) {
        int c = i % cols;
        int r = i / cols;
        state->icons[i].grid_x = 20 + c * col_w;
        state->icons[i].grid_y = 20 + r * row_h;
    }
}

void vis_desktop_refresh_icons(vis_state_t *state) {
    vis_ssh_connection_t *conn = active_conn(state);
    if (!conn) return;
    const char *home = conn->home_dir[0] ? conn->home_dir : "~";
    vis_file_entry_t *entries = NULL;
    int count = 0;
    if (vis_sftp_list(conn, home, &entries, &count) != 0) return;
    qsort(entries, count, sizeof(*entries), entry_cmp);
    if (count > state->icon_cap) {
        state->icon_cap = count;
        state->icons = realloc(state->icons, state->icon_cap * sizeof(*state->icons));
    }
    int shown = 0;
    for (int i = 0; i < count && shown < state->icon_cap; i++) {
        if (entries[i].name[0] == '.') continue;
        vis_desktop_icon_t *icon = &state->icons[shown];
        memset(icon, 0, sizeof(*icon));
        strncpy(icon->name, entries[i].name, sizeof(icon->name) - 1);
        strncpy(icon->path, entries[i].path, sizeof(icon->path) - 1);
        icon->is_dir = entries[i].is_dir;
        shown++;
    }
    state->icon_count = shown;
    free(entries);
    vis_desktop_layout_icons(state);
}

int vis_desktop_icon_at(vis_state_t *state, int x, int y) {
    for (int i = 0; i < state->icon_count; i++) {
        vis_desktop_icon_t *ic = &state->icons[i];
        if (x >= ic->grid_x && x < ic->grid_x + 72 &&
            y >= ic->grid_y && y < ic->grid_y + 80) return i;
    }
    return -1;
}

void vis_desktop_handle_icon_dblclick(vis_state_t *state, int idx) {
    if (idx < 0 || idx >= state->icon_count) return;
    vis_desktop_icon_t *ic = &state->icons[idx];
    if (ic->is_dir) vis_window_new_files(state, ic->path);
    else vis_window_new_editor(state, ic->path);
}

void vis_launcher_toggle(vis_state_t *state) {
    state->desktop.launcher_open = !state->desktop.launcher_open;
    state->desktop.launcher_w = 220;
    state->desktop.launcher_h = 150;
    state->desktop.launcher_x = 8;
    state->desktop.launcher_y = state->window_height - state->desktop.taskbar_height - state->desktop.launcher_h - 4;
}

int vis_launcher_hit(vis_state_t *state, int x, int y) {
    if (!state->desktop.launcher_open) return -1;
    int lx = state->desktop.launcher_x, ly = state->desktop.launcher_y;
    int lw = state->desktop.launcher_w, lh = state->desktop.launcher_h;
    if (x < lx || x >= lx + lw || y < ly || y >= ly + lh) return -1;
    int item_h = lh / 4;
    int item = (y - ly) / item_h;
    if (item < 0) item = 0;
    if (item > 3) item = 3;
    return item;
}

/* ---------- Context menu + run command ---------- */

static void ctx_add(vis_context_menu_t *c, const char *label, vis_ctx_action_t act) {
    if (c->item_count >= 8) return;
    strncpy(c->items[c->item_count].label, label, sizeof(c->items[0].label) - 1);
    c->items[c->item_count].action = act;
    c->item_count++;
}

static void ctx_position(vis_state_t *state, int x, int y) {
    vis_context_menu_t *c = &state->desktop.ctx;
    int row_h = 22;
    c->w = 200;
    c->h = c->item_count * row_h + 6;
    c->x = x;
    c->y = y;
    if (c->x + c->w > state->window_width) c->x = state->window_width - c->w - 4;
    int taskbar_y = state->window_height - state->desktop.taskbar_height;
    if (c->y + c->h > taskbar_y) c->y = taskbar_y - c->h - 4;
    if (c->x < 0) c->x = 0;
    if (c->y < 0) c->y = 0;
    c->hover_item = -1;
    c->open = true;
}

void vis_context_close(vis_state_t *state) {
    state->desktop.ctx.open = false;
    state->desktop.ctx.item_count = 0;
    state->desktop.ctx.target_win = NULL;
    state->desktop.ctx.target_entry = -1;
    state->desktop.ctx.target_icon = -1;
    state->desktop.ctx.target_path[0] = '\0';
}

void vis_context_open_for_files(vis_state_t *state, vis_window_t *win, int entry_idx, int x, int y) {
    vis_context_close(state);
    vis_context_menu_t *c = &state->desktop.ctx;
    c->target_win = win;
    c->target_entry = entry_idx;
    c->target_icon = -1;

    if (entry_idx >= 0 && entry_idx < win->entry_count) {
        vis_file_entry_t *e = &win->entries[entry_idx];
        strncpy(c->target_path, e->path, sizeof(c->target_path) - 1);
        c->target_is_dir = e->is_dir;
        /* permissions stored as octal string, e.g. "755" */
        int perm = (int)strtol(e->permissions, NULL, 8);
        c->target_is_exec = (perm & 0111) != 0;

        ctx_add(c, c->target_is_dir ? "Open folder" : "Open file", VIS_CTX_OPEN);
        if (!c->target_is_dir && c->target_is_exec)
            ctx_add(c, "Run", VIS_CTX_RUN);
        if (!state->read_only && strcmp(e->name, "..") != 0) {
            ctx_add(c, "Rename", VIS_CTX_RENAME);
            ctx_add(c, "Delete", VIS_CTX_DELETE);
        }
    }
    if (!state->read_only) {
        ctx_add(c, "New file", VIS_CTX_NEW_FILE);
        ctx_add(c, "New folder", VIS_CTX_NEW_FOLDER);
    }
    ctx_add(c, "Refresh", VIS_CTX_REFRESH);
    ctx_position(state, x, y);
}

void vis_context_open_for_icon(vis_state_t *state, int icon_idx, int x, int y) {
    vis_context_close(state);
    vis_context_menu_t *c = &state->desktop.ctx;
    c->target_icon = icon_idx;
    c->target_entry = -1;
    c->target_win = NULL;

    if (icon_idx >= 0 && icon_idx < state->icon_count) {
        vis_desktop_icon_t *ic = &state->icons[icon_idx];
        strncpy(c->target_path, ic->path, sizeof(c->target_path) - 1);
        c->target_is_dir = ic->is_dir;
        c->target_is_exec = false; /* unknown without stat */

        ctx_add(c, ic->is_dir ? "Open folder" : "Open file", VIS_CTX_OPEN);
        if (!ic->is_dir) ctx_add(c, "Run", VIS_CTX_RUN);
    }
    ctx_add(c, "New terminal", VIS_CTX_NEW_TERMINAL);
    ctx_add(c, "Open home", VIS_CTX_OPEN_HOME);
    ctx_add(c, "Refresh desktop", VIS_CTX_REFRESH);
    ctx_position(state, x, y);
}

void vis_context_open_for_desktop(vis_state_t *state, int x, int y) {
    vis_context_close(state);
    vis_context_menu_t *c = &state->desktop.ctx;
    ctx_add(c, "New terminal", VIS_CTX_NEW_TERMINAL);
    ctx_add(c, "Open home", VIS_CTX_OPEN_HOME);
    ctx_add(c, "Open /", VIS_CTX_PROPERTIES);
    ctx_add(c, "Refresh desktop", VIS_CTX_REFRESH);
    ctx_position(state, x, y);
}

int vis_context_hit(vis_state_t *state, int x, int y) {
    vis_context_menu_t *c = &state->desktop.ctx;
    if (!c->open) return -1;
    if (x < c->x || x >= c->x + c->w || y < c->y || y >= c->y + c->h) return -1;
    int row_h = 22;
    int item = (y - c->y - 3) / row_h;
    if (item < 0) item = 0;
    if (item >= c->item_count) item = c->item_count - 1;
    return item;
}

void vis_term_run_command(vis_state_t *state, const char *cmd) {
    vis_ssh_connection_t *conn = active_conn(state);
    if (!conn || !conn->channel) return;
    char line[VIS_MAX_PATH_LEN + 2];
    snprintf(line, sizeof(line), "%s\n", cmd);
    pthread_mutex_lock(&conn->session_lock);
    size_t sent = 0, len = strlen(line);
    while (sent < len && conn->channel) {
        ssize_t n = libssh2_channel_write(conn->channel, line + sent, len - sent);
        if (n == LIBSSH2_ERROR_EAGAIN || n == LIBSSH2_ERROR_TIMEOUT) continue;
        if (n < 0) break;
        sent += n;
    }
    pthread_mutex_unlock(&conn->session_lock);
    /* Also bring the terminal window to the front, if there is one. */
    for (vis_window_t *w = state->windows; w; w = w->next) {
        if (w->kind == VIS_WIN_TERMINAL) {
            vis_window_focus(state, w);
            break;
        }
    }
}

void vis_context_activate(vis_state_t *state, int item) {
    vis_context_menu_t *c = &state->desktop.ctx;
    if (item < 0 || item >= c->item_count) { vis_context_close(state); return; }
    vis_ctx_action_t a = c->items[item].action;
    vis_window_t *tw = c->target_win;
    char path[VIS_MAX_PATH_LEN];
    strncpy(path, c->target_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    bool is_dir = c->target_is_dir;
    vis_context_close(state);

    switch (a) {
        case VIS_CTX_OPEN:
            if (path[0]) {
                if (is_dir) vis_window_new_files(state, path);
                else vis_window_new_editor(state, path);
            }
            break;
        case VIS_CTX_RUN:
            if (path[0]) vis_term_run_command(state, path);
            break;
        case VIS_CTX_RENAME:
            if (tw && tw->kind == VIS_WIN_FILES) {
                int sel = tw->entry_selected;
                if (sel >= 0 && sel < tw->entry_count)
                    files_start_prompt(tw, 3, tw->entries[sel].name);
            }
            break;
        case VIS_CTX_DELETE:
            if (tw && tw->kind == VIS_WIN_FILES) files_delete_selected(state, tw);
            break;
        case VIS_CTX_NEW_FILE:
            if (tw && tw->kind == VIS_WIN_FILES) files_start_prompt(tw, 1, "untitled.txt");
            break;
        case VIS_CTX_NEW_FOLDER:
            if (tw && tw->kind == VIS_WIN_FILES) files_start_prompt(tw, 2, "new_folder");
            break;
        case VIS_CTX_REFRESH:
            if (tw && tw->kind == VIS_WIN_FILES) vis_files_refresh_async(tw, active_conn(state), NULL);
            else vis_desktop_refresh_icons(state);
            break;
        case VIS_CTX_NEW_TERMINAL:
            vis_window_new_terminal(state);
            break;
        case VIS_CTX_OPEN_HOME: {
            vis_ssh_connection_t *conn = active_conn(state);
            vis_window_new_files(state, conn && conn->home_dir[0] ? conn->home_dir : "/");
            break;
        }
        case VIS_CTX_PROPERTIES:
            vis_window_new_files(state, "/");
            break;
        case VIS_CTX_NONE: break;
    }
}

void vis_launcher_activate(vis_state_t *state, int item) {
    state->desktop.launcher_open = false;
    switch (item) {
        case 0: vis_window_new_terminal(state); break;
        case 1: {
            vis_ssh_connection_t *conn = active_conn(state);
            const char *home = conn && conn->home_dir[0] ? conn->home_dir : "/";
            vis_window_new_files(state, home);
            break;
        }
        case 2: vis_window_new_files(state, "/"); break;
        case 3: state->running = false; break;
    }
}

/* ---------- geometry helpers used by sdl.c ---------- */

int vis_window_dot_at(vis_window_t *win, int x, int y) {
    return titlebar_dot_at(win, x, y);
}

int vis_window_resize_at(vis_window_t *win, int x, int y) {
    return hit_resize_grip(win, x, y);
}

void vis_window_minimize(vis_state_t *state, vis_window_t *win) {
    (void)state;
    if (!win) return;
    win->minimized = true;
    win->visible = false;
}

void vis_window_toggle_max(vis_state_t *state, vis_window_t *win) {
    if (!win) return;
    window_toggle_maximize(state, win);
}
