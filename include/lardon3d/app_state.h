#ifndef LARDON3D_APP_STATE_H
#define LARDON3D_APP_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <lardon3d/hardware_profile.h>
typedef struct Lardon3DImageCatalog Lardon3DImageCatalog;
typedef struct Lardon3DImageView Lardon3DImageView;
typedef struct Lardon3DTaskQueue Lardon3DTaskQueue;
typedef struct Lardon3DResourceGovernor Lardon3DResourceGovernor;
typedef struct Lardon3DProjectDb Lardon3DProjectDb;
typedef struct Lardon3DOrbVulkanBackend Lardon3DOrbVulkanBackend;

enum { LARDON3D_APP_STATE_PATH_CAPACITY = 4096 };

typedef enum {
    LARDON3D_SCREEN_HOME = 0,
    LARDON3D_SCREEN_PROJECTS,
    LARDON3D_SCREEN_IMPORT,
    LARDON3D_SCREEN_VIEWER,
    LARDON3D_SCREEN_HELP,
    LARDON3D_SCREEN_TASKS,
    LARDON3D_SCREEN_RESOURCES,
    LARDON3D_SCREEN_OPTICS,
    LARDON3D_SCREEN_SSD
} Lardon3DScreen;

typedef struct Lardon3DAppState {
    Lardon3DScreen screen;
    bool running;
    bool project_loaded;
    char project_name[128];
    char project_path[LARDON3D_APP_STATE_PATH_CAPACITY];
    char project_stable_id[65];
    char status_message[256];
    Lardon3DImageCatalog *image_catalog;
    Lardon3DImageView *image_view;
    Lardon3DTaskQueue *task_queue;
    Lardon3DHardwareProfile hardware_profile;
    Lardon3DResourceGovernor *resource_governor;
    Lardon3DProjectDb *project_db;
    Lardon3DOrbVulkanBackend *orb_vulkan_backend;
    size_t recovery_inspected;
    size_t recovery_resumed;
    size_t recovery_skipped;
    size_t recovery_failed;
    size_t recovery_published_not_durable;
    bool recovery_queue_full;
} Lardon3DAppState;

void lardon3d_app_state_init(Lardon3DAppState *state);

#endif
