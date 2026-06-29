#pragma once

#include "graph_read.h"
#include "graph_write.h"

// Convert a graph between formats. Optionally remaps ids through `nodes` and sorts
// adjacency lists (input.opts.sort_neighbors); both honour input.opts.num_threads.
template <class K = uint32_t, class O = uint64_t>
void convert_graph(const GraphDescriptor &input, const NodeDescriptor *nodes, const std::string &output_path,
                   EdgesFormat output_fmt)
{
    DiGraphCsr<K, O> g = buildGraph<K, O>(input, nodes);
    if (input.opts.sort_neighbors)
        sortNeighbors(g, static_cast<int>(input.opts.num_threads));
    writeGraph(g, output_path, output_fmt, input.opts);
}
