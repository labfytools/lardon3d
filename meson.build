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
    ],
    include_directories: include_directories('include'),
    dependencies: [ncursesw],
)
