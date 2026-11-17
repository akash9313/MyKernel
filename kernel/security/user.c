#include <kernel/user.h>
#include <kernel/crypto.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/drivers/apic.h>

#define ACCOUNTS_DB_PATH "/data/etc/accounts.db"
#define ACCOUNTS_MAGIC   0x41434354U /* "ACCT" */
#define ACCOUNTS_VERSION 1

typedef struct {
    u32 magic;
    u32 version;
    u32 count;
    u32 checksum;
} accounts_db_header_t;

static struct {
    user_account_t accounts[USER_MAX_ACCOUNTS];
    int count;
    bool initialized;
} g_user_db = {0};

/* Simple checksum for account database */
static u32 calc_checksum(const void *data, size_t len) {
    const u8 *p = (const u8 *)data;
    u32 sum = 0x55AA55AA;
    for (size_t i = 0; i < len; i++) {
        sum = (sum << 5) + sum + p[i];
    }
    return sum;
}

/* Constant-time hash verification to mitigate timing attacks */
bool user_verify_hash_const_time(const u8 *a, const u8 *b, size_t len) {
    if (!a || !b) return false;
    u8 diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (a[i] ^ b[i]);
    }
    return (diff == 0);
}

/* PBKDF2-HMAC-SHA256 password key derivation */
void user_compute_hash(const char *password, const u8 *salt, size_t salt_len, u32 iterations, u8 *out_hash) {
    if (!password || !salt || iterations == 0 || !out_hash) return;

    size_t pass_len = kstrlen(password);
    if (salt_len > USER_SALT_SIZE) salt_len = USER_SALT_SIZE;

    /* U_1 = HMAC(P, S || 0x00000001) */
    u8 u_in[USER_SALT_SIZE + 4];
    kmemcpy(u_in, salt, salt_len);
    u_in[salt_len + 0] = 0;
    u_in[salt_len + 1] = 0;
    u_in[salt_len + 2] = 0;
    u_in[salt_len + 3] = 1;

    u8 u_prev[SHA256_DIGEST_SIZE];
    u8 u_curr[SHA256_DIGEST_SIZE];

    hmac_sha256(password, pass_len, u_in, salt_len + 4, u_prev);
    kmemcpy(out_hash, u_prev, SHA256_DIGEST_SIZE);

    /* Subsequent iterations: U_i = HMAC(P, U_{i-1}) */
    for (u32 iter = 1; iter < iterations; iter++) {
        hmac_sha256(password, pass_len, u_prev, SHA256_DIGEST_SIZE, u_curr);
        for (int b = 0; b < SHA256_DIGEST_SIZE; b++) {
            out_hash[b] ^= u_curr[b];
        }
        kmemcpy(u_prev, u_curr, SHA256_DIGEST_SIZE);
    }
}

int user_get_account_count(void) {
    return g_user_db.count;
}

const user_account_t *user_get_by_name(const char *username) {
    if (!username) return NULL;
    for (int i = 0; i < g_user_db.count; i++) {
        if (kstrcmp(g_user_db.accounts[i].username, username) == 0 && g_user_db.accounts[i].is_active) {
            return &g_user_db.accounts[i];
        }
    }
    return NULL;
}

const user_account_t *user_get_by_uid(uid_t uid) {
    for (int i = 0; i < g_user_db.count; i++) {
        if (g_user_db.accounts[i].uid == uid && g_user_db.accounts[i].is_active) {
            return &g_user_db.accounts[i];
        }
    }
    return NULL;
}

int user_list_accounts(user_account_t *out_users, int max_count) {
    if (!out_users || max_count <= 0) return 0;
    int n = 0;
    for (int i = 0; i < g_user_db.count && n < max_count; i++) {
        if (g_user_db.accounts[i].is_active) {
            out_users[n++] = g_user_db.accounts[i];
        }
    }
    return n;
}

int user_sync_to_disk(void) {
    /* Ensure /data/etc directory exists */
    vfs_mkdir("/data/etc");

    vfs_fd_t fd = vfs_open(ACCOUNTS_DB_PATH, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        return USER_ERR_IO;
    }

    accounts_db_header_t hdr;
    hdr.magic = ACCOUNTS_MAGIC;
    hdr.version = ACCOUNTS_VERSION;
    hdr.count = (u32)g_user_db.count;
    hdr.checksum = calc_checksum(g_user_db.accounts, sizeof(user_account_t) * g_user_db.count);

    if (vfs_write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        vfs_close(fd);
        return USER_ERR_IO;
    }

    size_t payload_size = sizeof(user_account_t) * g_user_db.count;
    if (vfs_write(fd, g_user_db.accounts, payload_size) != (ssize_t)payload_size) {
        vfs_close(fd);
        return USER_ERR_IO;
    }

    vfs_fsync(fd);
    vfs_close(fd);
    vfs_sync();
    return USER_OK;
}

int user_load_from_disk(void) {
    vfs_fd_t fd = vfs_open(ACCOUNTS_DB_PATH, VFS_O_RDONLY);
    if (fd < 0) {
        return USER_ERR_NOT_FOUND;
    }

    accounts_db_header_t hdr;
    if (vfs_read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        vfs_close(fd);
        return USER_ERR_IO;
    }

    if (hdr.magic != ACCOUNTS_MAGIC || hdr.version != ACCOUNTS_VERSION || hdr.count > USER_MAX_ACCOUNTS) {
        vfs_close(fd);
        return USER_ERR_IO;
    }

    user_account_t temp[USER_MAX_ACCOUNTS];
    size_t payload_size = sizeof(user_account_t) * hdr.count;
    if (vfs_read(fd, temp, payload_size) != (ssize_t)payload_size) {
        vfs_close(fd);
        return USER_ERR_IO;
    }

    vfs_close(fd);

    u32 csum = calc_checksum(temp, payload_size);
    if (csum != hdr.checksum) {
        kprintf("[USER] Accounts database checksum mismatch on disk!\n");
        return USER_ERR_IO;
    }

    kmemcpy(g_user_db.accounts, temp, payload_size);
    g_user_db.count = (int)hdr.count;
    kprintf("[USER] Loaded %d persistent user accounts from '%s'.\n", g_user_db.count, ACCOUNTS_DB_PATH);
    return USER_OK;
}

int user_create_account(const char *username, const char *realname, const char *password, uid_t uid, gid_t gid) {
    if (!username || !password || kstrlen(username) == 0 || kstrlen(password) == 0) {
        return USER_ERR_INVALID_PARAM;
    }
    if (g_user_db.count >= USER_MAX_ACCOUNTS) {
        return USER_ERR_LIMIT;
    }
    if (user_get_by_name(username) != NULL) {
        return USER_ERR_EXISTS;
    }
    if (user_get_by_uid(uid) != NULL) {
        return USER_ERR_EXISTS;
    }

    user_account_t *acc = &g_user_db.accounts[g_user_db.count];
    kmemset(acc, 0, sizeof(user_account_t));

    acc->uid = uid;
    acc->gid = gid;
    kstrncpy(acc->username, username, USER_NAME_MAX - 1);
    if (realname && kstrlen(realname) > 0) {
        kstrncpy(acc->realname, realname, USER_REALNAME_MAX - 1);
    } else {
        kstrncpy(acc->realname, username, USER_REALNAME_MAX - 1);
    }

    if (uid == 0) {
        kstrncpy(acc->homedir, "/root", sizeof(acc->homedir) - 1);
    } else {
        ksnprintf(acc->homedir, sizeof(acc->homedir), "/data/home/%s", username);
    }
    kstrncpy(acc->shell, "/bin/sh", sizeof(acc->shell) - 1);

    /* Generate 16-byte cryptographically secure random salt */
    crypto_get_random(acc->salt, USER_SALT_SIZE);
    acc->kdf_iterations = USER_KDF_ITERATIONS;

    /* Compute PBKDF2-HMAC-SHA256 password hash */
    user_compute_hash(password, acc->salt, USER_SALT_SIZE, acc->kdf_iterations, acc->pass_hash);

    acc->is_active = true;
    acc->is_locked = false;
    acc->failed_attempts = 0;
    acc->lockout_until_tick = 0;
    acc->created_timestamp = 0;
    acc->last_login_timestamp = 0;

    g_user_db.count++;

    /* Automatically create home directory */
    vfs_mkdir("/data/home");
    vfs_mkdir(acc->homedir);

    user_sync_to_disk();
    return USER_OK;
}

int user_change_password(const char *username, const char *old_pass, const char *new_pass) {
    if (!username || !old_pass || !new_pass) return USER_ERR_INVALID_PARAM;
    user_account_t *acc = NULL;
    for (int i = 0; i < g_user_db.count; i++) {
        if (kstrcmp(g_user_db.accounts[i].username, username) == 0 && g_user_db.accounts[i].is_active) {
            acc = &g_user_db.accounts[i];
            break;
        }
    }
    if (!acc) return USER_ERR_NOT_FOUND;

    /* Verify old password */
    u8 test_hash[USER_HASH_SIZE];
    user_compute_hash(old_pass, acc->salt, USER_SALT_SIZE, acc->kdf_iterations, test_hash);
    if (!user_verify_hash_const_time(test_hash, acc->pass_hash, USER_HASH_SIZE)) {
        return USER_ERR_AUTH_FAILED;
    }

    /* Generate fresh salt for new password */
    crypto_get_random(acc->salt, USER_SALT_SIZE);
    user_compute_hash(new_pass, acc->salt, USER_SALT_SIZE, acc->kdf_iterations, acc->pass_hash);
    acc->failed_attempts = 0;
    acc->lockout_until_tick = 0;

    user_sync_to_disk();
    return USER_OK;
}

int user_set_password_root(const char *username, const char *new_pass) {
    if (!username || !new_pass) return USER_ERR_INVALID_PARAM;
    user_account_t *acc = NULL;
    for (int i = 0; i < g_user_db.count; i++) {
        if (kstrcmp(g_user_db.accounts[i].username, username) == 0 && g_user_db.accounts[i].is_active) {
            acc = &g_user_db.accounts[i];
            break;
        }
    }
    if (!acc) return USER_ERR_NOT_FOUND;

    crypto_get_random(acc->salt, USER_SALT_SIZE);
    user_compute_hash(new_pass, acc->salt, USER_SALT_SIZE, acc->kdf_iterations, acc->pass_hash);
    acc->failed_attempts = 0;
    acc->lockout_until_tick = 0;

    user_sync_to_disk();
    return USER_OK;
}

int user_authenticate(const char *username, const char *password, uid_t *out_uid) {
    if (!username || !password) return USER_ERR_INVALID_PARAM;

    user_account_t *acc = NULL;
    for (int i = 0; i < g_user_db.count; i++) {
        if (kstrcmp(g_user_db.accounts[i].username, username) == 0 && g_user_db.accounts[i].is_active) {
            acc = &g_user_db.accounts[i];
            break;
        }
    }

    if (!acc) {
        /* Run dummy hash computation to mitigate username enumeration via timing */
        u8 dummy_salt[USER_SALT_SIZE] = {0};
        u8 dummy_hash[USER_HASH_SIZE];
        user_compute_hash(password, dummy_salt, USER_SALT_SIZE, USER_KDF_ITERATIONS, dummy_hash);
        return USER_ERR_NOT_FOUND;
    }

    /* Check account lockout rate-limiting */
    u64 current_tick = apic_get_ticks();
    if (acc->is_locked || (acc->lockout_until_tick > current_tick)) {
        return USER_ERR_LOCKED;
    }

    /* Compute candidate hash */
    u8 candidate_hash[USER_HASH_SIZE];
    user_compute_hash(password, acc->salt, USER_SALT_SIZE, acc->kdf_iterations, candidate_hash);

    if (user_verify_hash_const_time(candidate_hash, acc->pass_hash, USER_HASH_SIZE)) {
        acc->failed_attempts = 0;
        acc->lockout_until_tick = 0;
        if (out_uid) *out_uid = acc->uid;
        return USER_OK;
    } else {
        acc->failed_attempts++;
        if (acc->failed_attempts >= USER_MAX_FAILED_ATTEMPTS) {
            acc->lockout_until_tick = current_tick + USER_LOCKOUT_TICKS;
            kprintf("[SECURITY] Account '%s' locked out for 5 seconds (%d failed attempts)!\n",
                    acc->username, acc->failed_attempts);
        }
        return USER_ERR_AUTH_FAILED;
    }
}

int user_subsystem_init(void) {
    if (g_user_db.initialized) return USER_OK;

    kprintf("[USER] Initializing User Account & Security Subsystem...\n");
    g_user_db.count = 0;
    kmemset(g_user_db.accounts, 0, sizeof(g_user_db.accounts));

    /* Attempt loading existing persistent accounts from disk */
    int load_res = user_load_from_disk();
    if (load_res != USER_OK) {
        kprintf("[USER] No persistent account database found. Provisioning default secure accounts...\n");

        /* 1. System Administrator (root) */
        user_create_account("root", "System Administrator", "root", USER_ROOT_UID, USER_ROOT_GID);

        /* 2. Primary Standard User (akash) */
        user_create_account("akash", "Akash", "astra", USER_FIRST_USER_UID, USER_FIRST_USER_UID);

        /* 3. Restricted Guest User (guest) */
        user_create_account("guest", "Guest User", "guest", 1001, 1001);

        user_sync_to_disk();
    }

    g_user_db.initialized = true;
    kprintf("[USER] User Account Subsystem initialized (%d accounts active).\n", g_user_db.count);
    return USER_OK;
}
