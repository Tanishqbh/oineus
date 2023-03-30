#ifndef OINEUS_OINEUS_PERSISTENCE_BINDINGS_H
#define OINEUS_OINEUS_PERSISTENCE_BINDINGS_H

#include <iostream>
#include <vector>
#include <sstream>
#include <functional>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

namespace py = pybind11;

#include <oineus/timer.h>
#include <oineus/oineus.h>

using dim_type = oineus::dim_type;

template<class Real>
class PyOineusDiagrams {
public:
    PyOineusDiagrams() = default;

    PyOineusDiagrams(const oineus::Diagrams<Real>& _diagrams)
            :diagrams_(_diagrams) { }

    PyOineusDiagrams(oineus::Diagrams<Real>&& _diagrams)
            :diagrams_(_diagrams) { }

    py::array_t<Real> get_diagram_in_dimension(dim_type d)
    {
        auto dgm = diagrams_.get_diagram_in_dimension(d);

        size_t arr_sz = dgm.size() * 2;
        Real* ptr = new Real[arr_sz];
        for(size_t i = 0; i < dgm.size(); ++i) {
            ptr[2 * i] = dgm[i].birth;
            ptr[2 * i + 1] = dgm[i].death;
        }

        py::capsule free_when_done(ptr, [](void* p) {
          Real* pp = reinterpret_cast<Real*>(p);
          delete[] pp;
        });

        py::array::ShapeContainer shape {static_cast<long int>(dgm.size()), 2L};
        py::array::StridesContainer strides {static_cast<long int>(2 * sizeof(Real)),
                                             static_cast<long int>(sizeof(Real))};

        return py::array_t<Real>(shape, strides, ptr, free_when_done);
    }

private:
    oineus::Diagrams<Real> diagrams_;
};

template<class Int, class Real>
using DiagramV = std::pair<PyOineusDiagrams<Real>, typename oineus::VRUDecomposition<Int>::MatrixData>;

template<class Int, class Real>
using DiagramRV = std::tuple<PyOineusDiagrams<Real>, typename oineus::VRUDecomposition<Int>::MatrixData, typename oineus::VRUDecomposition<Int>::MatrixData>;

template<class Int, class Real, size_t D>
typename oineus::Grid<Int, Real, D>
get_grid(py::array_t<Real, py::array::c_style | py::array::forcecast> data, bool wrap)
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

    return Grid(dims, wrap, pdata);
}

template<class Int, class Real, size_t D>
typename oineus::Filtration<Int, Real, Int>
get_fr_filtration(py::array_t<Real, py::array::c_style | py::array::forcecast> data, bool negate, bool wrap, dim_type max_dim, int n_threads)
{
    auto grid = get_grid<Int, Real, D>(data, wrap);
    auto fil = grid.freudenthal_filtration(max_dim, negate, n_threads);
    return fil;
}

template<class Real, size_t D>
decltype(auto) numpy_to_point_vector(py::array_t<Real, py::array::c_style | py::array::forcecast> data)
{
    using PointVector = std::vector<oineus::Point<Real, D>>;

    if (data.ndim() != 2 or data.shape(1) != D)
        throw std::runtime_error("Dimension mismatch");

    py::buffer_info data_buf = data.request();

    PointVector points(data.shape(0));

    Real* pdata {static_cast<Real*>(data_buf.ptr)};

    for(size_t i = 0; i < data.size(); ++i)
        points[i / D][i % D] = pdata[i];

    return points;
}

template<typename Int, typename Real>
typename oineus::Filtration<Int, Real, Int>
list_to_filtration(py::list data) //take a list of simplices and turn it into a filtration for oineus. The list should contain simplices in the form '[id, [boundary], filtration value]'. 
{
    using IdxVector = std::vector<Int>;
    using FiltrationSimplex = oineus::Simplex<Int, Real, Int>;
    using FiltrationSimplexVector = std::vector<FiltrationSimplex>;
    using Filtration = oineus::Filtration<Int, Real, Int>;

    int n_simps = data.size();
    std::cout << "Number of cells in the complex is: " << n_simps << std::endl;
    FiltrationSimplexVector FSV;

    for(int i = 0; i < n_simps; i++) {
        auto data_i = data[i];
        int count = 0;
        Int id;
        std::vector<Int> vertices;
        Real val;
        for(auto item: data_i) {
            if (count == 0) {
                id = item.cast<Int>();
            } else if (count == 1) {
                vertices = item.cast<std::vector<Int>>();
            } else if (count == 2) {
                val = item.cast<Real>();
            }
            count++;
        }
        std::cout << "parsed the following data. id: " << id << " val: " << val << std::endl;
        FiltrationSimplex simp_i(id, vertices, val);
        FSV.push_back(simp_i);
    }
    Filtration Filt(FSV, false, 1);

    return Filt;
}


template<typename Int, typename Real>
typename oineus::Filtration<Int, Real, Int>
get_ls_filtration(const py::list& simplices, const py::array_t<Real>& vertex_values, bool negate, int n_threads)
// take a list of simplices and a numpy array of their values and turn it into a filtration for oineus.
// The list should contain simplices, each simplex is a list of vertices,
// e.g., triangulation of one segment is [[0], [1], [0, 1]]
{
    using Filtration = oineus::Filtration<Int, Real, Int>;
    using Simplex = typename Filtration::FiltrationSimplex;
    using IdxVector = std::vector<Int>;
    using SimplexVector = std::vector<Simplex>;

    Timer timer;
    timer.reset();

    SimplexVector fil_simplices;
    fil_simplices.reserve(simplices.size());

    if (vertex_values.ndim() != 1) {
        std::cerr << "get_ls_filtration: expected 1-dimensional array in get_ls_filtration, got " << vertex_values.ndim() << std::endl;
        throw std::runtime_error("Expected 1-dimensional array in get_ls_filtration");
    }

    auto cmp = negate ? [](Real x, Real y) { return x > y; } : [](Real x, Real y) { return x < y; };

    auto vv_buf = vertex_values.request();
    Real* p_vertex_values = static_cast<Real*>(vv_buf.ptr);

    for(auto&& item : simplices) {
        IdxVector vertices = item.cast<IdxVector>();

        Int critical_vertex;
        Real critical_value = negate ? std::numeric_limits<Real>::max() : std::numeric_limits<Real>::lowest();

        for(auto v : vertices) {
            Real vv = p_vertex_values[v];
            if (cmp(critical_value, vv)) {
                critical_vertex = v;
                critical_value = vv;
            }
        }

        fil_simplices.emplace_back(vertices, critical_value, critical_vertex);
    }

//    std::cerr << "without filtration ctor, in get_ls_filtration elapsed: " << timer.elapsed_reset() << std::endl;

    return Filtration(fil_simplices, negate, n_threads);
}



template<class Int, class Real, size_t D>
typename oineus::Filtration<Int, Real, oineus::VREdge>
get_vr_filtration(py::array_t<Real, py::array::c_style | py::array::forcecast> points, dim_type max_dim, Real max_radius, int n_threads)
{
    return oineus::get_vr_filtration_bk<Int, Real, D>(numpy_to_point_vector<Real, D>(points), max_dim, max_radius, n_threads);
}

template<class Int, class Real, class L>
PyOineusDiagrams<Real>
compute_diagrams_from_fil(const oineus::Filtration<Int, Real, L>& fil, int n_threads)
{
    oineus::VRUDecomposition<Int> d_matrix {fil, false};

    oineus::Params params;

    params.sort_dgms = false;
    params.clearing_opt = true;
    params.n_threads = n_threads;

    d_matrix.reduce_parallel(params);

    return PyOineusDiagrams<Real>(d_matrix.diagram(fil));
}

template<class Int, class Real, size_t D>
typename oineus::VRUDecomposition<Int>::MatrixData
get_boundary_matrix(py::array_t<Real, py::array::c_style | py::array::forcecast> data, bool negate, bool wrap, dim_type max_dim, int n_threads)
{
    auto fil = get_fr_filtration<Int, Real, D>(data, negate, wrap, max_dim, n_threads);
    return fil.boundary_matrix_full();
}

template<class Int, class Real, size_t D>
typename oineus::VRUDecomposition<Int>::MatrixData
get_coboundary_matrix(py::array_t<Real, py::array::c_style | py::array::forcecast> data, bool negate, bool wrap, dim_type max_dim, int n_threads)
{
    auto fil = get_fr_filtration<Int, Real, D>(data, negate, wrap, max_dim, n_threads);
    auto bm = fil.boundary_matrix_full();
    return oineus::antitranspose(bm);
}

template<class Int, class Real, size_t D>
PyOineusDiagrams<Real>
compute_diagrams_ls_freudenthal(py::array_t<Real, py::array::c_style | py::array::forcecast> data, bool negate, bool wrap, dim_type max_dim, oineus::Params& params, bool include_inf_points)
{
    // for diagram in dimension d, we need (d+1)-simplices
    Timer timer;
    auto fil = get_fr_filtration<Int, Real, D>(data, negate, wrap, max_dim + 1, params.n_threads);
    auto elapsed_fil = timer.elapsed_reset();
    oineus::VRUDecomposition<Int> decmp {fil, false};
    auto elapsed_decmp_ctor = timer.elapsed_reset();

    if (params.print_time)
        std::cerr << "Filtration: " << elapsed_fil << ", decomposition ctor: " << elapsed_decmp_ctor << std::endl;

    decmp.reduce(params);

    if (params.do_sanity_check and not decmp.sanity_check())
        throw std::runtime_error("sanity check failed");
    return PyOineusDiagrams<Real>(decmp.diagram(fil, include_inf_points));
}

template<typename Int, typename Real>
class PyKerImCokDgms {
private:
    oineus::KerImCokReduced<Int, Real> KICR;

public:

    PyKerImCokDgms(oineus::KerImCokReduced<Int, Real> KICR_)
            :
            KICR(KICR_)
    {

    }

    decltype(auto) kernel()
    {
        return PyOineusDiagrams<Real>(KICR.get_kernel_diagrams());
    }

    decltype(auto) image()
    {
        return PyOineusDiagrams(KICR.get_image_diagrams());
    }

    decltype(auto) cokernel()
    {
        return PyOineusDiagrams(KICR.get_cokernel_diagrams());
    }

};

template<typename Int, typename Real>
decltype(auto) compute_kernel_image_cokernel_diagrams(py::list K_, py::list L_, py::list IdMap_,
        int n_threads = 1) //take a list of simplices and turn it into a filtration for oineus. The list should contain simplices in the form '[id, [boundary], filtration value].
{
    using IdxVector = std::vector<Int>;
    using IntSparseColumn = oineus::SparseColumn<Int>;
    using MatrixData = std::vector<IntSparseColumn>;
    using FiltrationSimplex = oineus::Simplex<Int, Real, Int>;
    using FiltrationSimplexVector = std::vector<FiltrationSimplex>;
    using Filtration = oineus::Filtration<Int, Real, Int>;
    using KerImCokReduced = oineus::KerImCokReduced<Int, Real>;
    std::cout << "======================================" << std::endl;
    std::cout << std::endl;
    std::cout << "You have called \'compute_kernel_image_cokernel_diagrams\', it takes as input a complex K, and a subcomplex L, as lists of cells in the format:" << std::endl;
    std::cout << "          [id, [boundary], filtration value]" << std::endl;
    std::cout << "and a mapping from L to K, which takes the id of a cell in L and returns the id of the cell in K, as well as an integer, telling oineus how many threads to use." << std::endl;
    std::cout << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << std::endl;

    std::cout << "------------ Importing K ------------" << std::endl;
    Filtration K = list_to_filtration<Int, Real>(K_);
    std::cout << "------------ Importing L ------------" << std::endl;
    Filtration L = list_to_filtration<Int, Real>(L_);

    int n_L = IdMap_.size();
    std::vector<int> IdMapping;

    for(int i = 0; i < n_L; i++) {
        int i_map = IdMap_[i].cast<int>();
        IdMapping.push_back(i_map);
    }
    std::cout << "---------- Map from L to K ----------" << std::endl;
    for(int i = 0; i < n_L; i++) {
        std::cout << "Cell " << i << " in L is mapped to cell " << IdMapping[i] << " in K." << std::endl;
    }

    oineus::Params params;

    params.sort_dgms = false;
    params.clearing_opt = false;
    params.n_threads = n_threads;

    PyKerImCokDgms KICR(oineus::reduce_im_ker_cok<Int, Real>(K, L, IdMapping, params));

    return KICR;
}

template<class Int>
void init_oineus_common(py::module& m)
{
    using namespace pybind11::literals;

    using oineus::VREdge;

    using oineus::DenoiseStrategy;
    using oineus::ConflictStrategy;

    using Decomposition = oineus::VRUDecomposition<Int>;

    using ReductionParams = oineus::Params;

    using IndexDiagram = PyOineusDiagrams<size_t>;

    std::string vr_edge_name = "VREdge";

    std::string index_dgm_class_name = "IndexDiagrams";

    py::class_<VREdge>(m, vr_edge_name.c_str())
            .def(py::init<Int>())
            .def_readwrite("x", &VREdge::x)
            .def_readwrite("y", &VREdge::y)
            .def("__getitem__", [](const VREdge& p, int i) {
              if (i == 0)
                  return p.x;
              else if (i == 1)
                  return p.y;
              else
                  throw std::out_of_range("i must be 0 or 1");
            })
            .def("__repr__", [](const VREdge& p) {
              std::stringstream ss;
              ss << p;
              return ss.str();
            });

    py::class_<ReductionParams>(m, "ReductionParams")
            .def(py::init<>())
            .def_readwrite("n_threads", &ReductionParams::n_threads)
            .def_readwrite("chunk_size", &ReductionParams::chunk_size)
            .def_readwrite("write_dgms", &ReductionParams::write_dgms)
            .def_readwrite("sort_dgms", &ReductionParams::sort_dgms)
            .def_readwrite("clearing_opt", &ReductionParams::clearing_opt)
            .def_readwrite("acq_rel", &ReductionParams::acq_rel)
            .def_readwrite("print_time", &ReductionParams::print_time)
            .def_readwrite("elapsed", &ReductionParams::elapsed)
            .def_readwrite("compute_v", &ReductionParams::compute_v)
            .def_readwrite("compute_u", &ReductionParams::compute_u)
            .def_readwrite("do_sanity_check", &ReductionParams::do_sanity_check)
            .def(py::pickle(
                    // __getstate__
                    [](const ReductionParams& p) {
                      return py::make_tuple(p.n_threads, p.chunk_size, p.write_dgms,
                              p.sort_dgms, p.clearing_opt, p.acq_rel, p.print_time, p.compute_v, p.compute_u,
                              p.do_sanity_check, p.elapsed);
                    },
                    // __setstate__
                    [](py::tuple t) {
                      if (t.size() != 11)
                          throw std::runtime_error("Invalid tuple for ReductionParams");

                      ReductionParams p;

                      int i = 0;

                      p.n_threads = t[i++].cast<decltype(p.n_threads)>();
                      p.chunk_size = t[i++].cast<decltype(p.chunk_size)>();
                      p.write_dgms = t[i++].cast<decltype(p.write_dgms)>();
                      p.sort_dgms = t[i++].cast<decltype(p.sort_dgms)>();
                      p.clearing_opt = t[i++].cast<decltype(p.clearing_opt)>();
                      p.acq_rel = t[i++].cast<decltype(p.acq_rel)>();
                      p.print_time = t[i++].cast<decltype(p.print_time)>();
                      p.compute_v = t[i++].cast<decltype(p.compute_v)>();
                      p.compute_u = t[i++].cast<decltype(p.compute_u)>();
                      p.do_sanity_check = t[i++].cast<decltype(p.do_sanity_check)>();
                      p.elapsed = t[i++].cast<decltype(p.elapsed)>();

                      return p;
                    }));

    py::enum_<DenoiseStrategy>(m, "DenoiseStrategy", py::arithmetic())
            .value("BirthBirth", DenoiseStrategy::BirthBirth, "(b, d) maps to (b, b)")
            .value("DeathDeath", DenoiseStrategy::DeathDeath, "(b, d) maps to (d, d)")
            .value("Midway", DenoiseStrategy::Midway, "((b, d) maps to ((b+d)/2, (b+d)/2)")
            .def("as_str", [](const DenoiseStrategy& self) { return denoise_strategy_to_string(self); });

    py::enum_<ConflictStrategy>(m, "ConflictStrategy", py::arithmetic())
            .value("Max", ConflictStrategy::Max, "choose maximal displacement")
            .value("Avg", ConflictStrategy::Avg, "average gradients")
            .value("Sum", ConflictStrategy::Sum, "sum gradients")
            .value("FixCritAvg", ConflictStrategy::FixCritAvg, "use matching on critical, average gradients on other simplices")
            .def("as_str", [](const ConflictStrategy& self) { return conflict_strategy_to_string(self); });

    py::class_<Decomposition>(m, "Decomposition")
            .def(py::init<const oineus::Filtration<Int, double, Int>&, bool>())
            .def(py::init<const oineus::Filtration<Int, double, VREdge>&, bool>())
            .def(py::init<const oineus::Filtration<Int, float, Int>&, bool>())
            .def(py::init<const oineus::Filtration<Int, float, VREdge>&, bool>())
            .def_readwrite("r_data", &Decomposition::r_data)
            .def_readwrite("v_data", &Decomposition::v_data)
            .def_readwrite("u_data_t", &Decomposition::u_data_t)
            .def_readwrite("d_data", &Decomposition::d_data)
            .def("reduce", &Decomposition::reduce, py::call_guard<py::gil_scoped_release>())
            .def("sanity_check", &Decomposition::sanity_check, py::call_guard<py::gil_scoped_release>())
            .def("diagram", [](const Decomposition& self, const oineus::Filtration<Int, double, VREdge>& fil, bool include_inf_points) { return PyOineusDiagrams<double>(self.diagram(fil, include_inf_points)); })
            .def("diagram", [](const Decomposition& self, const oineus::Filtration<Int, float, VREdge>& fil, bool include_inf_points) { return PyOineusDiagrams<float>(self.diagram(fil, include_inf_points)); })
            .def("diagram", [](const Decomposition& self, const oineus::Filtration<Int, double, Int>& fil, bool include_inf_points) { return PyOineusDiagrams<double>(self.diagram(fil, include_inf_points)); })
            .def("diagram", [](const Decomposition& self, const oineus::Filtration<Int, float, Int>& fil, bool include_inf_points) { return PyOineusDiagrams<float>(self.diagram(fil, include_inf_points)); })
            .def("index_diagram", [](const Decomposition& self, const oineus::Filtration<Int, double, VREdge>& fil, bool include_inf_points, bool include_zero_persistence_points) {
              return PyOineusDiagrams<size_t>(self.index_diagram(fil, include_inf_points, include_zero_persistence_points));
            })
            .def("index_diagram", [](const Decomposition& self, const oineus::Filtration<Int, float, VREdge>& fil, bool include_inf_points, bool include_zero_persistence_points) {
              return PyOineusDiagrams<size_t>(self.index_diagram(fil, include_inf_points, include_zero_persistence_points));
            })
            .def("index_diagram", [](const Decomposition& self, const oineus::Filtration<Int, double, Int>& fil, bool include_inf_points, bool include_zero_persistence_points) {
              return PyOineusDiagrams<size_t>(self.index_diagram(fil, include_inf_points, include_zero_persistence_points));
            })
            .def("index_diagram", [](const Decomposition& self, const oineus::Filtration<Int, float, Int>& fil, bool include_inf_points, bool include_zero_persistence_points) {
              return PyOineusDiagrams<size_t>(self.index_diagram(fil, include_inf_points, include_zero_persistence_points));
            });

    using DgmPointInt = typename oineus::DgmPoint<Int>;
    using DgmPointSizet = typename oineus::DgmPoint<size_t>;

    py::class_<DgmPointInt>(m, "DgmPoint_int")
            .def(py::init<Int, Int>())
            .def_readwrite("birth", &DgmPointInt::birth)
            .def_readwrite("death", &DgmPointInt::death)
            .def("__getitem__", [](const DgmPointInt& p, int i) { return p[i]; })
            .def("__hash__", [](const DgmPointInt& p) { return std::hash<DgmPointInt>()(p); })
            .def("__repr__", [](const DgmPointInt& p) {
              std::stringstream ss;
              ss << p;
              return ss.str();
            });

    py::class_<DgmPointSizet>(m, "DgmPoint_Sizet")
            .def(py::init<size_t, size_t>())
            .def_readwrite("birth", &DgmPointSizet::birth)
            .def_readwrite("death", &DgmPointSizet::death)
            .def("__getitem__", [](const DgmPointSizet& p, int i) { return p[i]; })
            .def("__hash__", [](const DgmPointSizet& p) { return std::hash<DgmPointSizet>()(p); })
            .def("__repr__", [](const DgmPointSizet& p) {
              std::stringstream ss;
              ss << p;
              return ss.str();
            });

    py::class_<IndexDiagram>(m, index_dgm_class_name.c_str())
            .def(py::init<dim_type>())
            .def("in_dimension", &IndexDiagram::get_diagram_in_dimension)
            .def("__getitem__", &IndexDiagram::get_diagram_in_dimension);
}

template<class Int, class Real>
void init_oineus(py::module& m, std::string suffix)
{
    using namespace pybind11::literals;

    using DgmPoint = oineus::DgmPoint<Real>;
    using Diagram = PyOineusDiagrams<Real>;

    using LSFiltration = oineus::Filtration<Int, Real, Int>;
    using LSSimplex = typename LSFiltration::FiltrationSimplex;

    using oineus::VREdge;
    using VRFiltration = oineus::Filtration<Int, Real, VREdge>;
    using VRSimplex = typename VRFiltration::FiltrationSimplex;
    using VRUDecomp = oineus::VRUDecomposition<Int>;
    using KerImCokRed = oineus::KerImCokReduced<Int, Real>;
    using PyKerImCokRed = PyKerImCokDgms<Int, Real>;
    //using CokRed =  oineus::CokReduced<Int, Real>;

    std::string dgm_point_name = "DiagramPoint" + suffix;
    std::string dgm_class_name = "Diagrams" + suffix;

    std::string ls_simplex_class_name = "LSSimplex" + suffix;
    std::string ls_filtration_class_name = "LSFiltration" + suffix;

    std::string vr_simplex_class_name = "VRSimplex" + suffix;
    std::string vr_filtration_class_name = "VRFiltration" + suffix;

    std::string ker_im_cok_reduced_class_name = "KerImCokReduced" + suffix;
    std::string py_ker_im_cok_reduced_class_name = "PyKerImCokDgms" + suffix;

    py::class_<DgmPoint>(m, dgm_point_name.c_str())
            .def(py::init<Real, Real>())
            .def_readwrite("birth", &DgmPoint::birth)
            .def_readwrite("death", &DgmPoint::death)
            .def("__getitem__", [](const DgmPoint& p, int i) { return p[i]; })
            .def("__hash__", [](const DgmPoint& p) { return std::hash<DgmPoint>()(p); })
            .def("__repr__", [](const DgmPoint& p) {
              std::stringstream ss;
              ss << p;
              return ss.str();
            });

    py::class_<Diagram>(m, dgm_class_name.c_str())
            .def(py::init<dim_type>())
            .def("in_dimension", &Diagram::get_diagram_in_dimension)
            .def("__getitem__", &Diagram::get_diagram_in_dimension);

    py::class_<LSSimplex>(m, ls_simplex_class_name.c_str())
            .def(py::init<typename LSSimplex::IdxVector, Real, typename LSSimplex::ValueLocation>())
            .def_readonly("id", &LSSimplex::id_)
            .def_readonly("sorted_id", &LSSimplex::sorted_id_)
            .def_readonly("vertices", &LSSimplex::vertices_)
            .def_readonly("value", &LSSimplex::value_)
            .def_readonly("critical_vertex", &LSSimplex::critical_value_location_)
            .def("dim", &LSSimplex::dim)
            .def("boundary", &LSSimplex::boundary)
            .def("__repr__", [](const LSSimplex& sigma) {
              std::stringstream ss;
              ss << sigma;
              return ss.str();
            });

    py::class_<VRSimplex>(m, vr_simplex_class_name.c_str())
            .def(py::init<typename VRSimplex::IdxVector, Real, typename VRSimplex::ValueLocation>())
            .def_readonly("id", &VRSimplex::id_)
            .def_readonly("sorted_id", &VRSimplex::sorted_id_)
            .def_readonly("vertices", &VRSimplex::vertices_)
            .def_readonly("value", &VRSimplex::value_)
            .def_readonly("critical_edge", &VRSimplex::critical_value_location_)
            .def("dim", &VRSimplex::dim)
            .def("boundary", &VRSimplex::boundary)
            .def("__repr__", [](const VRSimplex& sigma) {
              std::stringstream ss;
              ss << sigma;
              return ss.str();
            });

    py::class_<LSFiltration>(m, ls_filtration_class_name.c_str())
            .def(py::init<typename LSFiltration::FiltrationSimplexVector, bool, int>())
            .def("max_dim", &LSFiltration::max_dim)
            .def("simplices", &LSFiltration::simplices_copy)
            .def("size_in_dimension", &LSFiltration::size_in_dimension)
            .def("critical_vertex", &LSFiltration::cvl)
            .def("simplex_value", &LSFiltration::value_by_sorted_id)
            .def("boundary_matrix", &LSFiltration::boundary_matrix_full);

    py::class_<VRFiltration>(m, vr_filtration_class_name.c_str())
            .def(py::init<typename VRFiltration::FiltrationSimplexVector, bool, int>())
            .def("max_dim", &VRFiltration::max_dim)
            .def("simplices", &VRFiltration::simplices_copy)
            .def("critical_edge", &VRFiltration::cvl)
            .def("size_in_dimension", &VRFiltration::size_in_dimension)
            .def("simplex_value", &VRFiltration::value_by_sorted_id)
            .def("boundary_matrix", &VRFiltration::boundary_matrix_full);

    py::class_<KerImCokRed>(m, ker_im_cok_reduced_class_name.c_str())
            .def(py::init<oineus::Filtration<Int, Real, Int>, oineus::Filtration<Int, Real, Int>, VRUDecomp, VRUDecomp, VRUDecomp, VRUDecomp, VRUDecomp, std::vector<int>, std::vector<int>, std::vector<int>>());

    py::class_<PyKerImCokRed>(m, py_ker_im_cok_reduced_class_name.c_str())
            .def(py::init<KerImCokRed>())
            .def("kernel", &PyKerImCokRed::kernel)
            .def("image", &PyKerImCokRed::image)
            .def("cokernel", &PyKerImCokRed::cokernel);

    std::string func_name;

    // diagrams
    func_name = "compute_diagrams_ls" + suffix + "_1";
    m.def(func_name.c_str(), &compute_diagrams_ls_freudenthal<Int, Real, 1>);

    func_name = "compute_diagrams_ls" + suffix + "_2";
    m.def(func_name.c_str(), &compute_diagrams_ls_freudenthal<Int, Real, 2>);

    func_name = "compute_diagrams_ls" + suffix + "_3";
    m.def(func_name.c_str(), &compute_diagrams_ls_freudenthal<Int, Real, 3>);

    // Lower-star Freudenthal filtration
    func_name = "get_fr_filtration" + suffix + "_1";
    m.def(func_name.c_str(), &get_fr_filtration<Int, Real, 1>);

    func_name = "get_fr_filtration" + suffix + "_2";
    m.def(func_name.c_str(), &get_fr_filtration<Int, Real, 2>);

    func_name = "get_fr_filtration" + suffix + "_3";
    m.def(func_name.c_str(), &get_fr_filtration<Int, Real, 3>);

    // Vietoris--Rips filtration
    func_name = "get_vr_filtration" + suffix + "_1";
    m.def(func_name.c_str(), &get_vr_filtration<Int, Real, 1>);

    func_name = "get_vr_filtration" + suffix + "_2";
    m.def(func_name.c_str(), &get_vr_filtration<Int, Real, 2>);

    func_name = "get_vr_filtration" + suffix + "_3";
    m.def(func_name.c_str(), &get_vr_filtration<Int, Real, 3>);

    // boundary matrix as vector of columns
    func_name = "get_boundary_matrix" + suffix + "_1";
    m.def(func_name.c_str(), &get_boundary_matrix<Int, Real, 1>);

    func_name = "get_boundary_matrix" + suffix + "_2";
    m.def(func_name.c_str(), &get_boundary_matrix<Int, Real, 2>);

    func_name = "get_boundary_matrix" + suffix + "_3";
    m.def(func_name.c_str(), &get_boundary_matrix<Int, Real, 3>);

    // target values
    func_name = "get_denoise_target" + suffix;
    m.def(func_name.c_str(), &oineus::get_denoise_target<Int, Real, Int>);

    func_name = "get_denoise_target" + suffix;
    m.def(func_name.c_str(), &oineus::get_denoise_target<Int, Real, VREdge>);

    func_name = "get_ls_wasserstein_matching_target_values" + suffix;
    m.def(func_name.c_str(), &oineus::get_target_from_matching<Int, Real, Int>);

    // target values -- diagram loss
    func_name = "get_target_values_diagram_loss" + suffix;
    m.def(func_name.c_str(), &oineus::get_prescribed_simplex_values_diagram_loss<Real>);

    // target values --- X set
    func_name = "get_ls_target_values_x" + suffix;
    m.def(func_name.c_str(), &oineus::get_prescribed_simplex_values_set_x<Int, Real, Int>);

    func_name = "get_vr_target_values_x" + suffix;
    m.def(func_name.c_str(), &oineus::get_prescribed_simplex_values_set_x<Int, Real, VREdge>);

    // to reproduce "Topology layer for ML" experiments
    func_name = "get_bruelle_target" + suffix;
    m.def(func_name.c_str(), &oineus::get_bruelle_target<Int, Real, VREdge>);

    // to reproduce "Well group loss" experiments
    func_name = "get_well_group_target" + suffix;
    m.def(func_name.c_str(), &oineus::get_well_group_target<Int, Real, VREdge>);

    func_name = "get_well_group_target" + suffix;
    m.def(func_name.c_str(), &oineus::get_well_group_target<Int, Real, Int>);

    func_name = "get_nth_persistence" + suffix;
    m.def(func_name.c_str(), &oineus::get_nth_persistence<Int, Real, VREdge>);

    // to equidistribute points
    func_name = "get_barycenter_target" + suffix;
    m.def(func_name.c_str(), &oineus::get_barycenter_target<Int, Real, VREdge>);

    func_name = "get_barycenter_target" + suffix;
    m.def(func_name.c_str(), &oineus::get_barycenter_target<Int, Real, Int>);

    // to get permutation for Warm Starts
    func_name = "get_permutation" + suffix;
    m.def(func_name.c_str(), &oineus::targets_to_permutation<Int, Real, Int>);

    func_name = "get_permutation" + suffix;
    m.def(func_name.c_str(), &oineus::targets_to_permutation<Int, Real, VREdge>);

    func_name = "get_permutation_dtv" + suffix;
    m.def(func_name.c_str(), &oineus::targets_to_permutation_dtv<Int, Real, Int>);

    func_name = "get_permutation_dtv" + suffix;
    m.def(func_name.c_str(), &oineus::targets_to_permutation_dtv<Int, Real, VREdge>);

    // reduce to create an ImKerReduced object
    func_name = "reduce_im_ker_cok" + suffix;
    m.def(func_name.c_str(), &oineus::reduce_im_ker_cok<Int, Real>);

    func_name = "list_to_filtration" + suffix;
    m.def(func_name.c_str(), &list_to_filtration<Int, Real>);

    func_name = "compute_kernel_image_cokernel_diagrams" + suffix;
    m.def(func_name.c_str(), &compute_kernel_image_cokernel_diagrams<Int, Real>);

    func_name = "get_ls_filtration" + suffix;
    m.def(func_name.c_str(), &get_ls_filtration<Int, Real>);
    /*func_name = "compute_cokernel_diagrams" + suffix;
    m.def(func_name.c_str(), &compute_cokernel_diagrams<Int, Real>);*/
}

#endif //OINEUS_OINEUS_PERSISTENCE_BINDINGS_H
