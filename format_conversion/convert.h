#pragma once

#include "graph_read.h"
#include "graph_write.h"

inline void convert_graph(const std::string &edges_file, const std::string &nodes_file, const std::string &output_file,
                          const ParseOptions &opts, EdgesFormat input_fmt, EdgesFormat output_fmt)
{
    // TODO: use different types for diffrent output formats (parquet needs 64-bit K)
    using Graph = DiGraphCsr<uint32_t, uint64_t>;
    Graph g = buildGraph<uint32_t, uint64_t>(edges_file, nodes_file, opts, input_fmt);
    writeGraph(g, output_file, output_fmt);
}
