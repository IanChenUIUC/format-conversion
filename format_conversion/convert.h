#pragma once

#include "graph_read.h"
#include "graph_write.h"

#include <optional>
#include <tuple>
#include <vector>

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

// One output target for convert_graph_multi: (path, format, per-output options).
// A nullopt options field inherits input.opts (same rule as convert_graph).
using OutputSpec = std::tuple<std::string, EdgesFormat, std::optional<ParseOptions>>;

// Read/build a graph ONCE and write it to several outputs, each with its own
// format and (optional) options. Avoids re-parsing the input per output.
template <class K = uint32_t, class O = uint64_t>
void convert_graph(const GraphDescriptor &input, const NodeDescriptor *nodes, const std::vector<OutputSpec> &outputs)
{
    // Pre-validate every output before touching the filesystem.
    for (const auto &spec : outputs)
    {
        const auto &oo = std::get<2>(spec);
        const ParseOptions &out = oo ? *oo : input.opts;
        if (std::get<1>(spec) == METIS && out.directed)
            throw std::runtime_error("METIS output is undirected-only; directed=true is not supported for METIS "
                                     "(use CSV_EDGELIST or CSR_PARQUET for directed graphs)");
    }

    DiGraphCsr<K, O> g = buildGraph<K, O>(input, nodes);
    if (input.opts.sort_neighbors)
        sortNeighbors(g, static_cast<int>(input.opts.num_threads));

    for (const auto &spec : outputs)
    {
        const auto &oo = std::get<2>(spec);
        const ParseOptions &out = oo ? *oo : input.opts;
        writeGraph(g, std::get<0>(spec), std::get<1>(spec), out);
    }
}
