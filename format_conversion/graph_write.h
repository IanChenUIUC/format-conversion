#pragma once

#include "formats.h"

#include <Graph.hxx>

#include <bit>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <limits>
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

// Ids are emitted as `v + base_index` through the 32-bit text and column paths, so
// the largest of them has to stay inside K's range.
template <class K, class O>
inline void validateWriteBase(const BuiltGraph<K, O> &bg, uint64_t base_index)
{
    const size_t n = bg.g.span();
    if (n == 0)
        return;
    if (base_index > uint64_t{std::numeric_limits<uint32_t>::max()} - (n - 1))
        throw std::runtime_error("base_index " + std::to_string(base_index) +
                                 " overflows the 32-bit id range (span " + std::to_string(n) + ")");
}

// Write the CSR as a METIS adjacency-list file. Parallelised over num_threads.

template <class K, class O>
void writeGraphToMetis(const BuiltGraph<K, O> &bg, const std::string &output_path, const MetisWrite &spec,
                       size_t num_threads)
{
    // METIS is an undirected adjacency format; its header edge count assumes a
    // symmetric graph (m = total arcs / 2). A graph stored as arcs only has no
    // faithful METIS representation, so reject it rather than emit a wrong count.
    if (!bg.symmetric)
        throw std::runtime_error("METIS output is undirected-only; this graph stores arcs only "
                                 "(use CsvEdgelist.Write or CsrParquet.Write for directed graphs)");

    const DiGraphCsr<K, O> &g = bg.g;
    const size_t n = g.span(), m = g.size() / 2;
    const uint32_t base = (uint32_t)spec.base_index;
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%zu %zu\n", n, m);

    auto lineBytes = [&](size_t u) {
        size_t bytes = 1; // trailing newline
        bool first = true;
        g.forEachEdgeKey((K)u, [&](K v) {
            if (!first)
                ++bytes; // space separator
            bytes += numDigits((uint32_t)v + base);
            first = false;
        });
        return bytes;
    };
    auto writeLine = [&](size_t u, char *p) {
        bool first = true;
        g.forEachEdgeKey((K)u, [&](K v) {
            if (!first)
                *p++ = ' ';
            auto [ptr, _] = std::to_chars(p, p + 11, (uint32_t)v + base);
            p = ptr;
            first = false;
        });
        *p = '\n';
    };

    writeLinesMmap(output_path + ".metis", n, std::string_view(header, hlen), lineBytes, writeLine, num_threads);
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
void writeGraphToParquet(const BuiltGraph<K, O> &bg, const std::string &output_path, const CsrParquetWrite &spec,
                         size_t)
{
    static_assert(sizeof(O) == 8, "offsets (O) must be uint64_t");
    static_assert(sizeof(K) == 4 || sizeof(K) == 8, "indices (K) must be 32- or 64-bit unsigned");

    const DiGraphCsr<K, O> &g = bg.g;
    const uint64_t base = spec.base_index;

    // Indices column. Default: emit K-width (uint32 when K=uint32) zero-copy.
    // A widened column or a non-zero base has values that exist nowhere in memory,
    // so those stream a chunk at a time rather than materialising a second copy of
    // edgeKeys.
    if (base == 0 && !(spec.u64_indices && sizeof(K) < 8))
    {
        auto idx_type = sizeof(K) == 4 ? arrow::uint32() : arrow::uint64();
        writeParquetColumn(output_path + ".indices.parquet", spec.indices_col,
                           wrapZeroCopy(g.edgeKeys.data(), static_cast<int64_t>(g.edgeKeys.size()), idx_type));
    }
    else
    {
        const K *keys = g.edgeKeys.data();
        auto fill = [keys, base](int64_t begin, int64_t end, auto &bufs) {
            using Id = std::decay_t<decltype(bufs[0][0])>;
            for (int64_t i = begin; i < end; ++i)
                bufs[0][static_cast<size_t>(i - begin)] = static_cast<Id>(keys[i] + base);
        };
        const std::string path = output_path + ".indices.parquet";
        const std::vector<std::string> names{spec.indices_col};
        const int64_t rows = static_cast<int64_t>(g.edgeKeys.size());
        if (spec.u64_indices || sizeof(K) == 8)
            writeParquetIdColumns<uint64_t>(path, names, rows, fill);
        else
            writeParquetIdColumns<uint32_t>(path, names, rows, fill);
    }

    // Offsets column. A base of k prepends k empty vertices, so row r starts at
    // offsets[r - k] and the k leading rows are zero.
    if (base == 0)
    {
        writeParquetColumn(output_path + ".indptr.parquet", spec.indptr_col,
                           wrapZeroCopy(g.offsets.data(), static_cast<int64_t>(g.offsets.size()), arrow::uint64()));
    }
    else
    {
        const O *off = g.offsets.data();
        writeParquetIdColumns<uint64_t>(
            output_path + ".indptr.parquet", {spec.indptr_col},
            static_cast<int64_t>(g.offsets.size() + base),
            [off, base](int64_t begin, int64_t end, std::vector<std::vector<uint64_t>> &bufs) {
                for (int64_t r = begin; r < end; ++r)
                    bufs[0][static_cast<size_t>(r - begin)] =
                        static_cast<uint64_t>(r) < base ? uint64_t{} : static_cast<uint64_t>(off[r - base]);
            });
    }
}

// Whether a writer emits every stored arc rather than one row per edge. A graph
// stored as arcs only has no duplicate to drop; a symmetric graph drops the v<u
// copy unless the caller asks for both directions back.
template <class K, class O, class Spec>
inline bool emitsEveryArc(const BuiltGraph<K, O> &bg, const Spec &spec)
{
    return !bg.symmetric || spec.expand_symmetric;
}

// Write a CSV edge list: one "u{sep}v" line per emitted row, optionally preceded
// by a header naming the two columns. Parallelised over num_threads.

template <class K, class O>
void writeGraphToCSV(const BuiltGraph<K, O> &bg, const std::string &output_path, const CsvEdgelistWrite &spec,
                     size_t num_threads)
{
    const DiGraphCsr<K, O> &g = bg.g;
    const size_t n = g.span();
    const char sep = spec.sep;
    const bool every_arc = emitsEveryArc(bg, spec);
    const uint32_t base = (uint32_t)spec.base_index;

    // Bound as a string_view below, so it has to outlive the write.
    std::string hdr;
    if (spec.header)
        hdr = spec.source_col + sep + spec.target_col + '\n';

    auto lineBytes = [&](size_t u) {
        size_t bytes = 0;
        g.forEachEdgeKey((K)u, [&](K v) {
            if (every_arc || v > (K)u)
                bytes += numDigits((uint32_t)u + base) + 1 + numDigits((uint32_t)v + base) + 1;
        });
        return bytes;
    };
    auto writeLine = [&](size_t u, char *p) {
        g.forEachEdgeKey((K)u, [&](K v) {
            if (every_arc || v > (K)u)
            {
                p = std::to_chars(p, p + 11, (uint32_t)u + base).ptr;
                *p++ = sep;
                p = std::to_chars(p, p + 11, (uint32_t)v + base).ptr;
                *p++ = '\n';
            }
        });
    };

    writeLinesMmap(output_path + ".csv", n, hdr, lineBytes, writeLine, num_threads);
}

// Write the CSR as a Parquet edge list: one file, two id columns named by
// spec.source_col / spec.target_col, matching the CSV writer's row selection.
//
// Neither column exists as contiguous memory in a CSR, so rows are materialised a
// chunk at a time rather than building both columns in full. Row placement mirrors
// writeLinesMmap: count rows per vertex, prefix-sum to absolute positions, then
// fill in parallel. The file itself is written serially — one FileWriter.

template <class K, class O>
void writeGraphToEdgelistParquet(const BuiltGraph<K, O> &bg, const std::string &output_path,
                                 const EdgelistParquetWrite &spec, size_t num_threads)
{
    const DiGraphCsr<K, O> &g = bg.g;
    const size_t n = g.span();
    const bool every_arc = emitsEveryArc(bg, spec);
    const uint32_t base = static_cast<uint32_t>(spec.base_index);
    const int T = num_threads > 1 ? static_cast<int>(num_threads) : 1;

    // Emitting every arc means the CSR offsets already are the row mapping.
    std::vector<O> row_off;
    const O *off = g.offsets.data();
    if (!every_arc)
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
                if (!every_arc && v <= (K)u)
                    return;
                if (pos >= 0 && pos < len)
                {
                    bufs[0][static_cast<size_t>(pos)] = static_cast<uint32_t>(u) + base;
                    bufs[1][static_cast<size_t>(pos)] = static_cast<uint32_t>(v) + base;
                }
                ++pos;
            });
        }
    };

    const std::string path = output_path + ".parquet";
    const std::vector<std::string> names{spec.source_col, spec.target_col};
    if (spec.u64_ids || sizeof(K) == 8)
        writeParquetIdColumns<uint64_t>(path, names, total_rows, fill);
    else
        writeParquetIdColumns<uint32_t>(path, names, total_rows, fill);
}

// Dispatch a graph to the writer named by its write spec.

template <class K, class O>
void writeGraph(const BuiltGraph<K, O> &bg, const std::string &output_path, const GraphSpec &spec,
                size_t num_threads)
{
    std::visit(
        [&](auto &&s) {
            using S = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<S, MetisWrite>)
                writeGraphToMetis(bg, output_path, s, num_threads);
            else if constexpr (std::is_same_v<S, CsrParquetWrite>)
                writeGraphToParquet(bg, output_path, s, num_threads);
            else if constexpr (std::is_same_v<S, EdgelistParquetWrite>)
                writeGraphToEdgelistParquet(bg, output_path, s, num_threads);
            else if constexpr (std::is_same_v<S, CsvEdgelistWrite>)
                writeGraphToCSV(bg, output_path, s, num_threads);
            else
                throw std::logic_error("writeGraph: not a write spec");
        },
        spec);
}
