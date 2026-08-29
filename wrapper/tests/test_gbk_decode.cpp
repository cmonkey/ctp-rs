// Regression test for the CTP string decoder.
//
// Background
// ----------
// CTP hands us codepage-936 (GBK) bytes. Converter.cpp decoded them
// with iconv_open("UTF-8", "GB2312"), and on any iconv error did:
//
//     printf("iconv failed, buf: [0x..], err: %s\n", strerror(errno));
//     dst[0] = '\0';
//
// GB2312 is a strict *subset* of GBK, so every GBK-only character was
// an EILSEQ — and the failure discarded the **entire field**, not just
// the offending character, while dumping the raw bytes to stdout.
// A separate ceiling came from the fixed `char dst_str[3072]` sink:
// input whose UTF-8 form exceeded it produced E2BIG, i.e. the same
// whole-field loss.
//
// wrapper/include/GbkDecode.h replaces both behaviours (ported from
// upstream ctp-rs 1748959, where the rewrite is welded to the 6.7.13
// upgrade and cannot be cherry-picked alone).
//
// PART 1 proves the old decoder loses the field; if it ever stops
// failing, the test says so rather than passing quietly.
//
// Build:
//   g++ -std=c++20 -O2 -fPIC -w wrapper/tests/test_gbk_decode.cpp \
//       -o /tmp/t && /tmp/t

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <iconv.h>

#include "../include/GbkDecode.h"   // the shipping implementation

// ── The decoder as it shipped before, kept verbatim as the baseline ──
#define MAX_BUF 3072

static void gb2312_to_utf8_OLD(const char *src, char *dst, int len) {
    int ret = 0;
    size_t inlen = strlen(src) + 1;
    size_t outlen = len;

    char *inbuf = (char *)malloc(inlen);
    char *inbuf_hold = inbuf;
    memcpy(inbuf, src, inlen);

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
        if (outbuf2 != NULL) { strcpy(dst, outbuf2); free(outbuf2); }
        iconv_close(cd);
    }
    if (ret != 0) dst[0] = '\0';   // ← the whole field is discarded
    free(inbuf_hold);
}

static std::string old_decode(const std::string &gbk) {
    char dst[MAX_BUF] = {0};
    gb2312_to_utf8_OLD(gbk.c_str(), dst, MAX_BUF);
    return std::string(dst);
}

// ── Helper: build GBK bytes from a UTF-8 literal ────────────────────
static std::string to_gbk(const std::string &utf8) {
    iconv_t cd = iconv_open("GBK", "UTF-8");
    if (cd == (iconv_t)-1) { std::perror("iconv_open"); std::exit(70); }
    size_t inlen = utf8.size();
    size_t outcap = inlen * 2 + 8;
    std::string out(outcap, '\0');
    char *in = const_cast<char *>(utf8.data());
    char *outp = &out[0];
    size_t outleft = outcap;
    if (iconv(cd, &in, &inlen, &outp, &outleft) == (size_t)-1) {
        std::fprintf(stderr, "to_gbk failed for a literal — bad fixture\n");
        std::exit(70);
    }
    iconv_close(cd);
    out.resize(outcap - outleft);
    return out;
}

static int failures = 0;
static void check(bool ok, const char *label, const std::string &detail) {
    std::printf("%-7s %s — %s\n", ok ? "ok" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}

int main() {
    // ── PART 1 — GBK-only characters destroyed the whole field ──────
    // 镕 (U+9555) and 堃 (U+5803) are in GBK but not GB2312.
    {
        const std::string gbk = to_gbk("朱镕基");
        const std::string got = old_decode(gbk);
        check(got.empty(), "PART 1",
              "old GB2312 decoder returned \"" + got +
                  "\" (empty = whole field discarded) for a GBK-only name");
    }

    // ── PART 2 — the port decodes it correctly ──────────────────────
    {
        const std::string gbk = to_gbk("朱镕基");
        const std::string got = gbk_to_utf8(gbk.c_str());
        check(got == "朱镕基", "PART 2",
              "gbk_to_utf8 → \"" + got + "\"");
    }

    // ── PART 3 — no regression on ordinary GB2312 content ───────────
    {
        const char *names[] = {"螺纹钢", "豆粕", "沪深300", "白银", "原油",
                               "聚氯乙烯", "铁矿石", "棕榈油"};
        bool all = true;
        std::string bad;
        for (const char *n : names) {
            const std::string gbk = to_gbk(n);
            const std::string a = old_decode(gbk);
            const std::string b = gbk_to_utf8(gbk.c_str());
            if (a != b || b != n) { all = false; bad = n; break; }
        }
        check(all, "PART 3",
              all ? "8 个常见品种名 old == new == 原文,无回归"
                  : "mismatch on " + bad);
    }

    // ── PART 4a — a byte that is valid GBK but absent from GB2312 ───
    // 0x80 is the euro sign in codepage 936. Perfectly decodable, yet
    // the GB2312 decoder threw the entire field away.
    {
        std::string gbk = to_gbk("螺纹钢");
        gbk.insert(gbk.begin() + 2, (char)0x80);

        const std::string oldv = old_decode(gbk);
        const std::string newv = gbk_to_utf8(gbk.c_str());

        const bool ok = oldv.empty() && newv == "螺\u20ac纹钢";
        check(ok, "PART 4a",
              "old → \"" + oldv + "\" (整串丢失); new → \"" + newv + "\"");
    }

    // ── PART 4b — a genuinely invalid byte no longer erases the rest ─
    // 0xFF is not a legal GBK lead byte in any position.
    {
        std::string gbk = to_gbk("螺纹钢");
        gbk.insert(gbk.begin() + 2, (char)0xFF);

        const std::string oldv = old_decode(gbk);
        const std::string newv = gbk_to_utf8(gbk.c_str());

        const bool ok = oldv.empty()
                     && newv.find("\xEF\xBF\xBD") != std::string::npos
                     && newv.find("钢") != std::string::npos;
        check(ok, "PART 4b",
              "old → \"" + oldv + "\" (整串丢失); new → \"" + newv +
                  "\" (U+FFFD 替换 + 尾部字符保留)");
    }

    // ── PART 5 — no 3072-byte ceiling ───────────────────────────────
    {
        std::string utf8;
        for (int i = 0; i < 1100; ++i) utf8 += "沪";   // 3300 UTF-8 bytes
        const std::string gbk = to_gbk(utf8);          // 2200 GBK bytes

        const std::string oldv = old_decode(gbk);
        const std::string newv = gbk_to_utf8(gbk.c_str());

        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "%zu GBK bytes → old %zu bytes (MAX_BUF=%d 撑爆), "
                      "new %zu bytes (== %zu 期望)",
                      gbk.size(), oldv.size(), MAX_BUF, newv.size(),
                      utf8.size());
        check(newv == utf8 && oldv != utf8, "PART 5", detail);
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
