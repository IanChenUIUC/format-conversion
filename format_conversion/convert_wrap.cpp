#include "convert.h"
#include "partition.h"
#include <filesystem>
#include <pybind11/pybind11.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;

// TODO: merge into same module

PYBIND11_MODULE(format, m)
{
    py::enum_<OutputFormat>(m, "OutputFormat")
        .value("METIS", OutputFormat::METIS)
        .value("CSR", OutputFormat::CSR_PARQUET)
        .export_values();

    py::class_<ParseOptions>(m, "ParseOptions")
        .def(py::init<>())
        .def_readwrite("sep", &ParseOptions::sep)
        .def_readwrite("node_header", &ParseOptions::node_header)
        .def_readwrite("edge_header", &ParseOptions::edge_header)
        .def_readwrite("skip_loops", &ParseOptions::skip_loops);

    m.def(
        "convert",
        [](const std::filesystem::path &nodes_file, const std::filesystem::path &edges_file,
           const std::filesystem::path &output_file, const ParseOptions &opts, OutputFormat fmt) {
            convert_graph(nodes_file.string(), edges_file.string(), output_file.string(), opts, fmt);
        },
        py::arg("nodes_file"), py::arg("edges_file"), py::arg("output_file"),
        py::arg_v("opts", ParseOptions{}, "parseOptions()"), py::arg("fmt"), py::call_guard<py::gil_scoped_release>());

    // m.def(
    //     "partition",
    //     [](const std::filesystem::path &nodes_file, const std::filesystem::path &labels_file,
    //        const std::filesystem::path &metis_file, const std::filesystem::path &output_file) {
    //         partition(nodes_file.string(), labels_file.string(), metis_file.string(), output_file.string());
    //     },
    //     "Filter a METIS graph into label-specific sub-edgelists", py::arg("nodelist_path"), py::arg("labels_path"),
    //     py::arg("metis_path"), py::arg("out_dir"), py::call_guard<py::gil_scoped_release>());
}
