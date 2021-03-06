// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/storage/parallel_upload.h"

namespace google {
namespace cloud {
namespace storage {
inline namespace STORAGE_CLIENT_NS {
namespace internal {

class ParallelObjectWriteStreambuf : public ObjectWriteStreambuf {
 public:
  ParallelObjectWriteStreambuf(
      std::shared_ptr<NonResumableParallelUploadState::Impl> state,
      std::size_t stream_idx,
      std::unique_ptr<ResumableUploadSession> upload_session,
      std::size_t max_buffer_size,
      std::unique_ptr<HashValidator> hash_validator)
      : ObjectWriteStreambuf(std::move(upload_session), max_buffer_size,
                             std::move(hash_validator)),
        state_(std::move(state)),
        stream_idx_(stream_idx) {}

  StatusOr<ResumableUploadResponse> Close() override {
    auto res = this->ObjectWriteStreambuf::Close();
    state_->StreamFinished(stream_idx_, res);
    return res;
  }

 private:
  std::shared_ptr<NonResumableParallelUploadState::Impl> state_;
  std::size_t stream_idx_;
};

NonResumableParallelUploadState::Impl::Impl(
    std::unique_ptr<ScopedDeleter> deleter, Composer composer)
    : deleter_(std::move(deleter)),
      composer_(std::move(composer)),
      finished_{},
      num_unfinished_streams_{} {}

NonResumableParallelUploadState::Impl::~Impl() { WaitForCompletion().wait(); }

StatusOr<ObjectWriteStream> NonResumableParallelUploadState::Impl::CreateStream(
    RawClient& raw_client, ResumableUploadRequest const& request) {
  auto session = raw_client.CreateResumableSession(request);
  std::unique_lock<std::mutex> lk(mu_);
  if (!session) {
    // Preserve the first error.
    res_ = session.status();
    return std::move(session).status();
  }
  return ObjectWriteStream(
      google::cloud::internal::make_unique<ParallelObjectWriteStreambuf>(
          shared_from_this(), num_unfinished_streams_++, *std::move(session),
          raw_client.client_options().upload_buffer_size(),
          CreateHashValidator(request)));
}

Status NonResumableParallelUploadState::Impl::EagerCleanup() {
  std::unique_lock<std::mutex> lk(mu_);
  if (!finished_) {
    return Status(StatusCode::kFailedPrecondition,
                  "Attempted to cleanup parallel upload state while it is "
                  "still in progress");
  }
  // Make sure that only one thread actually interacts with the deleter.
  if (deleter_) {
    cleanup_status_ = deleter_->ExecuteDelete();
    deleter_.reset();
  }
  return cleanup_status_;
}

void NonResumableParallelUploadState::Impl::Fail(Status status) {
  std::unique_lock<std::mutex> lk(mu_);
  assert(!status.ok());
  if (!res_) {
    // Preserve the first error.
    res_ = std::move(status);
  }
}

void NonResumableParallelUploadState::Impl::StreamFinished(
    std::size_t stream_idx, StatusOr<ResumableUploadResponse> const& response) {
  std::unique_lock<std::mutex> lk(mu_);

  --num_unfinished_streams_;
  if (!response) {
    // The upload failed, we don't event need to clean this up.
    if (!res_) {
      // Preserve the first error.
      res_ = response.status();
    }
  } else {
    ObjectMetadata const& metadata = *response->payload;
    to_compose_.resize(std::max(to_compose_.size(), stream_idx + 1));
    to_compose_[stream_idx] =
        ComposeSourceObject{metadata.name(), metadata.generation(), {}};
    deleter_->Add(metadata);
  }
  if (num_unfinished_streams_ > 0) {
    return;
  }
  if (!res_) {
    // only execute ComposeMany if all the streams succeeded.
    lk.unlock();
    auto res = composer_(to_compose_);
    lk.lock();
    res_ = std::move(res);
  }
  // All done, wake up whomever is waiting.
  finished_ = true;
  auto promises_to_satisfy = std::move(res_promises_);
  lk.unlock();
  for (auto& promise : promises_to_satisfy) {
    promise.set_value(*res_);
  }
}

future<StatusOr<ObjectMetadata>>
NonResumableParallelUploadState::Impl::WaitForCompletion() const {
  std::unique_lock<std::mutex> lk(mu_);

  if (finished_) {
    return make_ready_future(*res_);
  }
  res_promises_.emplace_back();
  auto res = res_promises_.back().get_future();
  return res;
}

ParallelUploadFileShard::~ParallelUploadFileShard() {
  // If the object wasn't moved-from (i.e. `state != nullptr`) and
  // `left_to_upload_ > 0` it means that the object is being destroyed without
  // actually uploading the file. We should make sure we don't create the
  // destination object and instead fail the whole operation.
  if (state_ != nullptr && left_to_upload_ > 0) {
    state_->Fail(Status(StatusCode::kCancelled,
                        "Shard destroyed before calling "
                        "ParallelUploadFileShard::Upload()."));
  }
}

Status ParallelUploadFileShard::Upload() {
  std::unique_ptr<char[]> buf(new char[upload_buffer_size_]);

  auto fail = [this](StatusCode error_code, std::string reason) {
    Status status(error_code, "ParallelUploadFileShard::Upload(" + file_name_ +
                                  "): " + reason);
    state_->Fail(status);
    ostream_.Close();
    return status;
  };
  std::ifstream istream(file_name_, std::ios::binary);
  if (!istream.good()) {
    return fail(StatusCode::kNotFound, "cannot open upload file source");
  }
  istream.seekg(offset_in_file_);
  if (!istream.good()) {
    return fail(StatusCode::kInternal, "file changed size during upload?");
  }
  while (left_to_upload_ > 0) {
    std::size_t const to_copy =
        std::min<std::uintmax_t>(left_to_upload_, upload_buffer_size_);
    istream.read(buf.get(), to_copy);
    if (!istream.good()) {
      return fail(StatusCode::kInternal, "cannot read from file source");
    }
    ostream_.write(buf.get(), to_copy);
    if (!ostream_.good()) {
      return Status(StatusCode::kInternal,
                    "Writing to output stream failed, look into whole parallel "
                    "upload status for more information");
    }
    left_to_upload_ -= to_copy;
  }
  ostream_.Close();
  if (ostream_.metadata()) {
    return Status();
  }
  return ostream_.metadata().status();
}

}  // namespace internal
}  // namespace STORAGE_CLIENT_NS
}  // namespace storage
}  // namespace cloud
}  // namespace google
