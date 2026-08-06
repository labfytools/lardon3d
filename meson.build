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
        'src/project.c',
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
