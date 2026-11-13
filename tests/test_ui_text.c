/* Every notification exists in every language, with the same arguments (src/ui_text.c) */
#include "check.h"
#include "ui_text.h"

#include <string.h>

static void conversions(const char *s, char *out, int max)
{
    int n = 0;
    for (; s && *s && n < max - 1; s++)
        if (s[0] == '%' && s[1]) out[n++] = *++s;
    out[n] = 0;
}

static void test_ui_text_everyLanguage_sameArguments(void)
{
    for (int m = 0; m < UI_COUNT; m++) {
        char want[16];
        conversions(ui_text_in(UI_EN, (ui_msg_t)m), want, sizeof(want));
        for (int l = 0; l < UI_LANG_COUNT; l++) {
            const char *t = ui_text_in((ui_lang_t)l, (ui_msg_t)m);
            char got[16];
            conversions(t, got, sizeof(got));
            CHECK(t && t[0], "language %d has message %d", l, m);
            CHECK(strcmp(got, want) == 0, "language %d message %d takes \"%s\" like English \"%s\"", l, m, got, want);
        }
    }
}

int main(void)
{
    test_ui_text_everyLanguage_sameArguments();
    return REPORT("test_ui_text");
}
