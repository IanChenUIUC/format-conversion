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
#include <parquet/exception.h>
#include <parquet/properties.h>

#include <algorithm>

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
    // METIS is an undirected adjacency format; its header edge count assumes a
    // symmetric graph (m = total arcs / 2). A directed graph has no faithful
    // METIS representation, so reject it rather than emit a wrong count.
    if (opts.directed)
        throw std::runtime_error("METIS output is undirected-only; directed=true is not supported for METIS "
                                 "(use CSV_EDGELIST or CSR_PARQUET for directed graphs)");

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

// Large row groups; Parquet still streams pages within a group.
inline constexpr int64_t PARQUET_ROW_GROUP = int64_t{1} << 24; // 16Mi rows

// Rows materialised per chunk by writeParquetIdColumns. Bounds that writer's
// scratch at chunk * ncols * sizeof(Id).
inline constexpr int64_t PARQUET_CHUNK_ROWS = int64_t{1} << 20; // 1Mi rows

// Size-tuned properties shared by every Parquet output here; see DESIGN.md for why
// dictionary is off and delta is on.
inline std::shared_ptr<parquet::WriterProperties> parquetIdColumnProperties()
{
    parquet::WriterProperties::Builder pb;
    pb.disable_dictionary();
    pb.encoding(parquet::Encoding::DELTA_BINARY_PACKED);
    pb.compression(arrow::Compression::ZSTD);
    pb.max_row_group_length(PARQUET_ROW_GROUP);
    return pb.build();
}

// Wrap contiguous memory as an Arrow array without copying. The caller must keep
// the memory alive until the write completes.
inline std::shared_ptr<arrow::Array> wrapZeroCopy(const void *data, int64_t len,
                                                  std::shared_ptr<arrow::DataType> type)
{
    auto buf = arrow::Buffer::Wrap(static_cast<const uint8_t *>(data),
                                   static_cast<size_t>(len) * static_cast<size_t>(type->byte_width()));
    auto ad = arrow::ArrayData::Make(std::move(type), len, {nullptr, buf});
    return arrow::MakeArray(ad);
}

// Write a single column that already exists as contiguous memory, zero-copy.
inline void writeParquetColumn(const std::string &path, const std::string &col_name,
                               std::shared_ptr<arrow::Array> arr)
{
    auto table = arrow::Table::Make(arrow::schema({arrow::field(col_name, arr->type())}), {arr});
    auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    PARQUET_THROW_NOT_OK(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, PARQUET_ROW_GROUP,
                                                    parquetIdColumnProperties(),
                                                    parquet::default_arrow_writer_properties()));
    PARQUET_THROW_NOT_OK(out->Close());
}

// Write id columns that do NOT already exist as contiguous memory, a chunk at a
// time. `fillChunk(row_begin, row_end, bufs)` fills bufs[c][0 .. row_end-row_begin)
// for each column; the buffers are reused across chunks, so peak scratch is
// PARQUET_CHUNK_ROWS * ncols * sizeof(Id) rather than the whole column.
template <class Id, class FillChunk>
void writeParquetIdColumns(const std::string &path, const std::vector<std::string> &names, int64_t total_rows,
                           FillChunk &&fillChunk)
{
    static_assert(sizeof(Id) == 4 || sizeof(Id) == 8, "id columns must be 32- or 64-bit unsigned");
    auto type = sizeof(Id) == 4 ? arrow::uint32() : arrow::uint64();

    arrow::FieldVector fields;
    for (const auto &n : names)
        fields.push_back(arrow::field(n, type));
    auto schema = arrow::schema(fields);

    auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    std::unique_ptr<parquet::arrow::FileWriter> writer;
    PARQUET_ASSIGN_OR_THROW(writer,
                            parquet::arrow::FileWriter::Open(*schema, arrow::default_memory_pool(), out,
                                                             parquetIdColumnProperties(),
                                                             parquet::default_arrow_writer_properties()));

    const int64_t chunk = std::min<int64_t>(PARQUET_CHUNK_ROWS, std::max<int64_t>(total_rows, 1));
    std::vector<std::vector<Id>> bufs(names.size());
    for (auto &b : bufs)
        b.resize(static_cast<size_t>(chunk));

    for (int64_t begin = 0; begin < total_rows; begin += chunk)
    {
        const int64_t end = std::min(begin + chunk, total_rows);
        fillChunk(begin, end, bufs);

        arrow::ArrayVector arrays;
        for (size_t c = 0; c < names.size(); ++c)
            arrays.push_back(wrapZeroCopy(bufs[c].data(), end - begin, type));
        PARQUET_THROW_NOT_OK(writer->WriteRecordBatch(*arrow::RecordBatch::Make(schema, end - begin, arrays)));
    }

    PARQUET_THROW_NOT_OK(writer->Close());
    PARQUET_THROW_NOT_OK(out->Close());
}

// Write the CSR as two single-column Parquet files: {path}.indices.parquet (the
// neighbor ids, native K width) and {path}.indptr.parquet (the offsets, uint64).
// Tuned for on-disk size (delta encoding + zstd); see DESIGN.md for the encoding
// rationale and the note on converting to Feather for zero-copy consumers.

template <class K, class O>
void writeGraphToParquet(const DiGraphCsr<K, O> &g, const std::string &output_path, const ParseOptions &opts = {})
{
    static_assert(sizeof(O) == 8, "offsets (O) must be uint64_t");
    static_assert(sizeof(K) == 4 || sizeof(K) == 8, "indices (K) must be 32- or 64-bit unsigned");

    // Indices column. Default: emit K-width (uint32 when K=uint32) zero-copy.
    // When use_u64_indices is requested and K is narrower than 64-bit, the widened
    // values exist nowhere in memory, so they are streamed a chunk at a time rather
    // than materialising a second copy of edgeKeys.
    if (opts.use_u64_indices && sizeof(K) < 8)
    {
        const K *keys = g.edgeKeys.data();
        writeParquetIdColumns<uint64_t>(
            output_path + ".indices.parquet", {"indices"}, static_cast<int64_t>(g.edgeKeys.size()),
            [keys](int64_t begin, int64_t end, std::vector<std::vector<uint64_t>> &bufs) {
                for (int64_t i = begin; i < end; ++i)
                    bufs[0][static_cast<size_t>(i - begin)] = static_cast<uint64_t>(keys[i]);
            });
    }
    else
    {
        auto idx_type = sizeof(K) == 4 ? arrow::uint32() : arrow::uint64();
        writeParquetColumn(output_path + ".indices.parquet", "indices",
                           wrapZeroCopy(g.edgeKeys.data(), static_cast<int64_t>(g.edgeKeys.size()), idx_type));
    }
    writeParquetColumn(output_path + ".indptr.parquet", "indptr",
                       wrapZeroCopy(g.offsets.data(), static_cast<int64_t>(g.offsets.size()), arrow::uint64()));
}

// Write a headerless CSV edge list. Undirected (default): one "u{sep}v" line per
// edge, emitted once with u<v (the CSR is symmetric, so the v<u copy is skipped).
// Directed (opts.directed): one line per stored arc u->v, every arc emitted.
// Parallelised over num_threads.

template <class K, class O>
void writeGraphToCSV(const DiGraphCsr<K, O> &g, const std::string &output_path, const ParseOptions &opts = {})
{
    const size_t n = g.span();
    const char sep = opts.sep;
    const bool directed = opts.directed;

    auto lineBytes = [&](size_t u) {
        size_t bytes = 0;
        g.forEachEdgeKey((K)u, [&](K v) {
            if (directed || v > (K)u)
                bytes += numDigits((uint32_t)u) + 1 + numDigits((uint32_t)v) + 1;
        });
        return bytes;
    };
    auto writeLine = [&](size_t u, char *p) {
        g.forEachEdgeKey((K)u, [&](K v) {
            if (directed || v > (K)u)
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

// Write the CSR as a Parquet edge list: one file, two id columns named by
// opts.source_col / opts.target_col. Undirected (default) emits each edge once as
// u,v with u<v, matching the CSV writer; directed emits every stored arc.
//
// Neither column exists as contiguous memory in a CSR, so rows are materialised a
// chunk at a time rather than building both columns in full. Row placement mirrors
// writeLinesMmap: count rows per vertex, prefix-sum to absolute positions, then
// fill in parallel. The file itself is written serially — one FileWriter.

template <class K, class O>
void writeGraphToEdgelistParquet(const DiGraphCsr<K, O> &g, const std::string &output_path,
                                 const ParseOptions &opts = {})
{
    const size_t n = g.span();
    const bool directed = opts.directed;
    const int T = opts.num_threads > 1 ? static_cast<int>(opts.num_threads) : 1;

    // Directed emits every arc, so the CSR offsets already are the row mapping.
    std::vector<O> row_off;
    const O *off = g.offsets.data();
    if (!directed)
    {
        row_off.resize(n + 1);
        std::vector<O> cnt(n);
#pragma omp parallel for num_threads(T) schedule(dynamic, 2048)
        for (size_t u = 0; u < n; ++u)
        {
            O c = O{};
            g.forEachEdgeKey((K)u, [&](K v) {
                if (v > (K)u)
                    ++c;
            });
            cnt[u] = c;
        }
        O total = O{};
        for (size_t u = 0; u < n; ++u)
        {
            row_off[u] = total;
            total += cnt[u];
        }
        row_off[n] = total;
        off = row_off.data();
    }
    const int64_t total_rows = static_cast<int64_t>(off[n]);

    // A vertex's rows may straddle a chunk boundary, so it is visited from both
    // chunks and writes only the part that falls inside the current one.
    auto fill = [&](int64_t begin, int64_t end, auto &bufs) {
        const int64_t len = end - begin;
        size_t u0 = static_cast<size_t>(std::upper_bound(off, off + n + 1, static_cast<O>(begin)) - off) - 1;
        size_t u1 = std::min<size_t>(static_cast<size_t>(std::lower_bound(off, off + n + 1, static_cast<O>(end)) - off), n);
#pragma omp parallel for num_threads(T) schedule(dynamic, 1024)
        for (size_t u = u0; u < u1; ++u)
        {
            int64_t pos = static_cast<int64_t>(off[u]) - begin;
            g.forEachEdgeKey((K)u, [&](K v) {
                if (!directed && v <= (K)u)
                    return;
                if (pos >= 0 && pos < len)
                {
                    bufs[0][static_cast<size_t>(pos)] = u;
                    bufs[1][static_cast<size_t>(pos)] = v;
                }
                ++pos;
            });
        }
    };

    const std::string path = output_path + ".parquet";
    const std::vector<std::string> names{opts.source_col, opts.target_col};
    if (opts.use_u64_indices || sizeof(K) == 8)
        writeParquetIdColumns<uint64_t>(path, names, total_rows, fill);
    else
        writeParquetIdColumns<uint32_t>(path, names, total_rows, fill);
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
    case EDGELIST_PARQUET:
        writeGraphToEdgelistParquet(g, output_path, opts);
        return;
    case CSV_EDGELIST:
        writeGraphToCSV(g, output_path, opts);
        return;
    default:
        throw std::runtime_error("writeGraph: unknown format");
    }
}
