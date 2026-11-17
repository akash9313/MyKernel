#ifndef ASTRA_KERNEL_SESSION_H
#define ASTRA_KERNEL_SESSION_H

#include <kernel/types.h>
#include <kernel/user.h>

typedef u32 sid_t;

#define SESSION_MAX_SESSIONS 8
#define SESSION_INVALID_SID  0

typedef enum session_state {
    SESSION_UNUSED      = 0,
    SESSION_ACTIVE      = 1, /* Displaying on desktop, receiving input */
    SESSION_LOCKED      = 2, /* Desktop locked, applications preserved, lock screen active */
    SESSION_BACKGROUND  = 3, /* User switched; applications preserved, hidden from active display */
    SESSION_TERMINATING = 4  /* Logging out, teardown in progress */
} session_state_t;

typedef struct session {
    sid_t           sid;
    uid_t           uid;
    char            username[USER_NAME_MAX];
    char            realname[USER_REALNAME_MAX];
    session_state_t state;
    u64             created_tick;
    u64             last_active_tick;
    
    /* Associated resources */
    int             window_count;
    int             window_ids[16];
    int             process_count;
    pid_t           pids[16];
    char            working_dir[128];
} session_t;

/* Status / Return Codes */
typedef enum {
    SESSION_OK               =  0,
    SESSION_ERR_NOT_FOUND    = -1,
    SESSION_ERR_AUTH_FAILED  = -2,
    SESSION_ERR_LOCKED       = -3,
    SESSION_ERR_LIMIT        = -4,
    SESSION_ERR_INVALID      = -5,
    SESSION_ERR_BUSY         = -6,
} session_err_t;

/* Session Lifecycle API */
int         session_subsystem_init(void);
session_t  *session_create(uid_t uid);
int         session_lock(sid_t sid);
int         session_unlock(sid_t sid, const char *password);
int         session_logout(sid_t sid);
int         session_switch(sid_t target_sid);
int         session_switch_to_user(const char *username, const char *password);
session_t  *session_get_active(void);
sid_t       session_get_active_sid(void);
uid_t       session_get_active_uid(void);
session_t  *session_find_by_sid(sid_t sid);
session_t  *session_find_by_uid(uid_t uid);
int         session_list(session_t *out_sessions, int max_count);
int         session_get_count(void);
bool        session_is_locked(void);

/* Resource Association */
void session_register_window(sid_t sid, int win_id);
void session_unregister_window(sid_t sid, int win_id);
void session_register_process(sid_t sid, pid_t pid);
void session_unregister_process(sid_t sid, pid_t pid);

#endif /* ASTRA_KERNEL_SESSION_H */
