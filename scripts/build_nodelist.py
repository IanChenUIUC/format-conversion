import argparse
import duckdb


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Extract distinct node IDs from an edge list."
    )
    parser.add_argument("input", help="Input edge-list file")
    parser.add_argument("output", help="Output file with one node ID per line")
    parser.add_argument("--sep", default=",", help="Field separator, e.g. ',' or '\\t'")
    parser.add_argument("--header", action=argparse.BooleanOptionalAction, default=True)
    args = parser.parse_args()

    con = duckdb.connect()
    con.execute("SET preserve_insertion_order = false;")

    read_edges = f"read_csv({args.input!r}, delim={args.sep!r}, header={str(args.header).lower()}, names=[source, target])"

    query = f"""
    COPY (
        WITH edges AS (
            SELECT *
            FROM {read_edges}
        )
        SELECT DISTINCT node_id
        FROM (
            SELECT source AS node_id FROM edges
            UNION ALL
            SELECT target AS node_id FROM edges
        )
    )
    TO {args.output!r}
    (FORMAT CSV, HEADER true);
    """

    con.execute(query)
    con.close()


if __name__ == "__main__":
    main()
