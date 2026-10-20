#include "vis.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ctype.h>

static int authenticate_key(vis_ssh_connection_t *conn, const vis_ssh_config_t *config) {
    char *pubkey = malloc(strlen(config->identity_file) + 5);
    snprintf(pubkey, strlen(config->identity_file) + 5, "%s.pub", config->identity_file);
    
    char *expanded_key = vis_expand_path(config->identity_file);
    char *expanded_pub = vis_expand_path(pubkey);
    free(pubkey);

    int rc = libssh2_userauth_publickey_fromfile(
        conn->session, config->user, expanded_pub, expanded_key, config->password);
    
    free(expanded_key);
    free(expanded_pub);
    return rc;
}

static int authenticate_password(vis_ssh_connection_t *conn, const vis_ssh_config_t *config) {
    return libssh2_userauth_password(conn->session, config->user, config->password);
}

static int authenticate_keyboard_interactive(vis_ssh_connection_t *conn, const vis_ssh_config_t *config) {
    (void)conn;
    (void)config;
    return -1;
}

vis_ssh_connection_t *vis_ssh_connect(const vis_ssh_config_t *config) {
    vis_ssh_connection_t *conn = calloc(1, sizeof(vis_ssh_connection_t));
    pthread_mutex_init(&conn->session_lock, NULL);

    struct sockaddr_in sin;
    const char *host = config->host;
    int port = config->port;

    if (strchr(host, '@')) {
        char *at = strchr(host, '@');
        strncpy(conn->username, host, at - host);
        conn->username[at - host] = '\0';
        host = at + 1;
    } else {
        strncpy(conn->username, config->user, sizeof(conn->username) - 1);
    }
    strncpy(conn->host, host, sizeof(conn->host) - 1);
    conn->port = port;

    conn->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (conn->sock == -1) {
        vis_log(VIS_LOG_ERROR, "Failed to create socket");
        free(conn);
        return NULL;
    }

    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = inet_addr(host);
    if (sin.sin_addr.s_addr == INADDR_NONE) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            vis_log(VIS_LOG_ERROR, "Failed to resolve hostname: %s", host);
            close(conn->sock);
            free(conn);
            return NULL;
        }
        memcpy(&sin.sin_addr, he->h_addr, he->h_length);
    }

    if (connect(conn->sock, (struct sockaddr *)(&sin), sizeof(sin)) != 0) {
        vis_log(VIS_LOG_ERROR, "Failed to connect to %s:%d", host, port);
        close(conn->sock);
        free(conn);
        return NULL;
    }

    conn->session = libssh2_session_init();
    if (!conn->session) {
        vis_log(VIS_LOG_ERROR, "Failed to create SSH session");
        close(conn->sock);
        free(conn);
        return NULL;
    }

    libssh2_session_set_blocking(conn->session, 1);
    libssh2_session_set_timeout(conn->session, config->timeout * 1000);

    int rc = libssh2_session_handshake(conn->session, conn->sock);
    if (rc) {
        vis_log(VIS_LOG_ERROR, "SSH handshake failed: %d", rc);
        libssh2_session_free(conn->session);
        close(conn->sock);
        free(conn);
        return NULL;
    }

    conn->connected = true;
    vis_log(VIS_LOG_INFO, "Connected to %s:%d", host, port);
    return conn;
}

int vis_ssh_authenticate(vis_ssh_connection_t *conn, const vis_ssh_config_t *config) {
    int rc = -1;
    
    if (config->identity_file[0]) {
        rc = authenticate_key(conn, config);
        if (rc == 0) {
            vis_log(VIS_LOG_INFO, "Authenticated via public key");
            conn->authenticated = true;
            return 0;
        }
    }

    if (config->password[0]) {
        rc = authenticate_password(conn, config);
        if (rc == 0) {
            vis_log(VIS_LOG_INFO, "Authenticated via password");
            conn->authenticated = true;
            return 0;
        }
    }

    rc = authenticate_keyboard_interactive(conn, config);
    if (rc == 0) {
        vis_log(VIS_LOG_INFO, "Authenticated via keyboard-interactive");
        conn->authenticated = true;
        return 0;
    }

    vis_log(VIS_LOG_ERROR, "All authentication methods failed");
    return -1;
}

static int get_remote_home(vis_ssh_connection_t *conn) {
    char output[1024];
    int rc = vis_ssh_exec(conn, "echo $HOME", output, sizeof(output));
    if (rc > 0) {
        char *newline = strchr(output, '\n');
        if (newline) *newline = '\0';
        strncpy(conn->home_dir, output, sizeof(conn->home_dir) - 1);
        vis_log(VIS_LOG_INFO, "Remote home directory: %s", conn->home_dir);
        return 0;
    }
    return -1;
}

int vis_ssh_shell(vis_ssh_connection_t *conn, int cols, int rows) {
    conn->channel = libssh2_channel_open_session(conn->session);
    if (!conn->channel) {
        vis_log(VIS_LOG_ERROR, "Failed to open channel");
        return -1;
    }

    int rc = libssh2_channel_request_pty(conn->channel, "xterm-256color");
    if (rc) {
        vis_log(VIS_LOG_WARN, "Failed to request PTY: %d", rc);
    }

    rc = libssh2_channel_request_pty_size(conn->channel, cols, rows);
    if (rc) {
        vis_log(VIS_LOG_WARN, "Failed to set PTY size: %d", rc);
    }

    rc = libssh2_channel_shell(conn->channel);
    if (rc) {
        vis_log(VIS_LOG_ERROR, "Failed to start shell: %d", rc);
        libssh2_channel_free(conn->channel);
        conn->channel = NULL;
        return -1;
    }

    get_remote_home(conn);
    strncpy(conn->current_dir, conn->home_dir, sizeof(conn->current_dir) - 1);

    vis_log(VIS_LOG_INFO, "Shell started");
    return 0;
}

int vis_ssh_exec(vis_ssh_connection_t *conn, const char *command, char *output, size_t output_size) {
    pthread_mutex_lock(&conn->session_lock);
    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(conn->session);
    if (!channel) { pthread_mutex_unlock(&conn->session_lock); return -1; }

    int rc = libssh2_channel_exec(channel, command);
    if (rc) {
        libssh2_channel_free(channel);
        pthread_mutex_unlock(&conn->session_lock);
        return -1;
    }

    size_t total = 0;
    char buffer[1024];
    ssize_t nread;
    while ((nread = libssh2_channel_read(channel, buffer, sizeof(buffer))) > 0) {
        if (total + nread < output_size) {
            memcpy(output + total, buffer, nread);
            total += nread;
        }
    }
    output[total] = '\0';

    libssh2_channel_wait_closed(channel);
    libssh2_channel_free(channel);
    pthread_mutex_unlock(&conn->session_lock);
    return total;
}

void *vis_ssh_reader_thread(void *arg) {
    vis_pane_t *pane = (vis_pane_t *)arg;
    vis_ssh_connection_t *conn = pane->conn;
    char buffer[4096];

    while (conn->connected && conn->channel) {
        /* Wait for socket data without holding the lock. */
        fd_set rfd;
        FD_ZERO(&rfd);
        FD_SET(conn->sock, &rfd);
        struct timeval tv = {0, 20 * 1000}; /* 20 ms */
        int sel = select(conn->sock + 1, &rfd, NULL, NULL, &tv);
        if (sel < 0) break;
        if (sel == 0) continue;

        /* Grab the lock, do ONE read, release. Prevents the SFTP worker
           thread and keystroke writes from waiting hundreds of ms behind a
           tight read-batch. */
        pthread_mutex_lock(&conn->session_lock);
        ssize_t nread = conn->channel
            ? libssh2_channel_read(conn->channel, buffer, sizeof(buffer) - 1) : -1;
        pthread_mutex_unlock(&conn->session_lock);

        if (nread > 0) {
            vis_vterm_write(pane, buffer, nread);
        } else if (nread == LIBSSH2_ERROR_EAGAIN || nread == LIBSSH2_ERROR_TIMEOUT) {
            continue;
        } else if (nread == 0) {
            pthread_mutex_lock(&conn->session_lock);
            int eof = conn->channel ? libssh2_channel_eof(conn->channel) : 1;
            pthread_mutex_unlock(&conn->session_lock);
            if (eof) break;
            continue;
        } else {
            vis_log(VIS_LOG_WARN, "SSH reader: nread=%zd, breaking", nread);
            break;
        }
    }

    /* Shell exited (or channel died). Tear down the channel but KEEP the
       SSH session alive so SFTP / Files / Editor keep working. The main
       loop does not exit; the pane is marked closed and shows a hint. */
    pthread_mutex_lock(&conn->session_lock);
    if (conn->channel) {
        libssh2_channel_free(conn->channel);
        conn->channel = NULL;
    }
    pthread_mutex_unlock(&conn->session_lock);
    pane->channel_closed = true;

    vis_log(VIS_LOG_INFO, "SSH reader thread exiting (shell closed)");
    return NULL;
}

void *vis_ssh_keepalive_thread(void *arg) {
    vis_ssh_connection_t *conn = (vis_ssh_connection_t *)arg;
    int next_keepalive = 0;
    while (conn->connected) {
        sleep(conn->keepalive_interval ? conn->keepalive_interval : 60);
        if (conn->connected && conn->session) {
            pthread_mutex_lock(&conn->session_lock);
            libssh2_keepalive_send(conn->session, &next_keepalive);
            pthread_mutex_unlock(&conn->session_lock);
        }
    }
    return NULL;
}

void vis_ssh_disconnect(vis_ssh_connection_t *conn) {
    if (!conn) return;
    
    conn->connected = false;
    
    if (conn->channel) {
        libssh2_channel_send_eof(conn->channel);
        libssh2_channel_wait_closed(conn->channel);
        libssh2_channel_free(conn->channel);
        conn->channel = NULL;
    }
    
    if (conn->sftp_session) {
        libssh2_sftp_shutdown(conn->sftp_session);
        conn->sftp_session = NULL;
    }
    
    if (conn->session) {
        libssh2_session_disconnect(conn->session, "Client disconnect");
        libssh2_session_free(conn->session);
        conn->session = NULL;
    }
    
    if (conn->sock >= 0) {
        close(conn->sock);
        conn->sock = -1;
    }
    
    if (conn->reader_thread) {
        pthread_join(conn->reader_thread, NULL);
    }
    if (conn->keepalive_thread) {
        pthread_join(conn->keepalive_thread, NULL);
    }
    pthread_mutex_destroy(&conn->session_lock);
    free(conn);
}