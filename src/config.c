#include "vis.h"
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>

static void config_set_defaults_ssh(vis_ssh_config_t *ssh) {
    snprintf(ssh->host, sizeof(ssh->host), "user@example.com");
    ssh->port = 22;
    snprintf(ssh->user, sizeof(ssh->user), "user");
    snprintf(ssh->identity_file, sizeof(ssh->identity_file), "~/.ssh/id_ed25519");
    ssh->password[0] = '\0';
    ssh->use_agent = true;
    ssh->timeout = 30;
    ssh->keepalive_interval = 60;
}

static void config_set_defaults_terminal(vis_terminal_config_t *term) {
    snprintf(term->term_type, sizeof(term->term_type), "xterm-256color");
    term->cols = 120;
    term->rows = 40;
    term->scrollback_lines = 10000;
    snprintf(term->theme, sizeof(term->theme), "default");
    term->show_status_bar = true;
    term->show_tab_bar = true;
    snprintf(term->status_bar_format, sizeof(term->status_bar_format),
             "[{host}] {user}@{host}:{pwd} | {mode} | {time}");
}

static void config_set_defaults_desktop(vis_desktop_config_t *desktop) {
    desktop->enable_file_manager = true;
    desktop->file_manager_width = 30;
    snprintf(desktop->file_manager_position, sizeof(desktop->file_manager_position), "left");
    desktop->auto_refresh = true;
    desktop->refresh_interval = 5;
    desktop->enable_sftp = true;
    snprintf(desktop->local_download_dir, sizeof(desktop->local_download_dir), "~/Downloads/visual");
    snprintf(desktop->remote_upload_dir, sizeof(desktop->remote_upload_dir), "~");
    desktop->max_transfer_size = 104857600;
}

static void config_set_defaults_behavior(vis_behavior_config_t *behavior) {
    behavior->read_only_mode = false;
    behavior->clean_history_on_exit = false;
    behavior->confirm_close = true;
    behavior->reconnect_on_disconnect = false;
    behavior->reconnect_attempts = 3;
    behavior->reconnect_delay = 5;
    behavior->log_level = VIS_LOG_INFO;
    snprintf(behavior->log_file, sizeof(behavior->log_file), "~/.local/share/visual/visual.log");
}

static void config_set_defaults_keys(vis_keybindings_t *keys) {
    snprintf(keys->key_quit, sizeof(keys->key_quit), "Ctrl+q");
    snprintf(keys->key_new_tab, sizeof(keys->key_new_tab), "Ctrl+t");
    snprintf(keys->key_close_tab, sizeof(keys->key_close_tab), "Ctrl+w");
    snprintf(keys->key_next_tab, sizeof(keys->key_next_tab), "Ctrl+Tab");
    snprintf(keys->key_prev_tab, sizeof(keys->key_prev_tab), "Ctrl+Shift+Tab");
    snprintf(keys->key_split_horizontal, sizeof(keys->key_split_horizontal), "Ctrl+h");
    snprintf(keys->key_split_vertical, sizeof(keys->key_split_vertical), "Ctrl+v");
    snprintf(keys->key_toggle_file_manager, sizeof(keys->key_toggle_file_manager), "Ctrl+f");
    snprintf(keys->key_command_mode, sizeof(keys->key_command_mode), ":");
    snprintf(keys->key_search, sizeof(keys->key_search), "/");
}

void vis_config_set_defaults(vis_config_t *config) {
    config_set_defaults_ssh(&config->ssh);
    config_set_defaults_terminal(&config->terminal);
    config_set_defaults_desktop(&config->desktop);
    config_set_defaults_behavior(&config->behavior);
    config_set_defaults_keys(&config->keys);
}

static char *strdup_trim(char *str) {
    char *end;
    while (isspace(*str)) str++;
    if (*str == '\0') return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) end--;
    *(end + 1) = '\0';
    return str;
}

static char *expand_path(const char *path) {
    if (!path || path[0] != '~') return strdup(path);
    const char *home = getenv("HOME");
    if (!home) return strdup(path);
    size_t len = strlen(home) + strlen(path);
    char *expanded = malloc(len + 1);
    snprintf(expanded, len + 1, "%s%s", home, path + 1);
    return expanded;
}

static int parse_log_level(const char *str) {
    if (strcasecmp(str, "debug") == 0) return VIS_LOG_DEBUG;
    if (strcasecmp(str, "info") == 0) return VIS_LOG_INFO;
    if (strcasecmp(str, "warn") == 0) return VIS_LOG_WARN;
    if (strcasecmp(str, "error") == 0) return VIS_LOG_ERROR;
    return VIS_LOG_INFO;
}

static void parse_config_line(vis_config_t *config, char *line) {
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = '\0';
    char *key = strdup_trim(line);
    char *value = strdup_trim(eq + 1);
    
    if (value[0] == '"' && value[strlen(value) - 1] == '"') {
        value[strlen(value) - 1] = '\0';
        value++;
    }

    #define SET_STR(section, field) \
        if (strcasecmp(key, #section "_" #field) == 0) { \
            char *expanded = expand_path(value); \
            snprintf(config->section.field, sizeof(config->section.field), "%s", expanded); \
            free(expanded); \
        }

    #define SET_INT(section, field) \
        if (strcasecmp(key, #section "_" #field) == 0) { \
            config->section.field = atoi(value); \
        }

    #define SET_BOOL(section, field) \
        if (strcasecmp(key, #section "_" #field) == 0) { \
            config->section.field = (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0); \
        }

    SET_STR(ssh, host);
    SET_INT(ssh, port);
    SET_STR(ssh, user);
    SET_STR(ssh, identity_file);
    SET_STR(ssh, password);
    SET_BOOL(ssh, use_agent);
    SET_INT(ssh, timeout);
    SET_INT(ssh, keepalive_interval);

    SET_STR(terminal, term_type);
    SET_INT(terminal, cols);
    SET_INT(terminal, rows);
    SET_INT(terminal, scrollback_lines);
    SET_STR(terminal, theme);
    SET_BOOL(terminal, show_status_bar);
    SET_BOOL(terminal, show_tab_bar);
    SET_STR(terminal, status_bar_format);

    SET_BOOL(desktop, enable_file_manager);
    SET_INT(desktop, file_manager_width);
    SET_STR(desktop, file_manager_position);
    SET_BOOL(desktop, auto_refresh);
    SET_INT(desktop, refresh_interval);
    SET_BOOL(desktop, enable_sftp);
    SET_STR(desktop, local_download_dir);
    SET_STR(desktop, remote_upload_dir);
    SET_INT(desktop, max_transfer_size);

    SET_BOOL(behavior, read_only_mode);
    SET_BOOL(behavior, clean_history_on_exit);
    SET_BOOL(behavior, confirm_close);
    SET_BOOL(behavior, reconnect_on_disconnect);
    SET_INT(behavior, reconnect_attempts);
    SET_INT(behavior, reconnect_delay);
    SET_INT(behavior, log_level);
    if (strcasecmp(key, "behavior_log_level") == 0) {
        config->behavior.log_level = parse_log_level(value);
    }
    SET_STR(behavior, log_file);

    SET_STR(keys, key_quit);
    SET_STR(keys, key_new_tab);
    SET_STR(keys, key_close_tab);
    SET_STR(keys, key_next_tab);
    SET_STR(keys, key_prev_tab);
    SET_STR(keys, key_split_horizontal);
    SET_STR(keys, key_split_vertical);
    SET_STR(keys, key_toggle_file_manager);
    SET_STR(keys, key_command_mode);
    SET_STR(keys, key_search);

    #undef SET_STR
    #undef SET_INT
    #undef SET_BOOL
}

void vis_config_load(vis_config_t *config) {
    vis_config_set_defaults(config);

    char *config_path = vis_get_config_path();
    FILE *f = fopen(config_path, "r");
    free(config_path);

    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = strdup_trim(line);
        if (*trimmed == '\0' || *trimmed == '#') continue;
        parse_config_line(config, trimmed);
    }
    fclose(f);
}

void vis_config_free(vis_config_t *config) {
    (void)config;
}

char *vis_get_config_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg) {
        char *path = malloc(strlen(xdg) + strlen("/visual/config") + 1);
        snprintf(path, strlen(xdg) + strlen("/visual/config") + 1, "%s/visual/config", xdg);
        return path;
    }
    const char *home = getenv("HOME");
    if (!home) return strdup("visual/config");
    char *path = malloc(strlen(home) + strlen("/.config/visual/config") + 1);
    snprintf(path, strlen(home) + strlen("/.config/visual/config") + 1, "%s/.config/visual/config", home);
    return path;
}

char *vis_expand_path(const char *path) {
    return expand_path(path);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-s] [-h] [-r] [-c config] [user@host[:port]]\n", prog);
    fprintf(stderr, "  -s  Stop running visual process and convert to normal SSH\n");
    fprintf(stderr, "  -H  Clean visual commands from shell history\n");
    fprintf(stderr, "  -r  Read-only mode\n");
    fprintf(stderr, "  -c  Use alternate config file\n");
    fprintf(stderr, "  -h  Show this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s                               # use default_host from config\n", prog);
    fprintf(stderr, "  %s user@host.example            # connect directly\n", prog);
    fprintf(stderr, "  echo user@host.example | %s      # connect from stdin\n", prog);
}

static void apply_target_string(vis_config_t *config, const char *raw) {
    while (*raw && isspace((unsigned char)*raw)) raw++;
    if (!*raw) return;

    char target[VIS_MAX_HOST_LEN * 2];
    size_t ti = 0;
    while (raw[ti] && !isspace((unsigned char)raw[ti]) && ti < sizeof(target) - 1) {
        target[ti] = raw[ti];
        ti++;
    }
    target[ti] = '\0';
    if (ti == 0) return;

    const char *host = target;
    const char *at = strrchr(target, '@');
    if (at) {
        size_t ulen = (size_t)(at - target);
        if (ulen >= sizeof(config->ssh.user)) ulen = sizeof(config->ssh.user) - 1;
        memcpy(config->ssh.user, target, ulen);
        config->ssh.user[ulen] = '\0';
        host = at + 1;
    }

    char hostbuf[VIS_MAX_HOST_LEN];
    strncpy(hostbuf, host, sizeof(hostbuf) - 1);
    hostbuf[sizeof(hostbuf) - 1] = '\0';
    char *colon = strrchr(hostbuf, ':');
    if (colon && colon != hostbuf) {
        *colon = '\0';
        int port = atoi(colon + 1);
        if (port > 0) config->ssh.port = port;
    }
    strncpy(config->ssh.host, hostbuf, sizeof(config->ssh.host) - 1);
    config->ssh.host[sizeof(config->ssh.host) - 1] = '\0';
}

static bool read_stdin_target(vis_config_t *config) {
    if (isatty(STDIN_FILENO)) return false;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    struct timeval tv = {0, 150000};

    if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) <= 0) return false;

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    char buf[1024];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags);
    if (n <= 0) return false;
    buf[n] = '\0';

    char *line = buf;
    while (*line && isspace((unsigned char)*line)) line++;
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    if (!*line) return false;

    const char *target = line;
    if (strncmp(line, "ssh ", 4) == 0) {
        target = line + 4;
        while (*target == ' ') target++;
    }

    apply_target_string(config, target);
    return true;
}

static void stop_running_instance(void) {
    char pid_file[VIS_MAX_PATH_LEN];
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(pid_file, sizeof(pid_file), "%s/.local/share/visual/visual.pid", home);
    
    FILE *f = fopen(pid_file, "r");
    if (!f) return;
    
    pid_t pid;
    if (fscanf(f, "%d", &pid) == 1) {
        kill(pid, SIGTERM);
        vis_log(VIS_LOG_INFO, "Sent SIGTERM to running vis instance (PID %d)", pid);
    }
    fclose(f);
    unlink(pid_file);
}

void write_pid_file(void) {
    char pid_file[VIS_MAX_PATH_LEN];
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(pid_file, sizeof(pid_file), "%s/.local/share/visual/visual.pid", home);
    
    char *dir = strdup(pid_file);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(dir, 0755);
    }
    free(dir);
    
    FILE *f = fopen(pid_file, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

int vis_config_parse_args(vis_config_t *config, int argc, char **argv, bool *piped_input) {
    int opt;
    bool stop_flag = false;
    bool clean_history = false;
    while ((opt = getopt(argc, argv, "shHrc:")) != -1) {
        switch (opt) {
            case 's':
                stop_flag = true;
                break;
            case 'H':
                clean_history = true;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            case 'r':
                config->behavior.read_only_mode = true;
                break;
            case 'c':
                break;
            default:
                print_usage(argv[0]);
                return -1;
        }
    }

    if (clean_history) {
        config->behavior.clean_history_on_exit = true;
    }

    if (stop_flag) {
        stop_running_instance();
        vis_log(VIS_LOG_INFO, "Stop flag triggered, exiting");
        exit(0);
    }

    *piped_input = false;
    if (optind < argc) {
        apply_target_string(config, argv[optind]);
    } else {
        *piped_input = read_stdin_target(config);
    }
    return 0;
}

void vis_clean_history(void) {
    char *histfile = getenv("HISTFILE");
    if (!histfile) histfile = "~/.zsh_history";
    char *expanded = vis_expand_path(histfile);
    
    FILE *f = fopen(expanded, "r");
    if (!f) {
        free(expanded);
        return;
    }

    char **lines = NULL;
    size_t count = 0, capacity = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "visual ") || strcmp(line, "visual\n") == 0) continue;
        if (count >= capacity) {
            capacity = capacity ? capacity * 2 : 16;
            lines = realloc(lines, capacity * sizeof(char *));
        }
        lines[count++] = strdup(line);
    }
    fclose(f);

    f = fopen(expanded, "w");
    if (f) {
        for (size_t i = 0; i < count; i++) {
            fputs(lines[i], f);
            free(lines[i]);
        }
        fclose(f);
    }
    free(lines);
    free(expanded);
}

void unlink_pid_file(void) {
    char pid_file[VIS_MAX_PATH_LEN];
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(pid_file, sizeof(pid_file), "%s/.local/share/visual/visual.pid", home);
    unlink(pid_file);
}