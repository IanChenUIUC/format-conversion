#pragma once

#include "graph_io.h"

#include <charconv>
#include <cstring>
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
//
// Own their MmapFile (RAII). Exposed to Python via shared_ptr holders so
// pybind11 never tries to copy them.

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
// Wraps either dense (identity) or sparse (robin_hood) mode.
// One bool branch per lookup — acceptable given that alternatives complicate
// all call sites.  In dense mode no heap memory is allocated.
//
// find(raw_id) returns the compact ID, or size() as a sentinel if not found.

template <class K = uint32_t> struct NodeMap
{
    bool dense;
    K N = 0;
    robin_hood::unordered_flat_map<K, K> map; // empty in dense mode

    // Dense: raw IDs are already 0-indexed compact IDs.
    explicit NodeMap(K n) : dense(true), N(n)
    {
    }

    // Sparse: caller populates map and sets N.
    NodeMap() : dense(false), N(0)
    {
    }

    K size() const
    {
        return N;
    }
    bool isDense() const
    {
        return dense;
    }

    // Returns compact ID, or N (sentinel for "not found").
    K find(K raw_id) const
    {
        if (dense)
            return raw_id;
        auto it = map.find(raw_id);
        return it != map.end() ? it->second : N;
    }
};

// ─── buildNodeMap ─────────────────────────────────────────────────────────────
//
// Scans the node CSV (mmap'd), builds sparse NodeMap (raw_id → compact_id).
// Respects skip_rows and comment_char from opts.
// Full implementation here; used from Milestone 4 onward.

template <class K = uint32_t> NodeMap<K> buildNodeMap(const NodeDescriptor &nd)
{
    NodeMap<K> nm;
    const char *p = nd.mmap.data;
    const char *end = p + nd.mmap.size;

    // Skip header/preamble rows.
    for (size_t i = 0; i < nd.opts.skip_rows && p < end; ++i)
    {
        const char *nl = (const char *)memchr(p, '\n', end - p);
        p = nl ? nl + 1 : end;
    }

    K compact = 0;
    while (p < end)
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
                ++compact;

        const char *nl = (const char *)memchr(p, '\n', end - p);
        p = nl ? nl + 1 : end;
    }
    nm.N = compact;
    return nm;
}

// ─── buildGraph ──────────────────────────────────────────────────────────────

template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildGraph(const GraphDescriptor &gd, const NodeDescriptor *nd)
{
    throw std::runtime_error("buildGraph: not implemented");
}
