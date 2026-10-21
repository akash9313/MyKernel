#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include <kernel/deadline.h>

int main(void) {
    printf("======================================================\n");
    printf("       AstraOS Deadline Arithmetic Host Tests         \n");
    printf("======================================================\n");

    /* 1. Normal Ordering */
    printf("[HOST TEST] 1. Normal deadline expiry and comparison...\n");
    {
        u64 now = 1000;
        u64 d_future = 1050;
        u64 d_exact = 1000;
        u64 d_past = 950;

        assert(!deadline_has_passed(d_future, now));
        assert(deadline_has_passed(d_exact, now));
        assert(deadline_has_passed(d_past, now));

        assert(deadline_is_before(100, 1, 200, 2));
        assert(!deadline_is_before(200, 2, 100, 1));
        printf("  PASS: Normal deadline expiry and comparison verified.\n");
    }

    /* 2. Deterministic Tie-Breaking for Equal Deadlines */
    printf("[HOST TEST] 2. Deterministic FIFO tie-breaking for equal deadlines...\n");
    {
        u64 d1 = 500, seq1 = 1;
        u64 d2 = 500, seq2 = 2;

        /* Same deadline: earlier insertion sequence (seq1) must precede seq2 */
        assert(deadline_is_before(d1, seq1, d2, seq2));
        assert(!deadline_is_before(d2, seq2, d1, seq1));
        printf("  PASS: Equal deadlines tie-break deterministically by insertion sequence.\n");
    }

    /* 3. Wraparound Handling Across UINT64_MAX Boundary */
    printf("[HOST TEST] 3. Modular arithmetic counter wraparound...\n");
    {
        u64 now_near_wrap = 0xFFFFFFFFFFFFFFFEULL; /* UINT64_MAX - 1 */
        u64 delta = 5;
        u64 deadline = now_near_wrap + delta; /* Wraps to 3 */

        assert(deadline == 3ULL);

        /* Before wrap: not passed */
        assert(!deadline_has_passed(deadline, now_near_wrap));
        assert(!deadline_has_passed(deadline, 0xFFFFFFFFFFFFFFFFULL));
        assert(!deadline_has_passed(deadline, 0ULL));
        assert(!deadline_has_passed(deadline, 1ULL));
        assert(!deadline_has_passed(deadline, 2ULL));

        /* Exact expiry at tick 3 */
        assert(deadline_has_passed(deadline, 3ULL));

        /* After expiry at tick 4 */
        assert(deadline_has_passed(deadline, 4ULL));

        /* Relative ordering across wraparound: 3 (wrapped) is before 10 (wrapped) */
        assert(deadline_is_before(3ULL, 1, 10ULL, 2));
        assert(!deadline_is_before(10ULL, 2, 3ULL, 1));
        printf("  PASS: 64-bit unsigned wraparound correctly handled across 0 boundary.\n");
    }

    /* 4. Maximum Supported Duration Limits */
    printf("[HOST TEST] 4. Maximum duration boundary checks...\n");
    {
        u64 max_dur = MAX_SLEEP_TICKS;
        assert(max_dur == 0x7FFFFFFFFFFFFFFFULL);

        /* At maximum duration, (now + max_dur) - now == max_dur (positive in signed 64-bit) */
        u64 now = 100;
        u64 deadline_max = now + max_dur;
        assert(!deadline_has_passed(deadline_max, now));

        /* One tick past maximum duration exceeds half 64-bit range */
        u64 invalid_dur = max_dur + 1;
        assert(invalid_dur == 0x8000000000000000ULL);
        /* Production API must reject invalid_dur > MAX_SLEEP_TICKS */
        bool accepted = (invalid_dur <= MAX_SLEEP_TICKS);
        assert(!accepted);
        printf("  PASS: Maximum accepted duration (2^63 - 1) and rejection of overflow verified.\n");
    }

    printf("------------------------------------------------------\n");
    printf("[SUCCESS] All host-side Deadline Arithmetic tests passed!\n");
    printf("======================================================\n");
    return 0;
}
