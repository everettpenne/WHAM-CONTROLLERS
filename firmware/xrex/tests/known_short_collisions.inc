/* XREX's known short-form collisions (found by test_command_table, 2026-09-26; NOT fixed, since changing the
   patterns changes the wire protocol): the digits of "GPOut09".."GPOut12" come after the lowercase part, so
   all four share the short form DIAG:GPO, which reaches GPOut12 (first in the table). */
    "DIAGnostic:GPOut11", "DIAGnostic:GPOut11?",
    "DIAGnostic:GPOut09", "DIAGnostic:GPOut09?",
    "DIAGnostic:GPOut10", "DIAGnostic:GPOut10?",
