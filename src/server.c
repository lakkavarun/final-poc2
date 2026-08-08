/**
 * @file server.c
 * @brief Basic multi-client TCP front-end for the Subscriber Management
 *        System: thread-per-connection, capped at SM_SRV_MAX_CLIENTS
 *        concurrent sessions.
 *
 * This is a SEPARATE entry point from main.c / the single-user CLI. It does
 * not modify, replace, or depend on main.c in any way -- it links against
 * the same core objects (hash_table.c, memory_pool.c, subscriber.c,
 * auth.c, shared_string.c, sha256.c, logger.c), which are already
 * thread-safe (see the "Thread safety" notes in subscriber.h and auth.h).
 *
 * Protocol: a tiny line-oriented text protocol over TCP, one client per
 * thread. Not meant to be a production server -- it is deliberately small
 * so it is easy to read, review, and stress-test with tsan/helgrind.
 *
 * Commands (one per line, fields space-separated, trailing fields to end
 * of line where noted):
 *   LOGIN <username> <password>
 *   LOGOUT
 *   ADD <imsi> <msisdn> <status:active|suspended|terminated> <name...>
 *   SEARCH <subscriber_id>
 *   DELETE <subscriber_id>
 *   LIST
 *   QUIT
 *
 * Every command except LOGIN/QUIT requires a prior successful LOGIN on
 * that connection; RBAC is still enforced via auth_authorize() exactly as
 * it is in the CLI, so a VIEWER-role login cannot ADD/DELETE, etc.
 */
#include "common.h"
#include "logger.h"
#include "shared_string.h"
#include "subscriber.h"
#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define MODULE_NAME "SERVER"

/* Basic-level cap: at most this many client threads running at once.
 * The (N+1)th connection blocks in accept()'s caller until a slot frees
 * up -- simple backpressure instead of a full thread-pool/queue. */
#define SM_SRV_MAX_CLIENTS 10
#define SM_SRV_DEFAULT_PORT 5050
#define SM_SRV_LINE_MAX 1024

static volatile sig_atomic_t g_srv_shutdown = 0;
static sm_subscriber_db_t *g_db = NULL;
static sem_t g_client_slots; /* counting semaphore, initial value SM_SRV_MAX_CLIENTS */

static void handle_sigint(int sig)
{
    (void)sig;
    g_srv_shutdown = 1;
}

/* ---------------- per-connection I/O helpers ---------------- */

typedef struct {
    int fd;
} sm_conn_t;

static int conn_send(sm_conn_t *c, const char *fmt, ...)
{
    char buf[SM_SRV_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) { return -1; }
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1U;
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = send(c->fd, buf + sent, len - sent, 0);
        if (w <= 0) { return -1; }
        sent += (size_t)w;
    }
    return 0;
}

/* Reads a single '\n'-terminated line (CRLF tolerant). Returns line length,
 * 0 on clean EOF, -1 on error. Strips the trailing newline(s). */
static ssize_t conn_recv_line(sm_conn_t *c, char *out, size_t out_len)
{
    size_t i = 0;
    for (;;) {
        char ch;
        ssize_t r = recv(c->fd, &ch, 1, 0);
        if (r == 0) { return (i == 0) ? 0 : (ssize_t)i; }
        if (r < 0) { return -1; }
        if (ch == '\n') { break; }
        if (ch != '\r' && i + 1U < out_len) { out[i++] = ch; }
    }
    out[i] = '\0';
    return (ssize_t)i;
}

/* ---------------- command handlers ---------------- */

static void cmd_add(sm_conn_t *c, const char *token, char *args)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_SUB_ADD, &user) != SM_OK) {
        (void)conn_send(c, "ERR unauthorized\n");
        return;
    }
    char imsi[SM_MAX_IMSI_LEN + 1U] = {0};
    char msisdn[SM_MAX_MSISDN_LEN + 1U] = {0};
    char status_str[16] = {0};
    char name[SM_MAX_NAME_LEN + 1U] = {0};

    int n = sscanf(args, "%15s %15s %15s %63[^\n]",
                   imsi, msisdn, status_str, name);
    if (n < 4) {
        (void)conn_send(c, "ERR usage: ADD <imsi> <msisdn> <status> <name...>\n");
        return;
    }
    sm_sub_status_t status = sub_status_from_str(status_str);
    uint64_t new_id = 0;
    sm_status_t st = subdb_add(g_db, imsi, msisdn, name, "UNSET", "UNSET",
                                status, &new_id);
    if (st == SM_OK) {
        (void)conn_send(c, "OK id=%llu\n", (unsigned long long)new_id);
        SM_LOG_INFO(MODULE_NAME, "user '%s' added subscriber id=%llu",
                    user.username, (unsigned long long)new_id);
    } else {
        (void)conn_send(c, "ERR %s\n", sm_status_str(st));
    }
}

static void cmd_search(sm_conn_t *c, const char *token, char *args)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_SUB_SEARCH, &user) != SM_OK) {
        (void)conn_send(c, "ERR unauthorized\n");
        return;
    }
    uint64_t id = 0;
    if (sscanf(args, "%llu", (unsigned long long *)&id) != 1) {
        (void)conn_send(c, "ERR usage: SEARCH <subscriber_id>\n");
        return;
    }
    sm_subscriber_t *sub = NULL;
    sm_status_t st = subdb_search_by_id(g_db, id, &sub);
    if (st == SM_OK && sub != NULL) {
        (void)conn_send(c, "OK id=%llu imsi=%s msisdn=%s status=%s\n",
                        (unsigned long long)sub->subscriber_id, sub->imsi,
                        sub->msisdn, sub_status_to_str(sub->status));
        subdb_free_one(sub);
    } else {
        (void)conn_send(c, "ERR %s\n", sm_status_str(st));
    }
}

static void cmd_delete(sm_conn_t *c, const char *token, char *args)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_SUB_DELETE, &user) != SM_OK) {
        (void)conn_send(c, "ERR unauthorized\n");
        return;
    }
    uint64_t id = 0;
    if (sscanf(args, "%llu", (unsigned long long *)&id) != 1) {
        (void)conn_send(c, "ERR usage: DELETE <subscriber_id>\n");
        return;
    }
    sm_status_t st = subdb_delete(g_db, id);
    (void)conn_send(c, "%s %s\n", (st == SM_OK) ? "OK" : "ERR", sm_status_str(st));
    if (st == SM_OK) {
        SM_LOG_INFO(MODULE_NAME, "user '%s' deleted subscriber id=%llu",
                    user.username, (unsigned long long)id);
    }
}

static void cmd_list(sm_conn_t *c, const char *token)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_SUB_LIST, &user) != SM_OK) {
        (void)conn_send(c, "ERR unauthorized\n");
        return;
    }
    sm_subscriber_t **arr = NULL;
    size_t count = 0;
    sm_status_t st = subdb_list_all(g_db, &arr, &count);
    if (st != SM_OK) {
        (void)conn_send(c, "ERR %s\n", sm_status_str(st));
        return;
    }
    (void)conn_send(c, "OK count=%zu\n", count);
    for (size_t i = 0; i < count; i++) {
        (void)conn_send(c, "%llu %s %s %s\n",
                        (unsigned long long)arr[i]->subscriber_id,
                        arr[i]->imsi, arr[i]->msisdn,
                        sub_status_to_str(arr[i]->status));
    }
    (void)conn_send(c, "END\n");
    subdb_free_results(arr, count);
}

/* ---------------- per-client thread ---------------- */

static void *client_thread(void *arg)
{
    sm_conn_t conn = *(sm_conn_t *)arg;
    free(arg);

    char token[SM_AUTH_TOKEN_BYTES * 2u + 1u] = {0};
    bool logged_in = false;

    (void)conn_send(&conn, "OK subscriber-mgmt-server ready\n");

    char line[SM_SRV_LINE_MAX];
    for (;;) {
        ssize_t n = conn_recv_line(&conn, line, sizeof(line));
        if (n <= 0) { break; } /* client disconnected or error */

        char cmd[16] = {0};
        int consumed = 0;
        if (sscanf(line, "%15s%n", cmd, &consumed) != 1) { continue; }
        char *args = line + consumed;
        while (*args == ' ') { args++; }

        if (strcmp(cmd, "LOGIN") == 0) {
            char uname[SM_AUTH_MAX_USERNAME + 1u] = {0};
            char pass[128] = {0};
            if (sscanf(args, "%32s %127s", uname, pass) != 2) {
                (void)conn_send(&conn, "ERR usage: LOGIN <user> <pass>\n");
                continue;
            }
            sm_session_t sess;
            sm_status_t st = auth_login(uname, pass, &sess);
            if (st == SM_OK) {
                (void)snprintf(token, sizeof(token), "%s", sess.token);
                logged_in = true;
                (void)conn_send(&conn, "OK role=%s\n", sm_role_to_str(sess.user.role));
                SM_LOG_INFO(MODULE_NAME, "user '%s' logged in over TCP", uname);
            } else {
                (void)conn_send(&conn, "ERR %s\n", sm_status_str(st));
            }
        } else if (strcmp(cmd, "LOGOUT") == 0) {
            if (logged_in) { (void)auth_logout(token); logged_in = false; }
            (void)conn_send(&conn, "OK\n");
        } else if (strcmp(cmd, "QUIT") == 0) {
            (void)conn_send(&conn, "OK bye\n");
            break;
        } else if (!logged_in) {
            (void)conn_send(&conn, "ERR login required\n");
        } else if (strcmp(cmd, "ADD") == 0) {
            cmd_add(&conn, token, args);
        } else if (strcmp(cmd, "SEARCH") == 0) {
            cmd_search(&conn, token, args);
        } else if (strcmp(cmd, "DELETE") == 0) {
            cmd_delete(&conn, token, args);
        } else if (strcmp(cmd, "LIST") == 0) {
            cmd_list(&conn, token);
        } else {
            (void)conn_send(&conn, "ERR unknown command\n");
        }
    }

    if (logged_in) { (void)auth_logout(token); }
    close(conn.fd);
    (void)sem_post(&g_client_slots); /* free our slot for the next client */
    return NULL;
}

/* ---------------- accept loop ---------------- */

int main(int argc, char **argv)
{
    int port = SM_SRV_DEFAULT_PORT;
    if (argc > 1) { port = atoi(argv[1]); }

    (void)logger_init("logs/server.log", LOG_INFO, 1024u * 1024u);
    SM_LOG_INFO(MODULE_NAME, "starting on port %d, max %d concurrent clients",
                port, SM_SRV_MAX_CLIENTS);

    if (sstr_pool_init(1024U) != SM_OK) {
        fprintf(stderr, "failed to init shared string pool\n");
        return 1;
    }
    if (auth_init("data/users.csv") != SM_OK) {
        fprintf(stderr, "failed to init auth store\n");
        return 1;
    }
    if (subdb_create(&g_db) != SM_OK) {
        fprintf(stderr, "failed to create subscriber db\n");
        return 1;
    }
    (void)subdb_load_csv(g_db, "data/subscribers.csv");

    (void)sem_init(&g_client_slots, 0, SM_SRV_MAX_CLIENTS);
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int yes = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* local only, by design */
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return 1;
    }
    if (listen(listen_fd, 16) < 0) {
        perror("listen"); close(listen_fd); return 1;
    }

    printf("subscriber-mgmt server listening on 127.0.0.1:%d (max %d clients)\n",
           port, SM_SRV_MAX_CLIENTS);

    while (!g_srv_shutdown) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) { continue; }
            break;
        }

        /* Backpressure: block here (not inside the client thread) until a
         * slot is free, so we never exceed SM_SRV_MAX_CLIENTS threads. */
        sem_wait(&g_client_slots);

        sm_conn_t *conn = malloc(sizeof(sm_conn_t));
        if (conn == NULL) { close(client_fd); sem_post(&g_client_slots); continue; }
        conn->fd = client_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, conn) != 0) {
            close(client_fd);
            free(conn);
            sem_post(&g_client_slots);
            continue;
        }
        pthread_detach(tid);
    }

    close(listen_fd);
    (void)subdb_save_csv(g_db, "data/subscribers.csv");
    subdb_destroy(g_db);
    auth_shutdown();
    sstr_pool_shutdown();
    logger_shutdown();
    sem_destroy(&g_client_slots);
    SM_LOG_INFO(MODULE_NAME, "server shut down cleanly");
    return 0;
}
