// CRPC -- tests for crpc/algorithm/tree_traversal.hpp.
//
// Expected sequences come from the textbook definition of each order applied to
// the known tree below, written out by hand in the comments:
//
/*        0
       / | \
      1  4  7
     / \     \
    2   3     8 */
//
//   pre_order    0 1 2 3 4 7 8
//   post_order   2 3 1 4 8 7 0
//   level_order  0 1 4 7 2 3 8
//   in_order     in-order of the left-child/right-sibling binary encoding:
//                subtree(1)=2,1,3 ; node 0 ; subtree(4)=4,8,7
//                -> 2 1 3 0 4 8 7
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <crpc/algorithm/tree_traversal.hpp>

#include "test_framework.hpp"

namespace {

using crpc::algorithm::tree_node;
using crpc::algorithm::visit_order;

constexpr std::size_t npos = tree_node::npos;

// Builds the tree in the file comment. Nodes are indexed 0..8.
std::vector<tree_node> sample_tree() {
    std::vector<tree_node> nodes(9);
    nodes[0].first_child = 1;
    nodes[1].next_sibling = 4;
    nodes[4].next_sibling = 7;
    nodes[1].first_child = 2;
    nodes[2].next_sibling = 3;
    nodes[7].first_child = 8;
    return nodes;
}

std::vector<std::size_t> collect(const std::vector<tree_node>& nodes,
                                 std::size_t root, visit_order order) {
    std::vector<std::size_t> out;
    crpc::algorithm::traverse_tree(nodes, root, order,
                                   [&out](std::size_t node) {
                                       out.push_back(node);
                                   });
    return out;
}

// A linear chain 0 -> 1 -> ... -> length-1 stored as first_child links.
std::vector<tree_node> make_chain(std::size_t length) {
    std::vector<tree_node> nodes(length);
    for (std::size_t i = 0; i + 1 < length; ++i) {
        nodes[i].first_child = i + 1;
    }
    return nodes;
}

void check_sequence(const std::vector<std::size_t>& actual,
                    const std::vector<std::size_t>& expected) {
    CRPC_CHECK_EQ(expected.size(), actual.size());
    if (actual.size() != expected.size()) {
        return;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (actual[i] != expected[i]) {
            CRPC_CHECK_EQ(expected[i], actual[i]);
            return;
        }
    }
    CRPC_CHECK(true);
}

} // namespace

CRPC_TEST(tree_pre_order_known_shape) {
    const std::vector<tree_node> nodes = sample_tree();
    check_sequence(collect(nodes, 0, visit_order::pre_order),
                   {0, 1, 2, 3, 4, 7, 8});
}

CRPC_TEST(tree_post_order_known_shape) {
    const std::vector<tree_node> nodes = sample_tree();
    // This is the case that distinguishes post_order from pre_order (M3): a
    // broken post_order that emits the same sequence as pre_order fails here.
    check_sequence(collect(nodes, 0, visit_order::post_order),
                   {2, 3, 1, 4, 8, 7, 0});
}

CRPC_TEST(tree_in_order_known_shape) {
    const std::vector<tree_node> nodes = sample_tree();
    check_sequence(collect(nodes, 0, visit_order::in_order),
                   {2, 1, 3, 0, 4, 8, 7});
}

CRPC_TEST(tree_level_order_known_shape) {
    const std::vector<tree_node> nodes = sample_tree();
    check_sequence(collect(nodes, 0, visit_order::level_order),
                   {0, 1, 4, 7, 2, 3, 8});
}

CRPC_TEST(tree_root_not_first_slot) {
    // The tree must be rooted at the argument, not at slot 0. In the sample
    // tree node 4 is a leaf and node 7 is its sibling with one child, so
    // rooting at 4 visits just {4} while rooting at 7 visits {7,8}.
    const std::vector<tree_node> nodes = sample_tree();
    check_sequence(collect(nodes, 4, visit_order::pre_order), {4});
    check_sequence(collect(nodes, 4, visit_order::post_order), {4});
    check_sequence(collect(nodes, 4, visit_order::level_order), {4});
    check_sequence(collect(nodes, 7, visit_order::pre_order), {7, 8});
    check_sequence(collect(nodes, 7, visit_order::post_order), {8, 7});
    check_sequence(collect(nodes, 7, visit_order::level_order), {7, 8});
}

CRPC_TEST(tree_empty_and_out_of_range_root) {
    const std::vector<tree_node> empty;
    for (const visit_order order :
         {visit_order::pre_order, visit_order::in_order,
          visit_order::post_order, visit_order::level_order}) {
        std::size_t visits = 0;
        crpc::algorithm::traverse_tree(empty, 0, order,
                                       [&visits](std::size_t) { ++visits; });
        CRPC_CHECK_EQ(0u, visits);
    }

    const std::vector<tree_node> nodes = sample_tree();
    std::size_t visits = 0;
    crpc::algorithm::traverse_tree(nodes, npos, visit_order::pre_order,
                                   [&visits](std::size_t) { ++visits; });
    crpc::algorithm::traverse_tree(nodes, nodes.size(),
                                   visit_order::pre_order,
                                   [&visits](std::size_t) { ++visits; });
    CRPC_CHECK_EQ(0u, visits);
}

CRPC_TEST(tree_single_node_all_orders) {
    std::vector<tree_node> node(1);
    for (const visit_order order :
         {visit_order::pre_order, visit_order::in_order,
          visit_order::post_order, visit_order::level_order}) {
        check_sequence(collect(node, 0, order), {0});
    }
}

// A chain 100000 deep must not overflow the stack: every algorithm here uses an
// explicit heap-allocated work list, so this is the case that would fail
// immediately with a recursive implementation.
CRPC_TEST(tree_deep_chain_100000) {
    constexpr std::size_t depth = 100000;
    const std::vector<tree_node> nodes = make_chain(depth);

    std::vector<std::size_t> pre;
    pre.reserve(depth);
    crpc::algorithm::traverse_tree(nodes, 0, visit_order::pre_order,
                                   [&pre](std::size_t node) {
                                       pre.push_back(node);
                                   });
    CRPC_CHECK_EQ(depth, pre.size());
    bool ascending = true;
    for (std::size_t i = 0; i < pre.size(); ++i) {
        if (pre[i] != i) {
            ascending = false;
            break;
        }
    }
    CRPC_CHECK(ascending);

    std::vector<std::size_t> post;
    post.reserve(depth);
    crpc::algorithm::traverse_tree(nodes, 0, visit_order::post_order,
                                   [&post](std::size_t node) {
                                       post.push_back(node);
                                   });
    CRPC_CHECK_EQ(depth, post.size());
    bool descending = true;
    for (std::size_t i = 0; i < post.size(); ++i) {
        if (post[i] != depth - 1 - i) {
            descending = false;
            break;
        }
    }
    CRPC_CHECK(descending);

    // in_order on a chain is the reverse of post_order (see the file comment).
    std::vector<std::size_t> in_order;
    in_order.reserve(depth);
    crpc::algorithm::traverse_tree(nodes, 0, visit_order::in_order,
                                   [&in_order](std::size_t node) {
                                       in_order.push_back(node);
                                   });
    CRPC_CHECK_EQ(depth, in_order.size());
    bool in_order_descending = true;
    for (std::size_t i = 0; i < in_order.size(); ++i) {
        if (in_order[i] != depth - 1 - i) {
            in_order_descending = false;
            break;
        }
    }
    CRPC_CHECK(in_order_descending);

    // Level order on a chain is ascending; the work list is the queue.
    std::size_t expected = 0;
    bool level_ok = true;
    std::size_t count = 0;
    crpc::algorithm::traverse_tree(nodes, 0, visit_order::level_order,
                                   [&](std::size_t node) {
                                       if (node != expected) {
                                           level_ok = false;
                                       }
                                       ++expected;
                                       ++count;
                                   });
    CRPC_CHECK_EQ(depth, count);
    CRPC_CHECK(level_ok);
}

// A wide tree: one root with 50000 children. Exercises the sibling chain and
// the in_order "remaining children" path without deep nesting.
CRPC_TEST(tree_wide_root) {
    constexpr std::size_t children = 50000;
    std::vector<tree_node> nodes(children + 1);
    nodes[0].first_child = 1;
    for (std::size_t i = 1; i <= children; ++i) {
        nodes[i].next_sibling = (i < children) ? i + 1 : npos;
    }

    std::vector<std::size_t> pre;
    pre.reserve(children + 1);
    crpc::algorithm::traverse_tree(nodes, 0, visit_order::pre_order,
                                   [&pre](std::size_t node) {
                                       pre.push_back(node);
                                   });
    CRPC_CHECK_EQ(children + 1, pre.size());
    CRPC_CHECK_EQ(0u, pre.front());

    std::vector<std::size_t> post;
    post.reserve(children + 1);
    crpc::algorithm::traverse_tree(nodes, 0, visit_order::post_order,
                                   [&post](std::size_t node) {
                                       post.push_back(node);
                                   });
    CRPC_CHECK_EQ(children + 1, post.size());
    CRPC_CHECK_EQ(0u, post.back());

    // in_order under the left-child/right-sibling reading: the first child's
    // subtree, then the node, then the sibling subtrees. Every child is a leaf,
    // so the sequence is 1, 0, 2, 3, ..., children.
    std::vector<std::size_t> in_order;
    in_order.reserve(children + 1);
    crpc::algorithm::traverse_tree(nodes, 0, visit_order::in_order,
                                   [&in_order](std::size_t node) {
                                       in_order.push_back(node);
                                   });
    CRPC_CHECK_EQ(children + 1, in_order.size());
    CRPC_CHECK_EQ(1u, in_order[0]);
    CRPC_CHECK_EQ(0u, in_order[1]);
    bool in_order_ascending = true;
    for (std::size_t i = 2; i < in_order.size(); ++i) {
        if (in_order[i] != i) {
            in_order_ascending = false;
            break;
        }
    }
    CRPC_CHECK(in_order_ascending);
}

// Malformed sibling cycle: 0's child list is 1 -> 0 -> 1 -> ... The traversal
// must terminate (bounded by the node count) and visit each node once.
CRPC_TEST(tree_sibling_cycle_terminates) {
    std::vector<tree_node> nodes(3);
    nodes[0].first_child = 1;
    nodes[1].next_sibling = 0; // cycle back to the parent
    nodes[2].first_child = npos;

    std::size_t visits = 0;
    crpc::algorithm::traverse_tree(nodes, 0, visit_order::pre_order,
                                   [&visits](std::size_t) { ++visits; });
    CRPC_CHECK_EQ(2u, visits);

    visits = 0;
    crpc::algorithm::traverse_tree(nodes, 0, visit_order::level_order,
                                   [&visits](std::size_t) { ++visits; });
    CRPC_CHECK_EQ(2u, visits);
}

// A DAG (the same node reachable from two parents) is visited exactly once.
CRPC_TEST(tree_shared_child_visited_once) {
    std::vector<tree_node> nodes(4);
    nodes[0].first_child = 1;
    nodes[1].next_sibling = 2;
    nodes[1].first_child = 3;
    nodes[2].first_child = 3; // node 3 has two parents

    std::size_t visits = 0;
    crpc::algorithm::traverse_tree(nodes, 0, visit_order::pre_order,
                                   [&visits](std::size_t) { ++visits; });
    CRPC_CHECK_EQ(4u, visits);
}

CRPC_TEST(tree_generic_matches_adjacency) {
    // The same shape as sample_tree, but through the callback interface and
    // with arbitrary (sparse) node ids.
    const auto children = [](std::size_t node, std::vector<std::size_t>& out) {
        out.clear();
        switch (node) {
        case 100:
            out = {200, 300, 400};
            break;
        case 200:
            out = {500, 600};
            break;
        case 400:
            out = {700};
            break;
        default:
            break;
        }
    };

    std::vector<std::size_t> pre;
    crpc::algorithm::traverse_generic(
        100, children, visit_order::pre_order,
        [&pre](std::size_t node) { pre.push_back(node); });
    check_sequence(pre, {100, 200, 500, 600, 300, 400, 700});

    std::vector<std::size_t> post;
    crpc::algorithm::traverse_generic(
        100, children, visit_order::post_order,
        [&post](std::size_t node) { post.push_back(node); });
    check_sequence(post, {500, 600, 200, 300, 700, 400, 100});

    std::vector<std::size_t> level;
    crpc::algorithm::traverse_generic(
        100, children, visit_order::level_order,
        [&level](std::size_t node) { level.push_back(node); });
    check_sequence(level, {100, 200, 300, 400, 500, 600, 700});

    std::vector<std::size_t> in_order;
    crpc::algorithm::traverse_generic(
        100, children, visit_order::in_order,
        [&in_order](std::size_t node) { in_order.push_back(node); });
    check_sequence(in_order, {500, 200, 600, 100, 300, 700, 400});
}

CRPC_TEST(tree_generic_cycle_terminates) {
    const auto children = [](std::size_t node, std::vector<std::size_t>& out) {
        out.clear();
        if (node < 3) {
            out.push_back(node + 1);
        } else {
            out.push_back(0);
        }
    };
    for (const visit_order order :
         {visit_order::pre_order, visit_order::in_order,
          visit_order::post_order, visit_order::level_order}) {
        std::size_t visits = 0;
        crpc::algorithm::traverse_generic(
            0, children, order, [&visits](std::size_t) { ++visits; });
        CRPC_CHECK_EQ(4u, visits);
    }
}

CRPC_TEST(tree_generic_deep_chain) {
    constexpr std::size_t depth = 100000;
    const auto children = [](std::size_t node, std::vector<std::size_t>& out) {
        out.clear();
        if (node + 1 < depth) {
            out.push_back(node + 1);
        }
    };
    std::size_t visits = 0;
    std::size_t last = 0;
    crpc::algorithm::traverse_generic(
        0, children, visit_order::pre_order, [&](std::size_t node) {
            ++visits;
            last = node;
        });
    CRPC_CHECK_EQ(depth, visits);
    CRPC_CHECK_EQ(depth - 1, last);
}

CRPC_TEST_MAIN()
