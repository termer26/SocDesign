"""Reproduce solver ablations on the provided sample; never computes an official score."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    project = Path(__file__).resolve().parent
    sample = project.parent / "case/cases/sample001"
    if not sample.is_dir():
        sample = project.parent / "case/case1/cases/sample001"
    output = (args.output or project / "output/contest").resolve()
    output.mkdir(parents=True, exist_ok=True)
    inputs = [sample / ("sample001." + suffix) for suffix in
              ("input.lef", "input.def", "thermal.input.json")]
    before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    binary = args.binary.resolve()
    common = [str(binary), "--lef", str(inputs[0]), "--def", str(inputs[1]), "--thermal", str(inputs[2])]
    variants = {
        "baseline": ["--iterations", "0", "--order", "def", "--thermal-weight", "0", "--present-weight", "0"],
        "optimized": [],
        "stress_baseline": ["--capacity-scale", "0.15", "--iterations", "0", "--order", "def", "--thermal-weight", "0", "--present-weight", "0"],
        "stress_optimized": ["--capacity-scale", "0.15"],
    }
    results = {}
    for name, options in variants.items():
        guide, report = output / f"{name}.guide", output / f"{name}.json"
        command = [*common, "--out", str(guide), "--report", str(report), *options]
        run = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=360)
        (output / f"{name}.log").write_text(run.stdout + run.stderr, encoding="utf-8")
        if run.returncode:
            raise RuntimeError(f"{name} exited {run.returncode}: {run.stderr}")
        result = json.loads(report.read_text(encoding="utf-8"))
        assert result["validation"]["passed"] and result["nets"]["failed"] == 0
        results[name] = {
            "command": command, "routed_nets": result["nets"]["routed"],
            "planar_um": result["wirelength"]["planar_um"],
            "equivalent_um": result["wirelength"]["equivalent_um"],
            "cross_die_vias": result["wirelength"]["cross_die_vias"],
            "total_overflow": result["congestion"]["total_overflow"],
            "thermal_pearson": result["thermal"]["pearson_raw"],
            "seconds": result["runtime"]["elapsed_seconds"],
            "peak_rss_mb": result["runtime"]["peak_rss_mb"],
        }
    after = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    assert before == after, "Source inputs changed"
    summary = {"official_score": None, "input_sha256": before,
               "note": "Local proxy metrics. Thermal data are synthetic. Stress capacities are scaled in memory, not official inputs.",
               "variants": results}
    (output / "comparison.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    rows = ["# sample001 reproducible local comparison", "", summary["note"], "",
            "| Variant | Routed | Planar um | Equivalent um | Cross-die | Overflow | Pearson | Seconds | Peak RSS MB |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for name, r in results.items():
        rows.append(f"| {name} | {r['routed_nets']} | {r['planar_um']:.3f} | {r['equivalent_um']:.3f} | {r['cross_die_vias']} | {r['total_overflow']} | {r['thermal_pearson']:.6f} | {r['seconds']:.4f} | {r['peak_rss_mb']:.2f} |")
    rows += ["", "Baseline: DEF net order, initial routing only, zero thermal and present-congestion weights.",
             "Optimized: short-net order, thermal-aware search and up to eight negotiated-congestion reroutes.",
             "Stress variants use capacity-scale=0.15 with a minimum of one track for previously positive cells.",
             "Equivalent length = planar length + ordinary vias * 1 um + cross-die vias * 8 um; pin access stubs excluded.",
             "Values from the supplied evaluation_report.json use a different model and are not directly comparable.", ""]
    (output / "comparison.md").write_text("\n".join(rows), encoding="utf-8")
    print("\n".join(rows))


if __name__ == "__main__":
    main()
