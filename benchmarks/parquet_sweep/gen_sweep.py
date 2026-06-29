#!/usr/bin/env python3
"""Generate the Parquet-sweep config (sweep.csv) consumed by bench_parquet.

The matrix is the cross product of representation x encoding x compression x sorted.
One easy, provably-correct prune is applied: a sorted=on row is dropped when it is
guaranteed to have the same size as its sorted=off twin, namely

    encoding == "plain" and compression == "none"

(raw fixed-width values; byte count is independent of order). The R analysis can
regenerate those dropped rows by copying the sorted=off `bytes` for the matching
(graph, repr, encoding, compression). No other combination is pruned: dictionary
and any compressed encoding can change size with order, so they are measured both
ways.

Edit the lists below (or pass --compression / --encoding) to change the matrix.
"""

import argparse
import csv
import sys

REPRS = ["csr", "edgelist"]
ENCODINGS = ["dictionary", "plain", "delta"]
COMPRESSIONS = ["none", "snappy", "zstd:3", "zstd:19", "gzip"]
SORTED = [False, True]


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "-o",
        "--out",
        default="sweep.csv",
        help="output config path (default: sweep.csv)",
    )
    ap.add_argument("--reprs", nargs="+", default=REPRS)
    ap.add_argument("--encodings", nargs="+", default=ENCODINGS)
    ap.add_argument("--compressions", nargs="+", default=COMPRESSIONS)
    args = ap.parse_args()

    rows = []
    for repr_ in args.reprs:
        for enc in args.encodings:
            for comp in args.compressions:
                for s in SORTED:
                    rows.append(
                        {
                            "repr": repr_,
                            "encoding": enc,
                            "compression": comp,
                            "sorted": 1 if s else 0,
                        }
                    )

    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["repr", "encoding", "compression", "sorted"])
        w.writeheader()
        w.writerows(rows)

    print(f"wrote {len(rows)} configs to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
