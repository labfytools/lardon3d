#ifndef LARDON3D_TASK_H
#define LARDON3D_TASK_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/resource_governor.h>

enum {
    LARDON3D_TASK_NAME_CAPACITY = 128,
    LARDON3D_TASK_MESSAGE_CAPACITY = 256,
};

typedef enum {
    TASK_PENDING = 0,
    TASK_RUNNING,
    TASK_PAUSED,
    TASK_CANCELLED,
    TASK_FAILED,
    TASK_COMPLETED
} Lardon3DTaskState;

typedef struct Lardon3DTask Lardon3DTask;
typedef bool (*Lardon3DTaskCallback)(Lardon3DTask *task, void *userdata);

typedef struct {
    uint64_t id;
    char name[LARDON3D_TASK_NAME_CAPACITY];
    unsigned int progress;
    Lardon3DTaskState state;
    char message[LARDON3D_TASK_MESSAGE_CAPACITY];
    struct timespec started_at;
    struct timespec finished_at;
} Lardon3DTaskSnapshot;

typedef struct {
    size_t batch_size;
    uint64_t memory_bytes;
    uint64_t gpu_memory_bytes;
    unsigned int cpu_threads;
    unsigned int gpu_slots;
    unsigned int io_slots;
} Lardon3DTaskExecutionContract;

Lardon3DTask *lardon3d_task_create(
    const char *name,
    const Lardon3DResourceEstimate *estimate,
    Lardon3DTaskCallback callback,
    void *userdata
);
void lardon3d_task_destroy(Lardon3DTask *task);
/* Exécute le callback dans le thread appelant. */
bool lardon3d_task_start(
    Lardon3DTask *task,
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceReservation *reservation
);
void lardon3d_task_request_cancel(Lardon3DTask *task);
bool lardon3d_task_pause(Lardon3DTask *task);
bool lardon3d_task_resume(Lardon3DTask *task);
bool lardon3d_task_join(Lardon3DTask *task);
bool lardon3d_task_checkpoint(Lardon3DTask *task);
bool lardon3d_task_set_progress(
    Lardon3DTask *task,
    unsigned int progress,
    const char *message
);
bool lardon3d_task_fail(Lardon3DTask *task, const char *message);
bool lardon3d_task_snapshot(
    const Lardon3DTask *task,
    Lardon3DTaskSnapshot *snapshot
);
uint64_t lardon3d_task_id(const Lardon3DTask *task);
bool lardon3d_task_assign_id(Lardon3DTask *task, uint64_t id);
bool lardon3d_task_resource_estimate(
    const Lardon3DTask *task,
    Lardon3DResourceEstimate *estimate
);
bool lardon3d_task_execution_contract(
    const Lardon3DTask *task,
    Lardon3DTaskExecutionContract *contract
);
/* Libère la réservation courante, en obtient une nouvelle auprès du gouverneur
 * et met à jour le contrat. À appeler uniquement depuis le callback en cours
 * d'exécution. Retourne false si pause/annulation est demandée ou si le
 * gouverneur répond WAIT/REJECT (la tâche passe alors en échec). */
bool lardon3d_task_sequence_break(
    Lardon3DTask *task,
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceReservation **out_reservation,
    Lardon3DTaskExecutionContract *out_contract
);
unsigned int lardon3d_task_sequence_count(const Lardon3DTask *task);
bool lardon3d_task_reject(Lardon3DTask *task, const char *message);
const char *lardon3d_task_state_name(Lardon3DTaskState state);

#endif
