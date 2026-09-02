/**
 * @file auth.h
 * @brief Authentication, session management, and role-based access control
 *        (RBAC) for the subscriber management system.
 *
 * Design notes:
 *  - Passwords are never stored in plaintext. Each user has a random salt
 *    and a hash computed as iterated SHA-256(salt || password), repeated
 *    SM_AUTH_HASH_ITERATIONS times (a simple, dependency-free stand-in for
 *    PBKDF2) to slow down brute-force/offline guessing.
 *  - Users are persisted to a CSV credential store
 *    (username,salt_hex,hash_hex,role,failed_attempts,locked) with 0600
 *    permissions.
 *  - A login produces an opaque random session token with an expiry.
 *    Every privileged action must present a valid, non-expired token;
 *    auth_check_permission() maps (role, action) -> allow/deny (RBAC).
 *  - Repeated failed logins lock the account after SM_AUTH_MAX_ATTEMPTS
 *    to blunt password-guessing attacks.
 *  - All operations are thread-safe (guarded by an internal mutex).
 */
#ifndef SM_AUTH_H
#define SM_AUTH_H

#include "common.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SM_AUTH_MAX_USERNAME    32u
#define SM_AUTH_SALT_BYTES      16u
#define SM_AUTH_HASH_ITERATIONS 100000u
#define SM_AUTH_TOKEN_BYTES     32u   /* 64 hex chars */
#define SM_AUTH_MAX_ATTEMPTS    5u
#define SM_AUTH_LOCKOUT_SECONDS 300u  /* 5 minutes */
#define SM_AUTH_SESSION_SECONDS 1800u /* 30 minutes idle/absolute timeout */

typedef enum {
    SM_ROLE_ADMIN = 0,    /* full control: manage users, all subscriber ops    */
    SM_ROLE_OPERATOR,     /* day-to-day ops: add/update/search/list, no delete,
                              no backup/restore, no user management           */
    SM_ROLE_VIEWER,       /* read-only: search/list/sort only                 */
    SM_ROLE_UNKNOWN
} sm_role_t;

/** Actions gated by RBAC. Every mutating/administrative entry point in the
 *  application should map to one of these before it touches the DB. */
typedef enum {
    SM_ACTION_SUB_ADD = 0,
    SM_ACTION_SUB_UPDATE,
    SM_ACTION_SUB_DELETE,
    SM_ACTION_SUB_SEARCH,
    SM_ACTION_SUB_LIST,
    SM_ACTION_DB_BACKUP,
    SM_ACTION_DB_RESTORE,
    SM_ACTION_USER_MANAGE,
    SM_ACTION_REPORT_GENERATE, /* generate subscriber reports              */
    SM_ACTION_VIEW_LOGS,       /* view system/audit logs (admin only)      */
    SM_ACTION_VIEW_STATS       /* memory pool / shared string diagnostics  */
} sm_action_t;

typedef struct {
    char       username[SM_AUTH_MAX_USERNAME + 1u];
    sm_role_t  role;
} sm_auth_user_t;

typedef struct {
    char            token[SM_AUTH_TOKEN_BYTES * 2u + 1u];
    sm_auth_user_t  user;
    time_t          expires_at;
} sm_session_t;

/* ---------------- lifecycle ---------------- */

/** Load (or create, if missing) the credential store at path. */
sm_status_t auth_init(const char *users_csv_path);
/** Persist any pending changes and release resources. */
void auth_shutdown(void);

/* ---------------- authentication ---------------- */

/**
 * Verify username/password and, on success, open a new session.
 * @return SM_OK, SM_ERR_UNAUTHORIZED (bad credentials or locked account),
 *         SM_ERR_NOT_FOUND (unknown user).
 */
sm_status_t auth_login(const char *username, const char *password, sm_session_t *out_session);

/** Invalidate a session token early. */
sm_status_t auth_logout(const char *token);

/**
 * Validate a session token (exists, not expired). On success also refreshes
 * the idle timeout. @return SM_OK or SM_ERR_UNAUTHORIZED.
 */
sm_status_t auth_verify_session(const char *token, sm_auth_user_t *out_user);

/* ---------------- RBAC ---------------- */

/** Pure function: does this role permit this action? */
bool auth_role_allows(sm_role_t role, sm_action_t action);

/**
 * Convenience gate for entry points: verifies the session AND checks RBAC
 * for the given action in one call.
 * @return SM_OK, SM_ERR_UNAUTHORIZED (bad/expired session or role forbids it).
 */
sm_status_t auth_authorize(const char *token, sm_action_t action, sm_auth_user_t *out_user);

const char *sm_role_to_str(sm_role_t role);
sm_role_t sm_role_from_str(const char *s);

/* ---------------- user management (admin only) ---------------- */

/**
 * Create a new user. Caller must already have verified the acting session
 * has SM_ACTION_USER_MANAGE permission via auth_authorize().
 * @return SM_OK, SM_ERR_DUPLICATE, SM_ERR_INVALID_ARG.
 */
sm_status_t auth_create_user(const char *username, const char *password, sm_role_t role);

/** Remove a user. Same permission precondition as auth_create_user(). */
sm_status_t auth_delete_user(const char *username);

/* ---------------- self-service ---------------- */

/**
 * Change the password of the currently authenticated user (identified by
 * their session token). The caller must supply their current password to
 * re-prove identity before the change is accepted.
 * @return SM_OK, SM_ERR_UNAUTHORIZED (bad/expired session or wrong old
 *         password), SM_ERR_INVALID_ARG (new password too short).
 */
sm_status_t auth_change_password(const char *token, const char *old_password,
                                  const char *new_password);

#ifdef __cplusplus
}
#endif
#endif /* SM_AUTH_H */
