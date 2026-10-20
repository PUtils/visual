#include "vis.h"

static int sftp_init_locked(vis_ssh_connection_t *conn) {
    if (conn->sftp_session) return 0;
    LIBSSH2_SFTP *s = NULL;
    for (;;) {
        s = libssh2_sftp_init(conn->session);
        if (s) break;
        int err = libssh2_session_last_errno(conn->session);
        if (err != LIBSSH2_ERROR_EAGAIN && err != LIBSSH2_ERROR_TIMEOUT) break;
    }
    if (!s) {
        vis_log(VIS_LOG_ERROR, "Failed to initialize SFTP session");
        return -1;
    }
    conn->sftp_session = s;
    vis_log(VIS_LOG_INFO, "SFTP session initialized");
    return 0;
}

int vis_sftp_init(vis_ssh_connection_t *conn) {
    if (!conn) return -1;
    pthread_mutex_lock(&conn->session_lock);
    int rc = sftp_init_locked(conn);
    pthread_mutex_unlock(&conn->session_lock);
    return rc;
}

int vis_sftp_download(vis_ssh_connection_t *conn, const char *remote_path, const char *local_path) {
    if (!conn) return -1;
    pthread_mutex_lock(&conn->session_lock);
    if (sftp_init_locked(conn) != 0) { pthread_mutex_unlock(&conn->session_lock); return -1; }

    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(
        conn->sftp_session, remote_path, LIBSSH2_FXF_READ, 0);
    if (!handle) {
        pthread_mutex_unlock(&conn->session_lock);
        vis_log(VIS_LOG_ERROR, "Failed to open remote file: %s", remote_path);
        return -1;
    }

    char *expanded_local = vis_expand_path(local_path);
    FILE *local_file = fopen(expanded_local, "wb");
    free(expanded_local);

    if (!local_file) {
        libssh2_sftp_close(handle);
        pthread_mutex_unlock(&conn->session_lock);
        vis_log(VIS_LOG_ERROR, "Failed to open local file for writing: %s", local_path);
        return -1;
    }

    char buffer[32768];
    ssize_t nread;
    size_t total = 0;
    while ((nread = libssh2_sftp_read(handle, buffer, sizeof(buffer))) > 0) {
        fwrite(buffer, 1, nread, local_file);
        total += nread;
    }

    fclose(local_file);
    libssh2_sftp_close(handle);
    pthread_mutex_unlock(&conn->session_lock);

    vis_log(VIS_LOG_INFO, "Downloaded %s (%zu bytes)", remote_path, total);
    return 0;
}

int vis_sftp_upload(vis_ssh_connection_t *conn, const char *local_path, const char *remote_path) {
    if (!conn) return -1;

    char *expanded_local = vis_expand_path(local_path);
    FILE *local_file = fopen(expanded_local, "rb");
    free(expanded_local);
    if (!local_file) {
        vis_log(VIS_LOG_ERROR, "Failed to open local file for reading: %s", local_path);
        return -1;
    }

    pthread_mutex_lock(&conn->session_lock);
    if (sftp_init_locked(conn) != 0) {
        pthread_mutex_unlock(&conn->session_lock);
        fclose(local_file);
        return -1;
    }

    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_open(
        conn->sftp_session, remote_path,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!handle) {
        pthread_mutex_unlock(&conn->session_lock);
        fclose(local_file);
        vis_log(VIS_LOG_ERROR, "Failed to open remote file for writing: %s", remote_path);
        return -1;
    }

    char buffer[32768];
    size_t nread;
    size_t total = 0;
    while ((nread = fread(buffer, 1, sizeof(buffer), local_file)) > 0) {
        ssize_t nwritten = libssh2_sftp_write(handle, buffer, nread);
        if (nwritten != (ssize_t)nread) {
            vis_log(VIS_LOG_ERROR, "Failed to write to remote file");
            break;
        }
        total += nread;
    }

    fclose(local_file);
    libssh2_sftp_close(handle);
    pthread_mutex_unlock(&conn->session_lock);
    vis_log(VIS_LOG_INFO, "Uploaded %s (%zu bytes)", remote_path, total);
    return 0;
}

int vis_sftp_list(vis_ssh_connection_t *conn, const char *path, vis_file_entry_t **entries, int *count) {
    if (!conn) return -1;
    pthread_mutex_lock(&conn->session_lock);
    if (sftp_init_locked(conn) != 0) { pthread_mutex_unlock(&conn->session_lock); return -1; }

    LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_opendir(conn->sftp_session, path);
    pthread_mutex_unlock(&conn->session_lock);
    if (!dir) {
        vis_log(VIS_LOG_ERROR, "Failed to open directory: %s", path);
        return -1;
    }

    LIBSSH2_SFTP_ATTRIBUTES attrs;
    char name[1024];
    int capacity = 64;
    *entries = calloc(capacity, sizeof(vis_file_entry_t));
    *count = 0;

    /* Lock-per-readdir: release between calls so the reader thread (and
       other main-thread SSH work) can slip in while we're walking a big
       directory. */
    for (;;) {
        pthread_mutex_lock(&conn->session_lock);
        int rc = libssh2_sftp_readdir(dir, name, sizeof(name), &attrs);
        pthread_mutex_unlock(&conn->session_lock);
        if (rc <= 0) break;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (*count >= capacity) {
            capacity *= 2;
            *entries = realloc(*entries, capacity * sizeof(vis_file_entry_t));
        }
        vis_file_entry_t *entry = &(*entries)[*count];
        strncpy(entry->name, name, sizeof(entry->name) - 1);
        if (strcmp(path, "/") == 0)
            snprintf(entry->path, sizeof(entry->path), "/%s", name);
        else
            snprintf(entry->path, sizeof(entry->path), "%s/%s", path, name);
        entry->is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                        (attrs.permissions & LIBSSH2_SFTP_S_IFDIR);
        entry->size = attrs.filesize;
        entry->mtime = attrs.mtime;
        snprintf(entry->permissions, sizeof(entry->permissions), "%lo",
                 (unsigned long)(attrs.permissions & 0777));
        strncpy(entry->owner, "user", sizeof(entry->owner) - 1);
        strncpy(entry->group, "group", sizeof(entry->group) - 1);
        (*count)++;
    }

    pthread_mutex_lock(&conn->session_lock);
    libssh2_sftp_closedir(dir);
    pthread_mutex_unlock(&conn->session_lock);
    return 0;
}

int vis_sftp_mkdir(vis_ssh_connection_t *conn, const char *path) {
    if (!conn) return -1;
    pthread_mutex_lock(&conn->session_lock);
    if (sftp_init_locked(conn) != 0) { pthread_mutex_unlock(&conn->session_lock); return -1; }
    int rc = libssh2_sftp_mkdir(conn->sftp_session, path, 0755);
    pthread_mutex_unlock(&conn->session_lock);
    if (rc) { vis_log(VIS_LOG_ERROR, "Failed to create directory: %s", path); return -1; }
    return 0;
}

int vis_sftp_rm(vis_ssh_connection_t *conn, const char *path) {
    if (!conn) return -1;
    pthread_mutex_lock(&conn->session_lock);
    if (sftp_init_locked(conn) != 0) { pthread_mutex_unlock(&conn->session_lock); return -1; }
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int rc = libssh2_sftp_stat(conn->sftp_session, path, &attrs);
    if (rc) { pthread_mutex_unlock(&conn->session_lock); return -1; }
    if (attrs.permissions & LIBSSH2_SFTP_S_IFDIR)
        rc = libssh2_sftp_rmdir(conn->sftp_session, path);
    else
        rc = libssh2_sftp_unlink(conn->sftp_session, path);
    pthread_mutex_unlock(&conn->session_lock);
    if (rc) { vis_log(VIS_LOG_ERROR, "Failed to remove: %s", path); return -1; }
    return 0;
}

int vis_sftp_rename(vis_ssh_connection_t *conn, const char *old_path, const char *new_path) {
    if (!conn) return -1;
    pthread_mutex_lock(&conn->session_lock);
    if (sftp_init_locked(conn) != 0) { pthread_mutex_unlock(&conn->session_lock); return -1; }
    int rc = libssh2_sftp_rename(conn->sftp_session, old_path, new_path);
    pthread_mutex_unlock(&conn->session_lock);
    if (rc) { vis_log(VIS_LOG_ERROR, "Failed to rename: %s -> %s", old_path, new_path); return -1; }
    return 0;
}

int vis_sftp_stat(vis_ssh_connection_t *conn, const char *path, LIBSSH2_SFTP_ATTRIBUTES *attrs) {
    if (!conn) return -1;
    pthread_mutex_lock(&conn->session_lock);
    if (sftp_init_locked(conn) != 0) { pthread_mutex_unlock(&conn->session_lock); return -1; }
    int rc = libssh2_sftp_stat(conn->sftp_session, path, attrs);
    pthread_mutex_unlock(&conn->session_lock);
    return rc;
}