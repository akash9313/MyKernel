#ifndef ASTRA_KERNEL_USER_H
#define ASTRA_KERNEL_USER_H

#include <kernel/types.h>

#define USER_ROOT_UID        0
#define USER_ROOT_GID        0
#define USER_FIRST_USER_UID  1000

#define USER_MAX_ACCOUNTS    8
#define USER_NAME_MAX        32
#define USER_REALNAME_MAX    48
#define USER_PASS_MAX        64
#define USER_SALT_SIZE       16
#define USER_HASH_SIZE       32
#define USER_KDF_ITERATIONS  2048

#define USER_MAX_FAILED_ATTEMPTS 5
#define USER_LOCKOUT_TICKS       500 /* 5 seconds at 100Hz */

typedef struct user_account {
    uid_t uid;
    gid_t gid;
    char  username[USER_NAME_MAX];
    char  realname[USER_REALNAME_MAX];
    char  homedir[64];
    char  shell[32];
    u8    salt[USER_SALT_SIZE];
    u8    pass_hash[USER_HASH_SIZE];
    u32   kdf_iterations;
    bool  is_active;
    bool  is_locked;
    u32   failed_attempts;
    u64   lockout_until_tick;
    u64   created_timestamp;
    u64   last_login_timestamp;
} user_account_t;

/* Status / Return Codes */
typedef enum {
    USER_OK                  =  0,
    USER_ERR_NOT_FOUND       = -1,
    USER_ERR_AUTH_FAILED     = -2,
    USER_ERR_LOCKED          = -3,
    USER_ERR_EXISTS          = -4,
    USER_ERR_LIMIT           = -5,
    USER_ERR_INVALID_PARAM   = -6,
    USER_ERR_PERMISSION      = -7,
    USER_ERR_IO              = -8,
} user_err_t;

/* User Accounts Management API */
int  user_subsystem_init(void);
int  user_authenticate(const char *username, const char *password, uid_t *out_uid);
int  user_create_account(const char *username, const char *realname, const char *password, uid_t uid, gid_t gid);
int  user_change_password(const char *username, const char *old_pass, const char *new_pass);
int  user_set_password_root(const char *username, const char *new_pass);
const user_account_t *user_get_by_name(const char *username);
const user_account_t *user_get_by_uid(uid_t uid);
int  user_list_accounts(user_account_t *out_users, int max_count);
int  user_get_account_count(void);
int  user_sync_to_disk(void);
int  user_load_from_disk(void);

/* Cryptographic Password Hashing & Verification */
void user_compute_hash(const char *password, const u8 *salt, size_t salt_len, u32 iterations, u8 *out_hash);
bool user_verify_hash_const_time(const u8 *a, const u8 *b, size_t len);

#endif /* ASTRA_KERNEL_USER_H */
