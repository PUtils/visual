#include "vis.h"

vis_scrollback_t *vis_scrollback_create(int capacity) {
    vis_scrollback_t *sb = calloc(1, sizeof(vis_scrollback_t));
    sb->capacity = capacity > 0 ? capacity : VIS_SCROLLBACK_MAX;
    sb->lines = calloc(sb->capacity, sizeof(char *));
    return sb;
}

void vis_scrollback_destroy(vis_scrollback_t *sb) {
    if (!sb) return;
    for (int i = 0; i < sb->count; i++) {
        int idx = (sb->start + i) % sb->capacity;
        free(sb->lines[idx]);
    }
    free(sb->lines);
    free(sb);
}

void vis_scrollback_add(vis_scrollback_t *sb, const char *line) {
    if (!sb || !line) return;
    
    char *copy = strdup(line);
    if (!copy) return;
    
    if (sb->count == sb->capacity) {
        free(sb->lines[sb->start]);
        sb->start = (sb->start + 1) % sb->capacity;
        sb->count--;
    }
    
    sb->end = (sb->start + sb->count) % sb->capacity;
    sb->lines[sb->end] = copy;
    sb->count++;
}

void vis_scrollback_get(vis_scrollback_t *sb, int index, char *buffer, size_t size) {
    if (!sb || index < 0 || index >= sb->count) {
        buffer[0] = '\0';
        return;
    }
    
    int idx = (sb->start + index) % sb->capacity;
    strncpy(buffer, sb->lines[idx], size - 1);
    buffer[size - 1] = '\0';
}

int vis_scrollback_count(vis_scrollback_t *sb) {
    return sb ? sb->count : 0;
}

vis_file_manager_t *vis_file_manager_create(void) {
    vis_file_manager_t *fm = calloc(1, sizeof(vis_file_manager_t));
    fm->capacity = 64;
    fm->entries = calloc(fm->capacity, sizeof(vis_file_entry_t));
    fm->selected = 0;
    fm->scroll_offset = 0;
    fm->show_hidden = false;
    fm->visible = false;
    fm->x = 0;
    fm->y = 0;
    fm->width = 0;
    fm->height = 0;
    strncpy(fm->current_path, "~", sizeof(fm->current_path) - 1);
    return fm;
}

void vis_file_manager_destroy(vis_file_manager_t *fm) {
    if (!fm) return;
    free(fm->entries);
    free(fm);
}

int vis_file_manager_refresh(vis_file_manager_t *fm, vis_ssh_connection_t *conn, const char *path) {
    if (!fm || !conn) return -1;
    
    vis_log(VIS_LOG_INFO, "Refreshing file manager: %s", path);
    vis_file_entry_t *entries = NULL;
    int count = 0;
    int rc = vis_sftp_list(conn, path, &entries, &count);
    
    if (rc != 0) {
        vis_log(VIS_LOG_ERROR, "Failed to list directory: %s", path);
        return -1;
    }
    
    vis_log(VIS_LOG_INFO, "Listed %d entries", count);
    free(fm->entries);
    fm->entries = entries;
    fm->count = count;
    fm->selected = 0;
    fm->scroll_offset = 0;
    strncpy(fm->current_path, path, sizeof(fm->current_path) - 1);
    
    return 0;
}

int vis_file_manager_navigate(vis_file_manager_t *fm, vis_ssh_connection_t *conn, int direction) {
    (void)conn;
    if (!fm) return -1;
    
    if (direction < 0) {
        if (fm->selected > 0) fm->selected--;
        if (fm->selected < fm->scroll_offset) fm->scroll_offset = fm->selected;
    } else {
        if (fm->selected < fm->count - 1) fm->selected++;
        if (fm->selected >= fm->scroll_offset + fm->height - 1) {
            fm->scroll_offset = fm->selected - fm->height + 2;
        }
    }
    return 0;
}

int vis_file_manager_enter(vis_file_manager_t *fm, vis_ssh_connection_t *conn) {
    if (!fm || !conn || fm->selected < 0 || fm->selected >= fm->count) return -1;
    
    vis_file_entry_t *entry = &fm->entries[fm->selected];
    if (!entry->is_dir) return 0;
    
    char new_path[VIS_MAX_PATH_LEN];
    if (strcmp(fm->current_path, "~") == 0 || strcmp(fm->current_path, "/") == 0) {
        snprintf(new_path, sizeof(new_path), "%s/%s", fm->current_path, entry->name);
    } else {
        snprintf(new_path, sizeof(new_path), "%s/%s", fm->current_path, entry->name);
    }
    
    return vis_file_manager_refresh(fm, conn, new_path);
}

int vis_file_manager_download(vis_file_manager_t *fm, vis_ssh_connection_t *conn, const char *local_dir) {
    if (!fm || !conn || fm->selected < 0 || fm->selected >= fm->count) return -1;
    
    vis_file_entry_t *entry = &fm->entries[fm->selected];
    if (entry->is_dir) return 0;
    
    char local_path[VIS_MAX_PATH_LEN];
    char *expanded_local = vis_expand_path(local_dir);
    snprintf(local_path, sizeof(local_path), "%s/%s", expanded_local, entry->name);
    free(expanded_local);
    
    return vis_sftp_download(conn, entry->path, local_path);
}

int vis_file_manager_upload(vis_file_manager_t *fm, vis_ssh_connection_t *conn, const char *local_path) {
    if (!fm || !conn) return -1;
    
    char remote_path[VIS_MAX_PATH_LEN];
    const char *filename = strrchr(local_path, '/');
    filename = filename ? filename + 1 : local_path;
    
    if (strcmp(fm->current_path, "~") == 0 || strcmp(fm->current_path, "/") == 0) {
        snprintf(remote_path, sizeof(remote_path), "%s/%s", fm->current_path, filename);
    } else {
        snprintf(remote_path, sizeof(remote_path), "%s/%s", fm->current_path, filename);
    }
    
    return vis_sftp_upload(conn, local_path, remote_path);
}