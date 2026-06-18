#include "partition.h"
#include <pybind11/pybind11.h>

namespace py = pybind11;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Parallel graph filter C++ extension";
    m.def("partition", &partition, "Filter a METIS graph into label-specific sub-edgelists", py::arg("nodelist_path"),
          py::arg("labels_path"), py::arg("metis_path"), py::arg("out_dir"), py::call_guard<py::gil_scoped_release>());
}
