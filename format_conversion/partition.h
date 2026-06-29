#pragma once

#include "graph_read.h"
#include "graph_write.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

// Write nodes.csv for one partition label: the node list's header followed by each
// vertex's verbatim source row (or its id, in dense mode), in local-id order.

template <class K> void writeNodelist(const std::string &path, const std::vector<K> &verts, const NodeMap<K> &nm)
{
    std::ofstream f(path);
    if (!f)
        throw std::runtime_error("Cannot create nodelist: " + path);

    // Re-emit the source header, or a minimal one if the input had none.
    if (!nm.header_row.empty())
        f << nm.header_row << '\n';
    else
        f << "node_id\n";

    for (K u : verts)
    {
        auto row = nm.getRow(u);
        if (!row.empty())
            f.write(row.data(), row.size());
        else
            f << u;
        f << '\n';
    }
}

// Extract sub-CSRs for a batch of labels in two passes. Local ids follow each
// label's vertex order in label_verts. See DESIGN.md for the memory model.

template <class K, class O, class L>
robin_hood::unordered_flat_map<L, DiGraphCsr<K, O>> extractSubgraphs(
    const DiGraphCsr<K, O> &g, const std::vector<L> &labels,
    const robin_hood::unordered_flat_map<L, std::vector<K>> &label_verts, const std::vector<L> &batch)
{
    static constexpr K INVALID = std::numeric_limits<K>::max();
    K N = static_cast<K>(g.span());

    // local_id[u] = position of u in its label's vertex list (INVALID if not in batch).
    std::vector<K> local_id(N, INVALID);
    {
        robin_hood::unordered_flat_map<L, K> count;
        for (const L &lab : batch)
            count[lab] = K{};
        for (K u = 0; u < N; ++u)
        {
            auto it = count.find(labels[u]);
            if (it != count.end())
                local_id[u] = it->second++;
        }
    }

    // Offsets start at zero; they double as degree arrays in pass 1.
    robin_hood::unordered_flat_map<L, DiGraphCsr<K, O>> result;
    for (const L &lab : batch)
    {
        K N_L = static_cast<K>(label_verts.at(lab).size());
        result[lab].offsets.assign(N_L + 1, O{});
    }

    // Pass 1: count intra-label degrees.
    for (K u = 0; u < N; ++u)
    {
        auto it = result.find(labels[u]);
        if (it == result.end())
            continue;
        K lu = local_id[u];
        const L &lu_label = labels[u];
        g.forEachEdgeKey(u, [&](K v) {
            if (labels[v] == lu_label)
                ++it->second.offsets[lu];
        });
    }

    // Prefix sums → offsets; allocate edgeKeys.
    robin_hood::unordered_flat_map<L, std::vector<O>> write_pos;
    for (auto &[lab, sub] : result)
    {
        K N_L = static_cast<K>(label_verts.at(lab).size());
        O total = O{};
        for (K u = 0; u < N_L; ++u)
        {
            O deg = sub.offsets[u];
            sub.offsets[u] = total;
            total += deg;
        }
        sub.offsets[N_L] = total;
        sub.edgeKeys.resize(static_cast<size_t>(total));
        write_pos[lab].assign(sub.offsets.begin(), sub.offsets.begin() + N_L);
    }

    // Pass 2: scatter edges into sub-CSRs.
    for (K u = 0; u < N; ++u)
    {
        auto it = result.find(labels[u]);
        if (it == result.end())
            continue;
        K lu = local_id[u];
        const L &lu_label = labels[u];
        auto &wp = write_pos[lu_label];
        g.forEachEdgeKey(u, [&](K v) {
            if (labels[v] == lu_label)
                it->second.edgeKeys[wp[lu]++] = local_id[v];
        });
    }

    return result;
}

// Partition a graph by per-node label, writing a sub-graph + node list per label.
// batch_size caps how many sub-CSRs are materialised at once (0 = all labels);
// memory scales with it. See DESIGN.md.

template <class K = uint32_t, class O = uint64_t, class L = int32_t>
void partition_graph(const GraphDescriptor &input, const NodeDescriptor *nodes, const std::string &labels_path,
                     const ParseOptions &label_opts, const std::string &output_dir, EdgesFormat output_fmt,
                     size_t batch_size = std::numeric_limits<size_t>::max())
{
    // 1. Build graph + NodeMap.
    NodeMap<K> nm;
    auto g = buildGraph<K, O>(input, nodes, nm);
    K N = static_cast<K>(g.span());

    // 2. Load labels.
    auto labels = buildLabelMap<L>(labels_path, N, label_opts);

    // 3. Build label_verts[L] = compact ids for label L, in compact-id order.
    robin_hood::unordered_flat_map<L, std::vector<K>> label_verts;
    for (K u = 0; u < N; ++u)
        label_verts[labels[u]].push_back(u);

    std::vector<L> unique_labels;
    unique_labels.reserve(label_verts.size());
    for (const auto &[lab, _] : label_verts)
        unique_labels.push_back(lab);
    std::sort(unique_labels.begin(), unique_labels.end());

    std::filesystem::create_directories(output_dir);

    // 4. Extract and write in batches.
    for (size_t i = 0; i < unique_labels.size(); i += batch_size)
    {
        size_t end_idx = std::min(i + batch_size, unique_labels.size());
        std::vector<L> batch(unique_labels.begin() + i, unique_labels.begin() + end_idx);

        auto sub_map = extractSubgraphs<K, O, L>(g, labels, label_verts, batch);
        for (const auto &[label, sub_g] : sub_map)
        {
            auto label_dir = std::filesystem::path(output_dir) / std::to_string(label);
            std::filesystem::create_directories(label_dir);

            writeGraph(sub_g, (label_dir / "graph").string(), output_fmt, input.opts);
            writeNodelist((label_dir / "nodes.csv").string(), label_verts.at(label), nm);
        }
    }
}
