#!/usr/bin/env python3
"""Task 3 graphs: communication delay vs EV grid size and vs line length.

Reads timing.csv (blocking) and timing_nonblocking.csv (non-blocking) and draws
the combined figure used in the slides: one panel for 2-D grids, one for line
topologies, each showing all four series
    neighbour query (REQUEST + REPLY)   blocking / non-blocking   solid lines
    base station report (REPORT + SUGGESTION) blocking / non-blocking  dashed lines

Usage:
    python plot_task3.py                         # CSVs in this folder, label "CAAS"
    python plot_task3.py --dir old_data --label "Task 1 stub, CAAS" --out task1
    python plot_task3.py --detail                # also the 4 per-message graphs with trend lines

Outputs: <out>_combined.png  <out>_grid.png  <out>_line.png  (+ <out>_detail_*.png with --detail)
"""
import argparse, os
import numpy as np, pandas as pd, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument("--dir", default=".", help="folder holding timing.csv and timing_nonblocking.csv")
ap.add_argument("--label", default="CAAS", help="text shown in the graph titles")
ap.add_argument("--out", default="task3", help="output file prefix")
ap.add_argument("--ymax", type=float, default=None, help="fix the y-axis maximum (ms)")
ap.add_argument("--detail", action="store_true", help="also write the four detailed graphs")
args = ap.parse_args()

FILES = {"blocking": "timing.csv", "non-blocking": "timing_nonblocking.csv"}
MSGS = {  # key: (count column, average column, legend name)
    "query":  ("nbr_query_count", "nbr_query_avg_ms", "Neighbour query"),
    "report": ("report_count",    "report_avg_ms",    "Base station report"),
}
# colour per series, matching the earlier Task 1 graphs
STYLE = {
    ("query",  "blocking"):     dict(color="#1f77b4", ls="-"),
    ("query",  "non-blocking"): dict(color="#2ca02c", ls="-"),
    ("report", "blocking"):     dict(color="#ff7f0e", ls="--"),
    ("report", "non-blocking"): dict(color="#9467bd", ls="--"),
}
data = {k: pd.read_csv(os.path.join(args.dir, f)) for k, f in FILES.items()}

def curve(df, topo, cnt, avg):
    """mean delay per x value, weighted by the number of messages timed in each run.
    grid: x = number of nodes (m*n), shapes with the same node count are pooled
    line: x = number of nodes in the line"""
    d = df[df.m == 1] if topo == "line" else df[df.m >= 2]
    d = d[d[cnt] > 0].copy()
    d["w"] = d[avg] * d[cnt]
    g = d.groupby("nodes").agg(w=("w", "sum"), c=(cnt, "sum")).reset_index()
    g["delay"] = g.w / g.c
    return g.sort_values("nodes")

curves = {(t, m, i): curve(data[i], t, *MSGS[m][:2])
          for t in ("grid", "line") for m in MSGS for i in FILES}
ymax = args.ymax or max(1.1, 1.08 * max(c.delay.max() for c in curves.values()))

TITLE = {"grid": f"Communication delay vs EV grid size (2-D grids, {args.label})",
         "line": f"Communication delay vs line topology size (1 x n, {args.label})"}
XLAB = {"grid": "Number of EV charging nodes (m x n)",
        "line": "Number of EV charging nodes in the line"}

def panel(ax, topo):
    for m in MSGS:
        for i in FILES:
            c = curves[(topo, m, i)]
            ax.plot(c.nodes, c.delay, marker="o", ms=3.5, lw=1.4, **STYLE[(m, i)],
                    label=f"{MSGS[m][2]} - {i}")
    ax.set_title(TITLE[topo], fontsize=10.5)
    ax.set_xlabel(XLAB[topo]); ax.set_ylabel("Round-trip communication delay (ms)")
    ax.set_ylim(0, ymax); ax.grid(alpha=.3); ax.legend(fontsize=7.5, loc="upper left")

fig, axs = plt.subplots(1, 2, figsize=(15, 4.8))
panel(axs[0], "grid"); panel(axs[1], "line")
fig.tight_layout(); fig.savefig(f"{args.out}_combined.png", dpi=200); plt.close(fig)
print(f"wrote {args.out}_combined.png")
for topo in ("grid", "line"):
    fig, ax = plt.subplots(figsize=(7.8, 4.8))
    panel(ax, topo); fig.tight_layout(); fig.savefig(f"{args.out}_{topo}.png", dpi=200); plt.close(fig)
    print(f"wrote {args.out}_{topo}.png")

if args.detail:   # per-message graphs: every (m,n) run as a point, plus a linear trend line
    DCOL = {"blocking": "#c0504d", "non-blocking": "#3b7dd8"}
    for topo in ("grid", "line"):
        for m, (cnt, avg, name) in MSGS.items():
            fig, ax = plt.subplots(figsize=(7.5, 4.4))
            for i, df in data.items():
                d = df[df.m == 1] if topo == "line" else df[df.m >= 2]
                d = d[d[cnt] > 0].copy(); d["w"] = d[avg] * d[cnt]
                g = d.groupby(["m", "n", "nodes"]).agg(w=("w", "sum"), c=(cnt, "sum")).reset_index()
                g["delay"] = g.w / g.c
                x = g.nodes
                ax.scatter(x, g.delay, s=22, color=DCOL[i], alpha=.8)
                if len(g) > 2:
                    s, b = np.polyfit(x, g.delay, 1); xs = np.linspace(x.min(), x.max(), 50)
                    ax.plot(xs, s * xs + b, color=DCOL[i], lw=2, label=f"{i}  (trend {s*1000:+.1f} µs per node)")
            ax.set_xlabel(XLAB[topo]); ax.set_ylabel("average delay (ms)")
            ax.set_title(f"{name}: {'line topology' if topo=='line' else '2-D grid'} ({args.label})", fontsize=11)
            ax.set_ylim(bottom=0); ax.grid(alpha=.3); ax.legend(fontsize=9)
            fig.tight_layout(); fig.savefig(f"{args.out}_detail_{topo}_{m}.png", dpi=200); plt.close(fig)
            print(f"wrote {args.out}_detail_{topo}_{m}.png")
