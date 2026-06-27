#pragma once

#include "graph_io.h"

#include <robin_hood.h>

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

// ─── NodeMap ─────────────────────────────────────────────────────────────────
//
// Maps a raw node id (as it appears in the edge/node files, after subtracting
// opts.base_index) to a compact id in [0, N). Three representations, chosen at
// build time by how dense the raw id space is:
//
//   Dense — identity. No node file given; the raw id *is* the compact id and N
//           is discovered while scanning edges. No memory, no lookup.
//   Array — flat remap[]. Raw ids are "mostly compact but offset": they span a
//           range only modestly larger than N. remap[raw - min_id] = compact,
//           with INVALID_ID marking gaps. One subtract + one load per lookup.
//   Hash  — robin_hood map. Fallback when the raw id span dwarfs N (e.g. true
//           64-bit ids), where an array would waste too much memory.
//
// find(raw) returns the compact id, or INVALID_ID if raw is unknown.
// The Array/Hash representations are built from a NodeDescriptor; the compact id
// equals the row's position in the node file (NOT sorted — file order is kept).

template <class K = uint32_t> struct NodeMap
{
    static constexpr K INVALID_ID = std::numeric_limits<K>::max();

    enum class Mode
    {
        Dense,
        Array,
        Hash
    };

    Mode mode = Mode::Hash;
    K N = 0;

    // Array mode.
    K min_id = 0;
    std::vector<K> remap; // remap[raw - min_id] = compact, or INVALID_ID for a gap

    // Hash mode.
    robin_hood::unordered_flat_map<K, K> map;

    // Borrowed from the NodeDescriptor's MmapFile (must outlive this NodeMap).
    // line_offsets[compact] = byte offset of that node's row; used by getRow.
    // Empty in dense mode.
    std::vector<size_t> line_offsets;
    const char *file_data = nullptr;
    size_t file_size = 0;

    // The first non-data line of the node file (typically the column-name header,
    // e.g. "node_id,name,weight"). Captured by buildNodeMap so writeNodelist can
    // reproduce it verbatim. Empty when there is no header (dense mode, or a file
    // that starts directly with data).
    std::string header_row;

    NodeMap() = default; // Hash mode, empty (also the METIS/parquet placeholder).
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
        return INVALID_ID; // unreachable
    }

    // Return the verbatim node-file row for a compact id (no trailing newline),
    // as a string_view into the mmap'd file. Empty in dense mode / out of range.
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
//
// Scan a node file once, assigning compact ids in file order. Then pick Array or
// Hash based on how dense the raw id space is: Array when the id span is within
// MAX_REMAP_SPAN_RATIO × N (cheap, cache-friendly), Hash otherwise.

template <class K = uint32_t> NodeMap<K> buildNodeMap(const NodeDescriptor &nd)
{
    // Array mode is used while  (max_id - min_id + 1) <= ratio * N
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

    // If skip_rows > 0 the caller is telling us the first line(s) are a header.
    // Capture the very first skipped line verbatim so writeNodelist can re-emit it.
    if (nd.opts.skip_rows > 0 && p < end)
    {
        const char *hl = p;
        skipLine();
        const char *row_end = p > hl ? p - 1 : hl; // point at the '\n' we just passed
        if (row_end > hl && *(row_end - 1) == '\r')
            --row_end;
        nm.header_row.assign(hl, row_end);
        for (size_t i = 1; i < nd.opts.skip_rows && p < end; ++i)
            skipLine();
    }

    // Single pass: collect normalised ids + row offsets in file order.
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
            // First non-blank, non-comment line that isn't a valid integer: it's
            // an implicit header (e.g. "node_id,name,weight"). Capture it once.
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
        nm.mode = NodeMap<K>::Mode::Hash; // empty graph; find() always misses
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

// ─── forEachValidEdge ─────────────────────────────────────────────────────────
//
// The single per-edge parse+validate hot path, shared by every CSR build pass.
// For each line in `blk` it parses (u_raw, v_raw), applies base_index, maps both
// endpoints through the NodeMap, drops unknown endpoints, and invokes
// cb(u_compact, v_compact).

template <class K, class Fn>
inline void forEachValidEdge(std::string_view blk, const NodeMap<K> &nm, const ParseOptions &opts, Fn &&cb)
{
    const uint64_t bias = opts.base_index;
    auto fb = [&](int64_t ri, int64_t rj, double) {
        auto bi = static_cast<uint64_t>(ri), bj = static_cast<uint64_t>(rj);
        if (bi < bias || bj < bias)
            return;
        K u = nm.find(static_cast<K>(bi - bias));
        K v = nm.find(static_cast<K>(bj - bias));
        if (u == NodeMap<K>::INVALID_ID || v == NodeMap<K>::INVALID_ID)
            return;
        if (!opts.keep_self_loops && u == v)
            return;
        cb(u, v);
    };

    switch (opts.sep)
    {
    case ',':
        switch (opts.comment_char)
        {
        case '#':
            readEdgelistFormatDoChecked<false, 0, ',', '#'>(blk, false, fb);
        case '%':
            readEdgelistFormatDoChecked<false, 0, ',', '%'>(blk, false, fb);
        }
    case '\t':
        switch (opts.comment_char)
        {
        case '#':
            readEdgelistFormatDoChecked<false, 0, '\t', '#'>(blk, false, fb);
        case '%':
            readEdgelistFormatDoChecked<false, 0, '\t', '%'>(blk, false, fb);
        }
    case ' ':
        switch (opts.comment_char)
        {
        case '#':
            readEdgelistFormatDoChecked<false, 0, '#', '#'>(blk, false, fb);
        case '%':
            readEdgelistFormatDoChecked<false, 0, '#', '%'>(blk, false, fb);
        }
    }
}

// ─── HugeArray ────────────────────────────────────────────────────────────────
//
// 2 MB-aligned scratch buffer advised for transparent huge pages. The big random-
// access arrays in the CSR build touch far more memory than the TLB can map with
// 4 KB pages, so backing them with 2 MB pages cuts page-table-walk overhead.
// Memory is left UNINITIALISED; the caller fills it in parallel so pages land on
// the writing core's NUMA node (first-touch placement).

template <class T> struct HugeArray
{
    T *p = nullptr;
    explicit HugeArray(size_t count)
    {
        if (posix_memalign(reinterpret_cast<void **>(&p), size_t{1} << 21, count * sizeof(T)) != 0)
            throw std::bad_alloc();
#ifdef MADV_HUGEPAGE
        madvise(p, count * sizeof(T), MADV_HUGEPAGE);
#endif
    }
    ~HugeArray()
    {
        std::free(p);
    }
    HugeArray(const HugeArray &) = delete;
    HugeArray &operator=(const HugeArray &) = delete;
    T *data() const
    {
        return p;
    }
};

inline void adviseHugePages(void *p, size_t bytes)
{
#ifdef MADV_HUGEPAGE
    if (p && bytes)
        madvise(p, bytes, MADV_HUGEPAGE);
#else
    (void)p;
    (void)bytes;
#endif
}

// ─── forEachValidEdgeStripe ───────────────────────────────────────────────────
//
// Drive forEachValidEdge over thread t's stripe of the file. The file is cut into
// fixed CSR_BLOCK_BYTES line-aligned blocks (a tiling: every line lands in exactly
// one block); thread t owns blocks t, t+T, t+2T, … . Round-robin assignment
// interleaves dense and sparse regions across threads so one heavy region can't
// stall a whole pass. The assignment depends only on (t, T), so a vertex's edges
// from a given block always go through the same per-thread cursor in both passes.

inline constexpr size_t CSR_BLOCK_BYTES = 1u << 20; // 1 MB

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

// ─── buildCSRFromCSV ──────────────────────────────────────────────────────────
//
// Two-pass, atomic-free CSR build from a CSV edge list. Works for any thread
// count: T == 1 is just the striped algorithm with a single stripe.
//
// The file is cut into 1 MB line-aligned blocks, assigned round-robin to threads.
// Each thread keeps its OWN row in the degree table, so no two threads write the
// same counter — no atomics. After pass 1 we know, per vertex v, exactly how many
// edges each thread contributes, so each thread gets a private, disjoint slice of
// v's adjacency region. Pass 2 then scatters with plain stores, again atomic-free.
//
//   Pass 1:  tdeg[t·N + v] = # endpoints at v contributed by thread t's blocks.
//   Offsets: offsets[v] = prefix sum of total degree; then overwrite tdeg in
//            place so tdeg[t·N + v] becomes thread t's write cursor into v.
//   Pass 2:  edgeKeys[tdeg[t·N + u]++] = v   (and symmetrically for v).
//
// The degree/cursor table is one flat T × N buffer (row-major by thread, so each
// thread's row is disjoint — no false sharing). It is huge-page-advised and
// first-touched in parallel. Scratch memory is T × N × sizeof(O).

template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildCSRFromCSV(std::string_view data, NodeMap<K> &nm, const ParseOptions &opts)
{
    // The GVEL parser natively handles '#' and '%' comments only.
    if (opts.comment_char != '#' && opts.comment_char != '%')
        throw std::runtime_error("comment_char: not yet implemented for chars other than '#' and '%'");

    const int T = opts.num_threads > 1 ? static_cast<int>(opts.num_threads) : 1;
    const size_t nblocks = (data.size() + CSR_BLOCK_BYTES - 1) / CSR_BLOCK_BYTES;

    // Dense mode (no node file): discover N = max(compact id) + 1 first, since we
    // must size the degree table before counting.
    if (nm.isDense() && nm.N == 0)
    {
        std::vector<K> tmax(T, K{});
#pragma omp parallel for num_threads(T) schedule(static)
        for (int t = 0; t < T; ++t)
        {
            K m = K{};
            forEachValidEdgeStripe(data, t, T, nblocks, nm, opts, [&](K u, K v) { m = std::max({m, u, v}); });
            tmax[t] = m;
        }
        K N = K{};
        for (int t = 0; t < T; ++t)
            N = std::max(N, tmax[t]);
        nm.N = static_cast<K>(N + 1);
    }
    const K N = nm.N;
    const size_t TN = static_cast<size_t>(T) * N;

    // Flat T × N degree/cursor table: row t is tdeg[t·N .. (t+1)·N).
    HugeArray<O> tdeg(TN);
    O *td = tdeg.data();

    // First-touch zero in parallel: thread t zeroes (and thus places) its own row.
    // Static schedule gives iteration t to the same physical thread here and in the
    // passes below, so the row is local to the core that uses it.
#pragma omp parallel for num_threads(T) schedule(static)
    for (int t = 0; t < T; ++t)
        std::memset(td + static_cast<size_t>(t) * N, 0, static_cast<size_t>(N) * sizeof(O));

    // ── Pass 1: per-thread degree counts (no shared writes) ────────────────────
#pragma omp parallel for num_threads(T) schedule(static)
    for (int t = 0; t < T; ++t)
    {
        O *deg = td + static_cast<size_t>(t) * N;
        forEachValidEdgeStripe(data, t, T, nblocks, nm, opts, [&](K u, K v) {
            ++deg[u];
            ++deg[v];
        });
    }

    // ── Offsets = prefix sum of total per-vertex degree ────────────────────────
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

    // ── Turn tdeg into per-thread write cursors (in place) ─────────────────────
    // cursor[t·N + v] = offsets[v] + sum over threads before t of their degree at v.
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

    // ── Pass 2: scatter into disjoint slices (no shared writes) ────────────────
    // Pass 2 writes every slot exactly once, so edgeKeys needs no zero-init value;
    // huge-page advice helps the random scatter regardless of who faults the pages.
    g.edgeKeys.resize(static_cast<size_t>(total));
    adviseHugePages(g.edgeKeys.data(), g.edgeKeys.size() * sizeof(K));
#pragma omp parallel for num_threads(T) schedule(static)
    for (int t = 0; t < T; ++t)
    {
        O *cur = td + static_cast<size_t>(t) * N;
        forEachValidEdgeStripe(data, t, T, nblocks, nm, opts, [&](K u, K v) {
            g.edgeKeys[cur[u]++] = v;
            g.edgeKeys[cur[v]++] = u;
        });
    }

    return g;
}

// ─── buildGraphFromMETIS ─────────────────────────────────────────────────────
//
// Parses a METIS adjacency-list file in a single pass. Header line "N M" gives
// both counts up front, so we allocate the full CSR and fill it in one scan —
// no two-pass needed. Lines 1..N hold space-separated 1-indexed neighbor ids;
// comment lines starting with '%' or opts.comment_char are skipped anywhere.

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
    (void)r2;
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
// Handles both uint32 and uint64 stored columns. Arrow headers are only needed
// here, so they are pulled in just above the function.

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

// ─── buildGraph ──────────────────────────────────────────────────────────────

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
            data = (nl != std::string_view::npos) ? data.substr(nl + 1) : std::string_view{};
        }
        // nd == nullptr → dense (identity) mode, N discovered during scan.
        // nd != nullptr → Array/Hash mode, N comes from the node map.
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

// ─── buildLabelMap ────────────────────────────────────────────────────────────
//
// Reads a label file (one label per line) into a vector<L> indexed by compact id.
// Respects skip_rows and comment_char from opts. L must be arithmetic.

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
