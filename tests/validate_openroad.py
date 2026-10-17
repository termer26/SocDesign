"""独立集成验证；在已安装 OpenROAD 的 Linux/WSL 中运行。

openroad -exit -python cpp_router/tests/validate_openroad.py
本脚本只验证坐标、DEF 回读、同一 net 的几何连通性，不执行 DRC。
"""
import csv
import json
import re
from collections import Counter, defaultdict
from pathlib import Path
from openroad import Tech, Design

BASE = Path(__file__).resolve().parents[1]


def load(lef_path, def_path):
    tech = Tech()
    tech.readLef(str(lef_path))
    design = Design(tech)
    design.readDef(str(def_path))
    return tech, design, design.getBlock()


def db_rectangles(block):
    """从 OpenDB 的 placement origin、orientation、原始 pin geometry 独立取坐标。"""
    transforms = {
        "R0": lambda x, y: (x, y), "R90": lambda x, y: (-y, x),
        "R180": lambda x, y: (-x, -y), "R270": lambda x, y: (y, -x),
        "MY": lambda x, y: (-x, y), "MX": lambda x, y: (x, -y),
        "MYR90": lambda x, y: (-y, -x), "MXR90": lambda x, y: (y, x),
    }
    result = []
    for inst in block.getInsts():
        ox, oy = inst.getOrigin()  # OpenDB 已处理 LEF ORIGIN 与放置外框的关系。
        transform = transforms[str(inst.getOrient())]
        for iterm in inst.getITerms():
            for mpin in iterm.getMTerm().getMPins():
                for box in mpin.getGeometry():
                    corners = [transform(x, y) for x in (box.xMin(), box.xMax())
                               for y in (box.yMin(), box.yMax())]
                    result.append(("INSTANCE", inst.getName(), iterm.getMTerm().getName(),
                                   box.getTechLayer().getName(),
                                   ox + min(p[0] for p in corners), oy + min(p[1] for p in corners),
                                   ox + max(p[0] for p in corners), oy + max(p[1] for p in corners)))
    for bterm in block.getBTerms():
        for bpin in bterm.getBPins():
            for box in bpin.getBoxes():
                result.append(("PORT", "PIN", bterm.getName(), box.getTechLayer().getName(),
                               box.xMin(), box.yMin(), box.xMax(), box.yMax()))
    return result


def csv_rectangles(path):
    with path.open(encoding="utf-8", newline="") as stream:
        return [(r["kind"], r["instance"], r["pin"], r["layer"],
                 *[int(r[k]) for k in ("x1_dbu", "y1_dbu", "x2_dbu", "y2_dbu")])
                for r in csv.DictReader(stream)]


def net_signature(block):
    return {n.getName(): (str(n.getSigType()),
                          sorted((t.getInst().getName(), t.getMTerm().getName()) for t in n.getITerms()),
                          sorted(t.getName() for t in n.getBTerms())) for n in block.getNets()}


def parse_routes(path, via_layers):
    """直接从输出 DEF 提取同层线段和过孔，不依赖 C++ 的 parser。"""
    text = path.read_text(encoding="utf-8")
    start = re.search(r"(?m)^\s*NETS\s+\d+\s*;", text).end()
    end = text.index("END NETS", start)
    routes = {}
    for record in text[start:end].split(";"):
        tokens = re.findall(r"\(|\)|\+|[^\s()+]+", record)
        if not tokens:
            continue
        assert tokens[0] == "-"
        name = tokens[1]
        connections = []
        i = 2
        while i < len(tokens) and tokens[i] == "(":
            assert tokens[i + 3] == ")"
            connections.append((tokens[i + 1], tokens[i + 2]))
            i += 4
        try:
            i = tokens.index("ROUTED", i) + 1
        except ValueError:
            continue
        segments, vias, nodes = [], [], set()
        layer = tokens[i]
        i += 1
        previous = None
        while i < len(tokens):
            value = tokens[i]
            if value == "NEW":
                layer = tokens[i + 1]
                previous = None
                i += 2
            elif value == "(":
                x, y = int(tokens[i + 1]), int(tokens[i + 2])
                assert tokens[i + 3] == ")"
                point = (layer, x, y)
                nodes.add(point)
                if previous is not None and point != previous:
                    assert point[1] == previous[1] or point[2] == previous[2], "Diagonal DEF wire"
                    segments.append((previous, point))
                previous = point
                i += 4
            elif value in via_layers:
                assert previous is not None
                metals = via_layers[value]
                assert len(metals) == 2 and layer in metals
                other = next(m for m in metals if m != layer)
                destination = (other, previous[1], previous[2])
                vias.append((previous, destination))
                nodes.add(destination)
                layer, previous = other, destination
                i += 1
            else:
                raise AssertionError(f"Unexpected route token {value} in {name}")
        routes[name] = (connections, segments, vias, nodes)
    return routes


def on_segment(point, segment):
    a, b = segment
    return (point[0] == a[0] and min(a[1], b[1]) <= point[1] <= max(a[1], b[1])
            and min(a[2], b[2]) <= point[2] <= max(a[2], b[2]))


def verify_connectivity(routes, rectangles):
    pins = defaultdict(list)
    for _, inst, pin, layer, x1, y1, x2, y2 in rectangles:
        pins[(inst, pin)].append((layer, x1, y1, x2, y2))
    for name, (connections, segments, vias, initial_nodes) in routes.items():
        nodes = set(initial_nodes)
        # 分支可在长线段内部接入；把同层相交点也加入节点。
        for index, (a, b) in enumerate(segments):
            for c, d in segments[index + 1:]:
                if a[0] != c[0]:
                    continue
                if a[2] == b[2] and c[1] == d[1]:
                    p = (a[0], c[1], a[2])
                elif a[1] == b[1] and c[2] == d[2]:
                    p = (a[0], a[1], c[2])
                else:
                    continue
                if on_segment(p, (a, b)) and on_segment(p, (c, d)):
                    nodes.add(p)
        graph = defaultdict(set)
        for segment in segments:
            a, b = segment
            axis = 1 if a[2] == b[2] else 2
            along = sorted((p for p in nodes if on_segment(p, segment)), key=lambda p: p[axis])
            for first, second in zip(along, along[1:]):
                graph[first].add(second)
                graph[second].add(first)
        for a, b in vias:
            graph[a].add(b)
            graph[b].add(a)
        root = next(iter(nodes))
        reached, todo = {root}, [root]
        while todo:
            for neighbor in graph[todo.pop()]:
                if neighbor not in reached:
                    reached.add(neighbor)
                    todo.append(neighbor)
        assert reached == nodes, f"Disconnected route pieces in {name}"
        for connection in connections:
            attached = any(p[0] == layer and x1 <= p[1] <= x2 and y1 <= p[2] <= y2
                           for layer, x1, y1, x2, y2 in pins[connection] for p in reached)
            assert attached, f"Missing physical pin access: {name}: {connection}"


def verify(lef, source, output, csv_path):
    tech, design, block = load(lef, source)
    rectangles = db_rectangles(block)
    actual = csv_rectangles(csv_path)
    assert Counter(rectangles) == Counter(actual), "Pin CSV disagrees with OpenDB geometry"
    signatures = net_signature(block)
    placements = {i.getName(): (i.getMaster().getName(), tuple(i.getLocation()), str(i.getOrient()))
                  for i in block.getInsts()}
    _, routed_design, routed_block = load(lef, output)
    assert signatures == net_signature(routed_block), "Net connections/properties changed"
    assert placements == {i.getName(): (i.getMaster().getName(), tuple(i.getLocation()), str(i.getOrient()))
                          for i in routed_block.getInsts()}, "Instance placements changed"
    assert Counter(rectangles) == Counter(db_rectangles(routed_block)), "Output pin geometry changed"
    via_layers = {v.getName(): sorted({box.getTechLayer().getName() for box in v.getBoxes()
                                     if str(box.getTechLayer().getType()) == "ROUTING"})
                  for v in tech.getTech().getVias()}
    routes = parse_routes(output, via_layers)
    verify_connectivity(routes, rectangles)
    for net in routed_block.getNets():
        if net.getName() in routes:
            assert net.getWire() is not None, f"OpenROAD did not read wires for {net.getName()}"
    summary = {"design": block.getName(), "pin_rectangles_checked": len(rectangles),
               "routed_nets_checked": len(routes), "pin_coordinates_match_opendb": True,
               "def_readback": True, "per_net_geometry_connected": True,
               "drc_checked": False}
    print("OPENROAD_VALIDATION", json.dumps(summary))
    return summary


sample = BASE.parent / "case/cases/sample001"
if not sample.is_dir():
    sample = BASE.parent / "case/case1/cases/sample001"
reports = [
    verify(BASE / "tests/fixtures/tiny.lef", BASE / "tests/fixtures/tiny.def",
           BASE / "output/tiny.routed.def", BASE / "output/tiny.pins.csv"),
    verify(sample / "sample001.input.lef", sample / "sample001.input.def",
           BASE / "output/sample001.routed.def", BASE / "output/sample001.pins.csv"),
]
(BASE / "output/openroad_validation.json").write_text(json.dumps(reports, indent=2) + "\n", encoding="utf-8")
print("PASS: OpenROAD readback, every pin rectangle, and every routed net's geometric connectivity.")
