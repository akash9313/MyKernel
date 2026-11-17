#ifndef ASTRA_KERNEL_POWER_H
#define ASTRA_KERNEL_POWER_H

#include <kernel/types.h>

typedef enum {
    POWER_STATE_RUNNING = 0,
    POWER_STATE_SHUTDOWN_REQUESTED,
    POWER_STATE_SYNCING,
    POWER_STATE_QUIESCING,
    POWER_STATE_POWERING_OFF,
    POWER_STATE_RESTARTING,
    POWER_STATE_SUSPENDING,
    POWER_STATE_SUSPENDED,
    POWER_STATE_RESUMING,
    POWER_STATE_FAILED
} power_state_t;

#define POWER_OK                 0
#define POWER_ERR_BUSY          -1
#define POWER_ERR_UNSUPPORTED   -2
#define POWER_ERR_IO            -3
#define POWER_ERR_INVALID_STATE -4

void power_init(void);
power_state_t power_get_state(void);
const char *power_state_to_string(power_state_t st);
bool power_is_in_progress(void);

int power_shutdown(void);
int power_restart(void);
int power_halt(void);
int power_suspend(void);
int power_lock(void);
int power_logout(void);

#endif /* ASTRA_KERNEL_POWER_H */
