#pragma once

#include "graph_read.h"
#include "graph_write.h"

// Convert a graph between formats. Optionally remaps ids through `nodes` and sorts
// adjacency lists (input.opts.sort_neighbors); both honour input.opts.num_threads.
//
// Reading uses input.opts; writing uses output_opts when provided, else input.opts
// (so the read and write sides can differ — e.g. read comma / write tab, or emit
// uint64 CSR indices only on output). sort_neighbors is a read-side transform and
// always comes from input.opts.
template <class K = uint32_t, class O = uint64_t>
void convert_graph(const GraphDescriptor &input, const NodeDescriptor *nodes, const std::string &output_path,
                   EdgesFormat output_fmt, const ParseOptions *output_opts = nullptr)
{
    DiGraphCsr<K, O> g = buildGraph<K, O>(input, nodes);
    if (input.opts.sort_neighbors)
        sortNeighbors(g, static_cast<int>(input.opts.num_threads));
    const ParseOptions &out = output_opts ? *output_opts : input.opts;
    writeGraph(g, output_path, output_fmt, out);
}
