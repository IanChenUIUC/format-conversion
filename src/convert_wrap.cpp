#include "convert.h"
#include <filesystem>
#include <pybind11/pybind11.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;

PYBIND11_MODULE(_core, m)
{
    py::class_<ConvertOptions>(m, "ConvertOptions")
        .def(py::init<>())
        .def_readwrite("sep", &ConvertOptions::sep)
        .def_readwrite("node_header", &ConvertOptions::node_header)
        .def_readwrite("edge_header", &ConvertOptions::edge_header)
        .def_readwrite("weighted", &ConvertOptions::weighted)
        .def_readwrite("skip_loops", &ConvertOptions::skip_loops)
        .def_readwrite("type", &ConvertOptions::type);

    m.def(
        "convert",
        [](const std::filesystem::path &nodes_file, const std::filesystem::path &edges_file,
           const std::filesystem::path &output_file, const ConvertOptions &opts) {
            convert_graph(nodes_file.string(), edges_file.string(), output_file.string(), opts);
        },
        py::arg("nodes_file"), py::arg("edges_file"), py::arg("output_file"), py::arg("opts") = ConvertOptions{},
        py::call_guard<py::gil_scoped_release>());
}
