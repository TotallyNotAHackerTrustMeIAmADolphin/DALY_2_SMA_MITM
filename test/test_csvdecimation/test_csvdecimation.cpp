// Native unit tests for the pure CSV decimation accumulator (#43):
// `pio test -e native`. Includes the real include/CsvDecimation.h - no
// mirrored copy to keep in sync. CsvDecimation doesn't know about
// TelemetrySchema; these tests build raw comma-separated lines directly
// and pick fields by bare index, exactly as the real adapter
// (SDLogger::readGraphSeries()) does after resolving indices itself.

#include <unity.h>
#include <string>
#include <cstring>
#include "CsvDecimation.h"

using CsvDecimation::Accumulator;

void setUp(void) {}
void tearDown(void) {}

// --- skip == 1: every line kept ---

static void test_skip1_all_lines_kept(void)
{
    // Each line's 3 fields chosen so picking indices {0,1,2} in order
    // reproduces the line unchanged - a simple, self-checking case.
    const std::string input = "T0,V0,I0\nT1,V1,I1\nT2,V2,I2\n";
    const size_t fields[3] = {0, 1, 2};
    Accumulator accum(fields, 3, /*skip=*/1);

    char out[256];
    size_t outLen = 0;
    accum.feed((const uint8_t *)input.data(), input.size(), out, sizeof(out), &outLen);
    accum.finish(out, sizeof(out), &outLen);

    const std::string expected = "T0,V0,I0\nT1,V1,I1\nT2,V2,I2\n";
    TEST_ASSERT_EQUAL_UINT32(expected.size(), outLen);
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), out, expected.size());
}

// --- skip == N > 1: only every Nth line kept ---

static void test_skipN_only_every_nth_line_kept(void)
{
    // 6 lines (index 0..5), skip=3: lineIdx 0 is always kept (keepLine_
    // starts true); index 1,2 dropped; index 3 kept (3 % 3 == 0); index
    // 4,5 dropped. Matches the pre-refactor `keepLine = (lineIdx % skip
    // == 0)` semantics exactly.
    std::string input;
    for (int i = 0; i < 6; i++)
        input += "L" + std::to_string(i) + "A,L" + std::to_string(i) + "B\n";

    const size_t fields[1] = {0}; // just the first field, to keep expected output short
    Accumulator accum(fields, 1, /*skip=*/3);

    char out[256];
    size_t outLen = 0;
    accum.feed((const uint8_t *)input.data(), input.size(), out, sizeof(out), &outLen);
    accum.finish(out, sizeof(out), &outLen);

    const std::string expected = "L0A\nL3A\n";
    TEST_ASSERT_EQUAL_UINT32(expected.size(), outLen);
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), out, expected.size());
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
    // first kLineBufSize-1 (319) bytes of the line are ever buffered
    // (matching the pre-refactor lineBuf's `lineLen < sizeof(lineBuf)-1`
    // bounds check), so field 1's extracted text is truncated to whatever
    // fit: 319 - strlen("AAAA,") = 314 'B's.
    std::string line = "AAAA," + std::string(400, 'B') + "\n";
    const size_t fields[2] = {0, 1};
    Accumulator accum(fields, 2, /*skip=*/1);

    char out[1024];
    size_t outLen = 0;
    accum.feed((const uint8_t *)line.data(), line.size(), out, sizeof(out), &outLen);
    accum.finish(out, sizeof(out), &outLen);

    const std::string expected = "AAAA," + std::string(314, 'B') + "\n";
    TEST_ASSERT_EQUAL_UINT32(expected.size(), outLen);
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), out, expected.size());
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

// finish() on an accumulator that never saw any input must not write anything.

static void test_finish_with_no_input_writes_nothing(void)
{
    const size_t fields[1] = {0};
    Accumulator accum(fields, 1, /*skip=*/1);

    char out[64];
    size_t outLen = 0;
    accum.finish(out, sizeof(out), &outLen);
    TEST_ASSERT_EQUAL_UINT32(0, outLen);
}

// --- CsvDecimation::estimateSkip (#96): the skip-interval math extracted
// out of readGraphSeries(), pure and directly testable now ---

using CsvDecimation::estimateSkip;

// Zero sample lines (huge single line, or a file barely bigger than its
// header): estimatedLines stays at its default of 1, so skip is always 1
// regardless of dataBytes/targetPoints.
static void test_estimateskip_zero_sample_lines_is_skip_one(void)
{
    TEST_ASSERT_EQUAL_UINT32(1, estimateSkip(/*dataBytes=*/500000, /*sampleBytes=*/4096, /*sampleLines=*/0, /*targetPoints=*/300));
    TEST_ASSERT_EQUAL_UINT32(1, estimateSkip(0, 0, 0, 1));
}

// A tiny file whose estimated line count rounds down to 0 is clamped back
// up to 1 rather than dividing by zero downstream.
static void test_estimateskip_tiny_file_clamped_to_one_line(void)
{
    // avgLineLen = 4096/100 ~= 41; dataBytes=10 -> estimatedLines rounds to 0.
    TEST_ASSERT_EQUAL_UINT32(1, estimateSkip(/*dataBytes=*/10, /*sampleBytes=*/4096, /*sampleLines=*/100, /*targetPoints=*/300));
}

// targetPoints far bigger than the estimated row count: keep every row (skip=1).
static void test_estimateskip_oversized_target_keeps_every_row(void)
{
    // avgLineLen = 4096/100 = 40.96; dataBytes=4096 -> estimatedLines ~= 100.
    TEST_ASSERT_EQUAL_UINT32(1, estimateSkip(/*dataBytes=*/4096, /*sampleBytes=*/4096, /*sampleLines=*/100, /*targetPoints=*/100000));
}

// Normal case: matches the pre-refactor inline math exactly (same floats,
// same integer truncation order: estimatedLines first, then /targetPoints).
static void test_estimateskip_normal_case_matches_old_inline_math(void)
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
    RUN_TEST(test_finish_with_no_input_writes_nothing);
    RUN_TEST(test_estimateskip_zero_sample_lines_is_skip_one);
    RUN_TEST(test_estimateskip_tiny_file_clamped_to_one_line);
    RUN_TEST(test_estimateskip_oversized_target_keeps_every_row);
    RUN_TEST(test_estimateskip_normal_case_matches_old_inline_math);
    RUN_TEST(test_estimateskip_zero_target_points_treated_as_one);
    return UNITY_END();
}
