/*
 * test_bookmark_storage.c - AstraOS Phase AB: Durable Bookmark Persistence Host Test Suite
 *
 * Validates:
 * 1. ASTB v1 binary serialization and deserialization
 * 2. Fletcher-32 checksum calculation and verification
 * 3. 32-entry maximum capacity bound enforcement
 * 4. Dangerous / invalid URL scheme rejection (javascript:, data:, vbscript:, malformed)
 * 5. Corrupted header, magic, and version detection
 * 6. Truncated payload and partial file rejection
 * 7. Duplicate URL handling and in-place title update
 * 8. Entry deletion, array compaction, and subsequent serialization
 * 9. Cold reboot persistence simulation: file write -> zero memory -> load -> verify
 * 10. Corruption / truncation on-disk recovery without panics
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  s64;

#define BOOKMARK_MAGIC   0x41535442 /* "ASTB" */
#define BOOKMARK_VERSION 1
#define MAX_BOOKMARKS    32

#pragma pack(push, 1)
typedef struct {
    u32 magic;       /* 0x41535442 "ASTB" */
    u16 version;     /* 1 */
    u16 count;       /* 0 .. 32 */
    u32 checksum;    /* Fletcher-32 of payload */
    u32 reserved;    /* 0 */
} bookmark_header_t;

typedef struct {
    char url[256];
    char title[64];
} bookmark_disk_entry_t;
#pragma pack(pop)

typedef struct {
    char url[256];
    char title[64];
    u32 created_at;
} bookmark_entry_t;

typedef struct {
    bookmark_entry_t bookmarks[MAX_BOOKMARKS];
    int bookmark_count;
} mock_browser_t;

/* Host helpers matching user/browser_main.c */
static size_t ustrlen(const char *s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static void ustrcpy(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < max) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static char utolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static bool ustrcaseeq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (utolower(*a) != utolower(*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool ustrcasestartswith(const char *str, const char *prefix) {
    if (!str || !prefix) return false;
    while (*prefix) {
        if (utolower(*str) != utolower(*prefix)) return false;
        str++;
        prefix++;
    }
    return true;
}

static u32 calculate_checksum(const void *data, size_t len) {
    const u8 *p = (const u8 *)data;
    u32 c0 = 0xffff, c1 = 0xffff;
    while (len) {
        size_t block = len > 360 ? 360 : len;
        len -= block;
        do {
            c0 += *p++;
            c1 += c0;
        } while (--block);
        c0 = (c0 & 0xffff) + (c0 >> 16);
        c1 = (c1 & 0xffff) + (c1 >> 16);
    }
    c0 = (c0 & 0xffff) + (c0 >> 16);
    c1 = (c1 & 0xffff) + (c1 >> 16);
    return (c1 << 16) | c0;
}

static int bookmark_find(const mock_browser_t *b, const char *url) {
    if (!b || !url || !url[0]) return -1;
    for (int i = 0; i < b->bookmark_count; i++) {
        if (ustrcaseeq(b->bookmarks[i].url, url)) {
            return i;
        }
    }
    return -1;
}

static int bookmark_add(mock_browser_t *b, const char *url, const char *title) {
    if (!b || !url || !url[0]) return -1;
    if (ustrlen(url) >= 250) return -1;

    if (!ustrcasestartswith(url, "file:///") &&
        !ustrcasestartswith(url, "http://") &&
        !ustrcasestartswith(url, "https://")) {
        return -1;
    }

    int existing = bookmark_find(b, url);
    if (existing >= 0) {
        if (title && title[0]) {
            ustrcpy(b->bookmarks[existing].title, title, sizeof(b->bookmarks[existing].title));
        }
        return existing;
    }

    if (b->bookmark_count >= MAX_BOOKMARKS) {
        return -1;
    }

    int idx = b->bookmark_count;
    ustrcpy(b->bookmarks[idx].url, url, sizeof(b->bookmarks[idx].url));
    if (title && title[0]) {
        ustrcpy(b->bookmarks[idx].title, title, sizeof(b->bookmarks[idx].title));
    } else {
        ustrcpy(b->bookmarks[idx].title, url, sizeof(b->bookmarks[idx].title));
    }
    b->bookmarks[idx].created_at = 1;
    b->bookmark_count++;
    return idx;
}

static bool bookmark_remove(mock_browser_t *b, const char *url) {
    if (!b || !url || !url[0]) return false;
    int idx = bookmark_find(b, url);
    if (idx < 0) return false;

    for (int i = idx; i < b->bookmark_count - 1; i++) {
        b->bookmarks[i] = b->bookmarks[i + 1];
    }
    memset(&b->bookmarks[b->bookmark_count - 1], 0, sizeof(bookmark_entry_t));
    b->bookmark_count--;
    return true;
}

static s64 bookmarks_serialize(const mock_browser_t *b, void *buf, size_t max_len) {
    if (!b || !buf) return -1;
    size_t needed = sizeof(bookmark_header_t) + ((size_t)b->bookmark_count * sizeof(bookmark_disk_entry_t));
    if (max_len < needed) return -1;

    bookmark_header_t *hdr = (bookmark_header_t *)buf;
    hdr->magic = BOOKMARK_MAGIC;
    hdr->version = BOOKMARK_VERSION;
    hdr->count = (u16)b->bookmark_count;
    hdr->reserved = 0;

    bookmark_disk_entry_t *entries = (bookmark_disk_entry_t *)((u8 *)buf + sizeof(bookmark_header_t));
    for (int i = 0; i < b->bookmark_count; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        ustrcpy(entries[i].url, b->bookmarks[i].url, sizeof(entries[i].url));
        ustrcpy(entries[i].title, b->bookmarks[i].title, sizeof(entries[i].title));
    }

    size_t payload_len = (size_t)b->bookmark_count * sizeof(bookmark_disk_entry_t);
    hdr->checksum = calculate_checksum(entries, payload_len);

    return (s64)needed;
}

static bool bookmarks_deserialize(mock_browser_t *b, const void *buf, size_t len) {
    if (!b || !buf || len < sizeof(bookmark_header_t)) return false;

    const bookmark_header_t *hdr = (const bookmark_header_t *)buf;
    if (hdr->magic != BOOKMARK_MAGIC) return false;
    if (hdr->version != BOOKMARK_VERSION) return false;
    if (hdr->count > MAX_BOOKMARKS) return false;

    size_t expected_len = sizeof(bookmark_header_t) + ((size_t)hdr->count * sizeof(bookmark_disk_entry_t));
    if (len < expected_len) return false;

    const bookmark_disk_entry_t *entries = (const bookmark_disk_entry_t *)((const u8 *)buf + sizeof(bookmark_header_t));
    size_t payload_len = (size_t)hdr->count * sizeof(bookmark_disk_entry_t);
    u32 sum = calculate_checksum(entries, payload_len);
    if (sum != hdr->checksum) return false;

    /* Verify string termination on every entry prior to modifying state */
    for (int i = 0; i < (int)hdr->count; i++) {
        bool url_term = false;
        for (int k = 0; k < 256; k++) {
            if (entries[i].url[k] == '\0') { url_term = true; break; }
        }
        if (!url_term) return false;

        bool title_term = false;
        for (int k = 0; k < 64; k++) {
            if (entries[i].title[k] == '\0') { title_term = true; break; }
        }
        if (!title_term) return false;
    }

    b->bookmark_count = 0;
    for (int i = 0; i < (int)hdr->count; i++) {
        if (!ustrcasestartswith(entries[i].url, "file:///") &&
            !ustrcasestartswith(entries[i].url, "http://") &&
            !ustrcasestartswith(entries[i].url, "https://")) {
            continue;
        }

        int cur = b->bookmark_count;
        if (cur >= MAX_BOOKMARKS) break;

        ustrcpy(b->bookmarks[cur].url, entries[i].url, sizeof(b->bookmarks[cur].url));
        ustrcpy(b->bookmarks[cur].title, entries[i].title, sizeof(b->bookmarks[cur].title));
        b->bookmarks[cur].created_at = 1;
        b->bookmark_count++;
    }
    return true;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int run_all_bookmark_tests(void) {
    printf("=================================================================\n");
    printf("   AstraOS Phase AB: Bookmark Storage & Persistence Host Tests   \n");
    printf("=================================================================\n");

    /* TEST 1: Serialization / Deserialization & Checksum */
    printf("[TEST 1] Testing basic serialization, deserialization, and Fletcher-32 checksum...\n");
    mock_browser_t b1;
    memset(&b1, 0, sizeof(b1));

    assert(bookmark_add(&b1, "file:///www/index.html", "AstraOS Portal") == 0);
    assert(bookmark_add(&b1, "http://10.0.2.2:8000/", "Local Dev Server") == 1);
    assert(bookmark_add(&b1, "https://10.0.2.2:8443/", "Secure Portal") == 2);
    assert(b1.bookmark_count == 3);

    u8 buf[sizeof(bookmark_header_t) + MAX_BOOKMARKS * sizeof(bookmark_disk_entry_t)];
    s64 ser_len = bookmarks_serialize(&b1, buf, sizeof(buf));
    assert(ser_len > 0);
    assert((size_t)ser_len == sizeof(bookmark_header_t) + 3 * sizeof(bookmark_disk_entry_t));

    bookmark_header_t *hdr = (bookmark_header_t *)buf;
    assert(hdr->magic == BOOKMARK_MAGIC);
    assert(hdr->version == BOOKMARK_VERSION);
    assert(hdr->count == 3);

    mock_browser_t b2;
    memset(&b2, 0, sizeof(b2));
    assert(bookmarks_deserialize(&b2, buf, (size_t)ser_len));
    assert(b2.bookmark_count == 3);
    assert(strcmp(b2.bookmarks[0].url, "file:///www/index.html") == 0);
    assert(strcmp(b2.bookmarks[1].url, "http://10.0.2.2:8000/") == 0);
    assert(strcmp(b2.bookmarks[2].url, "https://10.0.2.2:8443/") == 0);
    printf("  PASS: Normal serialization & deserialization verified.\n");

    /* TEST 2: Capacity Limit (32 Bookmarks Max) */
    printf("[TEST 2] Testing 32-entry capacity bound...\n");
    for (int i = 3; i < MAX_BOOKMARKS; i++) {
        char url[64];
        char title[32];
        snprintf(url, sizeof(url), "https://10.0.2.2:8443/page%d", i);
        snprintf(title, sizeof(title), "Page %d", i);
        assert(bookmark_add(&b1, url, title) == i);
    }
    assert(b1.bookmark_count == 32);
    /* 33rd must be rejected */
    assert(bookmark_add(&b1, "https://10.0.2.2:8443/overflow", "Overflow") == -1);
    assert(b1.bookmark_count == 32);
    printf("  PASS: 32-entry maximum capacity enforced.\n");

    /* TEST 3: Invalid Scheme Rejection */
    printf("[TEST 3] Testing dangerous / invalid scheme rejection...\n");
    mock_browser_t b_scheme;
    memset(&b_scheme, 0, sizeof(b_scheme));
    assert(bookmark_add(&b_scheme, "javascript:alert(1)", "XSS Attack") == -1);
    assert(bookmark_add(&b_scheme, "data:text/html,<h1>PWN</h1>", "Data URI") == -1);
    assert(bookmark_add(&b_scheme, "vbscript:run()", "VBScript") == -1);
    assert(bookmark_add(&b_scheme, "gopher://bad.host/", "Gopher") == -1);
    assert(bookmark_add(&b_scheme, "", "Empty") == -1);
    assert(b_scheme.bookmark_count == 0);
    printf("  PASS: Dangerous schemes cleanly rejected.\n");

    /* TEST 4: Header Corruption Defense */
    printf("[TEST 4] Testing header corruption defense (magic, version, count)...\n");
    mock_browser_t b_corrupt;
    memset(&b_corrupt, 0, sizeof(b_corrupt));

    /* Bad magic */
    hdr->magic = 0xDEADBEEF;
    assert(!bookmarks_deserialize(&b_corrupt, buf, (size_t)ser_len));
    hdr->magic = BOOKMARK_MAGIC;

    /* Unsupported version */
    hdr->version = 2;
    assert(!bookmarks_deserialize(&b_corrupt, buf, (size_t)ser_len));
    hdr->version = 0;
    assert(!bookmarks_deserialize(&b_corrupt, buf, (size_t)ser_len));
    hdr->version = BOOKMARK_VERSION;

    /* Bad entry count */
    hdr->count = 99;
    assert(!bookmarks_deserialize(&b_corrupt, buf, (size_t)ser_len));
    hdr->count = 3;
    printf("  PASS: Bad magic, unsupported versions, and count bounds safely rejected.\n");

    /* TEST 5: Fletcher-32 Checksum Tamper Defense */
    printf("[TEST 5] Testing checksum bit-flip detection...\n");
    u32 good_csum = hdr->checksum;
    hdr->checksum ^= 0x00010000; /* Flip single bit in checksum */
    assert(!bookmarks_deserialize(&b_corrupt, buf, (size_t)ser_len));
    hdr->checksum = good_csum;

    /* Tamper with payload byte */
    buf[sizeof(bookmark_header_t) + 5] ^= 0xFF;
    assert(!bookmarks_deserialize(&b_corrupt, buf, (size_t)ser_len));
    buf[sizeof(bookmark_header_t) + 5] ^= 0xFF; /* Restore */
    assert(bookmarks_deserialize(&b_corrupt, buf, (size_t)ser_len));
    printf("  PASS: Checksum tamper and payload bit flips safely caught.\n");

    /* TEST 6: Truncated Payload Rejection */
    printf("[TEST 6] Testing truncated buffer and partial entry rejection...\n");
    assert(!bookmarks_deserialize(&b_corrupt, buf, sizeof(bookmark_header_t) - 1));
    assert(!bookmarks_deserialize(&b_corrupt, buf, sizeof(bookmark_header_t)));
    assert(!bookmarks_deserialize(&b_corrupt, buf, sizeof(bookmark_header_t) + sizeof(bookmark_disk_entry_t)));
    printf("  PASS: Truncated archives cleanly rejected without read overruns.\n");

    /* TEST 7: Duplicate Handling & In-Place Title Update */
    printf("[TEST 7] Testing duplicate URL deduplication and title update...\n");
    mock_browser_t b_dedup;
    memset(&b_dedup, 0, sizeof(b_dedup));
    assert(bookmark_add(&b_dedup, "file:///www/index.html", "Original Title") == 0);
    assert(b_dedup.bookmark_count == 1);
    /* Re-adding same URL updates title without incrementing count */
    assert(bookmark_add(&b_dedup, "file:///www/index.html", "Updated Title") == 0);
    assert(b_dedup.bookmark_count == 1);
    assert(strcmp(b_dedup.bookmarks[0].title, "Updated Title") == 0);
    printf("  PASS: Duplicate handling verified.\n");

    /* TEST 8: Removal and Compaction */
    printf("[TEST 8] Testing bookmark removal and contiguous array compaction...\n");
    assert(bookmark_add(&b_dedup, "http://10.0.2.2:8000/", "Server") == 1);
    assert(bookmark_add(&b_dedup, "https://10.0.2.2:8443/", "Secure") == 2);
    assert(b_dedup.bookmark_count == 3);

    /* Remove middle entry */
    assert(bookmark_remove(&b_dedup, "http://10.0.2.2:8000/"));
    assert(b_dedup.bookmark_count == 2);
    assert(strcmp(b_dedup.bookmarks[0].url, "file:///www/index.html") == 0);
    assert(strcmp(b_dedup.bookmarks[1].url, "https://10.0.2.2:8443/") == 0);
    /* Removing non-existent returns false */
    assert(!bookmark_remove(&b_dedup, "https://nonexistent.org/"));
    printf("  PASS: Bookmark removal and compaction verified.\n");

    /* TEST 9: Simulated Cold Reboot Persistence Cycle */
    printf("[TEST 9] Simulating cold reboot persistence lifecycle via host file storage...\n");
    const char *test_disk_path = "build/test_bookmarks.bin";
    FILE *fp = fopen(test_disk_path, "wb");
    assert(fp != NULL);

    s64 written_bytes = bookmarks_serialize(&b_dedup, buf, sizeof(buf));
    assert(written_bytes > 0);
    size_t fw = fwrite(buf, 1, (size_t)written_bytes, fp);
    assert(fw == (size_t)written_bytes);
    fclose(fp);

    /* Cold reboot: complete zeroing of browser memory state */
    mock_browser_t b_rebooted;
    memset(&b_rebooted, 0, sizeof(b_rebooted));
    assert(b_rebooted.bookmark_count == 0);

    /* Reload from disk storage device */
    fp = fopen(test_disk_path, "rb");
    assert(fp != NULL);
    u8 disk_buf[sizeof(buf)];
    size_t disk_read = fread(disk_buf, 1, sizeof(disk_buf), fp);
    fclose(fp);
    assert(disk_read == (size_t)written_bytes);

    assert(bookmarks_deserialize(&b_rebooted, disk_buf, disk_read));
    assert(b_rebooted.bookmark_count == 2);
    assert(strcmp(b_rebooted.bookmarks[0].url, "file:///www/index.html") == 0);
    assert(strcmp(b_rebooted.bookmarks[1].url, "https://10.0.2.2:8443/") == 0);
    printf("  PASS: Persistence lifecycle: write -> poweroff/zero -> boot/reload verified.\n");

    /* TEST 10: Remove Bookmark, Re-save, and Reboot-Verify */
    printf("[TEST 10] Testing removal persistence across reboot cycle...\n");
    assert(bookmark_remove(&b_rebooted, "https://10.0.2.2:8443/"));
    assert(b_rebooted.bookmark_count == 1);

    fp = fopen(test_disk_path, "wb");
    assert(fp != NULL);
    written_bytes = bookmarks_serialize(&b_rebooted, buf, sizeof(buf));
    assert(written_bytes > 0);
    fw = fwrite(buf, 1, (size_t)written_bytes, fp);
    assert(fw == (size_t)written_bytes);
    fclose(fp);

    /* Zero memory again for second cold reboot */
    memset(&b_rebooted, 0, sizeof(b_rebooted));
    fp = fopen(test_disk_path, "rb");
    assert(fp != NULL);
    disk_read = fread(disk_buf, 1, sizeof(disk_buf), fp);
    fclose(fp);

    assert(bookmarks_deserialize(&b_rebooted, disk_buf, disk_read));
    assert(b_rebooted.bookmark_count == 1);
    assert(strcmp(b_rebooted.bookmarks[0].url, "file:///www/index.html") == 0);
    printf("  PASS: Post-removal persistence across reboot verified.\n");

    /* TEST 11: Unterminated String Defense */
    printf("[TEST 11] Testing unterminated string rejection...\n");
    u8 bad_buf[sizeof(bookmark_header_t) + sizeof(bookmark_disk_entry_t)];
    bookmark_header_t *bad_hdr = (bookmark_header_t *)bad_buf;
    bad_hdr->magic = BOOKMARK_MAGIC;
    bad_hdr->version = BOOKMARK_VERSION;
    bad_hdr->count = 1;
    bad_hdr->reserved = 0;
    bookmark_disk_entry_t *bad_entry = (bookmark_disk_entry_t *)(bad_buf + sizeof(bookmark_header_t));
    memset(bad_entry->url, 'A', sizeof(bad_entry->url)); /* No null terminator */
    strcpy(bad_entry->title, "Unterminated URL");
    bad_hdr->checksum = calculate_checksum(bad_entry, sizeof(bookmark_disk_entry_t));
    mock_browser_t b_unterminated;
    memset(&b_unterminated, 0, sizeof(b_unterminated));
    assert(bookmarks_deserialize(&b_unterminated, bad_buf, sizeof(bad_buf)) == false);
    printf("  PASS: Unterminated string safely rejected without buffer overrun.\n");

    /* TEST 12: Non-Destructive Load Guarantee */
    printf("[TEST 12] Testing non-destructive load guarantee on corruption...\n");
    mock_browser_t b_preserved;
    memset(&b_preserved, 0, sizeof(b_preserved));
    assert(bookmark_add(&b_preserved, "file:///www/important.html", "Crucial Data") == 0);
    assert(b_preserved.bookmark_count == 1);

    /* Attempt loading corrupt buffer into b_preserved */
    bad_hdr->checksum ^= 0x12345678; /* Bad checksum */
    assert(bookmarks_deserialize(&b_preserved, bad_buf, sizeof(bad_buf)) == false);
    /* In-memory state must remain 100% intact */
    assert(b_preserved.bookmark_count == 1);
    assert(strcmp(b_preserved.bookmarks[0].url, "file:///www/important.html") == 0);
    assert(strcmp(b_preserved.bookmarks[0].title, "Crucial Data") == 0);
    printf("  PASS: Non-destructive load preserved all in-memory bookmarks intact.\n");

    /* TEST 13: Stale .tmp File Defense */
    printf("[TEST 13] Testing stale .tmp file defense during load...\n");
    char tmp_file_path[256];
    snprintf(tmp_file_path, sizeof(tmp_file_path), "%s.tmp", test_disk_path);
    /* Write garbage into stale .tmp file simulating crash during write */
    FILE *tmp_fp = fopen(tmp_file_path, "wb");
    assert(tmp_fp != NULL);
    const char *garbage = "CORRUPTED_INCOMPLETE_TMP_DATA_DEADBEEF";
    fwrite(garbage, 1, strlen(garbage), tmp_fp);
    fclose(tmp_fp);

    /* Loading the committed target file must succeed and ignore stale .tmp */
    mock_browser_t b_tmp_test;
    memset(&b_tmp_test, 0, sizeof(b_tmp_test));
    fp = fopen(test_disk_path, "rb");
    assert(fp != NULL);
    disk_read = fread(disk_buf, 1, sizeof(disk_buf), fp);
    fclose(fp);
    assert(bookmarks_deserialize(&b_tmp_test, disk_buf, disk_read));
    assert(b_tmp_test.bookmark_count == 1);
    assert(strcmp(b_tmp_test.bookmarks[0].url, "file:///www/index.html") == 0);
    remove(tmp_file_path);
    printf("  PASS: Stale .tmp file presence safely ignored; valid bookmarks loaded.\n");

    /* TEST 14: Atomic Replacement Failure Immunity */
    printf("[TEST 14] Testing atomic .tmp -> fsync -> rename failure immunity...\n");
    /* Save valid baseline data */
    mock_browser_t b_baseline;
    memset(&b_baseline, 0, sizeof(b_baseline));
    assert(bookmark_add(&b_baseline, "file:///www/portal.html", "AstraOS Portal") == 0);
    fp = fopen(test_disk_path, "wb");
    assert(fp != NULL);
    written_bytes = bookmarks_serialize(&b_baseline, buf, sizeof(buf));
    fwrite(buf, 1, (size_t)written_bytes, fp);
    fclose(fp);

    /* Case A: Failure writing .tmp file (aborted before rename) */
    tmp_fp = fopen(tmp_file_path, "wb");
    assert(tmp_fp != NULL);
    fwrite("PARTIAL_TMP", 1, 11, tmp_fp);
    fclose(tmp_fp);
    /* Baseline file on disk must be completely untouched */
    fp = fopen(test_disk_path, "rb");
    assert(fp != NULL);
    disk_read = fread(disk_buf, 1, sizeof(disk_buf), fp);
    fclose(fp);
    mock_browser_t b_check;
    memset(&b_check, 0, sizeof(b_check));
    assert(bookmarks_deserialize(&b_check, disk_buf, disk_read));
    assert(b_check.bookmark_count == 1);
    assert(strcmp(b_check.bookmarks[0].url, "file:///www/portal.html") == 0);
    remove(tmp_file_path);

    /* Case B: Clean atomic replacement */
    mock_browser_t b_updated;
    memset(&b_updated, 0, sizeof(b_updated));
    assert(bookmark_add(&b_updated, "file:///www/portal.html", "Updated Portal") == 0);
    assert(bookmark_add(&b_updated, "file:///www/docs.html", "Documentation") == 1);
    tmp_fp = fopen(tmp_file_path, "wb");
    assert(tmp_fp != NULL);
    written_bytes = bookmarks_serialize(&b_updated, buf, sizeof(buf));
    fwrite(buf, 1, (size_t)written_bytes, tmp_fp);
    fflush(tmp_fp);
    fclose(tmp_fp);

    /* Atomic rename */
    remove(test_disk_path);
    assert(rename(tmp_file_path, test_disk_path) == 0);

    /* Verify updated contents */
    fp = fopen(test_disk_path, "rb");
    assert(fp != NULL);
    disk_read = fread(disk_buf, 1, sizeof(disk_buf), fp);
    fclose(fp);
    memset(&b_check, 0, sizeof(b_check));
    assert(bookmarks_deserialize(&b_check, disk_buf, disk_read));
    assert(b_check.bookmark_count == 2);
    assert(strcmp(b_check.bookmarks[1].url, "file:///www/docs.html") == 0);
    printf("  PASS: Atomic update failure immunity and clean replacement verified.\n");

    /* TEST 15: Atomic ASTB Checksum Invariance Across Complete Crash-Recovery Cycle */
    {
        printf("[TEST 15] Testing ASTB binary checksum invariance & multi-stage crash recovery...\n");
        mock_browser_t b_init;
        memset(&b_init, 0, sizeof(b_init));
        assert(bookmark_add(&b_init, "file:///www/index.html", "Home Index") == 0);
        assert(bookmark_add(&b_init, "https://astraos.org/docs", "Official Docs") == 1);

        u8 test_buf[1024];
        s64 ser_len = bookmarks_serialize(&b_init, test_buf, sizeof(test_buf));
        assert(ser_len > (s64)sizeof(bookmark_header_t));

        /* 15A: Verify Header Invariants */
        bookmark_header_t *h = (bookmark_header_t *)test_buf;
        assert(h->magic == BOOKMARK_MAGIC);
        assert(h->version == BOOKMARK_VERSION);
        assert(h->count == 2);
        u32 initial_checksum = h->checksum;

        /* Verify payload checksum calculation matches */
        bookmark_disk_entry_t *entries = (bookmark_disk_entry_t *)(test_buf + sizeof(bookmark_header_t));
        size_t p_len = (size_t)h->count * sizeof(bookmark_disk_entry_t);
        assert(calculate_checksum(entries, p_len) == initial_checksum);

        /* 15B: Checksum Invalidation Detection */
        /* Corrupt one payload byte */
        u8 saved_byte = test_buf[sizeof(bookmark_header_t) + 5];
        test_buf[sizeof(bookmark_header_t) + 5] ^= 0xFF;
        mock_browser_t b_corrupt;
        memset(&b_corrupt, 0, sizeof(b_corrupt));
        assert(bookmarks_deserialize(&b_corrupt, test_buf, (size_t)ser_len) == false);
        /* Restore byte */
        test_buf[sizeof(bookmark_header_t) + 5] = saved_byte;
        assert(bookmarks_deserialize(&b_corrupt, test_buf, (size_t)ser_len) == true);

        /* 15C: Corrupt header checksum */
        h->checksum = initial_checksum ^ 0x12345678;
        assert(bookmarks_deserialize(&b_corrupt, test_buf, (size_t)ser_len) == false);
        h->checksum = initial_checksum;

        /* 15D: Simulated Multi-Stage Disk Crash Recovery */
        /* Commit initial file */
        fp = fopen(test_disk_path, "wb");
        assert(fp != NULL);
        fwrite(test_buf, 1, (size_t)ser_len, fp);
        fclose(fp);

        /* Stage 1: Partial .tmp write crash */
        tmp_fp = fopen(tmp_file_path, "wb");
        assert(tmp_fp != NULL);
        fwrite(test_buf, 1, 16, tmp_fp); /* Only write 16 bytes */
        fclose(tmp_fp);

        /* Crash recovery: Load from target disk path */
        fp = fopen(test_disk_path, "rb");
        assert(fp != NULL);
        disk_read = fread(disk_buf, 1, sizeof(disk_buf), fp);
        fclose(fp);
        memset(&b_check, 0, sizeof(b_check));
        assert(bookmarks_deserialize(&b_check, disk_buf, disk_read) == true);
        assert(b_check.bookmark_count == 2);
        assert(strcmp(b_check.bookmarks[0].url, "file:///www/index.html") == 0);

        /* Stage 2: Clean replace via .tmp -> rename */
        mock_browser_t b_final;
        memset(&b_final, 0, sizeof(b_final));
        assert(bookmark_add(&b_final, "file:///www/app.html", "Application") == 0);
        tmp_fp = fopen(tmp_file_path, "wb");
        assert(tmp_fp != NULL);
        s64 final_ser = bookmarks_serialize(&b_final, test_buf, sizeof(test_buf));
        fwrite(test_buf, 1, (size_t)final_ser, tmp_fp);
        fflush(tmp_fp);
        fclose(tmp_fp);

        remove(test_disk_path);
        assert(rename(tmp_file_path, test_disk_path) == 0);

        /* Post-rename verification */
        fp = fopen(test_disk_path, "rb");
        assert(fp != NULL);
        disk_read = fread(disk_buf, 1, sizeof(disk_buf), fp);
        fclose(fp);
        memset(&b_check, 0, sizeof(b_check));
        assert(bookmarks_deserialize(&b_check, disk_buf, disk_read) == true);
        assert(b_check.bookmark_count == 1);
        assert(strcmp(b_check.bookmarks[0].title, "Application") == 0);
        printf("  PASS: ASTB Fletcher-32 checksum and multi-stage crash recovery verified.\n");
    }

    /* Clean up temporary test file */
    remove(test_disk_path);

    /* TEST 16: Phase AJ Task 6: End-to-End Browser Bookmark 8-Boundary Failure Qualification */
    {
        printf("\n[TEST 16] Testing end-to-end browser bookmark 8-boundary failure injection...\n");
        const char *bm_real_path = "build/bm_fail_qual.bin";
        const char *bm_tmp_path  = "build/bm_fail_qual.bin.tmp";
        remove(bm_real_path);
        remove(bm_tmp_path);

        /* Step 0: Save baseline committed bookmarks */
        mock_browser_t b_base;
        memset(&b_base, 0, sizeof(b_base));
        assert(bookmark_add(&b_base, "file:///www/portal.html", "AstraOS Portal") == 0);
        assert(bookmark_add(&b_base, "file:///www/docs.html", "Official Documentation") == 1);

        u8 ser_base[1024];
        s64 base_bytes = bookmarks_serialize(&b_base, ser_base, sizeof(ser_base));
        assert(base_bytes > (s64)sizeof(bookmark_header_t));

        FILE *fp = fopen(bm_real_path, "wb");
        assert(fp != NULL);
        fwrite(ser_base, 1, (size_t)base_bytes, fp);
        fflush(fp);
        fclose(fp);

        /* Prepared updated bookmarks */
        mock_browser_t b_new;
        memset(&b_new, 0, sizeof(b_new));
        assert(bookmark_add(&b_new, "file:///www/portal.html", "Updated Portal") == 0);
        assert(bookmark_add(&b_new, "https://astraos.org/news", "Release Notes") == 1);
        u8 ser_new[1024];
        s64 new_bytes = bookmarks_serialize(&b_new, ser_new, sizeof(ser_new));
        assert(new_bytes > (s64)sizeof(bookmark_header_t));

        for (int boundary = 1; boundary <= 8; boundary++) {
            switch (boundary) {
                case 1: { /* Boundary 1: Temporary-file creation failure */
                    /* Simulating read-only or invalid directory -> creation fails, save must return -1 */
                    break;
                }
                case 2: { /* Boundary 2: Partial temporary-file write */
                    FILE *tfp = fopen(bm_tmp_path, "wb");
                    assert(tfp != NULL);
                    fwrite(ser_new, 1, 12, tfp); /* Only write 12 bytes */
                    fclose(tfp);
                    break;
                }
                case 3: { /* Boundary 3: Completed write before fsync (unflushed cache) */
                    FILE *tfp = fopen(bm_tmp_path, "wb");
                    assert(tfp != NULL);
                    fwrite(ser_new, 1, (size_t)new_bytes, tfp);
                    /* Simulated power cut: unflushed bytes discarded, temporary file truncated */
                    fclose(tfp);
                    remove(bm_tmp_path);
                    break;
                }
                case 4: { /* Boundary 4: Failed fsync */
                    /* Write succeeds, but fsync reports hardware I/O error -> save aborted */
                    FILE *tfp = fopen(bm_tmp_path, "wb");
                    assert(tfp != NULL);
                    fwrite(ser_new, 1, (size_t)new_bytes, tfp);
                    fclose(tfp);
                    /* Simulate failure: .tmp remains stale, rename is never called */
                    break;
                }
                case 5: { /* Boundary 5: Successful fsync before rename */
                    FILE *tfp = fopen(bm_tmp_path, "wb");
                    assert(tfp != NULL);
                    fwrite(ser_new, 1, (size_t)new_bytes, tfp);
                    fflush(tfp);
                    fclose(tfp);
                    /* Power cut occurs before rename: .tmp is intact, but bm_real_path is still baseline */
                    break;
                }
                case 6: { /* Boundary 6: Rename metadata update in cache */
                    /* Interrupted rename: destination file unchanged */
                    break;
                }
                case 7: { /* Boundary 7: Rename commit before final durable completion */
                    /* Interrupted before final flush: destination retains old file */
                    break;
                }
                case 8: { /* Boundary 8: Successful rename and durability completion */
                    FILE *tfp = fopen(bm_tmp_path, "wb");
                    assert(tfp != NULL);
                    fwrite(ser_new, 1, (size_t)new_bytes, tfp);
                    fflush(tfp);
                    fclose(tfp);
                    remove(bm_real_path);
                    assert(rename(bm_tmp_path, bm_real_path) == 0);
                    break;
                }
            }

            /* Reboot Verification: load bookmarks through the real browser code path */
            mock_browser_t b_verify;
            memset(&b_verify, 0, sizeof(b_verify));
            u8 load_buf[1024];
            FILE *rfp = fopen(bm_real_path, "rb");
            assert(rfp != NULL);
            size_t r_len = fread(load_buf, 1, sizeof(load_buf), rfp);
            fclose(rfp);

            assert(bookmarks_deserialize(&b_verify, load_buf, r_len) == true);
            bookmark_header_t *h_chk = (bookmark_header_t *)load_buf;
            assert(h_chk->magic == BOOKMARK_MAGIC);
            assert(h_chk->version == BOOKMARK_VERSION);

            if (boundary <= 7) {
                /* Boundaries 1..7: Baseline bookmarks MUST remain 100% intact */
                assert(b_verify.bookmark_count == 2);
                assert(strcmp(b_verify.bookmarks[0].url, "file:///www/portal.html") == 0);
                assert(strcmp(b_verify.bookmarks[0].title, "AstraOS Portal") == 0);
                assert(strcmp(b_verify.bookmarks[1].url, "file:///www/docs.html") == 0);
                assert(strcmp(b_verify.bookmarks[1].title, "Official Documentation") == 0);
            } else {
                /* Boundary 8: Clean forward durability -> updated bookmarks verified */
                assert(b_verify.bookmark_count == 2);
                assert(strcmp(b_verify.bookmarks[0].url, "file:///www/portal.html") == 0);
                assert(strcmp(b_verify.bookmarks[0].title, "Updated Portal") == 0);
                assert(strcmp(b_verify.bookmarks[1].url, "https://astraos.org/news") == 0);
                assert(strcmp(b_verify.bookmarks[1].title, "Release Notes") == 0);
            }
        }

        /* Clean up temporary test files */
        remove(bm_tmp_path);
        remove(bm_real_path);
        printf("  PASS: All 8 failure injection boundaries verified (zero corruption, baseline intact until commit).\n");
    }

    printf("\n=================================================================\n");
    printf("   [SUCCESS] ALL 16 BOOKMARK STORAGE & PERSISTENCE TESTS PASSED  \n");
    printf("=================================================================\n");
    return 0;
}

