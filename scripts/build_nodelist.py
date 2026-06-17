import argparse
import duckdb


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Extract distinct node IDs from an edge list."
    )
    parser.add_argument("input", help="Input edge-list file")
    parser.add_argument("output", help="Output file with one node ID per line")
    parser.add_argument("--sep", default=",", help="Field separator, e.g. ',' or '\\t'")
    parser.add_argument(
        "--header",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Whether the input has a header row",
    )
    parser.add_argument(
        "--source-col",
        default=None,
        help="Source column name (default: source or column0)",
    )
    parser.add_argument(
        "--target-col",
        default=None,
        help="Target column name (default: target or column1)",
    )
    args = parser.parse_args()

    source_col = args.source_col or ("source" if args.header else "column0")
    target_col = args.target_col or ("target" if args.header else "column1")

    con = duckdb.connect()

    query = f"""
    COPY (
        WITH edges AS (
            SELECT *
            FROM read_csv({args.input!r}, delim={args.sep!r}, header=false)
        )
        SELECT DISTINCT id
        FROM (
            SELECT column0 AS id FROM edges
            UNION ALL
            SELECT column1 AS id FROM edges
        )
    )
    TO {args.output!r}
    (FORMAT CSV, HEADER true);
    """

    con.execute(query)
    con.close()


if __name__ == "__main__":
    main()
