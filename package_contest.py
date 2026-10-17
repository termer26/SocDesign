"""Package verified sources, Linux/Windows executables, sample and local reports."""
import hashlib
import io
import json
from pathlib import Path
import tarfile


def main():
    project = Path(__file__).resolve().parent
    workspace = project.parent
    sample = workspace / "case/cases/sample001"
    if not sample.is_dir():
        sample = workspace / "case/case1/cases/sample001"
    output = project / "output/contest/eda-3dic-router.tar.gz"
    selected = []
    for directory, suffixes in (("src", {".cpp"}), ("include", {".hpp"}),
                                ("tests", {".cpp", ".py", ".lef", ".def"})):
        selected.extend(p for p in (project / directory).rglob("*") if p.is_file() and p.suffix in suffixes)
    selected.extend(project / name for name in (
        "CMakeLists.txt", "CONTEST.md", "README.md", "build_contest.sh", "run_contest.sh",
        "benchmark_contest.py", "package_contest.py", "build-contest-linux/contest_router",
        "build-contest/contest_router.exe"))
    selected.extend(sample.glob("sample001.input.*"))
    selected.append(sample / "sample001.thermal.input.json")
    for variant in ("contest", "contest-linux"):
        directory = project / "output" / variant
        selected.extend(p for p in directory.iterdir() if p.is_file() and p.suffix in {".guide", ".json", ".md", ".log"})
    selected.append(project / "build-contest/Testing/Temporary/LastTest.log")
    selected = sorted(set(selected))
    for path in selected:
        if not path.is_file():
            raise FileNotFoundError(path)
    for variant in ("contest", "contest-linux"):
        for name in ("baseline", "optimized", "stress_baseline", "stress_optimized"):
            report = json.loads((project / f"output/{variant}/{name}.json").read_text(encoding="utf-8"))
            if report["nets"]["failed"] or not report["validation"]["passed"]:
                raise RuntimeError(f"Unverified report: {variant}/{name}")
    native = json.loads((project / "output/contest/openroad_guide_validation.json").read_text(encoding="utf-8"))
    if not native["passed"]:
        raise RuntimeError("OpenROAD guide readback has not passed")
    manifest = {str(p.relative_to(workspace)).replace("\\", "/"):
                {"bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in selected}
    header = """# 3DIC global-routing solver

Read cpp_router/CONTEST.md for architecture, input format, validation and limits.
Run from this directory (Ubuntu 22.04, x86_64):

```bash
bash cpp_router/run_contest.sh \\
  --lef case/cases/sample001/sample001.input.lef \\
  --def case/cases/sample001/sample001.input.def \\
  --thermal case/cases/sample001/sample001.thermal.input.json \\
  --out result.guide --report result.json
```

Rebuild: `bash cpp_router/build_contest.sh` (g++ 11.4 / C++17; no third-party C++ dependencies).
Windows binary: cpp_router/build-contest/contest_router.exe.
Local results: cpp_router/output/contest-linux/comparison.md.
This is a tested competition baseline. No official score or DRC signoff is claimed.
The sample thermal map is synthetic. SHA256SUMS.json records all bundled source files.
"""
    header = header.replace("case/cases/sample001", sample.relative_to(workspace).as_posix())
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, "w:gz") as archive:
        for path in selected:
            name = "eda-3dic-router/" + path.relative_to(workspace).as_posix()
            info = archive.gettarinfo(str(path), arcname=name)
            info.mode = 0o755 if path.suffix == ".sh" or path.name == "contest_router" else 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            with path.open("rb") as stream:
                archive.addfile(info, stream)
        for name, data in (("README.md", header.encode("utf-8")),
                           ("SHA256SUMS.json", (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))):
            info = tarfile.TarInfo("eda-3dic-router/" + name)
            info.mode = 0o644
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    # Verify every member, including byte hashes and Linux executable permission.
    with tarfile.open(output, "r:gz") as archive:
        for name, expected in manifest.items():
            member = archive.getmember("eda-3dic-router/" + name)
            assert hashlib.sha256(archive.extractfile(member).read()).hexdigest() == expected["sha256"]
            if name.endswith("build-contest-linux/contest_router"):
                assert member.mode & 0o111
    print(f"Verified {len(selected)} files: {output} ({output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
