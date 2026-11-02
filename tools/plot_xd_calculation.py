#!/usr/bin/env python3
"""Scientific schematic of Unicorn's experimental XD calculation."""

import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle
import numpy as np

OUT = Path(__file__).resolve().parents[1] / "doc"
expected = 3 - 0.8 * math.exp(-0 / 5) - 0.8 * math.exp(-1 / 5)

BLUE = "#326A9C"
ORANGE = "#C0663E"
GRAY = "#646B73"
PALE = "#EEF1F3"
GREEN = "#4D806A"

plt.rcParams.update({
    "font.family": "sans-serif", "font.size": 9,
    "axes.labelsize": 10, "axes.spines.top": False,
    "axes.spines.right": False, "pdf.fonttype": 42,
})
fig = plt.figure(figsize=(8.2, 8.4), layout="constrained")
gs = fig.add_gridspec(3, 2, height_ratios=[0.90, 1.08, 0.55],
                      hspace=0.22, wspace=0.20)
ax_a = fig.add_subplot(gs[0, :])
ax_b = fig.add_subplot(gs[1, 0])
ax_c = fig.add_subplot(gs[1, 1])
ax_d = fig.add_subplot(gs[2, :])

# A. Two aligned rows show reference-to-read substitutions at the same bases.
changes = {1: ("C", "T", BLUE), 10: ("A", "C", GRAY),
           19: ("G", "A", ORANGE)}
for position in range(1, 21):
    for y in (0.64, 0.25):
        ax_a.add_patch(Rectangle((position - 0.43, y), 0.86, 0.25,
                                 facecolor=PALE, edgecolor="white", linewidth=0.7))
    if position in changes:
        reference, observed, color = changes[position]
        ax_a.add_patch(Rectangle((position - 0.43, 0.64), 0.86, 0.25,
                                 facecolor="white", edgecolor=color, linewidth=1.3))
        ax_a.add_patch(Rectangle((position - 0.43, 0.25), 0.86, 0.25,
                                 facecolor=color, edgecolor="white", linewidth=0.7))
        ax_a.text(position, 0.765, reference, ha="center", va="center",
                  color=color, fontweight="bold", fontsize=10)
        ax_a.text(position, 0.375, observed, ha="center", va="center",
                  color="white", fontweight="bold", fontsize=10)
        ax_a.annotate("", xy=(position, 0.52), xytext=(position, 0.62),
                      arrowprops={"arrowstyle": "->", "color": color, "lw": 1.0})
ax_a.text(-2.5, 0.765, "Reference", ha="left", va="center", fontsize=9)
ax_a.text(-2.5, 0.375, "Read", ha="left", va="center", fontsize=9)
for y in (0.765, 0.375):
    ax_a.text(0.35, y, "5′", ha="right", va="center", fontsize=10)
    ax_a.text(20.65, y, "3′", ha="left", va="center", fontsize=10)
for x, label, distance, color in [
    (1, "C→T", "d = 0", BLUE),
    (10, "A→C", "other", GRAY),
    (19, "G→A", "d = 1", ORANGE),
]:
    ax_a.text(x, 1.05, label, ha="center", va="center", color=color,
              fontweight="bold", fontsize=10)
    ax_a.text(x, 0.10, distance, ha="center", va="center", color=color, fontsize=9)
ax_a.set_xlim(-2.7, 21.5)
ax_a.set_ylim(0.0, 1.18)
ax_a.set_xticks([1, 5, 10, 15, 19, 20])
ax_a.set_xlabel("Position along a 20-base read")
ax_a.set_yticks([])
for spine in ax_a.spines.values():
    spine.set_visible(False)
ax_a.text(-0.04, 1.06, "A", transform=ax_a.transAxes,
          fontweight="bold", fontsize=12)

# B/C. Show separate 5′ and 3′ rules at the same positions in the read.
position = np.linspace(1, 20, 191)
penalty_5prime = 1 - 0.8 * np.exp(-(position - 1) / 5)
penalty_3prime = 1 - 0.8 * np.exp(-(20 - position) / 5)
for ax, penalty, color, label, letter in [
    (ax_b, penalty_5prime, BLUE, "5′ C→T", "B"),
    (ax_c, penalty_3prime, ORANGE, "3′ G→A", "C"),
]:
    ax.plot(position, penalty, color=color, linewidth=2.0, label=label)
    ax.axhline(1, color=GRAY, linestyle="--", linewidth=1.3,
               label="Other edit")
    ax.set_xlim(1, 20)
    ax.set_ylim(0, 1.08)
    ax.set_xlabel("Position in read (5′ → 3′)")
    ax.set_xticks([1, 5, 10, 15, 20])
    ax.set_yticks([0, 0.2, 0.4, 0.6, 0.8, 1.0])
    ax.grid(axis="y", color="#E5E7E9", linewidth=0.6)
    ax.set_axisbelow(True)
    ax.text(-0.13, 1.04, letter, transform=ax.transAxes,
            fontweight="bold", fontsize=12)
    ax.legend(frameon=False, loc="lower center", fontsize=8)
ax_b.set_ylabel("Penalty contribution")
ax_c.set_ylabel("Penalty contribution")
ax_b.scatter([1], [0.2], color=BLUE, s=43, zorder=5)
ax_b.annotate("0.200", xy=(1, 0.2), xytext=(3.2, 0.30),
              fontsize=9, color=BLUE,
              arrowprops={"arrowstyle": "-", "color": BLUE, "lw": 0.8})
ax_c.scatter([19], [1 - 0.8 * math.exp(-1 / 5)],
             color=ORANGE, s=43, zorder=5)
ax_c.annotate("0.345", xy=(19, 1 - 0.8 * math.exp(-1 / 5)),
              xytext=(13.0, 0.30), fontsize=9, color=ORANGE,
              arrowprops={"arrowstyle": "-", "color": ORANGE, "lw": 0.8})

# D. The original edit count and adjusted burden for this one read.
ax_d.barh([1, 0], [3.0, expected], height=0.44,
          color=[GRAY, GREEN])
ax_d.set_yticks([1, 0], ["NM", "XD"])
ax_d.set_xlim(0, 3.45)
ax_d.set_ylim(-0.7, 1.7)
ax_d.set_xticks([0, 1, 2, 3])
ax_d.set_xlabel("Total edit burden")
ax_d.grid(axis="x", color="#E5E7E9", linewidth=0.6)
ax_d.set_axisbelow(True)
ax_d.text(3.04, 1, "3.000", va="center", fontsize=9, color=GRAY)
ax_d.text(expected + 0.04, 0, "1.545", va="center", fontsize=9, color=GREEN)
ax_d.text(-0.06, 1.04, "D", transform=ax_d.transAxes,
          fontweight="bold", fontsize=12)

fig.savefig(OUT / "xd_calculation.png", dpi=300)
fig.savefig(OUT / "xd_calculation.pdf")
plt.close(fig)
