#!/usr/bin/env python3
"""
AstraOS Phase U: Deterministic AstraJS Fuzz & Mutation Test Harness

Performs bounded host-side adversarial fuzz testing and grammar mutation on the AstraJS
interpreter using a fixed random seed (42).

Asserts that:
1. No adversarial input can trigger a memory fault, unhandled crash, or infinite hang.
2. Every input evaluates to a bounded, valid error enum or success.
3. Every error leaves the VM in a consistent state capable of subsequent clean evaluation.
"""

import os
import sys
import random
import ctypes
import subprocess
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
DLL_PATH = ROOT_DIR / "tests" / "fuzz_js_host.dll"
SRC_C = ROOT_DIR / "tests" / "fuzz_js_host.c"
ENGINE_C = ROOT_DIR / "user" / "js_engine.c"
CLANG_PATH = ROOT_DIR / "tools" / "llvm-toolchain" / "bin" / "clang.exe"

def ensure_dll_built():
    if not DLL_PATH.exists() or (os.path.getmtime(ENGINE_C) > os.path.getmtime(DLL_PATH)):
        cmd = [str(CLANG_PATH), "-O2", "-shared", str(SRC_C), str(ENGINE_C), "-o", str(DLL_PATH)]
        res = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT_DIR))
        if res.returncode != 0:
            print(f"[ERROR] Failed to compile fuzz_js_host.dll:\n{res.stderr}")
            sys.exit(1)

def run_fuzz_suite(num_iterations=500, seed=42):
    ensure_dll_built()
    
    lib = ctypes.CDLL(str(DLL_PATH))
    lib.fuzz_eval_js.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
    lib.fuzz_eval_js.restype = ctypes.c_int

    random.seed(seed)

    base_corpus = [
        "var x = 10 + 20;",
        "var s = 'hello ' + 'world';",
        "function add(a, b) { return a + b; } add(3, 4);",
        "if (1 < 2) { var y = 1; } else { var y = 2; }",
        "while (x < 10) { x = x + 1; }",
        "var elem = document.getElementById('test_elem'); elem.textContent = 'updated';",
        "elem.onclick = 'add(1, 2)';",
        "elem.onclick = add;",
        "var null_val = null; var undef_val = undefined;",
        "-9223372036854775807 - 1;",
        "100 / 0;",
        "while (1) {}",
        "function rec() { rec(); } rec();",
        "document.getElementById('missing');",
    ]

    malformed_snippets = [
        "",
        "   ",
        ";;;;;;;;",
        "var",
        "function",
        "return",
        "if",
        "while",
        "((((((((((((((((((((((((((((((((1))))))))))))))))))))))))))))))))",
        "((((((((((((((((",
        "))))))))))))))))",
        "{{{{{{{{{{{{{{{{",
        "}}}}}}}}}}}}}}}}",
        "var 123abc = 5;",
        "var x = ;",
        "var x = + + +;",
        "var x = * 5;",
        "var x = 5 / 0;",
        "var x = -9223372036854775808 / -1;",
        "var x = -9223372036854775808 % -1;",
        "'unterminated string literal",
        "\"unterminated double quote",
        "'escaped \\' still unterminated",
        "'\\\\\\\\\\\\\\\\\\\\\\\\'",
        "elem.onclick = 12345;",
        "elem.onclick = -99;",
        "document.getElementById().textContent = 1;",
        "document.getElementById('test_elem').textContent = 'a' * 5000;",
        "function broken( { return 1; }",
        "function (a, b) { return a; }",
        "function f(a) { return a + ; }",
        "window.open('evil');",
        "fetch('http://evil');",
        "eval('1 + 1');",
    ]

    print(f"[TEST] Starting AstraJS deterministic fuzz suite: {num_iterations} iterations, seed={seed}...")

    crashes = 0
    recovery_failures = 0
    reproducers = []

    # 1. Execute fixed malformed snippets
    for idx, snippet in enumerate(malformed_snippets):
        rec_code = ctypes.c_int(0)
        ret = lib.fuzz_eval_js(snippet.encode('utf-8', errors='ignore'), ctypes.byref(rec_code))
        if ret < 0 or ret > 11:
            print(f"[CRASH] Malformed snippet #{idx} returned invalid code {ret}: {snippet!r}")
            crashes += 1
            reproducers.append(snippet)
        if rec_code.value != 0:
            print(f"[RECOVERY FAIL] Post-eval recovery failed ({rec_code.value}) after snippet: {snippet!r}")
            recovery_failures += 1
            reproducers.append(snippet)

    # 2. Execute mutated inputs
    mutation_operators = [
        lambda s: s + random.choice([" {", " }", " (", " )", " ;", " ,", " +", " -", " *", " /", " %", " &&", " ||", " !"]),
        lambda s: s.replace(random.choice(["var", "function", "return", "if", "while", "=", "+", "(", ")"]), random.choice([" ", ";", "!", "&&", "undefined", "null"])),
        lambda s: s[:len(s)//2] if len(s) > 1 else s,
        lambda s: s + s,
        lambda s: "".join(c if random.random() > 0.08 else random.choice(" \t\n;(){}+-*/%=&|!0123456789'\"\\_") for c in s),
        lambda s: s + ("A" * random.randint(50, 4500)),
        lambda s: "function f() { " * random.randint(1, 20) + s + " }" * random.randint(1, 20),
    ]

    for i in range(len(malformed_snippets), num_iterations):
        base = random.choice(base_corpus + malformed_snippets)
        mutated = base
        num_mutations = random.randint(1, 4)
        for _ in range(num_mutations):
            op = random.choice(mutation_operators)
            try:
                mutated = op(mutated)
            except Exception:
                pass

        # Clamp length to 8000 bytes max
        if len(mutated) > 8000:
            mutated = mutated[:8000]

        rec_code = ctypes.c_int(0)
        try:
            ret = lib.fuzz_eval_js(mutated.encode('utf-8', errors='ignore'), ctypes.byref(rec_code))
        except Exception as e:
            print(f"[CRASH] Iteration #{i} threw exception: {e}")
            crashes += 1
            reproducers.append(mutated)
            continue

        if ret < 0 or ret > 11:
            print(f"[CRASH] Iteration #{i} returned out-of-range code {ret}")
            crashes += 1
            reproducers.append(mutated)
        if rec_code.value != 0:
            print(f"[RECOVERY FAIL] Iteration #{i} broke VM recovery ({rec_code.value})")
            recovery_failures += 1
            reproducers.append(mutated)

    print(f"\n[TEST] Fuzzing complete: {num_iterations} test cases executed.")
    print(f"       Crashes: {crashes}")
    print(f"       Recovery Failures: {recovery_failures}")

    if crashes > 0 or recovery_failures > 0:
        print(f"[FAILED] {len(reproducers)} failing inputs recorded.")
        sys.exit(1)

    print("[TEST] PASS: AstraJS deterministic host fuzz suite verified!\n")
    return 0

if __name__ == "__main__":
    sys.exit(run_fuzz_suite(500, 42))
