#pragma once

#include "graph_io.h"

#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <Graph.hxx>
#include <io.hxx>
#include <robin_hood.h>

// ─── Format enum ─────────────────────────────────────────────────────────────

enum EdgesFormat
{
    CSV_EDGELIST,
    METIS,
    CSR_PARQUET
};

// ─── ParseOptions ────────────────────────────────────────────────────────────

struct ParseOptions
{
    char sep = ',';
    char comment_char = '#';
    size_t skip_rows = 0;
    size_t num_threads = 1;
    uint64_t base_index = 0;
    size_t id_column = 0;
    size_t label_column = 0;
};

// ─── Descriptors ─────────────────────────────────────────────────────────────

struct NodeDescriptor
{
    MmapFile mmap;
    ParseOptions opts;

    NodeDescriptor(const std::string &path, ParseOptions opts = {}) : mmap(path), opts(std::move(opts))
    {
    }
};

struct GraphDescriptor
{
    MmapFile mmap;
    EdgesFormat fmt;
    ParseOptions opts;

    GraphDescriptor(const std::string &path, EdgesFormat fmt, ParseOptions opts = {})
        : mmap(path), fmt(fmt), opts(std::move(opts))
    {
    }
};

// ─── NodeMap ─────────────────────────────────────────────────────────────────
//
// Two modes, selected at construction:
//   dense  — identity mapping (no heap memory). Used when no node file is given.
//   sparse — robin_hood flat map built from a NodeDescriptor.
//
// find(raw_id) returns the compact ID, or INVALID_ID if not found (sparse mode).
// In dense mode, INVALID_ID is never returned.

template <class K = uint32_t> struct NodeMap
{
    static constexpr K INVALID_ID = std::numeric_limits<K>::max();

    bool dense;
    K N = 0;
    robin_hood::unordered_flat_map<K, K> map; // empty in dense mode

    // Borrowed from the NodeDescriptor's MmapFile — NodeDescriptor must outlive NodeMap.
    // Empty in dense mode (no file).
    std::vector<size_t> line_offsets; // line_offsets[compact_id] = byte offset of that row
    const char *file_data = nullptr;
    size_t file_size = 0;

    explicit NodeMap(K n) : dense(true), N(n)
    {
    } // Dense: raw IDs are already 0-indexed.
    NodeMap() : dense(false), N(0)
    {
    } // Sparse: caller populates map and N.

    K size() const
    {
        return N;
    }
    bool isDense() const
    {
        return dense;
    }

    K find(K raw_id) const
    {
        if (dense)
            return raw_id;
        auto it = map.find(raw_id);
        return it != map.end() ? it->second : INVALID_ID;
    }

    // Return the raw CSV row for a compact ID as a string_view into the mmap'd file.
    // Returns an empty view in dense mode or if the ID is out of range.
    std::string_view getRow(K compact_id) const
    {
        if (!file_data || compact_id >= static_cast<K>(line_offsets.size()))
            return {};
        const char *start = file_data + line_offsets[compact_id];
        const char *end = file_data + file_size;
        const char *nl = static_cast<const char *>(memchr(start, '\n', end - start));
        const char *row_end = nl ? nl : end;
        if (row_end > start && *(row_end - 1) == '\r') // trim Windows \r\n
            --row_end;
        return {start, static_cast<size_t>(row_end - start)};
    }
};

// ─── buildNodeMap ─────────────────────────────────────────────────────────────

template <class K = uint32_t> NodeMap<K> buildNodeMap(const NodeDescriptor &nd)
{
    NodeMap<K> nm;
    nm.file_data = nd.mmap.data;
    nm.file_size = nd.mmap.size;

    const char *p = nd.mmap.data;
    const char *end = p + nd.mmap.size;

    for (size_t i = 0; i < nd.opts.skip_rows && p < end; ++i)
    {
        const char *nl = (const char *)memchr(p, '\n', end - p);
        p = nl ? nl + 1 : end;
    }

    K compact = 0;
    while (p < end)
    {
        const char *line_start = p; // save before stripping whitespace, for getRow
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
            const char *nl = (const char *)memchr(p, '\n', end - p);
            p = nl ? nl + 1 : end;
            continue;
        }
        K id = 0;
        auto [next, ec] = std::from_chars(p, end, id);
        if (ec == std::errc{})
            if (nm.map.emplace(id, compact).second)
            {
                nm.line_offsets.push_back(static_cast<size_t>(line_start - nd.mmap.data));
                ++compact;
            }
        const char *nl = (const char *)memchr(p, '\n', end - p);
        p = nl ? nl + 1 : end;
    }
    nm.N = compact;
    return nm;
}

// ─── buildCSRFromCSVST ────────────────────────────────────────────────────────
//
// Single-threaded two-pass CSR build from a CSV edge list.
//
// Pass 1 — degree count + N discovery (one combined scan):
//   For each edge (u, v): grow degree[] if needed (dense mode only, since N is
//   unknown until we see the max ID), then ++degree[u] and ++degree[v].
//   In sparse mode N is already fixed from the node file so no resize occurs.
//
// Pass 2 — scatter:
//   For each edge (u, v): write v into edgeKeys at write_pos[u]++ and
//   u into edgeKeys at write_pos[v]++.
//
// Note: self-loops (u == v) are always skipped for undirected CSR.

template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildCSRFromCSVST(std::string_view data, NodeMap<K> &nm, const ParseOptions &opts)
{
    // The underlying parser natively handles '#' and '%' as comment characters.
    // Custom comment chars require a different parsing strategy.
    if (opts.comment_char != '#' && opts.comment_char != '%')
        throw std::runtime_error("comment_char: not yet implemented for chars other than '#' and '%'");

    std::vector<K> degree;

    // Helper: apply base_index, validate, return compact IDs via nm.
    // Invokes cb(u, v) only for valid, non-loop edges.
    auto forEachEdge = [&](auto cb) {
        readEdgelistFormatDoChecked<false, 0>(data, /*symmetric=*/false, [&](int64_t ri, int64_t rj, double) {
            auto bi = static_cast<uint64_t>(ri), bj = static_cast<uint64_t>(rj);
            if (bi < opts.base_index || bj < opts.base_index)
                return;
            K u = nm.find(static_cast<K>(bi - opts.base_index));
            K v = nm.find(static_cast<K>(bj - opts.base_index));
            if (u == NodeMap<K>::INVALID_ID || v == NodeMap<K>::INVALID_ID)
                return;
            if (u == v)
                return; // skip self-loops
            cb(u, v);
        });
    };

    // ── Pass 1: count degrees ─────────────────────────────────────────────────
    forEachEdge([&](K u, K v) {
        K maxuv = std::max(u, v);
        if (maxuv >= static_cast<K>(degree.size()))
            degree.resize(static_cast<size_t>(maxuv) + 1, K{});
        ++degree[u];
        ++degree[v];
    });

    K N = static_cast<K>(degree.size());
    if (nm.dense)
        nm.N = N; // update now that we know the actual N

    // ── Prefix sum → offsets ──────────────────────────────────────────────────
    DiGraphCsr<K, O> g;
    g.offsets.resize(N + 1);
    O total = O{};
    for (K u = 0; u < N; ++u)
    {
        g.offsets[u] = total;
        total += degree[u];
    }
    g.offsets[N] = total;
    degree.clear();
    degree.shrink_to_fit();

    // ── Pass 2: scatter edges ─────────────────────────────────────────────────
    g.edgeKeys.resize(static_cast<size_t>(total));
    std::vector<O> write_pos(g.offsets.begin(), g.offsets.begin() + N);

    forEachEdge([&](K u, K v) {
        g.edgeKeys[write_pos[u]++] = v;
        g.edgeKeys[write_pos[v]++] = u;
    });

    return g;
}

// ─── buildGraphFromMETIS ─────────────────────────────────────────────────────
//
// Parses a METIS adjacency-list file in a single pass.
// Header line: "N M" (vertex count, undirected edge count).
// Lines 1..N: space-separated 1-indexed neighbor IDs.
// Comment lines starting with '%' or opts.comment_char are skipped anywhere.
//
// Because the header gives us both N and M upfront, we allocate the full CSR
// before reading any edges and fill it in one sequential scan — no two-pass
// needed, unlike the CSV reader.

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

    // Skip leading comment lines.
    while (p < end && isCommentStart(p))
        skipLine();

    // Parse header: N M (ignore optional format weight flag on same line).
    size_t N = 0, M = 0;
    auto r1 = std::from_chars(p, end, N);
    p = r1.ptr;
    while (p < end && (*p == ' ' || *p == '\t'))
        ++p;
    auto r2 = std::from_chars(p, end, M);
    skipLine();

    DiGraphCsr<K, O> g;
    g.offsets.resize(N + 1);
    g.edgeKeys.resize(2 * M);

    O edge_pos = O{};
    for (size_t u = 0; u < N; ++u)
    {
        // Skip any comment lines between vertex records.
        while (p < end && isCommentStart(p))
            skipLine();

        g.offsets[u] = edge_pos;

        // Parse space-separated 1-indexed neighbors on this line.
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
            g.edgeKeys[edge_pos++] = v - K{1}; // 1-indexed → 0-indexed
            p = r.ptr;
        }
        skipLine();
    }
    g.offsets[N] = edge_pos;
    return g;
}

// ─── readParquetColumn ───────────────────────────────────────────────────────
//
// Reads a single named column from a Parquet file into a std::vector<T>.
// Handles both uint32 and uint64 stored columns — useful when reading files
// that may have been written with either K or O width.
// Add more Arrow types below as needed when supporting other formats.
//
// Arrow headers are only needed here; pull them in just above the function.

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

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

// ─── buildGraph ──────────────────────────────────────────────────────────────

template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildGraph(const GraphDescriptor &gd, const NodeDescriptor *nd)
{
    NodeMap<K> nm;
    return buildGraph(gd, nd, nm);
}

template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildGraph(const GraphDescriptor &gd, const NodeDescriptor *nd, NodeMap<K> &nm)
{
    switch (gd.fmt)
    {
    case CSV_EDGELIST: {
        // skip_rows strips header/preamble lines before handing data to the parser.
        std::string_view data = gd.mmap.view();
        for (size_t i = 0; i < gd.opts.skip_rows && !data.empty(); ++i)
        {
            auto nl = data.find('\n');
            if (nl != std::string_view::npos)
                data = data.substr(nl + 1);
            else
                data = {};
        }
        // nd == nullptr → dense (identity) mode, N discovered during scan.
        // nd != nullptr → sparse mode, N comes from the node map.
        nm = nd ? buildNodeMap<K>(*nd) : NodeMap<K>(K{});
        if (gd.opts.num_threads <= 1)
            return buildCSRFromCSVST<K, O>(data, nm, gd.opts);
        throw std::runtime_error("buildGraph: multi-threaded CSV not yet implemented");
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

// ─── buildLabelMap ────────────────────────────────────────────────────────────
//
// Reads a label file (one label per line) into a vector<L> indexed by compact ID.
// Respects skip_rows and comment_char from opts.
// L must be an arithmetic type (int32_t by default).

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
