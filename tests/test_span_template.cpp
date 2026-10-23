// CRPC -- class templates must stay header-only.
//
// `crpc::span<T>` is a class template, and it must NOT carry `CRPC_API`. A DLL
// can only export the specialisations it instantiated itself, so decorating the
// template makes every other specialisation a link error in the consumer:
//
//   probe.cpp(3): error LNK2019: unresolved external symbol
//     "public: unsigned __int64 __cdecl crpc::span<int>::size(void)const"
//     (__imp_?size@?$span@H@crpc@@QEBA_KXZ)
//
// That failure is invisible in the library's own build (it only instantiates
// `span<std::byte>` and `span<const std::byte>`), and it is invisible in every
// other test (they use those same specialisations). So this translation unit
// deliberately instantiates specialisations nothing else in the tree uses, and
// calls through them rather than merely naming them: taking an address or a
// `sizeof` can be satisfied by the compiler without an external symbol.
//
// This is the runtime half of the rule. The static half is
// `scripts/export_gate.py` rule D, which rejects the decoration in review.
//
// Scope: this test only has teeth in a **shared** build. In a static build
// `CRPC_API` expands to nothing, so a decorated `span<T>` is harmless and the
// test passes -- correctly, because there is no export table to get wrong. The
// static build still runs it as a plain functional test of the specialisations.
// Do not "fix" that by asserting a link failure; the link failure is the shared
// build's job, and `export_gate.py` rule D is what catches the decoration in
// both configurations.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <cstdint>
#include <string>

// `_DEBUG` is undefined around the harness include for the same reason
// test_mvc.cpp and test_codec.cpp do it: the Debug build's `.CRT$XCU` pragma
// would otherwise be emitted before the STL headers above fix
// _ITERATOR_DEBUG_LEVEL.
#undef _DEBUG
#include "test_framework.hpp"

#include <crpc/span.hpp>

namespace {

// One instantiation per element type, none of which the library itself uses.
// The bodies must be emitted here, which only happens when the members are
// inline in the header.
template <typename T>
std::size_t probe_size(crpc::span<T> view) {
    return view.size();
}

} // namespace

CRPC_TEST(span_int_specialisation_links) {
    int values[4] = {1, 2, 3, 4};
    crpc::span<int> view(values, 4);
    CRPC_CHECK_EQ(std::size_t{4}, probe_size(view));
    CRPC_CHECK_EQ(4, view[3]);
    CRPC_CHECK_EQ(std::size_t{16}, view.size_bytes());
    CRPC_CHECK(!view.empty());

    crpc::span<int> tail = view.subspan(2);
    CRPC_CHECK_EQ(std::size_t{2}, tail.size());
    CRPC_CHECK_EQ(3, tail[0]);
    // An out-of-range offset clamps to an empty span, like std::span.
    CRPC_CHECK(view.subspan(4).empty());
    CRPC_CHECK(view.subspan(99).empty());
}

CRPC_TEST(span_double_specialisation_links) {
    double values[3] = {1.5, 2.5, 3.5};
    crpc::span<double> view(values);
    CRPC_CHECK_EQ(std::size_t{3}, probe_size(view));
    CRPC_CHECK_EQ(std::size_t{24}, view.size_bytes());
    CRPC_CHECK_EQ(3.5, view[2]);
    CRPC_CHECK_EQ(values, view.data());
    CRPC_CHECK_EQ(values + 3, view.end());
    CRPC_CHECK_EQ(values, view.begin());
}

CRPC_TEST(span_const_and_string_specialisations_link) {
    const std::int64_t numbers[2] = {7, 8};
    crpc::span<const std::int64_t> view(numbers, 2);
    CRPC_CHECK_EQ(std::size_t{2}, probe_size(view));
    CRPC_CHECK_EQ(std::int64_t{8}, view[1]);

    std::string words[2] = {std::string("alpha"), std::string("beta")};
    crpc::span<std::string> text(words);
    CRPC_CHECK_EQ(std::size_t{2}, probe_size(text));
    CRPC_CHECK_EQ(std::string("beta"), text[1]);

    // A default-constructed span is empty and carries no pointer.
    crpc::span<unsigned char> empty;
    CRPC_CHECK(empty.empty());
    CRPC_CHECK_EQ(std::size_t{0}, probe_size(empty));
    CRPC_CHECK(empty.data() == nullptr);
}

CRPC_TEST_MAIN()
