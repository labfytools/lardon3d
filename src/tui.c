#include <ctype.h>
#include <limits.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stdio.h>

#include <lardon3d/layout.h>
#include <lardon3d/project.h>
#include <lardon3d/tui.h>

enum {
    PROJECT_INPUT_CAPACITY = 256,
    MINIMUM_ROWS = 20,
    MINIMUM_COLUMNS = 72,
};

typedef struct {
    bool active;
    char text[PROJECT_INPUT_CAPACITY];
    size_t length;
} ProjectInput;

static void
redraw(const Lardon3DAppState *state, const ProjectInput *input)
{
    int rows;
    int columns;
    getmaxyx(stdscr, rows, columns);

    const char *text = input->active ? input->text : NULL;
    lardon3d_layout_draw(state, text, rows, columns);
    (void)curs_set(
        input->active && rows >= MINIMUM_ROWS && columns >= MINIMUM_COLUMNS
            ? 1
            : 0
    );
}

static bool
handle_project_input(
    Lardon3DAppState *state,
    ProjectInput *input,
    int key
)
{
    if (key == KEY_RESIZE) {
        return true;
    }

    if (key == 27) {
        input->active = false;
        (void)snprintf(
            state->status_message,
            sizeof(state->status_message),
            "Création du projet annulée."
        );
        return true;
    }

    if (key == '\n' || key == '\r' || key == KEY_ENTER) {
        (void)lardon3d_project_set_name(state, input->text);
        input->active = false;
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
            (void)lardon3d_project_set_name(state, input->text);
            input->active = false;
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
    ProjectInput *input,
    int key
)
{
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
        return true;
    case 'n':
    case 'N':
        if (state->screen == LARDON3D_SCREEN_PROJECTS) {
            *input = (ProjectInput) {
                .active = true,
                .text = "",
                .length = 0,
            };
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

    return true;
}

bool
lardon3d_tui_run(Lardon3DAppState *state)
{
    if (!state) {
        return false;
    }

    ProjectInput input = {0};
    redraw(state, &input);

    while (state->running) {
        int key = getch();
        if (key == ERR) {
            return false;
        }

        bool should_redraw = input.active
            ? handle_project_input(state, &input, key)
            : handle_normal_input(state, &input, key);

        if (should_redraw) {
            redraw(state, &input);
        }
    }

    return true;
}

void
lardon3d_tui_shutdown(void)
{
    endwin();
}
