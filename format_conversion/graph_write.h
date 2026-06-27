#pragma once

#include "graph_io.h"

#include <bit>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <sys/mman.h>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

// ─── numDigits ───────────────────────────────────────────────────────────────
//
// Branchless decimal digit count for n >= 1.
// 1233/4096 ≈ log10(2); correction term handles all boundary values exactly.
// Compiles to: lzcnt, imul, shr, cmp, add.

inline uint32_t numDigits(uint32_t n)
{
    static constexpr uint32_t pow10[] = {1u,      10u,      100u,      1000u,      10000u,
                                         100000u, 1000000u, 10000000u, 100000000u, 1000000000u};
    uint32_t t = (std::bit_width(n) * 1233u) >> 12;
    return t + (n >= pow10[t]);
}

// ─── writeGraphToMetis ───────────────────────────────────────────────────────
//
// Two-pass write: first compute per-vertex line byte lengths (to know the
// total file size for pre-allocation), then fill the mmap'd output in a
// second pass. Single-threaded when opts.num_threads == 1 (the default);
// parallel when num_threads > 1 (both passes use the same thread count).

template <class K, class O>
void writeGraphToMetis(const DiGraphCsr<K, O> &g, const std::string &output_path, const ParseOptions &opts = {})
{
    size_t n = g.span(), m = g.size() / 2;
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%zu %zu\n", n, m);

    // ── Pass 1: compute per-vertex line byte lengths ──────────────────────────
    std::vector<size_t> line_bytes(n);

    auto computeLineSizes = [&](size_t u_begin, size_t u_end) {
        for (size_t u = u_begin; u < u_end; ++u)
        {
            size_t bytes = 1; // newline
            bool first = true;
            g.forEachEdgeKey((K)u, [&](K v) {
                if (!first)
                    ++bytes; // space separator
                bytes += numDigits((uint32_t)(v + 1));
                first = false;
            });
            line_bytes[u] = bytes;
        }
    };

    if (opts.num_threads <= 1)
    {
        computeLineSizes(0, n);
    }
    else
    {
#pragma omp parallel for num_threads(opts.num_threads) schedule(dynamic, 2048)
        for (size_t u = 0; u < n; ++u)
        {
            size_t bytes = 1;
            bool first = true;
            g.forEachEdgeKey((K)u, [&](K v) {
                if (!first)
                    ++bytes;
                bytes += numDigits((uint32_t)(v + 1));
                first = false;
            });
            line_bytes[u] = bytes;
        }
    }

    // ── Prefix sum → per-vertex byte offsets ─────────────────────────────────
    std::vector<size_t> line_off(n + 1);
    line_off[0] = (size_t)hlen;
    for (size_t i = 0; i < n; ++i)
        line_off[i + 1] = line_off[i] + line_bytes[i];
    std::vector<size_t>().swap(line_bytes);

    size_t total = line_off[n];
    std::string out_path = output_path + ".metis";
    int fd = open(out_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        throw std::runtime_error("Cannot create: " + out_path);
    if (posix_fallocate(fd, 0, (off_t)total) != 0)
    {
        close(fd);
        throw std::runtime_error("posix_fallocate failed: " + out_path);
    }
    char *buf = (char *)mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED)
    {
        close(fd);
        throw std::runtime_error("mmap failed: " + out_path);
    }

    memcpy(buf, header, hlen);

    // ── Pass 2: write adjacency lines ─────────────────────────────────────────
    auto writeLines = [&](size_t u_begin, size_t u_end) {
        for (size_t u = u_begin; u < u_end; ++u)
        {
            char *p = buf + line_off[u];
            bool first = true;
            g.forEachEdgeKey((K)u, [&](K v) {
                if (!first)
                    *p++ = ' ';
                auto [ptr, _] = std::to_chars(p, p + 11, (uint32_t)(v + 1));
                p = ptr;
                first = false;
            });
            *p = '\n';
        }
    };

    if (opts.num_threads <= 1)
    {
        writeLines(0, n);
    }
    else
    {
#pragma omp parallel for num_threads(opts.num_threads) schedule(dynamic, 2048)
        for (size_t u = 0; u < n; ++u)
        {
            char *p = buf + line_off[u];
            bool first = true;
            g.forEachEdgeKey((K)u, [&](K v) {
                if (!first)
                    *p++ = ' ';
                auto [ptr, _] = std::to_chars(p, p + 11, (uint32_t)(v + 1));
                p = ptr;
                first = false;
            });
            *p = '\n';
        }
    }

    msync(buf, total, MS_SYNC);
    munmap(buf, total);
    close(fd);
}

// ─── writeGraphToParquet ─────────────────────────────────────────────────────
//
// Writes two single-column Parquet files:
//   {output_path}.indices.parquet  — neighbor IDs, width = K (uint32 by default)
//   {output_path}.indptr.parquet   — CSR offset array (uint64)
//
// Both columns are wrapped ZERO-COPY straight from the in-memory CSR vectors —
// no cast, no temporary. (The old writer cast K → uint64 into a separate vector,
// which at scale meant a 150 GB allocation + copy and doubled the bytes on disk.
// Indices are now written at their native K width; a downstream stage can widen
// to uint64 if some consumer requires it.)
//
// These Parquet files are the long-term, on-disk representation, so they are
// tuned for SIZE: delta encoding + zstd. Dictionary encoding stays off (indices
// are high-cardinality — a dictionary is pure overhead at large N). Delta encoding
// (DELTA_BINARY_PACKED) shrinks both columns: monotonic indptr collapses to its
// gaps (degrees), and indices benefit further when adjacency lists are sorted
// (see ParseOptions::sort_neighbors). Statistics are kept (cheap, and enable page
// skipping for range queries). For a low-latency / zero-copy consumer, convert
// these to Feather/Arrow IPC instead — that is the throughput-oriented format.

template <class K, class O>
void writeGraphToParquet(const DiGraphCsr<K, O> &g, const std::string &output_path, const ParseOptions & = {})
{
    static_assert(sizeof(O) == 8, "offsets (O) must be uint64_t");
    static_assert(sizeof(K) == 4 || sizeof(K) == 8, "indices (K) must be 32- or 64-bit unsigned");

    parquet::WriterProperties::Builder pb;
    pb.disable_dictionary();
    pb.encoding(parquet::Encoding::DELTA_BINARY_PACKED);
    pb.compression(arrow::Compression::ZSTD);
    auto props = pb.build();
    auto arrow_props = parquet::default_arrow_writer_properties();

    // Large row groups keep per-group overhead negligible; Parquet still streams
    // pages within a group, so this does not buffer the whole column.
    constexpr int64_t kRowGroup = int64_t{1} << 24; // 16Mi rows

    // Wrap a contiguous vector as a single-column Parquet table and stream it out.
    auto writeColumn = [&](const std::string &path, const std::string &col_name, std::shared_ptr<arrow::Array> arr) {
        auto table = arrow::Table::Make(arrow::schema({arrow::field(col_name, arr->type())}), {arr});
        auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
        PARQUET_THROW_NOT_OK(
            parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, kRowGroup, props, arrow_props));
        PARQUET_THROW_NOT_OK(out->Close());
    };

    // Zero-copy: build an Arrow array that references the vector's memory directly.
    // The Buffer is non-owning, so the source vector must outlive the write (it does).
    auto wrapZeroCopy = [](const auto *vec_data, int64_t len, std::shared_ptr<arrow::DataType> type) {
        auto buf = arrow::Buffer::Wrap(vec_data, static_cast<size_t>(len));
        auto ad = arrow::ArrayData::Make(std::move(type), len, {nullptr, buf});
        return arrow::MakeArray(ad);
    };

    auto idx_type = sizeof(K) == 4 ? arrow::uint32() : arrow::uint64();
    writeColumn(output_path + ".indices.parquet", "indices",
                wrapZeroCopy(g.edgeKeys.data(), static_cast<int64_t>(g.edgeKeys.size()), idx_type));
    writeColumn(output_path + ".indptr.parquet", "indptr",
                wrapZeroCopy(g.offsets.data(), static_cast<int64_t>(g.offsets.size()), arrow::uint64()));
}

// ─── writeGraphToCSV ─────────────────────────────────────────────────────────
//
// Writes a headerless CSV edge list: one "u{sep}v" line per undirected edge,
// u < v, using compact 0-indexed IDs. opts.sep controls the delimiter.

template <class K, class O>
void writeGraphToCSV(const DiGraphCsr<K, O> &g, const std::string &output_path, const ParseOptions &opts = {})
{
    std::string out = output_path + ".csv";
    std::ofstream f(out);
    if (!f)
        throw std::runtime_error("Cannot create: " + out);
    size_t n = g.span();
    for (K u = 0; u < static_cast<K>(n); ++u)
        g.forEachEdgeKey(u, [&](K v) {
            if (v > u)
                f << u << opts.sep << v << '\n';
        });
}

// ─── writeGraph ──────────────────────────────────────────────────────────────

template <class K, class O>
void writeGraph(const DiGraphCsr<K, O> &g, const std::string &output_path, EdgesFormat fmt,
                const ParseOptions &opts = {})
{
    switch (fmt)
    {
    case METIS:
        writeGraphToMetis(g, output_path, opts);
        return;
    case CSR_PARQUET:
        writeGraphToParquet(g, output_path, opts);
        return;
    case CSV_EDGELIST:
        writeGraphToCSV(g, output_path, opts);
        return;
    default:
        throw std::runtime_error("writeGraph: unknown format");
    }
}
