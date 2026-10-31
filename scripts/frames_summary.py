#!/usr/bin/env python3
"""M13 §1.4 — résumé de ak-fsr-frames.csv (écrit avec [Debug] FrameLog=on).

Découpe le journal en passages (frames consécutives où AK-FSR tourne, coupées par une frame de plus de
--gap ms, typiquement un chargement) et donne pour chacun : fps moyen / 1 % bas / pire frame, ms moyennes et
p95 par passe GPU, attentes CPU (moyenne, max, frames au-dessus de 0,1 ms), créations et événements, puis les
pires frames avec leurs colonnes pour relier les chutes du minimum à une cause.

    python3 scripts/frames_summary.py ak-fsr-frames.csv
    python3 scripts/frames_summary.py ak-fsr-frames.csv --from 120 --to 250   # bornes en secondes
    python3 scripts/frames_summary.py ak-fsr-frames.csv --all                 # référence sans mod (Mode=0)
"""

import argparse
import csv
import math
import sys

GPU = ["gpu_total_ms", "gpu_pre_ms", "gpu_copy_in_ms", "gpu_gap_in_ms", "gpu_mv_ms", "gpu_fsr_ms", "gpu_vk_end_ms",
       "gpu_gap_out_ms", "gpu_copy_out_ms", "gpu_post_ms"]
WAITS = ["wait_flush_ms", "wait_lock_ms", "lock_held_ms", "wait_fence_ms", "wait_idle_ms", "wait_driver_ms"]


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return math.nan


def values(rows, col):
    return [x for x in (num(r[col]) for r in rows) if not math.isnan(x)]


def mean(xs):
    return sum(xs) / len(xs) if xs else math.nan


def pct(xs, p):
    if not xs:
        return math.nan
    s = sorted(xs)
    return s[min(len(s) - 1, int(math.ceil(p / 100.0 * len(s))) - 1)]


def segments(rows, gap_ms, min_frames, all_frames):
    out, cur = [], []
    for r in rows:
        ran = all_frames or r["insertion"] in ("S", "B")
        if not ran or num(r["cpu_frame_ms"]) > gap_ms:
            if len(cur) >= min_frames:
                out.append(cur)
            cur = []  # la frame de coupure (chargement) n'appartient à aucun passage
        else:
            cur.append(r)
    if len(cur) >= min_frames:
        out.append(cur)
    return out


def report(name, rows, worst):
    ft = values(rows, "cpu_frame_ms")
    t0, t1 = num(rows[0]["time_s"]), num(rows[-1]["time_s"])
    print(f"\n=== {name} : frames {rows[0]['frame']}-{rows[-1]['frame']}, {t0:.1f}-{t1:.1f} s, {len(rows)} frames ===")
    if ft:
        low = mean(sorted(ft)[-max(1, len(ft) // 100):])  # 1 % des frames les plus longues
        print(f"fps moyen {1000 / mean(ft):.1f} ({mean(ft):.2f} ms) | 1 % bas {1000 / low:.1f} fps ({low:.2f} ms) | "
              f"pire frame {max(ft):.2f} ms ({1000 / max(ft):.1f} fps) | min {min(ft):.2f} ms")
    ins = {k: sum(1 for r in rows if r["insertion"] == k) for k in "SB"}
    print(f"insertion : SMAA {ins['S']} frames, 0xB2A276FD {ins['B']} frames")
    print(f"CPU AK-FSR : moy {mean(values(rows, 'cpu_akfsr_ms')):.3f} ms, p95 {pct(values(rows, 'cpu_akfsr_ms'), 95):.3f}")
    print("GPU (ms)           moy     p95     max   (n)")
    for c in GPU:
        v = values(rows, c)
        print(f"  {c:<16} {mean(v):7.3f} {pct(v, 95):7.3f} {max(v) if v else math.nan:7.3f}  ({len(v)})")
    print("CPU attentes (ms)  moy     max   frames > 0,1 ms")
    for c in WAITS:
        v = values(rows, c)
        print(f"  {c:<16} {mean(v):7.3f} {max(v) if v else math.nan:7.3f}  {sum(1 for x in v if x > 0.1)}")
    created = [(r["frame"], r["time_s"], r["creations"]) for r in rows if r["creations"]]
    events = [(r["frame"], r["time_s"], r["events"]) for r in rows if r["events"]]
    print(f"créations : {len(created)} frames")
    for f, t, c in created[:20]:
        print(f"  frame {f} ({t} s) : {c}")
    print(f"événements : {len(events)}")
    for f, t, e in events[:20]:
        print(f"  frame {f} ({t} s) : {e}")
    print(f"pires frames ({worst}) :")
    cols = ["cpu_frame_ms", "gpu_total_ms", "gpu_fsr_ms", "gpu_gap_in_ms", "gpu_gap_out_ms", "wait_flush_ms",
            "wait_fence_ms", "wait_driver_ms"]
    print("  frame     t(s)  " + "  ".join(c.replace("_ms", "") for c in cols) + "  créations / événements")
    for r in sorted(rows, key=lambda r: -num(r["cpu_frame_ms"]))[:worst]:
        vals = "  ".join(f"{num(r[c]):{len(c) - 3}.2f}" for c in cols)
        print(f"  {r['frame']:>6} {num(r['time_s']):7.2f}  {vals}  {r['creations']} {r['events']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--from", dest="t_from", type=float, help="début (s) : un seul passage entre --from et --to")
    ap.add_argument("--to", dest="t_to", type=float, help="fin (s)")
    ap.add_argument("--gap", type=float, default=250.0, help="frame (ms) qui coupe un passage (défaut 250)")
    ap.add_argument("--min-frames", type=int, default=300, help="passage minimal (défaut 300 frames)")
    ap.add_argument("--worst", type=int, default=10)
    ap.add_argument("--all", action="store_true", help="passages sans AK-FSR aussi (référence « sans mod », Mode=0)")
    a = ap.parse_args()
    with open(a.csv, newline="", encoding="utf-8", errors="replace") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        sys.exit("CSV vide")
    if a.t_from is not None or a.t_to is not None:
        lo = a.t_from if a.t_from is not None else -math.inf
        hi = a.t_to if a.t_to is not None else math.inf
        sel = [r for r in rows if lo <= num(r["time_s"]) <= hi]
        if not sel:
            sys.exit("aucune frame dans l'intervalle")
        report("intervalle", sel, a.worst)
        return
    segs = segments(rows, a.gap, a.min_frames, a.all)
    if not segs:
        sys.exit("aucun passage d'au moins --min-frames frames avec AK-FSR actif")
    for i, s in enumerate(segs, 1):
        report(f"passage {i}", s, a.worst)


if __name__ == "__main__":
    main()
