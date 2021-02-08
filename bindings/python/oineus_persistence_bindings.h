#ifndef OINEUS_OINEUS_PERSISTENCE_BINDINGS_H
#define OINEUS_OINEUS_PERSISTENCE_BINDINGS_H

#include <iostream>
#include <vector>
#include <sstream>
#include <chrono>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

namespace py = pybind11;

#include <oineus/oineus.h>

using dim_type = oineus::dim_type;

template<class Int, class Real, size_t D>
oineus::Diagrams<Real>
compute_diagrams_ls_freudenthal(py::array_t<Real> data, bool negate, bool wrap, dim_type top_d, int n_threads)
{
    using Grid = oineus::Grid<Int, Real, D>;
    using GridPoint = typename Grid::GridPoint;

    py::buffer_info data_buf = data.request();

   if (data.ndim() != D)
        throw std::runtime_error("Dimension mismatch");

    Real* pdata {static_cast<Real*>(data_buf.ptr)};

    GridPoint dims;
    for(dim_type d = 0; d < D; ++d)
        dims[d] = data.shape(d);

    Grid grid {dims, wrap, pdata};

    auto start = std::chrono::steady_clock::now();
    auto fil = grid.freudenthal_filtration(top_d, negate);
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed_fil = end - start;
    std::cerr << "filtration created in " << elapsed_fil.count() << std::endl;

    start = std::chrono::steady_clock::now();
    auto bm = fil.boundary_matrix_full();
    end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed_bm = end - start;
    std::cerr << "matrix created in " << elapsed_fil.count() << std::endl;

    start = std::chrono::steady_clock::now();
    oineus::Params params;
    params.sort_dgms = false;
    params.clearing_opt = true;
//    params.print_time = true;
    params.n_threads = n_threads;
    bm.reduce_parallel(params);
    end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed_red = end - start;
    std::cerr << "matrix reduced in " << elapsed_red.count() << std::endl;

    return bm.diagram(fil);
}

template<class Int, class Real>
void init_oineus(py::module& m, std::string suffix)
{
    using namespace pybind11::literals;

    using RealDgmPoint = oineus::DgmPoint<Real>;
    using RealDiagram = oineus::Diagrams<Real>;

    std::string dgm_point_name = "DiagramPoint" + suffix;
    std::string dgm_class_name = "Diagrams" + suffix;

    py::class_<RealDgmPoint>(m, dgm_point_name.c_str())
            .def(py::init<Real, Real>())
            .def_readwrite("birth", &RealDgmPoint::birth)
            .def_readwrite("death", &RealDgmPoint::death)
            .def("__getitem__", [](const RealDgmPoint& p, int i) {
                if (i == 0)
                    return p.birth;
                else if (i == 1)
                    return p.death;
                else
                    throw std::out_of_range("");
            })
            .def("__repr__", [](const RealDgmPoint& p) {
                std::stringstream ss;
                ss << p;
                return ss.str();
            });

    py::class_<RealDiagram>(m, dgm_class_name.c_str())
            .def(py::init<>())
            .def("in_dimension", &RealDiagram::get_diagram_in_dimension)
            .def("__getitem__", &RealDiagram::get_diagram_in_dimension)
            .def("add_point", &RealDiagram::add_point)
            .def("sort", &RealDiagram::sort)
            .def("save_as_txt", &RealDiagram::save_as_txt);

    std::string func_name;

    func_name = "compute_diagrams_ls" + suffix + "_1";
    m.def(func_name.c_str(), &compute_diagrams_ls_freudenthal<Int, Real, 1>);

    func_name = "compute_diagrams_ls" + suffix + "_2";
    m.def(func_name.c_str(), &compute_diagrams_ls_freudenthal<Int, Real, 2>);

    func_name = "compute_diagrams_ls" + suffix + "_3";
    m.def(func_name.c_str(), &compute_diagrams_ls_freudenthal<Int, Real, 3>);
}

#endif //OINEUS_OINEUS_PERSISTENCE_BINDINGS_H