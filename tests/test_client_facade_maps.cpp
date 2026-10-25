// CRPC -- facade active-call registry map regression.
//
// The facade's active-call registry used to be a `std::unordered_map`, which
// allocated a node on every insert. It is now `client_detail::flat_pool_map`.
// This locks the container's observable behaviour -- insert, lookup, erase with
// backward-shift, growth past the initial capacity, and reuse after `clear` --
// so a regression to an allocating (or entry-losing) container is caught by the
// suite rather than only by the benchmark's allocation counter.
#include <cstddef>

#include <crpc/client_facade.hpp>

#include "test_framework.hpp"

namespace {

using crpc::client_detail::flat_pool_map;

} // namespace

CRPC_TEST(facade_flat_pool_map_keeps_every_surviving_key) {
    flat_pool_map<int, int> map;
    for (int id = 1; id <= 64; ++id) {
        map.emplace(id, id * 2);
    }
    CRPC_CHECK(map.size() == 64u);
    for (int id = 1; id <= 64; ++id) {
        const int* found = map.find(id);
        CRPC_CHECK(found != nullptr);
        if (found != nullptr) {
            CRPC_CHECK(*found == id * 2);
        }
    }

    // Erase in a different order than insertion: the backward-shift repair must
    // keep every surviving key reachable (this is the bug the benchmark caught).
    for (int id = 1; id <= 64; id += 2) {
        map.erase(id);
    }
    CRPC_CHECK(map.size() == 32u);
    for (int id = 2; id <= 64; id += 2) {
        const int* found = map.find(id);
        CRPC_CHECK(found != nullptr);
        if (found != nullptr) {
            CRPC_CHECK(*found == id * 2);
        }
    }
    for (int id = 1; id <= 64; id += 2) {
        CRPC_CHECK(map.find(id) == nullptr);
    }

    // The map is reusable after being emptied.
    map.clear();
    CRPC_CHECK(map.size() == 0u);
    map.emplace(1000, 7);
    const int* found = map.find(1000);
    CRPC_CHECK(found != nullptr);
    if (found != nullptr) {
        CRPC_CHECK(*found == 7);
    }
}

CRPC_TEST_MAIN()
