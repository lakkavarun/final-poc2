/**
 * @file main.c
 * @brief Authenticated CLI driver for the Telecom Subscriber Management
 *        System.
 *
 * Portability: this file (and every module it depends on) uses only
 * standard ISO C11 -- <stdio.h>, <stdlib.h>, <string.h>, <time.h>, etc.
 * There is no dependency on pthreads, termios, or any other POSIX/Linux
 * header, and the program runs single-threaded. It builds and runs with
 * any conforming C11 compiler (gcc, clang, MSVC/cl, etc.) on any platform.
 *
 * The role attached to a session (ADMIN / OPERATOR / VIEWER) determines
 * which menu actions are available -- each action is re-checked against
 * RBAC via auth_authorize() immediately before it touches the subscriber
 * DB, so a menu item being reachable is a UX nicety, not the actual
 * security boundary: auth_authorize() is that boundary.
 */
#include "common.h"
#include "logger.h"
#include "shared_string.h"
#include "subscriber.h"
#include "auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#define MODULE_NAME "MAIN"
#define SM_LOG_TAIL_LINES 50 /* how many most-recent log lines "View System Logs" shows */

static volatile sig_atomic_t g_shutdown_requested = 0;

static void handle_shutdown_signal(int sig)
{
    (void)sig;
    g_shutdown_requested = 1;
}

static void resolve_runtime_path(const char *input, char *out, size_t out_len, const char *argv0)
{
    if (input == NULL || out == NULL || out_len == 0U) {
        return;
    }
    out[0] = '\0';

    if (input[0] == '\0') {
        return;
    }

    if (input[0] == '/' || input[0] == '\\' || strchr(input, ':') != NULL) {
        (void)snprintf(out, out_len, "%s", input);
        return;
    }

    FILE *probe = fopen(input, "r");
    if (probe != NULL) {
        fclose(probe);
        (void)snprintf(out, out_len, "%s", input);
        return;
    }

    if (argv0 != NULL && argv0[0] != '\0') {
        const char *slash = strrchr(argv0, '/');
        const char *bslash = strrchr(argv0, '\\');
        const char *sep = (slash != NULL && (bslash == NULL || slash > bslash)) ? slash : bslash;
        if (sep != NULL) {
            char base_dir[512];
            size_t dir_len = (size_t)(sep - argv0);
            if (dir_len + 1U < sizeof(base_dir)) {
                memcpy(base_dir, argv0, dir_len);
                base_dir[dir_len] = '\0';

                char candidate[1024];
                if (snprintf(candidate, sizeof(candidate), "%s/%s", base_dir, input) >= 0 &&
                    (size_t)snprintf(candidate, sizeof(candidate), "%s/%s", base_dir, input) < sizeof(candidate)) {
                    FILE *candidate_probe = fopen(candidate, "r");
                    if (candidate_probe != NULL) {
                        fclose(candidate_probe);
                        if ((size_t)snprintf(out, out_len, "%s", candidate) < out_len) {
                            return;
                        }
                        out[0] = '\0';
                        return;
                    }
                }
            }
        }
    }

    if (snprintf(out, out_len, "%s", input) >= (int)out_len) {
        out[0] = '\0';
    }
}

/* ---------------- small input helpers ---------------- */

/** Read a line of input, strip the trailing newline, discard any overflow. */
static void read_line(char *buf, size_t len)
{
    if (fgets(buf, (int)len, stdin) == NULL) {
        buf[0] = '\0';
        return;
    }
    size_t got = strcspn(buf, "\r\n");
    buf[got] = '\0';
    if (got == len - 1u) {
        /* Line was longer than the buffer: drain the rest of stdin so it
         * does not leak into the next prompt. */
        int c = getchar();
        while (c != '\n' && c != EOF) {
            c = getchar();
        }
    }
}

/**
 * Read a password from the terminal.
 *
 * Standard C has no portable way to disable terminal echo (that requires
 * platform APIs: POSIX termios or Windows conio), so this build reads the
 * password as plain, visible input. This keeps the program buildable with
 * only ISO C11 on any platform. Callers should be aware the password will
 * be visible on screen.
 */
static void read_password(char *buf, size_t len)
{
    read_line(buf, len);
}

/** Parse a menu choice safely (no atoi -- avoids UB on non-numeric input). */
static long read_menu_choice(void)
{
    char raw[32];
    read_line(raw, sizeof(raw));
    char *end = NULL;
    long val = strtol(raw, &end, 10);
    if (end == raw) {
        return -1; /* not a number at all */
    }
    return val;
}

/** Parse an unsigned 64-bit id from a line the user typed. */
static uint64_t read_id(void)
{
    char raw[32];
    read_line(raw, sizeof(raw));
    return strtoull(raw, NULL, 10);
}

static void print_subscriber(const sm_subscriber_t *s)
{
    printf("  id=%-6llu imsi=%-15s msisdn=%-15s name=%-16s region=%-10s plan=%-14s status=%s\n",
           (unsigned long long)s->subscriber_id, s->imsi, s->msisdn,
           sstr_cstr(s->name), sstr_cstr(s->region), sstr_cstr(s->plan),
           sub_status_to_str(s->status));
}

/* ---------------- login / session state ---------------- */

typedef struct {
    bool         logged_in;
    sm_session_t session;
} sm_app_state_t;

/**
 * Prompt for username/password and attempt to log in, giving the user a
 * few tries before giving up (account-level lockout is still enforced
 * inside auth_login regardless of how many attempts happen here).
 */
static void action_login(sm_app_state_t *state)
{
    if (state->logged_in) {
        printf("Already logged in as '%s' (role=%s). Logout first to switch users.\n",
               state->session.user.username, sm_role_to_str(state->session.user.role));
        return;
    }

    char username[SM_AUTH_MAX_USERNAME + 1u];
    char password[128];

    for (int attempt = 0; attempt < 3; ++attempt) {
        printf("Username: "); fflush(stdout);
        read_line(username, sizeof(username));

        printf("Password: "); fflush(stdout);
        read_password(password, sizeof(password));

        sm_status_t st = auth_login(username, password, &state->session);
        memset(password, 0, sizeof(password)); /* don't leave it in memory longer than needed */

        if (st == SM_OK) {
            state->logged_in = true;
            printf("Login successful. Role: %s\n", sm_role_to_str(state->session.user.role));
            return;
        }
        if (st == SM_ERR_UNAUTHORIZED) {
            printf("Invalid credentials or account locked.\n");
        } else if (st == SM_ERR_NOT_FOUND) {
            printf("Unknown user.\n");
        } else {
            printf("Login error: %s\n", sm_status_str(st));
        }
    }
    printf("Too many failed login attempts.\n");
}

static void action_logout(sm_app_state_t *state)
{
    if (!state->logged_in) {
        printf("You are not logged in.\n");
        return;
    }
    (void)auth_logout(state->session.token);
    printf("User '%s' logged out.\n", state->session.user.username);
    memset(state, 0, sizeof(*state));
}

/** Every menu action but Login/Exit requires an active, unexpired session. */
static bool require_login(const sm_app_state_t *state)
{
    if (!state->logged_in) {
        printf("Please log in first (option 1).\n");
        return false;
    }
    return true;
}

/* ---------------- RBAC-gated subscriber actions ---------------- */

static void action_add(sm_subscriber_db_t *db, const char *token)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_SUB_ADD, &user) != SM_OK) {
        printf("Permission denied: your role cannot add subscribers.\n");
        return;
    }
    char imsi[32];
    char msisdn[32];
    char name[64];
    char region[32];
    char plan[32];

    printf("IMSI: ");    fflush(stdout); read_line(imsi, sizeof(imsi));
    printf("MSISDN: ");  fflush(stdout); read_line(msisdn, sizeof(msisdn));
    printf("Name: ");    fflush(stdout); read_line(name, sizeof(name));
    printf("Region: ");  fflush(stdout); read_line(region, sizeof(region));
    printf("Plan: ");    fflush(stdout); read_line(plan, sizeof(plan));

    uint64_t id = 0;
    sm_status_t st = subdb_add(db, imsi, msisdn, name, region, plan, SUB_STATUS_ACTIVE, &id);
    printf("add -> %s", sm_status_str(st));
    if (st == SM_OK) {
        printf(" (id=%llu)", (unsigned long long)id);
    }
    printf("\n");
    SM_LOG_INFO(MODULE_NAME, "user '%s' added subscriber (status=%s)", user.username, sm_status_str(st));
}

static void action_search(sm_subscriber_db_t *db, const char *token)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_SUB_SEARCH, &user) != SM_OK) {
        printf("Permission denied: your role cannot search subscribers.\n");
        return;
    }
    printf("Search by: 1) ID  2) IMSI  3) MSISDN\nChoice: "); fflush(stdout);
    long how = read_menu_choice();

    sm_subscriber_t *found = NULL;
    sm_status_t st;
    if (how == 2) {
        char imsi[32];
        printf("IMSI: "); fflush(stdout); read_line(imsi, sizeof(imsi));
        st = subdb_search_by_imsi(db, imsi, &found);
    } else if (how == 3) {
        char msisdn[32];
        printf("MSISDN: "); fflush(stdout); read_line(msisdn, sizeof(msisdn));
        st = subdb_search_by_msisdn(db, msisdn, &found);
    } else {
        printf("Subscriber ID: "); fflush(stdout);
        uint64_t id = read_id();
        st = subdb_search_by_id(db, id, &found);
    }

    if (st == SM_OK) {
        print_subscriber(found);
        subdb_free_one(found);
    } else {
        printf("search -> %s\n", sm_status_str(st));
    }
}

static void action_update(sm_subscriber_db_t *db, const char *token)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_SUB_UPDATE, &user) != SM_OK) {
        printf("Permission denied: your role cannot update subscribers.\n");
        return;
    }
    printf("Subscriber ID to update: "); fflush(stdout);
    uint64_t id = read_id();

    sm_subscriber_t *existing = NULL;
    if (subdb_search_by_id(db, id, &existing) != SM_OK) {
        printf("update -> %s\n", sm_status_str(SM_ERR_NOT_FOUND));
        return;
    }
    printf("Current: ");
    print_subscriber(existing);
    subdb_free_one(existing);

    char name[64];
    char region[32];
    char plan[32];
    char status_buf[16];

    printf("New name (blank = keep): ");   fflush(stdout); read_line(name, sizeof(name));
    printf("New region (blank = keep): "); fflush(stdout); read_line(region, sizeof(region));
    printf("New plan (blank = keep): ");   fflush(stdout); read_line(plan, sizeof(plan));
    printf("New status ACTIVE/SUSPENDED/TERMINATED (blank = keep): ");
    fflush(stdout); read_line(status_buf, sizeof(status_buf));

    const char *name_p   = (name[0]   != '\0') ? name   : NULL;
    const char *region_p = (region[0] != '\0') ? region : NULL;
    const char *plan_p   = (plan[0]   != '\0') ? plan   : NULL;
    sm_sub_status_t status_val = SUB_STATUS_UNKNOWN;
    const sm_sub_status_t *status_p = NULL;
    if (status_buf[0] != '\0') {
        status_val = sub_status_from_str(status_buf);
        status_p = &status_val;
    }

    sm_status_t st = subdb_update(db, id, name_p, region_p, plan_p, status_p);
    printf("update -> %s\n", sm_status_str(st));
    SM_LOG_INFO(MODULE_NAME, "user '%s' updated subscriber id=%llu (status=%s)",
                user.username, (unsigned long long)id, sm_status_str(st));
}

static void action_delete(sm_subscriber_db_t *db, const char *token)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_SUB_DELETE, &user) != SM_OK) {
        printf("Permission denied: your role cannot delete subscribers.\n");
        return;
    }
    printf("Subscriber ID to delete: "); fflush(stdout);
    uint64_t id = read_id();

    sm_status_t st = subdb_delete(db, id);
    printf("delete -> %s\n", sm_status_str(st));
    SM_LOG_INFO(MODULE_NAME, "user '%s' deleted subscriber id=%llu (status=%s)",
                user.username, (unsigned long long)id, sm_status_str(st));
}

static void action_list(sm_subscriber_db_t *db, const char *token)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_SUB_LIST, &user) != SM_OK) {
        printf("Permission denied: your role cannot list subscribers.\n");
        return;
    }
    sm_subscriber_t **all = NULL;
    size_t n = 0;
    (void)subdb_list_all(db, &all, &n);
    printf("%zu subscriber(s):\n", n);
    for (size_t i = 0; i < n; ++i) {
        print_subscriber(all[i]);
    }
    subdb_free_results(all, n);
}

/* ---------------- reports ---------------- */

static void action_report(sm_subscriber_db_t *db, const char *token)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_REPORT_GENERATE, &user) != SM_OK) {
        printf("Permission denied: your role cannot generate reports.\n");
        return;
    }

    size_t total = subdb_count(db);

    static const struct { sm_sub_status_t st; const char *label; } statuses[] = {
        { SUB_STATUS_ACTIVE,      "ACTIVE"      },
        { SUB_STATUS_SUSPENDED,   "SUSPENDED"   },
        { SUB_STATUS_TERMINATED,  "TERMINATED"  },
    };

    printf("\n===== Subscriber Report =====\n");
    printf("Total subscribers : %zu\n", total);
    printf("-- By status --\n");
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        sm_subscriber_t **arr = NULL;
        size_t cnt = 0;
        (void)subdb_search_by_status(db, statuses[i].st, &arr, &cnt);
        printf("  %-10s : %zu\n", statuses[i].label, cnt);
        subdb_free_results(arr, cnt);
    }
    printf("==============================\n");
    SM_LOG_INFO(MODULE_NAME, "user '%s' generated a subscriber report (total=%llu)",
                user.username, (unsigned long long)total);
}

/* ---------------- backup / restore ---------------- */

static void action_backup(sm_subscriber_db_t *db, const char *token)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_DB_BACKUP, &user) != SM_OK) {
        printf("Permission denied: your role cannot back up the database.\n");
        return;
    }
    char backup_path[256];
    sm_status_t st = subdb_backup(db, "data/backups", backup_path, sizeof(backup_path));
    printf("backup -> %s (%s)\n", sm_status_str(st), backup_path);
    SM_LOG_INFO(MODULE_NAME, "user '%s' triggered a backup (status=%s)", user.username, sm_status_str(st));
}

static void action_restore(sm_subscriber_db_t *db, const char *token)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_DB_RESTORE, &user) != SM_OK) {
        printf("Permission denied: your role cannot restore the database.\n");
        return;
    }
    char path[256];
    printf("Backup CSV path to restore from: "); fflush(stdout);
    read_line(path, sizeof(path));

    sm_status_t st = subdb_restore(db, path);
    printf("restore -> %s\n", sm_status_str(st));
    SM_LOG_INFO(MODULE_NAME, "user '%s' restored from '%s' (status=%s)",
                user.username, path, sm_status_str(st));
}

/* ---------------- logs ---------------- */

static void action_view_logs(const char *token)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_VIEW_LOGS, &user) != SM_OK) {
        printf("Permission denied: only admins can view system logs.\n");
        return;
    }

    FILE *f = fopen("logs/subscriber_mgmt.log", "r");
    if (f == NULL) {
        printf("No log file found yet.\n");
        return;
    }

    /* Simple ring buffer of the last SM_LOG_TAIL_LINES lines. */
    static char lines[SM_LOG_TAIL_LINES][SM_MAX_LINE_LEN];
    int count = 0;
    int next = 0;
    char buf[SM_MAX_LINE_LEN];
    while (fgets(buf, (int)sizeof(buf), f) != NULL) {
        (void)snprintf(lines[next], sizeof(lines[next]), "%s", buf);
        next = (next + 1) % SM_LOG_TAIL_LINES;
        if (count < SM_LOG_TAIL_LINES) {
            count++;
        }
    }
    fclose(f);

    printf("\n===== Last %d log line(s) =====\n", count);
    int start = (count < SM_LOG_TAIL_LINES) ? 0 : next;
    for (int i = 0; i < count; ++i) {
        int idx = (start + i) % SM_LOG_TAIL_LINES;
        printf("%s", lines[idx]);
    }
    printf("================================\n");
}

/* ---------------- statistics ---------------- */

static void action_memory_stats(sm_subscriber_db_t *db, const char *token)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_VIEW_STATS, &user) != SM_OK) {
        printf("Permission denied: your role cannot view memory statistics.\n");
        return;
    }
    sm_pool_stats_t stats;
    if (mempool_get_stats(db->pool, &stats) != SM_OK) {
        printf("Could not read memory pool statistics.\n");
        return;
    }
    printf("\n===== Memory Pool Statistics =====\n");
    printf("Block size        : %zu bytes\n", stats.block_size);
    printf("Total blocks      : %zu\n", stats.total_blocks);
    printf("Used blocks       : %zu\n", stats.used_blocks);
    printf("Peak blocks       : %zu\n", stats.peak_blocks);
    printf("Allocations       : %llu\n", (unsigned long long)stats.alloc_count);
    printf("Frees             : %llu\n", (unsigned long long)stats.free_count);
    printf("Alloc failures    : %llu\n", (unsigned long long)stats.alloc_failures);
    printf("Approx. bytes used: %zu\n", stats.block_size * stats.used_blocks);
    printf("===================================\n");
}

static void action_shared_string_stats(const char *token)
{
    sm_auth_user_t user;
    if (auth_authorize(token, SM_ACTION_VIEW_STATS, &user) != SM_OK) {
        printf("Permission denied: your role cannot view shared string statistics.\n");
        return;
    }
    sm_sstr_pool_stats_t stats;
    if (sstr_pool_get_stats(&stats) != SM_OK) {
        printf("Could not read shared string pool statistics.\n");
        return;
    }
    printf("\n===== Shared String Pool Statistics =====\n");
    printf("Distinct strings           : %zu\n", stats.distinct_strings);
    printf("Total references           : %zu\n", stats.total_references);
    printf("Bytes for distinct strings : %zu\n", stats.bytes_for_distinct_strings);
    printf("Bytes without interning    : %zu\n", stats.bytes_would_be_used_without_interning);
    if (stats.bytes_would_be_used_without_interning > stats.bytes_for_distinct_strings) {
        size_t saved = stats.bytes_would_be_used_without_interning - stats.bytes_for_distinct_strings;
        printf("Bytes saved by interning   : %zu\n", saved);
    }
    printf("===========================================\n");
}

/* ---------------- password change ---------------- */

static void action_change_password(const sm_app_state_t *state)
{
    char old_pw[128];
    char new_pw[128];
    char confirm_pw[128];

    printf("Current password: "); fflush(stdout);
    read_password(old_pw, sizeof(old_pw));
    printf("New password (min 8 chars): "); fflush(stdout);
    read_password(new_pw, sizeof(new_pw));
    printf("Confirm new password: "); fflush(stdout);
    read_password(confirm_pw, sizeof(confirm_pw));

    if (strcmp(new_pw, confirm_pw) != 0) {
        printf("New password and confirmation do not match.\n");
    } else {
        const sm_status_t st = auth_change_password(state->session.token, old_pw, new_pw);
        printf("change_password -> %s\n", sm_status_str(st));
    }

    memset(old_pw, 0, sizeof(old_pw));
    memset(new_pw, 0, sizeof(new_pw));
    memset(confirm_pw, 0, sizeof(confirm_pw));
}

/* ---------------- menu ---------------- */

static void print_menu(void)
{
    printf("\n========== Telecom Subscriber Management ==========\n");
    printf(" 1. Login\n");
    printf(" 2. Add Subscriber\n");
    printf(" 3. Search Subscriber\n");
    printf(" 4. Update Subscriber\n");
    printf(" 5. Delete Subscriber\n");
    printf(" 6. Display All Subscribers\n");
    printf(" 7. Generate Reports\n");
    printf(" 8. Backup Subscriber Data\n");
    printf(" 9. Restore Subscriber Data\n");
    printf("10. View System Logs\n");
    printf("11. Memory Usage Statistics\n");
    printf("12. Shared String Statistics\n");
    printf("13. Change Password\n");
    printf("14. Logout\n");
    printf("15. Exit\n");
    printf("=====================================================\n");
    printf("Enter your choice: ");
}

static bool run_menu(sm_subscriber_db_t *db, sm_app_state_t *state)
{
    /* If we are logged in, re-verify each loop iteration so an expired
     * session is caught immediately rather than only when an action is
     * attempted. */
    if (state->logged_in) {
        sm_auth_user_t user;
        if (auth_verify_session(state->session.token, &user) != SM_OK) {
            printf("Session expired. Please log in again.\n");
            memset(state, 0, sizeof(*state));
        }
    }

    if (g_shutdown_requested) {
        return false;
    }

    print_menu();
    fflush(stdout);
    long choice = read_menu_choice();

    if (g_shutdown_requested) {
        return false;
    }

    switch (choice) {
        case 1:  action_login(state); break;
        case 2:  if (require_login(state)) { action_add(db, state->session.token); } break;
        case 3:  if (require_login(state)) { action_search(db, state->session.token); } break;
        case 4:  if (require_login(state)) { action_update(db, state->session.token); } break;
        case 5:  if (require_login(state)) { action_delete(db, state->session.token); } break;
        case 6:  if (require_login(state)) { action_list(db, state->session.token); } break;
        case 7:  if (require_login(state)) { action_report(db, state->session.token); } break;
        case 8:  if (require_login(state)) { action_backup(db, state->session.token); } break;
        case 9:  if (require_login(state)) { action_restore(db, state->session.token); } break;
        case 10: if (require_login(state)) { action_view_logs(state->session.token); } break;
        case 11: if (require_login(state)) { action_memory_stats(db, state->session.token); } break;
        case 12: if (require_login(state)) { action_shared_string_stats(state->session.token); } break;
        case 13: if (require_login(state)) { action_change_password(state); } break;
        case 14: action_logout(state); break;
        case 15:
            printf("Exiting. Goodbye.\n");
            return false;
        default:
            printf("Unknown choice. Please enter a number from 1-15.\n");
            break;
    }
    return true;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)signal(SIGINT, handle_shutdown_signal);
    (void)signal(SIGTERM, handle_shutdown_signal);

    char log_path[512];
    char auth_path[512];
    char subscriber_path[512];
    resolve_runtime_path("logs/subscriber_mgmt.log", log_path, sizeof(log_path), (argv != NULL && argv[0] != NULL) ? argv[0] : "");
    resolve_runtime_path("data/users.csv", auth_path, sizeof(auth_path), (argv != NULL && argv[0] != NULL) ? argv[0] : "");
    resolve_runtime_path("data/subscribers.csv", subscriber_path, sizeof(subscriber_path), (argv != NULL && argv[0] != NULL) ? argv[0] : "");

    (void)logger_init(log_path, LOG_INFO, 5U * 1024U * 1024U);
    (void)sstr_pool_init(1024U);

    SM_LOG_INFO(MODULE_NAME, "=== Telecom Subscriber Management System starting ===");

    if (auth_init(auth_path) != SM_OK) {
        fprintf(stderr, "fatal: could not initialize authentication store\n");
        logger_shutdown();
        return EXIT_FAILURE;
    }

    sm_subscriber_db_t *db = NULL;
    if (subdb_create(&db) != SM_OK) {
        fprintf(stderr, "fatal: could not create subscriber database\n");
        auth_shutdown();
        logger_shutdown();
        return EXIT_FAILURE;
    }
    (void)subdb_load_csv(db, subscriber_path);

    sm_app_state_t state;
    memset(&state, 0, sizeof(state));

    bool keep_running = true;
    while (keep_running) {
        keep_running = run_menu(db, &state);
    }

    if (state.logged_in) {
        (void)auth_logout(state.session.token);
    }
    (void)subdb_save_csv(db, subscriber_path);

    subdb_destroy(db);
    auth_shutdown();

    SM_LOG_INFO(MODULE_NAME, "=== Telecom Subscriber Management System shutting down ===");
    sstr_pool_shutdown();
    logger_shutdown();
    return EXIT_SUCCESS;
}
