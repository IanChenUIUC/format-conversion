#include "convert.h"
#include "partition.h"

#include <filesystem>
#include <memory>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;

namespace
{

// Each format is a non-constructible holder class scoping its Read/Write pair.
template <class Tag> py::class_<Tag> formatHolder(py::module_ &m, const char *name)
{
    return py::class_<Tag>(m, name);
}

struct CsvEdgelistTag
{
};
struct MetisTag
{
};
struct CsrParquetTag
{
};
struct EdgelistParquetTag
{
};
struct NodelistTag
{
};
struct LabelsTag
{
};

} // namespace

PYBIND11_MODULE(format, m)
{
    py::register_exception_translator([](std::exception_ptr p) {
        try
        {
            if (p)
                std::rethrow_exception(p);
        }
        catch (const NotImplemented &e)
        {
            py::set_error(PyExc_NotImplementedError, e.what());
        }
    });

    // ── Format specs ─────────────────────────────────────────────────────────

    auto csv = formatHolder<CsvEdgelistTag>(m, "CsvEdgelist");
    py::class_<CsvEdgelistRead>(csv, "Read")
        .def(py::init([](char sep, char comment_char, size_t skip_rows, uint64_t base_index, bool keep_self_loops,
                         bool directed) {
                 return CsvEdgelistRead{sep, comment_char, skip_rows, base_index, keep_self_loops, directed};
             }),
             py::kw_only(), py::arg("sep") = ',', py::arg("comment_char") = '#', py::arg("skip_rows") = size_t{0},
             py::arg("base_index") = uint64_t{0}, py::arg("keep_self_loops") = false, py::arg("directed") = false)
        .def_readwrite("sep", &CsvEdgelistRead::sep)
        .def_readwrite("comment_char", &CsvEdgelistRead::comment_char)
        .def_readwrite("skip_rows", &CsvEdgelistRead::skip_rows)
        .def_readwrite("base_index", &CsvEdgelistRead::base_index)
        .def_readwrite("keep_self_loops", &CsvEdgelistRead::keep_self_loops)
        .def_readwrite("directed", &CsvEdgelistRead::directed);
    py::class_<CsvEdgelistWrite>(csv, "Write")
        .def(py::init([](char sep, uint64_t base_index, bool expand_symmetric) {
                 return CsvEdgelistWrite{sep, base_index, expand_symmetric};
             }),
             py::kw_only(), py::arg("sep") = ',', py::arg("base_index") = uint64_t{0},
             py::arg("expand_symmetric") = false)
        .def_readwrite("sep", &CsvEdgelistWrite::sep)
        .def_readwrite("base_index", &CsvEdgelistWrite::base_index)
        .def_readwrite("expand_symmetric", &CsvEdgelistWrite::expand_symmetric);

    auto metis = formatHolder<MetisTag>(m, "Metis");
    py::class_<MetisRead>(metis, "Read")
        .def(py::init([](char comment_char, uint64_t base_index) { return MetisRead{comment_char, base_index}; }),
             py::kw_only(), py::arg("comment_char") = '#', py::arg("base_index") = uint64_t{1})
        .def_readwrite("comment_char", &MetisRead::comment_char)
        .def_readwrite("base_index", &MetisRead::base_index);
    py::class_<MetisWrite>(metis, "Write")
        .def(py::init([](uint64_t base_index) { return MetisWrite{base_index}; }), py::kw_only(),
             py::arg("base_index") = uint64_t{1})
        .def_readwrite("base_index", &MetisWrite::base_index);

    auto csr = formatHolder<CsrParquetTag>(m, "CsrParquet");
    py::class_<CsrParquetRead>(csr, "Read")
        .def(py::init([](std::string indices_col, std::string indptr_col, uint64_t base_index, bool symmetric) {
                 return CsrParquetRead{std::move(indices_col), std::move(indptr_col), base_index, symmetric};
             }),
             py::kw_only(), py::arg("indices_col") = "indices", py::arg("indptr_col") = "indptr",
             py::arg("base_index") = uint64_t{0}, py::arg("symmetric") = true)
        .def_readwrite("indices_col", &CsrParquetRead::indices_col)
        .def_readwrite("indptr_col", &CsrParquetRead::indptr_col)
        .def_readwrite("base_index", &CsrParquetRead::base_index)
        .def_readwrite("symmetric", &CsrParquetRead::symmetric);
    py::class_<CsrParquetWrite>(csr, "Write")
        .def(py::init([](std::string indices_col, std::string indptr_col, uint64_t base_index, bool u64_indices) {
                 return CsrParquetWrite{std::move(indices_col), std::move(indptr_col), base_index, u64_indices};
             }),
             py::kw_only(), py::arg("indices_col") = "indices", py::arg("indptr_col") = "indptr",
             py::arg("base_index") = uint64_t{0}, py::arg("u64_indices") = false)
        .def_readwrite("indices_col", &CsrParquetWrite::indices_col)
        .def_readwrite("indptr_col", &CsrParquetWrite::indptr_col)
        .def_readwrite("base_index", &CsrParquetWrite::base_index)
        .def_readwrite("u64_indices", &CsrParquetWrite::u64_indices);

    auto epq = formatHolder<EdgelistParquetTag>(m, "EdgelistParquet");
    py::class_<EdgelistParquetRead>(epq, "Read")
        .def(py::init([](std::string source_col, std::string target_col, uint64_t base_index, bool keep_self_loops,
                         bool directed) {
                 return EdgelistParquetRead{std::move(source_col), std::move(target_col), base_index,
                                            keep_self_loops, directed};
             }),
             py::kw_only(), py::arg("source_col") = "source", py::arg("target_col") = "target",
             py::arg("base_index") = uint64_t{0}, py::arg("keep_self_loops") = false, py::arg("directed") = false)
        .def_readwrite("source_col", &EdgelistParquetRead::source_col)
        .def_readwrite("target_col", &EdgelistParquetRead::target_col)
        .def_readwrite("base_index", &EdgelistParquetRead::base_index)
        .def_readwrite("keep_self_loops", &EdgelistParquetRead::keep_self_loops)
        .def_readwrite("directed", &EdgelistParquetRead::directed);
    py::class_<EdgelistParquetWrite>(epq, "Write")
        .def(py::init([](std::string source_col, std::string target_col, uint64_t base_index, bool u64_ids,
                         bool expand_symmetric) {
                 return EdgelistParquetWrite{std::move(source_col), std::move(target_col), base_index, u64_ids,
                                             expand_symmetric};
             }),
             py::kw_only(), py::arg("source_col") = "source", py::arg("target_col") = "target",
             py::arg("base_index") = uint64_t{0}, py::arg("u64_ids") = false,
             py::arg("expand_symmetric") = false)
        .def_readwrite("source_col", &EdgelistParquetWrite::source_col)
        .def_readwrite("target_col", &EdgelistParquetWrite::target_col)
        .def_readwrite("base_index", &EdgelistParquetWrite::base_index)
        .def_readwrite("u64_ids", &EdgelistParquetWrite::u64_ids)
        .def_readwrite("expand_symmetric", &EdgelistParquetWrite::expand_symmetric);

    auto nodelist = formatHolder<NodelistTag>(m, "Nodelist");
    py::class_<NodelistCsv>(nodelist, "Csv")
        .def(py::init([](char comment_char, size_t skip_rows, uint64_t base_index) {
                 return NodelistCsv{comment_char, skip_rows, base_index};
             }),
             py::kw_only(), py::arg("comment_char") = '#', py::arg("skip_rows") = size_t{0},
             py::arg("base_index") = uint64_t{0})
        .def_readwrite("comment_char", &NodelistCsv::comment_char)
        .def_readwrite("skip_rows", &NodelistCsv::skip_rows)
        .def_readwrite("base_index", &NodelistCsv::base_index);

    auto labels = formatHolder<LabelsTag>(m, "Labels");
    py::class_<LabelsCsv>(labels, "Csv")
        .def(py::init([](char comment_char, size_t skip_rows) { return LabelsCsv{comment_char, skip_rows}; }),
             py::kw_only(), py::arg("comment_char") = '#', py::arg("skip_rows") = size_t{0})
        .def_readwrite("comment_char", &LabelsCsv::comment_char)
        .def_readwrite("skip_rows", &LabelsCsv::skip_rows);

    // ── Descriptors ──────────────────────────────────────────────────────────

    py::class_<NodeDescriptor, std::shared_ptr<NodeDescriptor>>(m, "NodeDescriptor")
        .def(py::init([](std::filesystem::path path, NodelistCsv spec) {
                 return std::make_shared<NodeDescriptor>(path.string(), std::move(spec));
             }),
             py::arg("path"), py::arg_v("spec", NodelistCsv{}, "Nodelist.Csv()"));

    py::class_<GraphDescriptor, std::shared_ptr<GraphDescriptor>>(m, "GraphDescriptor")
        .def(py::init([](std::filesystem::path path, GraphSpec spec) {
                 return std::make_shared<GraphDescriptor>(path.string(), std::move(spec));
             }),
             py::arg("path"), py::arg("spec"))
        .def_property_readonly("path", [](const GraphDescriptor &d) { return d.path; })
        .def_property_readonly("spec", [](const GraphDescriptor &d) { return d.spec; });

    // ── convert ──────────────────────────────────────────────────────────────

    m.def(
        "convert",
        [](std::shared_ptr<GraphDescriptor> input, std::shared_ptr<GraphDescriptor> output,
           std::shared_ptr<NodeDescriptor> nodes, // None → dense mode
           size_t num_threads, bool sort_neighbors) {
            convert_graph(*input, *output, nodes.get(), num_threads, sort_neighbors);
        },
        py::arg("input"), py::arg("output"), py::kw_only(), py::arg("nodes") = py::none(),
        py::arg("num_threads") = size_t{1}, py::arg("sort_neighbors") = false,
        py::call_guard<py::gil_scoped_release>());

    m.def(
        "convert",
        [](std::shared_ptr<GraphDescriptor> input, const std::vector<std::shared_ptr<GraphDescriptor>> &outputs,
           std::shared_ptr<NodeDescriptor> nodes, size_t num_threads, bool sort_neighbors) {
            std::vector<GraphDescriptor> targets;
            targets.reserve(outputs.size());
            for (const auto &o : outputs)
                targets.push_back(*o);
            py::gil_scoped_release rel;
            convert_graph(*input, targets, nodes.get(), num_threads, sort_neighbors);
        },
        py::arg("input"), py::arg("outputs"), py::kw_only(), py::arg("nodes") = py::none(),
        py::arg("num_threads") = size_t{1}, py::arg("sort_neighbors") = false);

    // ── partition ────────────────────────────────────────────────────────────

    m.def(
        "partition",
        [](std::shared_ptr<GraphDescriptor> input, std::filesystem::path labels_path,
           std::filesystem::path output_dir, GraphSpec output_spec, std::shared_ptr<NodeDescriptor> nodes,
           LabelsCsv label_spec, size_t num_threads, bool sort_neighbors, size_t batch_size) {
            partition_graph(*input, labels_path.string(), output_dir.string(), output_spec, nodes.get(),
                            label_spec, num_threads, sort_neighbors, batch_size);
        },
        py::arg("input"), py::arg("labels_path"), py::arg("output_dir"), py::arg("output_spec"), py::kw_only(),
        py::arg("nodes") = py::none(), py::arg_v("label_spec", LabelsCsv{}, "Labels.Csv()"),
        py::arg("num_threads") = size_t{1}, py::arg("sort_neighbors") = false,
        py::arg("batch_size") = std::numeric_limits<size_t>::max(), py::call_guard<py::gil_scoped_release>());
}
