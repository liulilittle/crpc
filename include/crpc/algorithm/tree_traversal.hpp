// CRPC -- non-recursive tree traversal.
//
// Two entry points share one set of iterative algorithms:
//
//   traverse_tree     walks an adjacency list in "first child / next sibling"
//                     form (one node per array slot, npos terminates a list).
//   traverse_generic  walks any tree whose children are produced by a callback.
//
// Nothing here recurses and nothing here can overflow the stack: the explicit
// work list lives on the heap, so a chain 100000 nodes deep is no different
// from a chain 3 nodes deep. Each node is visited at most once even if the
// input is malformed: a cycle (or a DAG where two parents share a child) is
// detected by the "already entered" guard and the repeat is skipped rather
// than looped over. There is no error channel in the signature, so the
// traversal is simply bounded by the number of distinct nodes.
//
// Order definitions (children are always processed left to right, i.e. in the
// order the child list yields them):
//
//   pre_order    node before its children.
//   in_order     exactly the in-order of the left-child/right-sibling binary
//                encoding of the tree: first-child subtree, then the node,
//                then the subtrees of the remaining children (the sibling
//                chain). A leaf visits just itself; a node with a single child
//                visits that child's subtree first, so a chain comes out in
//                reverse (post-order) order. If a different general-tree
//                in-order convention is wanted, use traverse_generic.
//   post_order   all children before the node.
//   level_order  breadth first, children in list order.
//
// The visitor is called as `visit(std::size_t node)` once per node; its return
// value is ignored, so a lambda that returns void is the normal form.
#ifndef CRPC_ALGORITHM_TREE_TRAVERSAL_HPP
#define CRPC_ALGORITHM_TREE_TRAVERSAL_HPP

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace crpc {
namespace algorithm {

// One slot of an adjacency-list tree. `npos` marks "no such node". A node may
// appear in several lists only in a malformed (DAG) input; traversal then
// visits it once.
struct tree_node {
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    std::size_t first_child = npos;
    std::size_t next_sibling = npos;
};

enum class visit_order : std::uint8_t {
    pre_order,
    in_order,
    post_order,
    level_order
};

namespace detail {

// Calls `emit(child)` for every child of `node` in list order. The walk is
// bounded by the node count, so a malformed sibling chain that loops around
// itself terminates instead of hanging (the duplicate is then filtered by the
// caller's `entered` set). Out-of-range indices are ignored for the same
// reason: this header is total on malformed input instead of undefined.
template <typename Emit>
void tree_children(const std::vector<tree_node>& nodes, std::size_t node,
                   Emit&& emit) {
    std::size_t steps = 0;
    for (std::size_t child = nodes[node].first_child; child != tree_node::npos;
         child = nodes[child].next_sibling) {
        if (child >= nodes.size() || steps >= nodes.size()) {
            return;
        }
        emit(child);
        ++steps;
    }
}

template <typename ChildrenFn>
void collect_children(std::size_t node, ChildrenFn& children,
                      std::vector<std::size_t>& out) {
    out.clear();
    children(node, out);
}

struct traversal_frame {
    std::size_t node = tree_node::npos;
    bool descend = true; // in_order: true until the first child is handled
};

} // namespace detail

// Traverses the tree rooted at `root` in `nodes` (adjacency list). A root of
// npos, or one outside `nodes`, visits nothing. Nodes are visited at most once.
template <typename Visit>
void traverse_tree(const std::vector<tree_node>& nodes, std::size_t root,
                   visit_order order, Visit&& visit) {
    if (root == tree_node::npos || root >= nodes.size()) {
        return;
    }
    std::vector<std::uint8_t> entered(nodes.size(), 0);
    std::vector<std::size_t> work;
    std::vector<std::size_t> kids;
    std::vector<std::size_t> reversed;

    switch (order) {
    case visit_order::pre_order: {
        work.push_back(root);
        while (!work.empty()) {
            const std::size_t node = work.back();
            work.pop_back();
            if (entered[node] != 0) {
                continue;
            }
            entered[node] = 1;
            visit(node);
            kids.clear();
            detail::tree_children(
                nodes, node,
                [&kids](std::size_t child) { kids.push_back(child); });
            for (std::size_t i = kids.size(); i-- > 0;) {
                work.push_back(kids[i]);
            }
        }
        break;
    }
    case visit_order::in_order: {
        // Each frame is pushed twice: once to descend before the first child
        // and once to visit after it. The remaining children are re-pushed
        // below the node so the next sibling is handled in order.
        std::vector<detail::traversal_frame> stack;
        stack.push_back({root, true});
        while (!stack.empty()) {
            const detail::traversal_frame frame = stack.back();
            if (frame.descend) {
                if (entered[frame.node] != 0) {
                    stack.pop_back();
                    continue;
                }
                entered[frame.node] = 1;
                kids.clear();
                detail::tree_children(
                    nodes, frame.node,
                    [&kids](std::size_t child) { kids.push_back(child); });
                stack.back().descend = false;
                if (!kids.empty()) {
                    stack.push_back({kids.front(), true});
                }
                continue;
            }
            stack.pop_back();
            visit(frame.node);
            kids.clear();
            detail::tree_children(
                nodes, frame.node,
                [&kids](std::size_t child) { kids.push_back(child); });
            for (std::size_t i = kids.size(); i-- > 1;) {
                stack.push_back({kids[i], true});
            }
        }
        break;
    }
    case visit_order::post_order: {
        // Two stages: (1) walk the tree pre-order pushing nodes onto
        // `reversed`; (2) emit `reversed` backwards. A reverse of a
        // right-to-left pre-order is a left-to-right post-order.
        work.push_back(root);
        while (!work.empty()) {
            const std::size_t node = work.back();
            work.pop_back();
            if (entered[node] != 0) {
                continue;
            }
            entered[node] = 1;
            reversed.push_back(node);
            kids.clear();
            detail::tree_children(
                nodes, node,
                [&kids](std::size_t child) { kids.push_back(child); });
            for (const std::size_t child : kids) {
                work.push_back(child);
            }
        }
        for (std::size_t i = reversed.size(); i-- > 0;) {
            visit(reversed[i]);
        }
        break;
    }
    case visit_order::level_order: {
        work.push_back(root);
        entered[root] = 1;
        for (std::size_t head = 0; head < work.size(); ++head) {
            const std::size_t node = work[head];
            visit(node);
            kids.clear();
            detail::tree_children(
                nodes, node,
                [&kids](std::size_t child) { kids.push_back(child); });
            for (const std::size_t child : kids) {
                if (entered[child] == 0) {
                    entered[child] = 1;
                    work.push_back(child);
                }
            }
        }
        break;
    }
    }
}

// Traverses an arbitrary tree. `children` is called as
// `void (std::size_t node, std::vector<std::size_t>& out)` and must append the
// children of `node` to `out` (the buffer is cleared before every call).
// Node ids are arbitrary size_t values. Nodes are visited at most once, which
// bounds a malformed (cyclic) input instead of looping forever.
template <typename ChildrenFn, typename Visit>
void traverse_generic(std::size_t root, ChildrenFn&& children, visit_order order,
                      Visit&& visit) {
    std::unordered_set<std::size_t> entered;
    std::vector<std::size_t> work;
    std::vector<std::size_t> buffer;
    std::vector<std::size_t> second;

    switch (order) {
    case visit_order::pre_order: {
        work.push_back(root);
        while (!work.empty()) {
            const std::size_t node = work.back();
            work.pop_back();
            if (!entered.insert(node).second) {
                continue;
            }
            visit(node);
            buffer.clear();
            children(node, buffer);
            for (std::size_t i = buffer.size(); i-- > 0;) {
                work.push_back(buffer[i]);
            }
        }
        break;
    }
    case visit_order::in_order: {
        std::vector<detail::traversal_frame> stack;
        stack.push_back({root, true});
        while (!stack.empty()) {
            detail::traversal_frame frame = stack.back();
            if (frame.descend) {
                if (!entered.insert(frame.node).second) {
                    stack.pop_back();
                    continue;
                }
                buffer.clear();
                children(frame.node, buffer);
                stack.back().descend = false;
                if (!buffer.empty()) {
                    stack.push_back({buffer.front(), true});
                }
                continue;
            }
            stack.pop_back();
            visit(frame.node);
            buffer.clear();
            children(frame.node, buffer);
            for (std::size_t i = buffer.size(); i-- > 1;) {
                stack.push_back({buffer[i], true});
            }
        }
        break;
    }
    case visit_order::post_order: {
        work.push_back(root);
        while (!work.empty()) {
            const std::size_t node = work.back();
            work.pop_back();
            if (!entered.insert(node).second) {
                continue;
            }
            second.push_back(node);
            buffer.clear();
            children(node, buffer);
            for (const std::size_t child : buffer) {
                work.push_back(child);
            }
        }
        for (std::size_t i = second.size(); i-- > 0;) {
            visit(second[i]);
        }
        break;
    }
    case visit_order::level_order: {
        work.push_back(root);
        entered.insert(root);
        for (std::size_t head = 0; head < work.size(); ++head) {
            const std::size_t node = work[head];
            visit(node);
            buffer.clear();
            children(node, buffer);
            for (const std::size_t child : buffer) {
                if (entered.insert(child).second) {
                    work.push_back(child);
                }
            }
        }
        break;
    }
    }
}

} // namespace algorithm
} // namespace crpc

#endif // CRPC_ALGORITHM_TREE_TRAVERSAL_HPP
