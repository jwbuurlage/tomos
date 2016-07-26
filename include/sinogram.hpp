#pragma once

#include <vector>

#include "common.hpp"

namespace tomo {

// the projector template argument is there so the reconstruction algorithms can
// construct an projector themselves when solving for the sinogram
template <dimension Dimension, typename T, class Geometry, class Projector>
class sinogram {
  public:
    using projector_type = Projector;

    sinogram(const Geometry& geometry) : geometry_(geometry) {
        data_.resize(geometry_.lines());
    }

    T& operator[] (int i) { return data_[i]; }
    const T& operator[] (int i) const { return data_[i]; }

    std::vector<T>& mutable_data() { return data_; }

    const Geometry& geometry() const { return geometry_; }

  private:
    const Geometry& geometry_;
    std::vector<T> data_;
};

} // namespace tomo
