"""Puts the Palantir lab script into the workbench's Queries list for the palantir database.

The lab was built by running palantir_lab.mdb with the command line, so the desktop workbench never saw those
statements. This splits the script at its banner comments (one entry per lab section, MySQL comments included) and
writes them to <repo>/data/.history/palantir.jsonl, the file the Queries tab reads. Run again to rebuild the list.
"""
import json, os, re, datetime

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.abspath(os.path.join(here, "..", ".."))
script = os.path.join(here, "palantir_lab.mdb")
out_dir = os.path.join(repo, "data", ".history")
banner = re.compile(r"^-- =+\s*$")

lines = open(script, encoding="utf-8").read().splitlines()
sections, current, in_title, seen = [], None, False, 0
for line in lines:
    if banner.match(line):
        if not in_title:                       # opening banner of a title block
            if current: sections.append(current)
            current, in_title, seen = [], True, seen + 1
        else:                                  # closing banner: the body follows
            in_title = False
        continue
    if current is not None:
        current.append(line)
if current: sections.append(current)

stamp = datetime.datetime.fromtimestamp(os.path.getmtime(script)).strftime("%Y-%m-%d %H:%M:%S")
entries = []
for sec in sections[1:]:                       # the first block is the file header
    text = "\n".join(sec).strip()
    if text: entries.append({"q": text, "t": stamp})
os.makedirs(out_dir, exist_ok=True)
with open(os.path.join(out_dir, "palantir.jsonl"), "w", encoding="utf-8", newline="\n") as f:
    for e in entries:
        f.write(json.dumps(e, ensure_ascii=False) + "\n")
print(f"{len(entries)} sections written to {os.path.join(out_dir, "palantir.jsonl")}")

# The writing area of the palantir database starts with the lab script, so it can be carried on from there: only the
# statements and their "-- MySQL:" comments (with the indented lines that continue them); the lab's own notes, banners
# and EXPECTED ERROR remarks are left out. An existing saved script is only replaced with --force (it may hold your edits).
import shutil, sys

def sql_only(text):
    out, in_mysql = [], False
    for line in text.splitlines():
        if line.startswith("-- MySQL:"):
            in_mysql = True
            out.append(line)
        elif in_mysql and re.match(r"--\s{2,}\S", line):
            out.append(line)
        elif line.lstrip().startswith("--") and not line.startswith(" "):
            in_mysql = False                      # a lab comment: dropped
        else:
            in_mysql = False
            if line.strip() or (out and out[-1].strip()):
                out.append(line.rstrip())
    return chr(10).join(out).strip() + chr(10)
target = os.path.join(out_dir, "palantir.script.mdb")
if not os.path.exists(target) or "--force" in sys.argv:
    with open(target, "w", encoding="utf-8", newline=chr(10)) as f:
        f.write(sql_only(open(script, encoding="utf-8").read()))
    print("editor script written to", target)
else:
    print("editor script kept (use --force to replace it)")
