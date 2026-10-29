# -*- coding: utf-8 -*-
"""
PCB reader for EasyEDA Pro projects (.epro2): board outline, mounting holes, top-layer part outlines and estimated heights.
Pure Python, no FreeCAD dependency; shared by gen_enclosure.py and ATX_PSU/mechanical/gen_atx_case.py.

Coordinates: mm, origin at the lower-left corner of the board outline, X along the long board edge, Y up (EasyEDA top view).
"""
import json
import math
import zipfile

MIL = 0.0254

# Part height (mm) estimated from footprint name keywords; callers can override with heights, e.g. {'XT60': 8.3}
DEFAULT_HEIGHTS = (('XT60', 8.3), ('USB-C', 3.26), ('HSOF', 2.4), ('SMB', 2.4), ('SMA', 2.3),
                   ('LQFP', 1.6), ('HDR-SMD', 4.5), ('PZ2.0', 6.0), ('IND', 1.1), ('R2512', 0.7),
                   ('MSOP', 1.1), ('VSSOP', 1.1), ('SOT', 1.3), ('WQFN', 0.8), ('LED', 0.9), ('SOD', 0.7))


def load_docs(path):
    with zipfile.ZipFile(path) as z:
        name = next(n for n in z.namelist() if n.endswith('.epru'))
        text = z.read(name).decode('utf-8', 'replace')
    docs, cur = [], None
    for line in text.splitlines():
        if '||' not in line:
            continue
        a, b = line.split('||', 1)
        try:
            head = json.loads(a)
        except ValueError:
            continue
        b = b[:-1] if b.endswith('|') else b
        try:
            data = json.loads(b) if b else None
        except ValueError:
            data = None
        if head.get('type') == 'DOCHEAD':
            cur = {'head': data or {}, 'recs': []}
            docs.append(cur)
        elif cur is not None:
            cur['recs'].append((head, data))
    return docs


def doc_meta(d):
    for h, v in d['recs']:
        if h.get('type') == 'META' and isinstance(v, dict):
            return v
    return {}


def arc_points(p0, p1, sweep_deg, n=8):
    """Points on an arc from start point, end point and sweep angle (round outlines such as electrolytic capacitors are drawn as two arcs)."""
    th = math.radians(sweep_deg)
    (x0, y0), (x1, y1) = p0, p1
    c = math.hypot(x1 - x0, y1 - y0)
    if abs(th) < 1e-9 or c < 1e-9:
        return [p1]
    k = (c / 2) / math.tan(th / 2)                       # chord midpoint to center (signed)
    cx = (x0 + x1) / 2 - (y1 - y0) / c * k
    cy = (y0 + y1) / 2 + (x1 - x0) / c * k
    r, a0 = math.hypot(x0 - cx, y0 - cy), math.atan2(y0 - cy, x0 - cx)
    return [(cx + r * math.cos(a0 + th * i / n), cy + r * math.sin(a0 + th * i / n)) for i in range(1, n + 1)]


def path_points(p):
    """Vertices of a POLY / FILL path, arcs sampled and added, used to compute the outline."""
    if not p:
        return []
    if isinstance(p[0], list):
        return [q for sub in p for q in path_points(sub)]
    if p[0] == 'R':
        x, y, w, h = p[1:5]
        rot = math.radians(p[5]) if len(p) > 5 and p[5] else 0.0
        pts = [(x, y), (x + w, y), (x + w, y - h), (x, y - h)]
        ca, sa = math.cos(rot), math.sin(rot)
        return [(x + (u - x) * ca - (v - y) * sa, y + (u - x) * sa + (v - y) * ca) for u, v in pts]
    if p[0] == 'CIRCLE':
        cx, cy, r = p[1:4]
        return [(cx - r, cy - r), (cx + r, cy + r)]
    pts, i, mode = [], 0, 'L'
    while i < len(p):
        if isinstance(p[i], str):
            mode = p[i]
            i += 1
            continue
        n = 3 if mode in ('ARC', 'CARC') else 2
        seg = p[i:i + n]
        if len(seg) == n and not any(isinstance(q, str) for q in seg):
            end = (seg[-2], seg[-1])
            pts += arc_points(pts[-1], end, seg[0]) if n == 3 and pts else [end]
            i += n
        else:
            i += 1
    return pts


def footprint_info(docs):
    info = {}
    for d in docs:
        if d['head'].get('docType') != 'FOOTPRINT':
            continue
        shape, pads = [], []
        for h, v in d['recs']:
            if not isinstance(v, dict):
                continue
            if v.get('layerId') == 48 and h.get('type') in ('POLY', 'FILL'):
                shape += path_points(v.get('path'))
            elif h.get('type') == 'PAD':
                pd = v.get('defaultPad') or {}
                r = max(pd.get('width') or 0, pd.get('height') or 0) / 2
                pads += [(v['centerX'] - r, v['centerY'] - r), (v['centerX'] + r, v['centerY'] + r)]
        info[d['head'].get('uuid')] = {'title': doc_meta(d).get('title', ''), 'shape': shape, 'pads': pads}
    return info


def part_body(fp, heights=None):
    """Estimate the part body from the footprint name: ('cyl', diameter, height) or ('box', None, height)."""
    f = fp.upper()
    if f.startswith('CAP-TH_BD'):
        dia = float(f.split('BD')[1].split('-')[0])
        return 'cyl', dia, {13.0: 21.0, 10.0: 16.0}.get(dia, dia * 1.6) + 0.5   # including sleeve / rubber pad
    over = heights or {}
    for key, h in DEFAULT_HEIGHTS:
        if key in f:
            return 'box', None, over.get(key, h)
    return 'box', None, 0.9


def load_pcb(epro_path, pcb_title, heights=None):
    """Returns {'w', 'h', 'r', 'holes': [(x, y, d)], 'parts': [{ref, fp, kind, dia, h, x1, x2, y1, y2}]}."""
    docs = load_docs(epro_path)
    fps = footprint_info(docs)
    pcb = next(d for d in docs if d['head'].get('docType') == 'PCB' and doc_meta(d).get('title') == pcb_title)
    outline, holes, comps, attrs = None, [], {}, {}
    for h, v in pcb['recs']:
        if not isinstance(v, dict):
            continue
        t = h.get('type')
        if t == 'POLY' and v.get('layerId') == 11:
            outline = v['path']
        elif t == 'PAD' and (v.get('hole') or {}).get('width', 0) * MIL >= 2.5:
            holes.append((v['centerX'], v['centerY'], v['hole']['width']))
        elif t == 'COMPONENT':
            comps[h['id']] = v
        elif t == 'ATTR' and v.get('parentId'):
            attrs.setdefault(v['parentId'], {})[v.get('key')] = v.get('value')
    if not outline or outline[0] != 'R':
        raise RuntimeError('Board outline is not a single rounded rectangle; load_pcb() needs to be extended')
    ox, oy_top, ow, oh = outline[1:5]
    ox0, oy0 = ox, oy_top - oh
    board = {'w': ow * MIL, 'h': oh * MIL, 'r': outline[6] * MIL,
             'holes': [((x - ox0) * MIL, (y - oy0) * MIL, d * MIL) for x, y, d in holes], 'parts': []}
    for cid, c in comps.items():
        a = attrs.get(cid, {})
        fp = fps.get(a.get('Footprint'), {'title': '', 'shape': [], 'pads': []})
        local = fp['shape'] or fp['pads']
        if not local or c.get('layerId') != 1:
            continue
        ang = math.radians(c.get('angle') or 0)
        ca, sa = math.cos(ang), math.sin(ang)
        xs = [c['x'] + u * ca - w * sa for u, w in local]
        ys = [c['y'] + u * sa + w * ca for u, w in local]
        kind, dia, hgt = part_body(fp['title'], heights)
        board['parts'].append({'ref': a.get('Designator') or '?', 'fp': fp['title'], 'kind': kind, 'dia': dia,
                               'h': hgt, 'x1': (min(xs) - ox0) * MIL, 'x2': (max(xs) - ox0) * MIL,
                               'y1': (min(ys) - oy0) * MIL, 'y2': (max(ys) - oy0) * MIL})
    return board
