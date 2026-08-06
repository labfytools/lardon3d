#include <ncurses.h>
#include <stdbool.h>

#include <lardon3d/layout.h>
#include <lardon3d/tui.h>

static void
redraw(Lardon3DScreen screen)
{
    int rows;
    int columns;
    getmaxyx(stdscr, rows, columns);
    lardon3d_layout_draw(screen, rows, columns);
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
lardon3d_tui_run(void)
{
    Lardon3DScreen screen = LARDON3D_SCREEN_HOME;
    redraw(screen);

    for (;;) {
        int key = getch();

        if (key == 'q' || key == 'Q') {
            return true;
        }
        if (key == ERR) {
            return false;
        }

        bool should_redraw = true;
        switch (key) {
        case KEY_F(1):
            screen = LARDON3D_SCREEN_HELP;
            break;
        case KEY_F(2):
            screen = LARDON3D_SCREEN_PROJECTS;
            break;
        case KEY_F(3):
            screen = LARDON3D_SCREEN_IMPORT;
            break;
        case KEY_F(4):
            screen = LARDON3D_SCREEN_VIEWER;
            break;
        case 27:
            screen = LARDON3D_SCREEN_HOME;
            break;
        case KEY_RESIZE:
            break;
        default:
            should_redraw = false;
            break;
        }

        if (should_redraw) {
            redraw(screen);
        }
    }
}

void
lardon3d_tui_shutdown(void)
{
    endwin();
}
