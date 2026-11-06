// CRPC -- binary heaps: the two sift primitives, the std-style free functions
// (make_heap / push_heap / pop_heap) and an owning heap container.
//
// Ordering convention (identical to the std counterparts):
//
//   comp(parent, child) == true means "parent must be below child", so the
//   root is the element for which comp(root, x) is never true. With the default
//   `std::less<>` this is a max-heap (parent >= child); pass `std::greater<>`
//   to get a min-heap. Every algorithm in this header compares with
//   `comp(a, b)` when a is the lower-priority element.
//
// Iterators must be random access. `sift_up`, `sift_down`, `make_heap`,
// `push_heap` and `pop_heap` are `O(log n)` / `O(n)` and use `std::iter_swap`,
// so the element type only has to be move-constructible, move-assignable and
// swappable -- not copyable.
//
// `binary_heap` borrows nothing and owns its storage. `top()` returns
// `const T*` and a null pointer when the heap is empty: an empty heap is a
// normal state, not a programming error, so this API has no precondition and
// no undefined behaviour. The returned pointer is invalidated by every
// mutating call (`push`, `try_pop`, `clear`, `reserve` is safe).
#ifndef CRPC_ALGORITHM_HEAP_HPP
#define CRPC_ALGORITHM_HEAP_HPP

#include <cstddef>
#include <functional>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

namespace crpc {
namespace algorithm {

namespace detail {

template <typename It>
using heap_iterator_category =
    typename std::iterator_traits<It>::iterator_category;

template <typename It>
constexpr bool heap_is_random_access =
    std::is_base_of<std::random_access_iterator_tag,
                    heap_iterator_category<It>>::value;

} // namespace detail

// Restores the heap property on the path from `index` to the root. Precondition:
// every element on that path except `*first[index]` already satisfies the heap
// property (i.e. the new element was just appended). `index` must be < the
// number of elements reachable from `first`.
template <typename It, typename Compare>
void sift_up(It first, std::size_t index, Compare comp) {
    static_assert(detail::heap_is_random_access<It>,
                  "crpc::algorithm::sift_up requires a random-access iterator");
    while (index != 0) {
        const std::size_t parent = (index - 1) / 2;
        if (!comp(*(first + parent), *(first + index))) {
            break;
        }
        std::iter_swap(first + parent, first + index);
        index = parent;
    }
}

// Restores the heap property in the subtree rooted at `index`, assuming both
// child subtrees already satisfy it. `count` is the number of elements in the
// heap (the range is `[first, first + count)`).
template <typename It, typename Compare>
void sift_down(It first, std::size_t count, std::size_t index, Compare comp) {
    static_assert(detail::heap_is_random_access<It>,
                  "crpc::algorithm::sift_down requires a random-access iterator");
    for (;;) {
        const std::size_t left = index * 2 + 1;
        if (left >= count) {
            break;
        }
        const std::size_t right = left + 1;
        std::size_t best = left;
        if (right < count && comp(*(first + best), *(first + right))) {
            best = right;
        }
        if (!comp(*(first + index), *(first + best))) {
            break;
        }
        std::iter_swap(first + index, first + best);
        index = best;
    }
}

// Rearranges `[first, last)` into a heap in O(n) (Floyd's bottom-up build).
template <typename It, typename Compare = std::less<>>
void make_heap(It first, It last, Compare comp = {}) {
    static_assert(detail::heap_is_random_access<It>,
                  "crpc::algorithm::make_heap requires a random-access iterator");
    const std::size_t count = static_cast<std::size_t>(last - first);
    if (count < 2) {
        return;
    }
    for (std::size_t index = count / 2; index-- > 0;) {
        sift_down(first, count, index, comp);
    }
}

// `[first, last - 1)` must already be a heap; `*(last - 1)` is the element that
// was just appended. On return the whole range is a heap. Precondition:
// `first != last`.
template <typename It, typename Compare = std::less<>>
void push_heap(It first, It last, Compare comp = {}) {
    static_assert(detail::heap_is_random_access<It>,
                  "crpc::algorithm::push_heap requires a random-access iterator");
    sift_up(first, static_cast<std::size_t>(last - first - 1), comp);
}

// Swaps the root to `last - 1` and restores the heap on `[first, last - 1)`.
// After the call the largest (per `comp`) element is at `last - 1`, which is the
// step `heap_sort` repeats. Precondition: `first != last`.
template <typename It, typename Compare = std::less<>>
void pop_heap(It first, It last, Compare comp = {}) {
    static_assert(detail::heap_is_random_access<It>,
                  "crpc::algorithm::pop_heap requires a random-access iterator");
    const std::size_t count = static_cast<std::size_t>(last - first);
    if (count < 2) {
        return;
    }
    std::iter_swap(first, last - 1);
    sift_down(first, count - 1, 0, comp);
}

// Owning binary heap. With `std::less<>` the root is the largest element; with
// `std::greater<>` it is the smallest. Not thread-safe; that matches every
// other mutable container in the library.
template <typename T, typename Compare = std::less<>>
class binary_heap {
public:
    using value_type = T;
    using size_type = std::size_t;

    explicit binary_heap(Compare comp = {}) : comp_(std::move(comp)) {}

    // Takes ownership of `data` and heapifies it in place, O(n) (no per-element
    // insert). The vector is moved in, so passing a temporary is cheap.
    explicit binary_heap(std::vector<T> data, Compare comp = {})
        : data_(std::move(data)), comp_(std::move(comp)) {
        // Qualified for the same ADL reason as heap_sort: std::make_heap is
        // visible through the vector iterator and makes the call ambiguous.
        crpc::algorithm::make_heap(data_.begin(), data_.end(), comp_);
    }

    void push(T value) {
        data_.push_back(std::move(value));
        sift_up(data_.begin(), data_.size() - 1, comp_);
    }

    // Moves the root into `out` and removes it. Returns false (leaving `out`
    // untouched) when the heap is empty, so no precondition and no UB.
    [[nodiscard]] bool try_pop(T& out) {
        if (data_.empty()) {
            return false;
        }
        out = std::move(data_.front());
        if (data_.size() > 1) {
            data_.front() = std::move(data_.back());
        }
        data_.pop_back();
        if (!data_.empty()) {
            sift_down(data_.begin(), data_.size(), 0, comp_);
        }
        return true;
    }

    // Borrowed pointer to the root, or nullptr when the heap is empty.
    // Invalidated by push(), try_pop() and clear().
    [[nodiscard]] const T* top() const noexcept {
        return data_.empty() ? nullptr : &data_.front();
    }

    void clear() noexcept { data_.clear(); }

    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }

    void reserve(std::size_t capacity) { data_.reserve(capacity); }

private:
    std::vector<T> data_;
    Compare comp_;
};

} // namespace algorithm
} // namespace crpc

#endif // CRPC_ALGORITHM_HEAP_HPP
