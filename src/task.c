#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lardon3d/task.h>

struct Lardon3DTask {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint64_t id;
    char name[LARDON3D_TASK_NAME_CAPACITY];
    unsigned int progress;
    Lardon3DTaskState state;
    char message[LARDON3D_TASK_MESSAGE_CAPACITY];
    struct timespec started_at;
    struct timespec finished_at;
    Lardon3DTaskCallback callback;
    void *userdata;
    Lardon3DTaskUserdataDestroy userdata_destroy;
    Lardon3DTaskFinishedCallback finished_callback;
    void *finished_userdata;
    bool finished_notified;
    bool finished_callback_running;
    char task_kind[LARDON3D_TASK_KIND_CAPACITY];
    uint32_t task_kind_version;
    Lardon3DResourceEstimate estimate;
    Lardon3DTaskExecutionContract contract;
    bool has_contract;
    bool pause_requested;
    bool cancel_requested;
    bool executing;
    Lardon3DResourceGovernor *governor;
    Lardon3DResourceReservation *current_reservation;
    unsigned int sequence_count;
};

static bool
is_terminal(Lardon3DTaskState state)
{
    return state == TASK_CANCELLED || state == TASK_FAILED
        || state == TASK_COMPLETED;
}

static bool
valid_state(Lardon3DTaskState state)
{
    return state >= TASK_PENDING && state <= TASK_COMPLETED;
}

bool
lardon3d_task_kind_is_valid(const char *task_kind)
{
    if (!task_kind) {
        return false;
    }
    size_t length = strnlen(task_kind, LARDON3D_TASK_KIND_CAPACITY);
    if (length == 0 || length >= LARDON3D_TASK_KIND_CAPACITY) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        char character = task_kind[index];
        if (!((character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || (index > 0 && (character == '.' || character == '_'
                    || character == '-')))) {
            return false;
        }
    }
    return true;
}

static Lardon3DTaskState
recovery_state(Lardon3DTaskState state)
{
    return state == TASK_RUNNING || state == TASK_PAUSED ? TASK_PENDING : state;
}

static void
copy_text(char *destination, size_t capacity, const char *text)
{
    (void)snprintf(destination, capacity, "%s", text ? text : "");
}

static void
finish_locked(
    Lardon3DTask *task,
    Lardon3DTaskState state,
    const char *message
)
{
    task->state = state;
    if (state == TASK_COMPLETED) {
        task->progress = 100;
    }
    if (message) {
        copy_text(task->message, sizeof(task->message), message);
    }
    (void)clock_gettime(CLOCK_REALTIME, &task->finished_at);
    task->executing = false;
    (void)pthread_cond_broadcast(&task->condition);
}

static void
notify_finished(Lardon3DTask *task)
{
    Lardon3DTaskFinishedCallback callback = NULL;
    void *userdata = NULL;
    (void)pthread_mutex_lock(&task->mutex);
    if (is_terminal(task->state) && !task->finished_notified) {
        task->finished_notified = true;
        callback = task->finished_callback;
        userdata = task->finished_userdata;
        task->finished_callback_running = callback != NULL;
    }
    (void)pthread_mutex_unlock(&task->mutex);
    if (callback) {
        callback(task, userdata);
        (void)pthread_mutex_lock(&task->mutex);
        task->finished_callback_running = false;
        (void)pthread_cond_broadcast(&task->condition);
        (void)pthread_mutex_unlock(&task->mutex);
    }
}

Lardon3DTask *
lardon3d_task_create(
    const char *name,
    const Lardon3DResourceEstimate *estimate,
    Lardon3DTaskCallback callback,
    void *userdata
)
{
    return lardon3d_task_create_typed(
        name, estimate, NULL, 0, callback, userdata, NULL
    );
}

Lardon3DTask *
lardon3d_task_create_typed(
    const char *name,
    const Lardon3DResourceEstimate *estimate,
    const char *task_kind,
    uint32_t task_kind_version,
    Lardon3DTaskCallback callback,
    void *userdata,
    Lardon3DTaskUserdataDestroy userdata_destroy
)
{
    bool typed = task_kind != NULL;
    if (!name || !name[0] || !estimate || !callback
        || (typed && (!lardon3d_task_kind_is_valid(task_kind)
            || task_kind_version == 0))
        || (!typed && (task_kind_version != 0 || userdata_destroy))) {
        return NULL;
    }
    Lardon3DTask *task = calloc(1, sizeof(*task));
    if (!task) {
        return NULL;
    }
    int written = snprintf(task->name, sizeof(task->name), "%s", name);
    if (written < 0 || (size_t)written >= sizeof(task->name)
        || pthread_mutex_init(&task->mutex, NULL) != 0) {
        free(task);
        return NULL;
    }
    if (pthread_cond_init(&task->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&task->mutex);
        free(task);
        return NULL;
    }
    task->state = TASK_PENDING;
    task->callback = callback;
    task->userdata = userdata;
    task->userdata_destroy = userdata_destroy;
    if (typed) {
        (void)snprintf(task->task_kind, sizeof(task->task_kind), "%s", task_kind);
        task->task_kind_version = task_kind_version;
    }
    task->estimate = *estimate;
    copy_text(task->message, sizeof(task->message), "En attente.");
    return task;
}

void
lardon3d_task_destroy(Lardon3DTask *task)
{
    if (!task) {
        return;
    }
    (void)pthread_mutex_lock(&task->mutex);
    if (!is_terminal(task->state)) {
        if (task->executing) {
            task->cancel_requested = true;
            copy_text(task->message, sizeof(task->message),
                "Annulation demandée.");
            (void)pthread_cond_broadcast(&task->condition);
        } else {
            /* Une tâche locale jamais soumise peut être abandonnée sans
             * publier une fausse annulation métier. */
            finish_locked(task, TASK_CANCELLED, "Tâche abandonnée.");
            task->finished_notified = true;
        }
    }
    (void)pthread_mutex_unlock(&task->mutex);
    (void)lardon3d_task_join(task);
    (void)pthread_cond_destroy(&task->condition);
    (void)pthread_mutex_destroy(&task->mutex);
    if (task->userdata_destroy) {
        task->userdata_destroy(task->userdata);
    }
    free(task);
}

bool
lardon3d_task_start(
    Lardon3DTask *task,
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceReservation *reservation
)
{
    Lardon3DResourceReservationInfo information;
    if (!task || !lardon3d_resource_reservation_get_active(
            governor,
            reservation,
            &information
        )) {
        return false;
    }
    (void)pthread_mutex_lock(&task->mutex);
    if (task->executing || is_terminal(task->state)) {
        (void)pthread_mutex_unlock(&task->mutex);
        return false;
    }
    task->executing = true;
    task->governor = governor;
    task->current_reservation = (Lardon3DResourceReservation *)reservation;
    task->contract = (Lardon3DTaskExecutionContract) {
        .batch_size = information.batch_size,
        .memory_bytes = information.memory_bytes,
        .gpu_memory_bytes = information.gpu_memory_bytes,
        .cpu_threads = information.cpu_threads,
        .gpu_slots = information.gpu_slots,
        .io_slots = information.io_slots,
    };
    task->has_contract = true;
    (void)clock_gettime(CLOCK_REALTIME, &task->started_at);
    task->finished_at = (struct timespec) {0};
    if (task->cancel_requested) {
        Lardon3DResourceReservation *reservation_copy = task->current_reservation;
        task->current_reservation = NULL;
        finish_locked(task, TASK_CANCELLED, "Tâche annulée.");
        (void)pthread_mutex_unlock(&task->mutex);
        if (reservation_copy) {
            (void)lardon3d_resource_governor_release(
                governor,
                reservation_copy
            );
        }
        notify_finished(task);
        return true;
    }
    while (task->pause_requested && !task->cancel_requested) {
        task->state = TASK_PAUSED;
        copy_text(task->message, sizeof(task->message), "Tâche en pause.");
        (void)pthread_cond_broadcast(&task->condition);
        (void)pthread_cond_wait(&task->condition, &task->mutex);
    }
    if (task->cancel_requested) {
        Lardon3DResourceReservation *reservation_copy = task->current_reservation;
        task->current_reservation = NULL;
        finish_locked(task, TASK_CANCELLED, "Tâche annulée.");
        (void)pthread_mutex_unlock(&task->mutex);
        if (reservation_copy) {
            (void)lardon3d_resource_governor_release(
                governor,
                reservation_copy
            );
        }
        notify_finished(task);
        return true;
    }
    task->state = TASK_RUNNING;
    copy_text(task->message, sizeof(task->message), "Tâche en cours.");
    (void)pthread_mutex_unlock(&task->mutex);

    bool succeeded = task->callback(task, task->userdata);

    (void)pthread_mutex_lock(&task->mutex);
    if (task->cancel_requested) {
        finish_locked(task, TASK_CANCELLED, "Tâche annulée.");
    } else if (task->state == TASK_FAILED || !succeeded) {
        finish_locked(
            task,
            TASK_FAILED,
            task->message[0] ? NULL : "Échec de la tâche."
        );
    } else {
        finish_locked(task, TASK_COMPLETED, "Tâche terminée.");
    }
    Lardon3DResourceReservation *reservation_copy = task->current_reservation;
    task->current_reservation = NULL;
    (void)pthread_mutex_unlock(&task->mutex);
    if (reservation_copy) {
        (void)lardon3d_resource_governor_release(
            governor,
            reservation_copy
        );
    }
    notify_finished(task);
    return true;
}

bool
lardon3d_task_set_finished_callback(
    Lardon3DTask *task,
    Lardon3DTaskFinishedCallback callback,
    void *userdata
)
{
    if (!task || !callback) return false;
    (void)pthread_mutex_lock(&task->mutex);
    bool accepted = !task->executing && task->state == TASK_PENDING
        && !task->finished_callback;
    if (accepted) {
        task->finished_callback = callback;
        task->finished_userdata = userdata;
    }
    (void)pthread_mutex_unlock(&task->mutex);
    return accepted;
}

void
lardon3d_task_request_cancel(Lardon3DTask *task)
{
    if (!task) {
        return;
    }
    bool finished_here = false;
    (void)pthread_mutex_lock(&task->mutex);
    if (!is_terminal(task->state)) {
        task->cancel_requested = true;
        copy_text(task->message, sizeof(task->message), "Annulation demandée.");
        if (!task->executing) {
            finish_locked(task, TASK_CANCELLED, "Tâche annulée.");
            finished_here = true;
        }
        (void)pthread_cond_broadcast(&task->condition);
    }
    (void)pthread_mutex_unlock(&task->mutex);
    if (finished_here) notify_finished(task);
}

bool
lardon3d_task_pause(Lardon3DTask *task)
{
    if (!task) {
        return false;
    }
    (void)pthread_mutex_lock(&task->mutex);
    bool accepted = !is_terminal(task->state) && !task->cancel_requested;
    if (accepted) {
        task->pause_requested = true;
        if (!task->executing) {
            task->state = TASK_PAUSED;
            copy_text(task->message, sizeof(task->message), "Tâche en pause.");
        }
    }
    (void)pthread_mutex_unlock(&task->mutex);
    return accepted;
}

bool
lardon3d_task_resume(Lardon3DTask *task)
{
    if (!task) {
        return false;
    }
    (void)pthread_mutex_lock(&task->mutex);
    bool accepted = !is_terminal(task->state) && task->pause_requested;
    if (accepted) {
        task->pause_requested = false;
        if (!task->executing) {
            task->state = TASK_PENDING;
            copy_text(task->message, sizeof(task->message), "En attente.");
        }
        (void)pthread_cond_broadcast(&task->condition);
    }
    (void)pthread_mutex_unlock(&task->mutex);
    return accepted;
}

bool
lardon3d_task_join(Lardon3DTask *task)
{
    if (!task) {
        return false;
    }
    (void)pthread_mutex_lock(&task->mutex);
    while (task->executing
        || (is_terminal(task->state) && task->finished_callback
            && (!task->finished_notified || task->finished_callback_running))) {
        (void)pthread_cond_wait(&task->condition, &task->mutex);
    }
    bool terminal = is_terminal(task->state);
    (void)pthread_mutex_unlock(&task->mutex);
    return terminal;
}

bool
lardon3d_task_checkpoint(Lardon3DTask *task)
{
    if (!task) {
        return false;
    }
    (void)pthread_mutex_lock(&task->mutex);
    while (task->pause_requested && !task->cancel_requested) {
        task->state = TASK_PAUSED;
        copy_text(task->message, sizeof(task->message), "Tâche en pause.");
        (void)pthread_cond_broadcast(&task->condition);
        (void)pthread_cond_wait(&task->condition, &task->mutex);
    }
    if (!task->cancel_requested && task->executing) {
        task->state = TASK_RUNNING;
    }
    bool continuing = !task->cancel_requested;
    (void)pthread_mutex_unlock(&task->mutex);
    return continuing;
}

enum {
    /* Attente bornée entre deux tentatives d'admission : 50 ms. */
    LARDON3D_SEQUENCE_ADMISSION_WAIT_NS = 50000000ULL,
};

bool
lardon3d_task_sequence_break(
    Lardon3DTask *task,
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceReservation **out_reservation,
    Lardon3DTaskExecutionContract *out_contract
)
{
    if (!task || !governor || !out_reservation || !out_contract) {
        return false;
    }
    *out_reservation = NULL;
    (void)pthread_mutex_lock(&task->mutex);
    if (!task->executing || is_terminal(task->state)) {
        (void)pthread_mutex_unlock(&task->mutex);
        return false;
    }
    while (task->pause_requested && !task->cancel_requested) {
        task->state = TASK_PAUSED;
        copy_text(task->message, sizeof(task->message), "Tâche en pause.");
        (void)pthread_cond_broadcast(&task->condition);
        (void)pthread_cond_wait(&task->condition, &task->mutex);
    }
    if (task->cancel_requested) {
        Lardon3DResourceReservation *res = task->current_reservation;
        task->current_reservation = NULL;
        finish_locked(task, TASK_CANCELLED, "Tâche annulée.");
        (void)pthread_mutex_unlock(&task->mutex);
        if (res) {
            (void)lardon3d_resource_governor_release(governor, res);
        }
        return false;
    }
    task->state = TASK_RUNNING;
    Lardon3DResourceReservation *previous = task->current_reservation;
    task->current_reservation = NULL;
    (void)pthread_mutex_unlock(&task->mutex);

    if (previous) {
        (void)lardon3d_resource_governor_release(governor, previous);
    }

    for (;;) {
        /* Vérifier pause et annulation avant chaque tentative d'admission. */
        (void)pthread_mutex_lock(&task->mutex);
        while (task->pause_requested && !task->cancel_requested) {
            task->state = TASK_PAUSED;
            copy_text(task->message, sizeof(task->message), "Tâche en pause.");
            (void)pthread_cond_broadcast(&task->condition);
            (void)pthread_cond_wait(&task->condition, &task->mutex);
        }
        if (task->cancel_requested) {
            Lardon3DResourceReservation *res = task->current_reservation;
            task->current_reservation = NULL;
            finish_locked(task, TASK_CANCELLED, "Tâche annulée.");
            (void)pthread_mutex_unlock(&task->mutex);
            if (res) {
                (void)lardon3d_resource_governor_release(governor, res);
            }
            return false;
        }
        task->state = TASK_RUNNING;
        (void)pthread_mutex_unlock(&task->mutex);

        uint64_t generation = lardon3d_resource_governor_generation(governor);
        Lardon3DResourceDecision decision;
        Lardon3DResourceReservation *next = NULL;
        bool admitted = lardon3d_resource_governor_reserve_available(
            governor,
            &task->estimate,
            &decision,
            &next
        );
        if (!admitted) {
            /* Erreur interne : échec d'allocation ou d'instantané. */
            if (next) {
                (void)lardon3d_resource_governor_release(governor, next);
            }
            (void)pthread_mutex_lock(&task->mutex);
            task->current_reservation = NULL;
            finish_locked(
                task,
                TASK_FAILED,
                "Impossible de réserver les ressources de la séquence."
            );
            (void)pthread_mutex_unlock(&task->mutex);
            return false;
        }
        switch (decision.kind) {
        case LARDON3D_RESOURCE_START:
        case LARDON3D_RESOURCE_REDUCE_BATCH: {
            if (!next) {
                (void)pthread_mutex_lock(&task->mutex);
                task->current_reservation = NULL;
                finish_locked(
                    task,
                    TASK_FAILED,
                    "Réservation de séquence invalide."
                );
                (void)pthread_mutex_unlock(&task->mutex);
                return false;
            }
            Lardon3DResourceReservationInfo information;
            if (!lardon3d_resource_reservation_get_active(
                    governor,
                    next,
                    &information
                )) {
                (void)lardon3d_resource_governor_release(governor, next);
                (void)pthread_mutex_lock(&task->mutex);
                task->current_reservation = NULL;
                finish_locked(
                    task,
                    TASK_FAILED,
                    "Réservation de séquence invalide."
                );
                (void)pthread_mutex_unlock(&task->mutex);
                return false;
            }
            (void)pthread_mutex_lock(&task->mutex);
            task->current_reservation = next;
            task->contract = (Lardon3DTaskExecutionContract) {
                .batch_size = information.batch_size,
                .memory_bytes = information.memory_bytes,
                .gpu_memory_bytes = information.gpu_memory_bytes,
                .cpu_threads = information.cpu_threads,
                .gpu_slots = information.gpu_slots,
                .io_slots = information.io_slots,
            };
            task->has_contract = true;
            ++task->sequence_count;
            *out_reservation = next;
            *out_contract = task->contract;
            (void)pthread_mutex_unlock(&task->mutex);
            return true;
        }
        case LARDON3D_RESOURCE_REJECT:
            if (next) {
                (void)lardon3d_resource_governor_release(governor, next);
            }
            (void)pthread_mutex_lock(&task->mutex);
            task->current_reservation = NULL;
            finish_locked(
                task,
                TASK_FAILED,
                decision.reason[0] ? decision.reason
                    : "Ressources impossibles à réserver."
            );
            (void)pthread_mutex_unlock(&task->mutex);
            return false;
        case LARDON3D_RESOURCE_WAIT:
            /* Indisponibilité temporaire : ne pas échouer, attendre un
             * changement de ressources puis retenter l'admission. */
            if (next) {
                (void)lardon3d_resource_governor_release(governor, next);
            }
            (void)lardon3d_resource_governor_wait_for_change(
                governor,
                generation,
                LARDON3D_SEQUENCE_ADMISSION_WAIT_NS
            );
            break;
        default:
            /* Décision inconnue : erreur interne, ne jamais boucler. */
            if (next) {
                (void)lardon3d_resource_governor_release(governor, next);
            }
            (void)pthread_mutex_lock(&task->mutex);
            task->current_reservation = NULL;
            finish_locked(
                task,
                TASK_FAILED,
                "Décision de ressource inconnue."
            );
            (void)pthread_mutex_unlock(&task->mutex);
            return false;
        }
    }
}

unsigned int
lardon3d_task_sequence_count(const Lardon3DTask *task)
{
    if (!task) {
        return 0;
    }
    Lardon3DTask *mutable_task = (Lardon3DTask *)task;
    (void)pthread_mutex_lock(&mutable_task->mutex);
    unsigned int count = task->sequence_count;
    (void)pthread_mutex_unlock(&mutable_task->mutex);
    return count;
}

bool
lardon3d_task_set_progress(
    Lardon3DTask *task,
    unsigned int progress,
    const char *message
)
{
    if (!task || progress > 100) {
        return false;
    }
    (void)pthread_mutex_lock(&task->mutex);
    bool accepted = !is_terminal(task->state);
    if (accepted) {
        task->progress = progress;
        if (message) {
            copy_text(task->message, sizeof(task->message), message);
        }
    }
    (void)pthread_mutex_unlock(&task->mutex);
    return accepted;
}

bool
lardon3d_task_fail(Lardon3DTask *task, const char *message)
{
    if (!task) {
        return false;
    }
    (void)pthread_mutex_lock(&task->mutex);
    bool accepted = task->executing && !is_terminal(task->state);
    if (accepted) {
        task->state = TASK_FAILED;
        copy_text(
            task->message,
            sizeof(task->message),
            message ? message : "Échec de la tâche."
        );
    }
    (void)pthread_mutex_unlock(&task->mutex);
    return accepted;
}

bool
lardon3d_task_snapshot(
    const Lardon3DTask *task,
    Lardon3DTaskSnapshot *snapshot
)
{
    if (!task || !snapshot) {
        return false;
    }
    Lardon3DTask *mutable_task = (Lardon3DTask *)task;
    (void)pthread_mutex_lock(&mutable_task->mutex);
    *snapshot = (Lardon3DTaskSnapshot) {
        .id = task->id,
        .progress = task->progress,
        .state = task->state,
        .started_at = task->started_at,
        .finished_at = task->finished_at,
    };
    copy_text(snapshot->name, sizeof(snapshot->name), task->name);
    copy_text(snapshot->message, sizeof(snapshot->message), task->message);
    (void)pthread_mutex_unlock(&mutable_task->mutex);
    return true;
}

bool
lardon3d_task_durable_snapshot(
    const Lardon3DTask *task,
    Lardon3DTaskDurableSnapshot *snapshot
)
{
    if (!task || !snapshot) {
        return false;
    }
    Lardon3DTask *mutable_task = (Lardon3DTask *)task;
    (void)pthread_mutex_lock(&mutable_task->mutex);
    *snapshot = (Lardon3DTaskDurableSnapshot) {
        .id = task->id,
        .estimate = task->estimate,
        .progress = task->progress,
        .saved_state = task->state,
        .recovery_state = recovery_state(task->state),
        .started_at = task->started_at,
        .finished_at = task->finished_at,
        .sequence_count = task->sequence_count,
    };
    copy_text(snapshot->name, sizeof(snapshot->name), task->name);
    copy_text(snapshot->message, sizeof(snapshot->message), task->message);
    (void)pthread_mutex_unlock(&mutable_task->mutex);
    return true;
}

Lardon3DTask *
lardon3d_task_restore(
    const Lardon3DTaskDurableSnapshot *snapshot,
    Lardon3DTaskCallback callback,
    void *userdata
)
{
    return lardon3d_task_restore_typed(
        snapshot, NULL, 0, callback, userdata, NULL
    );
}

Lardon3DTask *
lardon3d_task_restore_typed(
    const Lardon3DTaskDurableSnapshot *snapshot,
    const char *task_kind,
    uint32_t task_kind_version,
    Lardon3DTaskCallback callback,
    void *userdata,
    Lardon3DTaskUserdataDestroy userdata_destroy
)
{
    if (!snapshot || snapshot->id == 0 || !snapshot->name[0] || !callback
        || memchr(snapshot->name, '\0', sizeof(snapshot->name)) == NULL
        || memchr(snapshot->message, '\0', sizeof(snapshot->message)) == NULL
        || snapshot->progress > 100 || !valid_state(snapshot->saved_state)
        || !valid_state(snapshot->recovery_state)
        || snapshot->recovery_state != recovery_state(snapshot->saved_state)
        || (snapshot->recovery_state == TASK_COMPLETED
            && snapshot->progress != 100)
        || snapshot->estimate.minimum_batch_size == 0
        || snapshot->estimate.maximum_batch_size
            < snapshot->estimate.minimum_batch_size
        || snapshot->estimate.desired_cpu_threads == 0
        || snapshot->estimate.task_class < LARDON3D_RESOURCE_TASK_GENERAL
        || snapshot->estimate.task_class > LARDON3D_RESOURCE_TASK_MIXED
        || ((snapshot->estimate.gpu_memory_fixed_bytes != 0
                || snapshot->estimate.gpu_memory_bytes_per_item != 0)
            && snapshot->estimate.desired_gpu_slots == 0)) {
        return NULL;
    }
    Lardon3DTask *task = lardon3d_task_create_typed(
        snapshot->name,
        &snapshot->estimate,
        task_kind,
        task_kind_version,
        callback,
        userdata,
        userdata_destroy
    );
    if (!task) {
        return NULL;
    }
    (void)pthread_mutex_lock(&task->mutex);
    task->id = snapshot->id;
    task->progress = snapshot->progress;
    task->state = snapshot->recovery_state;
    copy_text(task->message, sizeof(task->message), snapshot->message);
    task->started_at = snapshot->started_at;
    task->finished_at = snapshot->finished_at;
    task->sequence_count = snapshot->sequence_count;
    (void)pthread_mutex_unlock(&task->mutex);
    return task;
}

bool
lardon3d_task_kind(
    const Lardon3DTask *task,
    char task_kind[LARDON3D_TASK_KIND_CAPACITY],
    uint32_t *task_kind_version
)
{
    if (!task || !task_kind || !task_kind_version) {
        return false;
    }
    Lardon3DTask *mutable_task = (Lardon3DTask *)task;
    (void)pthread_mutex_lock(&mutable_task->mutex);
    bool typed = task->task_kind[0] != '\0';
    if (typed) {
        copy_text(task_kind, LARDON3D_TASK_KIND_CAPACITY, task->task_kind);
        *task_kind_version = task->task_kind_version;
    } else {
        task_kind[0] = '\0';
        *task_kind_version = 0;
    }
    (void)pthread_mutex_unlock(&mutable_task->mutex);
    return typed;
}

uint64_t
lardon3d_task_id(const Lardon3DTask *task)
{
    if (!task) {
        return 0;
    }
    Lardon3DTaskSnapshot snapshot;
    return lardon3d_task_snapshot(task, &snapshot) ? snapshot.id : 0;
}

bool
lardon3d_task_assign_id(Lardon3DTask *task, uint64_t id)
{
    if (!task || id == 0) {
        return false;
    }
    (void)pthread_mutex_lock(&task->mutex);
    bool accepted = task->id == 0 && task->state == TASK_PENDING;
    if (accepted) {
        task->id = id;
    }
    (void)pthread_mutex_unlock(&task->mutex);
    return accepted;
}

bool
lardon3d_task_resource_estimate(
    const Lardon3DTask *task,
    Lardon3DResourceEstimate *estimate
)
{
    if (!task || !estimate) {
        return false;
    }
    Lardon3DTask *mutable_task = (Lardon3DTask *)task;
    (void)pthread_mutex_lock(&mutable_task->mutex);
    *estimate = task->estimate;
    (void)pthread_mutex_unlock(&mutable_task->mutex);
    return true;
}

bool
lardon3d_task_execution_contract(
    const Lardon3DTask *task,
    Lardon3DTaskExecutionContract *contract
)
{
    if (!task || !contract) {
        return false;
    }
    Lardon3DTask *mutable_task = (Lardon3DTask *)task;
    (void)pthread_mutex_lock(&mutable_task->mutex);
    bool available = task->has_contract;
    if (available) {
        *contract = task->contract;
    }
    (void)pthread_mutex_unlock(&mutable_task->mutex);
    return available;
}

bool
lardon3d_task_reject(Lardon3DTask *task, const char *message)
{
    if (!task) {
        return false;
    }
    (void)pthread_mutex_lock(&task->mutex);
    bool accepted = !task->executing && !is_terminal(task->state);
    if (accepted) {
        finish_locked(
            task,
            TASK_FAILED,
            message ? message : "Ressources impossibles à réserver."
        );
    }
    (void)pthread_mutex_unlock(&task->mutex);
    if (accepted) notify_finished(task);
    return accepted;
}

const char *
lardon3d_task_state_name(Lardon3DTaskState state)
{
    switch (state) {
    case TASK_PENDING:
        return "En attente";
    case TASK_RUNNING:
        return "En cours";
    case TASK_PAUSED:
        return "En pause";
    case TASK_CANCELLED:
        return "Annulée";
    case TASK_FAILED:
        return "Échec";
    case TASK_COMPLETED:
        return "Terminée";
    default:
        return "Inconnu";
    }
}
