#pragma once

// GBK → UTF-8 decoding for CTP string fields (POSIX/iconv path).
//
// Ported from upstream ctp-rs commit 1748959 ("feat: upgrade CTP API to
// 6.7.13"), where this rewrite is bundled with the 6.7.13 upgrade and
// therefore cannot be cherry-picked on its own. Two upstream properties
// are the reason to take it:
//
//   1. It decodes **GBK**, not GB2312. GBK is a strict superset, so
//      every byte sequence that decoded before decodes identically —
//      but GBK-only characters (镕, 堃, …) no longer fail. Under the
//      previous GB2312 decoder iconv returned EILSEQ for those and the
//      whole field came back as an empty string, with the offending
//      bytes dumped to **stdout**. CTP hands us codepage-936 (GBK)
//      data, so GB2312 was simply the wrong codec.
//
//   2. Invalid bytes are replaced per-character with U+FFFD instead of
//      discarding the entire field, matching what the Win32
//      MultiByteToWideChar(936, …) branch in Converter.cpp already did.
//      One bad byte no longer erases a whole instrument name.
//
// It is also cheaper on our hot path. The previous implementation
// declared `char dst_str[3072] = {0}` per call — a 3 KB zeroing for
// every string field of every callback — where this allocates a buffer
// sized to the input (typically tens of bytes).
//
// Kept in its own header, rather than inline in Converter.cpp, so
// wrapper/tests/test_gbk_decode.cpp exercises the shipping code rather
// than a copy of it.

#include <cerrno>
#include <cstring>
#include <string>

#include <iconv.h>

inline std::string gbk_to_utf8(const char *src_str) {
    if (src_str == nullptr)
        return std::string();

    // GBK (superset of GB2312) keeps parity with the Win32 codepage-936 path.
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == (iconv_t)-1)
        return std::string();

    // U+FFFD, matching the Win32 default substitution for invalid bytes.
    static const char REPL[] = "\xEF\xBF\xBD";
    const size_t REPL_LEN = 3;

    size_t inlen = std::strlen(src_str);  // do not include the null
    size_t outcap = inlen * 4 + 1;        // safe upper bound for GBK -> UTF-8
    std::string out(outcap, '\0');

    char *inbuf = const_cast<char *>(src_str);
    char *outbuf = &out[0];
    size_t outleft = outcap;

    while (inlen > 0) {
        size_t ret = iconv(cd, &inbuf, &inlen, &outbuf, &outleft);
        if (ret != (size_t)-1)
            break;  // all input consumed

        if (errno == E2BIG) {
            size_t used = outcap - outleft;
            outcap *= 2;
            out.resize(outcap);  // may reallocate; rebase below
            outbuf = &out[0] + used;
            outleft = outcap - used;
            continue;
        }

        // EILSEQ (invalid byte) or EINVAL (truncated tail): best-effort --
        // emit U+FFFD, skip one input byte, and keep going.
        if (outleft < REPL_LEN) {
            size_t used = outcap - outleft;
            outcap *= 2;
            out.resize(outcap);
            outbuf = &out[0] + used;
            outleft = outcap - used;
        }
        std::memcpy(outbuf, REPL, REPL_LEN);
        outbuf += REPL_LEN;
        outleft -= REPL_LEN;
        ++inbuf;
        --inlen;
    }

    iconv_close(cd);

    out.resize(outcap - outleft);  // shrink to actual bytes written
    return out;
}
