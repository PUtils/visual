#include "vis.h"

static FILE *log_fp = NULL;
static vis_log_level_t current_log_level = VIS_LOG_INFO;

void vis_log_init(const char *log_file, vis_log_level_t level) {
    current_log_level = level;
    if (log_file && log_file[0]) {
        char *expanded = vis_expand_path(log_file);
        char *dir = strdup(expanded);
        char *last_slash = strrchr(dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            mkdir(dir, 0755);
        }
        free(dir);
        log_fp = fopen(expanded, "a");
        free(expanded);
    }
}

void vis_log_cleanup(void) {
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
}

void vis_log(vis_log_level_t level, const char *fmt, ...) {
    if (level < current_log_level) return;
    
    const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
    
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    char final_msg[2080];
    snprintf(final_msg, sizeof(final_msg), "[%s] [%s] %s\n", time_str, level_str[level], buffer);
    
    if (log_fp) {
        fputs(final_msg, log_fp);
        fflush(log_fp);
    }
    
    if (level >= VIS_LOG_WARN) {
        fprintf(stderr, "%s", final_msg);
    }
}