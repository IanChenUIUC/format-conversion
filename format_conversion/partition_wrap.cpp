#include "partition.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;

PYBIND11_MODULE(partition, m)
{
    m.doc() = "Parallel graph filter C++ extension";
    m.def(
        "partition",
        [](const std::filesystem::path &nodes_file, const std::filesystem::path &labels_file,
           const std::filesystem::path &metis_file, const std::filesystem::path &output_file) {
            partition(nodes_file.string(), labels_file.string(), metis_file.string(), output_file.string());
        },
        "Filter a METIS graph into label-specific sub-edgelists", py::arg("nodelist_path"), py::arg("labels_path"),
        py::arg("metis_path"), py::arg("out_dir"), py::call_guard<py::gil_scoped_release>());
}
