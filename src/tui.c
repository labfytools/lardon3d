#include <ctype.h>
#include <limits.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <lardon3d/import_task.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/image_view.h>
#include <lardon3d/layout.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>
#include <lardon3d/tui.h>

enum {
    MINIMUM_ROWS = 20,
    MINIMUM_COLUMNS = 72,
    DISPLAYED_TASK_CAPACITY = 64,
};

typedef enum {
    INPUT_NONE = 0,
    INPUT_PROJECT_CREATE,
    INPUT_PROJECT_OPEN,
    INPUT_IMPORT_DIRECTORY,
    INPUT_IMAGE_FILTER,
} InputMode;

typedef struct {
    InputMode mode;
    char text[PATH_MAX];
    size_t length;
} TuiInput;

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
redraw(
    const Lardon3DAppState *state,
    const TuiInput *input,
    Lardon3DImportTask *task
)
{
    int rows;
    int columns;
    getmaxyx(stdscr, rows, columns);

    const char *text = input->mode != INPUT_NONE ? input->text : NULL;
    const char *label = "Nom du nouveau projet :";
    if (input->mode == INPUT_PROJECT_OPEN) {
        label = "Nom du dossier projet :";
    } else if (input->mode == INPUT_IMPORT_DIRECTORY) {
        label = "Dossier source :";
    } else if (input->mode == INPUT_IMAGE_FILTER) {
        label = "Filtre :";
    }
    Lardon3DImportTaskSnapshot snapshot;
    const Lardon3DImportTaskSnapshot *displayed_snapshot = NULL;
    if (task && lardon3d_import_task_snapshot(task, &snapshot)) {
        displayed_snapshot = &snapshot;
    }
    Lardon3DTaskSnapshot task_snapshots[DISPLAYED_TASK_CAPACITY];
    Lardon3DTaskQueueSummary task_summary;
    size_t task_count = lardon3d_task_queue_snapshot(
        state->task_queue,
        task_snapshots,
        DISPLAYED_TASK_CAPACITY,
        &task_summary
    );
    lardon3d_layout_draw(
        state,
        text,
        label,
        displayed_snapshot,
        task_snapshots,
        task_count,
        &task_summary,
        rows,
        columns
    );
    (void)curs_set(
        input->mode != INPUT_NONE
            && rows >= MINIMUM_ROWS
            && columns >= MINIMUM_COLUMNS
            ? 1
            : 0
    );
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

static bool
handle_active_input(
    Lardon3DAppState *state,
    TuiInput *input,
    Lardon3DImportTask **task,
    int key
)
{
    if (key == KEY_RESIZE) {
        return true;
    }

    if (key == 27) {
        const char *message = "Création du projet annulée.";
        if (input->mode == INPUT_PROJECT_OPEN) {
            message = "Ouverture du projet annulée.";
        } else if (input->mode == INPUT_IMPORT_DIRECTORY) {
            message = "Import annulé.";
        } else if (input->mode == INPUT_IMAGE_FILTER) {
            message = "Filtre annulé.";
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
        if (input->mode == INPUT_PROJECT_OPEN) {
            if (lardon3d_project_open(state, input->text)) {
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
            if (lardon3d_project_create(state, input->text)) {
                (void)reload_catalog(state, false);
            }
        }
        input->mode = INPUT_NONE;
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
            if (input->mode == INPUT_PROJECT_OPEN) {
                if (lardon3d_project_open(state, input->text)) {
                    (void)reload_catalog(state, false);
                }
            } else if (input->mode == INPUT_IMPORT_DIRECTORY) {
                (void)snprintf(
                    state->status_message,
                    sizeof(state->status_message),
                    "Erreur : chemin source trop long."
                );
            } else {
                if (lardon3d_project_create(state, input->text)) {
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

static bool
handle_normal_input(
    Lardon3DAppState *state,
    TuiInput *input,
    Lardon3DImportTask *task,
    int key
)
{
    if (task) {
        if (key == 'c' || key == 'C') {
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
        return key == KEY_RESIZE || key == 'c' || key == 'C'
            || key == 'q' || key == 'Q' || key == 27;
    }
    if (key == 'q' || key == 'Q') {
        state->running = false;
        return false;
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
            lardon3d_project_close(state);
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

bool
lardon3d_tui_run(Lardon3DAppState *state)
{
    if (!state) {
        return false;
    }

    TuiInput input = {0};
    Lardon3DImportTask *task = NULL;
    redraw(state, &input, task);

    while (state->running) {
        int key = getch();

        bool should_redraw = task != NULL
            || state->screen == LARDON3D_SCREEN_TASKS;
        if (task && lardon3d_import_task_is_finished(task)) {
            Lardon3DImportTaskSnapshot snapshot;
            if (!lardon3d_import_task_join(task)
                || !lardon3d_import_task_snapshot(task, &snapshot)) {
                lardon3d_import_task_destroy(task);
                return false;
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
                ? handle_active_input(state, &input, &task, key)
                : handle_normal_input(state, &input, task, key))
                || should_redraw;
        }

        if (should_redraw) {
            redraw(state, &input, task);
        }
    }

    if (task) {
        lardon3d_import_task_request_cancel(task);
        if (!lardon3d_import_task_join(task)) {
            lardon3d_import_task_destroy(task);
            return false;
        }
        lardon3d_import_task_destroy(task);
    }

    return true;
}

void
lardon3d_tui_shutdown(void)
{
    endwin();
}
