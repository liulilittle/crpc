// CRPC -- MSMQ local transaction implementation.
//
// begin() obtains an ITransaction from MQBeginTransaction and wraps it. commit
// maps to ITransaction::Commit(0, XACTTC_SYNC, 0) and abort to
// ITransaction::Abort, so the transactional semantics are exactly what the
// MSMQ runtime implements for the local coordinator; a rolled-back send or
// receive is what a real queue verifies.
#include <crpc/mq/msmq/transaction.hpp>

#include <string>
#include <utility>

#if defined(CRPC_OS_WINDOWS)
// See queue.cpp: mq.h needs the COM `interface` keyword that ole2.h provides.
#include <windows.h>
#include <ole2.h>
#include <mq.h>
#include <Transact.h>
#include <xolehlp.h>
#pragma comment(lib, "mqrt.lib")
#pragma comment(lib, "ole32.lib")
#endif

#include "msmq_internal.hpp"

namespace crpc {
namespace mq {
namespace msmq {

struct transaction::impl {
    void* native = nullptr; // Windows: ITransaction*
    bool active = false;

    ~impl() {
#if defined(CRPC_OS_WINDOWS)
        if (native != nullptr) {
            static_cast<ITransaction*>(native)->Release();
        }
#endif
    }
};

// Internal accessor used by queue.cpp to bind a send/receive to this
// transaction's native ITransaction pointer.
void* transaction_handle(transaction& tx) noexcept {
    return tx.impl_ ? tx.impl_->native : nullptr;
}

transaction::transaction(std::unique_ptr<impl> pimpl) noexcept
    : impl_(std::move(pimpl)) {}

transaction::~transaction() = default;

#if defined(CRPC_OS_WINDOWS)

namespace {

// MSMQ transactions are COM objects; the dispenser needs a COM apartment.
// Accept any outcome that leaves a usable apartment, including a mode that was
// already established by the process.
[[nodiscard]] bool ensure_com_initialized() noexcept {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return hr == S_OK || hr == S_FALSE || hr == RPC_E_CHANGED_MODE;
}

} // namespace

#endif // CRPC_OS_WINDOWS

result<std::unique_ptr<transaction>> transaction::begin() {
#if defined(CRPC_OS_WINDOWS)
    if (!ensure_com_initialized()) {
        return rpc_error{
            rpc_code::failed_precondition,
            "transaction.begin: the COM apartment could not be initialised"};
    }
    ITransaction* native = nullptr;
    const HRESULT hr = MQBeginTransaction(&native);
    if (FAILED(hr) || native == nullptr) {
        return detail::mq_error("transaction.begin: MQBeginTransaction failed",
                                hr);
    }
    std::unique_ptr<impl> state(new impl());
    state->native = native;
    state->active = true;
    return std::unique_ptr<transaction>(new transaction(std::move(state)));
#else
    return rpc_error{rpc_code::unavailable,
                     "transaction.begin: MSMQ is only available on Windows"};
#endif
}

status transaction::commit() {
#if defined(CRPC_OS_WINDOWS)
    if (impl_ == nullptr || impl_->native == nullptr || !impl_->active) {
        return status{rpc_code::failed_precondition,
                      "transaction.commit: the transaction is finished"};
    }
    ITransaction* native = static_cast<ITransaction*>(impl_->native);
    const HRESULT hr = native->Commit(0, XACTTC_SYNC, 0);
    if (FAILED(hr)) {
        return status(detail::mq_error("transaction.commit failed", hr));
    }
    impl_->active = false;
    return status{};
#else
    return status{rpc_code::unavailable,
                  "transaction.commit: MSMQ is only available on Windows"};
#endif
}

status transaction::abort() {
#if defined(CRPC_OS_WINDOWS)
    if (impl_ == nullptr || impl_->native == nullptr || !impl_->active) {
        return status{rpc_code::failed_precondition,
                      "transaction.abort: the transaction is finished"};
    }
    ITransaction* native = static_cast<ITransaction*>(impl_->native);
    const HRESULT hr = native->Abort(nullptr, FALSE, FALSE);
    if (FAILED(hr)) {
        return status(detail::mq_error("transaction.abort failed", hr));
    }
    impl_->active = false;
    return status{};
#else
    return status{rpc_code::unavailable,
                  "transaction.abort: MSMQ is only available on Windows"};
#endif
}

bool transaction::is_active() const noexcept {
    return impl_ != nullptr && impl_->active;
}

} // namespace msmq
} // namespace mq
} // namespace crpc