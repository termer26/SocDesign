"""Reproduce balanced or heat-focused public-case routing; optionally run the official Linux scorer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def linux_path(path):
    value = str(path.resolve()).replace('\\', '/')
    if len(value) > 2 and value[1] == ':':
        return '/mnt/' + value[0].lower() + value[2:]
    return value


def main():
    project = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--case-root', type=Path, default=project / 'case')
    parser.add_argument('--cases', nargs='+', choices=['001', '003', '005'], default=['001', '003', '005'])
    parser.add_argument('--output', type=Path, default=project / 'output/thermal-balanced')
    parser.add_argument('--profile', choices=['balanced','thermal'], default='balanced',
                        help='Balanced: public-case weights for wirelength near 90; thermal: the previous weight 6000')
    parser.add_argument('--thermal-weight', type=float, help='Override the selected profile for every requested case')
    parser.add_argument('--thermal-search-radius', type=int, help='Override the profile search radius in GCells')
    parser.add_argument('--tsv-route-trials',type=int,default=1)
    parser.add_argument('--time-limit', type=float, default=1800)
    parser.add_argument('--scorer', type=Path, help='Official Linux scorer executable; omitted for routing only')
    parser.add_argument('--score-only', action='store_true', help='Score existing guides instead of rerouting')
    parser.add_argument('--wsl-distro', default='ubun2', help='Windows only')
    args = parser.parse_args()
    if args.score_only and args.scorer is None:
        parser.error('--score-only requires --scorer')
    if args.scorer is not None:
        for name in ('score', 'POWV9.dat', 'POST9.dat'):
            path = args.scorer.resolve() if name == 'score' else args.scorer.resolve().parent / name
            if not path.is_file():
                parser.error(f'Missing scorer executable or FLUTE lookup table: {path}')
    binary = args.binary.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    summaries = []
    for case in args.cases:
        score_dir = output / f'score-case{case}'
        if score_dir.exists():
            raise RuntimeError(f'Refusing to replace a guide or report with existing official scores: {score_dir}; use a new --output directory')
        name = f'open_3d_case{case}'
        source = args.case_root.resolve() / name
        lef, placed, heat = (source / f'{name}.{suffix}' for suffix in
                             ('input.lef', 'input.def', 'thermal.input.json'))
        before = {str(p): digest(p) for p in (lef, placed, heat)}
        thermal_weight=args.thermal_weight
        if thermal_weight is None:
            thermal_weight={'001':600,'003':3000,'005':3000}[case] if args.profile=='balanced' else 6000
        thermal_radius=args.thermal_search_radius
        if thermal_radius is None:
            thermal_radius=32 if args.profile=='balanced' and case=='005' else 64
        guide, report = output / f'case{case}.guide', output / f'case{case}.json'
        if not args.score_only:
            text = placed.read_text(encoding='utf-8')
            dbu = int(re.search(r'UNITS\s+DISTANCE\s+MICRONS\s+(\d+)', text).group(1))
            patterns = re.findall(r'GCELLGRID\s+[XY]\s+-?\d+\s+DO\s+(\d+)\s+STEP\s+(\d+)', text)
            if not patterns:
                raise RuntimeError(f'No GCELLGRID in {placed}')
            pitch = statistics.median(int(step) for _, step in patterns) / dbu
            estimated_xy = max(int(n) for n, _ in patterns)**2
            del text
            # Preserve the previously validated wirelength settings, scaled by
            # physical grid spacing rather than hard-coding a case's dimensions.
            command = [str(binary), '--lef', str(lef), '--def', str(placed), '--thermal', str(heat),
                       '--out', str(guide), '--report', str(report), '--iterations', '0',
                       '--time-limit', str(args.time_limit), '--via-cost', str(4*pitch),
                       '--cross-via-cost', str(7*pitch), '--present-weight', '0.25',
                       '--astar-weight', '1.5' if estimated_xy > 1_000_000 else '1',
                       '--thermal-weight', str(thermal_weight),
                       '--thermal-search-radius', str(thermal_radius),
                       '--tsv-route-trials',str(args.tsv_route_trials)]
            print(f'Routing case{case}; log: {output / ("case"+case+".log")}', flush=True)
            (output / f'case{case}.command.json').write_text(json.dumps(command, indent=2), encoding='utf-8')
            with (output / f'case{case}.log').open('w', encoding='utf-8') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        local = json.loads(report.read_text(encoding='utf-8'))
        if local['nets']['failed'] or not local['validation']['passed']:
            raise RuntimeError(f'case{case}: incomplete or locally invalid route')
        summary = {'case': case, 'parameters':local['parameters'], 'routed_nets': local['nets']['routed'],
                   'correlation_above_ambient': local['thermal']['pearson_above_ambient'],
                   'planar_um': local['wirelength']['planar_um'],
                   'seconds': local['runtime']['elapsed_seconds'],
                   'guide_sha256': digest(guide), 'binary_sha256': digest(binary),
                   'inputs_sha256': before}
        if args.scorer is not None:
            work_dir = output / f'work-case{case}'
            work_dir.mkdir(parents=True, exist_ok=True)
            for name in ('POWV9.dat', 'POST9.dat'):
                shutil.copy2(args.scorer.resolve().parent / name, work_dir / name)
            score_args = [linux_path(guide), '--lefs', linux_path(lef), '--input-def', linux_path(placed),
                          '--thermal-json', linux_path(heat), '--output-dir', linux_path(score_dir)]
            if os.name == 'nt':
                command = ['wsl', '-d', args.wsl_distro, '--cd', linux_path(work_dir), '--',
                           linux_path(args.scorer), *score_args]
            else:
                command = [str(args.scorer.resolve()), *score_args]
            print(f'Scoring case{case}; log: {output / ("score-case"+case+".log")}', flush=True)
            with (output / f'score-case{case}.log').open('w', encoding='utf-8') as log:
                subprocess.run(command, cwd=work_dir, stdout=log, stderr=subprocess.STDOUT, check=True)
            official = json.loads((score_dir / 'evaluation_report.json').read_text(encoding='utf-8'))
            if official['status'] != 'PASS':
                raise RuntimeError(f'case{case}: official scorer did not pass')
            summary['official'] = official['score']
            summary['weighted_total'] = sum(official['score'][key+'_score'] * weight for key, weight in
                                             (('wirelength', 0.45), ('congestion', 0.30), ('thermal', 0.25)))
        after = {str(p): digest(p) for p in (lef, placed, heat)}
        if before != after:
            raise RuntimeError(f'case{case}: an input file changed during evaluation')
        summaries.append(summary)
        (output / 'summary.json').write_text(json.dumps(summaries, indent=2), encoding='utf-8')
        print(json.dumps(summary, ensure_ascii=False), flush=True)
    if all('official' in item for item in summaries):
        average = statistics.mean(item['official']['thermal_score'] for item in summaries)
        print(f'Official average thermal score: {average:.6f}', flush=True)


if __name__ == '__main__':
    main()
