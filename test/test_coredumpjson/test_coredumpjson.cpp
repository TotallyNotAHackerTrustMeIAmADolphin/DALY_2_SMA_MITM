// Native tests for include/CoreDumpInfo.h (#91): the
// /api/coredump/summary JSON and BoundedWriter.

#include <unity.h>
#include <string.h>
#include "CoreDumpInfo.h"

void setUp(void) {}
void tearDown(void) {}

static void test_no_dump_json(void)
{
    CoreDumpInfo d;
    d.checkName = "ESP_ERR_NOT_FOUND";
    char json[900];
    TEST_ASSERT_TRUE(formatCoreDumpJson(d, 1, "power-on", "0123456789abcdef", json, sizeof(json)));
    TEST_ASSERT_EQUAL_STRING("{\"check\":\"ESP_ERR_NOT_FOUND\",\"present\":false,"
                             "\"reset_reason\":1,\"reset_reason_name\":\"power-on\","
                             "\"running_elf_sha256\":\"0123456789abcdef\"}",
                             json);
}

static void test_full_backtrace_fits_and_is_exact(void)
{
    CoreDumpInfo d;
    d.status = CoreDumpInfo::Present;
    d.checkName = "ESP_OK";
    strcpy(d.task, "CAN_Task");
    d.pc = 0x400d1234;
    d.cause = 28;
    d.corrupted = true;
    d.backtraceDepth = CoreDumpInfo::kMaxBacktrace;
    for (int i = 0; i < d.backtraceDepth; i++)
        d.backtrace[i] = 0xFFFFFFF0u + i;
    strcpy(d.elfSha, "fedcba9876543210");
    char json[900];
    TEST_ASSERT_TRUE(formatCoreDumpJson(d, 4, "PANIC (crash)", "0123456789abcdef", json, sizeof(json)));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"task\":\"CAN_Task\",\"pc\":\"0x400d1234\",\"cause\":28,\"corrupted\":true,"
                                      "\"backtrace\":[\"0xfffffff0\",\"0xfffffff1\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"0xffffffff\"],\"crash_elf_sha256\":\"fedcba9876543210\""));
    TEST_ASSERT_EQUAL_CHAR('}', json[strlen(json) - 1]);
}

static void test_too_small_buffer_fails(void)
{
    CoreDumpInfo d;
    d.checkName = "ESP_ERR_NOT_FOUND";
    char json[40];
    TEST_ASSERT_FALSE(formatCoreDumpJson(d, 1, "power-on", "0123456789abcdef", json, sizeof(json)));
}

static void test_bounded_writer_is_sticky(void)
{
    char buf[8];
    BoundedWriter w(buf, sizeof(buf));
    TEST_ASSERT_TRUE(w.append("abc"));
    TEST_ASSERT_FALSE(w.append("defghij")); // would need 11 bytes
    TEST_ASSERT_FALSE(w.append("x"));       // sticky, even though it'd fit
    TEST_ASSERT_FALSE(w.ok());
    TEST_ASSERT_EQUAL_STRING("abc", buf); // the failed append left no partial text
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_dump_json);
    RUN_TEST(test_full_backtrace_fits_and_is_exact);
    RUN_TEST(test_too_small_buffer_fails);
    RUN_TEST(test_bounded_writer_is_sticky);
    return UNITY_END();
}
