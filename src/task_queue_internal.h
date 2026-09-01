#ifndef LARDON3D_TASK_QUEUE_INTERNAL_H
#define LARDON3D_TASK_QUEUE_INTERNAL_H

#include <lardon3d/task_queue.h>

#if defined(LARDON3D_TASK_QUEUE_TESTING)

/* TEST CONTRACT: these events expose exact Queue lifetime linearization points
 * only to test-task-queue and test-resource-external-storage. Production must
 * not declare, reference or call this seam. Both test binaries provide one
 * strong callback definition; task_queue.c keeps a weak reference so the seam
 * cannot become a required library/public ABI symbol. */
typedef enum {
    LARDON3D_TASK_QUEUE_TEST_CALL_REGISTERED = 1,
    LARDON3D_TASK_QUEUE_TEST_PRODUCER_WAITING = 2,
    LARDON3D_TASK_QUEUE_TEST_CLOSING = 3,
} Lardon3DTaskQueueTestEvent;

#if defined(LARDON3D_TASK_QUEUE_TEST_WEAK_REFERENCE) \
    && (defined(__GNUC__) || defined(__clang__))
#define LARDON3D_TASK_QUEUE_TEST_ATTRIBUTE __attribute__((weak))
#else
#define LARDON3D_TASK_QUEUE_TEST_ATTRIBUTE
#endif

void lardon3d_task_queue_internal_test_event(
    Lardon3DTaskQueue *queue,
    Lardon3DTaskQueueTestEvent event
) LARDON3D_TASK_QUEUE_TEST_ATTRIBUTE;

#undef LARDON3D_TASK_QUEUE_TEST_ATTRIBUTE

#endif

#endif
