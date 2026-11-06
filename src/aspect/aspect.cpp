// CRPC -- AOP aspect chain out-of-line implementation.
//
// `invoke()` and the onion walk are inline in the header: they are the hot
// path and nothing in them needs a translation-unit boundary. What lives here
// is the startup-only registration surface, so a shared build exports exactly
// one symbol group for it and the header stays a viable include for a consumer
// that only needs to run a chain.
#include <crpc/aspect/aspect.hpp>

namespace crpc {
namespace aspect {

pointcut pointcut::on(std::string_view exact_name) {
    if (exact_name.empty()) {
        return pointcut();
    }
    return pointcut(kind::exact, exact_name);
}

pointcut pointcut::on_prefix(std::string_view prefix) {
    if (prefix.empty()) {
        return pointcut();
    }
    return pointcut(kind::prefix, prefix);
}

pointcut pointcut::on_suffix(std::string_view suffix) {
    if (suffix.empty()) {
        return pointcut();
    }
    return pointcut(kind::suffix, suffix);
}

pointcut pointcut::on_all() noexcept {
    return pointcut(kind::all, std::string_view());
}

status aspect_chain::add(pointcut pc, advice* adv) {
    if (adv == nullptr) {
        return status(rpc_code::invalid_argument, "aspect: null advice");
    }
    if (!pc.valid()) {
        return status(rpc_code::invalid_argument, "aspect: pointcut matches nothing");
    }
    if (frozen_) {
        return status(rpc_code::failed_precondition, "aspect: chain is frozen");
    }
    if (count_ >= max_aspects) {
        return status(rpc_code::resource_exhausted, "aspect: chain is full");
    }
    entries_[count_] = entry{pc, adv};
    ++count_;
    return status();
}

} // namespace aspect
} // namespace crpc
