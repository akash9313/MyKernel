#ifndef ASTRA_KERNEL_PKG_H
#define ASTRA_KERNEL_PKG_H

#include <kernel/types.h>
#include <kernel/shell.h>

#define APKG_MAGIC              0x474B5041U /* "APKG" in LE */
#define APKG_VERSION            1
#define APKG_NAME_MAX           32
#define APKG_VER_MAX            16
#define APKG_ARCH_MAX           16
#define APKG_DESC_MAX           64
#define APKG_DEPS_MAX           64
#define APKG_FILES_MAX          16
#define PKG_MAX_INSTALLED       16
#define PKG_MAX_REPO_ENTRIES    16

typedef struct {
    u32 magic;                 /* APKG_MAGIC */
    u16 format_version;        /* 1 */
    u16 flags;                 /* Bit flags */
    char name[APKG_NAME_MAX];  /* Package name */
    char version[APKG_VER_MAX];/* Semantic version, e.g. "1.0.0" */
    char arch[APKG_ARCH_MAX];  /* Architecture, e.g. "x86_64" */
    char description[APKG_DESC_MAX];
    char depends[APKG_DEPS_MAX];
    u32 file_count;
    u32 payload_size;
    u8  payload_sha256[32];    /* SHA-256 digest of payload */
    u8  signature[256];        /* RSA-2048 PKCS#1 v1.5 signature */
} __attribute__((packed)) apkg_header_t;

typedef struct {
    char path[64];             /* Installation destination, e.g. "/bin/calc.elf" */
    u32  size;                 /* File byte length */
    u32  permissions;          /* Unix-style perms (0755) */
    u32  offset;               /* Offset in archive payload */
} __attribute__((packed)) apkg_file_entry_t;

typedef enum {
    PKG_OK                    =  0,
    PKG_ERR_NOT_FOUND         = -1,
    PKG_ERR_INVALID_FORMAT    = -2,
    PKG_ERR_BAD_CHECKSUM      = -3,
    PKG_ERR_BAD_SIGNATURE     = -4,
    PKG_ERR_DEP_CONFLICT      = -5,
    PKG_ERR_CIRCULAR_DEP      = -6,
    PKG_ERR_ALREADY_INSTALLED = -7,
    PKG_ERR_IO                = -8,
    PKG_ERR_NOSPC             = -9,
    PKG_ERR_ARCH_MISMATCH     = -10
} pkg_err_t;

typedef struct {
    char name[APKG_NAME_MAX];
    char version[APKG_VER_MAX];
    char arch[APKG_ARCH_MAX];
    char description[APKG_DESC_MAX];
    char depends[APKG_DEPS_MAX];
    u32  size;
    char sha256_hex[65];
    char location[64];         /* File or HTTPS URL */
    bool in_use;
} repo_package_t;

typedef struct {
    char name[APKG_NAME_MAX];
    char version[APKG_VER_MAX];
    char installed_files[APKG_FILES_MAX][64];
    int  file_count;
    bool in_use;
} installed_pkg_t;

void pkg_subsystem_init(void);
int pkg_search(const char *query, shell_out_fn out_fn, void *ctx);
int pkg_list(shell_out_fn out_fn, void *ctx);
int pkg_info(const char *name, shell_out_fn out_fn, void *ctx);
int pkg_install(const char *target, shell_out_fn out_fn, void *ctx);
int pkg_remove(const char *name, shell_out_fn out_fn, void *ctx);
int pkg_update(shell_out_fn out_fn, void *ctx);
int pkg_upgrade(shell_out_fn out_fn, void *ctx);
int pkg_clean(shell_out_fn out_fn, void *ctx);
int pkg_repo_list(shell_out_fn out_fn, void *ctx);
bool pkg_find_package_for_command(const char *cmd, char *out_pkg, char *out_ver);
bool pkg_find_package_details(const char *cmd, char *out_name, char *out_ver,
                              char *out_desc, u32 *out_size, char *out_deps);
bool pkg_self_test(void);

#endif /* ASTRA_KERNEL_PKG_H */
