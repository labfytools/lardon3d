#ifndef LARDON3D_OPENCV_TASK_THREAD_CONTROL_H
#define LARDON3D_OPENCV_TASK_THREAD_CONTROL_H

#include <stdbool.h>

#include <lardon3d/feature_extractor.h>
#include <lardon3d/task.h>

typedef struct {
  unsigned int previous;
  bool restore_required;
} Lardon3DOpenCvTaskThreadControl;

/* OpenCV owns one process-wide CPU pool. Queue's single callback owner makes
 * the change race-free, but configure may mutate before its verification
 * fails. Therefore begin attempts rollback on every post-capture failure and
 * end restores on every callback result. */
static inline bool lardon3d_opencv_task_threads_begin(
    Lardon3DTask *task, unsigned int validated_maximum,
    Lardon3DOpenCvTaskThreadControl *control) {
  if (!task || !control || validated_maximum == 0) return false;
  *control = (Lardon3DOpenCvTaskThreadControl){0};
  Lardon3DTaskExecutionContract contract;
  if (!lardon3d_task_execution_contract(task, &contract) ||
      contract.cpu_threads == 0 || contract.cpu_threads > validated_maximum) {
    return false;
  }
  control->previous = lardon3d_feature_opencv_thread_count();
  control->restore_required = true;
  if (!lardon3d_feature_opencv_configure_threads(contract.cpu_threads)) {
    /* Verification failure is after a possibly successful setNumThreads(). */
    (void)lardon3d_feature_opencv_configure_threads(control->previous);
    control->restore_required = false;
    return false;
  }
  return true;
}

static inline bool lardon3d_opencv_task_threads_end(
    Lardon3DOpenCvTaskThreadControl *control) {
  if (!control || !control->restore_required) return false;
  control->restore_required = false;
  return lardon3d_feature_opencv_configure_threads(control->previous);
}

#endif
