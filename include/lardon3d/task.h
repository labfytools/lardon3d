#ifndef LARDON3D_TASK_H
#define LARDON3D_TASK_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/resource_governor.h>

enum {
    LARDON3D_TASK_NAME_CAPACITY = 128,
    LARDON3D_TASK_MESSAGE_CAPACITY = 256,
    LARDON3D_TASK_KIND_CAPACITY = 65,
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
typedef void (*Lardon3DTaskUserdataDestroy)(void *userdata);
typedef void (*Lardon3DTaskFinishedCallback)(
    const Lardon3DTask *task,
    void *userdata
);

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

typedef struct {
    uint64_t id;
    char name[LARDON3D_TASK_NAME_CAPACITY];
    Lardon3DResourceEstimate estimate;
    unsigned int progress;
    Lardon3DTaskState saved_state;
    Lardon3DTaskState recovery_state;
    char message[LARDON3D_TASK_MESSAGE_CAPACITY];
    struct timespec started_at;
    struct timespec finished_at;
    unsigned int sequence_count;
} Lardon3DTaskDurableSnapshot;

Lardon3DTask *lardon3d_task_create(
    const char *name,
    const Lardon3DResourceEstimate *estimate,
    Lardon3DTaskCallback callback,
    void *userdata
);
Lardon3DTask *lardon3d_task_create_typed(
    const char *name,
    const Lardon3DResourceEstimate *estimate,
    const char *task_kind,
    uint32_t task_kind_version,
    Lardon3DTaskCallback callback,
    void *userdata,
    Lardon3DTaskUserdataDestroy userdata_destroy
);
void lardon3d_task_destroy(Lardon3DTask *task);
/* Exécute le callback dans le thread appelant. Le callback est invoqué hors
 * mutex de tâche; le contract d'exécution et l'état appartiennent à la tâche.
 */
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
bool lardon3d_task_durable_snapshot(
    const Lardon3DTask *task,
    Lardon3DTaskDurableSnapshot *snapshot
);
Lardon3DTask *lardon3d_task_restore(
    const Lardon3DTaskDurableSnapshot *snapshot,
    Lardon3DTaskCallback callback,
    void *userdata
);
Lardon3DTask *lardon3d_task_restore_typed(
    const Lardon3DTaskDurableSnapshot *snapshot,
    const char *task_kind,
    uint32_t task_kind_version,
    Lardon3DTaskCallback callback,
    void *userdata,
    Lardon3DTaskUserdataDestroy userdata_destroy
);
/* Une restauration typée réussie transfère userdata/userdata_destroy à la
 * tâche. En cas d'échec, l'appelant en reste propriétaire. */
bool lardon3d_task_kind_is_valid(const char *task_kind);
bool lardon3d_task_kind(
    const Lardon3DTask *task,
    char task_kind[LARDON3D_TASK_KIND_CAPACITY],
    uint32_t *task_kind_version
);
/* Appelé au plus une fois, hors mutex de tâche et après libération de la
 * réservation terminale. Le userdata de tâche reste vivant jusqu'au retour. */
bool lardon3d_task_set_finished_callback(
    Lardon3DTask *task,
    Lardon3DTaskFinishedCallback callback,
    void *userdata
);
uint64_t lardon3d_task_id(const Lardon3DTask *task);
bool lardon3d_task_assign_id(Lardon3DTask *task, uint64_t id);
bool lardon3d_task_resource_estimate(
    const Lardon3DTask *task,
    Lardon3DResourceEstimate *estimate
);
/* L'exécution ne reçoit pas de politique d'admission : c'est au gouverneur de
 * confirmer la réservation avant l'exécution.
 */
bool lardon3d_task_execution_contract(
    const Lardon3DTask *task,
    Lardon3DTaskExecutionContract *contract
);
/* Libère la réservation courante, en obtient une nouvelle auprès du gouverneur
 * et met à jour le contrat. À appeler uniquement depuis le callback en cours
 * d'exécution. Une réponse WAIT du gouverneur est une indisponibilité
 * temporaire : la fonction attend un changement de ressources puis retente
 * l'admission sans échouer la tâche. Les bornes de lot se poursuivent après
 * cette nouvelle admission. Retourne false si la tâche est annulée
 * (TASK_CANCELLED), si le gouverneur répond REJECT ou en cas d'erreur interne
 * (TASK_FAILED). */
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
