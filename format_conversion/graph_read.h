#pragma once

#include "graph_io.h"

#include <charconv>
#include <cstring>
#include <omp.h>
#include <vector>

#include <Graph.hxx>
#include <io.hxx>
#include <robin_hood.h>

template <class K = uint32_t> using NodeMap = robin_hood::unordered_flat_map<K, K>;

enum EdgesFormat
{
    METIS,
    CSR_PARQUET,
    CSV_EDGELIST,
};

struct ParseOptions
{
    size_t skip_edge_rows = 0; // include for any comments
    bool node_header = true;
    bool skip_loops = true;
    char sep = ',';
};

template <class K, class O>
DiGraphCsr<K, O> buildGraph(const std::string &edges_file, const std::string &nodes_file, const ParseOptions &opts,
                            EdgesFormat input_fmt)
{
    // TODO
}

// ─── buildNodeMap ────────────────────────────────────────────────────────────
//
// Mmaps the node CSV and inserts the first-column integer of each data row
// into node_map (raw_id -> compact_id). Returns N = number of distinct nodes.

template <class K = uint32_t>
size_t buildNodeMap(const std::string &path, const ParseOptions &opts, NodeMap<K> &node_map)
{
    MmapFile mf(path);
    const char *p = mf.data;
    const char *end = p + mf.size;

    if (opts.node_header)
    {
        const char *nl = (const char *)memchr(p, '\n', end - p);
        p = nl ? nl + 1 : end;
    }

    size_t N = 0;
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

        K id = 0;
        auto [next, ec] = std::from_chars(p, end, id);
        if (ec == std::errc{})
            if (node_map.emplace(id, (K)N).second)
                ++N;

        const char *nl = (const char *)memchr(p, '\n', end - p);
        p = nl ? nl + 1 : end;
    }
    return N;
}

// ─── buildCSRFromEdges ───────────────────────────────────────────────────────
//
// Two-pass parallel CSR construction over the edge CSV.
//
// Pass 1 — degree count with P=4 partitioned arrays (graph-csr-openmp
//           PARTITIONS pattern): reduces atomic contention on hub nodes 4×.
// Pass 2 — atomic-capture scatter into the pre-allocated edgeKeys array.
//
// Parsing is delegated to graph-csr-openmp's readEdgelistFormatBlock +
// readEdgelistFormatDoUnchecked, which handle comma-separated integer IDs
// by skipping non-digit characters to find each next integer.

template <class K = uint32_t, class O = uint64_t>
DiGraphCsr<K, O> buildGraphFromEdges(const std::string &edges_file, const NodeMap<K> &node_map,
                                     const ParseOptions &opts)
{
    MmapFile mf(edges_file);
    std::string_view data = mf.view();

    for (size_t i = 0; i < opts.skip_edge_rows; ++i)
    {
        const char *nl = (const char *)memchr(mf.data, '\n', mf.size);
        size_t skip = nl ? (size_t)(nl + 1 - mf.data) : mf.size;
        data = data.substr(skip);
    }

    constexpr int P = 4;
    const size_t BLOCK = 256 * 1024;

    size_t N = node_map.size();
    std::vector<std::vector<uint32_t>> pdeg(P, std::vector<uint32_t>(N, 0));

// ── Pass 1: count degrees ──
#pragma omp parallel
    {
        int p = omp_get_thread_num() % P;
#pragma omp for schedule(dynamic) nowait
        for (size_t b = 0; b < data.size(); b += BLOCK)
        {
            std::string_view block = readEdgelistFormatBlock(data, b, BLOCK);
            readEdgelistFormatDoUnchecked<false, 0>(block, /*symmetric=*/false, [&](uint64_t ru, uint64_t rv, double) {
                auto iu = node_map.find((K)ru);
                auto iv = node_map.find((K)rv);
                if (iu == node_map.end() || iv == node_map.end())
                    return;
                K u = iu->second, v = iv->second;
                if (opts.skip_loops && u == v)
                    return;
#pragma omp atomic
                ++pdeg[p][u];
#pragma omp atomic
                ++pdeg[p][v];
            });
        }
    }

#pragma omp parallel for schedule(static)
    for (size_t u = 0; u < N; ++u)
        for (int p = 1; p < P; ++p)
            pdeg[0][u] += pdeg[p][u];
    for (int p = 1; p < P; ++p)
        std::vector<uint32_t>().swap(pdeg[p]);

    // ── Exclusive prefix sum → offsets ──
    DiGraphCsr<K, O> g;
    g.offsets.resize(N + 1);
    O total = 0;
    for (size_t u = 0; u < N; ++u)
    {
        g.offsets[u] = total;
        total += pdeg[0][u];
    }
    g.offsets[N] = total;
    std::vector<uint32_t>().swap(pdeg[0]);

    g.edgeKeys.resize((size_t)total);
    std::vector<O> write_pos(g.offsets.begin(), g.offsets.begin() + N);

// ── Pass 2: scatter ──
#pragma omp parallel
    {
#pragma omp for schedule(dynamic) nowait
        for (size_t b = 0; b < data.size(); b += BLOCK)
        {
            std::string_view block = readEdgelistFormatBlock(data, b, BLOCK);
            readEdgelistFormatDoUnchecked<false, 0>(block, /*symmetric=*/false, [&](uint64_t ru, uint64_t rv, double) {
                auto iu = node_map.find((K)ru);
                auto iv = node_map.find((K)rv);
                if (iu == node_map.end() || iv == node_map.end())
                    return;
                K u = iu->second, v = iv->second;
                if (opts.skip_loops && u == v)
                    return;
                O su, sv;
#pragma omp atomic capture
                su = write_pos[u]++;
#pragma omp atomic capture
                sv = write_pos[v]++;
                g.edgeKeys[su] = v;
                g.edgeKeys[sv] = u;
            });
        }
    }

    return g;
}
