project(
    'lardon3d',
    'c',
    version: '0.1.0',
    license: 'MIT',
    default_options: [
        'c_std=c17',
        'warning_level=3',
        'buildtype=debugoptimized',
    ],
)

add_project_arguments(
    '-D_POSIX_C_SOURCE=200809L',
    '-Wpedantic',
    '-Wconversion',
    '-Wshadow',
    language: 'c',
)

ncursesw = dependency('ncursesw', required: true)
threads = dependency('threads')

executable(
    'lardon3d',
    sources: [
        'src/main.c',
        'src/app.c',
        'src/app_state.c',
        'src/tui.c',
        'src/layout.c',
        'src/import.c',
        'src/import_task.c',
        'src/image_catalog.c',
        'src/image_view.c',
        'src/project.c',
        'src/task.c',
        'src/task_queue.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
        'src/hardware_profile.c',
        'src/resource_snapshot.c',
        'src/resource_governor.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [ncursesw, threads],
)

import_test = executable(
    'test-import',
    sources: [
        'tests/test_import.c',
        'src/app_state.c',
        'src/import.c',
    ],
    include_directories: include_directories('include'),
)

test('import', import_test)

import_task_test = executable(
    'test-import-task',
    sources: [
        'tests/test_import_task.c',
        'src/app_state.c',
        'src/import.c',
        'src/import_task.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads],
)

test('import-task', import_task_test, timeout: 30)

image_catalog_test = executable(
    'test-image-catalog',
    sources: [
        'tests/test_image_catalog.c',
        'src/app_state.c',
        'src/image_catalog.c',
    ],
    include_directories: include_directories('include'),
)

test('image-catalog', image_catalog_test, timeout: 30)

image_view_test = executable(
    'test-image-view',
    sources: [
        'tests/test_image_view.c',
        'src/app_state.c',
        'src/image_catalog.c',
        'src/image_view.c',
    ],
    include_directories: include_directories('include'),
)

test('image-view', image_view_test, timeout: 30)

task_test = executable(
    'test-task',
    sources: [
        'tests/test_task.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads],
)

test('task', task_test, timeout: 30)

task_queue_test = executable(
    'test-task-queue',
    sources: [
        'tests/test_task_queue.c',
        'src/task.c',
        'src/task_queue.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads],
)

test('task-queue', task_queue_test, timeout: 30)

hardware_profile_test = executable(
    'test-hardware-profile',
    sources: [
        'tests/test_hardware_profile.c',
        'src/hardware_profile.c',
    ],
    include_directories: include_directories('include'),
)

test('hardware-profile', hardware_profile_test, timeout: 30)

resource_snapshot_test = executable(
    'test-resource-snapshot',
    sources: [
        'tests/test_resource_snapshot.c',
        'src/hardware_profile.c',
        'src/resource_snapshot.c',
    ],
    include_directories: include_directories('include'),
)

test('resource-snapshot', resource_snapshot_test, timeout: 30)

resource_governor_test = executable(
    'test-resource-governor',
    sources: [
        'tests/test_resource_governor.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads],
)

test('resource-governor', resource_governor_test, timeout: 30)

resource_reservation_test = executable(
    'test-resource-reservation',
    sources: [
        'tests/test_resource_reservation.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads],
)

test('resource-reservation', resource_reservation_test, timeout: 60)
