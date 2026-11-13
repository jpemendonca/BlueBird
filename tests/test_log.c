/* The log never grows without limit (src/log.c) */
#include "check.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH "build/host/test_log.txt"

static long size_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void test_log_line_pastLimit_rotatesToOld(void)
{
    /* arrange */
    unlink(PATH);
    unlink(PATH ".old");
    log_init(PATH);
    char filler[200];
    memset(filler, 'x', sizeof(filler) - 1);
    filler[sizeof(filler) - 1] = 0;

    /* act: about 2.5 MB of lines */
    for (int i = 0; i < 12000; i++) log_line("test: %d %s", i, filler);
    log_close();

    /* assert */
    CHECK(size_of(PATH) >= 0 && size_of(PATH) <= LOG_MAX_BYTES, "the log stays under the limit, %ld bytes", size_of(PATH));
    CHECK(size_of(PATH ".old") > LOG_MAX_BYTES / 2, "the older part is kept once in .old, %ld bytes", size_of(PATH ".old"));
}

int main(void)
{
    test_log_line_pastLimit_rotatesToOld();
    return REPORT("test_log");
}
