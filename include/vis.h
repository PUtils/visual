#ifndef VIS_H
#define VIS_H

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <SDL2/SDL.h>
#include <vterm.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VIS_VERSION "0.1.0"
#define VIS_MAX_HOST_LEN 256
#define VIS_MAX_PATH_LEN 4096
#define VIS_MAX_BUFFER 8192
#define VIS_SCROLLBACK_MAX 10000

typedef enum {
    VIS_MODE_NORMAL,
    VIS_MODE_COMMAND,
    VIS_MODE_SEARCH,
    VIS_MODE_FILE_MANAGER,
    VIS_MODE_READ_ONLY
} vis_mode_t;

typedef enum {
    VIS_LOG_DEBUG,
    VIS_LOG_INFO,
    VIS_LOG_WARN,
    VIS_LOG_ERROR
} vis_log_level_t;

typedef struct {
    char host[VIS_MAX_HOST_LEN];
    int port;
    char user[VIS_MAX_HOST_LEN];
    char identity_file[VIS_MAX_PATH_LEN];
    char password[VIS_MAX_PATH_LEN];
    bool use_agent;
    int timeout;
    int keepalive_interval;
} vis_ssh_config_t;

typedef struct {
    char term_type[32];
    int cols;
    int rows;
    int scrollback_lines;
    char theme[64];
    bool show_status_bar;
    bool show_tab_bar;
    char status_bar_format[256];
    int font_size;
    char font_family[64];
} vis_terminal_config_t;

typedef struct {
    bool enable_file_manager;
    int file_manager_width;
    char file_manager_position[16];
    bool auto_refresh;
    int refresh_interval;
    bool enable_sftp;
    char local_download_dir[VIS_MAX_PATH_LEN];
    char remote_upload_dir[VIS_MAX_PATH_LEN];
    size_t max_transfer_size;
} vis_desktop_config_t;

typedef struct {
    bool read_only_mode;
    bool clean_history_on_exit;
    bool confirm_close;
    bool reconnect_on_disconnect;
    int reconnect_attempts;
    int reconnect_delay;
    vis_log_level_t log_level;
    char log_file[VIS_MAX_PATH_LEN];
} vis_behavior_config_t;

typedef struct {
    char key_quit[32];
    char key_new_tab[32];
    char key_close_tab[32];
    char key_next_tab[32];
    char key_prev_tab[32];
    char key_split_horizontal[32];
    char key_split_vertical[32];
    char key_toggle_file_manager[32];
    char key_command_mode[32];
    char key_search[32];
    char key_copy[32];
    char key_paste[32];
} vis_keybindings_t;

typedef struct {
    vis_ssh_config_t ssh;
    vis_terminal_config_t terminal;
    vis_desktop_config_t desktop;
    vis_behavior_config_t behavior;
    vis_keybindings_t keys;
} vis_config_t;

typedef struct {
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    LIBSSH2_SFTP *sftp_session;
    int sock;
    pthread_t reader_thread;
    pthread_t keepalive_thread;
    pthread_mutex_t session_lock;
    bool connected;
    bool authenticated;
    char host[VIS_MAX_HOST_LEN];
    int port;
    char username[VIS_MAX_HOST_LEN];
    char current_dir[VIS_MAX_PATH_LEN];
    char home_dir[VIS_MAX_PATH_LEN];
    int keepalive_interval;
} vis_ssh_connection_t;

typedef struct {
    char **lines;
    int capacity;
    int count;
    int start;
    int end;
} vis_scrollback_t;

typedef struct vis_pane_s {
    int id;
    int x, y, width, height;
    vis_ssh_connection_t *conn;
    vis_scrollback_t scrollback;
    vis_mode_t mode;
    char command_buffer[VIS_MAX_BUFFER];
    int command_pos;
    char search_buffer[VIS_MAX_BUFFER];
    int search_pos;
    VTerm *vterm;
    VTermScreen *screen;
    struct vis_pane_s *next;
    struct vis_pane_s *prev;
    bool active;
    bool dirty;
    int scroll_offset;      /* 0 = live, N = N lines up in scrollback */
    bool channel_closed;    /* shell exited; reader thread tore channel down */
} vis_pane_t;

typedef struct {
    char name[256];
    char path[VIS_MAX_PATH_LEN];
    bool is_dir;
    long size;
    time_t mtime;
    char permissions[16];
    char owner[64];
    char group[64];
} vis_file_entry_t;

typedef struct {
    vis_file_entry_t *entries;
    int count;
    int capacity;
    int selected;
    int scroll_offset;
    char current_path[VIS_MAX_PATH_LEN];
    bool show_hidden;
    int x, y, width, height;
    bool visible;
} vis_file_manager_t;

typedef enum {
    VIS_WIN_TERMINAL,
    VIS_WIN_FILES,
    VIS_WIN_EDITOR
} vis_window_kind_t;

typedef struct vis_window_s {
    int id;
    vis_window_kind_t kind;
    int x, y, w, h;
    int prev_x, prev_y, prev_w, prev_h;
    int min_w, min_h;
    char title[256];
    bool visible;
    bool minimized;
    bool maximized;

    vis_pane_t *pane;

    char path[VIS_MAX_PATH_LEN];
    vis_file_entry_t *entries;
    int entry_count;
    int entry_cap;
    int entry_selected;
    int entry_scroll;

    bool prompting;
    int prompt_action;
    char prompt_buffer[VIS_MAX_PATH_LEN];
    int prompt_pos;

    pthread_t refresh_thread;
    bool refresh_thread_valid;
    volatile bool refresh_in_progress;
    volatile bool refresh_done;
    volatile bool refresh_requested;
    volatile int  write_in_progress;
    char refresh_target_path[VIS_MAX_PATH_LEN];
    vis_file_entry_t *refresh_result_entries;
    int refresh_result_count;
    int refresh_result_status;

    char *edit_buffer;
    size_t edit_len;
    size_t edit_cap;
    int edit_cursor;
    int edit_scroll_row;
    bool edit_dirty;

    struct vis_window_s *next;
    struct vis_window_s *prev;
} vis_window_t;

typedef struct {
    char name[256];
    char path[VIS_MAX_PATH_LEN];
    bool is_dir;
    int grid_x, grid_y;
    bool selected;
    Uint32 last_click_ms;
} vis_desktop_icon_t;

typedef enum {
    VIS_CTX_NONE,
    VIS_CTX_OPEN,
    VIS_CTX_RENAME,
    VIS_CTX_DELETE,
    VIS_CTX_RUN,
    VIS_CTX_NEW_TERMINAL,
    VIS_CTX_NEW_FILE,
    VIS_CTX_NEW_FOLDER,
    VIS_CTX_REFRESH,
    VIS_CTX_OPEN_HOME,
    VIS_CTX_PROPERTIES
} vis_ctx_action_t;

typedef struct {
    char label[48];
    vis_ctx_action_t action;
} vis_ctx_item_t;

typedef struct {
    bool open;
    int x, y, w, h;
    vis_ctx_item_t items[8];
    int item_count;
    int hover_item;
    vis_window_t *target_win;
    int target_entry;
    int target_icon;
    char target_path[VIS_MAX_PATH_LEN];
    bool target_is_dir;
    bool target_is_exec;
} vis_context_menu_t;

typedef struct {
    int taskbar_height;
    int titlebar_height;
    int border;
    vis_window_t *dragging;
    int drag_dx, drag_dy;
    vis_window_t *resizing;
    int resize_start_w, resize_start_h, resize_start_mx, resize_start_my;
    bool launcher_open;
    int launcher_x, launcher_y, launcher_w, launcher_h;

    int mouse_x, mouse_y;
    vis_window_t *hover_win;
    int hover_titlebar_btn;
    int hover_toolbar_btn;
    int hover_taskbar_btn;
    bool hover_resize;
    bool hover_launcher_btn;
    int hover_launcher_item;

    vis_context_menu_t ctx;
} vis_desktop_t;

typedef struct {
    vis_config_t config;
    vis_pane_t *panes;
    vis_pane_t *active_pane;
    vis_file_manager_t file_manager;
    int pane_count;
    int next_pane_id;
    bool running;
    bool read_only;
    bool cleanup_history;
    bool stop_requested;
    int argc;
    char **argv;
    bool piped_input;

    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *font_texture;
    int font_width, font_height;
    int window_width, window_height;
    int cell_width, cell_height;
    int cols, rows;

    vis_desktop_t desktop;

    vis_window_t *windows;
    vis_window_t *focused;
    int next_window_id;

    vis_desktop_icon_t *icons;
    int icon_count;
    int icon_cap;
    int icon_focus;
    Uint32 last_bg_click_ms;
} vis_state_t;

extern vis_state_t *g_state;

void vis_config_load(vis_config_t *config);
void vis_config_free(vis_config_t *config);
void vis_config_set_defaults(vis_config_t *config);
int vis_config_parse_args(vis_config_t *config, int argc, char **argv, bool *piped_input);

vis_ssh_connection_t *vis_ssh_connect(const vis_ssh_config_t *config);
void vis_ssh_disconnect(vis_ssh_connection_t *conn);
int vis_ssh_authenticate(vis_ssh_connection_t *conn, const vis_ssh_config_t *config);
int vis_ssh_exec(vis_ssh_connection_t *conn, const char *command, char *output, size_t output_size);
int vis_ssh_shell(vis_ssh_connection_t *conn, int cols, int rows);
void *vis_ssh_reader_thread(void *arg);
void *vis_ssh_keepalive_thread(void *arg);

vis_scrollback_t *vis_scrollback_create(int capacity);
void vis_scrollback_destroy(vis_scrollback_t *sb);
void vis_scrollback_add(vis_scrollback_t *sb, const char *line);
void vis_scrollback_get(vis_scrollback_t *sb, int index, char *buffer, size_t size);
int vis_scrollback_count(vis_scrollback_t *sb);

vis_file_manager_t *vis_file_manager_create(void);
void vis_file_manager_destroy(vis_file_manager_t *fm);
int vis_file_manager_refresh(vis_file_manager_t *fm, vis_ssh_connection_t *conn, const char *path);
int vis_file_manager_navigate(vis_file_manager_t *fm, vis_ssh_connection_t *conn, int direction);
int vis_file_manager_enter(vis_file_manager_t *fm, vis_ssh_connection_t *conn);
int vis_file_manager_download(vis_file_manager_t *fm, vis_ssh_connection_t *conn, const char *local_dir);
int vis_file_manager_upload(vis_file_manager_t *fm, vis_ssh_connection_t *conn, const char *local_path);

int vis_sdl_init(vis_state_t *state);
void vis_sdl_cleanup(vis_state_t *state);
void vis_sdl_render(vis_state_t *state);
void vis_sdl_handle_event(vis_state_t *state, SDL_Event *event);
void vis_update_layout(vis_state_t *state);
void vis_sdl_draw_pane(vis_state_t *state, vis_pane_t *pane);
void vis_sdl_draw_file_manager(vis_state_t *state);
void vis_sdl_draw_status_bar(vis_state_t *state, vis_pane_t *pane);
void vis_sdl_draw_command_line(vis_state_t *state, vis_pane_t *pane);

int vis_vterm_init_pane(vis_pane_t *pane, int cols, int rows);
void vis_vterm_destroy_pane(vis_pane_t *pane);
void vis_vterm_handle_key(vis_pane_t *pane, SDL_Keycode key, SDL_Keymod mod, const char *text);
void vis_vterm_handle_mouse(vis_pane_t *pane, int x, int y, uint32_t buttons);
void vis_vterm_write(vis_pane_t *pane, const char *data, size_t len);
void vis_vterm_resize(vis_pane_t *pane, int cols, int rows);
void vis_vterm_damage_callback(VTermRect rect, void *user);

void vis_log(vis_log_level_t level, const char *fmt, ...);
void vis_log_init(const char *log_file, vis_log_level_t level);
void vis_log_cleanup(void);
void write_pid_file(void);
void unlink_pid_file(void);

int vis_sftp_init(vis_ssh_connection_t *conn);
int vis_sftp_download(vis_ssh_connection_t *conn, const char *remote_path, const char *local_path);
int vis_sftp_upload(vis_ssh_connection_t *conn, const char *local_path, const char *remote_path);
int vis_sftp_list(vis_ssh_connection_t *conn, const char *path, vis_file_entry_t **entries, int *count);
int vis_sftp_mkdir(vis_ssh_connection_t *conn, const char *path);
int vis_sftp_rm(vis_ssh_connection_t *conn, const char *path);
int vis_sftp_rename(vis_ssh_connection_t *conn, const char *old_path, const char *new_path);
int vis_sftp_stat(vis_ssh_connection_t *conn, const char *path, LIBSSH2_SFTP_ATTRIBUTES *attrs);

char *vis_expand_path(const char *path);
char *vis_get_config_path(void);
void vis_clean_history(void);
int vis_run_piped_mode(vis_state_t *state);
int vis_run_interactive_mode(vis_state_t *state);

void vis_ui_handle_command(vis_state_t *state, const char *cmd);
void vis_ui_handle_search(vis_pane_t *pane, const char *query);
void vis_ui_new_tab(vis_state_t *state);
void vis_ui_close_tab(vis_state_t *state);
void vis_ui_split_pane(vis_state_t *state, bool horizontal);
void vis_ui_toggle_file_manager(vis_state_t *state);

vis_window_t *vis_window_new_terminal(vis_state_t *state);
vis_window_t *vis_window_new_files(vis_state_t *state, const char *path);
vis_window_t *vis_window_new_editor(vis_state_t *state, const char *path);
void vis_window_close(vis_state_t *state, vis_window_t *win);
void vis_window_focus(vis_state_t *state, vis_window_t *win);
void vis_window_minimize(vis_state_t *state, vis_window_t *win);
void vis_window_toggle_max(vis_state_t *state, vis_window_t *win);
void vis_window_bring_to_front(vis_state_t *state, vis_window_t *win);
vis_window_t *vis_window_at(vis_state_t *state, int x, int y);
int vis_window_hit_titlebar(vis_state_t *state, vis_window_t *win, int x, int y);
int vis_window_hit_close(vis_state_t *state, vis_window_t *win, int x, int y);
int vis_window_dot_at(vis_window_t *win, int x, int y);
int vis_window_resize_at(vis_window_t *win, int x, int y);
int vis_files_toolbar_hit(vis_state_t *state, vis_window_t *win, int x, int y);
const char *vis_files_btn_label(int idx);
void vis_files_btn_geom(vis_state_t *state, vis_window_t *win, int idx, int *x, int *y, int *w, int *h);
int vis_files_btn_total(vis_state_t *state);
void vis_window_handle_dblclick(vis_state_t *state, vis_window_t *win, int x, int y);
void vis_window_handle_click(vis_state_t *state, vis_window_t *win, int x, int y);
void vis_window_handle_key(vis_state_t *state, vis_window_t *win, SDL_Keycode key, SDL_Keymod mod);
void vis_window_handle_text(vis_state_t *state, vis_window_t *win, const char *text);
void vis_window_destroy(vis_window_t *win);

int vis_editor_load(vis_window_t *win, vis_ssh_connection_t *conn);
int vis_editor_save(vis_window_t *win, vis_ssh_connection_t *conn);

void vis_files_refresh(vis_window_t *win, vis_ssh_connection_t *conn);
void vis_files_refresh_async(vis_window_t *win, vis_ssh_connection_t *conn, const char *path);
void vis_windows_tick(vis_state_t *state);

void vis_desktop_init(vis_state_t *state);
void vis_desktop_refresh_icons(vis_state_t *state);
int vis_desktop_icon_at(vis_state_t *state, int x, int y);
void vis_desktop_handle_icon_dblclick(vis_state_t *state, int idx);
void vis_desktop_layout_icons(vis_state_t *state);
void vis_launcher_toggle(vis_state_t *state);
int vis_launcher_hit(vis_state_t *state, int x, int y);
void vis_launcher_activate(vis_state_t *state, int item);

void vis_context_open_for_files(vis_state_t *state, vis_window_t *win, int entry_idx, int x, int y);
void vis_context_open_for_icon(vis_state_t *state, int icon_idx, int x, int y);
void vis_context_open_for_desktop(vis_state_t *state, int x, int y);
void vis_context_close(vis_state_t *state);
int vis_context_hit(vis_state_t *state, int x, int y);
void vis_context_activate(vis_state_t *state, int item);

void vis_term_run_command(vis_state_t *state, const char *cmd);

#endif