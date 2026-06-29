#pragma once

#include "formats.h"

#include <Graph.hxx>

#include <bit>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

// Branchless decimal digit count for n >= 0 (used to size output buffers).

inline uint32_t numDigits(uint32_t n)
{
    static constexpr uint32_t pow10[] = {1u,      10u,      100u,      1000u,      10000u,
                                         100000u, 1000000u, 10000000u, 100000000u, 1000000000u};
    uint32_t t = (std::bit_width(n) * 1233u) >> 12;
    return (n == 0) ? 1u : t + (n >= pow10[t]);
}

// Two-pass, byte-exact, parallel writer of a text file. For each u in [0, n),
// lineBytes(u) returns the number of bytes u contributes and writeLine(u, p)
// writes exactly that many bytes starting at p. An optional header is written
// first. Shared by the METIS and CSV writers.
template <class LineBytes, class WriteLine>
void writeLinesMmap(const std::string &path, size_t n, std::string_view header, LineBytes lineBytes,
                    WriteLine writeLine, size_t num_threads)
{
    const int T = num_threads > 1 ? static_cast<int>(num_threads) : 1;

    std::vector<size_t> off(n + 1);
    off[0] = header.size();
    {
        std::vector<size_t> bytes(n);
#pragma omp parallel for num_threads(T) schedule(dynamic, 2048)
        for (size_t u = 0; u < n; ++u)
            bytes[u] = lineBytes(u);
        for (size_t u = 0; u < n; ++u)
            off[u + 1] = off[u] + bytes[u];
    }
    const size_t total = off[n];

    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        throw std::runtime_error("Cannot create: " + path);
    if (total == 0)
    {
        close(fd);
        return;
    }
    if (posix_fallocate(fd, 0, static_cast<off_t>(total)) != 0)
    {
        close(fd);
        throw std::runtime_error("posix_fallocate failed: " + path);
    }
    char *buf = static_cast<char *>(mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (buf == MAP_FAILED)
    {
        close(fd);
        throw std::runtime_error("mmap failed: " + path);
    }
    if (!header.empty())
        memcpy(buf, header.data(), header.size());

#pragma omp parallel for num_threads(T) schedule(dynamic, 2048)
    for (size_t u = 0; u < n; ++u)
        writeLine(u, buf + off[u]);

    msync(buf, total, MS_SYNC);
    munmap(buf, total);
    close(fd);
}

// Write the CSR as a METIS adjacency-list file. Parallelised over num_threads.

template <class K, class O>
void writeGraphToMetis(const DiGraphCsr<K, O> &g, const std::string &output_path, const ParseOptions &opts = {})
{
    const size_t n = g.span(), m = g.size() / 2;
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%zu %zu\n", n, m);

    auto lineBytes = [&](size_t u) {
        size_t bytes = 1; // trailing newline
        bool first = true;
        g.forEachEdgeKey((K)u, [&](K v) {
            if (!first)
                ++bytes; // space separator
            bytes += numDigits((uint32_t)(v + 1));
            first = false;
        });
        return bytes;
    };
    auto writeLine = [&](size_t u, char *p) {
        bool first = true;
        g.forEachEdgeKey((K)u, [&](K v) {
            if (!first)
                *p++ = ' ';
            auto [ptr, _] = std::to_chars(p, p + 11, (uint32_t)(v + 1)); // 1-indexed
            p = ptr;
            first = false;
        });
        *p = '\n';
    };

    writeLinesMmap(output_path + ".metis", n, std::string_view(header, hlen), lineBytes, writeLine, opts.num_threads);
}

// Write the CSR as two single-column Parquet files: {path}.indices.parquet (the
// neighbor ids, native K width) and {path}.indptr.parquet (the offsets, uint64).
// Tuned for on-disk size (delta encoding + zstd); see DESIGN.md for the encoding
// rationale and the note on converting to Feather for zero-copy consumers.

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

    // Large row groups; Parquet still streams pages within a group.
    constexpr int64_t kRowGroup = int64_t{1} << 24; // 16Mi rows

    // Wrap a contiguous vector as a single-column Parquet table and stream it out.
    auto writeColumn = [&](const std::string &path, const std::string &col_name, std::shared_ptr<arrow::Array> arr) {
        auto table = arrow::Table::Make(arrow::schema({arrow::field(col_name, arr->type())}), {arr});
        auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
        PARQUET_THROW_NOT_OK(
            parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, kRowGroup, props, arrow_props));
        PARQUET_THROW_NOT_OK(out->Close());
    };

    // Zero-copy: the Arrow array references the vector's memory (which outlives the write).
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

// Write a headerless CSV edge list, one "u{sep}v" line per undirected edge (u<v).
// Parallelised over num_threads.

template <class K, class O>
void writeGraphToCSV(const DiGraphCsr<K, O> &g, const std::string &output_path, const ParseOptions &opts = {})
{
    const size_t n = g.span();
    const char sep = opts.sep;

    auto lineBytes = [&](size_t u) {
        size_t bytes = 0;
        g.forEachEdgeKey((K)u, [&](K v) {
            if (v > (K)u)
                bytes += numDigits((uint32_t)u) + 1 + numDigits((uint32_t)v) + 1;
        });
        return bytes;
    };
    auto writeLine = [&](size_t u, char *p) {
        g.forEachEdgeKey((K)u, [&](K v) {
            if (v > (K)u)
            {
                p = std::to_chars(p, p + 11, (uint32_t)u).ptr;
                *p++ = sep;
                p = std::to_chars(p, p + 11, (uint32_t)v).ptr;
                *p++ = '\n';
            }
        });
    };

    writeLinesMmap(output_path + ".csv", n, {}, lineBytes, writeLine, opts.num_threads);
}

// Dispatch a CSR to the writer for the requested output format.

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
