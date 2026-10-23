#!/usr/bin/env python3
"""Task 2 evidence: how the report period changes the base station's answers.
Reads logs/period_<p>_run<r>.txt, prints a table, writes period_results.csv
and period_vs_outcomes.png."""
import re, glob, collections
import pandas as pd, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

rows = []
for f in glob.glob("logs/period_*_run*.txt"):
    t = open(f).read()
    g = lambda pat: float(re.search(pat, t).group(1))
    rows.append(dict(period=g(r"report period ([\d.]+) s"),
                     alerts=g(r"Congestion alerts\s*: (\d+)"),
                     with_nodes=g(r"answered with nodes\s*: (\d+)"),
                     none=g(r"answered 'none nearby' : (\d+)"),
                     messages=g(r"Total messages in system\s*: (\d+)")))
df = pd.DataFrame(rows).groupby("period").mean().reset_index()
df["pct_with_nodes"] = 100 * df.with_nodes / df.alerts
print(df.round(1).to_string(index=False))
df.to_csv("period_results.csv", index=False)

fig, ax = plt.subplots(figsize=(7, 4.2))
x = range(len(df)); lab = [f"{p:g} s" for p in df.period]
ax.bar(x, df.with_nodes, color="#4fb3a9", label="answered with nodes")
ax.bar(x, df.none, bottom=df.with_nodes, color="#c0504d", label="answered 'none nearby'")
ax.set_xticks(list(x)); ax.set_xticklabels(lab)
ax.set_xlabel("base station report period"); ax.set_ylabel("congestion alerts per run (mean of runs)")
ax.set_title("Effect of the report period on base station answers"); ax.legend(); ax.grid(axis="y", alpha=.3)
fig.tight_layout(); fig.savefig("period_vs_outcomes.png", dpi=200)
print("wrote period_vs_outcomes.png")
