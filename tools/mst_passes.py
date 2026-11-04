#!/usr/bin/env python3
"""tools/mst_passes.py TRACE [FPS] [--complete-frames]: GPU time by pass kind and encoder,
and GPU idle inside/between command buffers. Use --complete-frames to count frames from
command-buffer assignments instead of estimating them from FPS. BB_PASS_LABELS=1 names passes."""
import collections
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mst_summary as m


def complete_frame_rows(gpu_rows, assignments):
    """Select contiguous Metal frames with present commands, excluding capture boundaries."""
    frame_by_cb, presented = {}, set()
    for r in assignments:
        if 'bb-probe' not in r['process'][1]:
            continue
        cb, frame = r['cmdbuffer-id'][0], int(r['frame-number'][0])
        if cb in frame_by_cb and frame_by_cb[cb] != frame:
            raise ValueError('conflicting command-buffer frame assignments')
        frame_by_cb[cb] = frame
        if r['has-present'][0] == '1':
            presented.add(frame)
    gpu_rows = [r for r in gpu_rows if 'bb-probe' in r['process'][1]]
    observed = {frame_by_cb[r['cmdbuffer-id'][0]] for r in gpu_rows
                if r['cmdbuffer-id'][0] in frame_by_cb}
    candidates = sorted(presented & observed)
    if len(candidates) < 3:
        raise ValueError('not enough presented frames with GPU intervals')
    frames = set(candidates[1:-1])
    if max(frames) - min(frames) + 1 != len(frames):
        raise ValueError('missing frame assignments inside the selected window')
    selected = [r for r in gpu_rows if frame_by_cb.get(r['cmdbuffer-id'][0]) in frames]
    begin = min(int(r['start'][0]) for r in selected)
    end = max(int(r['start'][0]) + int(r['duration'][0]) for r in selected)
    if any(r['cmdbuffer-id'][0] not in frame_by_cb and begin <= int(r['start'][0]) < end
           for r in gpu_rows):
        raise ValueError('unassigned GPU intervals inside the selected window')
    return selected, len(frames)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace')
    parser.add_argument('fps', nargs='?', type=float)
    parser.add_argument('--complete-frames', action='store_true',
                        help='count frames with present commands; exclude capture boundaries')
    args = parser.parse_args()
    if args.complete_frames and args.fps is not None:
        parser.error('choose FPS normalization or --complete-frames')
    trace = args.trace
    labels, kinds = {}, {}
    for r in m.rows(m.export(trace, 'metal-application-intervals')):
        name = m.pass_label(r['event-label'][1])
        if 'bb-probe' in r['process'][1] and 'encoder-id' in r and name is not None:
            labels[r['encoder-id'][0]] = name.split(' (bb:')[0]
    for r in m.rows(m.export(trace, 'metal-application-encoders-list')):
        if 'bb-probe' in r['process'][1]:
            kinds[r['encoder-id'][0]] = re.sub(r'\d+', 'N', r['encoder-label'][1])
    total = collections.defaultdict(collections.Counter)
    encoders = collections.defaultdict(set)
    per_cb = collections.defaultdict(list)
    t0, t1 = None, 0
    gpu_rows = m.rows(m.export(trace, 'metal-gpu-intervals'))
    frame_count = None
    if args.complete_frames:
        try:
            gpu_rows, frame_count = complete_frame_rows(
                gpu_rows, m.rows(m.export(trace, 'metal-command-buffer-frame-assignment')))
        except ValueError as error:
            parser.error(str(error))
    for r in gpu_rows:
        if 'bb-probe' not in r['process'][1]:
            continue
        s, d = int(r['start'][0]), int(r['duration'][0])
        t0 = s if t0 is None else min(t0, s)
        t1 = max(t1, s + d)
        e = r['encoder-id'][0]
        key = re.sub(r' vs [0-9a-f]+ ps [0-9a-f]+', '', labels.get(e) or f'[{kinds.get(e, "?")}]')
        total[key][r['channel-name'][1]] += d
        encoders[key].add(e)
        per_cb[r['cmdbuffer-id'][0]].append((s, s + d))
    if t0 is None:
        parser.error('no GPU intervals of bb-probe')
    span = (t1 - t0) / 1e9
    fps = args.fps
    frames = frame_count if frame_count is not None else span * fps if fps else 1
    unit = 'ms/frame' if frame_count is not None or fps else 'ms'
    if frame_count is not None:
        print(f'{span:.3f} s GPU span, {frames} complete Metal frames ({frames / span:.2f} FPS)')
    else:
        print(f'{span:.1f} s traced' + (f', ~{frames:.0f} frames' if fps else ''))
    ranked = sorted(total.items(), key=lambda kv: -sum(kv[1].values()))
    for key, ch in ranked[:int(os.environ.get('TOP', 25))]:
        print(f"{sum(ch.values()) / 1e6 / frames:7.2f} {unit} (frag {ch['Fragment'] / 1e6 / frames:5.2f}"
              f" vtx {ch['Vertex'] / 1e6 / frames:5.2f} cs {ch['Compute'] / 1e6 / frames:5.2f})"
              f" {len(encoders[key]) / frames:6.1f} enc  {key[:100]}")
    intervals = sorted(x for v in per_cb.values() for x in v)
    busy, cur = 0, None
    for s, e in intervals:
        if cur is None or s > cur[1]:
            busy += cur[1] - cur[0] if cur else 0
            cur = [s, e]
        else:
            cur[1] = max(cur[1], e)
    busy += cur[1] - cur[0]
    inner = 0
    for v in per_cb.values():
        v.sort()
        u, c = 0, None
        for s, e in v:
            if c is None or s > c[1]:
                u += c[1] - c[0] if c else 0
                c = [s, e]
            else:
                c[1] = max(c[1], e)
        u += c[1] - c[0]
        inner += max(e for _, e in v) - v[0][0] - u
    whole = (t1 - t0)
    print(f'GPU busy {busy / 1e6 / frames:.1f} {unit}; idle inside command buffers {inner / 1e6 / frames:.1f},'
          f' between them {(whole - busy - inner) / 1e6 / frames:.1f}')


if __name__ == '__main__':
    main()
