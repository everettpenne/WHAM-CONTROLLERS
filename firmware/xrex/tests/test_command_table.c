/*
 * test_command_table.c -- every pattern in the real command table
 * (src/app/commands/command_table.c) is reachable: its short form and its
 * long form, as a command or query, dispatch to that pattern's own row and
 * not to an earlier one that shadows it. The patterns are extracted from
 * command_table.c by the Makefile (command_patterns.inc), so this follows
 * the table as it changes.
 */
#include "scpi_parser.h"
#include "test_util.h"
#include <ctype.h>
#include <string.h>

static const char *const patterns[] = {
#include "command_patterns.inc"
};
#define N ((int)(sizeof(patterns) / sizeof(patterns[0])))

/* Short form: each level's leading uppercase run (keeps '*', digits, '?'). */
static void short_form(const char *p, char *out)
{
    int keep = 1;
    for (; *p != '\0'; p++)
    {
        if (*p == ':') { keep = 1; *out++ = ':'; continue; }
        if (*p == '?') { *out++ = '?'; continue; }
        if (islower((unsigned char)*p)) { keep = 0; }
        if (keep) { *out++ = *p; }
    }
    *out = '\0';
}

/* Short-form collisions that exist in THIS product's table and are deliberately not fixed (changing a pattern
   changes the wire protocol): listed in tests/known_short_collisions.inc, one product's list per repo (empty
   in a product with none). Their long forms are still checked. Remove an entry once its pattern is fixed;
   never add one to hide a new collision. This test file itself is shared, byte-identical, between the two
   products (python/check_shared.py). */
static const char *const known_short_collisions[] = {
#include "known_short_collisions.inc"
    NULL
};

static int known_collision(const char *pattern)
{
    for (size_t k = 0; known_short_collisions[k] != NULL; k++)
    {
        if (strcmp(pattern, known_short_collisions[k]) == 0) { return 1; }
    }
    return 0;
}

static int first_match(const char *mnemonic)
{
    for (int i = 0; i < N; i++)
    {
        if (scpi_match(patterns[i], mnemonic)) { return i; }
    }
    return -1;
}

int main(void)
{
    char s[128], l[128];
    CHECK(N > 10);   /* the extraction found the table */

    for (int i = 0; i < N; i++)
    {
        short_form(patterns[i], s);
        strcpy(l, patterns[i]);
        for (char *c = l; *c != '\0'; c++) { *c = (char)toupper((unsigned char)*c); }

        int fs = first_match(s), fl = first_match(l);
        if (known_collision(patterns[i]))
        {
            CHECK(fl == i);
            continue;
        }
        if (fs != i || fl != i)
        {
            printf("  pattern %-28s short %-18s -> %s   long -> %s\n", patterns[i], s,
                   fs < 0 ? "none" : patterns[fs], fl < 0 ? "none" : patterns[fl]);
        }
        CHECK(fs == i);
        CHECK(fl == i);
    }
    return test_report("test_command_table");
}
