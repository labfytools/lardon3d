#include <ncurses.h>
#include <stdbool.h>

#include <lardon3d/layout.h>
#include <lardon3d/tui.h>

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
lardon3d_tui_run(void)
{
    if (!lardon3d_layout_draw()) {
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
        if (key == KEY_RESIZE && !lardon3d_layout_draw()) {
            return false;
        }
    }
}

void
lardon3d_tui_shutdown(void)
{
    endwin();
}
