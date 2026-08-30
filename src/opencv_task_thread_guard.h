#ifndef LARDON3D_OPENCV_TASK_THREAD_GUARD_H
#define LARDON3D_OPENCV_TASK_THREAD_GUARD_H

#include <climits>

extern "C" {
#include <lardon3d/task.h>
}

#include <opencv2/core.hpp>

/* OpenCV's thread count is process-wide. Queue's single execution owner makes
 * this scoped set/restore safe for Task callbacks: the admitted count is
 * applied for the whole callback and the previous runtime setting is restored
 * on success, failure, cancellation, and C++ exception unwinding. */
class Lardon3DOpenCvTaskThreadGuard {
 public:
  explicit Lardon3DOpenCvTaskThreadGuard(Lardon3DTask *task) noexcept {
    try {
      Lardon3DTaskExecutionContract contract{};
      previous_ = cv::getNumThreads();
      previous_known_ = true;
      valid_ = lardon3d_task_execution_contract(task, &contract) &&
               contract.cpu_threads > 0 && contract.cpu_threads <= INT_MAX;
      if (valid_) {
        cv::setNumThreads(static_cast<int>(contract.cpu_threads));
        valid_ = cv::getNumThreads() == static_cast<int>(contract.cpu_threads);
      }
    } catch (...) {
      valid_ = false;
    }
  }

  ~Lardon3DOpenCvTaskThreadGuard() noexcept {
    (void)restore();
  }

  bool restore() noexcept {
    if (restored_)
      return restore_succeeded_;
    if (!previous_known_)
      return false;
    try {
      cv::setNumThreads(previous_ > 0 ? previous_ : 1);
      restored_ = true;
      restore_succeeded_ =
          cv::getNumThreads() == (previous_ > 0 ? previous_ : 1);
      return restore_succeeded_;
    } catch (...) {
      /* C callback boundary: restoration failure cannot escape as C++. */
      return false;
    }
  }

  Lardon3DOpenCvTaskThreadGuard(const Lardon3DOpenCvTaskThreadGuard &) = delete;
  Lardon3DOpenCvTaskThreadGuard &operator=(
      const Lardon3DOpenCvTaskThreadGuard &) = delete;

  bool valid() const noexcept { return valid_; }

 private:
  int previous_{};
  bool previous_known_{};
  bool valid_{};
  bool restored_{};
  bool restore_succeeded_{};
};

#endif
