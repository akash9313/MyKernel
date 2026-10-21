#ifndef ASTRA_KERNEL_DEADLINE_H
#define ASTRA_KERNEL_DEADLINE_H

#include <kernel/types.h>

/*
 * Maximum sleep duration delta: strictly half the 64-bit unsigned range (2^63 - 1).
 * This guarantees that unsigned modular difference arithmetic remains strictly unambiguous
 * and overflow-proof across the entire counter space without implementation-defined casts.
 */
#define MAX_SLEEP_TICKS         0x7FFFFFFFFFFFFFFFULL

/*
 * deadline_has_passed(deadline, current_ticks):
 * Evaluates whether current_ticks has reached or passed deadline using pure unsigned modular subtraction.
 *
 * For any delta <= MAX_SLEEP_TICKS:
 * When the deadline is reached or passed (and serviced within MAX_SLEEP_TICKS),
 * the unsigned difference (current_ticks - deadline) modulo 2^64 lies in [0, MAX_SLEEP_TICKS].
 * While the deadline is in the future, (current_ticks - deadline) lies in (MAX_SLEEP_TICKS, 2^64 - 1].
 *
 * This uses 100% standard C unsigned arithmetic with zero implementation-defined signed conversions.
 */
static inline bool deadline_has_passed(u64 deadline, u64 current_ticks) {
    return (current_ticks - deadline) <= MAX_SLEEP_TICKS;
}

/*
 * deadline_is_strictly_before(d1, d2):
 * Compares two deadlines modularly.
 * Returns true if and only if d1 is strictly earlier than d2:
 *   (d1 != d2) && ((d2 - d1) <= MAX_SLEEP_TICKS)
 *
 * When d1 == d2, returns false. This property allows stable queue insertion:
 * newly arriving threads with deadline equal to existing queue entries are placed
 * AFTER the existing entries, deterministically guaranteeing FIFO tie-breaking
 * without requiring sequence numbers or risking sequence counter wraparound.
 */
static inline bool deadline_is_strictly_before(u64 d1, u64 d2) {
    return (d1 != d2) && ((d2 - d1) <= MAX_SLEEP_TICKS);
}

#endif /* ASTRA_KERNEL_DEADLINE_H */
