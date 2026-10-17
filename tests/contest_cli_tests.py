"""End-to-end CLI contracts; no third-party Python packages required."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = Path(sys.argv[1]).resolve()
    source = Path(__file__).resolve().parents[1]
    lef = source / "tests/fixtures/tiny.lef"
    design = source / "tests/fixtures/tiny.def"
    originals = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in (lef, design)}
    checks = 0

    def run(args, expected=0):
        nonlocal checks
        result = subprocess.run([str(binary), *map(str, args)], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=30)
        checks += 1
        assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
        return result

    base = ["--lef", lef, "--def", design]
    with tempfile.TemporaryDirectory(prefix="contest-cli-", dir=Path.cwd()) as temporary:
        root = Path(temporary)
        out = root / "nested dir/route.guide"
        run([*base, "--out", out, "--iterations", 2])
        report = json.loads(Path(str(out) + ".json").read_text())
        assert report["nets"] == {"total": 3, "routed": 3, "failed": 0, "errors": []}
        assert report["validation"]["passed"] and not report["validation"]["drc_verified"]
        assert report["official_score"] is None
        run([*base, "--check", out])
        first = out.read_bytes()
        assert b"\r" not in first, "Guides must use LF bytes for Linux EDA readers"
        run([*base, "--out", out, "--iterations", 2])
        assert first == out.read_bytes(), "Routing must be deterministic"

        # Invalid inputs cannot truncate a prior complete result or any source.
        bad_thermal = root / "broken.json"
        bad_thermal.write_text('{"nodes": [[0, 0, 0, NaN]]}')
        run([*base, "--out", out, "--thermal", bad_thermal], 1)
        assert first == out.read_bytes()
        run([*base, "--out", design], 1)
        run([*base, "--out", out, "--report", lef], 1)
        run([*base, "--out", out, "--report", out], 1)
        if os.name == "nt":
            run([*base, "--out", root / "NEW.guide", "--report", root / "new.guide"], 1)
            assert not (root / "NEW.guide").exists()
            for alias_name in ("NEW.guide.", "NEW.guide ", "NUL.guide", "CON.json"):
                run([*base, "--out", root / "NEW.guide", "--report", root / alias_name], 1)
        run([*base, "--out", out, "--iterations", -1], 1)
        run([*base, "--out", out, "--thermal-weight", "nan"], 1)
        run([*base, "--out", out, "--thermal-search-radius", -1], 1)
        run([*base, "--out", out, "--thermal-search-radius", 4097], 1)
        run([*base, "--out", out, "--tsv-route-trials", 0], 1)
        run([*base, "--out", out, "--tsv-route-trials", 33], 1)
        run([*base, "--out", out, "--via-cost", "1e308"], 1)
        run([*base, "--out", out, "--capacity-scale", 0], 1)
        run([*base, "--out", out, "--astar-weight", 0.5], 1)
        run([*base, "--out", out, "--max-grid-nodes", 1], 1)
        run([*base, "--out", out, "--grid-memory-mb", 0], 1)
        run([*base, "--out", out, "--thermal-coordinates", "unknown"], 1)
        run([*base, "--out", out, "--thermal-boundary-tolerance-dbu", -1], 1)
        run([*base, "--out", out, "--cross-via", "DOES_NOT_EXIST"], 1)
        run([*base, "--out", out, "--check", out], 1)
        run([*base, "--out"], 1)

        # Check the actual file: disconnected regions or missing nets must fail.
        corrupt = root / "corrupt.guide"
        corrupt.write_text("unknown_net\n(\n0 0 1000 1000 M1\n)\n")
        run([*base, "--check", corrupt], 2)
        checked = json.loads(Path(str(corrupt) + ".validation.json").read_text())
        assert not checked["validation"]["passed"]

        timeout = root / "timeout.guide"
        run([*base, "--out", timeout, "--time-limit", 0], 2)
        timed = json.loads(Path(str(timeout) + ".json").read_text())
        assert timed["runtime"]["time_limit_reached"]
        assert timed["nets"]["failed"] == 3 and not timed["validation"]["passed"]

        # A technology LEF plus cell LEF must behave identically to a combined file.
        text = lef.read_text()
        split = text.index("MACRO TEST")
        tech, cells = root / "tech.lef", root / "cells.lef"
        tech.write_text(text[:split] + "\nEND LIBRARY\n")
        cells.write_text(text[split:])
        separate = root / "separate.guide"
        run(["--lef", tech, "--lef", cells, "--def", design, "--out", separate, "--iterations", 2])
        assert separate.read_bytes() == first

        # File identity protection also catches hard-link aliases.
        alias = root / "input-alias.guide"
        copy = root / "input-copy.def"
        copy.write_bytes(design.read_bytes())
        try:
            os.link(copy, alias)
        except OSError:
            pass  # Some file systems do not expose hard links.
        else:
            run(["--lef", lef, "--def", copy, "--out", alias], 1)
            assert copy.read_bytes() == design.read_bytes()
        assert not list(out.parent.glob("*.tmp.*")), "Temporary outputs must be cleaned"

    assert all(hashlib.sha256(p.read_bytes()).hexdigest() == digest for p, digest in originals.items())
    print(f"PASS: {checks} CLI invocations; deterministic output, validation, timeouts, input protection, split LEFs")


if __name__ == "__main__":
    main()
