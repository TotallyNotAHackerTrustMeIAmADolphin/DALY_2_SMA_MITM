// Native unit tests for the pure CSV decimation accumulator (#43):
// `pio test -e native`. Includes the real include/CsvDecimation.h - no
// mirrored copy to keep in sync. CsvDecimation doesn't know about
// TelemetrySchema; these tests build raw comma-separated lines directly
// and pick fields by bare index, exactly as the real adapter
// (SDLogger::readGraphSeries()) does after resolving indices itself.

#include <unity.h>
#include <string>
#include <cstring>
#include <vector>
#include "CsvDecimation.h"

using CsvDecimation::Accumulator;

void setUp(void) {}
void tearDown(void) {}

// Feeds `input` through a fresh Accumulator in one shot and returns whatever
// finish() left in the output buffer, so each test below is just a
// fields/skip/input/expected comparison.
static std::string run(const size_t *fields, size_t nFields, size_t skip, const std::string &input, size_t outCap = 256)
{
    Accumulator accum(fields, nFields, skip);
    std::vector<char> out(outCap);
    size_t outLen = 0;
    accum.feed((const uint8_t *)input.data(), input.size(), out.data(), out.size(), &outLen);
    accum.finish(out.data(), out.size(), &outLen);
    return std::string(out.data(), outLen);
}

// --- skip == 1: every line kept ---

static void test_skip1_all_lines_kept(void)
{
    // Each line's 3 fields chosen so picking indices {0,1,2} in order
    // reproduces the line unchanged - a simple, self-checking case.
    const size_t fields[3] = {0, 1, 2};
    TEST_ASSERT_EQUAL_STRING("T0,V0,I0\nT1,V1,I1\nT2,V2,I2\n",
                              run(fields, 3, 1, "T0,V0,I0\nT1,V1,I1\nT2,V2,I2\n").c_str());
}

// --- skip == N > 1: only every Nth line kept ---

static void test_skipN_only_every_nth_line_kept(void)
{
    // 6 lines (index 0..5), skip=3: lineIdx 0 is always kept (keepLine_
    // starts true); index 1,2 dropped; index 3 kept (3 % 3 == 0); index 4,5 dropped.
    std::string input;
    for (int i = 0; i < 6; i++)
        input += "L" + std::to_string(i) + "A,L" + std::to_string(i) + "B\n";

    const size_t fields[1] = {0}; // just the first field, to keep expected output short
    TEST_ASSERT_EQUAL_STRING("L0A\nL3A\n", run(fields, 1, 3, input).c_str());
}

// --- a line split across two feed() calls (chunk boundary mid-line) ---

static void test_chunk_boundary_mid_line(void)
{
    // "ABCDE,FGHIJ,KLMNO\n" split mid-second-field: chunk 1 ends inside
    // "FGHIJ", chunk 2 carries the rest plus the newline. The accumulated
    // line must still be parsed as a whole once the '\n' arrives in the
    // second chunk.
    const std::string chunk1 = "ABCDE,FG";
    const std::string chunk2 = "HIJ,KLMNO\n";
    const size_t fields[2] = {0, 2};
    Accumulator accum(fields, 2, /*skip=*/1);

    char out[256];
    size_t outLen = 0;
    accum.feed((const uint8_t *)chunk1.data(), chunk1.size(), out, sizeof(out), &outLen);
    // No newline seen yet - nothing should have been written.
    TEST_ASSERT_EQUAL_UINT32(0, outLen);

    accum.feed((const uint8_t *)chunk2.data(), chunk2.size(), out, sizeof(out), &outLen);
    accum.finish(out, sizeof(out), &outLen);

    const std::string expected = "ABCDE,KLMNO\n";
    TEST_ASSERT_EQUAL_UINT32(expected.size(), outLen);
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), out, expected.size());
}

// --- a line exceeding kLineBufSize (320) is safely truncated ---

static void test_oversized_line_safely_truncated(void)
{
    // Field 0 is short ("AAAA"), field 1 is 400 'B's - well past
    // kLineBufSize (320) once combined with the leading "AAAA,". Only the
    // first kLineBufSize-1 (319) bytes of the line are ever buffered, so
    // field 1's extracted text is truncated to whatever fit: 319 -
    // strlen("AAAA,") = 314 'B's.
    std::string line = "AAAA," + std::string(400, 'B') + "\n";
    const size_t fields[2] = {0, 1};
    const std::string expected = "AAAA," + std::string(314, 'B') + "\n";
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), run(fields, 2, 1, line, 1024).c_str());
}

// --- a final line with no trailing newline is only flushed by finish() ---

static void test_final_line_without_newline_flushed_by_finish(void)
{
    const std::string input = "X,Y,Z\nP,Q,R"; // second line has no trailing '\n'
    const size_t fields[3] = {0, 1, 2};
    Accumulator accum(fields, 3, /*skip=*/1);

    char out[256];
    size_t outLen = 0;
    accum.feed((const uint8_t *)input.data(), input.size(), out, sizeof(out), &outLen);
    // Only the first (newline-terminated) line should have been flushed so far.
    TEST_ASSERT_EQUAL_UINT32(6, outLen); // "X,Y,Z\n"
    TEST_ASSERT_EQUAL_MEMORY("X,Y,Z\n", out, 6);

    accum.finish(out, sizeof(out), &outLen);
    const std::string expected = "X,Y,Z\nP,Q,R\n"; // finish() always appends its own '\n'
    TEST_ASSERT_EQUAL_UINT32(expected.size(), outLen);
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), out, expected.size());
}

// --- a tiny outCap exhausted mid-line (#115): flushCurrentLine() now only
// emits a line if it fits outCap in full, so the caller's buffer is never
// overflowed AND the output always ends on a complete line - a line that
// doesn't fit is dropped whole (along with every line after it, since the
// remaining capacity can't grow), never truncated mid-field. ---

static void test_tiny_outcap_never_overflows_and_ends_on_complete_line(void)
{
    const std::string input = "1,2,3\n4,5,6\n7,8,9\n";
    const size_t fields[3] = {0, 1, 2};
    Accumulator accum(fields, 3, /*skip=*/1);

    // Room for exactly the first decimated line ("1,2,3\n" = 6 bytes) plus
    // a few spare bytes - not enough for a second full line (also 6 bytes).
    char out[9];
    size_t outLen = 0;
    accum.feed((const uint8_t *)input.data(), input.size(), out, sizeof(out), &outLen);
    accum.finish(out, sizeof(out), &outLen);

    // Never overflows the caller's buffer.
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(out), outLen);
    // Output ends on a complete line: the first line only, byte-identical,
    // with its trailing '\n' - never a partial field.
    const std::string expected = "1,2,3\n";
    TEST_ASSERT_EQUAL_UINT32(expected.size(), outLen);
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), out, expected.size());

    // A buffer too small for even the first line emits nothing at all,
    // rather than a truncated fragment.
    char tiny[3];
    size_t tinyLen = 0;
    Accumulator accum2(fields, 3, /*skip=*/1);
    accum2.feed((const uint8_t *)input.data(), input.size(), tiny, sizeof(tiny), &tinyLen);
    accum2.finish(tiny, sizeof(tiny), &tinyLen);
    TEST_ASSERT_EQUAL_UINT32(0, tinyLen);
}

// finish() on an accumulator that never saw any input must not write anything.

static void test_finish_with_no_input_writes_nothing(void)
{
    const size_t fields[1] = {0};
    TEST_ASSERT_EQUAL_STRING("", run(fields, 1, 1, "").c_str());
}

// --- CsvDecimation::estimateSkip (#96): the skip-interval math extracted
// out of readGraphSeries(), pure and directly testable now ---

using CsvDecimation::estimateSkip;

// Cases whose skip is always 1: zero sample lines (estimatedLines stays at
// its default), a tiny file whose estimated line count rounds down to 0
// (clamped back up rather than dividing by zero downstream), and a
// targetPoints far bigger than the estimated row count (keep every row).
struct EstimateSkipCase
{
    uint32_t dataBytes, sampleBytes;
    size_t sampleLines, targetPoints, expectedSkip;
};

static void test_estimateskip_skip_one_cases(void)
{
    static const EstimateSkipCase cases[] = {
        {500000, 4096, 0, 300, 1},
        {0, 0, 0, 1, 1},
        {10, 4096, 100, 300, 1},
        {4096, 4096, 100, 100000, 1},
    };
    for (const auto &c : cases)
        TEST_ASSERT_EQUAL_UINT32(c.expectedSkip, estimateSkip(c.dataBytes, c.sampleBytes, c.sampleLines, c.targetPoints));
}

// Normal case: same integer truncation order as the formula (estimatedLines
// first, then /targetPoints).
static void test_estimateskip_normal_case(void)
{
    const uint32_t dataBytes = 24000;
    const uint32_t sampleBytes = 4096;
    const size_t sampleLines = 100;
    const size_t targetPoints = 100;

    float avgLineLen = (float)sampleBytes / (float)sampleLines; // 40.96
    size_t estimatedLines = (size_t)((float)dataBytes / avgLineLen); // truncates
    size_t expectedSkip = (estimatedLines > targetPoints) ? (estimatedLines / targetPoints) : 1;

    TEST_ASSERT_EQUAL_UINT32(expectedSkip, estimateSkip(dataBytes, sampleBytes, sampleLines, targetPoints));
    // Pin the actual number too, so a future accidental formula change is caught.
    TEST_ASSERT_EQUAL_UINT32(5, estimateSkip(dataBytes, sampleBytes, sampleLines, targetPoints));
}

// targetPoints == 0 is treated as 1, same as the caller-side clamp this
// used to rely on (callers still clamp before calling, but estimateSkip
// doesn't divide by zero even if one doesn't).
static void test_estimateskip_zero_target_points_treated_as_one(void)
{
    TEST_ASSERT_EQUAL_UINT32(estimateSkip(24000, 4096, 100, 1), estimateSkip(24000, 4096, 100, 0));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_skip1_all_lines_kept);
    RUN_TEST(test_skipN_only_every_nth_line_kept);
    RUN_TEST(test_chunk_boundary_mid_line);
    RUN_TEST(test_oversized_line_safely_truncated);
    RUN_TEST(test_final_line_without_newline_flushed_by_finish);
    RUN_TEST(test_tiny_outcap_never_overflows_and_ends_on_complete_line);
    RUN_TEST(test_finish_with_no_input_writes_nothing);
    RUN_TEST(test_estimateskip_skip_one_cases);
    RUN_TEST(test_estimateskip_normal_case);
    RUN_TEST(test_estimateskip_zero_target_points_treated_as_one);
    return UNITY_END();
}
