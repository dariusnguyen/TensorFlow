/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/platform/status.h"

#include <stdio.h>

#include <deque>
#include <map>
#include <string>

#include "absl/base/call_once.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_format.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/stacktrace.h"
#include "tensorflow/core/platform/str_util.h"
#include "tensorflow/core/platform/strcat.h"
#include "tensorflow/core/platform/stringprintf.h"
#include "tensorflow/core/protobuf/error_codes.pb.h"

namespace tensorflow {

namespace {

// Log sink is used to collect recent warning and error log messages to be
// attached to the error status.
class StatusLogSink : public TFLogSink {
 public:
  static StatusLogSink* GetInstance() {
    static StatusLogSink* sink = new StatusLogSink();
    return sink;
  }

  void enable() {
    absl::call_once(flag_, [this] {
      num_messages_ = 5;  // default to 5 messages

      if (const char* num_msgs_str =
              getenv("TF_WORKER_NUM_FORWARDED_LOG_MESSAGES")) {
        if (!absl::SimpleAtoi(num_msgs_str, &num_messages_)) {
          LOG(WARNING) << "Failed to parse env variable "
                          "TF_WORKER_NUM_WARNING_ERROR_LOG_IN_STATUS="
                       << num_msgs_str << " as int. Using the default value "
                       << num_messages_ << ".";
        }
      }

      if (num_messages_ > 0) {
        TFAddLogSink(this);
      }
    });
  }

  void GetMessages(std::vector<std::string>* logs) TF_LOCKS_EXCLUDED(mu_) {
    mutex_lock lock(mu_);

    for (auto& msg : messages_) {
      logs->push_back(msg);
    }
  }

  void Send(const TFLogEntry& entry) override TF_LOCKS_EXCLUDED(mu_) {
    if (entry.log_severity() < absl::LogSeverity::kWarning) return;

    mutex_lock lock(mu_);
    messages_.emplace_back(entry.ToString());
    if (messages_.size() > static_cast<size_t>(num_messages_)) {
      messages_.pop_front();
    }
  }

 private:
  mutex mu_;
  // for allowing repeated/concurrent calls to enable()
  absl::once_flag flag_;
  int num_messages_ = 0;
  std::deque<std::string> messages_ TF_GUARDED_BY(mu_);
};

}  // namespace

Status::Status(tensorflow::error::Code code, tensorflow::StringPiece msg,
               std::vector<StackFrame>&& stack_trace) {
  assert(code != tensorflow::error::OK);
  state_ = std::unique_ptr<State>(new State);
  state_->code = code;
  state_->msg = std::string(msg);
  state_->stack_trace = std::move(stack_trace);
  VLOG(5) << "Generated non-OK status: \"" << *this << "\". "
          << CurrentStackTrace();
}

void Status::Update(const Status& new_status) {
  if (ok()) {
    *this = new_status;
  }
}

void Status::SlowCopyFrom(const State* src) {
  if (src == nullptr) {
    state_ = nullptr;
  } else {
    state_ = std::unique_ptr<State>(new State(*src));
  }
}

const std::string& Status::empty_string() {
  static std::string* empty = new std::string;
  return *empty;
}

const std::vector<StackFrame>& Status::empty_stack_trace() {
  static std::vector<StackFrame>* empty = new std::vector<StackFrame>();
  return *empty;
}

std::string error_name(error::Code code) {
  switch (code) {
    case tensorflow::error::OK:
      return "OK";
      break;
    case tensorflow::error::CANCELLED:
      return "CANCELLED";
      break;
    case tensorflow::error::UNKNOWN:
      return "UNKNOWN";
      break;
    case tensorflow::error::INVALID_ARGUMENT:
      return "INVALID_ARGUMENT";
      break;
    case tensorflow::error::DEADLINE_EXCEEDED:
      return "DEADLINE_EXCEEDED";
      break;
    case tensorflow::error::NOT_FOUND:
      return "NOT_FOUND";
      break;
    case tensorflow::error::ALREADY_EXISTS:
      return "ALREADY_EXISTS";
      break;
    case tensorflow::error::PERMISSION_DENIED:
      return "PERMISSION_DENIED";
      break;
    case tensorflow::error::UNAUTHENTICATED:
      return "UNAUTHENTICATED";
      break;
    case tensorflow::error::RESOURCE_EXHAUSTED:
      return "RESOURCE_EXHAUSTED";
      break;
    case tensorflow::error::FAILED_PRECONDITION:
      return "FAILED_PRECONDITION";
      break;
    case tensorflow::error::ABORTED:
      return "ABORTED";
      break;
    case tensorflow::error::OUT_OF_RANGE:
      return "OUT_OF_RANGE";
      break;
    case tensorflow::error::UNIMPLEMENTED:
      return "UNIMPLEMENTED";
      break;
    case tensorflow::error::INTERNAL:
      return "INTERNAL";
      break;
    case tensorflow::error::UNAVAILABLE:
      return "UNAVAILABLE";
      break;
    case tensorflow::error::DATA_LOSS:
      return "DATA_LOSS";
      break;
    default:
      char tmp[30];
      snprintf(tmp, sizeof(tmp), "UNKNOWN_CODE(%d)", static_cast<int>(code));
      return tmp;
      break;
  }
}

std::string Status::ToString() const {
  if (state_ == nullptr) {
    return "OK";
  } else {
    std::string result =
        absl::StrFormat("%s: %s", error_name(code()), state_->msg);

    for (const auto& payload : state_->payloads) {
      absl::StrAppendFormat(&result, " [%s='%s']", payload.first,
                            absl::CHexEscape(std::string(payload.second)));
    }

    return result;
  }
}

void Status::IgnoreError() const {
  // no-op
}

void Status::SetPayload(tensorflow::StringPiece type_url, absl::Cord payload) {
  if (ok()) return;
  state_->payloads[type_url] = std::move(payload);
}

absl::optional<absl::Cord> Status::GetPayload(
    tensorflow::StringPiece type_url) const {
  if (ok()) return absl::nullopt;
  auto payload_iter = state_->payloads.find(type_url);
  if (payload_iter == state_->payloads.end()) return absl::nullopt;
  return payload_iter->second;
}

bool Status::ErasePayload(tensorflow::StringPiece type_url) {
  if (ok()) return false;
  auto payload_iter = state_->payloads.find(type_url);
  if (payload_iter == state_->payloads.end()) return false;
  state_->payloads.erase(payload_iter);
  return true;
}

void Status::ForEachPayload(
    const std::function<void(absl::string_view, const absl::Cord&)>& visitor)
    const {
  if (ok()) return;
  for (const auto& payload : state_->payloads) {
    visitor(payload.first, payload.second);
  }
}

std::ostream& operator<<(std::ostream& os, const Status& x) {
  os << x.ToString();
  return os;
}

std::string* TfCheckOpHelperOutOfLine(const ::tensorflow::Status& v,
                                      const char* msg) {
  std::string r("Non-OK-status: ");
  r += msg;
  r += " status: ";
  r += v.ToString();
  // Leaks string but this is only to be used in a fatal error message
  return new std::string(r);
}

// kDerivedMarker is appended to the Status message string to indicate whether a
// Status object is the root cause of an error or if it has been triggered by
// cancelling/aborting a step.
static const char* kDerivedMarker = "[_Derived_]";

Status StatusGroup::MakeDerived(const Status& s) {
  if (IsDerived(s)) {
    return s;
  } else {
    return Status(s.code(), strings::StrCat(kDerivedMarker, s.error_message()));
  }
}

bool StatusGroup::IsDerived(const Status& s) {
  return s.error_message().find(kDerivedMarker) != std::string::npos;
}

void StatusGroup::ConfigureLogHistory() {
  StatusLogSink::GetInstance()->enable();
}

void StatusGroup::Update(const Status& s) {
  if (s.ok()) {
    ++num_ok_;
  } else {
    ok_ = false;
    children_.push_back(s);
  }
}

static std::vector<Status> GetNonDerivedStatuses(
    const std::vector<Status>& status) {
  std::vector<Status> nonderived_statuses;
  for (auto& s : status) {
    if (!StatusGroup::IsDerived(s)) {
      nonderived_statuses.push_back(s);
    }
  }
  return nonderived_statuses;
}

static constexpr int kMaxAggregatedStatusMessageSize = 8 * 1024;
static constexpr int kMaxAttachedLogMessageSize = 512;

// Summarize all the status objects in the StatusGroup. This is used when
// individual Status objects in the StatusGroup are not already summarized.
Status StatusGroup::as_summary_status() const {
  if (ok_) {
    return Status::OK();
  }

  // Gather recent logs as a string
  auto get_recent_logs = [this]() -> std::string {
    if (!recent_logs_.empty()) {
      std::vector<std::string> fmt;
      fmt.push_back("\nRecent warning and error logs:");
      for (auto& log : recent_logs_) {
        // Add an indentation to make it look nicer.
        fmt.push_back("  " + log.substr(0, kMaxAttachedLogMessageSize));
      }
      return absl::StrJoin(fmt, "\n");
    } else {
      return "";
    }
  };

  std::vector<Status> nonderived_statuses = GetNonDerivedStatuses(children_);

  // If only one root status is found, do not add summary header and footer.
  if (nonderived_statuses.size() == 1) {
    return Status(nonderived_statuses[0].code(),
                  strings::StrCat(nonderived_statuses[0].error_message(),
                                  get_recent_logs()));
  }

  if (!nonderived_statuses.empty()) {
    std::vector<std::string> fmt;

    fmt.push_back(strings::Printf("%zu root error(s) found.",
                                  nonderived_statuses.size()));

    int index = 0;
    auto code = tensorflow::error::CANCELLED;
    for (auto& s : nonderived_statuses) {
      // NOTE: Avoid using CANCELLED as the code of summary status if the group
      // contains other error code.
      if (code == tensorflow::error::CANCELLED &&
          s.code() != tensorflow::error::CANCELLED) {
        code = s.code();
      }
      fmt.emplace_back(strings::StrCat("  (", index, ") ", s.ToString()));
      ++index;
    }

    fmt.push_back(strings::Printf("%zu successful operations.", num_ok_));
    fmt.push_back(
        strings::Printf("%zu derived errors ignored.",
                        children_.size() - nonderived_statuses.size()));

    std::string error_msg =
        absl::StrJoin(fmt, "\n").substr(0, kMaxAggregatedStatusMessageSize);

    return Status(code, strings::StrCat(error_msg, get_recent_logs()));
  } else {
    // All statuses are derived. Pick the first available status to return.
    return children_[0];
  }
}

// Concatenate all the status objects in the StatusGroup. This is used when
// individual Status objects in the StatusGroup are already summarized Status.
Status StatusGroup::as_concatenated_status() const {
  if (ok_) {
    return Status::OK();
  }

  std::vector<Status> nonderived_statuses = GetNonDerivedStatuses(children_);

  // If only one root status is found, return it directly.
  if (nonderived_statuses.size() == 1) {
    return nonderived_statuses[0];
  }

  if (!nonderived_statuses.empty()) {
    std::vector<string> fmt;
    fmt.emplace_back("\n=====================");
    for (auto& s : nonderived_statuses) {
      fmt.emplace_back(s.ToString());
    }
    fmt.emplace_back("=====================\n");
    return Status(
        nonderived_statuses[0].code(),
        absl::StrJoin(fmt, "\n").substr(0, kMaxAggregatedStatusMessageSize));
  } else {
    // All statuses are derived. Pick the first available status to return.
    // This should not happen in normal execution.
    return children_[0];
  }
}

void StatusGroup::AttachLogMessages() {
  recent_logs_.clear();
  StatusLogSink::GetInstance()->GetMessages(&recent_logs_);
}

}  // namespace tensorflow
