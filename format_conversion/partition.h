#pragma once

#include "graph_read.h"
#include "graph_write.h"

#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>

template <class K = uint32_t, class O = uint64_t, class L = int32_t>
void partition_graph(const GraphDescriptor &input, const NodeDescriptor *nodes, const std::string &labels_path,
                     const ParseOptions &label_opts, const std::string &output_dir, EdgesFormat output_fmt,
                     size_t batch_size = std::numeric_limits<size_t>::max())
{
    throw std::runtime_error("partition_graph: not implemented");
}
