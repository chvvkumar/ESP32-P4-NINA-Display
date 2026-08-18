/* Host test for main/image_probe.h -- magic-byte sniffing and PNG/GIF header
 * dimension reads used to gate fetched payloads before they reach the decoder
 * (main/jpeg_utils.c image_probe_format_dims(), main/goes_client.c). No
 * ESP-IDF dependency; assert-style like test/host/test_poll_backoff.c. */
#include "image_probe.h"
#include <stdio.h>

static int fails = 0;

static const char *fmt_name(img_fmt_t f) {
    switch (f) {
    case IMG_FMT_JPEG: return "JPEG";
    case IMG_FMT_PNG:  return "PNG";
    case IMG_FMT_GIF:  return "GIF";
    default:           return "UNKNOWN";
    }
}

static void check_fmt(const char *label, img_fmt_t got, img_fmt_t expect) {
    printf("%-62s got=%-8s expect=%-8s %s\n", label, fmt_name(got), fmt_name(expect),
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

static void check_u32(const char *label, uint32_t got, uint32_t expect) {
    printf("%-62s got=%-10u expect=%-10u %s\n", label, got, expect,
           got == expect ? "OK" : "FAIL");
    if (got != expect) fails++;
}

/* Probe helper: runs the sniff and checks format + both dimensions at once. */
static void check_all(const char *label, const uint8_t *data, size_t size,
                      img_fmt_t expect_fmt, uint32_t expect_w, uint32_t expect_h) {
    uint32_t w = 0xDEADBEEF, h = 0xDEADBEEF; /* poisoned: must be overwritten */
    img_fmt_t got = image_probe_sniff(data, size, &w, &h);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s [fmt]", label);
    check_fmt(buf, got, expect_fmt);
    snprintf(buf, sizeof(buf), "%s [w]", label);
    check_u32(buf, w, expect_w);
    snprintf(buf, sizeof(buf), "%s [h]", label);
    check_u32(buf, h, expect_h);
}

/* -- fixtures -------------------------------------------------------------
 * PNG: 8-byte signature, IHDR chunk length + type, then big-endian u32
 * width @16 and height @20. 600x550 is the NWS RIDGE-2 radar geometry.  */
static const uint8_t png_600x550[24] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, /* signature */
    0x00, 0x00, 0x00, 0x0D,                         /* IHDR length = 13 */
    'I',  'H',  'D',  'R',
    0x00, 0x00, 0x02, 0x58,                         /* width  = 600 */
    0x00, 0x00, 0x02, 0x26,                         /* height = 550 */
};

/* Asymmetric across every byte: any byte-order slip changes the value. */
static const uint8_t png_asym[24] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    0x00, 0x00, 0x00, 0x0D,
    'I',  'H',  'D',  'R',
    0x01, 0x02, 0x03, 0x04,                         /* width  = 0x01020304 */
    0x0A, 0x0B, 0x0C, 0x0D,                         /* height = 0x0A0B0C0D */
};

/* Endianness regression: 0x00000100 = 256 big-endian, but 0x00010000 = 65536
 * if the four bytes were read the other way round. Both are plausible pixel
 * counts, so a wrong answer here cannot be mistaken for a bounds failure. */
static const uint8_t png_endian_trap[24] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    0x00, 0x00, 0x00, 0x0D,
    'I',  'H',  'D',  'R',
    0x00, 0x00, 0x01, 0x00,                         /* width  = 256, not 65536 */
    0x00, 0x00, 0x08, 0x00,                         /* height = 2048, not 8 */
};

/* PNG signature with one byte wrong (0x4E -> 0x4F: "PNG" -> "POG"). */
static const uint8_t png_bad_sig[24] = {
    0x89, 0x50, 0x4F, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    0x00, 0x00, 0x00, 0x0D,
    'I',  'H',  'D',  'R',
    0x00, 0x00, 0x02, 0x58,
    0x00, 0x00, 0x02, 0x26,
};

/* GIF: "GIF87a"/"GIF89a", then little-endian u16 width @6 and height @8.
 * 600 = 0x0258 -> 58 02; 550 = 0x0226 -> 26 02. */
static const uint8_t gif87a_600x550[13] = {
    'G', 'I', 'F', '8', '7', 'a',
    0x58, 0x02,             /* width  = 600 */
    0x26, 0x02,             /* height = 550 */
    0xF7, 0x00, 0x00,       /* packed fields, bg index, aspect */
};

static const uint8_t gif89a_600x550[13] = {
    'G', 'I', 'F', '8', '9', 'a',
    0x58, 0x02,
    0x26, 0x02,
    0xF7, 0x00, 0x00,
};

/* "GIF8" with the version bytes wrong ('5' is not '7'/'9'). */
static const uint8_t gif_bad_version[13] = {
    'G', 'I', 'F', '8', '5', 'a',
    0x58, 0x02,
    0x26, 0x02,
    0xF7, 0x00, 0x00,
};

/* "GIF8" followed by a valid digit but no trailing 'a'. */
static const uint8_t gif_bad_suffix[13] = {
    'G', 'I', 'F', '8', '9', 'b',
    0x58, 0x02,
    0x26, 0x02,
    0xF7, 0x00, 0x00,
};

/* JPEG SOI + APP0/JFIF. Dimensions live behind the SOF walk, which is
 * jpeg_utils.c's job -- image_probe_sniff() must report 0x0 here. */
static const uint8_t jpeg_hdr[20] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F',
    0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
};

/* What an origin server actually returns instead of an image. */
static const uint8_t html_doctype[] = "<!DOCTYPE html>\n<html><body>404</body></html>";
static const uint8_t html_bare[]    = "<html><head><title>Error</title></head></html>";
static const uint8_t json_error[]   = "{\"error\":\"not found\",\"status\":404}";

int main(void) {
    /* -- happy path: each format is recognised ----------------------------- */
    check_all("JPEG: SOI FF D8 (dims deferred to SOF walker)",
              jpeg_hdr, sizeof(jpeg_hdr), IMG_FMT_JPEG, 0, 0);
    check_all("PNG: 600x550 (RIDGE-2 radar geometry)",
              png_600x550, sizeof(png_600x550), IMG_FMT_PNG, 600, 550);
    check_all("PNG: asymmetric 0x01020304 x 0x0A0B0C0D",
              png_asym, sizeof(png_asym), IMG_FMT_PNG, 0x01020304u, 0x0A0B0C0Du);
    check_all("GIF87a: 600x550 little-endian",
              gif87a_600x550, sizeof(gif87a_600x550), IMG_FMT_GIF, 600, 550);
    check_all("GIF89a: 600x550 little-endian",
              gif89a_600x550, sizeof(gif89a_600x550), IMG_FMT_GIF, 600, 550);

    /* -- endianness regression ---------------------------------------------
     * PNG is big-endian: 00 00 01 00 is 256, NOT 65536. GIF is little-endian:
     * 58 02 is 600, NOT 0x5802 (22530). Both wrong answers are plausible
     * image sizes, which is the point. */
    check_all("PNG endian: big-endian 256x2048, not 65536x8",
              png_endian_trap, sizeof(png_endian_trap), IMG_FMT_PNG, 256, 2048);
    {
        uint32_t w = 0, h = 0;
        image_probe_sniff(gif89a_600x550, sizeof(gif89a_600x550), &w, &h);
        check_u32("GIF endian: width 600, not byte-swapped 22530", w, 600);
        check_u32("GIF endian: height 550, not byte-swapped 9730", h, 550);
    }

    /* -- non-images must be rejected outright ------------------------------- */
    check_all("reject: <!DOCTYPE html>", html_doctype, sizeof(html_doctype) - 1,
              IMG_FMT_UNKNOWN, 0, 0);
    check_all("reject: <html>", html_bare, sizeof(html_bare) - 1,
              IMG_FMT_UNKNOWN, 0, 0);
    check_all("reject: JSON error body", json_error, sizeof(json_error) - 1,
              IMG_FMT_UNKNOWN, 0, 0);

    /* -- near-miss magics ---------------------------------------------------- */
    check_all("near-miss: PNG signature with one byte wrong",
              png_bad_sig, sizeof(png_bad_sig), IMG_FMT_UNKNOWN, 0, 0);
    check_all("near-miss: GIF8 then bad version digit",
              gif_bad_version, sizeof(gif_bad_version), IMG_FMT_UNKNOWN, 0, 0);
    check_all("near-miss: GIF89 without trailing 'a'",
              gif_bad_suffix, sizeof(gif_bad_suffix), IMG_FMT_UNKNOWN, 0, 0);
    check_all("near-miss: bare \"GIF8\", nothing after it",
              gif87a_600x550, 4, IMG_FMT_UNKNOWN, 0, 0);

    /* -- NULL data ------------------------------------------------------------ */
    check_all("NULL data pointer", NULL, 1024, IMG_FMT_UNKNOWN, 0, 0);
    check_all("NULL data, size 0", NULL, 0, IMG_FMT_UNKNOWN, 0, 0);
    check_fmt("NULL out params do not crash",
              image_probe_sniff(png_600x550, sizeof(png_600x550), NULL, NULL),
              IMG_FMT_PNG);

    /* -- truncation: every prefix length must be safe and yield 0 dims -------
     * Below 4 bytes nothing is even looked at. A matched magic with a short
     * header keeps the format (the caller then skips its size cap) but must
     * still report 0x0 rather than reading past the buffer. */
    static const size_t trunc[] = { 0, 1, 3, 5, 9, 23 };
    for (size_t i = 0; i < sizeof(trunc) / sizeof(trunc[0]); i++) {
        size_t n = trunc[i];
        char label[128];

        /* JPEG: SOI is 2 bytes but the 4-byte floor gates everything. */
        snprintf(label, sizeof(label), "trunc JPEG size=%zu", n);
        check_all(label, jpeg_hdr, n, n >= 4 ? IMG_FMT_JPEG : IMG_FMT_UNKNOWN, 0, 0);

        /* PNG: needs 8 bytes to match, 24 to read dimensions. */
        snprintf(label, sizeof(label), "trunc PNG size=%zu", n);
        check_all(label, png_600x550, n, n >= 8 ? IMG_FMT_PNG : IMG_FMT_UNKNOWN,
                  n >= 24 ? 600u : 0u, n >= 24 ? 550u : 0u);

        /* GIF: needs 6 bytes to match, 10 to read dimensions. */
        snprintf(label, sizeof(label), "trunc GIF size=%zu", n);
        check_all(label, gif89a_600x550, n, n >= 6 ? IMG_FMT_GIF : IMG_FMT_UNKNOWN,
                  n >= 10 ? 600u : 0u, n >= 10 ? 550u : 0u);
    }

    /* -- exact boundary sizes, either side of each read ---------------------- */
    check_all("PNG at exactly 23 bytes: format kept, dims 0",
              png_600x550, 23, IMG_FMT_PNG, 0, 0);
    check_all("PNG at exactly 24 bytes: dims readable",
              png_600x550, 24, IMG_FMT_PNG, 600, 550);
    check_all("GIF at exactly 9 bytes: format kept, dims 0",
              gif89a_600x550, 9, IMG_FMT_GIF, 0, 0);
    check_all("GIF at exactly 10 bytes: dims readable",
              gif89a_600x550, 10, IMG_FMT_GIF, 600, 550);

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
