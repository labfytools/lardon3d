project(
    'scan3d-vulkan',
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
    'scan3d-vulkan',
    sources: [
        'src/main.c',
        'src/app.c',
        'src/tui.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [ncursesw],
)
