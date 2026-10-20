#include "vis.h"

vis_state_t *g_state = NULL;

static void handle_signal(int sig) {
    vis_log(VIS_LOG_INFO, "Signal %d received, shutting down", sig);
    if (g_state) g_state->running = false;
}

static void setup_signals(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
}

static int start_session(vis_state_t *state) {
    vis_ssh_config_t ssh_config = state->config.ssh;
    vis_log(VIS_LOG_INFO, "Connecting to %s@%s:%d",
            ssh_config.user, ssh_config.host, ssh_config.port);

    vis_ssh_connection_t *conn = vis_ssh_connect(&ssh_config);
    if (!conn) return 1;

    if (vis_ssh_authenticate(conn, &ssh_config) != 0) {
        vis_ssh_disconnect(conn);
        return 2;
    }

    if (vis_ssh_shell(conn, state->config.terminal.cols, state->config.terminal.rows) != 0) {
        vis_ssh_disconnect(conn);
        return 3;
    }

    state->panes = calloc(1, sizeof(vis_pane_t));
    state->panes->id = 1;
    state->panes->conn = conn;
    state->panes->scrollback = *vis_scrollback_create(state->config.terminal.scrollback_lines);
    state->panes->mode = VIS_MODE_NORMAL;
    state->panes->active = true;
    state->active_pane = state->panes;
    state->pane_count = 1;
    state->next_pane_id = 2;

    vis_vterm_init_pane(state->panes, state->config.terminal.cols, state->config.terminal.rows);

    state->file_manager = *vis_file_manager_create();

    pthread_create(&conn->reader_thread, NULL, vis_ssh_reader_thread, state->panes);
    pthread_create(&conn->keepalive_thread, NULL, vis_ssh_keepalive_thread, conn);

    vis_window_new_terminal(state);
    vis_desktop_refresh_icons(state);

    state->running = true;
    return 0;
}

int main(int argc, char **argv) {
    g_state = calloc(1, sizeof(vis_state_t));
    g_state->argc = argc;
    g_state->argv = argv;
    
    vis_config_load(&g_state->config);
    
    bool piped_input = false;
    if (vis_config_parse_args(&g_state->config, argc, argv, &piped_input) != 0) {
        return 1;
    }

    g_state->piped_input = piped_input;
    g_state->read_only = g_state->config.behavior.read_only_mode;
    g_state->cleanup_history = g_state->config.behavior.clean_history_on_exit;
    
    if (g_state->cleanup_history) {
        vis_clean_history();
    }
    
    vis_log_init(g_state->config.behavior.log_file, g_state->config.behavior.log_level);
    vis_log(VIS_LOG_INFO, "vis %s starting", VIS_VERSION);
    
    write_pid_file();
    setup_signals();
    
    if (vis_sdl_init(g_state) != 0) {
        vis_log(VIS_LOG_ERROR, "Failed to initialize SDL");
        return 1;
    }
    
    int ret = start_session(g_state);
    
    if (ret == 0 && g_state->running) {
        vis_log(VIS_LOG_INFO, "Starting main event loop, running=%d", g_state->running);
        SDL_Event event;
        int frame = 0;
        int iterations = 0;
        while (g_state->running) {
            iterations++;
            if (iterations == 1 || iterations % 100 == 0) {
                vis_log(VIS_LOG_INFO, "Loop iteration %d, running=%d", iterations, g_state->running);
            }
            int events = 0;
            while (SDL_PollEvent(&event)) {
                events++;
                if (event.type == SDL_QUIT) {
                    vis_log(VIS_LOG_INFO, "Received SDL_QUIT event");
                } else if (event.type == SDL_WINDOWEVENT) {
                    vis_log(VIS_LOG_DEBUG, "Window event: %d", event.window.event);
                }
                vis_sdl_handle_event(g_state, &event);
            }
            if (events > 0) {
                vis_log(VIS_LOG_DEBUG, "Processed %d events", events);
            }
            
            /* We used to exit vis when the SSH connection flipped to
               !connected, but that fired whenever the user ran `exit` in
               the shell, killing the whole desktop. The SSH session is
               still fine for SFTP after the shell dies, so just keep going;
               the user quits via Cmd/Ctrl+Q, the window close box, or the
               launcher's Quit item. */
            vis_sdl_render(g_state);
            if (frame++ % 60 == 0) {
                vis_log(VIS_LOG_DEBUG, "Frame %d, window=%p, renderer=%p, running=%d", frame, g_state->window, g_state->renderer, g_state->running);
            }
            SDL_Delay(16);
        }
        vis_log(VIS_LOG_INFO, "Main event loop exited after %d iterations, running=%d", iterations, g_state->running);
    }
    
    vis_sdl_cleanup(g_state);
    vis_log_cleanup();
    vis_config_free(&g_state->config);
    unlink_pid_file();
    free(g_state);
    
    return ret;
}