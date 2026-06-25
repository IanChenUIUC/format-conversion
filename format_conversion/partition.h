#pragma once

#include "graph_read.h"
#include "graph_write.h"

// TODO: reduce the number of arguments with a Reader and Writer class?
inline void partition_graph(const std::string &edges_file, const std::string &nodes_file,
                            const std::string &labels_file, const ParseOptions &opts, EdgesFormat input_fmt,
                            const std::string &output_dir)
{
}
