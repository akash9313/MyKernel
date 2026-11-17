#include <kernel/session.h>
#include <kernel/user.h>
#include <kernel/wm.h>
#include <kernel/vfs.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/string.h>
#include <kernel/drivers/apic.h>

static struct {
    session_t sessions[SESSION_MAX_SESSIONS];
    sid_t active_sid;
    sid_t next_sid;
    int count;
    bool initialized;
} g_session_mgr = {0};

/* Forward declaration for desktop notification if GUI is running */
extern void desktop_notify_session_change(sid_t sid, session_state_t state);

int session_get_count(void) {
    return g_session_mgr.count;
}

session_t *session_get_active(void) {
    if (g_session_mgr.active_sid == SESSION_INVALID_SID) return NULL;
    return session_find_by_sid(g_session_mgr.active_sid);
}

sid_t session_get_active_sid(void) {
    return g_session_mgr.active_sid;
}

uid_t session_get_active_uid(void) {
    session_t *s = session_get_active();
    return s ? s->uid : USER_ROOT_UID;
}

bool session_is_locked(void) {
    session_t *s = session_get_active();
    return s ? (s->state == SESSION_LOCKED) : false;
}

session_t *session_find_by_sid(sid_t sid) {
    if (sid == SESSION_INVALID_SID) return NULL;
    for (int i = 0; i < SESSION_MAX_SESSIONS; i++) {
        if (g_session_mgr.sessions[i].sid == sid && g_session_mgr.sessions[i].state != SESSION_UNUSED) {
            return &g_session_mgr.sessions[i];
        }
    }
    return NULL;
}

session_t *session_find_by_uid(uid_t uid) {
    for (int i = 0; i < SESSION_MAX_SESSIONS; i++) {
        if (g_session_mgr.sessions[i].uid == uid && g_session_mgr.sessions[i].state != SESSION_UNUSED) {
            return &g_session_mgr.sessions[i];
        }
    }
    return NULL;
}

int session_list(session_t *out_sessions, int max_count) {
    if (!out_sessions || max_count <= 0) return 0;
    int n = 0;
    for (int i = 0; i < SESSION_MAX_SESSIONS && n < max_count; i++) {
        if (g_session_mgr.sessions[i].state != SESSION_UNUSED) {
            out_sessions[n++] = g_session_mgr.sessions[i];
        }
    }
    return n;
}

void session_register_window(sid_t sid, int win_id) {
    session_t *s = session_find_by_sid(sid);
    if (!s) return;
    for (int i = 0; i < s->window_count; i++) {
        if (s->window_ids[i] == win_id) return;
    }
    if (s->window_count < 16) {
        s->window_ids[s->window_count++] = win_id;
    }
}

void session_unregister_window(sid_t sid, int win_id) {
    session_t *s = session_find_by_sid(sid);
    if (!s) return;
    for (int i = 0; i < s->window_count; i++) {
        if (s->window_ids[i] == win_id) {
            for (int j = i; j < s->window_count - 1; j++) {
                s->window_ids[j] = s->window_ids[j + 1];
            }
            s->window_count--;
            break;
        }
    }
}

void session_register_process(sid_t sid, pid_t pid) {
    session_t *s = session_find_by_sid(sid);
    if (!s) return;
    for (int i = 0; i < s->process_count; i++) {
        if (s->pids[i] == pid) return;
    }
    if (s->process_count < 16) {
        s->pids[s->process_count++] = pid;
    }
}

void session_unregister_process(sid_t sid, pid_t pid) {
    session_t *s = session_find_by_sid(sid);
    if (!s) return;
    for (int i = 0; i < s->process_count; i++) {
        if (s->pids[i] == pid) {
            for (int j = i; j < s->process_count - 1; j++) {
                s->pids[j] = s->pids[j + 1];
            }
            s->process_count--;
            break;
        }
    }
}

session_t *session_create(uid_t uid) {
    const user_account_t *acc = user_get_by_uid(uid);
    if (!acc) {
        kprintf("[SESSION ERROR] Cannot create session: UID %u not found in accounts DB!\n", uid);
        return NULL;
    }

    /* Check if session already exists for this UID */
    session_t *existing = session_find_by_uid(uid);
    if (existing) {
        return existing;
    }

    /* Find free slot */
    session_t *slot = NULL;
    for (int i = 0; i < SESSION_MAX_SESSIONS; i++) {
        if (g_session_mgr.sessions[i].state == SESSION_UNUSED) {
            slot = &g_session_mgr.sessions[i];
            break;
        }
    }

    if (!slot) {
        kprintf("[SESSION ERROR] Maximum concurrent sessions reached (%d)!\n", SESSION_MAX_SESSIONS);
        return NULL;
    }

    kmemset(slot, 0, sizeof(session_t));
    slot->sid = g_session_mgr.next_sid++;
    slot->uid = uid;
    kstrncpy(slot->username, acc->username, USER_NAME_MAX - 1);
    kstrncpy(slot->realname, acc->realname, USER_REALNAME_MAX - 1);
    slot->state = SESSION_ACTIVE;
    slot->created_tick = apic_get_ticks();
    slot->last_active_tick = slot->created_tick;
    kstrncpy(slot->working_dir, acc->homedir, sizeof(slot->working_dir) - 1);

    g_session_mgr.count++;

    /* If no active session or current was None, set active */
    if (g_session_mgr.active_sid == SESSION_INVALID_SID) {
        g_session_mgr.active_sid = slot->sid;
    }

    kprintf("[SESSION] Created session %u for user '%s' (UID %u, Dir '%s').\n",
            slot->sid, slot->username, slot->uid, slot->working_dir);

    desktop_notify_session_change(slot->sid, slot->state);
    return slot;
}

int session_lock(sid_t sid) {
    session_t *s = session_find_by_sid(sid);
    if (!s) return SESSION_ERR_NOT_FOUND;
    if (s->state == SESSION_LOCKED) return SESSION_OK;

    s->state = SESSION_LOCKED;
    kprintf("[SESSION] Session %u ('%s') LOCKED.\n", s->sid, s->username);

    desktop_notify_session_change(s->sid, s->state);
    return SESSION_OK;
}

int session_unlock(sid_t sid, const char *password) {
    session_t *s = session_find_by_sid(sid);
    if (!s) return SESSION_ERR_NOT_FOUND;
    if (s->state != SESSION_LOCKED) return SESSION_OK;

    uid_t auth_uid = 0;
    int auth_res = user_authenticate(s->username, password, &auth_uid);
    if (auth_res != USER_OK) {
        kprintf("[SESSION] Unlock failed for session %u ('%s'): invalid credentials (err=%d).\n",
                s->sid, s->username, auth_res);
        return SESSION_ERR_AUTH_FAILED;
    }

    s->state = SESSION_ACTIVE;
    s->last_active_tick = apic_get_ticks();
    kprintf("[SESSION] Session %u ('%s') UNLOCKED successfully.\n", s->sid, s->username);

    desktop_notify_session_change(s->sid, s->state);
    return SESSION_OK;
}

int session_logout(sid_t sid) {
    session_t *s = session_find_by_sid(sid);
    if (!s) return SESSION_ERR_NOT_FOUND;

    kprintf("[SESSION] Logging out session %u ('%s')...\n", s->sid, s->username);
    s->state = SESSION_TERMINATING;

    /* 1. Destroy all windows registered to this session */
    for (int i = 0; i < s->window_count; i++) {
        wm_window_t *w = wm_find_by_id(s->window_ids[i]);
        if (w) {
            wm_destroy_window(w);
        }
    }
    s->window_count = 0;

    /* 2. Close open fds and kill processes belonging to this session */
    for (int i = 0; i < s->process_count; i++) {
        vfs_close_all_for_pid(s->pids[i]);
    }
    s->process_count = 0;

    /* 3. Flush user file transactions to disk */
    vfs_sync();

    /* 4. Release session slot */
    s->state = SESSION_UNUSED;
    g_session_mgr.count--;

    /* 5. Switch to another existing session or return to Login Display Manager */
    if (g_session_mgr.active_sid == sid) {
        g_session_mgr.active_sid = SESSION_INVALID_SID;
        for (int i = 0; i < SESSION_MAX_SESSIONS; i++) {
            if (g_session_mgr.sessions[i].state != SESSION_UNUSED) {
                session_switch(g_session_mgr.sessions[i].sid);
                break;
            }
        }
    }

    desktop_notify_session_change(g_session_mgr.active_sid, SESSION_UNUSED);
    kprintf("[SESSION] Logout complete for session %u.\n", sid);
    return SESSION_OK;
}

int session_switch(sid_t target_sid) {
    session_t *target = session_find_by_sid(target_sid);
    if (!target) return SESSION_ERR_NOT_FOUND;

    if (g_session_mgr.active_sid == target_sid) return SESSION_OK;

    session_t *prev = session_get_active();
    if (prev) {
        if (prev->state == SESSION_ACTIVE) {
            prev->state = SESSION_BACKGROUND;
        }
        /* Hide windows of outgoing session without destroying them */
        for (int i = 0; i < prev->window_count; i++) {
            wm_window_t *w = wm_find_by_id(prev->window_ids[i]);
            if (w) w->visible = false;
        }
    }

    /* Reveal windows of incoming session */
    for (int i = 0; i < target->window_count; i++) {
        wm_window_t *w = wm_find_by_id(target->window_ids[i]);
        if (w) w->visible = true;
    }

    g_session_mgr.active_sid = target_sid;
    if (target->state == SESSION_BACKGROUND) {
        target->state = SESSION_ACTIVE;
    }
    target->last_active_tick = apic_get_ticks();

    kprintf("[SESSION] Switched active session to %u ('%s', state=%d).\n",
            target->sid, target->username, target->state);

    desktop_notify_session_change(target->sid, target->state);
    return SESSION_OK;
}

int session_switch_to_user(const char *username, const char *password) {
    if (!username) return SESSION_ERR_INVALID;

    const user_account_t *acc = user_get_by_name(username);
    if (!acc) return SESSION_ERR_NOT_FOUND;

    session_t *existing = session_find_by_uid(acc->uid);
    if (existing) {
        /* If existing session is locked, authenticate before switching */
        if (existing->state == SESSION_LOCKED) {
            if (!password) return SESSION_ERR_AUTH_FAILED;
            int u_res = session_unlock(existing->sid, password);
            if (u_res != SESSION_OK) return u_res;
        }
        return session_switch(existing->sid);
    }

    /* Otherwise, authenticate and create fresh session */
    uid_t uid = 0;
    int auth_res = user_authenticate(username, password ? password : "", &uid);
    if (auth_res != USER_OK) {
        return SESSION_ERR_AUTH_FAILED;
    }

    session_t *new_s = session_create(uid);
    if (!new_s) return SESSION_ERR_LIMIT;

    return session_switch(new_s->sid);
}

int session_subsystem_init(void) {
    if (g_session_mgr.initialized) return SESSION_OK;

    kprintf("[SESSION] Initializing Session Management Subsystem...\n");
    kmemset(&g_session_mgr, 0, sizeof(g_session_mgr));
    g_session_mgr.next_sid = 1;
    g_session_mgr.active_sid = SESSION_INVALID_SID;

    /* Create primary session for default standard user (akash, UID 1000) */
    session_t *initial_session = session_create(USER_FIRST_USER_UID);
    if (initial_session) {
        g_session_mgr.active_sid = initial_session->sid;
        kprintf("[SESSION] Primary user session initialized (SID=%u, User='%s').\n",
                initial_session->sid, initial_session->username);
    } else {
        /* Fallback to root if akash not found */
        session_t *root_session = session_create(USER_ROOT_UID);
        if (root_session) {
            g_session_mgr.active_sid = root_session->sid;
        }
    }

    g_session_mgr.initialized = true;
    return SESSION_OK;
}
