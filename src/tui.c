#include <ncurses.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <scan3d/tui.h>

static void
draw_centered(int row, int rows, int columns, const char *text)
{
    if (row < 0 || row >= rows || columns <= 2) {
        return;
    }

    int length = (int)strlen(text);
    int available = columns - 2;
    int displayed = length < available ? length : available;
    int column = (columns - displayed) / 2;

    (void)mvaddnstr(row, column, text, displayed);
}

static bool
draw_interface(void)
{
    int rows;
    int columns;
    getmaxyx(stdscr, rows, columns);

    if (erase() == ERR) {
        return false;
    }

    if (rows >= 2 && columns >= 2) {
        (void)box(stdscr, 0, 0);
    }

    draw_centered(1, rows, columns, "Scan3D Vulkan");
    draw_centered(3, rows, columns, "Projet de reconstruction 3D");

    char dimensions[64];
    (void)snprintf(
        dimensions,
        sizeof(dimensions),
        "Terminal : %d x %d",
        columns,
        rows
    );
    draw_centered(5, rows, columns, dimensions);
    draw_centered(rows - 2, rows, columns, "q : quitter");

    return refresh() != ERR;
}

bool
scan3d_tui_init(void)
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
scan3d_tui_run(void)
{
    if (!draw_interface()) {
        return false;
    }

    for (;;) {
        int key = getch();
        if (key == 'q' || key == 'Q') {
            return true;
        }
        if (key == ERR) {
            return false;
        }
        if (key == KEY_RESIZE && !draw_interface()) {
            return false;
        }
    }
}

void
scan3d_tui_shutdown(void)
{
    endwin();
}
