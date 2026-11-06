// CRPC -- tests for crpc/algorithm/sort.hpp.
//
// Every correctness expectation is checked against std::sort on the same input
// (the standard algorithm is the reference), never against a hand-written
// expected permutation. The comparison-count cases are what make the
// "intro_sort must not degrade to O(n^2)" property testable: a count bound is
// the observable form of the complexity claim, and the adversarial case uses
// McIlroy's antiqsort comparator, whose blow-up is a theorem rather than a
// property of one particular input permutation.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <numeric>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <crpc/algorithm/sort.hpp>

#include "test_framework.hpp"

namespace {

std::uint64_t xorshift64(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// floor(log2(n)), 0 for n < 2.
std::size_t log2_floor(std::size_t n) {
    std::size_t bits = 0;
    while (n > 1) {
        n >>= 1;
        ++bits;
    }
    return bits;
}

// Runs intro_sort, quick_sort and heap_sort on a copy of `input` and checks all
// three against std::sort.
void expect_all_sorts_match_std(std::vector<int> input) {
    std::vector<int> expected = input;
    std::sort(expected.begin(), expected.end());

    std::vector<int> intro = input;
    crpc::algorithm::intro_sort(intro.begin(), intro.end());
    CRPC_CHECK(intro == expected);

    std::vector<int> quick = input;
    crpc::algorithm::quick_sort(quick.begin(), quick.end());
    CRPC_CHECK(quick == expected);

    std::vector<int> heaps = input;
    crpc::algorithm::heap_sort(heaps.begin(), heaps.end());
    CRPC_CHECK(heaps == expected);
}

struct counting_less {
    std::size_t* comparisons = nullptr;
    bool operator()(int a, int b) const {
        ++*comparisons;
        return a < b;
    }
};

// McIlroy's antiqsort adversary ("A Killer Adversary for Quicksort", 1999).
// Values are element ids 0..n-1; a side table starts as "gas" (n) and hands out
// increasing solid values as the sort probes pairs. Against any median-of-three
// quicksort that compares a candidate pivot against elements in order, this
// forces quadratic work; the heap-sort fallback in intro_sort is exactly what
// bounds it. The comparator is deterministic, so the counts are reproducible.
class killer_adversary {
    // The sort takes its comparator by value (as std::sort does), so the
    // adversary's mutable state lives behind a shared_ptr: the copy the sort
    // works on and the caller's handle must observe the same table.
    struct state {
        explicit state(std::size_t count)
            : gas(static_cast<int>(count)),
              value(count, static_cast<int>(count)) {}
        int gas;
        std::vector<int> value;
        int solid = 0;
        int candidate = 0;
        std::size_t comparisons = 0;
    };

public:
    explicit killer_adversary(std::size_t count)
        : state_(std::make_shared<state>(count)) {}

    bool operator()(int a, int b) {
        ++state_->comparisons;
        int& left = state_->value[static_cast<std::size_t>(a)];
        int& right = state_->value[static_cast<std::size_t>(b)];
        if (left == state_->gas && right == state_->gas) {
            if (a == state_->candidate) {
                left = state_->solid++;
            } else {
                right = state_->solid++;
            }
        }
        if (left == state_->gas) {
            state_->candidate = a;
        } else if (right == state_->gas) {
            state_->candidate = b;
        }
        return left < right;
    }

    [[nodiscard]] std::size_t comparisons() const {
        return state_->comparisons;
    }

private:
    std::shared_ptr<state> state_;
};

// Detects whether radix_sort is callable for an element type. Used to prove the
// integral-only constraint rather than describe it in a comment.
template <typename T, typename = void>
struct radix_callable : std::false_type {};

template <typename T>
struct radix_callable<
    T, std::void_t<decltype(crpc::algorithm::radix_sort(
           std::declval<T*>(), std::declval<T*>()))>> : std::true_type {};

} // namespace

CRPC_TEST(sort_empty_and_single) {
    std::vector<int> empty;
    crpc::algorithm::intro_sort(empty.begin(), empty.end());
    crpc::algorithm::quick_sort(empty.begin(), empty.end());
    crpc::algorithm::heap_sort(empty.begin(), empty.end());
    crpc::algorithm::radix_sort(empty.begin(), empty.end());
    CRPC_CHECK(empty.empty());

    std::vector<int> single{42};
    crpc::algorithm::intro_sort(single.begin(), single.end());
    crpc::algorithm::quick_sort(single.begin(), single.end());
    crpc::algorithm::heap_sort(single.begin(), single.end());
    crpc::algorithm::radix_sort(single.begin(), single.end());
    CRPC_CHECK_EQ(42, single.front());
}

CRPC_TEST(sort_two_elements_each_order) {
    std::vector<int> ascending{1, 2};
    std::vector<int> descending{2, 1};
    expect_all_sorts_match_std(ascending);
    expect_all_sorts_match_std(descending);
}

CRPC_TEST(sort_sorted_input) {
    std::vector<int> input(1000);
    std::iota(input.begin(), input.end(), -500);
    expect_all_sorts_match_std(input);
}

CRPC_TEST(sort_reverse_input) {
    std::vector<int> input(1000);
    std::iota(input.rbegin(), input.rend(), -500);
    expect_all_sorts_match_std(input);
}

// A sorted range long enough that a quadratic sort would be unmistakable.
CRPC_TEST(sort_sorted_large_does_not_degrade) {
    constexpr std::size_t count = 100000;
    std::vector<int> input(count);
    std::iota(input.begin(), input.end(), 0);

    std::size_t comparisons = 0;
    crpc::algorithm::intro_sort(input.begin(), input.end(),
                                counting_less{&comparisons});
    CRPC_CHECK(std::is_sorted(input.begin(), input.end()));
    // 2 compares per element per level is the three-way partition's constant;
    // n^2/2 would be 5e9 here. 40*n*log2(n) is a bound a quadratic run cannot
    // approach while leaving ample headroom for implementation tweaks.
    const std::size_t bound = 40 * count * log2_floor(count);
    CRPC_CHECK(comparisons < bound);
}

CRPC_TEST(sort_all_equal) {
    std::vector<int> input(5000, 7);
    expect_all_sorts_match_std(input);

    constexpr std::size_t count = 100000;
    std::vector<int> large(count, 7);
    std::size_t comparisons = 0;
    crpc::algorithm::intro_sort(large.begin(), large.end(),
                                counting_less{&comparisons});
    CRPC_CHECK(std::is_sorted(large.begin(), large.end()));
    // Three-way partition: one pass over the range, two comparisons each.
    CRPC_CHECK(comparisons < 8 * count);
}

CRPC_TEST(sort_many_duplicates) {
    std::vector<int> input(50000);
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    for (int& value : input) {
        value = static_cast<int>(xorshift64(state) % 10);
    }
    expect_all_sorts_match_std(input);

    std::size_t comparisons = 0;
    crpc::algorithm::intro_sort(input.begin(), input.end(),
                                counting_less{&comparisons});
    CRPC_CHECK(std::is_sorted(input.begin(), input.end()));
    // Duplicate keys are partitioned out in one pass, so the count stays well
    // under the distinct-key budget.
    CRPC_CHECK(comparisons < 40 * input.size() * log2_floor(input.size()));
}

CRPC_TEST(sort_random_large) {
    constexpr std::size_t count = 50000;
    std::vector<int> input(count);
    std::uint64_t state = 0xDEADBEEF12345678ull;
    for (int& value : input) {
        value = static_cast<int>(xorshift64(state) % 1000000) - 500000;
    }
    expect_all_sorts_match_std(input);
}

CRPC_TEST(sort_custom_comparator) {
    std::vector<int> input{5, 1, 9, 3, 7, 3, 0, -2};
    std::vector<int> descending = input;
    crpc::algorithm::intro_sort(descending.begin(), descending.end(),
                                std::greater<>());
    CRPC_CHECK(std::is_sorted(descending.begin(), descending.end(),
                              std::greater<>()));

    std::vector<int> quick = input;
    crpc::algorithm::quick_sort(quick.begin(), quick.end(), std::greater<>());
    CRPC_CHECK(std::is_sorted(quick.begin(), quick.end(), std::greater<>()));

    std::vector<int> heaps = input;
    crpc::algorithm::heap_sort(heaps.begin(), heaps.end(), std::greater<>());
    CRPC_CHECK(std::is_sorted(heaps.begin(), heaps.end(), std::greater<>()));

    // A comparator on a projection (sort pairs by second field).
    std::vector<std::pair<int, int>> pairs{
        {1, 30}, {2, 10}, {3, 20}, {4, 10}};
    crpc::algorithm::intro_sort(
        pairs.begin(), pairs.end(),
        [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
            return a.second < b.second;
        });
    CRPC_CHECK_EQ(10, pairs[0].second);
    CRPC_CHECK_EQ(10, pairs[1].second);
    CRPC_CHECK_EQ(20, pairs[2].second);
    CRPC_CHECK_EQ(30, pairs[3].second);
}

CRPC_TEST(sort_range_overloads) {
    std::vector<int> values{4, 2, 5, 1, 3};
    crpc::algorithm::intro_sort(values);
    for (std::size_t i = 0; i < values.size(); ++i) {
        CRPC_CHECK_EQ(static_cast<int>(i) + 1, values[i]);
    }

    int raw[5] = {9, 8, 7, 6, 5};
    crpc::algorithm::quick_sort(raw);
    CRPC_CHECK(std::is_sorted(std::begin(raw), std::end(raw)));

    std::vector<unsigned> unsigned_values{4u, 2u, 5u, 1u, 3u};
    crpc::algorithm::radix_sort(unsigned_values);
    CRPC_CHECK(std::is_sorted(unsigned_values.begin(), unsigned_values.end()));

    std::vector<int> heap_values{3, 1, 2};
    crpc::algorithm::heap_sort(heap_values, std::greater<>());
    CRPC_CHECK(std::is_sorted(heap_values.begin(), heap_values.end(),
                              std::greater<>()));
}

// M1 gate: intro_sort must survive an adversarial comparison order with
// bounded work, and the heap-sort fallback must stay reachable and effective.
// Two intro arms pin the two mechanisms:
//
//   * public entry: the sorted pre-check plus the full introsort path. With
//     this adversary every pre-check scan comparison freezes the scan's
//     second argument and answers "not less", so the public call may
//     legitimately return after n-1 comparisons; the bound covers both that
//     outcome and the full partition path.
//   * direct detail::intro_sort_impl call with the same depth budget the
//     public entry computes: the exact path the pre-check-free entry took,
//     where the depth budget and the heap-sort fallback live. With the
//     fallback this costs ~6*n*log2(n) (measured 5.7-5.9 across n=2^11..2^15);
//     with the fallback removed the same adversary drives it to the quicksort
//     quadratic count (~100*n*log2(n) at this size), which is what turns the
//     arm red.
//
// The quick_sort control is unchanged: the same adversary drives it
// quadratic, proving the adversary itself is still effective and the bounds
// above are not merely loose.
CRPC_TEST(sort_intro_survives_killer_adversary) {
    constexpr std::size_t count = 2000;
    const std::size_t bits = log2_floor(count);

    std::vector<int> input(count);
    std::iota(input.begin(), input.end(), 0);

    killer_adversary killer(count);
    crpc::algorithm::intro_sort(input.begin(), input.end(), killer);
    CRPC_CHECK(killer.comparisons() < 25 * count * bits);

    // Fallback isolation: no pre-check, same depth budget as the public entry.
    std::vector<int> direct(count);
    std::iota(direct.begin(), direct.end(), 0);
    killer_adversary direct_adversary(count);
    {
        std::reference_wrapper<killer_adversary> comp(direct_adversary);
        const int depth_limit = 2 * static_cast<int>(bits);
        crpc::algorithm::detail::intro_sort_impl(direct.begin(), direct.end(),
                                                 comp, depth_limit);
    }
    CRPC_CHECK(direct_adversary.comparisons() < 25 * count * bits);

    std::vector<int> fresh(count);
    std::iota(fresh.begin(), fresh.end(), 0);
    killer_adversary adversary(count);
    crpc::algorithm::quick_sort(fresh.begin(), fresh.end(), adversary);
    CRPC_CHECK(adversary.comparisons() > count * count / 8);
}

CRPC_TEST(sort_radix_unsigned_widths) {
    std::uint64_t state = 0x123456789ABCDEF0ull;
    {
        std::vector<std::uint8_t> input(1000);
        for (std::uint8_t& value : input) {
            value = static_cast<std::uint8_t>(xorshift64(state) & 0xFF);
        }
        std::vector<std::uint8_t> expected = input;
        std::sort(expected.begin(), expected.end());
        crpc::algorithm::radix_sort(input.begin(), input.end());
        CRPC_CHECK(input == expected);
    }
    {
        std::vector<std::uint16_t> input(2000);
        for (std::uint16_t& value : input) {
            value = static_cast<std::uint16_t>(xorshift64(state) & 0xFFFF);
        }
        std::vector<std::uint16_t> expected = input;
        std::sort(expected.begin(), expected.end());
        crpc::algorithm::radix_sort(input.begin(), input.end());
        CRPC_CHECK(input == expected);
    }
    {
        std::vector<std::uint32_t> input(5000);
        for (std::uint32_t& value : input) {
            value = static_cast<std::uint32_t>(xorshift64(state));
        }
        std::vector<std::uint32_t> expected = input;
        std::sort(expected.begin(), expected.end());
        crpc::algorithm::radix_sort(input.begin(), input.end());
        CRPC_CHECK(input == expected);
    }
    {
        std::vector<std::uint64_t> input(20000);
        for (std::uint64_t& value : input) {
            value = xorshift64(state);
        }
        std::vector<std::uint64_t> expected = input;
        std::sort(expected.begin(), expected.end());
        crpc::algorithm::radix_sort(input.begin(), input.end());
        CRPC_CHECK(input == expected);
    }
}

CRPC_TEST(sort_radix_signed_edges) {
    using limits = std::numeric_limits<int>;
    std::vector<int> input{0,
                           -1,
                           1,
                           limits::min(),
                           limits::max(),
                           limits::min() + 1,
                           limits::max() - 1,
                           -42,
                           42,
                           -1000000,
                           1000000};
    std::vector<int> expected = input;
    std::sort(expected.begin(), expected.end());
    crpc::algorithm::radix_sort(input.begin(), input.end());
    CRPC_CHECK(input == expected);

    // Signed 8-bit boundaries: the sign-bit flip must order -128 before -1 and
    // -1 before 0, which plain unsigned digit order would get wrong.
    std::vector<std::int8_t> small{0, -1, 1,
                                  std::numeric_limits<std::int8_t>::min(),
                                  std::numeric_limits<std::int8_t>::max(),
                                  -128, 127, -64, 64};
    std::vector<std::int8_t> small_expected = small;
    std::sort(small_expected.begin(), small_expected.end());
    crpc::algorithm::radix_sort(small.begin(), small.end());
    CRPC_CHECK(small == small_expected);
}

CRPC_TEST(sort_radix_random_large_signed) {
    constexpr std::size_t count = 50000;
    std::vector<std::int64_t> input(count);
    std::uint64_t state = 0x0F1E2D3C4B5A6978ull;
    for (std::int64_t& value : input) {
        value = static_cast<std::int64_t>(xorshift64(state));
    }
    std::vector<std::int64_t> expected = input;
    std::sort(expected.begin(), expected.end());
    crpc::algorithm::radix_sort(input.begin(), input.end());
    CRPC_CHECK(input == expected);
}

CRPC_TEST(sort_radix_integral_only) {
    CRPC_CHECK(radix_callable<int>::value);
    CRPC_CHECK(radix_callable<unsigned long long>::value);
    CRPC_CHECK(radix_callable<std::int8_t>::value);
    CRPC_CHECK(radix_callable<char>::value);
    CRPC_CHECK_FALSE(radix_callable<double>::value);
    CRPC_CHECK_FALSE(radix_callable<float>::value);
    CRPC_CHECK_FALSE(radix_callable<bool>::value);
}

// intro_sort must not require copyable elements: swaps and the insertion-sort
// hole use moves only.
CRPC_TEST(sort_move_only_elements) {
    std::vector<std::unique_ptr<int>> input;
    const int values[] = {5, 3, 9, 1, 7};
    for (const int value : values) {
        input.push_back(std::make_unique<int>(value));
    }
    crpc::algorithm::intro_sort(
        input.begin(), input.end(),
        [](const std::unique_ptr<int>& a, const std::unique_ptr<int>& b) {
            return *a < *b;
        });
    CRPC_CHECK_EQ(1, *input[0]);
    CRPC_CHECK_EQ(3, *input[1]);
    CRPC_CHECK_EQ(5, *input[2]);
    CRPC_CHECK_EQ(7, *input[3]);
    CRPC_CHECK_EQ(9, *input[4]);
}

// The sorted pre-check runs one forward scan before any partitioning: a range
// that is already non-descending -- which includes the all-equal range --
// returns after n-1 comparisons. Without the scan the same inputs cost ~2*n
// comparisons per partition level, so these count bounds are the observable
// form of the optimization: they fail without the scan and hold with it.
// Ordering semantics themselves are pinned by the std::sort comparisons in
// every other case; the counts only pin the O(n) friendly-input path.
CRPC_TEST(sort_intro_sorted_precheck_comparison_count) {
    constexpr std::size_t count = 100000;
    std::vector<int> input(count);
    std::iota(input.begin(), input.end(), 0);

    std::size_t comparisons = 0;
    crpc::algorithm::intro_sort(input.begin(), input.end(),
                                counting_less{&comparisons});
    CRPC_CHECK(std::is_sorted(input.begin(), input.end()));
    // n-1 comparisons for the scan plus constant slack. A partition walk on
    // this input costs ~2*n*log2(n) and cannot approach this bound.
    CRPC_CHECK(comparisons < count + 2 * log2_floor(count));
}

CRPC_TEST(sort_intro_all_equal_precheck_comparison_count) {
    constexpr std::size_t count = 100000;
    std::vector<int> input(count, 7);

    std::size_t comparisons = 0;
    crpc::algorithm::intro_sort(input.begin(), input.end(),
                                counting_less{&comparisons});
    CRPC_CHECK(std::is_sorted(input.begin(), input.end()));
    // All-equal is non-descending, so the same scan detects it in n-1
    // comparisons; the three-way partition alone would cost 2*n here.
    CRPC_CHECK(comparisons < count + 2 * log2_floor(count));
}

CRPC_TEST_MAIN()
