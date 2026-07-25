#pragma once

// Reading graphs into an in-memory CSR (DiGraphCsr), plus the node-id remapping
// and the in-place adjacency sort. See DESIGN.md for the build algorithm and the
// node-map representation choices.

#include "formats.h"
#include "system.h"

#include <Graph.hxx>
#include <io.hxx>
#include <robin_hood.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// Maps a raw node id (after subtracting base_index) to a compact id in [0, N).
// Built from a node list; the compact id is the row's position in that list.
// find(raw) returns the compact id, or INVALID_ID if the raw id is unknown.
template <class K = uint32_t> struct NodeMap
{
    static constexpr K INVALID_ID = std::numeric_limits<K>::max();

    enum class Mode
    {
        Dense, // identity: no node list; raw id is the compact id
        Array, // flat remap[raw - min_id]; for compact-but-offset id spaces
        Hash   // hash map; fallback for sparse id spaces
    };

    Mode mode = Mode::Hash;
    K N = 0;

    K min_id = 0;
    std::vector<K> remap;

    robin_hood::unordered_flat_map<K, K> map;

    // Backing node-file bytes (must outlive this map); used by getRow.
    std::vector<size_t> line_offsets;
    const char *file_data = nullptr;
    size_t file_size = 0;

    // The node file's header line, re-emitted verbatim by writeNodelist. Empty
    // when the file has no header.
    std::string header_row;

    NodeMap() = default;
    explicit NodeMap(K n) : mode(Mode::Dense), N(n)
    {
    }

    K size() const
    {
        return N;
    }
    bool isDense() const
    {
        return mode == Mode::Dense;
    }

    K find(K raw) const
    {
        switch (mode)
        {
        case Mode::Dense:
            return raw;
        case Mode::Array: {
            if (raw < min_id)
                return INVALID_ID;
            K off = static_cast<K>(raw - min_id);
            return off < remap.size() ? remap[off] : INVALID_ID;
        }
        case Mode::Hash: {
            auto it = map.find(raw);
            return it != map.end() ? it->second : INVALID_ID;
        }
        }
        return INVALID_ID;
    }

    // The verbatim node-file row for a compact id (no trailing newline), as a view
    // into the backing file. Empty in dense mode or when out of range.
    std::string_view getRow(K compact_id) const
    {
        if (!file_data || compact_id >= static_cast<K>(line_offsets.size()))
            return {};
        const char *start = file_data + line_offsets[compact_id];
        const char *end = file_data + file_size;
        const char *nl = static_cast<const char *>(memchr(start, '\n', end - start));
        const char *row_end = nl ? nl : end;
        if (row_end > start && *(row_end - 1) == '\r')
            --row_end;
        return {start, static_cast<size_t>(row_end - start)};
    }
};

// Build a NodeMap from a node list, assigning compact ids in file order. Chooses
// the Array representation for compact-but-offset id spaces and Hash otherwise.
template <class K = uint32_t> NodeMap<K> buildNodeMap(const NodeDescriptor &nd)
{
    static constexpr double MAX_REMAP_SPAN_RATIO = 2.0;

    NodeMap<K> nm;
    nm.file_data = nd.mmap.data;
    nm.file_size = nd.mmap.size;

    const char *base = nd.mmap.data;
    const char *p = base;
    const char *end = p + nd.mmap.size;
    const uint64_t bias = nd.spec.base_index;

    auto skipLine = [&] {
        const char *nl = (const char *)memchr(p, '\n', end - p);
        p = nl ? nl + 1 : end;
    };

    // Capture the first skipped line as the header (for writeNodelist).
    if (nd.spec.skip_rows > 0 && p < end)
    {
        const char *hl = p;
        skipLine();
        const char *row_end = p > hl ? p - 1 : hl;
        if (row_end > hl && *(row_end - 1) == '\r')
            --row_end;
        nm.header_row.assign(hl, row_end);
        for (size_t i = 1; i < nd.spec.skip_rows && p < end; ++i)
            skipLine();
    }

    std::vector<K> ids;
    bool have_any = false;
    K min_id = 0, max_id = 0;

    while (p < end)
    {
        const char *line_start = p;
        while (p < end && (*p == ' ' || *p == '\t'))
            ++p;
        if (p >= end)
            break;
        if (*p == '\n')
        {
            ++p;
            continue;
        }
        if (*p == nd.spec.comment_char)
        {
            skipLine();
            continue;
        }
        uint64_t raw = 0;
        auto [next, ec] = std::from_chars(p, end, raw);
        if (ec == std::errc{} && raw >= bias)
        {
            K id = static_cast<K>(raw - bias);
            ids.push_back(id);
            nm.line_offsets.push_back(static_cast<size_t>(line_start - base));
            if (!have_any)
            {
                min_id = max_id = id;
                have_any = true;
            }
            else
            {
                min_id = std::min(min_id, id);
                max_id = std::max(max_id, id);
            }
        }
        else if (!have_any && nm.header_row.empty())
        {
            // First non-numeric line is an implicit header; capture it once.
            const char *nl = (const char *)memchr(line_start, '\n', end - line_start);
            const char *row_end = nl ? nl : end;
            if (row_end > line_start && *(row_end - 1) == '\r')
                --row_end;
            nm.header_row.assign(line_start, row_end);
        }
        p = next;
        skipLine();
    }

    nm.N = static_cast<K>(ids.size());

    if (!have_any)
    {
        nm.mode = NodeMap<K>::Mode::Hash;
        return nm;
    }

    const uint64_t span = static_cast<uint64_t>(max_id) - static_cast<uint64_t>(min_id) + 1;
    const bool use_array = static_cast<double>(span) <= MAX_REMAP_SPAN_RATIO * static_cast<double>(nm.N);

    if (use_array)
    {
        nm.mode = NodeMap<K>::Mode::Array;
        nm.min_id = min_id;
        nm.remap.assign(static_cast<size_t>(span), NodeMap<K>::INVALID_ID);
        for (K compact = 0; compact < nm.N; ++compact)
            nm.remap[static_cast<size_t>(ids[compact]) - min_id] = compact;
    }
    else
    {
        nm.mode = NodeMap<K>::Mode::Hash;
        nm.map.reserve(nm.N);
        for (K compact = 0; compact < nm.N; ++compact)
            nm.map.emplace(ids[compact], compact);
    }
    return nm;
}

// Map one raw edge to compact ids: subtract base_index, reject ids too wide for K,
// look both endpoints up in the NodeMap, and apply the self-loop rule. Invokes
// cb(u, v) only for edges that survive. Format-agnostic — every reader funnels
// through this so the filtering rules cannot drift between formats.
template <class K, class Spec, class Fn>
inline void mapRawEdge(uint64_t bi, uint64_t bj, const NodeMap<K> &nm, const Spec &spec, Fn &&cb)
{
    static_assert(std::is_integral_v<K> && std::is_unsigned_v<K>, "K (index type) must be an unsigned integer");

    const uint64_t bias = spec.base_index;
    if (bi < bias || bj < bias)
        return;
    uint64_t nu = bi - bias, nv = bj - bias;
    if constexpr (sizeof(K) < sizeof(uint64_t))
    {
        if (nu > std::numeric_limits<K>::max() || nv > std::numeric_limits<K>::max())
            throw std::runtime_error("node id exceeds the width of the index type K "
                                     "(use a wider K, e.g. uint64_t)");
    }
    K u = nm.find(static_cast<K>(nu));
    K v = nm.find(static_cast<K>(nv));
    if (u == NodeMap<K>::INVALID_ID || v == NodeMap<K>::INVALID_ID)
        return;
    if (!spec.keep_self_loops && u == v)
        return;
    cb(u, v);
}

// Parse one block of edge text, mapping endpoints through the NodeMap and invoking
// cb(u, v) for each valid edge (unknown endpoints and, unless keep_self_loops,
// self-loops are dropped). Throws on a malformed number or an id wider than K.
template <class K, class Fn>
inline void forEachValidEdge(std::string_view blk, const NodeMap<K> &nm, const CsvEdgelistRead &spec, Fn &&cb)
{
    auto fb = [&](int64_t ri, int64_t rj, double) {
        mapRawEdge<K>(static_cast<uint64_t>(ri), static_cast<uint64_t>(rj), nm, spec, cb);
    };

    // Dispatch the runtime (sep, comment_char) onto the matching compile-time
    // parser instantiation (validated by the caller, see buildCSRFromCSV).
    const bool hash = spec.comment_char == '#';
    switch (spec.sep)
    {
    case ',':
        if (hash)
            readEdgelistFormatDoChecked<false, 0, ',', '#'>(blk, false, fb);
        else
            readEdgelistFormatDoChecked<false, 0, ',', '%'>(blk, false, fb);
        break;
    case '\t':
        if (hash)
            readEdgelistFormatDoChecked<false, 0, '\t', '#'>(blk, false, fb);
        else
            readEdgelistFormatDoChecked<false, 0, '\t', '%'>(blk, false, fb);
        break;
    case ' ':
        if (hash)
            readEdgelistFormatDoChecked<false, 0, ' ', '#'>(blk, false, fb);
        else
            readEdgelistFormatDoChecked<false, 0, ' ', '%'>(blk, false, fb);
        break;
    default:
        throw std::runtime_error("sep must be ',', '\\t', or ' '");
    }
}

inline constexpr size_t CSR_BLOCK_BYTES = 1u << 20;

// Run forEachValidEdge over thread t's blocks of the file. The file is tiled into
// CSR_BLOCK_BYTES line-aligned blocks; thread t owns blocks t, t+T, t+2T, …, a
// stable assignment both passes of the build rely on.
template <class K, class Fn>
inline void forEachValidEdgeStripe(std::string_view data, int t, int T, size_t nblocks, const NodeMap<K> &nm,
                                   const CsvEdgelistRead &spec, Fn &&cb)
{
    for (size_t b = static_cast<size_t>(t); b < nblocks; b += static_cast<size_t>(T))
    {
        auto blk = readEdgelistFormatBlock(data, b * CSR_BLOCK_BYTES, CSR_BLOCK_BYTES);
        forEachValidEdge(blk, nm, spec, cb);
    }
}

// Build a CSR from any edge source. `forEachStripe(t, T, cb)` must invoke cb(u, v)
// for thread t's share of the edges, already remapped and filtered. It is called
// once per pass and must assign the same edges to the same t every time — the
// no-atomics scatter below depends on it (see DESIGN.md).
//
// Parallelised over num_threads with no atomics on the per-edge path; output
// adjacency order is unspecified unless sorted later.
template <class K, class O, class Spec, class ForEachStripe>
DiGraphCsr<K, O> buildCSRFromEdgeSource(NodeMap<K> &nm, const Spec &spec, size_t num_threads,
                                        ForEachStripe &&forEachStripe)
{
    const int T = num_threads > 1 ? static_cast<int>(num_threads) : 1;

    // Dense mode: N is unknown up front, so discover max(id)+1 first.
    if (nm.isDense() && nm.N == 0)
    {
        std::vector<K> tmax(T, K{});
        parallelStripes(T, [&](int t) {
            K m = K{};
            forEachStripe(t, T, [&](K u, K v) { m = std::max({m, u, v}); });
            tmax[t] = m;
        });
        K N = K{};
        for (int t = 0; t < T; ++t)
            N = std::max(N, tmax[t]);
        nm.N = static_cast<K>(N + 1);
    }
    const K N = nm.N;
    const size_t TN = static_cast<size_t>(T) * N;

    // Per-thread degree/cursor table, row t = tdeg[t·N .. (t+1)·N).
    HugeArray<O> tdeg(TN);
    O *td = tdeg.data();
    std::memset(td, 0, TN * sizeof(O));

    // Pass 1: each thread counts degrees into its own row (no shared writes).
    // Undirected symmetrizes (counts both endpoints); directed counts only the
    // out-endpoint. The branch is hoisted out of the per-edge path.
    if (spec.directed)
        parallelStripes(T, [&](int t) {
            O *deg = td + static_cast<size_t>(t) * N;
            forEachStripe(t, T, [&](K u, K) { ++deg[u]; });
        });
    else
        parallelStripes(T, [&](int t) {
            O *deg = td + static_cast<size_t>(t) * N;
            forEachStripe(t, T, [&](K u, K v) {
                ++deg[u];
                ++deg[v];
            });
        });

    // Offsets = prefix sum of total per-vertex degree.
    DiGraphCsr<K, O> g;
    g.offsets.resize(static_cast<size_t>(N) + 1);
    O total = O{};
    for (K v = 0; v < N; ++v)
    {
        g.offsets[v] = total;
        for (int t = 0; t < T; ++t)
            total += td[static_cast<size_t>(t) * N + v];
    }
    g.offsets[N] = total;

    // Turn the degree table into per-thread write cursors over disjoint slices.
#pragma omp parallel for num_threads(T) schedule(static)
    for (K v = 0; v < N; ++v)
    {
        O base = g.offsets[v];
        for (int t = 0; t < T; ++t)
        {
            size_t i = static_cast<size_t>(t) * N + v;
            O d = td[i];
            td[i] = base;
            base += d;
        }
    }

    // Pass 2: scatter into the disjoint slices (no shared writes, every slot
    // written exactly once). Undirected writes both arcs; directed only u->v.
    g.edgeKeys.resize(static_cast<size_t>(total));
    adviseHugePages(g.edgeKeys.data(), g.edgeKeys.size() * sizeof(K));
    if (spec.directed)
        parallelStripes(T, [&](int t) {
            O *cur = td + static_cast<size_t>(t) * N;
            forEachStripe(t, T, [&](K u, K v) { g.edgeKeys[cur[u]++] = v; });
        });
    else
        parallelStripes(T, [&](int t) {
            O *cur = td + static_cast<size_t>(t) * N;
            forEachStripe(t, T, [&](K u, K v) {
                g.edgeKeys[cur[u]++] = v;
                g.edgeKeys[cur[v]++] = u;
            });
        });

    return g;
}

// Build a CSR from a CSV edge list. Throws (catchably) on malformed input or
// out-of-range ids.
template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildCSRFromCSV(std::string_view data, NodeMap<K> &nm, const CsvEdgelistRead &spec,
                                 size_t num_threads)
{
    if (spec.comment_char != '#' && spec.comment_char != '%')
        throw std::runtime_error("comment_char must be '#' or '%'");
    if (spec.sep != ',' && spec.sep != '\t' && spec.sep != ' ')
        throw std::runtime_error("sep must be ',', '\\t', or ' '");

    const size_t nblocks = (data.size() + CSR_BLOCK_BYTES - 1) / CSR_BLOCK_BYTES;
    return buildCSRFromEdgeSource<K, O>(nm, spec, num_threads, [&](int t, int T, auto &&cb) {
        forEachValidEdgeStripe(data, t, T, nblocks, nm, spec, cb);
    });
}

// Build a CSR from a METIS adjacency-list file (single pass; the "N M" header
// gives both counts up front). The i-th adjacency line is vertex i, whose file id
// is i + spec.base_index; neighbor ids carry that same base.
template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildGraphFromMETIS(std::string_view data, const MetisRead &spec)
{
    const char *p = data.data();
    const char *end = p + data.size();

    auto skipLine = [&] {
        while (p < end && *p != '\n')
            ++p;
        if (p < end)
            ++p;
    };

    auto isCommentStart = [&](const char *q) { return *q == '%' || *q == spec.comment_char; };

    while (p < end && isCommentStart(p))
        skipLine();

    size_t N = 0, M = 0;
    auto r1 = std::from_chars(p, end, N);
    p = r1.ptr;
    while (p < end && (*p == ' ' || *p == '\t'))
        ++p;
    auto r2 = std::from_chars(p, end, M);
    (void)r2;
    skipLine();

    DiGraphCsr<K, O> g;
    g.offsets.resize(N + 1);
    g.edgeKeys.resize(2 * M);

    const K base = static_cast<K>(spec.base_index);
    O edge_pos = O{};
    for (size_t u = 0; u < N; ++u)
    {
        while (p < end && isCommentStart(p))
            skipLine();

        g.offsets[u] = edge_pos;

        while (p < end && *p != '\n')
        {
            while (p < end && (*p == ' ' || *p == '\t'))
                ++p;
            if (p >= end || *p == '\n')
                break;
            K v = K{};
            auto r = std::from_chars(p, end, v);
            if (r.ec != std::errc{})
                break;
            if (v < base)
                throw std::runtime_error("METIS neighbor id " + std::to_string(v) + " is below base_index " +
                                         std::to_string(spec.base_index));
            g.edgeKeys[edge_pos++] = v - base;
            p = r.ptr;
        }
        skipLine();
    }
    g.offsets[N] = edge_pos;
    return g;
}

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#include <parquet/exception.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/statistics.h>

// Read a single named column from a Parquet file (uint32 or uint64) into a vector.
template <class T> std::vector<T> readParquetColumn(const std::string &path, const std::string &col_name)
{
    auto infile = arrow::io::ReadableFile::Open(path).ValueOrDie();
    auto reader = parquet::arrow::OpenFile(infile, arrow::default_memory_pool()).ValueOrDie();

    std::shared_ptr<arrow::Table> table = reader->ReadTable().ValueOrDie();
    auto col = table->GetColumnByName(col_name);
    if (!col)
        throw std::runtime_error("Column '" + col_name + "' not found in " + path);

    std::vector<T> result;
    result.reserve(static_cast<size_t>(col->length()));

    for (const auto &chunk : col->chunks())
    {
        if (auto a = std::dynamic_pointer_cast<arrow::UInt32Array>(chunk))
            for (int64_t i = 0; i < a->length(); ++i)
                result.push_back(static_cast<T>(a->Value(i)));
        else if (auto a = std::dynamic_pointer_cast<arrow::UInt64Array>(chunk))
            for (int64_t i = 0; i < a->length(); ++i)
                result.push_back(static_cast<T>(a->Value(i)));
        else
            throw std::runtime_error("Unexpected column type: " + chunk->type()->ToString());
    }
    return result;
}

// Resolve a column name to its position in a Parquet schema. Throws listing the
// file's actual columns, which is the usual cause of a mismatch.
inline int parquetColumnIndex(const parquet::SchemaDescriptor *schema, const std::string &name,
                              const std::string &path)
{
    std::string available;
    for (int i = 0; i < schema->num_columns(); ++i)
    {
        if (schema->Column(i)->name() == name)
            return i;
        if (!available.empty())
            available += ", ";
        available += schema->Column(i)->name();
    }
    throw std::runtime_error("column '" + name + "' not found in " + path + " (columns: " + available + ")");
}

// Flatten one decoded id column into `out` as uint64. Accepts the four physical
// types Parquet uses for integer ids; a negative value can never be a node id, so
// it is rejected here rather than wrapping into a huge unsigned one.
inline void readIdColumn(const arrow::ChunkedArray &col, std::vector<uint64_t> &out)
{
    out.clear();
    out.reserve(static_cast<size_t>(col.length()));

    auto push_signed = [&out](auto *a) {
        for (int64_t i = 0; i < a->length(); ++i)
        {
            auto v = a->Value(i);
            if (v < 0)
                throw std::runtime_error("negative node id in Parquet edge list");
            out.push_back(static_cast<uint64_t>(v));
        }
    };

    for (const auto &chunk : col.chunks())
    {
        if (auto a = std::dynamic_pointer_cast<arrow::UInt32Array>(chunk))
            for (int64_t i = 0; i < a->length(); ++i)
                out.push_back(a->Value(i));
        else if (auto a = std::dynamic_pointer_cast<arrow::UInt64Array>(chunk))
            for (int64_t i = 0; i < a->length(); ++i)
                out.push_back(a->Value(i));
        else if (auto a = std::dynamic_pointer_cast<arrow::Int32Array>(chunk))
            push_signed(a.get());
        else if (auto a = std::dynamic_pointer_cast<arrow::Int64Array>(chunk))
            push_signed(a.get());
        else
            throw std::runtime_error("unsupported Parquet id column type: " + chunk->type()->ToString());
    }
}

// Largest id across both columns, from row-group statistics alone (no decode).
// Returns false when any row group lacks them, in which case the caller must fall
// back to a scan. Parquet has no unsigned physical types — a uint32 column is
// stored as INT32 with an unsigned logical type — so the statistic must be
// reinterpreted rather than sign-extended, or ids above 2^31 come back negative.
inline bool maxIdFromStatistics(const parquet::FileMetaData &md, int ci_src, int ci_dst, uint64_t &out_max)
{
    uint64_t mx = 0;
    for (int rg = 0; rg < md.num_row_groups(); ++rg)
    {
        auto rgm = md.RowGroup(rg);
        for (int ci : {ci_src, ci_dst})
        {
            auto st = rgm->ColumnChunk(ci)->statistics();
            if (!st || !st->HasMinMax())
                return false;
            if (st->physical_type() == parquet::Type::INT32)
                mx = std::max(mx, static_cast<uint64_t>(
                                      static_cast<uint32_t>(std::static_pointer_cast<parquet::Int32Statistics>(st)->max())));
            else if (st->physical_type() == parquet::Type::INT64)
                mx = std::max(mx, static_cast<uint64_t>(std::static_pointer_cast<parquet::Int64Statistics>(st)->max()));
            else
                return false;
        }
    }
    out_max = mx;
    return true;
}

// Build a CSR from a Parquet edge list (two id columns, named by spec.source_col /
// spec.target_col; any other columns are never decoded).
//
// The file is decoded twice — once to count degrees, once to scatter — rather than
// buffered, which keeps resident memory at one row group per thread instead of the
// whole edge list. See DESIGN.md.
template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildCSRFromEdgelistParquet(const std::string &path, NodeMap<K> &nm,
                                             const EdgelistParquetRead &spec, size_t num_threads)
{
    auto md = parquet::ParquetFileReader::OpenFile(path, false)->metadata();
    const int ci_src = parquetColumnIndex(md->schema(), spec.source_col, path);
    const int ci_dst = parquetColumnIndex(md->schema(), spec.target_col, path);
    const int nrg = md->num_row_groups();
    const std::vector<int> cols{ci_src, ci_dst};

    // Dense mode needs max(id)+1 up front. Statistics carry it in the footer, for
    // no decode at all — but they describe every row, while a scan sees only the
    // edges that survive filtering, so the two disagree whenever the largest id
    // appears solely on a dropped edge. In dense mode an edge is dropped only by
    // the self-loop rule or by a base_index underflow, so the footer is
    // authoritative exactly when neither can happen. Otherwise N is left unset and
    // the build below discovers it by scanning, matching the CSV path.
    const bool stats_agree_with_scan = spec.keep_self_loops && spec.base_index == 0;
    if (nm.isDense() && nm.N == 0 && stats_agree_with_scan)
    {
        uint64_t raw_max = 0;
        if (maxIdFromStatistics(*md, ci_src, ci_dst, raw_max) && raw_max >= spec.base_index)
        {
            uint64_t n = raw_max - spec.base_index + 1;
            if constexpr (sizeof(K) < sizeof(uint64_t))
            {
                if (n > std::numeric_limits<K>::max())
                    throw std::runtime_error("node id exceeds the width of the index type K "
                                             "(use a wider K, e.g. uint64_t)");
            }
            nm.N = static_cast<K>(n);
        }
    }

    return buildCSRFromEdgeSource<K, O>(nm, spec, num_threads, [&](int t, int T, auto &&cb) {
        // One reader per thread: concurrent ReadRowGroup on a shared reader is not
        // documented as safe.
        auto infile = arrow::io::ReadableFile::Open(path).ValueOrDie();
        auto reader = parquet::arrow::OpenFile(infile, arrow::default_memory_pool()).ValueOrDie();

        std::vector<uint64_t> src, dst;
        for (int rg = t; rg < nrg; rg += T)
        {
            std::shared_ptr<arrow::Table> table;
            PARQUET_THROW_NOT_OK(reader->ReadRowGroup(rg, cols, &table));
            readIdColumn(*table->column(0), src);
            readIdColumn(*table->column(1), dst);
            const size_t n = std::min(src.size(), dst.size());
            for (size_t i = 0; i < n; ++i)
                mapRawEdge<K>(src[i], dst[i], nm, spec, cb);
        }
    });
}

// Rebase a CSR read from a file whose first vertex is numbered `base`: drop the
// leading entries of offsets and shift every neighbor id down. The inverse of what
// CsrParquet.Write does, so it refuses a file whose leading vertices carry edges
// rather than silently discarding them — a CSR has no per-edge drop that keeps
// offsets consistent.
template <class K, class O> void dropCsrBase(DiGraphCsr<K, O> &g, uint64_t base)
{
    if (base == 0)
        return;
    if (g.offsets.size() <= base)
        throw std::runtime_error("CsrParquet.Read: indptr is shorter than base_index " + std::to_string(base));
    for (uint64_t i = 0; i <= base; ++i)
        if (g.offsets[i] != O{})
            throw std::runtime_error("CsrParquet.Read: base_index " + std::to_string(base) +
                                     " requires the leading vertices to have no edges");
    g.offsets.erase(g.offsets.begin(), g.offsets.begin() + static_cast<ptrdiff_t>(base));

    const K k = static_cast<K>(base);
    for (K &v : g.edgeKeys)
    {
        if (v < k)
            throw std::runtime_error("CsrParquet.Read: index " + std::to_string(v) + " is below base_index " +
                                     std::to_string(base));
        v -= k;
    }
}

// Sort each vertex's adjacency list in place. num_threads <= 0 uses all available.
template <class K, class O> void sortNeighbors(DiGraphCsr<K, O> &g, int num_threads = 0)
{
    const size_t N = g.span();
#ifdef _OPENMP
    const int T = num_threads > 0 ? num_threads : omp_get_max_threads();
#else
    const int T = 1;
    (void)num_threads;
#endif
    K *keys = g.edgeKeys.data();
#pragma omp parallel for num_threads(T) schedule(dynamic, 1024)
    for (size_t v = 0; v < N; ++v)
        std::sort(keys + g.offsets[v], keys + g.offsets[v + 1]);
}

// Build a CSR from an input graph, optionally remapping ids through a node list
// (nd == nullptr selects dense mode). nm is populated for downstream use.
//
// Only the text formats are memory-mapped, and only for the duration of the build:
// the CSR is materialised before the mapping is dropped, and the Parquet readers
// open the file through Arrow instead.
template <class K = uint32_t, class O = uint64_t>
BuiltGraph<K, O> buildGraph(const GraphDescriptor &gd, const NodeDescriptor *nd, NodeMap<K> &nm, size_t num_threads)
{
    if (!isReadSpec(gd.spec))
        throw std::runtime_error("expected a read spec, got " + specName(gd.spec));

    BuiltGraph<K, O> out;
    out.symmetric = readSymmetric(gd.spec);

    std::visit(
        [&](auto &&spec) {
            using S = std::decay_t<decltype(spec)>;
            if constexpr (std::is_same_v<S, CsvEdgelistRead>)
            {
                MmapFile mf(gd.path);
                std::string_view data = mf.view();
                for (size_t i = 0; i < spec.skip_rows && !data.empty(); ++i)
                {
                    auto nl = data.find('\n');
                    data = (nl != std::string_view::npos) ? data.substr(nl + 1) : std::string_view{};
                }
                nm = nd ? buildNodeMap<K>(*nd) : NodeMap<K>(K{});
                out.g = buildCSRFromCSV<K, O>(data, nm, spec, num_threads);
            }
            else if constexpr (std::is_same_v<S, MetisRead>)
            {
                MmapFile mf(gd.path);
                out.g = buildGraphFromMETIS<K, O>(mf.view(), spec);
            }
            else if constexpr (std::is_same_v<S, EdgelistParquetRead>)
            {
                nm = nd ? buildNodeMap<K>(*nd) : NodeMap<K>(K{});
                out.g = buildCSRFromEdgelistParquet<K, O>(gd.path, nm, spec, num_threads);
            }
            else if constexpr (std::is_same_v<S, CsrParquetRead>)
            {
                const std::string suffix = ".indices.parquet";
                if (!gd.path.ends_with(suffix))
                    throw std::runtime_error("CsrParquet.Read path must end with .indices.parquet");
                std::string base = gd.path.substr(0, gd.path.size() - suffix.size());

                out.g.edgeKeys = readParquetColumn<K>(base + ".indices.parquet", spec.indices_col);
                out.g.offsets = readParquetColumn<O>(base + ".indptr.parquet", spec.indptr_col);
                dropCsrBase(out.g, spec.base_index);
            }
            else
                throw std::logic_error("buildGraph: not a read spec");
        },
        gd.spec);

    return out;
}

template <class K = uint32_t, class O = uint64_t>
BuiltGraph<K, O> buildGraph(const GraphDescriptor &gd, const NodeDescriptor *nd, size_t num_threads)
{
    NodeMap<K> nm;
    return buildGraph<K, O>(gd, nd, nm, num_threads);
}

// Read a label file (one label per line) into a vector indexed by compact id.
template <class L = int32_t>
std::vector<L> buildLabelMap(const std::string &labels_path, size_t N, const LabelsCsv &spec)
{
    static_assert(std::is_arithmetic_v<L>, "label type must be arithmetic; string labels not yet supported");

    MmapFile mf(labels_path);
    const char *p = mf.data ? mf.data : nullptr;
    const char *end = p ? p + mf.size : nullptr;

    for (size_t i = 0; i < spec.skip_rows && p < end; ++i)
    {
        const char *nl = (const char *)memchr(p, '\n', end - p);
        p = nl ? nl + 1 : end;
    }

    std::vector<L> labels;
    labels.reserve(N);

    while (p && p < end && labels.size() < N)
    {
        while (p < end && (*p == ' ' || *p == '\t'))
            ++p;
        if (p >= end)
            break;
        if (*p == '\n')
        {
            ++p;
            continue;
        }
        if (*p == spec.comment_char)
        {
            const char *nl = (const char *)memchr(p, '\n', end - p);
            p = nl ? nl + 1 : end;
            continue;
        }
        L val{};
        auto [next, ec] = std::from_chars(p, end, val);
        if (ec == std::errc{})
        {
            labels.push_back(val);
            p = next;
        }
        const char *nl = (const char *)memchr(p, '\n', end - p);
        p = nl ? nl + 1 : end;
    }

    if (labels.size() != N)
        throw std::runtime_error("buildLabelMap: expected " + std::to_string(N) + " labels, got " +
                                 std::to_string(labels.size()));
    return labels;
}
