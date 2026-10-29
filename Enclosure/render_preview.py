# -*- coding: utf-8 -*-
"""Check-image rendering for gen_enclosure.py (pure matplotlib, does not load FreeCAD).

Usage: python render_preview.py out/_preview.json out/
"""
import json
import os
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt                                  # noqa: E402
import numpy as np                                               # noqa: E402

plt.rcParams['font.sans-serif'] = ['Microsoft JhengHei', 'Microsoft YaHei', 'SimHei', 'DejaVu Sans']
plt.rcParams['axes.unicode_minus'] = False


def sections(d, out):
    fig, axs = plt.subplots(1, len(d['sections']), figsize=(16, 7))
    for ax, sec in zip(np.atleast_1d(axs), d['sections']):
        for name, color, wires in sec['items']:
            first = True
            for w in wires:
                p = np.asarray(w['pts'])
                if len(p) < 2:
                    continue
                if w['closed']:
                    ax.fill(p[:, 0], p[:, 1], color=color, alpha=0.55, label=name if first else None)
                    first = False
                ax.plot(p[:, 0], p[:, 1], color=color, lw=0.8)
        ax.set_aspect('equal')
        ax.grid(alpha=0.3)
        ax.set_xlabel('Y (mm, front -> back)')
        ax.set_ylabel('Z (mm)')
        ax.set_title(sec['title'])
        ax.legend(loc='upper left', fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(out, 'preview_section.png'), dpi=110)
    plt.close(fig)


def top(d, out):
    t = d['top']
    fig, ax = plt.subplots(figsize=(11, 7.5))
    for pl in t['outline']:
        p = np.asarray(pl)
        ax.plot(p[:, 0], p[:, 1], color='#555b62', lw=1.2)
    x0, y0, x1, y1 = t['board']
    ax.add_patch(plt.Rectangle((x0, y0), x1 - x0, y1 - y0, color='#1d7a3a', alpha=0.15))
    for ref, px1, py1, px2, py2, h in t['parts']:
        tall = h > 5
        ax.add_patch(plt.Rectangle((px1, py1), px2 - px1, py2 - py1, fill=tall, lw=0.6,
                                   color='#b08a2e' if tall else '#777', alpha=0.5 if tall else 0.8))
        if tall or ref.startswith(('H', 'POWER', 'U9')):
            ax.text((px1 + px2) / 2, (py1 + py2) / 2, '%s\n%.0fmm' % (ref, h), ha='center', va='center', fontsize=7)
    for name, color, (bx0, by0, bx1, by1) in t['modules']:
        ax.add_patch(plt.Rectangle((bx0, by0), bx1 - bx0, by1 - by0, fill=False, ec=color, lw=1.6, ls='--'))
        ax.text(bx0 + 1, by1 - 3, name + ' (slanted-face projection)', color=color, fontsize=9)
    ax.set_aspect('equal')
    ax.set_xlim(-3, d['W'] + 3)
    ax.set_ylim(-3, d['D'] + 3)
    ax.grid(alpha=0.3)
    ax.set_title('Top-view layout (front = bottom, left = USB-C, right = XT60)')
    fig.tight_layout()
    fig.savefig(os.path.join(out, 'preview_top.png'), dpi=110)
    plt.close(fig)


def main():
    with open(sys.argv[1], encoding='utf-8') as f:
        d = json.load(f)
    out = sys.argv[2]
    sections(d, out)
    top(d, out)


if __name__ == '__main__':
    main()
