#include "server/src/evals/eval_failure_utils.hpp"

namespace isla::server::evals {

std::string StatusCodeName(absl::StatusCode code) {
    switch (code) {
    case absl::StatusCode::kOk:
        return "ok";
    case absl::StatusCode::kCancelled:
        return "cancelled";
    case absl::StatusCode::kUnknown:
        return "unknown";
    case absl::StatusCode::kInvalidArgument:
        return "invalid_argument";
    case absl::StatusCode::kDeadlineExceeded:
        return "deadline_exceeded";
    case absl::StatusCode::kNotFound:
        return "not_found";
    case absl::StatusCode::kAlreadyExists:
        return "already_exists";
    case absl::StatusCode::kPermissionDenied:
        return "permission_denied";
    case absl::StatusCode::kResourceExhausted:
        return "resource_exhausted";
    case absl::StatusCode::kFailedPrecondition:
        return "failed_precondition";
    case absl::StatusCode::kAborted:
        return "aborted";
    case absl::StatusCode::kOutOfRange:
        return "out_of_range";
    case absl::StatusCode::kUnimplemented:
        return "unimplemented";
    case absl::StatusCode::kInternal:
        return "internal";
    case absl::StatusCode::kUnavailable:
        return "unavailable";
    case absl::StatusCode::kDataLoss:
        return "data_loss";
    case absl::StatusCode::kUnauthenticated:
        return "unauthenticated";
    }
    return "unknown";
}

EvalFailure FailureFromStatus(const absl::Status& status) {
    return EvalFailure{
        .code = StatusCodeName(status.code()),
        .message = std::string(status.message()),
    };
}

nlohmann::ordered_json FailureToJson(const std::optional<EvalFailure>& failure) {
    if (!failure.has_value()) {
        return nullptr;
    }
    return nlohmann::ordered_json{
        { "code", failure->code },
        { "message", failure->message },
    };
}

} // namespace isla::server::evals
