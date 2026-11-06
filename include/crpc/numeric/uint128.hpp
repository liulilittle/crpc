// CRPC -- 128-bit unsigned integer.
//
// The width-specific headers exist so that a translation unit can include just
// the type it names; each is a one-line alias over uint_big in bigint.hpp.
#ifndef CRPC_NUMERIC_UINT128_HPP
#define CRPC_NUMERIC_UINT128_HPP

#include <crpc/numeric/bigint.hpp>

namespace crpc {
namespace numeric {

using uint128 = uint_big<128>;

} // namespace numeric
} // namespace crpc

#endif // CRPC_NUMERIC_UINT128_HPP
