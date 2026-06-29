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
    const uint64_t bias = nd.opts.base_index;

    auto skipLine = [&] {
        const char *nl = (const char *)memchr(p, '\n', end - p);
        p = nl ? nl + 1 : end;
    };

    // Capture the first skipped line as the header (for writeNodelist).
    if (nd.opts.skip_rows > 0 && p < end)
    {
        const char *hl = p;
        skipLine();
        const char *row_end = p > hl ? p - 1 : hl;
        if (row_end > hl && *(row_end - 1) == '\r')
            --row_end;
        nm.header_row.assign(hl, row_end);
        for (size_t i = 1; i < nd.opts.skip_rows && p < end; ++i)
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
        if (*p == nd.opts.comment_char)
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

// Parse one block of edge text, mapping endpoints through the NodeMap and invoking
// cb(u, v) for each valid edge (unknown endpoints and, unless keep_self_loops,
// self-loops are dropped). Throws on a malformed number or an id wider than K.
template <class K, class Fn>
inline void forEachValidEdge(std::string_view blk, const NodeMap<K> &nm, const ParseOptions &opts, Fn &&cb)
{
    static_assert(std::is_integral_v<K> && std::is_unsigned_v<K>, "K (index type) must be an unsigned integer");

    const uint64_t bias = opts.base_index;
    auto fb = [&](int64_t ri, int64_t rj, double) {
        auto bi = static_cast<uint64_t>(ri), bj = static_cast<uint64_t>(rj);
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
        if (!opts.keep_self_loops && u == v)
            return;
        cb(u, v);
    };

    // Dispatch the runtime (sep, comment_char) onto the matching compile-time
    // parser instantiation (validated by the caller, see buildCSRFromCSV).
    const bool hash = opts.comment_char == '#';
    switch (opts.sep)
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
                                   const ParseOptions &opts, Fn &&cb)
{
    for (size_t b = static_cast<size_t>(t); b < nblocks; b += static_cast<size_t>(T))
    {
        auto blk = readEdgelistFormatBlock(data, b * CSR_BLOCK_BYTES, CSR_BLOCK_BYTES);
        forEachValidEdge(blk, nm, opts, cb);
    }
}

// Build a CSR from a CSV edge list. Parallelised over num_threads with no atomics
// on the per-edge path; output adjacency order is unspecified unless sorted later.
// Throws (catchably) on malformed input or out-of-range ids.
template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildCSRFromCSV(std::string_view data, NodeMap<K> &nm, const ParseOptions &opts)
{
    if (opts.comment_char != '#' && opts.comment_char != '%')
        throw std::runtime_error("comment_char must be '#' or '%'");
    if (opts.sep != ',' && opts.sep != '\t' && opts.sep != ' ')
        throw std::runtime_error("sep must be ',', '\\t', or ' '");

    const int T = opts.num_threads > 1 ? static_cast<int>(opts.num_threads) : 1;
    const size_t nblocks = (data.size() + CSR_BLOCK_BYTES - 1) / CSR_BLOCK_BYTES;

    // Dense mode: N is unknown up front, so discover max(id)+1 first.
    if (nm.isDense() && nm.N == 0)
    {
        std::vector<K> tmax(T, K{});
        parallelStripes(T, [&](int t) {
            K m = K{};
            forEachValidEdgeStripe(data, t, T, nblocks, nm, opts, [&](K u, K v) { m = std::max({m, u, v}); });
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
    parallelStripes(T, [&](int t) {
        O *deg = td + static_cast<size_t>(t) * N;
        forEachValidEdgeStripe(data, t, T, nblocks, nm, opts, [&](K u, K v) {
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
    // written exactly once).
    g.edgeKeys.resize(static_cast<size_t>(total));
    adviseHugePages(g.edgeKeys.data(), g.edgeKeys.size() * sizeof(K));
    parallelStripes(T, [&](int t) {
        O *cur = td + static_cast<size_t>(t) * N;
        forEachValidEdgeStripe(data, t, T, nblocks, nm, opts, [&](K u, K v) {
            g.edgeKeys[cur[u]++] = v;
            g.edgeKeys[cur[v]++] = u;
        });
    });

    return g;
}

// Build a CSR from a METIS adjacency-list file (single pass; the "N M" header
// gives both counts up front). Neighbor ids are 1-indexed in the file.
template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildGraphFromMETIS(std::string_view data, const ParseOptions &opts)
{
    const char *p = data.data();
    const char *end = p + data.size();

    auto skipLine = [&] {
        while (p < end && *p != '\n')
            ++p;
        if (p < end)
            ++p;
    };

    auto isCommentStart = [&](const char *q) { return *q == '%' || *q == opts.comment_char; };

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
            g.edgeKeys[edge_pos++] = v - K{1};
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
template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildGraph(const GraphDescriptor &gd, const NodeDescriptor *nd, NodeMap<K> &nm)
{
    switch (gd.fmt)
    {
    case CSV_EDGELIST: {
        std::string_view data = gd.mmap.view();
        for (size_t i = 0; i < gd.opts.skip_rows && !data.empty(); ++i)
        {
            auto nl = data.find('\n');
            data = (nl != std::string_view::npos) ? data.substr(nl + 1) : std::string_view{};
        }
        nm = nd ? buildNodeMap<K>(*nd) : NodeMap<K>(K{});
        return buildCSRFromCSV<K, O>(data, nm, gd.opts);
    }

    case METIS:
        return buildGraphFromMETIS<K, O>(gd.mmap.view(), gd.opts);

    case CSR_PARQUET: {
        const std::string suffix = ".indices.parquet";
        const std::string &full = gd.mmap.path;
        if (!full.ends_with(suffix))
            throw std::runtime_error("CSR_PARQUET path must end with .indices.parquet");
        std::string base = full.substr(0, full.size() - suffix.size());

        DiGraphCsr<K, O> g;
        g.edgeKeys = readParquetColumn<K>(base + ".indices.parquet", "indices");
        g.offsets = readParquetColumn<O>(base + ".indptr.parquet", "indptr");
        return g;
    }

    default:
        throw std::runtime_error("buildGraph: unknown format");
    }
}

template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildGraph(const GraphDescriptor &gd, const NodeDescriptor *nd)
{
    NodeMap<K> nm;
    return buildGraph<K, O>(gd, nd, nm);
}

// Read a label file (one label per line) into a vector indexed by compact id.
template <class L = int32_t>
std::vector<L> buildLabelMap(const std::string &labels_path, size_t N, const ParseOptions &opts)
{
    static_assert(std::is_arithmetic_v<L>, "label type must be arithmetic; string labels not yet supported");

    MmapFile mf(labels_path);
    const char *p = mf.data ? mf.data : nullptr;
    const char *end = p ? p + mf.size : nullptr;

    for (size_t i = 0; i < opts.skip_rows && p < end; ++i)
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
        if (*p == opts.comment_char)
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
