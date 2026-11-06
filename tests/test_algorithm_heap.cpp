// CRPC -- tests for crpc/algorithm/heap.hpp.
//
// The reference for the owning container and the free functions is
// std::priority_queue / std::make_heap / std::push_heap / std::pop_heap: every
// sequence comparison is "pop everything and compare with the standard
// container", so an ordering bug shows up as a mismatched sequence rather than
// as a plausible-looking permutation.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include <crpc/algorithm/heap.hpp>

#include "test_framework.hpp"

namespace {

std::uint64_t xorshift64(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// Drains a binary_heap into a vector by repeated try_pop.
template <typename Heap>
std::vector<typename Heap::value_type> drain(Heap& heap) {
    std::vector<typename Heap::value_type> out;
    typename Heap::value_type value{};
    while (heap.try_pop(value)) {
        out.push_back(value);
    }
    return out;
}

// Drains a std::priority_queue into a vector.
template <typename Queue>
std::vector<typename Queue::value_type> drain_queue(Queue& queue) {
    std::vector<typename Queue::value_type> out;
    while (!queue.empty()) {
        out.push_back(queue.top());
        queue.pop();
    }
    return out;
}

struct task {
    int priority = 0;
    std::string name;
};

} // namespace

CRPC_TEST(heap_empty_behavior) {
    crpc::algorithm::binary_heap<int> heap;
    CRPC_CHECK(heap.empty());
    CRPC_CHECK_EQ(0u, heap.size());
    // top() on an empty heap is a normal state and yields nullptr; it is not a
    // precondition violation. (The header documents this choice.)
    CRPC_CHECK(heap.top() == nullptr);

    int out = 12345;
    CRPC_CHECK_FALSE(heap.try_pop(out));
    CRPC_CHECK_EQ(12345, out); // untouched on failure

    heap.push(5);
    CRPC_CHECK_FALSE(heap.empty());
    CRPC_CHECK_EQ(1u, heap.size());
    CRPC_CHECK(heap.top() != nullptr);
    CRPC_CHECK_EQ(5, *heap.top());
    CRPC_CHECK(heap.try_pop(out));
    CRPC_CHECK_EQ(5, out);
    CRPC_CHECK(heap.top() == nullptr);

    heap.clear();
    CRPC_CHECK(heap.empty());
}

CRPC_TEST(heap_make_heap_from_vector_max) {
    // Reference: std::priority_queue built from the same values pops them in
    // descending order. The O(n) constructor must agree.
    const std::vector<int> values{5, 1, 9, 3, 7, 3, 0, -2, 8, 8, 6};
    std::priority_queue<int> reference(values.begin(), values.end());

    crpc::algorithm::binary_heap<int> heap(values);
    CRPC_CHECK_EQ(values.size(), heap.size());
    CRPC_CHECK_EQ(9, *heap.top());
    CRPC_CHECK(drain(heap) == drain_queue(reference));
    CRPC_CHECK(heap.empty());
}

CRPC_TEST(heap_push_pop_max_order) {
    std::priority_queue<int> reference;
    crpc::algorithm::binary_heap<int> heap;

    std::uint64_t state = 0xABCDEF0123456789ull;
    for (int i = 0; i < 2000; ++i) {
        const int value = static_cast<int>(xorshift64(state) % 2001) - 1000;
        heap.push(value);
        reference.push(value);
    }
    CRPC_CHECK_EQ(reference.size(), heap.size());
    CRPC_CHECK_EQ(reference.top(), *heap.top());
    CRPC_CHECK(drain(heap) == drain_queue(reference));
}

CRPC_TEST(heap_min_heap_with_greater) {
    // A min-heap is binary_heap<T, std::greater<>>; the reference is
    // std::priority_queue<T, vector<T>, greater<T>>.
    std::priority_queue<int, std::vector<int>, std::greater<>> reference;
    crpc::algorithm::binary_heap<int, std::greater<>> heap;

    std::uint64_t state = 0x0FEDCBA987654321ull;
    for (int i = 0; i < 2000; ++i) {
        const int value = static_cast<int>(xorshift64(state) % 2001) - 1000;
        heap.push(value);
        reference.push(value);
    }
    CRPC_CHECK_EQ(reference.top(), *heap.top());

    std::vector<int> expected = drain_queue(reference);
    std::vector<int> actual = drain(heap);
    CRPC_CHECK(actual == expected);
    // Sanity: the drain sequence is non-decreasing.
    CRPC_CHECK(std::is_sorted(actual.begin(), actual.end()));
}

CRPC_TEST(heap_custom_comparator_struct) {
    // Largest priority first; the name is only payload. A custom comparator is
    // part of the type, exactly as with std::priority_queue.
    std::vector<task> tasks{{1, "low"}, {9, "urgent"}, {5, "mid"}};
    const auto by_priority = [](const task& a, const task& b) {
        return a.priority < b.priority;
    };
    crpc::algorithm::binary_heap<task, decltype(by_priority)> heap(
        std::move(tasks), by_priority);
    CRPC_CHECK_EQ(9, heap.top()->priority);
    CRPC_CHECK_EQ(std::string("urgent"), heap.top()->name);

    task out;
    CRPC_CHECK(heap.try_pop(out));
    CRPC_CHECK_EQ(9, out.priority);
    CRPC_CHECK(heap.try_pop(out));
    CRPC_CHECK_EQ(5, out.priority);
    CRPC_CHECK(heap.try_pop(out));
    CRPC_CHECK_EQ(1, out.priority);
    CRPC_CHECK_FALSE(heap.try_pop(out));
}

CRPC_TEST(heap_free_functions_match_std) {
    const std::vector<int> values{4, 1, 3, 2, 9, 7, 8, 6, 5, 0};

    std::vector<int> reference = values;
    std::make_heap(reference.begin(), reference.end());
    std::vector<int> actual = values;
    crpc::algorithm::make_heap(actual.begin(), actual.end(), std::less<>());
    CRPC_CHECK(actual == reference);
    CRPC_CHECK_EQ(reference.front(), actual.front());

    // push_heap: append and sift up.
    reference.push_back(11);
    std::push_heap(reference.begin(), reference.end());
    actual.push_back(11);
    crpc::algorithm::push_heap(actual.begin(), actual.end(), std::less<>());
    CRPC_CHECK(actual == reference);

    // pop_heap: the standard version swaps the root to the back and re-heaps
    // the prefix, exactly like the free function here.
    std::pop_heap(reference.begin(), reference.end());
    crpc::algorithm::pop_heap(actual.begin(), actual.end(), std::less<>());
    CRPC_CHECK(actual == reference);
    CRPC_CHECK_EQ(11, actual.back());

    // Min-heap variant with greater<>.
    std::vector<int> min_reference = values;
    std::make_heap(min_reference.begin(), min_reference.end(),
                   std::greater<>());
    std::vector<int> min_actual = values;
    crpc::algorithm::make_heap(min_actual.begin(), min_actual.end(),
                               std::greater<>());
    CRPC_CHECK(min_actual == min_reference);
    CRPC_CHECK_EQ(0, min_actual.front());
}

CRPC_TEST(heap_free_functions_on_single_and_empty_ranges) {
    std::vector<int> empty;
    crpc::algorithm::make_heap(empty.begin(), empty.end(), std::less<>());
    crpc::algorithm::pop_heap(empty.begin(), empty.end(), std::less<>());
    CRPC_CHECK(empty.empty());

    std::vector<int> single{42};
    crpc::algorithm::make_heap(single.begin(), single.end(), std::less<>());
    CRPC_CHECK_EQ(42, single.front());
    crpc::algorithm::pop_heap(single.begin(), single.end(), std::less<>());
    CRPC_CHECK_EQ(42, single.front());

    std::vector<int> two{1, 2};
    crpc::algorithm::make_heap(two.begin(), two.end(), std::less<>());
    CRPC_CHECK_EQ(2, two.front());
    crpc::algorithm::pop_heap(two.begin(), two.end(), std::less<>());
    CRPC_CHECK_EQ(2, two.back());
    CRPC_CHECK_EQ(1, two.front());
}

CRPC_TEST(heap_sort_order_matches_reference_sort) {
    // Sorting through the heap primitives must equal std::sort's result.
    std::uint64_t state = 0x5555AAAA12344321ull;
    for (int round = 0; round < 20; ++round) {
        std::vector<int> input(static_cast<std::size_t>(round * 7 + 1));
        for (int& value : input) {
            value = static_cast<int>(xorshift64(state) % 1000);
        }
        std::vector<int> expected = input;
        std::sort(expected.begin(), expected.end());

        std::vector<int> actual = input;
        crpc::algorithm::make_heap(actual.begin(), actual.end(), std::less<>());
        std::vector<int> ordered;
        while (!actual.empty()) {
            ordered.push_back(actual.front());
            crpc::algorithm::pop_heap(actual.begin(), actual.end(),
                                      std::less<>());
            actual.pop_back();
        }
        std::reverse(ordered.begin(), ordered.end());
        CRPC_CHECK(ordered == expected);
    }
}

CRPC_TEST(heap_move_only_elements) {
    const auto by_value = [](const std::unique_ptr<int>& a,
                             const std::unique_ptr<int>& b) { return *a < *b; };
    crpc::algorithm::binary_heap<std::unique_ptr<int>, decltype(by_value)> heap(
        by_value);
    heap.push(std::make_unique<int>(3));
    heap.push(std::make_unique<int>(9));
    heap.push(std::make_unique<int>(5));
    CRPC_CHECK_EQ(9, **heap.top());

    std::unique_ptr<int> out;
    CRPC_CHECK(heap.try_pop(out));
    CRPC_CHECK_EQ(9, *out);
    CRPC_CHECK(heap.try_pop(out));
    CRPC_CHECK_EQ(5, *out);
    CRPC_CHECK(heap.try_pop(out));
    CRPC_CHECK_EQ(3, *out);
    CRPC_CHECK_FALSE(heap.try_pop(out));
}

CRPC_TEST(heap_reserve_and_size) {
    crpc::algorithm::binary_heap<int> heap;
    heap.reserve(1000);
    CRPC_CHECK(heap.empty());
    for (int value = 0; value < 1000; ++value) {
        heap.push(value);
    }
    CRPC_CHECK_EQ(1000u, heap.size());
    CRPC_CHECK_EQ(999, *heap.top());
    heap.clear();
    CRPC_CHECK_EQ(0u, heap.size());
    CRPC_CHECK(heap.top() == nullptr);
}

CRPC_TEST(heap_initializer_list_construction) {
    crpc::algorithm::binary_heap<int> heap({3, 1, 4, 1, 5, 9, 2, 6});
    CRPC_CHECK_EQ(9, *heap.top());
    std::vector<int> actual = drain(heap);
    CRPC_CHECK(std::is_sorted(actual.rbegin(), actual.rend()));
    CRPC_CHECK_EQ(8u, actual.size());
}

CRPC_TEST_MAIN()
