// CRPC -- generic A* search.
//
// The search is template-only (there is nothing to compile out of line): A*
// over an arbitrary state space needs the neighbour/cost/heuristic functions
// to be part of the type, so this header ships the whole algorithm and the
// test suite proves optimality against a Dijkstra reference. The calling
// contract:
//
//   Node         state type; must be copyable and support `==`.
//   Hash         std::hash<Node> by default; pass a custom hasher when Node
//                is not std::hash-able.
//   neighbors    `void (const Node&, std::vector<Node>&)`; appends the
//                reachable neighbours of the node. A neighbour that is not
//                reachable (a blocked cell, a missing edge, ...) is simply
//                never emitted.
//   edge_cost    `Cost (const Node&, const Node&)`; the non-negative cost of
//                moving directly from the first node to the second. It is
//                only ever called for pairs `neighbors` produced.
//   heuristic    `Cost (const Node&, const Node&)`; an admissible estimate of
//                the remaining cost to the goal. Admissibility (never
//                overestimating) is what guarantees the returned path is
//                optimal; with an overestimating heuristic A* still terminates
//                but may return a suboptimal path.
//
// Termination is unconditional for finite reachable state spaces: a node is
// expanded at most once (the first pop from the open set is its optimal cost),
// so the open set drains. An unreachable goal yields an error with
// `rpc_code::not_found` -- a definite "no path", never a hang.
//
// Optimality is not asserted at runtime; it is a theorem about the inputs. The
// test suite verifies it empirically by comparing every heuristic against a
// Dijkstra reference on hundreds of random grids.
#ifndef CRPC_ALGORITHM_ASTAR_HPP
#define CRPC_ALGORITHM_ASTAR_HPP

#include <cstddef>
#include <functional>
#include <queue>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <crpc/error.hpp>

namespace crpc {
namespace algorithm {

// Tuning knobs for a single search.
struct astar_config {
    // Upper bound on the number of nodes expanded. Zero means "no bound".
    // Exceeding the bound aborts the search with `resource_exhausted`; it
    // exists so a caller can bound a hostile graph, not as a correctness
    // requirement (the algorithm terminates on its own).
    std::size_t max_expanded = 0;
};

namespace detail {

template <typename Cost, typename Node, typename Hash, typename FNeighbors,
          typename FCost, typename FHeuristic>
result<std::vector<Node>>
astar_impl(const Node& start, const Node& goal, FNeighbors neighbors,
           FCost edge_cost, FHeuristic heuristic, std::size_t max_expanded,
           Hash hash) {
    if (start == goal) {
        return std::vector<Node>{start};
    }

    // The open set is a min-heap keyed by f = g + h. `order` breaks ties so
    // two entries with equal f are ordered deterministically without requiring
    // Node to support `<`. The entry carries its own g: the staleness check on
    // pop compares that against `best_g`, which avoids recomputing the
    // heuristic for every popped (and often stale) entry.
    struct queue_entry {
        Cost f;
        Cost g;
        std::size_t order;
        Node node;
    };
    struct lower_f {
        bool operator()(const queue_entry& a, const queue_entry& b) const {
            if (a.f != b.f) {
                return a.f > b.f;
            }
            return a.order > b.order;
        }
    };

    std::priority_queue<queue_entry, std::vector<queue_entry>, lower_f> open;
    std::unordered_map<Node, Cost, Hash> best_g(1, hash);
    std::unordered_map<Node, Node, Hash> came_from(1, hash);

    const Cost start_h = static_cast<Cost>(heuristic(start, goal));
    best_g.emplace(start, Cost{0});
    open.push({start_h, Cost{0}, 0, start});

    std::size_t counter = 1;
    std::size_t expanded = 0;
    std::vector<Node> nbrs;

    while (!open.empty()) {
        const Cost g = open.top().g;
        const Node node = open.top().node;
        open.pop();

        const auto found = best_g.find(node);
        if (found == best_g.end() || g < found->second ||
            found->second < g) {
            continue; // a stale open-set entry for an already-improved node
        }

        if (node == goal) {
            std::vector<Node> path;
            Node current = node;
            for (;;) {
                path.push_back(current);
                if (current == start) {
                    break;
                }
                const auto prev = came_from.find(current);
                current = prev->second; // present: only start lacks a predecessor
            }
            std::reverse(path.begin(), path.end());
            return path;
        }

        if (max_expanded != 0 && expanded >= max_expanded) {
            return rpc_error{
                rpc_code::resource_exhausted,
                "astar: node expansion budget exceeded (max_expanded)"};
        }
        ++expanded;

        nbrs.clear();
        neighbors(node, nbrs);
        for (const Node& next : nbrs) {
            const Cost step = static_cast<Cost>(edge_cost(node, next));
            const Cost tentative = g + step;
            const auto existing = best_g.find(next);
            if (existing != best_g.end()) {
                if (!(tentative < existing->second)) {
                    continue;
                }
                existing->second = tentative;
            } else {
                best_g.emplace(next, tentative);
            }
            came_from.insert_or_assign(next, node);
            open.push({tentative + static_cast<Cost>(heuristic(next, goal)),
                       tentative, counter++, next});
        }
    }

    return rpc_error{rpc_code::not_found,
                     "astar: no path from start to goal exists"};
}

} // namespace detail

// Runs A* from `start` to `goal`. Returns the optimal path when one exists
// (start and goal included, in order); see the file comment for the exact
// callable contracts.
template <typename Node, typename Hash = std::hash<Node>, typename FNeighbors,
          typename FCost, typename FHeuristic>
[[nodiscard]] result<std::vector<Node>>
astar_path(const Node& start, const Node& goal, FNeighbors neighbors,
           FCost edge_cost, FHeuristic heuristic,
           const astar_config& config = {}, Hash hash = Hash{}) {
    using cost_type =
        std::decay_t<std::invoke_result_t<FCost, const Node&, const Node&>>;
    using heuristic_type =
        std::invoke_result_t<FHeuristic, const Node&, const Node&>;
    using neighbor_slot = std::vector<Node>&;
    static_assert(
        std::is_convertible<heuristic_type, cost_type>::value,
        "crpc::algorithm::astar_path: the heuristic must return a type "
        "convertible to the edge-cost type");
    static_assert(
        std::is_same<std::decay_t<std::invoke_result_t<FNeighbors, const Node&,
                                                      neighbor_slot>>,
                     void>::value,
        "crpc::algorithm::astar_path: neighbors must be callable as "
        "void (const Node&, std::vector<Node>&)");
    return detail::astar_impl<cost_type>(start, goal, neighbors, edge_cost,
                                         heuristic, config.max_expanded, hash);
}

} // namespace algorithm
} // namespace crpc

#endif // CRPC_ALGORITHM_ASTAR_HPP