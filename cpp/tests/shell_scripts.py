# cpp/tests/shell_scripts.py
"""
The scripts typed into the shell by the shell tests: gen_shell_transcripts.py records what the Python shell
prints for each (golden_transcripts.h, replayed by the C++ unit tests) and shell_diff.py drives both real
shells with the same text and diffs everything they print.

Non-ASCII characters are built with chr() so this file stays plain ASCII.
"""

E_ACUTE = chr(0xE9)
SMILE = chr(0x1F600)
NAMASTE = "".join(chr(c) for c in (0x928, 0x92E, 0x938, 0x94D, 0x924, 0x947))
NBSP = chr(0xA0)
IDEOGRAPHIC_SPACE = chr(0x3000)

SCRIPTS = {
    # ---- statements, multi-line input, results -------------------------------------------------------
    "basic": (
        "BANAO TABLE t (id INT MUKHYA KUNJI, n TEXT);\n"
        "DAALO MEIN t (id, n) MAAN (1, 'Ravi'),\n"
        "  (2, 'Priya');\n"
        "DIKHAO * SE t;\n"
        ".tables\n"
        ".schema t\n"
    ),
    "unicode": (
        "BANAO TABLE u (id INT, n TEXT);\n"
        f"DAALO MEIN u MAAN (1, '{E_ACUTE}{SMILE}'), (2, '{NAMASTE}'), (3, 'plain');\n"
        "DIKHAO * SE u;\n"
        "DIKHAO n SE u JAHAN id = 2;\n"
    ),
    "long_multi_line": (
        "BANAO TABLE m (a INT, b TEXT);\n"
        "DAALO MEIN m MAAN (1, 'x'), (2, 'y'), (3, 'z');\n"
        "DIKHAO\n"
        "  a,\n"
        "  b\n"
        "SE m\n"
        "JAHAN a > 1\n"
        "KRAM a ULTA\n"
        ";\n"
    ),
    "several_statements_one_line": "DIKHAO TABLES; BANAO TABLE a (x INT); DIKHAO TABLES; BATAO nosuch;\n",
    "samjhao": "BANAO TABLE e (id INT);\nSAMJHAO DIKHAO * SE e JAHAN id = 1;\n",
    "use_database": (
        "BANAO DATABASE college;\nISTEMAL college;\nBANAO TABLE s (id INT);\nDIKHAO TABLES;\nISTEMAL main;\nDIKHAO TABLES;\n"
    ),
    "transaction": (
        "SHURU;\nBANAO TABLE x (a INT);\nDAALO MEIN x MAAN (1);\nDIKHAO * SE x;\nWAPAS;\nDIKHAO TABLES;\nSHURU;\nPAKKA;\n"
    ),
    "trigger_one_line": (
        "BANAO TABLE s (id INT, n TEXT);\n"
        "BANAO TABLE log (msg TEXT);\n"
        "BANAO TRIGGER t1 BAAD DAALO PAR s SHURU DAALO MEIN log MAAN ('naya'); KHATAM;\n"
        "DAALO MEIN s MAAN (1, 'a');\n"
        "DIKHAO * SE log;\n"
    ),
    # a body written over several lines is cut at its first `;` (the shell only looks at line ends)
    "trigger_multi_line": (
        "BANAO TABLE s (id INT);\nBANAO TABLE log (msg TEXT);\n"
        "BANAO TRIGGER t1 BAAD DAALO PAR s SHURU\n  DAALO MEIN log MAAN ('x');\nKHATAM;\n.tables\n"
    ),
    "users_local": "BANAO USER ravi GUPT 'pw';\nADHIKAR DO SAB PAR t KO ravi;\nDIKHAO TABLES;\n",
    # ---- errors ----------------------------------------------------------------------------------------
    "errors": (
        "DIKHAO * SE gayab;\n"
        "DIKHAO 1;\n"
        "DAALO MEIN\n"
        "BANAO TABLE a (x INT);\n"
        "BANAO TABLE a (x INT);\n"
        "BANAO TABLE a (x INT);\n"
        "DIKHAO * SE gayab; BANAO TABLE b (y INT); DIKHAO * SE b;\n"
    ),
    "string_semicolon": "DIKHAO 'a;\nb';\n",
    "unterminated_string": "DIKHAO 'bina band;\nDIKHAO TABLES;\n",
    # ---- dot-commands ------------------------------------------------------------------------------------
    "dot_commands": (
        ".help join\n.HELP  foreign key\n.help zzzz\n.tables\n.schema nosuch\n.schema\n.run\n"
        ".run no_such_file.mdb\n.run nodir/file.mdb\n.bogus arg\n.exit now\nDIKHAO TABLES;\n"
    ),
    "help_full": ".help\n.exit\n",
    "help_topics": ".HELP SANDARBH\n.help  Foreign   KEY \n.help   \n.exit\n",
    "quit": ".quit\n",
    "nikal": ".NIKAL\n",
    "indented_dot": "   .tables  \n\t.exit\n",
    "dot_inside_statement": "DIKHAO\n.tables\n;\n",
    # ---- the "a blank line starts a statement" quirk ---------------------------------------------------------
    "blank_line_quirk": "\n.tables\n;\n.tables\n",
    "semicolon_only": ";\n  ;  \n.tables\n",
    "comments": "-- hi;\nDIKHAO 1 SE x;\n-- trailing\n",
    "unicode_space": (
        f"BANAO TABLE w (id INT);{NBSP}\nDIKHAO * SE w ;{IDEOGRAPHIC_SPACE}\n.tables\n"
    ),
    # ---- end of input -------------------------------------------------------------------------------------
    "eof_empty": "",
    "eof_in_statement": "DIKHAO * SE",
    "eof_partial_last_line": "BANAO TABLE p (x INT);",
    "eof_after_newline": "BANAO TABLE p (x INT);\n",
    # ---- newline styles and stray characters ------------------------------------------------------------------
    "crlf": "BANAO TABLE c (id INT);\r\nDAALO MEIN c MAAN (1);\r\nDIKHAO * SE c;\r\n",
    "lone_cr": "BANAO TABLE c (id INT);\rDAALO MEIN c MAAN (1);\rDIKHAO * SE c;\r",
    "ctrl_z_in_pipe": "BANAO TABLE z (id INT);\n\x1a\n.tables\n;\n",
}

# The tokenizer's "Ye character samajh nahi aaya: '<c>'" shows an unprintable character raw, where Python's repr()
# escapes it ('\xa0', '\x1a'). A core divergence (docs/CPP.md), not a shell one: not compared.
KNOWN_REPR_DIVERGENCE = ["unicode_space", "ctrl_z_in_pipe"]
