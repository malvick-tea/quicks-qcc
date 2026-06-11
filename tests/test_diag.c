/*
 * Tests for the diag module: recording, tallies, severity names, and the
 * rendered "file:line:col" output against a real source.
 */
#include "qtest.h"

#include "diag/diag.h"
#include "source/source.h"
#include "status/status.h"

/* Recording diagnostics must keep count and per-severity tallies in sync. */
static void test_record_and_tally(void)
{
    qcc_diag_sink sink;
    qcc_diag_sink_init(&sink);

    QTEST_CHECK_EQ_UINT(qcc_diag_count(&sink), 0, "fresh sink is empty");

    qcc_status st = qcc_diag_emit(&sink, QCC_DIAG_ERROR, NULL, 0, 0,
                                  "plain %s", "error");
    QTEST_CHECK_EQ_INT(st, QCC_OK, "emit error");
    st = qcc_diag_emit(&sink, QCC_DIAG_WARNING, NULL, 0, 0, "a warning");
    QTEST_CHECK_EQ_INT(st, QCC_OK, "emit warning");
    st = qcc_diag_emit(&sink, QCC_DIAG_ERROR, NULL, 0, 0, "another error");
    QTEST_CHECK_EQ_INT(st, QCC_OK, "emit second error");

    QTEST_CHECK_EQ_UINT(qcc_diag_count(&sink), 3, "three recorded");
    QTEST_CHECK_EQ_UINT(qcc_diag_severity_count(&sink, QCC_DIAG_ERROR), 2,
                        "two errors");
    QTEST_CHECK_EQ_UINT(qcc_diag_severity_count(&sink, QCC_DIAG_WARNING), 1,
                        "one warning");
    QTEST_CHECK_EQ_UINT(qcc_diag_severity_count(&sink, QCC_DIAG_NOTE), 0,
                        "no notes");

    /* The formatted message must have been expanded and stored. */
    QTEST_CHECK_SPAN(sink.items[0].message, strlen(sink.items[0].message),
                     "plain error", "formatted message stored");

    qcc_diag_sink_dispose(&sink);
    QTEST_CHECK_EQ_UINT(qcc_diag_count(&sink), 0, "dispose empties the sink");
}

/* Severity names are a stable contract (tests and tools grep for them). */
static void test_severity_names(void)
{
    QTEST_CHECK_SPAN(qcc_diag_severity_str(QCC_DIAG_ERROR),
                     strlen(qcc_diag_severity_str(QCC_DIAG_ERROR)),
                     "error", "error name");
    QTEST_CHECK_SPAN(qcc_diag_severity_str(QCC_DIAG_WARNING),
                     strlen(qcc_diag_severity_str(QCC_DIAG_WARNING)),
                     "warning", "warning name");
    QTEST_CHECK_SPAN(qcc_diag_severity_str(QCC_DIAG_NOTE),
                     strlen(qcc_diag_severity_str(QCC_DIAG_NOTE)),
                     "note", "note name");
}

/*
 * Render a diagnostic anchored in a real source through a tmpfile and check the
 * "name:line:col: severity: message" prefix plus the excerpt-and-caret shape.
 * tmpfile() is ISO C (C11 §7.21.4.4), not a POSIX extension, so it stays within
 * the portable seed subset (ADR-0009).
 */
static void test_print_format(void)
{
    /* Line 2 holds "bad token"; the span covers "bad" (offset 6, length 3). */
    static const char text[] = "line1\nbad token\n";

    qcc_source src;
    qcc_status st = qcc_source_from_memory("t.c", text, sizeof(text) - 1, &src);
    QTEST_CHECK_EQ_INT(st, QCC_OK, "source init");

    qcc_diag_sink sink;
    qcc_diag_sink_init(&sink);
    st = qcc_diag_emit(&sink, QCC_DIAG_ERROR, &src, 6, 3, "unexpected '%s'",
                       "bad");
    QTEST_CHECK_EQ_INT(st, QCC_OK, "emit");

    FILE *capture = tmpfile();
    QTEST_CHECK_TRUE(capture != NULL);
    if (capture != NULL) {
        qcc_diag_sink_print(&sink, capture);

        char   buffer[256] = {0};
        rewind(capture);
        size_t got = fread(buffer, 1, sizeof(buffer) - 1, capture);
        QTEST_CHECK_TRUE(got > 0);

        static const char expected[] =
            "t.c:2:1: error: unexpected 'bad'\n"
            "    bad token\n"
            "    ^~~\n";
        QTEST_CHECK_SPAN(buffer, got, expected, "rendered diagnostic");

        fclose(capture);
    }

    qcc_diag_sink_dispose(&sink);
    qcc_source_dispose(&src);
}

int main(void)
{
    test_record_and_tally();
    test_severity_names();
    test_print_format();
    return qtest_report("diag");
}
