#!/usr/bin/env python3
"""Compares the C++ workbench with the Python workbench on every scenario of workbench_scenarios.json.

Each scenario is run twice, with fresh data and working folders: by workbench_pilot.py (meradb/tui.py driven with
Textual's Pilot) and by the wb_probe program (the C++ workbench session). Both print the observables of every
`snapshot` step as JSON; they are normalised (folders, timings, export paths) and compared snapshot by snapshot.

    python workbench_diff.py --probe path/to/wb_probe [--only NAME ...]

Exit code: 0 all match, 1 a mismatch or a failed run, 77 (ctest SKIP_RETURN_CODE) when Textual is not installed.
"""
import argparse
import difflib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
SCENARIOS = HERE / "workbench_scenarios.json"
PILOT = HERE / "workbench_pilot.py"
TIMEOUT = 120


def textual_available():
    try:
        import textual  # noqa: F401
        from textual.widgets.text_area import Selection  # noqa: F401
    except Exception:
        return False
    return True


def clean_env():
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    return env


def spellings(path):
    """The ways a folder may be spelled in output: as given, with the other slash, and resolved."""
    forms = {str(path), str(path).replace("\\", "/"), str(path).replace("/", "\\")}
    try:
        resolved = str(pathlib.Path(path).resolve())
        forms |= {resolved, resolved.replace("\\", "/"), resolved.replace("/", "\\")}
    except OSError:
        pass
    return sorted(forms, key=len, reverse=True)  # longest first, so a form is not eaten by a shorter one


def normalise(value, replacements):
    if isinstance(value, str):
        for form, token in replacements:
            value = value.replace(form, token)
        value = re.sub(r"\(\d+\.\d ms\)", "(<ms>)", value)
        value = re.sub(r"CSV mein save: .*", "CSV mein save: <EXPORT>", value)
        return value
    if isinstance(value, list):
        return [normalise(v, replacements) for v in value]
    if isinstance(value, dict):
        return {k: normalise(v, replacements) for k, v in value.items()}
    return value


def run_side(command, data, cwd):
    result = subprocess.run(command + ["--data", data, "--cwd", cwd], capture_output=True, timeout=TIMEOUT,
                            env=clean_env(), cwd=cwd)
    if result.returncode != 0:
        raise RuntimeError("exit code %d\n%s" % (result.returncode, result.stderr.decode("utf-8", "replace")[-2000:]))
    return json.loads(result.stdout.decode("utf-8"))


def pretty(value):
    return json.dumps(value, indent=1, ensure_ascii=False).splitlines()


def compare(name, pilot_out, probe_out):
    """Returns the list of mismatch descriptions for one scenario."""
    problems = []
    for label in list(pilot_out) + [l for l in probe_out if l not in pilot_out]:
        if label not in pilot_out or label not in probe_out:
            problems.append("%s/%s: snapshot only on the %s side" % (name, label, "Python" if label in pilot_out else "C++"))
            continue
        if pilot_out[label] != probe_out[label]:
            diff = difflib.unified_diff(pretty(pilot_out[label]), pretty(probe_out[label]),
                                        "python %s/%s" % (name, label), "cpp %s/%s" % (name, label), lineterm="", n=2)
            problems.append("\n".join(diff))
    return problems


def run_scenario(name, probe):
    base = tempfile.mkdtemp(prefix="meradb_wbdiff_")
    try:
        sides = {}
        for side in ("python", "cpp"):
            data = os.path.join(base, side + "_data")
            cwd = os.path.join(base, side + "_cwd")
            os.mkdir(data)
            os.mkdir(cwd)
            if side == "python":
                command = [sys.executable, str(PILOT), str(SCENARIOS), name]
            else:
                command = [probe, str(SCENARIOS), name]
            raw = run_side(command, data, cwd)
            replacements = [(form, "<DATA>") for form in spellings(data)] + [(form, "<CWD>") for form in spellings(cwd)]
            replacements.sort(key=lambda r: len(r[0]), reverse=True)
            sides[side] = normalise(raw, replacements)
        return compare(name, sides["python"], sides["cpp"])
    finally:
        shutil.rmtree(base, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, help="path to the wb_probe executable")
    parser.add_argument("--only", nargs="*", help="run only these scenario names")
    args = parser.parse_args()
    if not textual_available():
        print("workbench_diff: Textual is not installed; skipping")
        return 77
    args.probe = os.path.abspath(args.probe)
    scenarios = [s["name"] for s in json.load(open(SCENARIOS, encoding="utf-8"))]
    if args.only:
        scenarios = [n for n in scenarios if n in args.only]
    failed = 0
    for name in scenarios:
        try:
            problems = run_scenario(name, args.probe)
        except Exception as e:  # a crash or a timeout is a failure of that scenario
            problems = ["%s: run failed: %s" % (name, e)]
        if problems:
            failed += 1
            print("MISMATCH %s" % name)
            for p in problems:
                print(p)
        else:
            print("ok       %s" % name)
    print("%d of %d scenarios match" % (len(scenarios) - failed, len(scenarios)))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
