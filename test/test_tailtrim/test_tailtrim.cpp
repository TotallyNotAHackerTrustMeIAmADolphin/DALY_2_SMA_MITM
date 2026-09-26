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

static void test_content_shorter_than_maxbytes_untouched(void)
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

static void test_content_exceeding_maxbytes_trims_to_last_n_bytes(void)
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

static void test_finish_drops_leading_partial_line_only_when_truncated(void)
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

// finish() drops a single leading bare newline (the retained tail's first
// byte, with more content after it) the same way it drops a leading
// partial line up to and including its newline.
static void test_finish_drops_leading_bare_newline(void)
{
    // Retained 11-byte tail (last 11 bytes of content) is "\nYYYYYYYYYY":
    // the leading '\n' is not the last byte, so finish() strips it.
    const std::string content = "XXXXXXXXXX\nYYYYYYYYYY"; // 21 bytes, no trailing newline
    Trimmer trimmer(11);
    feedInChunks(trimmer, content, 5);

    TEST_ASSERT_TRUE(trimmer.truncated());
    trimmer.finish();

    const std::string expected = "YYYYYYYYYY";
    TEST_ASSERT_EQUAL_UINT32(expected.size(), trimmer.length());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), trimmer.data(), expected.size());
}

// The actual last-byte guard (#112): when the retained tail's only '\n' IS
// its very last byte, finish() must leave it alone rather than erasing the
// whole buffer (erasing up to and including index 0 there would empty a
// non-empty, truncated tail).
static void test_finish_keeps_tail_when_newline_is_last_byte(void)
{
    // Retained 5-byte tail (last 5 bytes of content) is "ZZZZ\n".
    const std::string content = "XXXXXXXXXXZZZZ\n"; // 15 bytes, longer than maxBytes below
    Trimmer trimmer(5);
    feedInChunks(trimmer, content, 4);

    TEST_ASSERT_TRUE(trimmer.truncated());
    trimmer.finish();

    const std::string expected = "ZZZZ\n";
    TEST_ASSERT_EQUAL_UINT32(expected.size(), trimmer.length());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), trimmer.data(), expected.size());
}

// A chunkBytes matching the caller's real read-buffer size must reserve
// enough headroom that feed() never reallocates mid-scan (#100) - checked
// here by watching data()'s pointer stay put across every feed() call,
// for both the default chunk size and an explicitly different one.
static void test_matching_chunk_size_never_reallocates(void)
{
    std::string content;
    for (int i = 0; i < 200; i++)
        content += "0123456789\n"; // 2200 bytes, well past either maxBytes below

    // Default chunkBytes (no second constructor argument).
    {
        Trimmer trimmer(50);
        const char *firstPtr = nullptr;
        for (size_t off = 0; off < content.size(); off += Trimmer::kDefaultChunkBytes)
        {
            size_t n = std::min(Trimmer::kDefaultChunkBytes, content.size() - off);
            trimmer.feed((const uint8_t *)content.data() + off, n);
            if (!firstPtr)
                firstPtr = trimmer.data();
            TEST_ASSERT_EQUAL_PTR(firstPtr, trimmer.data());
        }
    }

    // A chunk size other than the default (#100: chunkBytes is now an
    // explicit constructor argument, not a hardcoded assumption).
    {
        const size_t chunkBytes = 128;
        Trimmer trimmer(50, chunkBytes);
        const char *firstPtr = nullptr;
        for (size_t off = 0; off < content.size(); off += chunkBytes)
        {
            size_t n = std::min(chunkBytes, content.size() - off);
            trimmer.feed((const uint8_t *)content.data() + off, n);
            if (!firstPtr)
                firstPtr = trimmer.data();
            TEST_ASSERT_EQUAL_PTR(firstPtr, trimmer.data());
        }
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_content_shorter_than_maxbytes_untouched);
    RUN_TEST(test_content_exceeding_maxbytes_trims_to_last_n_bytes);
    RUN_TEST(test_finish_drops_leading_partial_line_only_when_truncated);
    RUN_TEST(test_finish_drops_leading_bare_newline);
    RUN_TEST(test_finish_keeps_tail_when_newline_is_last_byte);
    RUN_TEST(test_matching_chunk_size_never_reallocates);
    return UNITY_END();
}
