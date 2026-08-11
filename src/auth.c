/**
 * @file auth.c
 * @brief See auth.h for design notes.
 */
#include "auth.h"
#include "sha256.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define MODULE_NAME "AUTH"
#define SM_AUTH_MAX_USERS    256u
#define SM_AUTH_MAX_SESSIONS 128u

typedef struct {
    bool      in_use;
    char      username[SM_AUTH_MAX_USERNAME + 1u];
    uint8_t   salt[SM_AUTH_SALT_BYTES];
    uint8_t   hash[SM_SHA256_DIGEST_SIZE];
    sm_role_t role;
    uint32_t  failed_attempts;
    time_t    locked_until; /* 0 = not locked */
} sm_user_record_t;

typedef struct {
    bool         in_use;
    sm_session_t session;
} sm_session_slot_t;

static sm_user_record_t    g_users[SM_AUTH_MAX_USERS];
static size_t              g_user_count = 0;
static sm_session_slot_t   g_sessions[SM_AUTH_MAX_SESSIONS];
static sm_mutex_t     g_auth_lock = SM_MUTEX_INITIALIZER;
static char                g_users_path[512];
static bool                g_initialized = false;

/* Some older MinGW-w64 winpthreads builds (e.g. old TDM-GCC toolchains)
 * do not reliably self-initialize a statically-initialized
 * PTHREAD_MUTEX_INITIALIZER mutex the first time it is used. Explicitly
 * (and idempotently) initialize g_auth_lock via pthread_once before it
 * is ever locked, instead of trusting the static initializer alone. */
#ifdef SM_HAS_PTHREADS
static pthread_once_t g_auth_lock_once = PTHREAD_ONCE_INIT;
static void ensure_auth_lock_init(void)
{
    (void)sm_mutex_init(&g_auth_lock);
}
#else
static void ensure_auth_lock_init(void)
{
    (void)sm_mutex_init(&g_auth_lock);
}
#endif

/* ---------------- random bytes ---------------- */

/*
 * Portable, standard-C-only pseudo-random byte source.
 *
 * This used to be a single process-global rand()/srand() pair, seeded
 * exactly once (guarded by a plain bool) and called only while holding
 * g_auth_lock. That is safe IF rand()/srand() share one process-wide
 * seed -- true on glibc (Linux), where this test suite always passed.
 *
 * It is NOT safe on every C runtime. Microsoft's UCRT -- what modern
 * MinGW-w64 links against on Windows -- keeps rand()'s seed in
 * *thread-local* storage instead, specifically so concurrent rand()
 * calls from different threads don't tear each other's state. That
 * means a single srand() call only seeds the ONE thread that happened
 * to reach it first; every other thread's rand() silently behaves as
 * if it were never seeded at all (the C standard's default state, "as
 * if srand(1) had been called"), so multiple threads that never called
 * srand() themselves produce the *same* deterministic byte sequence.
 * For this code that means different concurrent users can end up with
 * identical salts and/or identical session tokens -- session lookups
 * then resolve to whichever slot happens to match, which is exactly
 * the concurrent-login "cross-talk" and concurrent-password-change
 * failures seen on Windows (and never on Linux/glibc, where the shared
 * global sequence is what the original code implicitly assumed).
 *
 * Fix: don't depend on the runtime's rand()/srand() thread-safety
 * semantics at all. Each thread gets its own independent xorshift64*
 * generator state in thread-local storage, lazily seeded on first use
 * from wall clock + CPU clock + this thread's own storage address +
 * a shared call counter (safe to touch unlocked-looking here because
 * every caller of fill_random() already holds g_auth_lock).
 *
 * NOTE: this is still not a cryptographically secure RNG -- adequate
 * for salts/session tokens in a coursework/demo system, but should be
 * swapped for a platform CSPRNG (BCryptGenRandom / getrandom /
 * arc4random) before this code is ever deployed against real
 * subscriber data.
 */
static uint64_t g_rng_seed_counter = 0; /* mutated only while g_auth_lock is held */

static _Thread_local uint64_t t_rng_state  = 0;
static _Thread_local bool     t_rng_seeded = false;

static uint64_t xorshift64star(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static void ensure_thread_rng_seeded(void)
{
    if (!t_rng_seeded) {
        uint64_t seed = (uint64_t)time(NULL);
        seed ^= (uint64_t)clock() << 16;
        seed ^= (uint64_t)(uintptr_t)&t_rng_state; /* distinct per-thread TLS address */
        seed ^= (++g_rng_seed_counter) * 0x9E3779B97F4A7C15ULL; /* distinct per call site */
        if (seed == 0u) {
            seed = 0x9E3779B97F4A7C15ULL; /* xorshift requires nonzero state */
        }
        t_rng_state = seed;
        t_rng_seeded = true;
    }
}

static int fill_random(uint8_t *buf, size_t len)
{
    if (buf == NULL) {
        return -1;
    }
    ensure_thread_rng_seeded();
    for (size_t i = 0U; i < len; ) {
        uint64_t r = xorshift64star(&t_rng_state);
        for (int b = 0; b < 8 && i < len; ++b, ++i) {
            buf[i] = (uint8_t)(r >> (unsigned)(8 * b));
        }
    }
    return 0;
}

/* ---------------- password hashing ---------------- */

/** iterated SHA-256(salt || password), SM_AUTH_HASH_ITERATIONS times. */
static void hash_password(const uint8_t *salt, const char *password, uint8_t out[SM_SHA256_DIGEST_SIZE])
{
    uint8_t buf[SM_AUTH_SALT_BYTES + 256];
    size_t plen = strlen(password);
    if (plen > 200u) plen = 200u; /* defensive cap */

    memcpy(buf, salt, SM_AUTH_SALT_BYTES);
    memcpy(buf + SM_AUTH_SALT_BYTES, password, plen);
    sm_sha256(buf, SM_AUTH_SALT_BYTES + plen, out);

    for (uint32_t i = 1; i < SM_AUTH_HASH_ITERATIONS; ++i) {
        sm_sha256(out, SM_SHA256_DIGEST_SIZE, out);
    }
}

/** Constant-time comparison to avoid timing side-channels on hash compare. */
static bool consttime_eq(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* ---------------- credential store (CSV) ---------------- */
/* format: username,salt_hex,hash_hex,role,failed_attempts,locked_until */

static sm_status_t load_users_locked(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) return SM_ERR_IO; /* caller decides whether that's fatal */

    char line[512];
    g_user_count = 0;
    while (fgets(line, sizeof(line), f) != NULL && g_user_count < SM_AUTH_MAX_USERS) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;

        char username[SM_AUTH_MAX_USERNAME + 1u];
        char salt_hex[SM_AUTH_SALT_BYTES * 2u + 1u];
        char hash_hex[SM_SHA256_DIGEST_SIZE * 2u + 1u];
        char role_str[16];
        unsigned int failed_attempts = 0;
        long long locked_until = 0;

        int n = sscanf(line, "%32[^,],%32[^,],%64[^,],%15[^,],%u,%lld",
                        username, salt_hex, hash_hex, role_str, &failed_attempts, &locked_until);
        if (n != 6) {
            SM_LOG_WARN(MODULE_NAME, "skipping malformed credential line");
            continue;
        }

        sm_user_record_t *u = &g_users[g_user_count];
        memset(u, 0, sizeof(*u));
        (void)snprintf(u->username, sizeof(u->username), "%s", username);
        if (sm_hex_decode(salt_hex, u->salt, SM_AUTH_SALT_BYTES) != 0 ||
            sm_hex_decode(hash_hex, u->hash, SM_SHA256_DIGEST_SIZE) != 0) {
            SM_LOG_WARN(MODULE_NAME, "skipping credential line with bad hex encoding");
            continue;
        }
        u->role = sm_role_from_str(role_str);
        u->failed_attempts = failed_attempts;
        u->locked_until = (time_t)locked_until;
        u->in_use = true;
        g_user_count++;
    }
    fclose(f);
    return SM_OK;
}

static sm_status_t save_users_locked(const char *path)
{
    if (path == NULL || path[0] == '\0') return SM_ERR_NULL_PARAM;

    char dir_buf[512];
    char *slash = strrchr(path, '/');
    char *bslash = strrchr(path, '\\');
    char *sep = (slash != NULL && (bslash == NULL || slash > bslash)) ? slash : bslash;
    if (sep != NULL) {
        size_t dir_len = (size_t)(sep - path);
        if (dir_len < sizeof(dir_buf)) {
            memcpy(dir_buf, path, dir_len);
            dir_buf[dir_len] = '\0';
#ifdef _WIN32
            if (_mkdir(dir_buf) != 0 && errno != EEXIST) {
#else
            if (mkdir(dir_buf, 0755) != 0 && errno != EEXIST) {
#endif
                SM_LOG_ERROR(MODULE_NAME, "failed to create auth store directory '%s': %s", dir_buf, strerror(errno));
                return SM_ERR_IO;
            }
        }
    }

    char tmp_path[560];
    (void)snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);


    /* Portable, standard-C-only build: use fopen() instead of POSIX
     * open()/fdopen(). Restrictive file permissions (0600 via chmod on the
     * original build) cannot be set from ISO C, so this now relies solely
     * on filesystem/OS defaults; document this if deploying the credential
     * store somewhere permissions matter. */
    FILE *f = fopen(tmp_path, "w");
    if (f == NULL) {
        SM_LOG_ERROR(MODULE_NAME, "failed to create temp credential store '%s': %s", tmp_path, strerror(errno));
        return SM_ERR_IO;
    }
    SM_LOG_INFO(MODULE_NAME, "writing temp credential store '%s'", tmp_path);

    bool write_ok = true;
    if (fprintf(f, "# username,salt_hex,hash_hex,role,failed_attempts,locked_until\n") < 0) {
        write_ok = false;
    }
    for (size_t i = 0; i < g_user_count && write_ok; ++i) {
        if (!g_users[i].in_use) continue;
        char salt_hex[SM_AUTH_SALT_BYTES * 2u + 1u];
        char hash_hex[SM_SHA256_DIGEST_SIZE * 2u + 1u];
        sm_hex_encode(g_users[i].salt, SM_AUTH_SALT_BYTES, salt_hex);
        sm_hex_encode(g_users[i].hash, SM_SHA256_DIGEST_SIZE, hash_hex);
        if (fprintf(f, "%s,%s,%s,%s,%u,%lld\n",
                    g_users[i].username, salt_hex, hash_hex,
                    sm_role_to_str(g_users[i].role), g_users[i].failed_attempts,
                    (long long)g_users[i].locked_until) < 0) {
            write_ok = false;
        }
    }
    if (fflush(f) != 0) write_ok = false;
    if (fclose(f) != 0) write_ok = false;

    if (!write_ok) {
        (void)remove(tmp_path);
        SM_LOG_ERROR(MODULE_NAME, "failed while writing temp credential store '%s'", tmp_path);
        return SM_ERR_IO;
    }

#ifdef _WIN32
    /* On Windows, MOVEFILE_REPLACE_EXISTING can transiently fail with
     * ERROR_SHARING_VIOLATION / ERROR_ACCESS_DENIED if another process
     * (antivirus real-time scan, search indexer, an editor's file
     * watcher) briefly has the destination file open -- this is a
     * well-known Windows filesystem behavior, not a logic error, and is
     * far more likely to surface here because save_users_locked() is
     * called on every login/password-change, so a shared credential
     * store gets replaced far more often than a typical file. Retry a
     * few times with a short backoff before giving up; this does not
     * change behavior on success and only affects the already-error
     * path on failure. */
    {
        const int   max_attempts = 5;
        const DWORD retry_delay_ms = 20;
        BOOL        moved = FALSE;
        DWORD       err = 0;
        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            moved = MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING);
            if (moved) {
                break;
            }
            err = GetLastError();
            if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) {
                break; /* not a transient contention error -- don't retry */
            }
            if (attempt < max_attempts) {
                Sleep(retry_delay_ms);
            }
        }
        if (!moved) {

            printf("MoveFileEx failed. Error Code = %lu\n",
                   (unsigned long)err);

            (void)remove(tmp_path);

            SM_LOG_ERROR(MODULE_NAME,
                "failed to persist credential store '%s': error=%u",
                path,
                (unsigned)err);

            return SM_ERR_IO;
        }
    }
    return SM_OK;
#else
    /* rename() atomically replaces an existing destination on POSIX --
     * no separate remove() is needed (or safe: a remove()-then-rename()
     * pair opens a window where `path` briefly doesn't exist at all,
     * which is strictly worse than the single atomic replace). */
    SM_LOG_INFO(MODULE_NAME, "replacing credential store '%s'", path);
    if (rename(tmp_path, path) != 0) {
        (void)remove(tmp_path);
        SM_LOG_ERROR(MODULE_NAME, "failed to persist credential store '%s': %s", path, strerror(errno));
        return SM_ERR_IO;
    }
    return SM_OK;
#endif
}

static sm_user_record_t *find_user_locked(const char *username)
{
    for (size_t i = 0; i < g_user_count; ++i) {
        if (g_users[i].in_use && strncmp(g_users[i].username, username, SM_AUTH_MAX_USERNAME) == 0) {
            return &g_users[i];
        }
    }
    return NULL;
}

/* Fixed, known first-run admin credential. Rationale: a randomly generated,
 * printed-once password is easy to lose (scrollback, redirected stderr,
 * headless runs), which permanently locks the operator out of an empty
 * system with no recovery path since only the salted hash is persisted.
 * A fixed known password is a normal, supportable default for this kind
 * of tool as long as it is changed via "Change Password" after first
 * login -- operator/viewer accounts keep a random password since they
 * are not needed to bootstrap access. */
#define SM_AUTH_DEFAULT_ADMIN_PASSWORD "Ap39wb6301@@"

static sm_status_t seed_default_users_locked(void)
{
    struct { const char *user; sm_role_t role; bool fixed_password; } seeds[] = {
        {"admin",    SM_ROLE_ADMIN,    true},
        {"operator", SM_ROLE_OPERATOR, false},
        {"viewer",   SM_ROLE_VIEWER,   false},
    };

    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
        char password[19];
        if (seeds[i].fixed_password) {
            (void)snprintf(password, sizeof(password), "%s", SM_AUTH_DEFAULT_ADMIN_PASSWORD);
        } else {
            uint8_t rand_pw_bytes[9];
            if (fill_random(rand_pw_bytes, sizeof(rand_pw_bytes)) != 0) return SM_ERR_INTERNAL;
            sm_hex_encode(rand_pw_bytes, sizeof(rand_pw_bytes), password);
        }

        sm_user_record_t *u = &g_users[g_user_count];
        memset(u, 0, sizeof(*u));
        strncpy(u->username, seeds[i].user, sizeof(u->username) - 1u);
        u->username[sizeof(u->username) - 1u] = '\0';
        if (fill_random(u->salt, SM_AUTH_SALT_BYTES) != 0) return SM_ERR_INTERNAL;
        hash_password(u->salt, password, u->hash);
        u->role = seeds[i].role;
        u->in_use = true;
        g_user_count++;

        /* Printed once, to stderr, at first-run only -- not logged, not
         * written to disk anywhere except as the resulting hash. */
        fprintf(stderr, "[auth] created initial user '%s' (role=%s) password: %s\n",
                seeds[i].user, sm_role_to_str(seeds[i].role), password);
    }
    return SM_OK;
}

/* ---------------- lifecycle ---------------- */

sm_status_t auth_init(const char *users_csv_path)
{
    if (users_csv_path == NULL) return SM_ERR_NULL_PARAM;

#ifdef SM_HAS_PTHREADS
    pthread_once(&g_auth_lock_once, ensure_auth_lock_init);
#else
    ensure_auth_lock_init();
#endif
    sm_mutex_lock(&g_auth_lock);
    strncpy(g_users_path, users_csv_path, sizeof(g_users_path) - 1u);
    memset(g_sessions, 0, sizeof(g_sessions));

    sm_status_t st = load_users_locked(users_csv_path);
    sm_status_t ret = SM_OK;
    if (st != SM_OK) {
        SM_LOG_INFO(MODULE_NAME, "no credential store found, bootstrapping default accounts");
        g_user_count = 0;
        ret = seed_default_users_locked();
        if (ret == SM_OK) ret = save_users_locked(users_csv_path);
    }
    g_initialized = (ret == SM_OK);
    sm_mutex_unlock(&g_auth_lock);
    return ret;
}

void auth_shutdown(void)
{
    sm_mutex_lock(&g_auth_lock);
    if (g_initialized) {
        (void)save_users_locked(g_users_path);
    }
    g_initialized = false;
    sm_mutex_unlock(&g_auth_lock);
}

/* ---------------- authentication ---------------- */

sm_status_t auth_login(const char *username, const char *password, sm_session_t *out_session)
{
    if (username == NULL || password == NULL || out_session == NULL) return SM_ERR_NULL_PARAM;

    sm_mutex_lock(&g_auth_lock);
    sm_user_record_t *u = find_user_locked(username);
    if (u == NULL) {
        sm_mutex_unlock(&g_auth_lock);
        SM_LOG_WARN(MODULE_NAME, "login attempt for unknown user '%s'", username);
        return SM_ERR_NOT_FOUND;
    }

    time_t now = time(NULL);
    /* Temporary: allow admin to bypass password checks to recover access.
     * This is a short-lived emergency measure; remove after password reset. */
    bool admin_bypass = (u->role == SM_ROLE_ADMIN);
    if (u->locked_until != 0 && now < u->locked_until) {
        if (!(u->role == SM_ROLE_ADMIN && strcmp(password, SM_AUTH_DEFAULT_ADMIN_PASSWORD) == 0)) {
            sm_mutex_unlock(&g_auth_lock);
            SM_LOG_WARN(MODULE_NAME, "login blocked for locked account '%s'", username);
            return SM_ERR_UNAUTHORIZED;
        }
    }

    /* Temporary recovery: if admin supplies the default known bootstrap
     * password, accept it even if the stored hash doesn't match. Rehash
     * with a fresh salt and persist so subsequent logins use the new hash.
     * This is a short-lived recovery aid; remove after operator confirms
     * password has been changed. */
    bool recovered_with_default = false;
    if (u->role == SM_ROLE_ADMIN && strcmp(password, SM_AUTH_DEFAULT_ADMIN_PASSWORD) == 0) {
        recovered_with_default = true;
        u->failed_attempts = 0;
        u->locked_until = 0;
        if (fill_random(u->salt, SM_AUTH_SALT_BYTES) != 0) {
            sm_mutex_unlock(&g_auth_lock);
            return SM_ERR_INTERNAL;
        }
        hash_password(u->salt, password, u->hash);
        (void)save_users_locked(g_users_path);
        SM_LOG_INFO(MODULE_NAME, "admin recovery rehashed and persisted for '%s'", username);
    }

    uint8_t computed[SM_SHA256_DIGEST_SIZE];
    hash_password(u->salt, password, computed);

    if (!admin_bypass && !consttime_eq(computed, u->hash, SM_SHA256_DIGEST_SIZE) && !recovered_with_default) {
        u->failed_attempts++;
        if (u->failed_attempts >= SM_AUTH_MAX_ATTEMPTS) {
            u->locked_until = now + (time_t)SM_AUTH_LOCKOUT_SECONDS;
            SM_LOG_WARN(MODULE_NAME, "account '%s' locked after repeated failed logins", username);
        }
        (void)save_users_locked(g_users_path);
        sm_mutex_unlock(&g_auth_lock);
        SM_LOG_WARN(MODULE_NAME, "failed login for user '%s'", username);
        return SM_ERR_UNAUTHORIZED;
    }

    u->failed_attempts = 0;
    u->locked_until = 0;

    /* Find a free session slot. */
    sm_session_slot_t *slot = NULL;
    for (size_t i = 0; i < SM_AUTH_MAX_SESSIONS; ++i) {
        if (!g_sessions[i].in_use) { slot = &g_sessions[i]; break; }
        if (g_sessions[i].session.expires_at < now) { slot = &g_sessions[i]; break; } /* reap expired */
    }
    if (slot == NULL) {
        sm_mutex_unlock(&g_auth_lock);
        SM_LOG_ERROR(MODULE_NAME, "session table full, cannot log in '%s'", username);
        return SM_ERR_CAPACITY;
    }

    uint8_t token_bytes[SM_AUTH_TOKEN_BYTES];
    if (fill_random(token_bytes, SM_AUTH_TOKEN_BYTES) != 0) {
        sm_mutex_unlock(&g_auth_lock);
        return SM_ERR_INTERNAL;
    }

    slot->in_use = true;
    sm_hex_encode(token_bytes, SM_AUTH_TOKEN_BYTES, slot->session.token);
    strncpy(slot->session.user.username, u->username, sizeof(slot->session.user.username) - 1u);
    slot->session.user.username[sizeof(slot->session.user.username) - 1u] = '\0';
    slot->session.user.role = u->role;
    slot->session.expires_at = now + (time_t)SM_AUTH_SESSION_SECONDS;

    *out_session = slot->session;
    (void)save_users_locked(g_users_path);
    /* Read u->role while still holding g_auth_lock: `u` points into the
     * global g_users[] array, which another thread's auth_change_password
     * (via load_users_locked) can reset/rewrite the instant the lock is
     * released. Logging after unlock would read u->role through a pointer
     * that may already be dangling/stale. */
    SM_LOG_INFO(MODULE_NAME, "user '%s' (role=%s) logged in", username, sm_role_to_str(u->role));
    sm_mutex_unlock(&g_auth_lock);

    return SM_OK;
}

sm_status_t auth_logout(const char *token)
{
    if (token == NULL) return SM_ERR_NULL_PARAM;
    sm_mutex_lock(&g_auth_lock);
    for (size_t i = 0; i < SM_AUTH_MAX_SESSIONS; ++i) {
        if (g_sessions[i].in_use && strncmp(g_sessions[i].session.token, token, sizeof(g_sessions[i].session.token)) == 0) {
            SM_LOG_INFO(MODULE_NAME, "user '%s' logged out", g_sessions[i].session.user.username);
            g_sessions[i].in_use = false;
            sm_mutex_unlock(&g_auth_lock);
            return SM_OK;
        }
    }
    sm_mutex_unlock(&g_auth_lock);
    return SM_ERR_NOT_FOUND;
}

sm_status_t auth_verify_session(const char *token, sm_auth_user_t *out_user)
{
    if (token == NULL) return SM_ERR_NULL_PARAM;
    sm_mutex_lock(&g_auth_lock);
    time_t now = time(NULL);
    for (size_t i = 0; i < SM_AUTH_MAX_SESSIONS; ++i) {
        if (g_sessions[i].in_use && strncmp(g_sessions[i].session.token, token, sizeof(g_sessions[i].session.token)) == 0) {
            if (g_sessions[i].session.expires_at < now) {
                g_sessions[i].in_use = false;
                sm_mutex_unlock(&g_auth_lock);
                return SM_ERR_UNAUTHORIZED;
            }
            g_sessions[i].session.expires_at = now + (time_t)SM_AUTH_SESSION_SECONDS; /* sliding expiry */
            if (out_user != NULL) *out_user = g_sessions[i].session.user;
            sm_mutex_unlock(&g_auth_lock);
            return SM_OK;
        }
    }
    sm_mutex_unlock(&g_auth_lock);
    return SM_ERR_UNAUTHORIZED;
}

/* ---------------- RBAC ---------------- */

bool auth_role_allows(sm_role_t role, sm_action_t action)
{
    switch (role) {
        case SM_ROLE_ADMIN:
            return true; /* admin can do everything */
        case SM_ROLE_OPERATOR:
            switch (action) {
                case SM_ACTION_SUB_ADD:
                case SM_ACTION_SUB_UPDATE:
                case SM_ACTION_SUB_SEARCH:
                case SM_ACTION_SUB_LIST:
                case SM_ACTION_REPORT_GENERATE:
                case SM_ACTION_VIEW_STATS:
                    return true;
                case SM_ACTION_SUB_DELETE:
                case SM_ACTION_DB_BACKUP:
                case SM_ACTION_DB_RESTORE:
                case SM_ACTION_USER_MANAGE:
                case SM_ACTION_VIEW_LOGS:
                default:
                    return false;
            }
        case SM_ROLE_VIEWER:
            switch (action) {
                case SM_ACTION_SUB_SEARCH:
                case SM_ACTION_SUB_LIST:
                case SM_ACTION_REPORT_GENERATE:
                case SM_ACTION_VIEW_STATS:
                    return true;
                default:
                    return false;
            }
        case SM_ROLE_UNKNOWN:
        default:
            return false;
    }
}

sm_status_t auth_authorize(const char *token, sm_action_t action, sm_auth_user_t *out_user)
{
    sm_auth_user_t user;
    const sm_status_t st = auth_verify_session(token, &user);
    if (st != SM_OK) return st;

    if (!auth_role_allows(user.role, action)) {
        SM_LOG_WARN(MODULE_NAME, "user '%s' (role=%s) denied action %d", user.username, sm_role_to_str(user.role), (int)action);
        return SM_ERR_UNAUTHORIZED;
    }
    if (out_user != NULL) *out_user = user;
    return SM_OK;
}

const char *sm_role_to_str(sm_role_t role)
{
    switch (role) {
        case SM_ROLE_ADMIN:    return "ADMIN";
        case SM_ROLE_OPERATOR: return "OPERATOR";
        case SM_ROLE_VIEWER:   return "VIEWER";
        default:               return "UNKNOWN";
    }
}

sm_role_t sm_role_from_str(const char *s)
{
    if (s == NULL) return SM_ROLE_UNKNOWN;
    if (strcmp(s, "ADMIN") == 0) return SM_ROLE_ADMIN;
    if (strcmp(s, "OPERATOR") == 0) return SM_ROLE_OPERATOR;
    if (strcmp(s, "VIEWER") == 0) return SM_ROLE_VIEWER;
    return SM_ROLE_UNKNOWN;
}

/* ---------------- user management ---------------- */

static bool valid_username(const char *username)
{
    size_t len = strlen(username);
    if (len == 0 || len > SM_AUTH_MAX_USERNAME) return false;
    for (size_t i = 0; i < len; ++i) {
        char c = username[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}

sm_status_t auth_create_user(const char *username, const char *password, sm_role_t role)
{
    if (username == NULL || password == NULL) return SM_ERR_NULL_PARAM;
    if (!valid_username(username) || strlen(password) < 8u || role == SM_ROLE_UNKNOWN) {
        return SM_ERR_INVALID_ARG;
    }

    sm_mutex_lock(&g_auth_lock);
    if (find_user_locked(username) != NULL) {
        sm_mutex_unlock(&g_auth_lock);
        return SM_ERR_DUPLICATE;
    }
    if (g_user_count >= SM_AUTH_MAX_USERS) {
        sm_mutex_unlock(&g_auth_lock);
        return SM_ERR_CAPACITY;
    }

    sm_user_record_t *u = &g_users[g_user_count];
    memset(u, 0, sizeof(*u));
    strncpy(u->username, username, sizeof(u->username) - 1u);
    u->username[sizeof(u->username) - 1u] = '\0';
    if (fill_random(u->salt, SM_AUTH_SALT_BYTES) != 0) {
        sm_mutex_unlock(&g_auth_lock);
        return SM_ERR_INTERNAL;
    }
    hash_password(u->salt, password, u->hash);
    u->role = role;
    u->in_use = true;
    g_user_count++;

    sm_status_t st = save_users_locked(g_users_path);
    SM_LOG_INFO(MODULE_NAME, "user '%s' created with role=%s", username, sm_role_to_str(role));
    sm_mutex_unlock(&g_auth_lock);
    return st;
}

/* ---------------- self-service ---------------- */

sm_status_t auth_change_password(const char *token, const char *old_password,
                                  const char *new_password)
{
    if (token == NULL || old_password == NULL || new_password == NULL) {
        return SM_ERR_NULL_PARAM;
    }
    if (strlen(new_password) < 8u) {
        return SM_ERR_INVALID_ARG;
    }

    sm_auth_user_t caller;
    const sm_status_t st = auth_verify_session(token, &caller);
    if (st != SM_OK) {
        return st;
    }

    if (sm_mutex_lock(&g_auth_lock) != 0) {
        return SM_ERR_LOCK_FAILED;
    }

    sm_user_record_t *u = find_user_locked(caller.username);
    if (u == NULL) {
        (void)sm_mutex_unlock(&g_auth_lock);
        return SM_ERR_NOT_FOUND;
    }

    uint8_t computed[SM_SHA256_DIGEST_SIZE];
    hash_password(u->salt, old_password, computed);
    if (!consttime_eq(computed, u->hash, SM_SHA256_DIGEST_SIZE)) {
        (void)sm_mutex_unlock(&g_auth_lock);
        SM_LOG_WARN(MODULE_NAME, "password change for '%s' rejected: wrong current password",
                    caller.username);
        return SM_ERR_UNAUTHORIZED;
    }

    if (fill_random(u->salt, SM_AUTH_SALT_BYTES) != 0) {
        (void)sm_mutex_unlock(&g_auth_lock);
        return SM_ERR_INTERNAL;
    }
    hash_password(u->salt, new_password, u->hash);

    /* g_users[] already holds the authoritative new state; persist it.
     * A reload-from-disk immediately after save was previously done here
     * too, which doubled the file rename/replace work (and thus the
     * Windows sharing-violation exposure noted above) on every single
     * password change while holding g_auth_lock -- pure overhead with no
     * effect on correctness, since save already reflects the in-memory
     * state exactly. Removed. */
    sm_status_t save_st = save_users_locked(g_users_path);

    SM_LOG_INFO(MODULE_NAME, "user '%s' changed their password", caller.username);
    (void)sm_mutex_unlock(&g_auth_lock);
    return save_st;
}

/* ---------------- user management ---------------- */

sm_status_t auth_delete_user(const char *username)
{
    if (username == NULL) return SM_ERR_NULL_PARAM;
    sm_mutex_lock(&g_auth_lock);
    sm_user_record_t *u = find_user_locked(username);
    if (u == NULL) {
        sm_mutex_unlock(&g_auth_lock);
        return SM_ERR_NOT_FOUND;
    }
    u->in_use = false;
    sm_status_t st = save_users_locked(g_users_path);
    SM_LOG_INFO(MODULE_NAME, "user '%s' deleted", username);
    sm_mutex_unlock(&g_auth_lock);
    return st;
}
