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

executable(
    'lardon3d',
    sources: [
        'src/main.c',
        'src/app.c',
        'src/app_state.c',
        'src/tui.c',
        'src/layout.c',
        'src/import.c',
        'src/project.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [ncursesw],
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
