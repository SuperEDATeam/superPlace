#!/usr/bin/env python3
"""Independent geometry and force analysis of opt-in GP snapshots (stdlib only)."""
import argparse
import collections
import csv
import json
import math
from pathlib import Path


def clip(a, b):
    r = (max(a[0], b[0]), max(a[1], b[1]), min(a[2], b[2]), min(a[3], b[3]))
    return r if r[2] > r[0] and r[3] > r[1] else None


def union_area(rects):
    rects = [r for r in rects if r is not None]
    xs = sorted({v for r in rects for v in (r[0], r[2])})
    area = 0.0
    for left, right in zip(xs, xs[1:]):
        spans = sorted((r[1], r[3]) for r in rects if r[0] < right and r[2] > left)
        length, high = 0., -math.inf
        for lo, hi in spans:
            if hi > high:
                length += hi - max(lo, high)
                high = hi
        area += (right - left) * length
    return area


def static_union_charge(box, rows, fixed, target):
    rows = [r for r in (clip(r, box) for r in rows) if r]
    fixed = [r for r in (clip(r, box) for r in fixed) if r]
    intersections = [clip(r, f) for r in rows for f in fixed]
    return target * ((box[2] - box[0]) * (box[3] - box[1]) -
                     union_area(rows) + union_area(intersections))


def analyze(path):
    meta = json.loads((path / 'metadata.json').read_text())
    dim = meta['grid_dim']; ox, oy = meta['origin']; sx, sy = meta['bin_step']
    target = None
    rows_by_y, fixed_by_bin = collections.defaultdict(list), collections.defaultdict(list)
    with (path / 'geometry.csv').open() as f:
        for r in csv.DictReader(f):
            rect = tuple(float(r[k]) for k in ['lx', 'ly', 'hx', 'hy'])
            x0, x1 = max(0, int(math.floor((rect[0]-ox)/sx))), min(dim-1, int(math.floor((rect[2]-ox)/sx)))
            y0, y1 = max(0, int(math.floor((rect[1]-oy)/sy))), min(dim-1, int(math.floor((rect[3]-oy)/sy)))
            for y in range(y0, y1+1):
                if r['kind'] == 'row':
                    rows_by_y[y].append(rect)
                else:
                    for x in range(x0, x1+1): fixed_by_bin[x*dim+y].append(rect)
    result = dict(stage=meta['stage'], point=meta['point'], iter=meta['iter'],
                  overflow=meta['overflow'], static_floor_tau=meta['static_floor_tau'])
    blocked = dark = static_error = positive_error = max_error = 0.
    occupied_bins = 0
    with (path / 'bins.csv').open() as f:
        for r in csv.DictReader(f):
            x, y = int(r['bx']), int(r['by'])
            box = tuple(float(r[k]) for k in ['lx', 'ly', 'hx', 'hy'])
            cap, terminal, base, over = (float(r[k]) for k in ['capacity','terminal_charge','base_charge','overflow_area'])
            if target is None: target = cap / (sx*sy)
            expected = static_union_charge(box, rows_by_y[y], fixed_by_bin.get(x*dim+y, ()), target)
            error = terminal + base - expected
            static_error += error; positive_error += max(0., error); max_error = max(max_error, abs(error))
            if over > 0:
                occupied_bins += 1
                if terminal >= .99*cap: blocked += over
                if base > 1e-5: dark += over
    comps = sorted(meta['components'], key=lambda c: c['overflow_area'], reverse=True)
    result.update(positive_bins=occupied_bins, components=len(comps),
        top5_overflow_fraction=sum(c['overflow_area'] for c in comps[:5])/max(meta['overflow_area'],1e-30),
        fully_blocked_overflow_fraction=blocked/max(meta['overflow_area'],1e-30),
        dark_bin_overflow_fraction=dark/max(meta['overflow_area'],1e-30),
        static_charge_error=static_error, static_positive_error=positive_error,
        static_max_bin_error=max_error, static_positive_error_over_denominator=positive_error/meta['overflow_denominator'])
    groups = collections.defaultdict(lambda: collections.defaultdict(float))
    samples = collections.defaultdict(list)
    with (path / 'nodes.csv').open() as f:
        for index, r in enumerate(csv.DictReader(f)):
            label = 'filler' if int(r['filler']) else 'real'
            g = groups[label]; g['count'] += 1
            wx, wy, fx, fy, gx, gy, dx, dy = (float(r[k]) for k in
                ['wl_x','wl_y','lambda_force_x','lambda_force_y','gradient_x','gradient_y','actual_dx','actual_dy'])
            nw, nf, ng = math.hypot(wx,wy), math.hypot(fx,fy), math.hypot(gx,gy)
            movement = math.hypot(dx,dy)
            g['wl_norm_sum'] += nw; g['lambda_force_norm_sum'] += nf
            g['gradient_norm_sum'] += ng; g['movement_sum'] += movement
            g['movement_max'] = max(g['movement_max'],movement)
            g['gradient_max'] = max(g['gradient_max'],ng)
            g['boundary_count'] += int(r['boundary'])
            if nw > 0 and nf > 0:
                cosine = (wx*fx+wy*fy)/(nw*nf)
                g['cancellation_cos_sum'] += cosine; g['cancellation_count'] += 1
                g['strong_cancellation_count'] += cosine > .9 and .5 < nw/nf < 2
            if index % 97 == 0:
                samples[label+'_movement'].append(movement)
                samples[label+'_gradient'].append(ng)
    for name, values in samples.items():
        values.sort(); result[name+'_sample_quantiles'] = [values[int((len(values)-1)*q)] for q in [.5,.9,.99]]
    result['node_groups'] = dict(groups)
    result['energy'] = meta['energy']; result['probes'] = meta['probes']
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('snapshots', nargs='+', type=Path)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    result = []
    for path in a.snapshots:
        value = analyze(path); value['source'] = str(path); result.append(value)
        print(json.dumps(value), flush=True)
    a.out.write_text(json.dumps(result,indent=2)+'\n')


if __name__ == '__main__':
    main()
