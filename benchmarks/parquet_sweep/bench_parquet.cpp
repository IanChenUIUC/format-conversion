// Parquet encoding/compression size sweep (sizes only).
//
// For each graph in the manifest, builds the CSR once, then writes the chosen
// representation under each sweep config to an in-memory Arrow BufferOutputStream
// and records the resulting byte count. Nothing touches the disk except the final
// results CSV. No timing, no read-back.
//
//   repr        : csr (indices column only; indptr / "nodelist" ignored)
//                 edgelist (src,dst columns, one row per undirected edge u<v)
//   encoding    : dictionary | plain | delta
//   compression : none | snappy | zstd:N | gzip
//   sorted      : 0 | 1  (adjacency / edge order sorted before writing)
//
// Output row: graph,n,m,repr,encoding,compression,sorted,bytes

#include "graph_read.h"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using K = uint32_t;
using O = uint64_t;

namespace
{

struct Config
{
    std::string repr;        // csr | edgelist
    std::string encoding;    // dictionary | plain | delta
    std::string compression; // none | snappy | zstd:N | gzip
    bool sorted = false;
};

struct ManifestRow
{
    std::string name;
    std::string path;
    std::string format; // csv | metis | csr_parquet
    std::string nodes;  // optional node list (empty -> dense)
    size_t skip_rows = 0;
};

std::vector<std::string> splitCSV(const std::string &line)
{
    std::vector<std::string> out;
    std::string cell;
    std::stringstream ss(line);
    while (std::getline(ss, cell, ','))
    {
        // trim surrounding whitespace
        size_t a = cell.find_first_not_of(" \t\r\n");
        size_t b = cell.find_last_not_of(" \t\r\n");
        out.push_back(a == std::string::npos ? "" : cell.substr(a, b - a + 1));
    }
    return out;
}

// Read a CSV with a header row; return rows as field->value maps keyed by header.
std::vector<std::vector<std::string>> readCSV(const std::string &path, std::vector<std::string> &header)
{
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("cannot open: " + path);
    std::vector<std::vector<std::string>> rows;
    std::string line;
    bool first = true;
    while (std::getline(f, line))
    {
        if (line.empty())
            continue;
        if (first)
        {
            header = splitCSV(line);
            first = false;
            continue;
        }
        rows.push_back(splitCSV(line));
    }
    return rows;
}

int colIndex(const std::vector<std::string> &header, const std::string &name)
{
    for (size_t i = 0; i < header.size(); ++i)
        if (header[i] == name)
            return static_cast<int>(i);
    return -1;
}

std::string get(const std::vector<std::string> &row, const std::vector<std::string> &header, const std::string &name,
                const std::string &dflt = "")
{
    int i = colIndex(header, name);
    if (i < 0 || i >= static_cast<int>(row.size()) || row[i].empty())
        return dflt;
    return row[i];
}

EdgesFormat parseFormat(const std::string &s)
{
    if (s == "csv" || s == "csv_edgelist")
        return CSV_EDGELIST;
    if (s == "metis")
        return METIS;
    if (s == "csr_parquet" || s == "parquet")
        return CSR_PARQUET;
    throw std::runtime_error("unknown format: " + s);
}

// Zero-copy wrap of a uint32 buffer as an Arrow array (the source must outlive use).
std::shared_ptr<arrow::Array> wrapU32(const uint32_t *data, int64_t len)
{
    auto buf = arrow::Buffer::Wrap(data, static_cast<size_t>(len));
    return arrow::MakeArray(arrow::ArrayData::Make(arrow::uint32(), len, {nullptr, buf}));
}

arrow::Compression::type parseCompression(const std::string &c, int &level)
{
    level = std::numeric_limits<int>::min(); // sentinel: use codec default
    if (c == "none")
        return arrow::Compression::UNCOMPRESSED;
    if (c == "snappy")
        return arrow::Compression::SNAPPY;
    if (c == "gzip")
        return arrow::Compression::GZIP;
    if (c.rfind("zstd", 0) == 0)
    {
        auto colon = c.find(':');
        if (colon != std::string::npos)
            level = std::stoi(c.substr(colon + 1));
        return arrow::Compression::ZSTD;
    }
    throw std::runtime_error("unknown compression: " + c);
}

// Encode `table` to an in-memory Parquet buffer under `cfg` and return its size.
size_t parquetSize(const std::shared_ptr<arrow::Table> &table, const Config &cfg)
{
    parquet::WriterProperties::Builder pb;
    pb.disable_statistics(); // measure encoded data only, not per-page metadata

    if (cfg.encoding == "dictionary")
    {
        pb.enable_dictionary();
    }
    else
    {
        pb.disable_dictionary();
        pb.encoding(cfg.encoding == "delta" ? parquet::Encoding::DELTA_BINARY_PACKED : parquet::Encoding::PLAIN);
    }

    int level = 0;
    pb.compression(parseCompression(cfg.compression, level));
    if (level != std::numeric_limits<int>::min())
        pb.compression_level(level);

    auto props = pb.build();
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    // One large row group keeps per-group overhead negligible without buffering
    // more than the column itself.
    auto st = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), sink, int64_t{1} << 26, props);
    if (!st.ok())
        throw std::runtime_error("WriteTable: " + st.ToString());
    auto buf = sink->Finish().ValueOrDie();
    return static_cast<size_t>(buf->size());
}

// Build the Arrow table for a representation from the current CSR state.
std::shared_ptr<arrow::Table> csrTable(const DiGraphCsr<K, O> &g)
{
    auto arr = wrapU32(g.edgeKeys.data(), static_cast<int64_t>(g.edgeKeys.size()));
    auto schema = arrow::schema({arrow::field("indices", arrow::uint32())});
    return arrow::Table::Make(schema, {arr});
}

std::shared_ptr<arrow::Table> edgelistTable(const DiGraphCsr<K, O> &g, std::vector<K> &src, std::vector<K> &dst)
{
    const size_t n = g.span();
    src.clear();
    dst.clear();
    src.reserve(g.size() / 2);
    dst.reserve(g.size() / 2);
    for (size_t u = 0; u < n; ++u)
        g.forEachEdgeKey(static_cast<K>(u), [&](K v) {
            if (v > static_cast<K>(u))
            {
                src.push_back(static_cast<K>(u));
                dst.push_back(v);
            }
        });
    auto sa = wrapU32(src.data(), static_cast<int64_t>(src.size()));
    auto da = wrapU32(dst.data(), static_cast<int64_t>(dst.size()));
    auto schema = arrow::schema({arrow::field("src", arrow::uint32()), arrow::field("dst", arrow::uint32())});
    return arrow::Table::Make(schema, {sa, da});
}

} // namespace

int main(int argc, char **argv)
{
    std::string manifest_path, sweep_path, results_path;
    int build_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (build_threads < 1)
        build_threads = 1;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--manifest")
            manifest_path = next();
        else if (a == "--sweep")
            sweep_path = next();
        else if (a == "--results")
            results_path = next();
        else if (a == "--build-threads")
            build_threads = std::stoi(next());
        else
        {
            std::cerr << "unknown arg: " << a << "\n";
            return 2;
        }
    }
    if (manifest_path.empty() || sweep_path.empty() || results_path.empty())
    {
        std::cerr << "usage: bench_parquet --manifest graphs.csv --sweep sweep.csv "
                     "--results results.csv [--build-threads N]\n";
        return 2;
    }

    // Load sweep configs.
    std::vector<std::string> sh;
    auto srows = readCSV(sweep_path, sh);
    std::vector<Config> configs;
    for (auto &r : srows)
    {
        Config c;
        c.repr = get(r, sh, "repr");
        c.encoding = get(r, sh, "encoding");
        c.compression = get(r, sh, "compression");
        c.sorted = get(r, sh, "sorted", "0") == "1";
        configs.push_back(c);
    }

    // Load manifest.
    std::vector<std::string> mh;
    auto mrows = readCSV(manifest_path, mh);

    std::ofstream out(results_path);
    if (!out)
        throw std::runtime_error("cannot write: " + results_path);
    out << "graph,n,m,repr,encoding,compression,sorted,bytes\n";

    for (auto &row : mrows)
    {
        ManifestRow mr;
        mr.name = get(row, mh, "name");
        mr.path = get(row, mh, "path");
        mr.format = get(row, mh, "format");
        mr.nodes = get(row, mh, "nodes");
        mr.skip_rows = std::stoul(get(row, mh, "skip_rows", "0"));

        std::cerr << "[build] " << mr.name << " (" << mr.format << ") ..." << std::flush;

        ParseOptions opts;
        opts.num_threads = static_cast<size_t>(build_threads);
        opts.skip_rows = mr.skip_rows;

        GraphDescriptor gd(mr.path, parseFormat(mr.format), opts);
        std::unique_ptr<NodeDescriptor> nd;
        if (!mr.nodes.empty())
        {
            ParseOptions nopts;
            nopts.skip_rows = mr.skip_rows;
            nd = std::make_unique<NodeDescriptor>(mr.nodes, nopts);
        }

        DiGraphCsr<K, O> g = buildGraph<K, O>(gd, nd.get());
        const size_t n = g.span(), m = g.size() / 2;
        std::cerr << " n=" << n << " m=" << m << "\n";

        std::vector<K> src, dst; // reused edgelist scratch

        // Run every config whose `sorted` flag matches the current CSR state.
        auto runPhase = [&](bool sorted_state) {
            for (const auto &c : configs)
            {
                if (c.sorted != sorted_state)
                    continue;
                std::shared_ptr<arrow::Table> table =
                    (c.repr == "csr") ? csrTable(g) : edgelistTable(g, src, dst);
                size_t bytes = parquetSize(table, c);
                out << mr.name << ',' << n << ',' << m << ',' << c.repr << ',' << c.encoding << ','
                    << c.compression << ',' << (c.sorted ? 1 : 0) << ',' << bytes << '\n';
                out.flush();
                std::cerr << "  " << c.repr << ' ' << c.encoding << ' ' << c.compression
                          << " sorted=" << c.sorted << " -> " << bytes << " bytes\n";
            }
            // free edgelist scratch between phases
            std::vector<K>().swap(src);
            std::vector<K>().swap(dst);
        };

        runPhase(/*sorted_state=*/false);
        sortNeighbors(g, build_threads);
        runPhase(/*sorted_state=*/true);
    }

    std::cerr << "wrote " << results_path << "\n";
    return 0;
}
