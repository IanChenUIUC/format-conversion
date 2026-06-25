#pragma once

#include "graph_read.h"
#include "graph_write.h"

template <class K = uint32_t, class O = uint64_t>
void convert_graph(const GraphDescriptor &input, const NodeDescriptor *nodes, const std::string &output_path,
                   EdgesFormat output_fmt)
{
    DiGraphCsr<K, O> g = buildGraph<K, O>(input, nodes);
    writeGraph(g, output_path, output_fmt);
}
