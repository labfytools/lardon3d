#include <ctype.h>
#include <limits.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdio.h>

#include <lardon3d/import_task.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/layout.h>
#include <lardon3d/project.h>
#include <lardon3d/tui.h>

enum {
    MINIMUM_ROWS = 20,
    MINIMUM_COLUMNS = 72,
};

typedef enum {
    INPUT_NONE = 0,
    INPUT_PROJECT_CREATE,
    INPUT_PROJECT_OPEN,
    INPUT_IMPORT_DIRECTORY,
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
    return rows > 16 ? (size_t)(rows - 16) : 1;
}

static void
normalize_catalog_navigation(Lardon3DAppState *state)
{
    size_t count = lardon3d_image_catalog_count(state->image_catalog);
    if (count == 0) {
        state->image_selection = 0;
        state->image_offset = 0;
        return;
    }
    if (state->image_selection >= count) {
        state->image_selection = count - 1;
    }
    if (state->image_offset > state->image_selection) {
        state->image_offset = state->image_selection;
    }
    size_t page_size = catalog_page_size();
    if (state->image_selection - state->image_offset >= page_size) {
        state->image_offset = state->image_selection - page_size + 1;
    }
}

static bool
reload_catalog(Lardon3DAppState *state, bool announce_success)
{
    lardon3d_image_catalog_destroy(state->image_catalog);
    state->image_catalog = NULL;
    char message[sizeof(state->status_message)];
    state->image_catalog = lardon3d_image_catalog_load(
        state,
        message,
        sizeof(message)
    );
    normalize_catalog_navigation(state);
    if (!state->image_catalog || message[0]) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "%s",
            message
        );
        return state->image_catalog != NULL;
    }
    if (announce_success) {
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "Catalogue rechargé : %zu image%s.",
            lardon3d_image_catalog_count(state->image_catalog),
            lardon3d_image_catalog_count(state->image_catalog) == 1 ? "" : "s"
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
    }
    Lardon3DImportTaskSnapshot snapshot;
    const Lardon3DImportTaskSnapshot *displayed_snapshot = NULL;
    if (task && lardon3d_import_task_snapshot(task, &snapshot)) {
        displayed_snapshot = &snapshot;
    }
    lardon3d_layout_draw(
        state,
        text,
        label,
        displayed_snapshot,
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
        if (input->length + 1 >= sizeof(input->text)) {
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
    case 27:
        state->screen = LARDON3D_SCREEN_HOME;
        return true;
    case KEY_RESIZE:
        normalize_catalog_navigation(state);
        return true;
    case KEY_UP:
    case 'k':
        if (state->screen == LARDON3D_SCREEN_IMPORT
            && state->image_selection > 0) {
            --state->image_selection;
            normalize_catalog_navigation(state);
            return true;
        }
        return false;
    case KEY_DOWN:
    case 'j': {
        size_t count = lardon3d_image_catalog_count(state->image_catalog);
        if (state->screen == LARDON3D_SCREEN_IMPORT
            && state->image_selection + 1 < count) {
            ++state->image_selection;
            normalize_catalog_navigation(state);
            return true;
        }
        return false;
    }
    case KEY_PPAGE:
        if (state->screen == LARDON3D_SCREEN_IMPORT) {
            size_t page_size = catalog_page_size();
            state->image_selection = state->image_selection > page_size
                ? state->image_selection - page_size
                : 0;
            normalize_catalog_navigation(state);
            return true;
        }
        return false;
    case KEY_NPAGE:
        if (state->screen == LARDON3D_SCREEN_IMPORT) {
            size_t count = lardon3d_image_catalog_count(state->image_catalog);
            if (count > 0) {
                size_t remaining = count - 1 - state->image_selection;
                size_t page_size = catalog_page_size();
                state->image_selection += remaining < page_size
                    ? remaining
                    : page_size;
                normalize_catalog_navigation(state);
            }
            return true;
        }
        return false;
    case KEY_HOME:
        if (state->screen == LARDON3D_SCREEN_IMPORT) {
            state->image_selection = 0;
            normalize_catalog_navigation(state);
            return true;
        }
        return false;
    case KEY_END:
        if (state->screen == LARDON3D_SCREEN_IMPORT) {
            size_t count = lardon3d_image_catalog_count(state->image_catalog);
            state->image_selection = count > 0 ? count - 1 : 0;
            normalize_catalog_navigation(state);
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

        bool should_redraw = task != NULL;
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
