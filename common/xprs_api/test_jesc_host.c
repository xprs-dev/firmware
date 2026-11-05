/* Host test for xapi_jesc. */
#include <stdio.h>
#include <string.h>
int xapi_jesc(char *out, size_t cap, const char *in, int inlen);

static int fails, checks;
static void eq(const char *in, int inlen, const char *want)
{
    char out[256];
    xapi_jesc(out, sizeof out, in, inlen < 0 ? (int)strlen(in) : inlen);
    checks++;
    if (strcmp(out, want)) { fails++; printf("FAIL: got [%s] want [%s]\n", out, want); }
}

int main(void)
{
    eq("plain", -1, "plain");
    eq("a\"b\\c\nd", -1, "a\\\"b\\\\c\\nd");
    eq("tab\there", -1, "tab\\u0009here");
    eq("del\x7f", -1, "del\\u007f");
    /* Text people write passes through: the 2026-10-24 "Eh la" case. */
    eq("Eh l\xc3\xa1", -1, "Eh l\xc3\xa1");
    eq("\xe2\x80\x94 \xe2\x82\xac", -1, "\xe2\x80\x94 \xe2\x82\xac");  /* em dash, euro */
    eq("\xf0\x9f\x98\x80", -1, "\xf0\x9f\x98\x80");                  /* emoji */
    /* Not UTF-8: escaped byte by byte, never raw. */
    eq("x\xb7y", -1, "x\\u00b7y");                 /* the stray card byte */
    eq("\xc3", -1, "\\u00c3");                     /* truncated */
    eq("\xc0\xaf", -1, "\\u00c0\\u00af");          /* overlong '/' */
    eq("\xed\xa0\x80", -1, "\\u00ed\\u00a0\\u0080"); /* surrogate */
    eq("\xf4\x90\x80\x80", -1, "\\u00f4\\u0090\\u0080\\u0080"); /* > U+10FFFF */
    /* inlen cuts a sequence in half: the half is not passed raw. */
    eq("\xc3\xa1", 1, "\\u00c3");
    /* The end of the buffer: the writer stops while 7 bytes remain (room
     * for one \uXXXX), the rule it always had, so a sequence of up to four
     * bytes is never cut in half -- it is all there, or not at all. */
    {
        char out[8];
        xapi_jesc(out, sizeof out, "abcd\xc3\xa1", 6);
        checks++;
        if (strcmp(out, "a")) { fails++; printf("FAIL: tail [%s]\n", out); }
        char o12[12];
        xapi_jesc(o12, sizeof o12, "abcd\xf0\x9f\x98\x80", 8);
        checks++;
        if (strcmp(o12, "abcd\xf0\x9f\x98\x80")) { fails++; printf("FAIL: whole [%s]\n", o12); }
    }
    printf("xapi_jesc: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
