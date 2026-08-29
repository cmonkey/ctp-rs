#pragma once

// Bounded assignment into CTP fixed-width struct fields.
//
// Why this exists
// ---------------
// Every CThostFtdc*Field member is a fixed `char[N]`. Converter.cpp
// fills them from Rust-side strings at ~2900 call sites. Doing that
// with plain strcpy() is a latent process abort: this wrapper is
// compiled optimised, and the toolchain then rewrites strcpy() into a
// destination of known size as __strcpy_chk(), which calls abort() the
// moment the source is longer than the field.
//
// That is not theoretical. It killed the production engine mid
// night-session on 2026-08-28 22:04:00 CST, 21 minutes into the
// session, leaving live positions unmanaged until systemd restarted
// the unit 10 seconds later:
//
//     *** buffer overflow detected ***: terminated
//     ctp-tick-engine.service: Main process exited, code=dumped,
//                              status=6/ABRT
//
// The shipped binary imports __strcpy_chk, confirming fortification was
// active; wrapper/tests/test_ctp_field_overflow.cpp reproduces the
// abort byte-for-byte under the same flags and proves this header
// prevents it.
//
// Reachable with entirely valid exchange data: a Chinese instrument
// name is 2 bytes per character in GB2312 but 3 in UTF-8, so an
// 8-character name occupying 16 bytes on the wire becomes 24 bytes
// after conversion — past the 21-byte InstrumentName field it came
// from.
//
// Why truncate rather than abort
// ------------------------------
// An over-long field is rejected by CTP; an aborted process abandons
// live positions. Truncation is strictly the safer failure. It must
// never be silent, though — every occurrence names the converter
// function and the field, so the real defect stays findable instead of
// hiding behind a process that merely stopped crashing.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string.h>  // strnlen is POSIX, not std::

// Reports a truncation. Loud for the first occurrences, then only on
// powers of two, so a hot-path field cannot flood the journal.
inline std::atomic<std::uint64_t> &ctp_field_truncation_counter() {
    static std::atomic<std::uint64_t> seen{0};
    return seen;
}

// Total truncations since process start. Zero is the expected steady
// state; any non-zero value is a live defect that used to be a crash.
inline std::uint64_t ctp_field_truncation_count() {
    return ctp_field_truncation_counter().load(std::memory_order_relaxed);
}

inline void ctp_report_field_truncation(const char *fn, const char *field,
                                        std::size_t src_len,
                                        std::size_t capacity) {
    const std::uint64_t n =
        ctp_field_truncation_counter().fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || (n & (n - 1)) == 0) {
        std::fprintf(stderr,
                     "[ctp-rs] CTP field truncated: %s %s src_len=%zu "
                     "capacity=%zu (occurrence %llu) — this value would have "
                     "aborted the process under _FORTIFY_SOURCE\n",
                     fn, field, src_len, capacity,
                     static_cast<unsigned long long>(n));
        std::fflush(stderr);
    }
}

// Copies `src` into the fixed-width field `dst`, always NUL-terminated
// and never one byte past the array.
//
// Binding `char (&)[N]` rather than `char *` is deliberate: a
// destination whose size is not known at compile time will not compile
// here, instead of silently reintroducing the unbounded copy.
template <std::size_t N>
inline void ctp_set_field(char (&dst)[N], const char *src, const char *fn,
                          const char *field) {
    static_assert(N > 0, "CTP field must be a non-empty char array");
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    // strnlen stops at the field capacity, so an unterminated source
    // is not over-read either.
    const std::size_t len = ::strnlen(src, N);
    if (len == N) {  // no room left for the terminator → truncate
        std::memcpy(dst, src, N - 1);
        dst[N - 1] = '\0';
        ctp_report_field_truncation(fn, field, std::strlen(src), N);
        return;
    }
    std::memcpy(dst, src, len);
    dst[len] = '\0';
}

// Copies a **byte-blob** field (Rust side is `Vec<u8>`, C side a fixed
// `char[N]`) with the same bound + report discipline as `ctp_set_field`.
//
// 与上面的字符串族是同一个 abort 类,2026-08-29 补齐。Converter.cpp 里约
// 130 处写的是
//
//     memcpy(y.RandomString, x.RandomString.data(), x.RandomString.size());
//
// 目标 `TThostFtdcRandomStringType` 是 `char[17]`,源长度却完全由 Rust 侧
// 决定。超长时 __memcpy_chk 同样 abort 整个进程 —— 与杀死生产引擎的
// __strcpy_chk 一模一样,只是当初收敛 strcpy 时把这一族漏了。
//
// 不强制补 NUL:调用方拷贝前对整个结构做过 memset(0),未填满时天然以 0
// 结尾;填满时按 CTP 定长语义本就无终止符。
template <std::size_t N, typename Bytes>
inline void ctp_set_bytes(char (&dst)[N], const Bytes &src, const char *fn,
                          const char *field) {
    static_assert(N > 0, "CTP field must be a non-empty char array");
    const std::size_t len = src.size();
    if (len > N) {
        std::memcpy(dst, src.data(), N);
        ctp_report_field_truncation(fn, field, len, N);
        return;
    }
    std::memcpy(dst, src.data(), len);
}

// Drop-in for `memcpy(y.Field, x.Field.data(), x.Field.size() * sizeof(uint8_t))`.
#define CTP_SET_BYTES(dst, src) ctp_set_bytes((dst), (src), __func__, #dst)

// Drop-in for `strcpy(y.Field, x.Field.c_str())`. __func__ expands at
// the call site, so the report carries the converter's name.
#define CTP_SET_FIELD(dst, src) ctp_set_field((dst), (src), __func__, #dst)
