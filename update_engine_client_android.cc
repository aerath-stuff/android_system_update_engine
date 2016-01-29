//
// Copyright (C) 2016 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <sysexits.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback.h>
#include <base/command_line.h>
#include <base/logging.h>
#include <base/strings/string_split.h>
#include <binder/IServiceManager.h>
#include <brillo/binder_watcher.h>
#include <brillo/daemons/daemon.h>
#include <brillo/flag_helper.h>
#include <brillo/message_loops/message_loop.h>
#include <brillo/syslog_logging.h>
#include <utils/String16.h>
#include <utils/StrongPointer.h>

#include "android/os/BnUpdateEngineCallback.h"
#include "android/os/IUpdateEngine.h"
#include "update_engine/client_library/include/update_engine/update_status.h"
#include "update_engine/update_status_utils.h"
#include "update_engine/common/error_code.h"

using android::binder::Status;

namespace chromeos_update_engine {
namespace internal {

class UpdateEngineClientAndroid : public brillo::Daemon {
 public:
  UpdateEngineClientAndroid(int argc, char** argv) : argc_(argc), argv_(argv) {
  }

  int ExitWhenIdle(const Status& status);
  int ExitWhenIdle(int return_code);

 private:
  class UECallback : public android::os::BnUpdateEngineCallback {
   public:
    UECallback(UpdateEngineClientAndroid* client) : client_(client) {
    }

    // android::os::BnUpdateEngineCallback overrides.
    Status onStatusUpdate(int status_code, float progress) override;
    Status onPayloadApplicationComplete(int error_code) override;

   private:
    UpdateEngineClientAndroid* client_;
  };

  int OnInit() override;

  // Copy of argc and argv passed to main().
  int argc_;
  char** argv_;

  android::sp<android::os::IUpdateEngine> service_;
  android::sp<android::os::BnUpdateEngineCallback> callback_;

  brillo::BinderWatcher binder_watcher_;
};

Status UpdateEngineClientAndroid::UECallback::onStatusUpdate(
    int status_code, float progress) {
  update_engine::UpdateStatus status =
      static_cast<update_engine::UpdateStatus>(status_code);
  LOG(INFO) << "onStatusUpdate(" << UpdateStatusToString(status) << " ("
            << status_code << "), " << progress << ")";
  return Status::ok();
}

Status UpdateEngineClientAndroid::UECallback::onPayloadApplicationComplete(
    int error_code) {
  ErrorCode code = static_cast<ErrorCode>(error_code);
  // TODO(deymo): Print the ErrorCode as a string.
  LOG(INFO) << "onPayloadApplicationComplete(" << error_code << ")";
  client_->ExitWhenIdle(code == ErrorCode::kSuccess ? EX_OK : 1);
  return Status::ok();
}

int UpdateEngineClientAndroid::OnInit() {
  int ret = Daemon::OnInit();
  if (ret != EX_OK)
    return ret;

  DEFINE_bool(update, false, "Start a new update, if no update in progress.");
  DEFINE_string(payload,
                "http://127.0.0.1:8080/payload",
                "The URI to the update payload to use.");
  DEFINE_string(headers,
                "",
                "A list of key-value pairs, one element of the list per line.");

  DEFINE_bool(suspend, false, "Suspend an ongoing update and exit.");
  DEFINE_bool(resume, false, "Resume a suspended update.");
  DEFINE_bool(cancel, false, "Cancel the ongoing update and exit.");
  DEFINE_bool(follow,
              false,
              "Follow status update changes until a final state is reached. "
              "Exit status is 0 if the update succeeded, and 1 otherwise.");

  // Boilerplate init commands.
  base::CommandLine::Init(argc_, argv_);
  brillo::FlagHelper::Init(argc_, argv_, "Android Update Engine Client");
  if (argc_ == 1) {
    LOG(ERROR) << "Nothing to do. Run with --help for help.";
    return 1;
  }

  // Ensure there are no positional arguments.
  const std::vector<std::string> positional_args =
      base::CommandLine::ForCurrentProcess()->GetArgs();
  if (!positional_args.empty()) {
    LOG(ERROR) << "Found a positional argument '" << positional_args.front()
               << "'. If you want to pass a value to a flag, pass it as "
                  "--flag=value.";
    return 1;
  }

  bool keep_running = false;

  brillo::InitLog(brillo::kLogToStderr);
  android::status_t status = android::getService(
      android::String16("android.os.UpdateEngineService"), &service_);
  if (status != android::OK) {
    LOG(ERROR) << "Failed to get IUpdateEngine binder from service manager: "
               << Status::fromStatusT(status).toString8();
  }

  if (FLAGS_suspend) {
    return ExitWhenIdle(service_->suspend());
  }

  if (FLAGS_resume) {
    return ExitWhenIdle(service_->resume());
  }

  if (FLAGS_cancel) {
    return ExitWhenIdle(service_->cancel());
  }

  if (FLAGS_follow) {
    // Register a callback object with the service.
    callback_ = new UECallback(this);
    bool bound;
    if (!service_->bind(callback_, &bound).isOk() || !bound) {
      LOG(ERROR) << "Failed to bind() the UpdateEngine daemon.";
      return 1;
    }
    keep_running = true;
  }

  if (FLAGS_update) {
    std::vector<std::string> headers = base::SplitString(
        FLAGS_headers, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    std::vector<android::String16> and_headers;
    for (const auto& header : headers) {
      and_headers.push_back(android::String16{header.data(), header.size()});
    }
    Status status = service_->applyPayload(
        android::String16{FLAGS_payload.data(), FLAGS_payload.size()},
        and_headers);
    if (!status.isOk())
      return ExitWhenIdle(status);
  }

  if (!keep_running)
    return ExitWhenIdle(EX_OK);

  // Initialize a binder watcher.
  binder_watcher_.Init();
  return EX_OK;
}

int UpdateEngineClientAndroid::ExitWhenIdle(const Status& status) {
  if (status.isOk())
    return ExitWhenIdle(EX_OK);
  LOG(ERROR) << status.toString8();
  return ExitWhenIdle(status.exceptionCode());
}

int UpdateEngineClientAndroid::ExitWhenIdle(int return_code) {
  auto delayed_exit = base::Bind(
      &Daemon::QuitWithExitCode, base::Unretained(this), return_code);
  if (!brillo::MessageLoop::current()->PostTask(delayed_exit))
    return 1;
  return EX_OK;
}

}  // namespace internal
}  // namespace chromeos_update_engine

int main(int argc, char** argv) {
  chromeos_update_engine::internal::UpdateEngineClientAndroid client(
      argc, argv);
  return client.Run();
}
