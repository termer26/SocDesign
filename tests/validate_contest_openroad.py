"""Independently read the sample routing guide into OpenROAD/OpenDB.

Run inside Linux/WSL with OpenROAD's Python interpreter:
    openroad -exit -python cpp_router/tests/validate_contest_openroad.py

CONTEST_GUIDE optionally selects another guide for the same sample001 LEF/DEF.
The default is cpp_router/output/contest/optimized.guide. All default paths are
relative to this script, so the current working directory does not matter.
This checks guide syntax and database readback/coverage only, not detailed
routing, pin accessibility, electrical connectivity, or DRC.
"""
import json
import os
import re
import sys
from collections import Counter
from pathlib import Path


BASE = Path(__file__).resolve().parents[1]
SAMPLE = BASE.parent / "case" / "cases" / "sample001"
if not SAMPLE.is_dir():
    SAMPLE = BASE.parent / "case" / "case1" / "cases" / "sample001"
REPORT = BASE / "output" / "contest" / "openroad_guide_validation.json"
EXPECTED_NETS = 203


def guide_path():
    value = os.environ.get("CONTEST_GUIDE")
    if not value:
        return BASE / "output" / "contest" / "optimized.guide"
    # Accept an absolute Windows path when the script is invoked under WSL.
    if os.name == "posix" and re.match(r"^[A-Za-z]:[\\/]", value):
        value = "/mnt/" + value[0].lower() + "/" + value[3:].replace("\\", "/")
    return Path(value).expanduser().resolve()


def tcl_word(value):
    """Quote one Tcl word without allowing substitution or command injection."""
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"').replace(
        "$", "\\$").replace("[", "\\[").replace("]", "\\]").replace(
        "\n", "\\n").replace("\r", "\\r") + '"'


def file_guides(path):
    """Count the written guide entries independently of the C++ implementation."""
    lines = [(number, line.strip()) for number, line in
             enumerate(path.read_text(encoding="utf-8").splitlines(), 1) if line.strip()]
    result = {}
    index = 0
    while index < len(lines):
        number, name = lines[index]
        if name in result:
            raise ValueError(f"Duplicate guide net {name!r} at line {number}")
        index += 1
        if index == len(lines) or lines[index][1] != "(":
            raise ValueError(f"Missing opening parenthesis after net {name!r}")
        index += 1
        rectangles = []
        while index < len(lines) and lines[index][1] != ")":
            number, line = lines[index]
            fields = line.split()
            if len(fields) != 5:
                raise ValueError(f"Expected x1 y1 x2 y2 layer at line {number}")
            x1, y1, x2, y2 = map(int, fields[:4])
            if x1 >= x2 or y1 >= y2:
                raise ValueError(f"Invalid guide rectangle at line {number}")
            rectangles.append((x1, y1, x2, y2, fields[4]))
            index += 1
        if index == len(lines):
            raise ValueError(f"Missing closing parenthesis for net {name!r}")
        index += 1
        result[name] = rectangles
    return result


def declared_nets(path):
    """Read the sample's NETS declarations, excluding nets synthesized by OpenDB."""
    text = path.read_text(encoding="utf-8")
    header = re.search(r"(?m)^\s*NETS\s+(\d+)\s*;", text)
    if header is None:
        raise ValueError("Source DEF has no NETS section")
    end = re.search(r"\bEND\s+NETS\b", text[header.end():])
    if end is None:
        raise ValueError("Source DEF has no END NETS")
    names = []
    for record in text[header.end():header.end() + end.start()].split(";"):
        if not record.strip():
            continue
        match = re.match(r"\s*-\s+(\S+)", record)
        if match is None:
            raise ValueError("Cannot parse a source DEF NETS declaration")
        names.append(match.group(1))
    if len(names) != int(header.group(1)) or len(set(names)) != len(names):
        raise ValueError("Source DEF NETS count mismatch or duplicate net name")
    return set(names)


def validate(report):
    from openroad import Tech, Design

    guide = Path(report["inputs"]["guide"])
    expected = file_guides(guide)
    tech = Tech()
    tech.readLef(report["inputs"]["lef"])
    design = Design(tech)
    design.readDef(report["inputs"]["def"])
    block = design.getBlock()
    all_nets = {net.getName(): net for net in block.getNets()}
    # OpenDB may synthesize additional dangling nets from DEF PINS declarations.
    # Match the contest's NETS section explicitly and disclose those extra nets.
    declared = declared_nets(Path(report["inputs"]["def"]))
    missing_declarations = sorted(declared - set(all_nets))
    if missing_declarations:
        report["declared_nets_missing_from_opendb"] = missing_declarations
        raise ValueError("OpenDB did not import every source DEF NETS declaration")
    nets = {name: all_nets[name] for name in declared}
    report["design"] = block.getName()
    report["source_db_nets"] = len(nets)
    report["source_db_total_nets"] = len(all_nets)
    report["opendb_nets_not_declared_in_nets"] = sorted(set(all_nets) - declared)
    report["guide_file_nets"] = len(expected)
    report["guide_file_rectangles"] = sum(map(len, expected.values()))
    report["source_net_types"] = dict(sorted(Counter(str(net.getSigType()) for net in nets.values()).items()))
    if len(nets) != EXPECTED_NETS:
        raise ValueError(f"Expected {EXPECTED_NETS} sample001 nets, found {len(nets)} in OpenDB")
    if set(expected) != set(nets):
        report["missing_guide_nets"] = sorted(set(nets) - set(expected))
        report["unknown_guide_nets"] = sorted(set(expected) - set(nets))
        raise ValueError("Guide net names do not exactly cover the source OpenDB nets")
    empty = sorted(name for name, rectangles in expected.items() if not rectangles)
    if empty:
        report["empty_guide_nets"] = empty
        raise ValueError("Guide file contains nets without rectangles")

    # Use the native Tcl command, not a substitute parser or C++ solver checker.
    status = design.evalTclString(
        "catch [list read_guides " + tcl_word(guide) + "] ::contestGuideReadError")
    if str(status) != "0":
        error = design.evalTclString("set ::contestGuideReadError")
        raise ValueError(f"OpenROAD read_guides failed: {error}")
    report["read_guides_succeeded"] = True
    counts = {name: len(list(net.getGuides())) for name, net in nets.items()}
    report["nets_with_db_guides"] = sum(count > 0 for count in counts.values())
    report["imported_db_guide_rectangles"] = sum(counts.values())
    report["db_guide_counts_per_net"] = dict(sorted(counts.items()))
    missing = sorted(name for name, count in counts.items() if count == 0)
    report["missing_db_guide_nets"] = missing
    mismatched = sorted(name for name, count in counts.items() if count != len(expected[name]))
    report["rectangle_count_mismatches"] = mismatched
    if missing or mismatched:
        raise ValueError("OpenDB guide coverage/counts differ from the written guide file")
    report["passed"] = True
    report["status"] = "PASS_OPENROAD_GUIDE_READBACK"


def main():
    report = {
        "schema_version": 1,
        "tool": "OpenROAD native read_guides and OpenDB Python API",
        "scope": "Guide syntax and OpenDB readback; exact sample001 net coverage and per-net rectangle counts",
        "expected_nets": EXPECTED_NETS,
        "inputs": {
            "lef": str(SAMPLE / "sample001.input.lef"),
            "def": str(SAMPLE / "sample001.input.def"),
            "guide": str(guide_path()),
        },
        "passed": False,
        "status": "FAIL_OPENROAD_GUIDE_READBACK",
        "read_guides_succeeded": False,
        "detailed_routing_run": False,
        "pin_access_checked": False,
        "electrical_connectivity_checked": False,
        "drc_checked": False,
        "errors": [],
    }
    try:
        validate(report)
    except Exception as error:
        report["errors"].append(f"{type(error).__name__}: {error}")
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print("OPENROAD_GUIDE_VALIDATION", json.dumps({
        key: report.get(key) for key in (
            "status", "expected_nets", "source_db_nets", "guide_file_nets",
            "nets_with_db_guides", "imported_db_guide_rectangles", "drc_checked", "errors")
    }))
    print("Report:", REPORT)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
