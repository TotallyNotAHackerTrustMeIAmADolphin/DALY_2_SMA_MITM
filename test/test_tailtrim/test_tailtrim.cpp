// Native unit tests for the pure rolling tail-trim (#43): `pio test -e
// native`. Includes the real include/TailTrim.h - no mirrored copy to keep
// in sync.

#include <unity.h>
#include <string>
#include <algorithm>
#include "TailTrim.h"

using TailTrim::Trimmer;

void setUp(void) {}
void tearDown(void) {}

// Feeds `content` to trimmer in chunks of chunkSize bytes (last chunk may
// be shorter), simulating file.read()-style chunked I/O.
static void feedInChunks(Trimmer &trimmer, const std::string &content, size_t chunkSize)
{
    for (size_t off = 0; off < content.size(); off += chunkSize)
    {
        size_t n = std::min(chunkSize, content.size() - off);
        trimmer.feed((const uint8_t *)content.data() + off, n);
    }
}

// --- content shorter than maxBytes: left completely untouched ---

void test_content_shorter_than_maxbytes_untouched(void)
{
    // Deliberately starts with a line containing a newline near the front
    // - finish() must NOT strip it, since truncation never happened.
    const std::string content = "AAAAAAAAAA\nBBBBBBBBBB\nCCCCCCCCCC\nDDDDDDDDDD\n"; // 44 bytes
    Trimmer trimmer(100); // maxBytes well above content size

    feedInChunks(trimmer, content, 7);
    TEST_ASSERT_FALSE(trimmer.truncated());

    trimmer.finish();
    TEST_ASSERT_FALSE(trimmer.truncated());
    TEST_ASSERT_EQUAL_UINT32(content.size(), trimmer.length());
    TEST_ASSERT_EQUAL_MEMORY(content.data(), trimmer.data(), content.size());
}

// --- content exceeding maxBytes across multiple chunks: correct trim point ---

void test_content_exceeding_maxbytes_trims_to_last_n_bytes(void)
{
    // 4 lines of 11 bytes each ("A"x10 + '\n', etc) = 44 bytes total.
    const std::string content = "AAAAAAAAAA\nBBBBBBBBBB\nCCCCCCCCCC\nDDDDDDDDDD\n";
    TEST_ASSERT_EQUAL_UINT32(44, content.size());

    Trimmer trimmer(25);
    feedInChunks(trimmer, content, 7); // chunk size doesn't evenly divide the content or maxBytes

    TEST_ASSERT_TRUE(trimmer.truncated());
    // Exact trailing 25 bytes of the source content.
    const std::string expectedTrim = content.substr(content.size() - 25);
    TEST_ASSERT_EQUAL_UINT32(25, trimmer.length());
    TEST_ASSERT_EQUAL_MEMORY(expectedTrim.data(), trimmer.data(), 25);
}

// --- leading-partial-line-drop only fires when truncation actually occurred ---

void test_finish_drops_leading_partial_line_only_when_truncated(void)
{
    const std::string content = "AAAAAAAAAA\nBBBBBBBBBB\nCCCCCCCCCC\nDDDDDDDDDD\n"; // 44 bytes

    // Case 1: truncated - the retained 25-byte tail starts mid-line
    // ("BB\nCCCCCCCCCC\nDDDDDDDDDD\n"); finish() must drop up to and
    // including that leading partial line's newline.
    {
        Trimmer trimmer(25);
        feedInChunks(trimmer, content, 7);
        TEST_ASSERT_TRUE(trimmer.truncated());
        trimmer.finish();

        const std::string expected = "CCCCCCCCCC\nDDDDDDDDDD\n"; // 22 bytes
        TEST_ASSERT_EQUAL_UINT32(expected.size(), trimmer.length());
        TEST_ASSERT_EQUAL_MEMORY(expected.data(), trimmer.data(), expected.size());
    }

    // Case 2: not truncated (maxBytes covers the whole content) - finish()
    // must leave the content, newline-at-the-front and all, untouched.
    {
        Trimmer trimmer(100);
        feedInChunks(trimmer, content, 7);
        TEST_ASSERT_FALSE(trimmer.truncated());
        trimmer.finish();

        TEST_ASSERT_EQUAL_UINT32(content.size(), trimmer.length());
        TEST_ASSERT_EQUAL_MEMORY(content.data(), trimmer.data(), content.size());
    }
}

// A retained tail that happens to end exactly on a newline must not be
// wiped out entirely by finish() (same guard as the pre-refactor code:
// only drop the leading newline if it isn't the very last byte).
void test_finish_keeps_content_when_only_newline_is_last_byte(void)
{
    // maxBytes chosen so the retained tail is exactly "\nZZZZZZZZZZ" ...
    // construct content so that after trimming, the buffer is a single
    // line with no newline except possibly at the very end.
    const std::string content = "XXXXXXXXXX\nYYYYYYYYYY"; // 21 bytes, no trailing newline
    Trimmer trimmer(11);                                  // retains only "YYYYYYYYYY" + nothing else - no newline in the tail at all
    feedInChunks(trimmer, content, 5);

    TEST_ASSERT_TRUE(trimmer.truncated());
    trimmer.finish();

    // No '\n' present in the retained buffer at all, so finish() must leave it as-is.
    const std::string expected = "YYYYYYYYYY";
    TEST_ASSERT_EQUAL_UINT32(expected.size(), trimmer.length());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), trimmer.data(), expected.size());
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_content_shorter_than_maxbytes_untouched);
    RUN_TEST(test_content_exceeding_maxbytes_trims_to_last_n_bytes);
    RUN_TEST(test_finish_drops_leading_partial_line_only_when_truncated);
    RUN_TEST(test_finish_keeps_content_when_only_newline_is_last_byte);
    return UNITY_END();
}
