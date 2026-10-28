// CRPC -- MSMQ error mapping.
//
// The HRESULT values come from the MSMQ API surface (Mq.h). map_mq_error is
// deliberately the only place that knows them; every other adapter function
// funnels its HRESULT through here, so the library's behaviour is consistent:
// a timed-out receive is `deadline_exceeded` (never a fatal surprise), a
// missing queue is `not_found`, caller-supplied buffer problems are
// `invalid_argument`.
#include <crpc/mq/msmq/types.hpp>

#include <cstdint>

#if defined(CRPC_OS_WINDOWS)
// See queue.cpp: mq.h needs the COM `interface` keyword that ole2.h provides.
#include <windows.h>
#include <ole2.h>
#include <mq.h>
#endif

namespace crpc {
namespace mq {
namespace msmq {

rpc_code map_mq_error(std::uint32_t hr) noexcept {
#if defined(CRPC_OS_WINDOWS)
    switch (static_cast<HRESULT>(hr)) {
    case MQ_OK:
        return rpc_code::ok;
    case MQ_ERROR_QUEUE_NOT_FOUND:
    case MQ_ERROR_MESSAGE_NOT_FOUND:
        return rpc_code::not_found;
    case MQ_ERROR_IO_TIMEOUT:
        return rpc_code::deadline_exceeded;
    case MQ_ERROR_ACCESS_DENIED:
    case MQ_ERROR_PRIVILEGE_NOT_HELD:
    case MQ_ERROR_QUEUE_EXISTS:
    case MQ_ERROR_TRANSACTION_USAGE:
    case MQ_ERROR_TRANSACTION_SEQUENCE:
    case MQ_ERROR_TRANSACTION_IMPORT:
    case MQ_ERROR_TRANSACTION_ENLIST:
    case MQ_ERROR_MESSAGE_LOCKED_UNDER_TRANSACTION:
        return rpc_code::failed_precondition;
    case MQ_ERROR_INVALID_PARAMETER:
    case MQ_ERROR_ILLEGAL_PROPERTY_VALUE:
    case MQ_ERROR_ILLEGAL_PROPERTY_VT:
    case MQ_ERROR_ILLEGAL_PROPID:
    case MQ_ERROR_PROPERTY:
    case MQ_ERROR_LABEL_TOO_LONG:
    case MQ_ERROR_LABEL_BUFFER_TOO_SMALL:
    case MQ_ERROR_SENDER_CERT_BUFFER_TOO_SMALL:
    case MQ_ERROR_PROPERTIES_CONFLICT:
    case MQ_ERROR_TOO_MANY_PROPERTIES:
        return rpc_code::invalid_argument;
    case MQ_ERROR_BUFFER_OVERFLOW:
        // A body buffer that is too small: the message stays in the queue and
        // the caller retries with the size MSMQ reported. A size mistake, not
        // a queue fault.
        return rpc_code::invalid_argument;
    case MQ_ERROR_QUEUE_NOT_AVAILABLE:
    case MQ_ERROR_SERVICE_NOT_AVAILABLE:
        return rpc_code::unavailable;
    case MQ_ERROR_INSUFFICIENT_RESOURCES:
        return rpc_code::resource_exhausted;
    case MQ_ERROR_INVALID_HANDLE:
    case MQ_ERROR_STALE_HANDLE:
    case MQ_ERROR_QUEUE_DELETED:
        return rpc_code::connection_reset;
    case MQ_ERROR_MESSAGE_STORAGE_FAILED:
        return rpc_code::internal;
    default:
        // Any other positive (informational) HRESULT -- MQ_INFORMATION_* -- is
        // a success on this machine's service: the message was sent, a
        // property was ignored, and so on. Treat the whole positive half of
        // the space as ok, exactly as SUCCEEDED() does for callers.
        if (hr <= 0x7FFFFFFFu) {
            return rpc_code::ok;
        }
        return rpc_code::application;
    }
#else
    (void)hr;
    return rpc_code::unavailable;
#endif
}

} // namespace msmq
} // namespace mq
} // namespace crpc