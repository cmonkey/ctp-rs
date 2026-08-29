// Regression test for the SIGABRT that killed the production engine
// mid night-session on 2026-08-28 22:04:00 CST.
//
// Background
// ----------
// systemd recorded:
//
//     *** buffer overflow detected ***: terminated
//     ctp-tick-engine.service: Main process exited, code=dumped, status=6/ABRT
//     ctp-tick-engine.service: Failed with result 'core-dump'
//
// `Converter.cpp` converts Rust-side strings into CTP request structs
// with 2912 call sites of the shape:
//
//     strcpy(y.InstrumentID, x.InstrumentID.c_str());
//
// Every CThostFtdc*Field member is a fixed `char[N]`. Because this
// translation unit is built optimised, the toolchain rewrites those
// strcpy() calls to __strcpy_chk() — confirmed present in the shipped
// binary's dynamic relocations:
//
//     $ nm -D --undefined-only /opt/vrp/bin/ctp-tick-engine | grep _chk
//     __memcpy_chk  __memmove_chk  __memset_chk
//     __printf_chk  __stack_chk_fail  __strcpy_chk
//
// __strcpy_chk calls abort() the instant the source is longer than the
// destination field. "__stack_chk_fail" prints "stack smashing
// detected", so the observed "buffer overflow detected" can only have
// come from one of the *_chk family above — all of which live in this
// C++ wrapper, since rustc emits no fortified calls.
//
// What this test proves
// ---------------------
//   PART 1 — the mechanism is real under *this repo's* build flags:
//            an over-long source aborts the process with SIGABRT.
//            If PART 1 does not abort, the hypothesis above is wrong
//            and the test says so loudly instead of passing quietly.
//   PART 2 — the fix (ctp_set_field) truncates, keeps the process
//            alive, and NUL-terminates within the field.
//   PART 3 — the fix never writes past the field: a guard byte placed
//            immediately after the destination array is untouched.
//   PART 4 — normal (fitting) values are copied byte-identically, so
//            the swap is behaviour-preserving on the happy path.
//
// Build with the same flags cc-rs uses for a release build:
//   g++ -std=c++20 -O2 -fPIC -w -I sdk/v6.7.11 \
//       wrapper/tests/test_ctp_field_overflow.cpp -o /tmp/t && /tmp/t

#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "ThostFtdcUserApiStruct.h"

#include "../include/CtpFieldGuard.h"

// ── Harness ────────────────────────────────────────────────────────
//
// The over-long source is built at runtime from a heap std::string so
// the optimiser cannot constant-fold the length and elide the check.

enum Variant { VARIANT_STRCPY, VARIANT_FIXED, VARIANT_MEMCPY, VARIANT_BYTES };

// Runs one variant in a forked child. Returns the raw waitpid status.
static int run_child(Variant v, const std::string &src) {
    pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); std::exit(70); }
    if (pid == 0) {
        // Pre-fix variant deliberately aborts; suppress its core file
        // so repeated runs cannot fill /var/crash.
        prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

        CThostFtdcInputOrderField y;
        std::memset(&y, 0, sizeof(y));

        if (v == VARIANT_STRCPY) {
            std::strcpy(y.InstrumentID, src.c_str());   // ← the shipped code
        } else if (v == VARIANT_FIXED) {
            CTP_SET_FIELD(y.InstrumentID, src.c_str()); // ← the fix
        } else {
            // 字节块族:Rust 侧是 Vec<u8>,C 侧是定长 char[N]。
            // 与字符串族同一个 abort 类,收敛 strcpy 时漏掉了。
            CThostFtdcRspUserLogin2Field z;
            std::memset(&z, 0, sizeof(z));
            std::vector<unsigned char> blob(src.size(), 'Z');
            if (v == VARIANT_MEMCPY) {
                std::memcpy(z.RandomString, blob.data(), blob.size());  // ← 修前
            } else {
                CTP_SET_BYTES(z.RandomString, blob);                    // ← 修后
            }
            std::fprintf(stdout, "%zu\n", std::strlen(z.RandomString));
            _exit(0);
        }

        // Consume the result so nothing above is dead-code eliminated.
        std::fprintf(stdout, "%zu\n", std::strlen(y.InstrumentID));
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

static bool died_on_abort(int status) {
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

int main() {
    int failures = 0;
    const size_t cap = sizeof(CThostFtdcInputOrderField::InstrumentID);
    std::printf("CThostFtdcInputOrderField::InstrumentID capacity = %zu\n", cap);

    // Two bytes past the field: guarantees no room even for the NUL.
    const std::string too_long(cap + 1, 'X');
    const std::string fits(cap - 1, 'Y');

    // ── PART 1 — the defect is real ────────────────────────────────
    {
        int st = run_child(VARIANT_STRCPY, too_long);
        if (died_on_abort(st)) {
            std::printf("PART 1 ok   — strcpy(%zu chars → char[%zu]) aborted "
                        "with SIGABRT, reproducing the 22:04 crash\n",
                        too_long.size(), cap);
        } else {
            std::printf("PART 1 FAIL — strcpy did NOT abort (status=%d). The "
                        "fortify hypothesis does not hold under these build "
                        "flags; do NOT ship the fix on this reasoning.\n", st);
            ++failures;
        }
    }

    // ── PART 2 — the fix survives and truncates ────────────────────
    {
        int st = run_child(VARIANT_FIXED, too_long);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            std::printf("PART 2 ok   — ctp_set_field survived the same input\n");
        } else {
            std::printf("PART 2 FAIL — ctp_set_field did not exit cleanly "
                        "(status=%d)\n", st);
            ++failures;
        }
    }

    // ── PART 3 — the fix never writes past the field ───────────────
    {
        struct { char field[8]; unsigned char guard; } probe;
        std::memset(&probe, 0, sizeof(probe));
        probe.guard = 0xAB;

        const std::uint64_t base = ctp_field_truncation_count();
        ctp_set_field(probe.field, std::string(64, 'Z').c_str(), "PART3", "probe.field");

        bool ok = probe.guard == 0xAB
               && std::strlen(probe.field) == sizeof(probe.field) - 1
               && ctp_field_truncation_count() - base == 1;
        std::printf("PART 3 %s — guard=0x%02X len=%zu truncations=%llu\n",
                    ok ? "ok  " : "FAIL", probe.guard,
                    std::strlen(probe.field),
                    (unsigned long long)(ctp_field_truncation_count() - base));
        if (!ok) ++failures;
    }

    // ── PART 4 — fitting values are byte-identical to strcpy ───────
    {
        CThostFtdcInputOrderField a, b;
        std::memset(&a, 0, sizeof(a));
        std::memset(&b, 0, sizeof(b));

        const std::uint64_t base = ctp_field_truncation_count();
        std::strcpy(a.InstrumentID, fits.c_str());
        CTP_SET_FIELD(b.InstrumentID, fits.c_str());

        bool ok = std::memcmp(a.InstrumentID, b.InstrumentID, cap) == 0
               && ctp_field_truncation_count() - base == 0;
        std::printf("PART 4 %s — %zu-char value copied identically, "
                    "truncations=%llu\n",
                    ok ? "ok  " : "FAIL", fits.size(),
                    (unsigned long long)(ctp_field_truncation_count() - base));
        if (!ok) ++failures;
    }

    // A realistic case: a Chinese instrument name is 2 bytes/char in
    // GB2312 but 3 in UTF-8, so a name that fits the CTP field on the
    // wire no longer fits after conversion — the round-trip that makes
    // this overflow reachable with entirely valid exchange data.
    {
        const size_t name_cap = sizeof(CThostFtdcInstrumentField::InstrumentName);
        std::string utf8;
        for (size_t i = 0; i * 3 < name_cap + 3; ++i) utf8 += "沪";
        std::printf("note        — InstrumentName capacity=%zu; %zu GB2312 "
                    "chars (%zu bytes on the wire) become %zu bytes as UTF-8\n",
                    name_cap, utf8.size() / 3, utf8.size() / 3 * 2, utf8.size());
    }

    // ── PART 6/7 — 字节块族(memcpy)是同一个 abort 类 ──────────────
    {
        const size_t cap = sizeof(CThostFtdcRspUserLogin2Field::RandomString);
        const std::string over(cap + 8, 'Z');

        int st_old = run_child(VARIANT_MEMCPY, over);
        if (died_on_abort(st_old)) {
            std::printf("PART 6 ok   — memcpy(%zu bytes → char[%zu]) 同样以 "
                        "SIGABRT 死亡(__memcpy_chk)\n", over.size(), cap);
        } else {
            std::printf("PART 6 FAIL — memcpy 未 abort(status=%d);字节块族的 "
                        "假设不成立,不要据此改代码\n", st_old);
            ++failures;
        }

        int st_new = run_child(VARIANT_BYTES, over);
        if (WIFEXITED(st_new) && WEXITSTATUS(st_new) == 0) {
            std::printf("PART 7 ok   — ctp_set_bytes 同样输入下存活\n");
        } else {
            std::printf("PART 7 FAIL — ctp_set_bytes 未干净退出(status=%d)\n",
                        st_new);
            ++failures;
        }
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
