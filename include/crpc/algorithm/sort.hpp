// CRPC -- in-place sorting algorithms: introspective sort, quicksort, heap
// sort and an LSD radix sort for integral keys.
//
// Every iterator overload requires random-access iterators and compares with
// `comp(a, b) == true` meaning "a must come before b"; the default
// `std::less<>` sorts ascending. Range overloads (`intro_sort(range)`, ...)
// take any lvalue range with begin()/end(), including C arrays.
//
// Stability, stated up front:
//
//   intro_sort   NOT stable. Equal elements may be reordered.
//   quick_sort   NOT stable.
//   heap_sort    NOT stable.
//   radix_sort   STABLE: the LSD counting passes preserve the input order of
//                equal keys. With a plain integral element type equal elements
//                are indistinguishable, so this is a property of the method
//                rather than something a caller can observe through this API.
//
// Complexity:
//
//   intro_sort   O(n log n) worst case, O(1) extra memory (the recursion is
//                pushed onto the smaller side only and the depth budget is
//                2*floor(log2(n)); when it runs out the range is heap-sorted,
//                which is what removes the quicksort O(n^2) worst case).
//   quick_sort   O(n log n) expected, O(n^2) adversarial worst case. The
//                three-way partition makes all-equal and duplicate-heavy input
//                O(n log n) (indeed O(n) for a single all-equal range), and the
//                tail-recursion elimination keeps stack depth O(log n) on the
//                inputs where the split is balanced. It is NOT the protected
//                sort; use intro_sort when adversarial input is possible.
//   heap_sort    O(n log n) worst and average, O(1) extra memory.
//   radix_sort   O(b * (n + 256)) where b is sizeof(value_type); O(n) extra
//                memory for the ping-pong buffer. Only integral element types
//                (signed, unsigned, and character types) are accepted; floating
//                point and string keys are rejected at compile time.
//
// `intro_sort` and `quick_sort` share the same three-way partition. It compares
// against `*first` while leaving that element in place, so the pivot never has
// to be copied and move-only element types are supported.
#ifndef CRPC_ALGORITHM_SORT_HPP
#define CRPC_ALGORITHM_SORT_HPP

#include <array>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include <crpc/algorithm/heap.hpp>

namespace crpc {
namespace algorithm {

// Forward declaration: the introspective sort falls back to this when its
// depth budget is exhausted.
template <typename It, typename Compare = std::less<>>
void heap_sort(It first, It last, Compare comp = {});

namespace detail {

template <typename It>
using sort_iterator_category =
    typename std::iterator_traits<It>::iterator_category;

template <typename It>
constexpr bool sort_is_random_access =
    std::is_base_of<std::random_access_iterator_tag,
                    sort_iterator_category<It>>::value;

// Ranges at or below this size are finished with insertion sort: below it the
// partition overhead dominates and insertion sort is faster in practice.
constexpr std::ptrdiff_t insertion_sort_threshold = 16;

// floor(log2(n)); 0 for n == 0 and n == 1.
inline int floor_log2(std::size_t value) noexcept {
    int result = 0;
    while (value > 1) {
        value >>= 1;
        ++result;
    }
    return result;
}

template <typename It, typename Compare>
void insertion_sort(It first, It last, Compare comp) {
    static_assert(sort_is_random_access<It>,
                  "crpc::algorithm insertion sort requires a random-access "
                  "iterator");
    if (first == last) {
        return;
    }
    for (It current = first + 1; current != last; ++current) {
        auto value = std::move(*current);
        It hole = current;
        while (hole != first && comp(value, *(hole - 1))) {
            *hole = std::move(*(hole - 1));
            --hole;
        }
        *hole = std::move(value);
    }
}

// Single forward scan over adjacent pairs: true when the range is already
// non-descending under comp. The all-equal range satisfies this too, so one
// scan covers both friendly distributions. Cost is n-1 comparisons when the
// answer is yes; when it is no, the first out-of-order pair ends the scan
// (1-2 comparisons on average for unordered input), which keeps the overhead
// on random and adversarial input in the noise. Only comparisons are made --
// no element is read as an rvalue and nothing is swapped, so exception and
// move-only semantics are untouched.
template <typename It, typename Compare>
bool is_sorted_forward(It first, It last, Compare& comp) {
    if (last - first < 2) {
        return true;
    }
    It prev = first;
    for (It current = first + 1; current != last; ++current) {
        if (comp(*current, *prev)) {
            return false;
        }
        prev = current;
    }
    return true;
}

// Orders `*first`, `*mid`, `*last_minus_one` so that `*first <= *mid <=
// *last_minus_one`, then swaps the median (`*mid`) into `*first`, where the
// partition expects it. Equal elements are fine: any of them can act as pivot.
template <typename It, typename Compare>
void median_of_three_to_first(It first, It mid, It last_minus_one,
                              Compare& comp) {
    if (comp(*mid, *first)) {
        std::iter_swap(mid, first);
    }
    if (comp(*last_minus_one, *first)) {
        std::iter_swap(last_minus_one, first);
    }
    if (comp(*last_minus_one, *mid)) {
        std::iter_swap(last_minus_one, mid);
    }
    // *first <= *mid <= *last_minus_one; *mid is the median.
    std::iter_swap(first, mid);
}

// Three-way partition around `*first` (the pivot value stays at `first` for the
// whole scan, so it never has to be copied). Returns the half-open range of
// elements equal to the pivot:
//
//   [first, result.first)   less than the pivot
//   [result.first, result.second)   equal to the pivot
//   [result.second, last)   greater than the pivot
//
// A three-way partition is what makes duplicate-heavy input cheap: equal
// elements land in the middle and are never partitioned again.
template <typename It, typename Compare>
std::pair<It, It> partition_three_way(It first, It last, Compare& comp) {
    It less_end = first + 1; // one past the last element known to be less
    It scan = first + 1;
    It greater_begin = last; // one before the first element known to be greater
    while (scan < greater_begin) {
        if (comp(*scan, *first)) {
            std::iter_swap(less_end, scan);
            ++less_end;
            ++scan;
        } else if (comp(*first, *scan)) {
            --greater_begin;
            std::iter_swap(scan, greater_begin);
        } else {
            ++scan;
        }
    }
    --less_end; // the pivot itself belongs at the end of the less range
    std::iter_swap(first, less_end);
    return {less_end, greater_begin};
}

// Recurses into the smaller partition and loops on the larger one, so the call
// stack is O(log n) even on a badly split input (the classic tail-recursion
// elimination). `depth_limit` is the remaining heap-sort budget: reaching zero
// switches the current range to heap sort.
template <typename It, typename Compare>
void intro_sort_impl(It first, It last, Compare& comp, int depth_limit) {
    if (last - first <= insertion_sort_threshold) {
        insertion_sort(first, last, comp);
        return;
    }
    for (;;) {
        if (depth_limit == 0) {
            heap_sort(first, last, comp);
            return;
        }
        --depth_limit;
        median_of_three_to_first(first, first + (last - first) / 2, last - 1,
                                 comp);
        const std::pair<It, It> equal_range =
            partition_three_way(first, last, comp);
        const auto left_size = equal_range.first - first;
        const auto right_size = last - equal_range.second;
        if (left_size < right_size) {
            intro_sort_impl(first, equal_range.first, comp, depth_limit);
            first = equal_range.second;
        } else {
            intro_sort_impl(equal_range.second, last, comp, depth_limit);
            last = equal_range.first;
        }
        if (last - first <= insertion_sort_threshold) {
            insertion_sort(first, last, comp);
            return;
        }
    }
}

template <typename It, typename Compare>
void quick_sort_impl(It first, It last, Compare& comp) {
    const auto size = last - first;
    if (size <= insertion_sort_threshold) {
        insertion_sort(first, last, comp);
        return;
    }
    for (;;) {
        median_of_three_to_first(first, first + (last - first) / 2, last - 1,
                                 comp);
        const std::pair<It, It> equal_range = partition_three_way(first, last, comp);
        const auto left_size = equal_range.first - first;
        const auto right_size = last - equal_range.second;
        if (left_size < right_size) {
            quick_sort_impl(first, equal_range.first, comp);
            first = equal_range.second;
        } else {
            quick_sort_impl(equal_range.second, last, comp);
            last = equal_range.first;
        }
        if (last - first <= insertion_sort_threshold) {
            break;
        }
    }
    insertion_sort(first, last, comp);
}

// Integer digit extraction for radix_sort. The sign bit is flipped for signed
// types so that the unsigned bit pattern orders negative values before
// non-negative ones (two's-complement encoding is assumed; C++17 guarantees
// modulo conversion and every supported target is two's complement).
template <typename Value>
struct radix_traits {
    static_assert(std::is_integral<Value>::value,
                  "crpc::algorithm::radix_sort only supports integral types");
    using unsigned_type = typename std::make_unsigned<Value>::type;

    static unsigned_type key_of(Value value) noexcept {
        unsigned_type bits = static_cast<unsigned_type>(value);
        if constexpr (std::is_signed<Value>::value) {
            bits ^= static_cast<unsigned_type>(
                unsigned_type(1) << (sizeof(unsigned_type) * 8 - 1));
        }
        return bits;
    }
};

template <typename It>
void radix_sort_impl(It first, It last) {
    using value_type = typename std::iterator_traits<It>::value_type;
    using unsigned_type = typename radix_traits<value_type>::unsigned_type;
    const std::size_t count = static_cast<std::size_t>(last - first);
    if (count < 2) {
        return;
    }

    // Ping-pong buffers: one pass per byte of the key. `in_place` tracks which
    // side currently holds the partially sorted data.
    std::vector<value_type> scratch(count);
    bool input_holds_current = true;
    std::array<std::size_t, 256> offsets{};

    const std::size_t passes = sizeof(unsigned_type);
    for (std::size_t pass = 0; pass < passes; ++pass) {
        const unsigned shift = static_cast<unsigned>(pass * 8);
        auto digit_of = [shift](value_type value) noexcept -> std::size_t {
            return static_cast<std::size_t>(
                (radix_traits<value_type>::key_of(value) >> shift) &
                static_cast<unsigned_type>(0xFF));
        };

        offsets.fill(0);
        for (std::size_t index = 0; index < count; ++index) {
            const value_type value =
                input_holds_current ? *(first + index) : scratch[index];
            ++offsets[digit_of(value)];
        }
        std::size_t running = 0;
        for (std::size_t bucket = 0; bucket < offsets.size(); ++bucket) {
            const std::size_t frequency = offsets[bucket];
            offsets[bucket] = running;
            running += frequency;
        }
        // Stable scatter: elements with equal digits keep their relative order.
        for (std::size_t index = 0; index < count; ++index) {
            const value_type value =
                input_holds_current ? *(first + index) : scratch[index];
            const std::size_t bucket = digit_of(value);
            if (input_holds_current) {
                scratch[offsets[bucket]] = value;
            } else {
                *(first + offsets[bucket]) = value;
            }
            ++offsets[bucket];
        }
        input_holds_current = !input_holds_current;
    }

    if (!input_holds_current) {
        // The last finished pass wrote to `scratch`; move it back.
        for (std::size_t index = 0; index < count; ++index) {
            *(first + index) = scratch[index];
        }
    }
}

} // namespace detail

// Introspective sort: median-of-three three-way quicksort, insertion sort for
// small ranges, and a heap-sort fallback once the recursion budget of
// 2*floor(log2(n)) partitions is exhausted. A single forward scan before any
// partitioning returns immediately on already non-descending input (sorted and
// all-equal ranges), which makes those distributions O(n). Worst case
// O(n log n); not stable. The scan never replaces the heap-sort fallback: on
// anything the scan rejects, the full introsort path -- depth budget and heap
// sort included -- runs as before.
template <typename It, typename Compare = std::less<>>
void intro_sort(It first, It last, Compare comp = {}) {
    static_assert(detail::sort_is_random_access<It>,
                  "crpc::algorithm::intro_sort requires random-access "
                  "iterators");
    const auto count = last - first;
    if (count < 2) {
        return;
    }
    if (detail::is_sorted_forward(first, last, comp)) {
        return;
    }
    const int depth_limit =
        2 * detail::floor_log2(static_cast<std::size_t>(count));
    detail::intro_sort_impl(first, last, comp, depth_limit);
}

// Three-way quicksort with median-of-three pivots and tail-recursion
// elimination. Average O(n log n), adversarial worst case O(n^2); not stable.
// Use intro_sort when the input may be adversarial.
template <typename It, typename Compare = std::less<>>
void quick_sort(It first, It last, Compare comp = {}) {
    static_assert(detail::sort_is_random_access<It>,
                  "crpc::algorithm::quick_sort requires random-access "
                  "iterators");
    if (last - first < 2) {
        return;
    }
    detail::quick_sort_impl(first, last, comp);
}

// Heap sort built on the primitives in heap.hpp. O(n log n) with O(1) extra
// memory; not stable.
template <typename It, typename Compare>
void heap_sort(It first, It last, Compare comp) {
    static_assert(detail::sort_is_random_access<It>,
                  "crpc::algorithm::heap_sort requires random-access "
                  "iterators");
    // Qualified: `std::make_heap`/`std::pop_heap` are found by ADL through the
    // iterator type and would otherwise make these calls ambiguous on MSVC.
    crpc::algorithm::make_heap(first, last, comp);
    for (It end = last; end - first > 1; --end) {
        crpc::algorithm::pop_heap(first, end, comp);
    }
}

// LSD radix sort for integral element types. Stable; O(b * (n + 256)) time and
// O(n) extra memory, where b == sizeof(value_type). Signed keys are ordered
// correctly (the sign bit is flipped before the unsigned digit passes).
//
// Not available for floating-point or string keys: the enable_if below turns
// those calls into "no matching function", and the static_assert in
// radix_traits documents the reason when a non-integral iterator reaches the
// body through another path. `bool` is excluded because `std::make_unsigned`
// is ill-formed for it (and sorting bools is meaningless anyway).
template <typename It,
          typename std::enable_if<
              std::is_integral<typename std::iterator_traits<It>::value_type>::
                      value &&
                  !std::is_same<typename std::iterator_traits<It>::value_type,
                                bool>::value,
              int>::type = 0>
void radix_sort(It first, It last) {
    static_assert(detail::sort_is_random_access<It>,
                  "crpc::algorithm::radix_sort requires random-access "
                  "iterators");
    detail::radix_sort_impl(first, last);
}

// Generic overloads that accept a range instead of an iterator pair. They
// forward to the iterator overloads above, so they inherit the same
// requirements and the same container support.
namespace detail {

// A `Range` here means "something std::begin works on". The trait excludes
// iterators (and pointers) on purpose: without it, a call with two lvalue
// iterators would silently pick the range overload and treat the second
// iterator as the comparator.
template <typename T, typename = void>
struct is_range : std::false_type {};

template <typename T>
struct is_range<T, std::void_t<decltype(std::begin(std::declval<T&>())),
                               decltype(std::end(std::declval<T&>()))>>
    : std::true_type {};

template <typename Range>
using range_value_t =
    typename std::iterator_traits<decltype(std::begin(
        std::declval<Range&>()))>::value_type;

} // namespace detail

template <typename Range, typename Compare = std::less<>,
          typename std::enable_if<detail::is_range<Range>::value, int>::type = 0>
void intro_sort(Range& range, Compare comp = {}) {
    intro_sort(std::begin(range), std::end(range), comp);
}

template <typename Range, typename Compare = std::less<>,
          typename std::enable_if<detail::is_range<Range>::value, int>::type = 0>
void quick_sort(Range& range, Compare comp = {}) {
    quick_sort(std::begin(range), std::end(range), comp);
}

template <typename Range, typename Compare = std::less<>,
          typename std::enable_if<detail::is_range<Range>::value, int>::type = 0>
void heap_sort(Range& range, Compare comp = {}) {
    heap_sort(std::begin(range), std::end(range), comp);
}

template <typename Range,
          typename std::enable_if<
              detail::is_range<Range>::value &&
                  std::is_integral<detail::range_value_t<Range>>::value &&
                  !std::is_same<detail::range_value_t<Range>, bool>::value,
              int>::type = 0>
void radix_sort(Range& range) {
    radix_sort(std::begin(range), std::end(range));
}

} // namespace algorithm
} // namespace crpc

#endif // CRPC_ALGORITHM_SORT_HPP
