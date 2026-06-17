"""
metis_to_csr.py

Convert a METIS adjacency-list file to two Parquet files holding the
CSR (Compressed Sparse Row) representation of an undirected graph:

    indices.parquet  –  one column "indices"  (uint64, 0-based neighbour IDs)
    indptr.parquet   –  one column "indptr"   (uint64, n+1 row-start offsets)

Both arrays are uint64 so they can be passed directly to icebug's fromCSR
without any Arrow cast:

    import pyarrow.parquet as pq
    indices = pq.read_table("indices.parquet")["indices"].combine_chunks()
    indptr  = pq.read_table("indptr.parquet")["indptr"].combine_chunks()
    g = Graph.fromCSR(n, directed=False, out_indices=indices, out_indptr=indptr)

Usage
-----
    python metis_to_csr_parquet.py <input.metis> <indices.parquet> <indptr.parquet>
"""

import sys
import time
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import pyarrow.feather as pf


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------


def parse_metis(path: str) -> tuple[np.ndarray, np.ndarray]:
    """
    Parse a METIS graph file and return (indices, indptr) as uint64 numpy arrays.

    indices  – concatenated neighbour lists (0-based)
    indptr   – length n+1; indptr[u] is the start of vertex u's neighbours
    """
    with open(path, "r", buffering=1 << 20) as f:
        lines = f.readlines()

    # Strip comments
    lines = [ln for ln in lines if not ln.lstrip().startswith("%")]

    if not lines:
        raise ValueError("METIS file is empty or contains only comments.")

    # Parse header
    header_parts = lines[0].split()
    if len(header_parts) < 2:
        raise ValueError(f"Malformed METIS header: {lines[0]!r}")
    n = int(header_parts[0])
    m = int(header_parts[1])
    # fmt / ncon (header_parts[2:]) are not needed for unweighted undirected graphs

    adj_lines = lines[1:]

    # Pre-allocate output arrays.
    # For an undirected METIS file each undirected edge appears twice,
    # so the total number of directed neighbours is 2*m.
    indices = np.empty(2 * m, dtype=np.uint64)
    indptr = np.empty(n + 1, dtype=np.uint64)

    pos: int = 0
    indptr[0] = 0

    for u in range(n):
        if u < len(adj_lines):
            raw = adj_lines[u].strip()
            neighbors = np.fromstring(raw, dtype=np.uint64, sep=" ") - 1
            count = len(neighbors)
            indices[pos : pos + count] = neighbors
            pos += count

        indptr[u + 1] = pos

    return indices, indptr


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> None:
    if len(sys.argv) != 4:
        print(__doc__)
        print(
            "Usage: python metis_to_csr.py "
            "<input.metis> <indices.parquet> <indptr.parquet>"
            # "<input.metis> <indices.feather> <indptr.feather>"
        )
        sys.exit(1)

    metis_path, indices_path, indptr_path = sys.argv[1], sys.argv[2], sys.argv[3]

    print(f"Reading {metis_path} ...")
    indices, indptr = parse_metis(metis_path)

    # n = len(indptr) - 1
    # undirected_edges = int(indptr[n]) // 2
    # print(f"  Vertices  : {n:,}")
    # print(f"  Edges     : {undirected_edges:,}")

    pa_indices = pa.array(indices, type=pa.uint64())
    pa_indptr = pa.array(indptr, type=pa.uint64())

    print(f"Writing {indices_path} ...")
    pq.write_table(
        pa.table({"indices": pa_indices}),
        indices_path,
        compression="snappy",
    )
    print(f"Writing {indptr_path} ...")
    pq.write_table(
        pa.table({"indptr": pa_indptr}),
        indptr_path,
        compression="snappy",
    )

    # print(f"Writing {indices_path} ...")
    # pf.write_feather(
    #     pa.table({"indices": pa_indices}),
    #     indices_path,
    #     compression="uncompressed",
    # )
    # print(f"Writing {indptr_path} ...")
    # pf.write_feather(
    #     pa.table({"indptr": pa_indptr}),
    #     indptr_path,
    #     compression="uncompressed",
    # )


if __name__ == "__main__":
    main()
