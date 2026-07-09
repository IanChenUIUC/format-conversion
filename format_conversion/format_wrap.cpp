#include "convert.h"
#include "partition.h"

#include <filesystem>
#include <memory>
#include <optional>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;

PYBIND11_MODULE(format, m)
{
    // ── EdgesFormat ──────────────────────────────────────────────────────────

    py::enum_<EdgesFormat>(m, "EdgesFormat")
        .value("CSV_EDGELIST", EdgesFormat::CSV_EDGELIST)
        .value("METIS", EdgesFormat::METIS)
        .value("CSR_PARQUET", EdgesFormat::CSR_PARQUET)
        .export_values();

    // ── ParseOptions ─────────────────────────────────────────────────────────

    py::class_<ParseOptions>(m, "ParseOptions")
        .def(py::init<>())
        .def_readwrite("sep", &ParseOptions::sep)
        .def_readwrite("comment_char", &ParseOptions::comment_char)
        .def_readwrite("skip_rows", &ParseOptions::skip_rows)
        .def_readwrite("num_threads", &ParseOptions::num_threads)
        .def_readwrite("base_index", &ParseOptions::base_index)
        .def_readwrite("keep_self_loops", &ParseOptions::keep_self_loops)
        .def_readwrite("sort_neighbors", &ParseOptions::sort_neighbors)
        .def_readwrite("use_u64_indices", &ParseOptions::use_u64_indices)
        .def_readwrite("directed", &ParseOptions::directed);

    // ── NodeDescriptor ───────────────────────────────────────────────────────

    py::class_<NodeDescriptor, std::shared_ptr<NodeDescriptor>>(m, "NodeDescriptor")
        .def(py::init([](std::filesystem::path path, ParseOptions opts) {
                 return std::make_shared<NodeDescriptor>(path, std::move(opts));
             }),
             py::arg("path"), py::arg_v("opts", ParseOptions{}, "ParseOptions()"));

    // ── GraphDescriptor ──────────────────────────────────────────────────────

    py::class_<GraphDescriptor, std::shared_ptr<GraphDescriptor>>(m, "GraphDescriptor")
        .def(py::init([](std::filesystem::path path, EdgesFormat fmt, ParseOptions opts) {
                 return std::make_shared<GraphDescriptor>(path, fmt, std::move(opts));
             }),
             py::arg("path"), py::arg("fmt"), py::arg_v("opts", ParseOptions{}, "ParseOptions()"));

    // ── convert ──────────────────────────────────────────────────────────────

    m.def(
        "convert",
        [](std::shared_ptr<GraphDescriptor> input,
           std::shared_ptr<NodeDescriptor> nodes, // None → dense mode
           std::filesystem::path output_path, EdgesFormat output_fmt,
           std::optional<ParseOptions> output_opts) { // None → reuse input options
            convert_graph(*input, nodes.get(), output_path.string(), output_fmt, output_opts ? &*output_opts : nullptr);
        },
        py::arg("input"), py::arg("nodes") = py::none(), py::arg("output_path"), py::arg("output_fmt"),
        py::arg("output_opts") = py::none(), py::call_guard<py::gil_scoped_release>());

    m.def(
        "convert",
        [](std::shared_ptr<GraphDescriptor> input, std::shared_ptr<NodeDescriptor> nodes,
           const std::vector<py::tuple> &outputs) {
            std::vector<OutputSpec> specs;
            specs.reserve(outputs.size());
            for (const auto &t : outputs)
            {
                if (t.size() != 2 && t.size() != 3)
                    throw std::runtime_error("each output must be (path, fmt) or (path, fmt, opts)");
                std::string path = py::cast<std::filesystem::path>(t[0]).string();
                EdgesFormat fmt = py::cast<EdgesFormat>(t[1]);
                std::optional<ParseOptions> oo;
                if (t.size() == 3 && !t[2].is_none())
                    oo = py::cast<ParseOptions>(t[2]);
                specs.emplace_back(std::move(path), fmt, std::move(oo));
            }
            {
                py::gil_scoped_release rel;
                convert_graph(*input, nodes.get(), specs);
            }
        },
        py::arg("input"), py::arg("nodes") = py::none(), py::arg("outputs"));

    // ── partition ────────────────────────────────────────────────────────────

    m.def(
        "partition",
        [](std::shared_ptr<GraphDescriptor> input, std::shared_ptr<NodeDescriptor> nodes,
           std::filesystem::path labels_path, std::filesystem::path output_dir, EdgesFormat output_fmt,
           ParseOptions label_opts, size_t batch_size) {
            partition_graph(*input, nodes.get(), labels_path.string(), label_opts, output_dir.string(), output_fmt,
                            batch_size);
        },
        py::arg("input"), py::arg("nodes") = py::none(), py::arg("labels_path"), py::arg("output_dir"),
        py::arg("output_fmt"), py::arg_v("label_opts", ParseOptions{}, "ParseOptions()"),
        py::arg("batch_size") = std::numeric_limits<size_t>::max(), py::call_guard<py::gil_scoped_release>());
}
