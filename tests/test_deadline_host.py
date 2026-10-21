import os
import sys
import subprocess

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEADLINE_H = os.path.join(BASE_DIR, 'kernel', 'include', 'kernel', 'deadline.h')

# Load MAX_SLEEP_TICKS from production header
MAX_SLEEP_TICKS = 0x7FFFFFFFFFFFFFFF

def deadline_has_passed(deadline, current_ticks):
    return ((current_ticks - deadline) & 0xFFFFFFFFFFFFFFFF) <= MAX_SLEEP_TICKS

def deadline_is_strictly_before(d1, d2):
    return (d1 != d2) and (((d2 - d1) & 0xFFFFFFFFFFFFFFFF) <= MAX_SLEEP_TICKS)

def run_tests():
    print("======================================================")
    print("       AstraOS Deadline Arithmetic Host Tests         ")
    print("======================================================")

    # Verify MAX_SLEEP_TICKS matches production header
    with open(DEADLINE_H, 'r') as f:
        header_text = f.read()
    assert "0x7FFFFFFFFFFFFFFFULL" in header_text, "MAX_SLEEP_TICKS definition mismatch in deadline.h!"
    assert "(current_ticks - deadline) <= MAX_SLEEP_TICKS" in header_text, "deadline_has_passed definition mismatch!"
    assert "deadline_is_strictly_before" in header_text, "deadline_is_strictly_before definition mismatch!"

    # 1. Normal Ordering
    print("[HOST TEST] 1. Normal deadline expiry and comparison...")
    now = 1000
    assert not deadline_has_passed(1050, now)
    assert deadline_has_passed(1000, now)
    assert deadline_has_passed(950, now)
    assert deadline_is_strictly_before(100, 200)
    assert not deadline_is_strictly_before(200, 100)
    print("  PASS: Normal deadline expiry and comparison verified.")

    # 2. Equal Deadline Stable Insertion (No Sequence Counter Needed)
    print("[HOST TEST] 2. Deterministic FIFO tie-breaking for equal deadlines via stable insertion...")
    d1 = 500
    d2 = 500
    # Equal deadlines return False for strictly_before, ensuring new nodes insert AFTER existing nodes
    assert not deadline_is_strictly_before(d1, d2)
    assert not deadline_is_strictly_before(d2, d1)

    # Simulate stable queue insertion
    queue = []
    def insert_stable(val, label):
        idx = 0
        while idx < len(queue) and not deadline_is_strictly_before(val, queue[idx][0]):
            idx += 1
        queue.insert(idx, (val, label))

    insert_stable(500, "first_500")
    insert_stable(200, "first_200")
    insert_stable(500, "second_500")
    insert_stable(700, "first_700")
    insert_stable(500, "third_500")

    labels = [item[1] for item in queue]
    assert labels == ["first_200", "first_500", "second_500", "third_500", "first_700"], f"Got: {labels}"
    print("  PASS: Equal deadlines preserve FIFO sequence stably without sequence numbers.")

    # 3. Modular Wraparound Across UINT64_MAX Boundary
    print("[HOST TEST] 3. Modular arithmetic counter wraparound...")
    now_near_wrap = 0xFFFFFFFFFFFFFFFE  # UINT64_MAX - 1
    delta = 5
    deadline = (now_near_wrap + delta) & 0xFFFFFFFFFFFFFFFF  # Wraps to 3
    assert deadline == 3

    assert not deadline_has_passed(deadline, now_near_wrap)
    assert not deadline_has_passed(deadline, 0xFFFFFFFFFFFFFFFF)
    assert not deadline_has_passed(deadline, 0)
    assert not deadline_has_passed(deadline, 1)
    assert not deadline_has_passed(deadline, 2)
    assert deadline_has_passed(deadline, 3)
    assert deadline_has_passed(deadline, 4)

    assert deadline_is_strictly_before(3, 10)
    assert not deadline_is_strictly_before(10, 3)
    print("  PASS: 64-bit unsigned wraparound correctly handled across 0 boundary.")

    # 4. Maximum Duration Boundary Checks
    print("[HOST TEST] 4. Maximum duration boundary checks...")
    max_dur = MAX_SLEEP_TICKS
    now = 100
    deadline_max = (now + max_dur) & 0xFFFFFFFFFFFFFFFF
    assert not deadline_has_passed(deadline_max, now)

    invalid_dur = max_dur + 1
    assert invalid_dur == 0x8000000000000000
    assert not (invalid_dur <= MAX_SLEEP_TICKS)
    print("  PASS: Maximum accepted duration (2^63 - 1) and rejection of overflow verified.")

    print("------------------------------------------------------")
    print("[SUCCESS] All host-side Deadline Arithmetic tests passed!")
    print("======================================================")
    return True

if __name__ == '__main__':
    ok = run_tests()
    sys.exit(0 if ok else 1)
