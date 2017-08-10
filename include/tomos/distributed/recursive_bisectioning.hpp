#pragma once

#include <cmath>
#include <cstddef>
#include <experimental/optional>
#include <limits>
#include <queue>
#include <set>
#include <stack>
#include <utility>
#include <vector>

template <typename T>
using optional = std::experimental::optional<T>;

#include "../common.hpp"
#include "../math.hpp"
#include "../projectors/closest.hpp"
#include "../volume.hpp"

#include <bulk/bulk.hpp>

namespace tomo {
namespace distributed {

// alias the split type of the form (d, a)
template <dimension D>
using box_t = std::array<math::vec2<int>, D>;

using split_t = bulk::util::split;

template <dimension D, typename T>
image<D, T> partial_sums(const image<D, T>& w) {
    auto w_sums = image<D, T>(w.get_volume());
    auto v = w.get_volume();

    auto idx = [&](auto x, auto y, auto z) {
        return v.voxels()[0] * v.voxels()[1] * x + v.voxels()[0] * y + z;
    };

    // 2. Compute 3D partial sums everywhere
    for (int k = 0; k < v.voxels()[2]; ++k) {
        for (int j = 0; j < v.voxels()[1]; ++j) {
            for (int i = 1; i < v.voxels()[0]; ++i) {
                w_sums[idx(i, j, k)] =
                    w_sums[idx(i - 1, j, k)] + w[idx(i, j, k)];
            }
        }
    }

    for (int k = 0; k < v.voxels()[2]; ++k) {
        for (int j = 1; j < v.voxels()[1]; ++j) {
            for (int i = 0; i < v.voxels()[0]; ++i) {
                w_sums[idx(i, j, k)] =
                    w_sums[idx(i, j - 1, k)] + w[idx(i, j - 1, k)];
            }
        }
    }

    for (int k = 1; k < v.voxels()[2]; ++k) {
        for (int j = 0; j < v.voxels()[1]; ++j) {
            for (int i = 0; i < v.voxels()[0]; ++i) {
                w_sums[idx(i, j, k)] =
                    w_sums[idx(i, j, k - 1)] + w[idx(i, j, k - 1)];
            }
        }
    }

    return w_sums;
}

template <dimension D, typename T>
image<D, T> voxel_weights(const geometry::base<D, T>& geometry,
                          const volume<D, T>& volume) {
    auto w = image<D, T>(volume);
    auto proj = dim::closest<D, T>(volume);
    for (auto line : geometry) {
        for (auto [idx, value] : proj(line)) {
            (void)value;
            w[idx] += 1.0;
        }
    }

    return partial_sums(w);
}

template <dimension D, typename T>
T weight(math::vec3<int> base, math::vec3<int> end, image<D, T> ws) {
    auto v = ws.get_volume();

    auto x = end.x;
    auto y = end.y;
    auto z = end.z;
    auto x0 = base.x - 1;
    auto y0 = base.y - 1;
    auto z0 = base.z - 1;

    auto sum = [&](auto i, auto j, auto k) { return ws[ws.index(i, j, k)]; };

    auto a = sum(x, y, z);
    auto b = x0 > 0 ? sum(x0 - 1, y, z) : (T)0.0;
    auto c = y0 > 0 ? sum(x, y0 - 1, z) : (T)0.0;
    auto d = (x0 > 0 && y0 > 0) ? sum(x0 - 1, y0 - 1, z) : (T)0.0;
    auto e = z0 > 0 ? sum(x, y, z0 - 1) : (T)0.0;
    auto f = (x0 > 0 && z0 > 0) ? sum(x0 - 1, y, z0 - 1) : (T)0.0;
    auto g = (y0 > 0 && z0 > 0) ? sum(x, y0 - 1, z0 - 1) : (T)0.0;
    auto h =
        (x0 > 0 && y0 > 0 && z0 > 0) ? sum(x0 - 1, y0 - 1, z0 - 1) : (T)0.0;

    auto v1 = a - b - c + d;
    auto v2 = e - f - g + h;

    return v1 - v2;
}

template <dimension D, typename T>
split_t find_split(const std::vector<math::line<D, T>>& lines,
                   std::vector<math::line<D, T>>& lines_left,
                   std::vector<math::line<D, T>>& lines_right, box_t<D> bounds,
                   const image<D, T>& ws, T max_epsilon = 0.2) {
    struct crossing_event {
        math::vec<D, T> point;
        std::size_t line_index;
        math::vec<D, int> direction;
    };

    std::vector<crossing_event> crossings;

    // we want to tag each line with an entry point and exit point
    // the priority queue depends on the dimension!
    // first intersect each line with the bounds
    std::size_t idx = 0;
    for (auto line : lines) {
        auto intersections = math::intersect_bounds<D, T>(line, bounds);
        if (intersections) {
            auto a = intersections.value().first;
            auto b = intersections.value().second;
            crossings.push_back({a, idx, math::sign<D, T>(b - a)});
            crossings.push_back({b, idx, math::sign<D, T>(a - b)});
        } else {
            std::cout << "Line that should intersect does indeed not..\n";
        }
        ++idx;
    }

    T best_imbalance = max_epsilon;
    int best_overlap = std::numeric_limits<int>::max();
    int best_split = 0;
    int best_d = -1;

    for (int d = 0; d < D; ++d) {
        auto compare = [d](crossing_event& lhs, crossing_event& rhs) {
            return lhs.point[d] < rhs.point[d];
        };

        // TODO use ws here..
        (void)ws;
        // all lines are in physical coordinates..
        // we need some way to discretize the bounds
        // for this we use the number of voxels in `ws.get_volume()`
        auto imbalance = [&](int i) -> T {
            return math::abs(
                0.5 - (T)(i - bounds[d][0]) / (bounds[d][1] - bounds[d][0]));
        };

        std::sort(crossings.begin(), crossings.end(), compare);

        int overlap = 0;
        std::size_t current_crossing = 0;
        // FIXME can make this exponential search to speed up
        while (current_crossing < crossings.size() &&
               math::approx_equal(crossings[current_crossing].point[d],
                                  (T)bounds[d][0])) {
            ++overlap;
            ++current_crossing;
        }

        // only if point changes
        int last_split = bounds[d][0];
        for (; current_crossing < crossings.size(); ++current_crossing) {
            auto crossing = crossings[current_crossing];

            int split = (int)crossings[current_crossing].point[d];
            if (split != last_split) {
                int half_split = (last_split + split) / 2;
                last_split = split;

                // FIXME we dont get here *after* the final crossing
                auto epsilon = imbalance(half_split);
                if ((overlap < best_overlap && epsilon < max_epsilon) ||
                    (overlap == best_overlap && epsilon < best_imbalance)) {
                    best_overlap = overlap;
                    best_imbalance = epsilon;
                    best_split = half_split;
                    best_d = d;
                }
            }

            overlap += crossing.direction[d];
        }
    }

    if (best_d < 0) {
        // FIXME output debug info
        // using box_t = std::array<math::vec2<int>, D>;
        std::cout << "Can't find split for bounds: "
                  << "[" << bounds[0][0] << ", " << bounds[0][1] << "], "
                  << "[" << bounds[1][0] << ", " << bounds[1][1] << "], "
                  << "[" << bounds[2][0] << ", " << bounds[2][1] << "] "
                  << "crossings.size(): " << crossings.size() << ", "
                  << "lines.size(): " << lines.size() << ", "
                  << "\n";
        best_d = 0;
        best_split = (T)0.5 * (bounds[0][0] + bounds[0][1]);
    }

    auto best_compare = [best_d](crossing_event& lhs, crossing_event& rhs) {
        return lhs.point[best_d] < rhs.point[best_d];
    };
    std::sort(crossings.begin(), crossings.end(), best_compare);

    std::set<std::size_t> indices_left;
    std::set<std::size_t> indices_right;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        indices_right.insert(i);
    }

    for (auto& crossing : crossings) {
        if (crossing.point[best_d] > best_split)
            break;

        if (crossing.direction[best_d] > 0) {
            // entering: in indices left as well as right
            indices_left.insert(crossing.line_index);
        } else if (crossing.direction[best_d] != 0) {
            indices_right.erase(crossing.line_index);
        } else {
            // do both redundantly
            indices_left.insert(crossing.line_index);
            indices_right.erase(crossing.line_index);
        }
    }

    for (auto left_idx : indices_left) {
        lines_left.push_back(lines[left_idx]);
    }

    for (auto right_idx : indices_right) {
        lines_right.push_back(lines[right_idx]);
    }

    return split_t{best_d, (int)(best_split)};
}

template <dimension D, typename T>
bulk::util::binary_tree<bulk::util::split>
partition_bisection(const tomo::geometry::base<D, T>& geometry,
                    tomo::volume<D, T> object_volume, int processors,
                    T max_epsilon = 0.2) {
    bulk::util::binary_tree<bulk::util::split> result;

    // We want the resulting partitioning to be portable for different detector
    // configurations. We do this in two steps:
    // - Do the splitting in physical coordinates here
    // - Make a conversion function in voxel-based coordinates

    // assert that the number of processors is a power of two
    // TODO: support for non-2^k-way partitionings
    assert(math::is_power_of_two(processors));

    // compute the 'depth' of the tree (i.e. log(p))
    auto depth = (int)log2(processors);
    assert(1 << depth == processors);

    // containers for the current left and right lines
    std::vector<math::line<D, T>> all_lines;
    for (auto l : geometry) {
        auto line = math::truncate_to_volume(l, object_volume);
        if (line) {
            all_lines.push_back(line.value());
        }
    }

    auto ws = voxel_weights<D, T>(geometry, object_volume);

    using node = typename bulk::util::binary_tree<split_t>::node;
    using dir = typename bulk::util::binary_tree<split_t>::dir;

    // all the information we need to split a subvolume and save it
    struct subvolume {
        box_t<D> bounds;
        node* parent;
        dir direction;
        std::vector<math::line<D, T>> lines;
        int depth;
    };

    // state should be the correct lines and bounds
    std::stack<subvolume> split_stack;

    box_t<D> bounds;
    for (int d = 0; d < D; ++d) {
        bounds[d][1] = object_volume.voxels()[d];
    }

    auto complete_volume = subvolume{bounds, nullptr, dir::left, all_lines, 0};
    split_stack.push(complete_volume);

    while (!split_stack.empty()) {
        const auto& sub = split_stack.top();

        int sub_depth = sub.depth;
        auto sub_bounds = sub.bounds;

        // containers to hold the left and right lines
        std::vector<math::line<D, T>> left;
        std::vector<math::line<D, T>> right;

        // we now have the subvolume, and want to find the best split
        // TODO not max_epsilon, but something dynamic
        auto best_split =
            find_split<D, T>(sub.lines, left, right, sub.bounds, ws, max_epsilon);

        // add the split to the tree
        node* current_node = result.add(sub.parent, sub.direction, best_split);

        split_stack.pop();

        // now right and left contain lines for recursion
        // we move these into the 'lower splits'
        if (sub.depth + 1 < depth) {
            auto bounds_left = sub_bounds;
            bounds_left[best_split.d][1] = best_split.a;
            split_stack.push(subvolume{bounds_left, current_node, dir::left,
                                       std::move(left), sub_depth + 1});

            auto bounds_right = sub_bounds;
            bounds_right[best_split.d][0] = best_split.a;
            split_stack.push(subvolume{bounds_right, current_node, dir::right,
                                       std::move(right), sub_depth + 1});
        }
    }

    return result;
}

} // namespace distributed
} // namespace tomo
