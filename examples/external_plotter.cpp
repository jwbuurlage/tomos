/**
 * TODO:
 * - [ ] Arguments
 */
#include <cmath>
#include <iostream>

#include <glm/gtx/string_cast.hpp>

#include "tomo.hpp"
#include "util/plotter.hpp"

using T = double;
constexpr tomo::dimension D = 3_D;

int main(int argc, char* argv[]) {
    auto opt = tomo::util::args(argc, argv);

    // request a plotter scene
    auto plotter =
        tomo::ext_plotter<D, T>("tcp://localhost:5555", "Sequential test");

    auto v = tomo::volume<D, T>(opt.k);
    auto g = tomo::geometry::parallel<D, T>(v, opt.k, opt.k);
    auto f = tomo::modified_shepp_logan_phantom<T>(v);

    std::cout << glm::to_string(v.origin()) << "\n";
    std::cout << glm::to_string(v.voxels()) << "\n";

    auto kernel = tomo::dim::joseph<D, T>(v);

    plotter.plot(f);
    tomo::ascii_plot(f);

    if (opt.sirt) {
        auto sino = tomo::forward_projection<D, T>(f, g, kernel);
        auto y = tomo::reconstruction::sirt(
            v, g, kernel, sino, 0.5, 10,
            {[&](tomo::image<D, T>& image) { plotter.plot(image); }});
    }


    return 0;
}
