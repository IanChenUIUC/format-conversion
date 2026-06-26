#pragma once

#include "graph_read.h"
#include "graph_write.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

// ─── writeNodelist ────────────────────────────────────────────────────────────
//
// Writes nodes.csv for one partition label.
// verts: global compact IDs belonging to this label, in local-ID order.
// nm:    NodeMap built from the input NodeDescriptor.
//   - Sparse mode (node file provided): nm.getRow(u) returns the verbatim input
//     CSV row (including any extra columns), written directly.
//   - Dense mode (no node file): nm.getRow(u) returns empty; we write u instead.
// Always writes a "node_id" header so consumers can skip it uniformly.

template <class K> void writeNodelist(const std::string &path, const std::vector<K> &verts, const NodeMap<K> &nm)
{
    std::ofstream f(path);
    if (!f)
        throw std::runtime_error("Cannot create nodelist: " + path);
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

// ─── extractSubgraphs ────────────────────────────────────────────────────────
//
// Extracts sub-CSRs for a batch of labels simultaneously in two passes.
//
// label_verts: pre-built map L → sorted vector of global compact IDs.
//              Passed in so partition_graph can reuse it across batches.
// batch:       which labels to extract in this call (a subset of label_verts).
//
// Local IDs within each sub-graph follow the ordering in label_verts[L]:
//   local_id = position of global compact ID in label_verts[L].
//
// Memory: local_id array (N × K) + write_pos cursors (N × O total across batch).
//   Peak for the edgeKeys across all D labels ≤ total intra-label edges ≤ |E|.

template <class K, class O, class L>
robin_hood::unordered_flat_map<L, DiGraphCsr<K, O>> extractSubgraphs(
    const DiGraphCsr<K, O> &g, const std::vector<L> &labels,
    const robin_hood::unordered_flat_map<L, std::vector<K>> &label_verts, const std::vector<L> &batch)
{
    static constexpr K INVALID = std::numeric_limits<K>::max();
    K N = static_cast<K>(g.span());

    // Build local_id[u] = position of u in its label's vertex list.
    // Vertices whose label is not in this batch get INVALID.
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

    // Initialise sub-CSR offsets to zero; they double as degree arrays in pass 1.
    robin_hood::unordered_flat_map<L, DiGraphCsr<K, O>> result;
    for (const L &lab : batch)
    {
        K N_L = static_cast<K>(label_verts.at(lab).size());
        result[lab].offsets.assign(N_L + 1, O{});
    }

    // ── Pass 1: count intra-label degrees ─────────────────────────────────────
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

    // ── Prefix sums → proper offsets; allocate edgeKeys ───────────────────────
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

    // ── Pass 2: scatter edges into sub-CSRs ───────────────────────────────────
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

// ─── partition_graph ─────────────────────────────────────────────────────────
//
// Full pipeline:
//   buildGraph (with NodeMap) → buildLabelMap → batch loop of extractSubgraphs
//   → writeGraph + writeNodelist per label per batch.
//
// batch_size controls how many sub-CSRs are materialised simultaneously.
// defaults to all labels at once. Memory scales linearly with batch_size;
// see extractSubgraphs for details.

template <class K = uint32_t, class O = uint64_t, class L = int32_t>
void partition_graph(const GraphDescriptor &input, const NodeDescriptor *nodes, const std::string &labels_path,
                     const ParseOptions &label_opts, const std::string &output_dir, EdgesFormat output_fmt,
                     size_t batch_size = std::numeric_limits<size_t>::max())
{
    // ── 1. Build graph + NodeMap ───────────────────────────────────────────────
    NodeMap<K> nm;
    auto g = buildGraph<K, O>(input, nodes, nm);
    K N = static_cast<K>(g.span());

    // ── 2. Load labels ────────────────────────────────────────────────────────
    auto labels = buildLabelMap<L>(labels_path, N, label_opts);

    // ── 3. Build label_verts and sorted unique label list ─────────────────────
    // label_verts[L] = global compact IDs for label L, in compact-ID order.
    // Ordering is automatic: we iterate u = 0..N-1 and push_back in order.
    robin_hood::unordered_flat_map<L, std::vector<K>> label_verts;
    for (K u = 0; u < N; ++u)
        label_verts[labels[u]].push_back(u);

    std::vector<L> unique_labels;
    unique_labels.reserve(label_verts.size());
    for (const auto &[lab, _] : label_verts)
        unique_labels.push_back(lab);
    std::sort(unique_labels.begin(), unique_labels.end());

    std::filesystem::create_directories(output_dir);

    // ── 4. Extract and write in batches of size D ─────────────────────────────
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
