#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lardon3d/import_task.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/image_view.h>
#include <lardon3d/layout.h>
#include <lardon3d/project.h>
#include <lardon3d/runtime_observer.h>
#include <lardon3d/runtime_session.h>
#include <lardon3d/tui.h>
#include <lardon3d/tui_model.h>
#include <lardon3d/tui_optics.h>
#include <lardon3d/tui_ssd_async.h>

enum {
    MINIMUM_ROWS = 15,
    MINIMUM_COLUMNS = 60,
};

typedef enum {
    INPUT_NONE = 0,
    INPUT_PROJECT_CREATE,
    INPUT_PROJECT_OPEN,
    INPUT_IMPORT_DIRECTORY,
    INPUT_IMAGE_FILTER,
    INPUT_OPTICS_CREATE_BODY,
    INPUT_OPTICS_CREATE_LENS,
    INPUT_OPTICS_CREATE_CONFIGURATION,
    INPUT_OPTICS_INSPECT_CAPTURE,
    INPUT_OPTICS_ASSIGN_CAPTURE,
    INPUT_OPTICS_ASSIGN_GROUP,
    INPUT_OPTICS_LOOKUP_METADATA,
} InputMode;

typedef struct {
    InputMode mode;
    char text[PATH_MAX];
    size_t length;
} TuiInput;

typedef struct {
    Lardon3DRuntimeObserver *observer;
    Lardon3DRuntimeSnapshot observation;
    Lardon3DTuiSsdAsync *ssd_operation;
    Lardon3DTuiSsdAsyncSnapshot ssd_operation_snapshot;
    Lardon3DTuiOptics *optics;
    Lardon3DTuiOpticsSnapshot optics_snapshot;
    Lardon3DProjectDb *optics_database;
    bool optics_retry_requested;
    Lardon3DTuiPalette palette;
    size_t selected_task;
    bool fatal_error;
} TuiRuntime;

static size_t
catalog_page_size(void)
{
    int rows;
    int columns;
    getmaxyx(stdscr, rows, columns);
    (void)columns;
    return rows > 18 ? (size_t)(rows - 18) : 1;
}

static void
normalize_view_navigation(Lardon3DAppState *state)
{
    lardon3d_image_view_normalize(state->image_view, catalog_page_size());
}

static bool
reload_catalog(Lardon3DAppState *state, bool announce_success)
{
    Lardon3DImageSort sort = lardon3d_image_view_sort(state->image_view);
    char filter[LARDON3D_IMAGE_FILTER_CAPACITY];
    (void)snprintf(
        filter,
        sizeof(filter),
        "%s",
        lardon3d_image_view_filter(state->image_view)
    );
    size_t selected_catalog = 0;
    bool preserve_selection = lardon3d_image_view_catalog_index(
        state->image_view,
        lardon3d_image_view_selection(state->image_view),
        &selected_catalog
    );
    lardon3d_image_view_destroy(state->image_view);
    state->image_view = NULL;
    lardon3d_image_catalog_destroy(state->image_catalog);
    state->image_catalog = NULL;
    char message[sizeof(state->status_message)] = "";
    state->image_catalog = lardon3d_image_catalog_load(
        state,
        message,
        sizeof(message)
    );
    if (!state->image_catalog) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "%s",
            message
        );
        return false;
    }
    char catalog_message[sizeof(message)];
    (void)snprintf(catalog_message, sizeof(catalog_message), "%s", message);
    char view_error[sizeof(message)];
    state->image_view = lardon3d_image_view_create(state->image_catalog);
    if (!state->image_view
        || !lardon3d_image_view_set_sort(state->image_view, sort)
        || !lardon3d_image_view_set_filter(
            state->image_view,
            filter,
            view_error,
            sizeof(view_error)
        )) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "Erreur : impossible de construire la vue des images."
        );
        lardon3d_image_view_destroy(state->image_view);
        state->image_view = NULL;
        return false;
    }
    if (preserve_selection) {
        size_t count = lardon3d_image_view_count(state->image_view);
        for (size_t index = 0; index < count; ++index) {
            size_t catalog_index;
            if (lardon3d_image_view_catalog_index(
                    state->image_view,
                    index,
                    &catalog_index
                )
                && catalog_index == selected_catalog) {
                lardon3d_image_view_select(
                    state->image_view,
                    index,
                    catalog_page_size()
                );
                break;
            }
        }
    }
    normalize_view_navigation(state);
    if (catalog_message[0]) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "%s",
            catalog_message
        );
        return true;
    }
    if (announce_success) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "Catalogue rechargé : %zu image%s.",
            lardon3d_image_view_count(state->image_view),
            lardon3d_image_view_count(state->image_view) == 1 ? "" : "s"
        );
    }
    return true;
}

static void
sync_optics(Lardon3DAppState *state, TuiRuntime *runtime)
{
    if (runtime->optics_database != state->project_db
        || runtime->optics_retry_requested) {
        lardon3d_tui_optics_unbind(runtime->optics);
        runtime->optics_database = state->project_db;
        runtime->optics_retry_requested = false;
        if (state->project_db) {
            (void)lardon3d_tui_optics_bind(
                runtime->optics, state->project_db);
        }
    }
    (void)lardon3d_tui_optics_snapshot(
        runtime->optics, &runtime->optics_snapshot);
}

static bool
reset_project_session(Lardon3DAppState *state, TuiRuntime *runtime)
{
    /* Observers borrow Queue/DB owners and must disappear before the exact
     * Queue-finished-callback -> DB-close boundary executes. */
    lardon3d_tui_optics_unbind(runtime->optics);
    runtime->optics_database = NULL;
    runtime->optics_retry_requested = false;
    lardon3d_runtime_observer_destroy(runtime->observer);
    runtime->observer = NULL;
    if (!lardon3d_runtime_project_boundary(
            state, LARDON3D_TASK_QUEUE_PRODUCTION_CAPACITY)) {
        runtime->fatal_error = true;
        state->running = false;
        return false;
    }
    runtime->observer = lardon3d_runtime_observer_create(
        &state->hardware_profile, state->task_queue,
        state->resource_governor);
    if (!runtime->observer) {
        (void)snprintf(state->status_message,
            sizeof(state->status_message),
            "Erreur : impossible de recréer l'observateur runtime.");
        runtime->fatal_error = true;
        state->running = false;
        return false;
    }
    runtime->observation = (Lardon3DRuntimeSnapshot) {0};
    runtime->selected_task = 0;
    return true;
}

static const char *
input_label(InputMode mode)
{
    switch (mode) {
    case INPUT_PROJECT_OPEN:
        return "Nom du dossier projet :";
    case INPUT_IMPORT_DIRECTORY:
        return "Dossier source :";
    case INPUT_IMAGE_FILTER:
        return "Filtre :";
    case INPUT_OPTICS_CREATE_BODY:
        return "Body immutable: manufacturer|model|name";
    case INPUT_OPTICS_CREATE_LENS:
        return "Lens: interface|range|min_mm|max_mm|maker|model|name";
    case INPUT_OPTICS_CREATE_CONFIGURATION:
        return "Focale entière mm, ou ? pour absence explicite";
    case INPUT_OPTICS_INSPECT_CAPTURE:
        return "Capture ID à inspecter :";
    case INPUT_OPTICS_ASSIGN_CAPTURE:
        return "Capture ID à assigner :";
    case INPUT_OPTICS_ASSIGN_GROUP:
        return "Campaign Task ID:group ID :";
    case INPUT_OPTICS_LOOKUP_METADATA:
        return "make|model|lens_make|lens_model (exact; lens vide permis)";
    case INPUT_PROJECT_CREATE:
    case INPUT_NONE:
    default:
        return "Nom du nouveau projet :";
    }
}

static void
merge_ssd_observation(
    Lardon3DRuntimeSnapshot *observation,
    const Lardon3DTuiSsdAsyncSnapshot *ssd
)
{
    if (!observation || !ssd || !ssd->controller_snapshot_known) {
        return;
    }
    observation->ssd_controller_available = true;
    observation->ssd = ssd->controller_snapshot;
    /* Physical identity/actions stay on the SSD panel. Governor usage is
     * captured separately by RuntimeObserver from the registered copy and is
     * never overwritten by an independently timed controller observation. */
}

static void
redraw(
    Lardon3DAppState *state,
    const TuiInput *input,
    Lardon3DImportTask *task,
    TuiRuntime *runtime
)
{
    int rows;
    int columns;
    getmaxyx(stdscr, rows, columns);
    (void)lardon3d_runtime_observer_refresh(
        runtime->observer, state->project_loaded, false,
        &runtime->observation);
    if (runtime->selected_task >= runtime->observation.task_count) {
        runtime->selected_task = runtime->observation.task_count > 0
            ? runtime->observation.task_count - 1 : 0;
    }
    if (runtime->ssd_operation) {
        /* This schedules at most one coalesced worker call; no controller or
         * D-Bus work executes on the ncurses thread. */
        (void)lardon3d_tui_ssd_async_refresh(runtime->ssd_operation);
        (void)lardon3d_tui_ssd_async_poll(
            runtime->ssd_operation, &runtime->ssd_operation_snapshot);
        merge_ssd_observation(
            &runtime->observation, &runtime->ssd_operation_snapshot);
    } else {
        runtime->ssd_operation_snapshot =
            (Lardon3DTuiSsdAsyncSnapshot) {0};
    }
    sync_optics(state, runtime);

    Lardon3DImportTaskSnapshot snapshot;
    const Lardon3DImportTaskSnapshot *displayed_snapshot = NULL;
    if (task && lardon3d_import_task_snapshot(task, &snapshot)) {
        displayed_snapshot = &snapshot;
    }
    lardon3d_layout_draw_runtime(
        state,
        input->mode != INPUT_NONE ? input->text : NULL,
        input_label(input->mode),
        displayed_snapshot,
        &runtime->observation,
        &runtime->ssd_operation_snapshot,
        &runtime->optics_snapshot,
        runtime->selected_task,
        &runtime->palette,
        input->mode != INPUT_NONE
            ? LARDON3D_TUI_INTERACTION_TEXT_INPUT
            : (task ? LARDON3D_TUI_INTERACTION_IMPORT_RUNNING
                    : LARDON3D_TUI_INTERACTION_IDLE),
        rows,
        columns
    );
    (void)curs_set(
        input->mode != INPUT_NONE
            && rows >= MINIMUM_ROWS && columns >= MINIMUM_COLUMNS ? 1 : 0);
}

static bool
is_optics_input(InputMode mode)
{
    return mode >= INPUT_OPTICS_CREATE_BODY
        && mode <= INPUT_OPTICS_LOOKUP_METADATA;
}

static bool
parse_positive_u64(const char *text, uint64_t maximum, uint64_t *value)
{
    if (value) {
        *value = 0;
    }
    if (!text || !text[0] || text[0] == '-' || !value || maximum == 0) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed == 0
        || parsed > maximum) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool
parse_nonnegative_u32(const char *text, uint32_t *value)
{
    if (value) {
        *value = 0;
    }
    if (!text || !text[0] || text[0] == '-' || !value) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool
parse_focal_mm(const char *text, uint32_t *micrometres)
{
    uint32_t millimetres;
    if (!parse_nonnegative_u32(text, &millimetres) || millimetres == 0
        || millimetres > UINT32_MAX / 1000U) {
        return false;
    }
    *micrometres = millimetres * 1000U;
    return true;
}

static bool
split_exact_fields(char *text, char **fields, size_t expected_count)
{
    if (!text || !fields || expected_count == 0) {
        return false;
    }
    size_t count = 1;
    fields[0] = text;
    for (char *character = text; *character; ++character) {
        if (*character != '|') {
            continue;
        }
        if (count >= expected_count) {
            return false;
        }
        *character = '\0';
        fields[count++] = character + 1;
    }
    return count == expected_count;
}

static bool
copy_optical_text(
    char destination[LARDON3D_OPTICAL_TEXT_CAPACITY],
    const char *source
)
{
    int written = snprintf(destination, LARDON3D_OPTICAL_TEXT_CAPACITY,
        "%s", source ? source : "");
    return written >= 0 && written < LARDON3D_OPTICAL_TEXT_CAPACITY;
}

static bool
parse_lens_interface(
    const char *text,
    Lardon3DOpticalLensInterface *interface_kind
)
{
    if (strcmp(text, "manual") == 0) {
        *interface_kind = LARDON3D_OPTICAL_LENS_MANUAL;
    } else if (strcmp(text, "electronic") == 0) {
        *interface_kind = LARDON3D_OPTICAL_LENS_ELECTRONIC;
    } else if (strcmp(text, "integrated") == 0) {
        *interface_kind = LARDON3D_OPTICAL_LENS_INTEGRATED;
    } else {
        return false;
    }
    return true;
}

static bool
parse_focal_range(
    const char *text,
    Lardon3DOpticalFocalRangeKind *range_kind
)
{
    if (strcmp(text, "unknown") == 0) {
        *range_kind = LARDON3D_OPTICAL_FOCAL_RANGE_UNKNOWN;
    } else if (strcmp(text, "prime") == 0) {
        *range_kind = LARDON3D_OPTICAL_FOCAL_RANGE_PRIME;
    } else if (strcmp(text, "zoom") == 0) {
        *range_kind = LARDON3D_OPTICAL_FOCAL_RANGE_ZOOM;
    } else {
        return false;
    }
    return true;
}

static void
copy_optics_message(Lardon3DAppState *state, TuiRuntime *runtime)
{
    (void)lardon3d_tui_optics_snapshot(
        runtime->optics, &runtime->optics_snapshot);
    (void)snprintf(state->status_message, sizeof(state->status_message),
        "%s", runtime->optics_snapshot.message);
}

static bool
complete_optics_input(
    Lardon3DAppState *state,
    TuiInput *input,
    TuiRuntime *runtime
)
{
    char copy[sizeof(input->text)];
    (void)snprintf(copy, sizeof(copy), "%s", input->text);
    bool success = false;
    bool model_called = false;

    if (!runtime->optics_database) {
        (void)snprintf(state->status_message, sizeof(state->status_message),
            "Aucun Project DB lié; aucune identité optique n'est devinée.");
        input->mode = INPUT_NONE;
        return false;
    }

    if (input->mode == INPUT_OPTICS_CREATE_BODY) {
        char *fields[3];
        Lardon3DOpticalCameraBodyProfile body = {0};
        if (split_exact_fields(copy, fields, 3)
            && copy_optical_text(body.manufacturer, fields[0])
            && copy_optical_text(body.model, fields[1])
            && copy_optical_text(body.name, fields[2])) {
            model_called = true;
            success = lardon3d_tui_optics_create_body(
                runtime->optics, &body);
        } else {
            (void)snprintf(state->status_message,
                sizeof(state->status_message),
                "Format body invalide: manufacturer|model|name.");
        }
    } else if (input->mode == INPUT_OPTICS_CREATE_LENS) {
        char *fields[7];
        Lardon3DOpticalLensProfile lens = {0};
        uint32_t minimum_mm;
        uint32_t maximum_mm;
        bool parsed = split_exact_fields(copy, fields, 7)
            && parse_lens_interface(fields[0], &lens.interface_kind)
            && parse_focal_range(fields[1], &lens.focal_range_kind)
            && parse_nonnegative_u32(fields[2], &minimum_mm)
            && parse_nonnegative_u32(fields[3], &maximum_mm)
            && minimum_mm <= UINT32_MAX / 1000U
            && maximum_mm <= UINT32_MAX / 1000U
            && copy_optical_text(lens.manufacturer, fields[4])
            && copy_optical_text(lens.model, fields[5])
            && copy_optical_text(lens.name, fields[6]);
        if (parsed) {
            lens.minimum_focal_um = minimum_mm * 1000U;
            lens.maximum_focal_um = maximum_mm * 1000U;
            model_called = true;
            success = lardon3d_tui_optics_create_lens(
                runtime->optics, &lens);
        } else {
            (void)snprintf(state->status_message,
                sizeof(state->status_message),
                "Format lens invalide; interface manual/electronic/integrated, range unknown/prime/zoom.");
        }
    } else if (input->mode == INPUT_OPTICS_CREATE_CONFIGURATION) {
        bool has_focal = strcmp(copy, "?") != 0;
        uint32_t focal_um = 0;
        if (!has_focal || parse_focal_mm(copy, &focal_um)) {
            model_called = true;
            success = lardon3d_tui_optics_create_configuration(
                runtime->optics, has_focal, focal_um);
        } else {
            (void)snprintf(state->status_message,
                sizeof(state->status_message),
                "Focale invalide: entier positif en millimètres ou ?.");
        }
    } else if (input->mode == INPUT_OPTICS_INSPECT_CAPTURE
        || input->mode == INPUT_OPTICS_ASSIGN_CAPTURE) {
        uint64_t capture_id;
        if (parse_positive_u64(copy, INT64_MAX, &capture_id)) {
            model_called = true;
            success = input->mode == INPUT_OPTICS_INSPECT_CAPTURE
                ? lardon3d_tui_optics_inspect_capture(
                    runtime->optics, capture_id)
                : lardon3d_tui_optics_assign_capture(
                    runtime->optics, capture_id);
        } else {
            (void)snprintf(state->status_message,
                sizeof(state->status_message),
                "Capture ID invalide (SQLite INTEGER positif requis).");
        }
    } else if (input->mode == INPUT_OPTICS_ASSIGN_GROUP) {
        char *separator = strchr(copy, ':');
        uint64_t campaign_id;
        uint64_t group_id;
        if (separator && !strchr(separator + 1, ':')) {
            *separator = '\0';
        }
        if (separator
            && parse_positive_u64(copy, INT64_MAX, &campaign_id)
            && parse_positive_u64(separator + 1, UINT32_MAX, &group_id)) {
            model_called = true;
            success = lardon3d_tui_optics_assign_campaign_group(
                runtime->optics, campaign_id, (uint32_t)group_id);
        } else {
            (void)snprintf(state->status_message,
                sizeof(state->status_message),
                "Format campagne invalide: TaskID:groupID avec IDs positifs.");
        }
    } else if (input->mode == INPUT_OPTICS_LOOKUP_METADATA) {
        char *fields[4];
        if (split_exact_fields(copy, fields, 4)) {
            model_called = true;
            success = lardon3d_tui_optics_lookup_exact_metadata(
                runtime->optics, fields[0], fields[1], fields[2], fields[3]);
        } else {
            (void)snprintf(state->status_message,
                sizeof(state->status_message),
                "Format métadonnées invalide: make|model|lens_make|lens_model.");
        }
    }
    if (model_called) {
        copy_optics_message(state, runtime);
    }
    input->mode = INPUT_NONE;
    input->text[0] = '\0';
    input->length = 0;
    return success;
}

static bool
start_import_task(
    Lardon3DAppState *state,
    TuiInput *input,
    Lardon3DImportTask **task
)
{
    *task = lardon3d_import_task_create();
    if (!*task) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "Erreur : impossible de créer la tâche d'import."
        );
        input->mode = INPUT_NONE;
        return false;
    }
    if (!lardon3d_import_task_start(*task, state, input->text)) {
        Lardon3DImportTaskSnapshot snapshot;
        if (lardon3d_import_task_snapshot(*task, &snapshot)
            && snapshot.message[0]) {
            (void)snprintf(
                state->status_message,
                sizeof(state->status_message),
                "%s",
                snapshot.message
            );
        }
        lardon3d_import_task_destroy(*task);
        *task = NULL;
        input->mode = INPUT_NONE;
        return false;
    }
    input->mode = INPUT_NONE;
    return true;
}

static bool handle_ssd_key(
    Lardon3DAppState *state,
    TuiRuntime *runtime
);

static bool
handle_active_input(
    Lardon3DAppState *state,
    TuiInput *input,
    Lardon3DImportTask **task,
    TuiRuntime *runtime,
    int key
)
{
    if (key == KEY_RESIZE) {
        return true;
    }
    if (key == KEY_F(10)) {
        /* F10 remains reachable while a text field owns ordinary keystrokes.
         * Discard the uncommitted field before switching screens so hidden
         * input cannot later mutate a Project/optical workflow. */
        *input = (TuiInput) {0};
        return handle_ssd_key(state, runtime);
    }

    if (key == 27) {
        const char *message = "Création du projet annulée.";
        if (input->mode == INPUT_PROJECT_OPEN) {
            message = "Ouverture du projet annulée.";
        } else if (input->mode == INPUT_IMPORT_DIRECTORY) {
            message = "Import annulé.";
        } else if (input->mode == INPUT_IMAGE_FILTER) {
            message = "Filtre annulé.";
        } else if (is_optics_input(input->mode)) {
            message = "Opération optique annulée.";
        }
        input->mode = INPUT_NONE;
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "%s",
            message
        );
        return true;
    }

    if (key == '\n' || key == '\r' || key == KEY_ENTER) {
        if (is_optics_input(input->mode)) {
            (void)complete_optics_input(state, input, runtime);
        } else if (input->mode == INPUT_PROJECT_OPEN) {
            if (reset_project_session(state, runtime)
                && lardon3d_project_open(state, input->text)) {
                (void)reload_catalog(state, false);
            }
        } else if (input->mode == INPUT_IMPORT_DIRECTORY) {
            (void)start_import_task(state, input, task);
        } else if (input->mode == INPUT_IMAGE_FILTER) {
            char message[sizeof(state->status_message)];
            if (lardon3d_image_view_set_filter(
                    state->image_view,
                    input->text,
                    message,
                    sizeof(message)
                )) {
                normalize_view_navigation(state);
                if (lardon3d_image_view_count(state->image_view) == 0) {
                    (void)snprintf(
                        state->status_message,
                        sizeof(state->status_message),
                        "Aucune image ne correspond au filtre."
                    );
                } else {
                    (void)snprintf(
                        state->status_message,
                        sizeof(state->status_message),
                        "Filtre appliqué : %.230s",
                        input->text
                    );
                }
            } else {
                (void)snprintf(
                    state->status_message,
                    sizeof(state->status_message),
                    "%s",
                    message
                );
            }
        } else {
            if (reset_project_session(state, runtime)
                && lardon3d_project_create(state, input->text)) {
                (void)reload_catalog(state, false);
            }
        }
        if (input->mode != INPUT_NONE) {
            input->mode = INPUT_NONE;
        }
        return true;
    }

    if (key == KEY_BACKSPACE || key == 127 || key == '\b') {
        if (input->length > 0) {
            --input->length;
            input->text[input->length] = '\0';
        }
        return true;
    }

    if (key >= 0 && key <= UCHAR_MAX && isprint((unsigned char)key)) {
        size_t capacity = input->mode == INPUT_IMAGE_FILTER
            ? LARDON3D_IMAGE_FILTER_CAPACITY
            : sizeof(input->text);
        if (input->length + 1 >= capacity) {
            if (input->mode == INPUT_IMAGE_FILTER) {
                (void)snprintf(
                    state->status_message,
                    sizeof(state->status_message),
                    "Erreur : filtre trop long."
                );
                return true;
            }
            if (is_optics_input(input->mode)) {
                (void)snprintf(
                    state->status_message,
                    sizeof(state->status_message),
                    "Erreur : saisie optique trop longue."
                );
                input->mode = INPUT_NONE;
                input->text[0] = '\0';
                input->length = 0;
                return true;
            }
            if (input->mode == INPUT_PROJECT_OPEN) {
                if (reset_project_session(state, runtime)
                    && lardon3d_project_open(state, input->text)) {
                    (void)reload_catalog(state, false);
                }
            } else if (input->mode == INPUT_IMPORT_DIRECTORY) {
                (void)snprintf(
                    state->status_message,
                    sizeof(state->status_message),
                    "Erreur : chemin source trop long."
                );
            } else {
                if (reset_project_session(state, runtime)
                    && lardon3d_project_create(state, input->text)) {
                    (void)reload_catalog(state, false);
                }
            }
            input->mode = INPUT_NONE;
            return true;
        }

        input->text[input->length] = (char)key;
        ++input->length;
        input->text[input->length] = '\0';
        return true;
    }

    return false;
}

static void
begin_input(TuiInput *input, InputMode mode)
{
    *input = (TuiInput) {
        .mode = mode,
        .text = "",
        .length = 0,
    };
}

static bool
handle_ssd_key(
    Lardon3DAppState *state,
    TuiRuntime *runtime
)
{
    /* F10 is the persistent SSD control key, not merely navigation. Move to
     * the state screen first, then act immediately only from the freshly
     * polled, validated snapshot; UNKNOWN remains a no-action observation. */
    state->screen = LARDON3D_SCREEN_SSD;
    if (!runtime->ssd_operation) {
        (void)snprintf(state->status_message,
            sizeof(state->status_message),
            "Contrôleur SSD indisponible; état UNKNOWN, aucune action.");
        return true;
    }
    (void)lardon3d_tui_ssd_async_poll(
        runtime->ssd_operation, &runtime->ssd_operation_snapshot);
    if (runtime->ssd_operation_snapshot.running) {
        (void)snprintf(state->status_message,
            sizeof(state->status_message),
            "Opération SSD %s déjà en cours.",
            lardon3d_tui_ssd_action_name(
                runtime->ssd_operation_snapshot.action));
        return true;
    }
    if (!runtime->ssd_operation_snapshot.controller_snapshot_known
        || !runtime->ssd_operation_snapshot.controller_snapshot_actionable) {
        (void)snprintf(state->status_message,
            sizeof(state->status_message),
            "Télémétrie SSD indéterminée/invalide; contrôle désactivé.");
        return true;
    }
    Lardon3DTuiSsdAction action;
    const Lardon3DSsdSnapshot *current =
        &runtime->ssd_operation_snapshot.controller_snapshot;
    if (!lardon3d_tui_ssd_action_for_snapshot(current, &action)) {
        (void)snprintf(state->status_message,
            sizeof(state->status_message),
            "Aucune transition SSD sûre depuis %s.",
            lardon3d_ssd_state_name(current->state));
        return true;
    }
    if (!lardon3d_tui_ssd_async_request(runtime->ssd_operation, action)) {
        (void)snprintf(state->status_message,
            sizeof(state->status_message),
            "Impossible de lancer l'opération SSD bornée.");
        return true;
    }
    (void)snprintf(state->status_message, sizeof(state->status_message),
        "SSD %s lancé hors du thread ncurses.",
        lardon3d_tui_ssd_action_name(action));
    return true;
}

static bool
handle_task_key(
    Lardon3DAppState *state,
    TuiRuntime *runtime,
    int key
)
{
    if (key == KEY_UP || key == 'k') {
        if (runtime->selected_task > 0) {
            --runtime->selected_task;
        }
        return true;
    }
    if (key == KEY_DOWN || key == 'j') {
        if (runtime->selected_task + 1 < runtime->observation.task_count) {
            ++runtime->selected_task;
        }
        return true;
    }
    if (runtime->selected_task >= runtime->observation.task_count) {
        return false;
    }
    uint64_t task_id = runtime->observation.tasks[
        runtime->selected_task].id;
    bool result = false;
    const char *operation = NULL;
    if (key == 'p' || key == 'P') {
        operation = "pause";
        result = lardon3d_task_queue_pause(state->task_queue, task_id);
    } else if (key == 'r' || key == 'R') {
        operation = "reprise";
        result = lardon3d_task_queue_resume(state->task_queue, task_id);
    } else if (key == 'c' || key == 'C') {
        operation = "annulation";
        result = lardon3d_task_queue_cancel(state->task_queue, task_id);
    }
    if (!operation) {
        return false;
    }
    (void)snprintf(state->status_message, sizeof(state->status_message),
        "Tâche #%llu: %s %s.", (unsigned long long)task_id,
        operation, result ? "demandée" : "refusée/indisponible");
    return true;
}

static bool
handle_optics_key(
    Lardon3DAppState *state,
    TuiInput *input,
    TuiRuntime *runtime,
    int key
)
{
    bool handled = true;
    if (key == '\t') {
        Lardon3DTuiOpticsPane next = (Lardon3DTuiOpticsPane)(
            ((int)runtime->optics_snapshot.active_pane + 1)
            % ((int)LARDON3D_TUI_OPTICS_PANE_CALIBRATION + 1));
        (void)lardon3d_tui_optics_select_pane(runtime->optics, next);
    } else if (key == KEY_UP || key == 'k') {
        (void)lardon3d_tui_optics_move_selection(runtime->optics, -1);
    } else if (key == KEY_DOWN || key == 'j') {
        (void)lardon3d_tui_optics_move_selection(runtime->optics, 1);
    } else if (key == '[' || key == ']') {
        (void)lardon3d_tui_optics_page(runtime->optics,
            runtime->optics_snapshot.active_pane, key == ']');
    } else if (key == 'B') {
        begin_input(input, INPUT_OPTICS_CREATE_BODY);
    } else if (key == 'L') {
        begin_input(input, INPUT_OPTICS_CREATE_LENS);
    } else if (key == 'C') {
        begin_input(input, INPUT_OPTICS_CREATE_CONFIGURATION);
    } else if (key == 'V') {
        begin_input(input, INPUT_OPTICS_INSPECT_CAPTURE);
    } else if (key == 'A') {
        begin_input(input, INPUT_OPTICS_ASSIGN_CAPTURE);
    } else if (key == 'G') {
        begin_input(input, INPUT_OPTICS_ASSIGN_GROUP);
    } else if (key == 'E') {
        begin_input(input, INPUT_OPTICS_LOOKUP_METADATA);
    } else if (key == 'K') {
        (void)lardon3d_tui_optics_select_capture_calibration(
            runtime->optics);
        copy_optics_message(state, runtime);
    } else if (key == 'R') {
        runtime->optics_retry_requested = true;
        sync_optics(state, runtime);
        copy_optics_message(state, runtime);
    } else {
        handled = false;
    }
    (void)lardon3d_tui_optics_snapshot(
        runtime->optics, &runtime->optics_snapshot);
    return handled;
}

static bool
handle_normal_input(
    Lardon3DAppState *state,
    TuiInput *input,
    Lardon3DImportTask *task,
    TuiRuntime *runtime,
    int key
)
{
    if (key == KEY_F(10)) {
        return handle_ssd_key(state, runtime);
    }
    if (task) {
        if (key == 'x' || key == 'X') {
            lardon3d_import_task_request_cancel(task);
        } else if (key == 'q' || key == 'Q') {
            (void)snprintf(
                state->status_message,
                sizeof(state->status_message),
                "Quitter est désactivé pendant l'import."
            );
        } else if (key == 27) {
            (void)snprintf(
                state->status_message,
                sizeof(state->status_message),
                "Accueil indisponible pendant l'import."
            );
        }
        return key == KEY_RESIZE || key == 'x' || key == 'X'
            || key == 'q' || key == 'Q' || key == 27;
    }
    if (key == 'q' || key == 'Q') {
        state->running = false;
        return false;
    }
    if (state->screen == LARDON3D_SCREEN_TASKS
        && handle_task_key(state, runtime, key)) {
        return true;
    }
    if (state->screen == LARDON3D_SCREEN_OPTICS
        && handle_optics_key(state, input, runtime, key)) {
        return true;
    }

    switch (key) {
    case KEY_F(1):
        state->screen = LARDON3D_SCREEN_HELP;
        return true;
    case KEY_F(2):
        state->screen = LARDON3D_SCREEN_PROJECTS;
        return true;
    case KEY_F(3):
        state->screen = LARDON3D_SCREEN_IMPORT;
        return true;
    case KEY_F(4):
        state->screen = LARDON3D_SCREEN_VIEWER;
        return true;
    case KEY_F(5):
        state->screen = LARDON3D_SCREEN_TASKS;
        return true;
    case KEY_F(6):
        state->screen = LARDON3D_SCREEN_RESOURCES;
        return true;
    case KEY_F(7):
        state->screen = LARDON3D_SCREEN_OPTICS;
        return true;
    case 27:
        state->screen = LARDON3D_SCREEN_HOME;
        return true;
    case KEY_RESIZE:
        normalize_view_navigation(state);
        return true;
    case KEY_UP:
    case 'k':
        if (state->screen == LARDON3D_SCREEN_IMPORT
            && lardon3d_image_view_selection(state->image_view) > 0) {
            lardon3d_image_view_select(
                state->image_view,
                lardon3d_image_view_selection(state->image_view) - 1,
                catalog_page_size()
            );
            return true;
        }
        return false;
    case KEY_DOWN:
    case 'j': {
        size_t count = lardon3d_image_view_count(state->image_view);
        size_t selection = lardon3d_image_view_selection(state->image_view);
        if (state->screen == LARDON3D_SCREEN_IMPORT
            && selection < count && selection + 1 < count) {
            lardon3d_image_view_select(
                state->image_view,
                selection + 1,
                catalog_page_size()
            );
            return true;
        }
        return false;
    }
    case KEY_PPAGE:
        if (state->screen == LARDON3D_SCREEN_IMPORT) {
            size_t page_size = catalog_page_size();
            size_t selection = lardon3d_image_view_selection(state->image_view);
            selection = selection > page_size
                ? selection - page_size
                : 0;
            lardon3d_image_view_select(state->image_view, selection, page_size);
            return true;
        }
        return false;
    case KEY_NPAGE:
        if (state->screen == LARDON3D_SCREEN_IMPORT) {
            size_t count = lardon3d_image_view_count(state->image_view);
            if (count > 0) {
                size_t selection = lardon3d_image_view_selection(
                    state->image_view
                );
                size_t remaining = count - 1 - selection;
                size_t page_size = catalog_page_size();
                selection += remaining < page_size
                    ? remaining
                    : page_size;
                lardon3d_image_view_select(
                    state->image_view,
                    selection,
                    page_size
                );
            }
            return true;
        }
        return false;
    case KEY_HOME:
        if (state->screen == LARDON3D_SCREEN_IMPORT) {
            lardon3d_image_view_select(
                state->image_view,
                0,
                catalog_page_size()
            );
            return true;
        }
        return false;
    case KEY_END:
        if (state->screen == LARDON3D_SCREEN_IMPORT) {
            size_t count = lardon3d_image_view_count(state->image_view);
            lardon3d_image_view_select(
                state->image_view,
                count > 0 ? count - 1 : 0,
                catalog_page_size()
            );
            return true;
        }
        return false;
    case 'n':
    case 'N':
        if (state->screen == LARDON3D_SCREEN_PROJECTS) {
            *input = (TuiInput) {
                .mode = INPUT_PROJECT_CREATE,
                .text = "",
                .length = 0,
            };
            return true;
        }
        return false;
    case 'o':
    case 'O':
        if (state->screen == LARDON3D_SCREEN_PROJECTS) {
            *input = (TuiInput) {
                .mode = INPUT_PROJECT_OPEN,
                .text = "",
                .length = 0,
            };
            return true;
        }
        return false;
    case 'i':
    case 'I':
        if (state->screen == LARDON3D_SCREEN_IMPORT) {
            if (!state->project_loaded) {
                (void)snprintf(
                    state->status_message,
                    sizeof(state->status_message),
                    "Aucun projet chargé."
                );
                return true;
            }
            *input = (TuiInput) {
                .mode = INPUT_IMPORT_DIRECTORY,
                .text = "",
                .length = 0,
            };
            return true;
        }
        return false;
    case 'r':
    case 'R':
        if (state->screen == LARDON3D_SCREEN_IMPORT) {
            if (!state->project_loaded) {
                (void)snprintf(
                    state->status_message,
                    sizeof(state->status_message),
                    "Aucun projet chargé."
                );
            } else {
                (void)reload_catalog(state, true);
            }
            return true;
        }
        return false;
    case 's':
    case 'S':
        if (state->screen == LARDON3D_SCREEN_IMPORT && state->image_view) {
            Lardon3DImageSort sort = lardon3d_image_view_sort(
                state->image_view
            );
            sort = (Lardon3DImageSort)(
                ((int)sort + 1) % ((int)LARDON3D_IMAGE_SORT_SIZE_DESC + 1)
            );
            if (lardon3d_image_view_set_sort(state->image_view, sort)) {
                normalize_view_navigation(state);
                (void)snprintf(
                    state->status_message,
                    sizeof(state->status_message),
                    "Tri : %s.",
                    lardon3d_image_view_sort_name(sort)
                );
            }
            return true;
        }
        return false;
    case '/':
        if (state->screen == LARDON3D_SCREEN_IMPORT && state->image_view) {
            const char *filter = lardon3d_image_view_filter(state->image_view);
            *input = (TuiInput) {
                .mode = INPUT_IMAGE_FILTER,
                .text = "",
                .length = strlen(filter),
            };
            (void)snprintf(input->text, sizeof(input->text), "%s", filter);
            return true;
        }
        return false;
    case 'x':
    case 'X':
        if (state->screen == LARDON3D_SCREEN_IMPORT && state->image_view) {
            char message[sizeof(state->status_message)];
            if (lardon3d_image_view_set_filter(
                    state->image_view,
                    "",
                    message,
                    sizeof(message)
                )) {
                normalize_view_navigation(state);
                (void)snprintf(
                    state->status_message,
                    sizeof(state->status_message),
                    "Filtre effacé."
                );
            }
            return true;
        }
        return false;
    case 'c':
    case 'C':
        if (state->screen == LARDON3D_SCREEN_PROJECTS) {
            (void)reset_project_session(state, runtime);
            return true;
        }
        return false;
    default:
        return false;
    }
}

bool
lardon3d_tui_init(void)
{
    if (!initscr()) {
        return false;
    }

    if (cbreak() == ERR || noecho() == ERR || keypad(stdscr, true) == ERR
        || curs_set(0) == ERR) {
        endwin();
        return false;
    }
    timeout(75);

    return true;
}

static void
initialize_palette(Lardon3DTuiPalette *palette)
{
    if (!palette || !has_colors() || start_color() == ERR) {
        lardon3d_tui_palette_plan(false, 0, palette);
        return;
    }
    short background = use_default_colors() == OK ? -1 : COLOR_BLACK;
    lardon3d_tui_palette_plan(true, COLOR_PAIRS, palette);
    static const struct {
        Lardon3DTuiSemantic semantic;
        short foreground;
    } colors[] = {
        {LARDON3D_TUI_SEMANTIC_HEALTHY, COLOR_GREEN},
        {LARDON3D_TUI_SEMANTIC_WARNING, COLOR_YELLOW},
        {LARDON3D_TUI_SEMANTIC_ERROR, COLOR_RED},
        {LARDON3D_TUI_SEMANTIC_GPU, COLOR_CYAN},
        {LARDON3D_TUI_SEMANTIC_CPU, COLOR_BLUE},
        {LARDON3D_TUI_SEMANTIC_SSD, COLOR_MAGENTA},
    };
    bool any_pair = false;
    for (size_t index = 0; index < sizeof(colors) / sizeof(colors[0]);
         ++index) {
        short pair = palette->color_pair[colors[index].semantic];
        if (pair > 0 && init_pair(pair, colors[index].foreground,
                background) == OK) {
            any_pair = true;
        } else {
            palette->color_pair[colors[index].semantic] = 0;
        }
    }
    palette->color_enabled = any_pair;
}

static void
destroy_runtime(TuiRuntime *runtime)
{
    if (!runtime) {
        return;
    }
    /* The DB borrow ends here. The SSD worker is separately borrowed from App
     * and deliberately survives this local runtime until Queue teardown. */
    lardon3d_tui_optics_unbind(runtime->optics);
    runtime->optics_database = NULL;
    lardon3d_tui_optics_destroy(runtime->optics);
    runtime->optics = NULL;
    /* The SSD operation is borrowed. App keeps its Governor registration alive
     * until after Queue destroy has joined every Task and released every
     * scratch lease; tearing it down in this local frame would invert that
     * ownership boundary. */
    runtime->ssd_operation = NULL;
    lardon3d_runtime_observer_destroy(runtime->observer);
    runtime->observer = NULL;
}

bool
lardon3d_tui_run_with_ssd_operation(
    Lardon3DAppState *state,
    Lardon3DTuiSsdAsync *ssd_operation
)
{
    if (!state) {
        return false;
    }

    TuiInput input = {0};
    Lardon3DImportTask *task = NULL;
    TuiRuntime runtime = {0};
    runtime.observer = lardon3d_runtime_observer_create(
        &state->hardware_profile, state->task_queue,
        state->resource_governor);
    runtime.optics = lardon3d_tui_optics_create();
    runtime.ssd_operation = ssd_operation;
    if (!runtime.observer || !runtime.optics) {
        destroy_runtime(&runtime);
        return false;
    }
    initialize_palette(&runtime.palette);
    if (!lardon3d_runtime_observer_refresh(
            runtime.observer, state->project_loaded, true,
            &runtime.observation)) {
        /* With no previous cache the observer returns an initialized UNKNOWN
         * copy; later failures retain and mark the last bounded copy stale. */
        if (!runtime.observation.status[0]) {
            (void)snprintf(runtime.observation.status,
                sizeof(runtime.observation.status),
                "Runtime observation unavailable; values remain UNKNOWN");
        }
        (void)snprintf(state->status_message,
            sizeof(state->status_message), "%s",
            runtime.observation.status);
    }
    redraw(state, &input, task, &runtime);

    bool success = true;

    while (state->running) {
        int key = getch();

        bool should_redraw = task != NULL
            || state->screen == LARDON3D_SCREEN_HOME
            || state->screen == LARDON3D_SCREEN_TASKS
            || state->screen == LARDON3D_SCREEN_RESOURCES
            || state->screen == LARDON3D_SCREEN_SSD
            || state->screen == LARDON3D_SCREEN_OPTICS
            || runtime.ssd_operation_snapshot.running;
        if (task && lardon3d_import_task_is_finished(task)) {
            Lardon3DImportTaskSnapshot snapshot;
            if (!lardon3d_import_task_join(task)
                || !lardon3d_import_task_snapshot(task, &snapshot)) {
                lardon3d_import_task_destroy(task);
                task = NULL;
                success = false;
                break;
            }
            (void)snprintf(
                state->status_message,
                sizeof(state->status_message),
                "%s",
                snapshot.message
            );
            if (snapshot.status == LARDON3D_IMPORT_TASK_SUCCEEDED) {
                (void)reload_catalog(state, false);
            }
            lardon3d_import_task_destroy(task);
            task = NULL;
            should_redraw = true;
        }

        if (key != ERR) {
            should_redraw = (input.mode != INPUT_NONE
                ? handle_active_input(state, &input, &task, &runtime, key)
                : handle_normal_input(
                    state, &input, task, &runtime, key))
                || should_redraw;
        }

        if (should_redraw) {
            redraw(state, &input, task, &runtime);
        }
    }

    if (task) {
        lardon3d_import_task_request_cancel(task);
        if (!lardon3d_import_task_join(task)) {
            lardon3d_import_task_destroy(task);
            task = NULL;
            success = false;
        } else {
            lardon3d_import_task_destroy(task);
            task = NULL;
        }
    }

    destroy_runtime(&runtime);
    return success && !runtime.fatal_error;
}

bool
lardon3d_tui_run_with_ssd(
    Lardon3DAppState *state,
    Lardon3DSsdController *ssd_controller
)
{
    Lardon3DTuiSsdAsync *operation = ssd_controller
        ? lardon3d_tui_ssd_async_create(ssd_controller) : NULL;
    if (ssd_controller && !operation) {
        return false;
    }
    bool success = lardon3d_tui_run_with_ssd_operation(state, operation);
    if (operation
        && !lardon3d_tui_ssd_async_destroy_checked(&operation)) {
        return false;
    }
    return success;
}

bool
lardon3d_tui_run(Lardon3DAppState *state)
{
    return lardon3d_tui_run_with_ssd_operation(state, NULL);
}

void
lardon3d_tui_shutdown(void)
{
    endwin();
}
