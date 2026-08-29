// SUPERSEDED (2026-08-29) — the `gb2312_to_utf8(src, dst, len)` helper
// this file exercises no longer exists. Converter::Gb2312ToUtf8 now
// delegates to wrapper/include/GbkDecode.h, which never copies the
// source into a scratch buffer at all, so the over-read class this test
// guards cannot return by construction. Kept as the record of the
// original SIGSEGV and of the guard-page technique used to pin it down;
// it is self-contained (it embeds its own pre-/post-fix copies) and
// still builds and passes on its own.
//
// Regression test for buffer over-read in gb2312_to_utf8.
//
// Background
// ----------
// `wrapper/src/Converter.cpp::gb2312_to_utf8` (Linux/iconv path) used
// to do:
//
//     size_t inlen = strlen(src) + 1;
//     char *inbuf = (char *)malloc(len);     // len = MAX_BUF = 3072
//     memcpy(inbuf, src, len);                // reads len bytes from src
//
// `src` is typically a CTP struct field (e.g. InstrumentID[31], up to
// ~80 bytes). The memcpy reads MAX_BUF (3072) bytes from `src` — well
// past the field's end, into adjacent struct fields / padding /
// neighbouring heap chunks. Usually harmless (those bytes are mapped),
// but SIGSEGVs when the field happens to be near a page boundary and
// the next page is unmapped (heap arena edge, mmap'd region end).
//
// Observed in production: 3 SIGSEGV crashes in 24 hours, all with
// identical stack:
//   #0 libc memcpy
//   #1 gb2312_to_utf8
//   #2 Converter::Gb2312ToUtf8
//   #3 Converter::Gb2312ToRustString
//   #4 CThostFtdcDepthMarketDataFieldToRust
//   #5 CMdSpi::OnRtnDepthMarketData
//
// What this test does
// -------------------
// Two modes, each runs an embedded copy of `gb2312_to_utf8` (no FFI /
// no CTP linking required):
//
// 1. GUARD-PAGE mode — places src at the very END of an mmap'd page,
//    the next page is PROT_NONE. If the function over-reads, memcpy
//    crosses the page boundary → SIGSEGV. We fork per sample so the
//    test runner survives the crash.
//
// 2. HEAP-MIDDLE mode — places src at the start of a 4096-byte heap
//    buffer (rest filled with 'X'). Over-read stays within mapped
//    memory → no SIGSEGV. Also verifies the iconv OUTPUT matches
//    what we'd expect for the input (round-trip correctness).
//
// Pre-fix expectation: GUARD-PAGE → SEGV for every input;
//                     HEAP-MIDDLE → OK + correct output.
// Post-fix expectation: BOTH modes → OK + correct output.
//
// Build (standalone, no CTP linking):
//   g++ -O2 -Wall -o test_gb2312_overread test_gb2312_overread.cpp
//
// Run:
//   ./test_gb2312_overread
//
// Exit codes:
//   0 — all assertions pass
//   1 — one or more failures (details on stderr)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iconv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

// ─── EMBEDDED COPY of the function under test ────────────────────────
// Lifted verbatim from wrapper/src/Converter.cpp:31-79 (pre-fix). The
// fix is applied to the source file; this test imports a snapshot and
// can be re-pointed at the fixed version (rename FN_UNDER_TEST).
//
// Test harness pattern: include a copy here so a fresh build can run
// without depending on the cxx-bridge auto-generated headers. The
// real fix lands in Converter.cpp; this test verifies behavior.

#define MAX_BUF 3072

// ALWAYS calls the (potentially buggy) inline version we ship in this
// file. To verify the fixed Converter.cpp, point the include below at
// the real source instead of the embedded copy and rebuild.
static void gb2312_to_utf8_under_test(const char *src, char *dst, int len);

// ─────────────────────────────────────────────────────────────────────

enum LayoutMode {
    LAYOUT_GUARD_PAGE,   // src at end of page, next page PROT_NONE
    LAYOUT_HEAP_MIDDLE,  // src in middle of 4 KiB heap buf
};

// One sample run in a forked child. Returns the child's wait status.
// Pipes the OUTPUT (dst) back to the parent on success so we can check
// round-trip correctness.
static int run_sample(const std::string& id, LayoutMode mode,
                      char* out_dst, size_t out_dst_size) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        // Suppress core dump generation for this child. Guard-page
        // mode intentionally SEGVs the pre-fix code, which would
        // otherwise spam /var/lib/systemd/coredump (~50KB per fork
        // × N samples × repeat runs → fills disk + journalctl).
        // Setting PR_SET_DUMPABLE to 0 makes the kernel skip core
        // file creation but the signal still propagates to the
        // parent's waitpid as SIGSEGV — exactly what we want.
        prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
        close(pipefd[0]);
        long page = sysconf(_SC_PAGESIZE);
        size_t need = id.size() + 1;
        char* src = nullptr;
        if (mode == LAYOUT_GUARD_PAGE) {
            char* base = (char*)mmap(nullptr, 2 * page,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (base == MAP_FAILED) _exit(2);
            if (mprotect(base + page, page, PROT_NONE) != 0) _exit(3);
            if (need > (size_t)page) _exit(4);
            src = base + page - need;
            memcpy(src, id.c_str(), need);
        } else {
            char* buf = (char*)malloc(4096);
            if (!buf) _exit(2);
            memset(buf, 'X', 4096);
            memcpy(buf, id.c_str(), need);
            src = buf;
        }
        char dst[MAX_BUF];
        memset(dst, 0, sizeof(dst));
        gb2312_to_utf8_under_test(src, dst, MAX_BUF);
        // Send the produced dst back to parent (NUL-terminated string).
        size_t to_send = strnlen(dst, sizeof(dst));
        write(pipefd[1], dst, to_send);
        close(pipefd[1]);
        _exit(0);
    }
    close(pipefd[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        ssize_t n = read(pipefd[0], out_dst, out_dst_size - 1);
        if (n < 0) n = 0;
        out_dst[n] = '\0';
    }
    close(pipefd[0]);
    return status;
}

// CTP InstrumentIDs cover a length range of ~5..15 chars (sampled
// from a 18 996-row Redis dump on production). One representative
// per length plus a few edge cases.
static std::vector<std::string> sample_ids() {
    return {
        "a2605",            // 5  — futures, shortest
        "ad2606",           // 6
        "FG607C810",        // 9
        "au2606C632",       // 10
        "ag2606C8000",      // 11
        "ad2606C20000",     // 12
        "bz2607-C-4800",    // 13 — dash style
        "bz2607-C-10000",   // 14
        "c2609-MS-C-2120",  // 15 — multi-strike
        "",                 // edge: empty string
        "x",                // edge: single char
    };
}

int main() {
    auto ids = sample_ids();
    size_t fail = 0;

    // ─── GUARD-PAGE mode ─────────────────────────────────────────────
    // Pre-fix: every sample → SEGV. Post-fix: every sample → OK.
    fprintf(stderr, "[guard-page] testing %zu samples ...\n", ids.size());
    for (const auto& id : ids) {
        char dst[MAX_BUF] = {0};
        int status = run_sample(id, LAYOUT_GUARD_PAGE, dst, sizeof(dst));
        bool segv = WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
        bool ok   = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (segv) {
            fprintf(stderr, "  ✘ FAIL guard-page id=%-18s → SEGV\n", id.c_str());
            fail++;
        } else if (ok) {
            // Round-trip: for ASCII InstrumentIDs (no Chinese), iconv
            // output should equal input byte-for-byte (UTF-8 of ASCII
            // is ASCII).
            if (strcmp(dst, id.c_str()) != 0) {
                fprintf(stderr, "  ✘ FAIL guard-page id=%-18s → got '%s' (expected '%s')\n",
                        id.c_str(), dst, id.c_str());
                fail++;
            } else {
                fprintf(stderr, "  ✓ guard-page id=%-18s → '%s'\n",
                        id.c_str(), dst);
            }
        } else {
            fprintf(stderr, "  ✘ FAIL guard-page id=%-18s → status=%d\n",
                    id.c_str(), status);
            fail++;
        }
    }

    // ─── HEAP-MIDDLE mode ────────────────────────────────────────────
    // Production-like layout (src has plenty of mapped slack). Should
    // never SEGV regardless of the bug; verifies output correctness.
    fprintf(stderr, "\n[heap-middle] testing %zu samples ...\n", ids.size());
    for (const auto& id : ids) {
        char dst[MAX_BUF] = {0};
        int status = run_sample(id, LAYOUT_HEAP_MIDDLE, dst, sizeof(dst));
        bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!ok) {
            fprintf(stderr, "  ✘ FAIL heap-middle id=%-18s → status=%d\n",
                    id.c_str(), status);
            fail++;
        } else if (strcmp(dst, id.c_str()) != 0) {
            fprintf(stderr, "  ✘ FAIL heap-middle id=%-18s → got '%s' (expected '%s')\n",
                    id.c_str(), dst, id.c_str());
            fail++;
        } else {
            fprintf(stderr, "  ✓ heap-middle id=%-18s → '%s'\n",
                    id.c_str(), dst);
        }
    }

    fprintf(stderr, "\n");
    if (fail == 0) {
        fprintf(stderr, "PASS — all %zu samples × 2 modes\n", ids.size());
        return 0;
    }
    fprintf(stderr, "FAIL — %zu of %zu × 2 sample-modes failed\n",
            fail, ids.size());
    return 1;
}

// ─────────────────────────────────────────────────────────────────────
// CHOOSE WHICH VERSION TO TEST.
// Toggle GB2312_FIX_APPLIED to switch between pre-fix (buggy) and
// post-fix (correct) implementations. CI/PR review can build both
// to confirm: pre-fix fails GUARD-PAGE, post-fix passes both.
// ─────────────────────────────────────────────────────────────────────

// Default 1 (post-fix). Build with -DGB2312_FIX_APPLIED=0 to re-verify
// the original bug class still reproduces under guard-page layout.
#ifndef GB2312_FIX_APPLIED
#define GB2312_FIX_APPLIED 1
#endif

static void gb2312_to_utf8_under_test(const char *src, char *dst, int len) {
    int ret = 0;
    size_t inlen = strlen(src) + 1;
    size_t outlen = len;

#if GB2312_FIX_APPLIED
    // Post-fix: malloc + memcpy bounded by the actual source length.
    char *inbuf = (char *)malloc(inlen);
    char *inbuf_hold = inbuf;
    memcpy(inbuf, src, inlen);
#else
    // Pre-fix (buggy): reads `len` bytes from src — over-read past the
    // typical 30-byte CTP field. SEGV-trigger when src is near a page
    // boundary with next page unmapped.
    char *inbuf = (char *)malloc(len);
    char *inbuf_hold = inbuf;
    memcpy(inbuf, src, len);
#endif

    char *outbuf2 = NULL;
    char *outbuf = dst;
    iconv_t cd;
    if (src == dst) {
        outbuf2 = (char *)malloc(len);
        memset(outbuf2, 0, len);
        outbuf = outbuf2;
    }
    cd = iconv_open("UTF-8", "GB2312");
    if (cd != (iconv_t)-1) {
        ret = iconv(cd, &inbuf, &inlen, &outbuf, &outlen);
        if (outbuf2 != NULL) {
            strcpy(dst, outbuf2);
            free(outbuf2);
        }
        iconv_close(cd);
    }
    if (ret != 0) dst[0] = '\0';
    free(inbuf_hold);
}
