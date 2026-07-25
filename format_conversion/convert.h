#pragma once

#include "graph_read.h"
#include "graph_write.h"

#include <string>
#include <vector>

// METIS cannot represent a graph stored as arcs only. Whether the build produces
// one is a property of the read spec, so every target can be checked before any
// file is written.
inline void requireMetisTargetsSymmetric(const GraphDescriptor &input, const GraphSpec &out_spec)
{
    if (std::holds_alternative<MetisWrite>(out_spec) && !readSymmetric(input.spec))
        throw std::runtime_error("METIS output is undirected-only; the input is read with directed=true "
                                 "(use CsvEdgelist.Write or CsrParquet.Write for directed graphs)");
}

// Convert a graph between formats. Optionally remaps ids through `nodes` and sorts
// adjacency lists; both honour num_threads.
template <class K = uint32_t, class O = uint64_t>
void convert_graph(const GraphDescriptor &input, const GraphDescriptor &output, const NodeDescriptor *nodes,
                   size_t num_threads, bool sort_neighbors)
{
    requireSide(input, true, "convert");
    requireSide(output, false, "convert");
    requireMetisTargetsSymmetric(input, output.spec);

    BuiltGraph<K, O> bg = buildGraph<K, O>(input, nodes, num_threads);
    if (sort_neighbors)
        sortNeighbors(bg.g, static_cast<int>(num_threads));
    writeGraph(bg, output.path, output.spec, num_threads);
}

// Read/build a graph ONCE and write it to several outputs. Every target is
// validated before the first byte is written, so a bad one is all-or-nothing.
template <class K = uint32_t, class O = uint64_t>
void convert_graph(const GraphDescriptor &input, const std::vector<GraphDescriptor> &outputs,
                   const NodeDescriptor *nodes, size_t num_threads, bool sort_neighbors)
{
    requireSide(input, true, "convert");
    for (const auto &out : outputs)
    {
        requireSide(out, false, "convert");
        requireMetisTargetsSymmetric(input, out.spec);
    }

    BuiltGraph<K, O> bg = buildGraph<K, O>(input, nodes, num_threads);
    if (sort_neighbors)
        sortNeighbors(bg.g, static_cast<int>(num_threads));

    for (const auto &out : outputs)
        writeGraph(bg, out.path, out.spec, num_threads);
}
