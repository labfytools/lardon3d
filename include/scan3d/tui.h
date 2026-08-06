#ifndef SCAN3D_TUI_H
#define SCAN3D_TUI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool scan3d_tui_init(void);
bool scan3d_tui_run(void);
void scan3d_tui_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SCAN3D_TUI_H */
