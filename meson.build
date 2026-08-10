project(
    'lardon3d',
    'c', 'cpp',
    version: '0.1.0',
    license: 'MIT',
    default_options: [
        'c_std=c17',
        'cpp_std=c++17',
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
add_project_arguments('-Wpedantic', '-Wconversion', '-Wshadow', language: 'cpp')

ncursesw = dependency('ncursesw', required: true)
threads = dependency('threads')
sqlite3 = dependency('sqlite3', required: true)
openssl = dependency('openssl', required: true)
opencv = dependency(
    'opencv5',
    required: true,
    include_type: 'system',
    modules: ['opencv_core', 'opencv_imgcodecs', 'opencv_features2d', 'opencv_geometry'],
)
opencv_benchmark = dependency(
    'opencv5',
    required: true,
    include_type: 'system',
    modules: ['opencv_core', 'opencv_imgcodecs', 'opencv_features2d', 'opencv_imgproc'],
)

opencv_geometry = dependency(
    'opencv5',
    version: '>=5.0.0',
    modules: ['opencv_core', 'opencv_geometry'],
)

vulkan_orb = get_option('vulkan_orb')
vulkan = dependency('vulkan', required: vulkan_orb)
glslc = find_program('glslc', required: vulkan_orb)
python = find_program('python3', required: true)
matcher_vulkan_enabled = vulkan_orb.allowed() and vulkan.found() and glslc.found()
matcher_vulkan_configuration = configuration_data()
matcher_vulkan_configuration.set10('LARDON3D_HAVE_VULKAN', matcher_vulkan_enabled)
matcher_vulkan_config = configure_file(
    output: 'matcher_vulkan_config.h',
    configuration: matcher_vulkan_configuration,
)
matcher_backend_sources = [
    'src/orb_vulkan_backend.cpp',
    matcher_vulkan_config,
]
matcher_backend_dependencies = []
if matcher_vulkan_enabled
    orb_top2_spv = custom_target(
        'orb-top2-spv',
        input: 'shaders/orb_top2.comp',
        output: 'orb_top2.spv',
        command: [glslc, '-fshader-stage=compute', '@INPUT@', '-o', '@OUTPUT@'],
    )
    orb_top2_header = custom_target(
        'orb-top2-header',
        input: orb_top2_spv,
        output: 'orb_top2_spv.h',
        command: [python, files('tools/embed_spirv.py'), '@INPUT@', '@OUTPUT@'],
    )
    sift_top2_spv = custom_target(
        'sift-top2-spv',
        input: 'shaders/sift_top2.comp',
        output: 'sift_top2.spv',
        command: [glslc, '-fshader-stage=compute', '@INPUT@', '-o', '@OUTPUT@'],
    )
    sift_top2_header = custom_target(
        'sift-top2-header',
        input: sift_top2_spv,
        output: 'sift_top2_spv.h',
        command: [python, files('tools/embed_spirv.py'), '@INPUT@', '@OUTPUT@'],
    )
    sift_top2_fp64_spv = custom_target(
        'sift-top2-fp64-spv',
        input: 'shaders/sift_top2.comp',
        output: 'sift_top2_fp64.spv',
        command: [
            glslc,
            '-DLARDON3D_SIFT_FP64=1',
            '-fshader-stage=compute',
            '@INPUT@',
            '-o',
            '@OUTPUT@',
        ],
    )
    sift_top2_fp64_header = custom_target(
        'sift-top2-fp64-header',
        input: sift_top2_fp64_spv,
        output: 'sift_top2_fp64_spv.h',
        command: [python, files('tools/embed_spirv.py'), '@INPUT@', '@OUTPUT@'],
    )
    matcher_backend_sources += [orb_top2_header]
    sift_feasibility_sources = matcher_backend_sources + [
        sift_top2_header,
        sift_top2_fp64_header,
    ]
    matcher_backend_dependencies += [vulkan]
endif

opencv_test_environment = environment()
if get_option('b_sanitize').contains('thread')
    opencv_test_environment.set(
        'TSAN_OPTIONS',
        'suppressions=' + meson.project_source_root() + '/tests/tsan-opencv.supp',
    )
endif

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
        'src/image_catalog_persistent.c',
        'src/feature_extractor_opencv.cpp',
        'src/feature_store.c',
        'src/feature_extractor_opencv.cpp',
        'src/feature_task.c', 'src/sift_task.c', 'src/precision_features.c',
        'src/visual_index.c',
        'src/visual_index_task.c',
        'src/candidate_pair_gen.c',
        'src/candidate_pair_task.c',
        'src/matcher_task.c',
        'src/geometric_verifier_task.c',
        'src/image_view.c',
        'src/project.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/sparse_sfm_geometry.cpp',
        'src/task.c',
        'src/task_checkpoint.c',
        'src/task_kind_registry.c',
        'src/task_kinds.c',
        'src/task_queue.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
        'src/hardware_profile.c',
        'src/match_file.c',
        'src/matcher.cpp',
        'src/geometric_verifier.cpp',
        'src/track_builder.cpp',
        'src/track_builder_project.cpp',
        'src/track_builder_task.cpp',
    ] + matcher_backend_sources,
    include_directories: include_directories('include'),
    c_args: ['-DLARDON3D_TRACK_BUILDER_TASK_AVAILABLE'],
    dependencies: [ncursesw, threads, sqlite3, openssl, opencv, opencv_geometry]
        + matcher_backend_dependencies,
)

import_test = executable(
    'test-import',
    sources: [
        'tests/test_import.c',
        'src/app_state.c',
        'src/import.c',
        'src/image_catalog_persistent.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl],
)

test('import', import_test)

import_task_test = executable(
    'test-import-task',
    sources: [
        'tests/test_import_task.c',
        'src/app_state.c',
        'src/import.c',
        'src/import_task.c',
        'src/project.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/task_checkpoint.c',
        'src/task_kind_registry.c',
        'src/task_kinds.c',
        'src/matcher_task.c', 'src/matcher.cpp', 'src/match_file.c',
        'src/geometric_verifier_task.c', 'src/geometric_verifier.cpp',
        'src/feature_task.c', 'src/sift_task.c', 'src/precision_features.c',
        'src/feature_store.c',
        'src/visual_index.c',
        'src/visual_index_task.c',
        'src/candidate_pair_gen.c', 'src/candidate_pair_task.c',
        'src/feature_extractor_opencv.cpp',
        'src/task_queue.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
        'src/image_catalog.c',
        'src/image_catalog_persistent.c',
        'src/image_view.c',
    ] + matcher_backend_sources,
    c_args: [
        '-DLARDON3D_IMPORT_TASK_TESTING',
        '-DLARDON3D_PROJECT_DB_TESTING',
        '-DLARDON3D_CHECKPOINT_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv] + matcher_backend_dependencies,
)

test('import-task', import_task_test, timeout: 30)

image_catalog_test = executable(
    'test-image-catalog',
    sources: [
        'tests/test_image_catalog.c',
        'src/app_state.c',
        'src/image_catalog.c',
        'src/image_catalog_persistent.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl],
)

test('image-catalog', image_catalog_test, timeout: 30)

persistent_image_catalog_test = executable(
    'test-persistent-image-catalog',
    sources: [
        'tests/test_persistent_image_catalog.c',
        'src/app_state.c',
        'src/image_catalog_persistent.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    c_args: ['-DLARDON3D_PROJECT_DB_TESTING'],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl],
)

test('persistent-image-catalog', persistent_image_catalog_test, timeout: 60)

feature_store_test = executable(
    'test-feature-store',
    sources: [
        'tests/test_feature_store.c',
        'src/app_state.c',
        'src/image_catalog_persistent.c',
        'src/feature_extractor_opencv.cpp',
        'src/feature_store.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    c_args: ['-DLARDON3D_PROJECT_DB_TESTING', '-DLARDON3D_FEATURE_STORE_TESTING'],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv],
)

test('feature-store', feature_store_test, timeout: 60)

feature_file_robustness_test = executable(
    'test-feature-file-robustness',
    sources: [
        'tests/test_feature_file_robustness.c',
        'src/app_state.c',
        'src/image_catalog_persistent.c',
        'src/feature_extractor_opencv.cpp',
        'src/feature_store.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    c_args: ['-DLARDON3D_PROJECT_DB_TESTING', '-DLARDON3D_FEATURE_STORE_TESTING'],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv],
)

test('feature-file-robustness', feature_file_robustness_test, timeout: 60)

visual_index_test = executable(
    'test-visual-index',
    sources: [
        'tests/test_visual_index.c', 'src/app_state.c',
        'src/image_catalog_persistent.c', 'src/feature_store.c',
        'src/feature_extractor_opencv.cpp',
        'src/visual_index.c', 'src/project_db.c', 'src/project_db_sparse_sfm.c', 'src/task.c',
        'src/resource_governor.c', 'src/resource_snapshot.c',
    ],
    c_args: [
        '-DLARDON3D_PROJECT_DB_TESTING', '-DLARDON3D_VISUAL_INDEX_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv],
)
test('visual-index', visual_index_test, timeout: 60)

candidate_pair_gen_test = executable(
    'test-candidate-pair-gen',
    sources: [
        'tests/test_candidate_pair_gen.c', 'src/app_state.c',
        'src/image_catalog_persistent.c', 'src/feature_store.c',
        'src/feature_extractor_opencv.cpp',
        'src/visual_index.c', 'src/candidate_pair_gen.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c', 'src/task.c',
        'src/resource_governor.c', 'src/resource_snapshot.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv],
)
test('candidate-pair-gen', candidate_pair_gen_test, timeout: 60)

candidate_pair_task_test = executable(
    'test-candidate-pair-task',
    sources: [
        'tests/test_candidate_pair_task.c', 'src/app_state.c',
        'src/project.c', 'src/project_db.c', 'src/project_db_sparse_sfm.c', 'src/task.c',
        'src/task_checkpoint.c', 'src/task_kind_registry.c',
        'src/task_kinds.c', 'src/matcher_task.c', 'src/matcher.cpp',
        'src/geometric_verifier_task.c', 'src/geometric_verifier.cpp',
        'src/match_file.c', 'src/task_queue.c', 'src/import.c',
        'src/import_task.c', 'src/image_catalog.c',
        'src/image_catalog_persistent.c', 'src/image_view.c',
        'src/feature_task.c', 'src/sift_task.c',
        'src/precision_features.c', 'src/feature_store.c',
        'src/visual_index.c', 'src/visual_index_task.c',
        'src/candidate_pair_gen.c', 'src/candidate_pair_task.c',
        'src/feature_extractor_opencv.cpp',
        'src/resource_governor.c', 'src/resource_snapshot.c',
    ] + matcher_backend_sources,
    c_args: [
        '-DLARDON3D_PROJECT_DB_TESTING',
        '-DLARDON3D_CANDIDATE_PAIR_TASK_TESTING',
        '-DLARDON3D_VISUAL_INDEX_TASK_TESTING',
        '-DLARDON3D_FEATURE_TASK_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv] + matcher_backend_dependencies,
)
test('candidate-pair-task', candidate_pair_task_test, timeout: 60,
     env: opencv_test_environment)

matcher_task_test = executable(
    'test-matcher-task',
    sources: [
        'tests/test_matcher_task.c', 'src/app_state.c', 'src/project.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c', 'src/task.c', 'src/task_checkpoint.c',
        'src/task_kind_registry.c', 'src/task_kinds.c', 'src/task_queue.c',
        'src/matcher_task.c', 'src/matcher.cpp', 'src/match_file.c',
        'src/geometric_verifier_task.c', 'src/geometric_verifier.cpp',
        'src/feature_store.c', 'src/resource_governor.c',
        'src/resource_snapshot.c', 'src/hardware_profile.c',
        'src/import.c', 'src/import_task.c', 'src/image_catalog.c',
        'src/image_catalog_persistent.c', 'src/image_view.c',
        'src/feature_task.c', 'src/sift_task.c', 'src/precision_features.c',
        'src/feature_extractor_opencv.cpp', 'src/visual_index.c',
        'src/visual_index_task.c', 'src/candidate_pair_gen.c',
        'src/candidate_pair_task.c',
    ] + matcher_backend_sources,
    c_args: [
        '-DLARDON3D_PROJECT_DB_TESTING',
        '-DLARDON3D_MATCHER_TASK_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv] + matcher_backend_dependencies,
)
test('matcher-task', matcher_task_test, timeout: 60, env: opencv_test_environment)

feature_task_test = executable(
    'test-feature-task',
    sources: [
        'tests/test_feature_task.c', 'src/app_state.c', 'src/project.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c', 'src/task.c', 'src/task_checkpoint.c',
        'src/task_kind_registry.c', 'src/task_kinds.c', 'src/task_queue.c',
        'src/matcher_task.c', 'src/matcher.cpp', 'src/match_file.c',
        'src/geometric_verifier_task.c', 'src/geometric_verifier.cpp',
        'src/import.c', 'src/import_task.c', 'src/image_catalog.c',
        'src/image_catalog_persistent.c', 'src/image_view.c',
        'src/feature_task.c', 'src/sift_task.c', 'src/precision_features.c',
        'src/feature_store.c', 'src/visual_index.c',
        'src/visual_index_task.c',
        'src/candidate_pair_gen.c', 'src/candidate_pair_task.c',
        'src/feature_extractor_opencv.cpp', 'src/resource_governor.c',
        'src/resource_snapshot.c',
    ] + matcher_backend_sources,
    c_args: [
        '-DLARDON3D_PROJECT_DB_TESTING', '-DLARDON3D_FEATURE_TASK_TESTING',
        '-DLARDON3D_VISUAL_INDEX_TASK_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv] + matcher_backend_dependencies,
)
test('feature-task', feature_task_test, timeout: 60, env: opencv_test_environment)

precision_features_test = executable(
    'test-precision-features',
    sources: ['tests/test_precision_features.cpp', 'src/feature_extractor_opencv.cpp'],
    include_directories: include_directories('include'),
    dependencies: [openssl, opencv_benchmark],
)
test('precision-features', precision_features_test, timeout: 60, env: opencv_test_environment)

precision_consolidation_test = executable(
    'test-precision-consolidation',
    sources: [
        'tests/test_precision_consolidation.c', 'src/app_state.c',
        'src/project.c', 'src/project_db.c', 'src/project_db_sparse_sfm.c', 'src/task.c',
        'src/task_checkpoint.c', 'src/task_kind_registry.c',
        'src/task_kinds.c', 'src/matcher_task.c', 'src/matcher.cpp',
        'src/geometric_verifier_task.c', 'src/geometric_verifier.cpp',
        'src/match_file.c', 'src/task_queue.c', 'src/import.c',
        'src/import_task.c', 'src/image_catalog.c',
        'src/image_catalog_persistent.c', 'src/image_view.c',
        'src/feature_task.c', 'src/sift_task.c',
        'src/precision_features.c', 'src/feature_store.c',
        'src/visual_index.c', 'src/visual_index_task.c',
        'src/candidate_pair_gen.c', 'src/candidate_pair_task.c',
        'src/feature_extractor_opencv.cpp',
        'src/resource_governor.c', 'src/resource_snapshot.c',
    ] + matcher_backend_sources,
    c_args: [
        '-DLARDON3D_PROJECT_DB_TESTING',
        '-DLARDON3D_FEATURE_TASK_TESTING',
        '-DLARDON3D_VISUAL_INDEX_TASK_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv] + matcher_backend_dependencies,
)
test('precision-consolidation', precision_consolidation_test,
     timeout: 120, env: opencv_test_environment)

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

task_checkpoint_test = executable(
    'test-task-checkpoint',
    sources: [
        'tests/test_task_checkpoint.c',
        'src/task.c',
        'src/task_checkpoint.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    c_args: ['-DLARDON3D_CHECKPOINT_TESTING'],
    include_directories: include_directories('include'),
    dependencies: [threads],
)

test('task-checkpoint', task_checkpoint_test, timeout: 30)

project_db_test = executable(
    'test-project-db',
    sources: [
        'tests/test_project_db.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    c_args: ['-DLARDON3D_PROJECT_DB_TESTING'],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl],
)

test('project-db', project_db_test, timeout: 30)

sparse_sfm_model_test = executable(
    'test-sparse-sfm-model',
    sources: [
        'tests/test_sparse_sfm_model.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl],
)

test('sparse-sfm-model', sparse_sfm_model_test, timeout: 30)

sparse_sfm_geometry_test = executable(
    'test-sparse-sfm-geometry',
    sources: ['tests/test_sparse_sfm_geometry.cpp', 'src/sparse_sfm_geometry.cpp'],
    include_directories: include_directories('include'),
    dependencies: [opencv_geometry],
)

test('sparse-sfm-geometry', sparse_sfm_geometry_test, timeout: 60)

sparse_sfm_resource_test = executable(
    'test-sparse-sfm-resource',
    sources: [
        'tests/test_sparse_sfm_resource.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl],
)

match_result_test = executable(
    'test-match-result',
    sources: [
        'tests/test_match_result.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    c_args: ['-DLARDON3D_PROJECT_DB_TESTING'],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl],
)

test('match-result', match_result_test, timeout: 30)

geometric_verification_test = executable(
    'test-geometric-verification',
    sources: [
        'tests/test_geometric_verification.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    c_args: ['-DLARDON3D_PROJECT_DB_TESTING'],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl],
)

test('geometric-verification', geometric_verification_test, timeout: 30)

project_test = executable(
    'test-project',
    sources: [
        'tests/test_project.c',
        'src/app_state.c',
        'src/project.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/task_checkpoint.c',
        'src/task_kind_registry.c',
        'src/task_kinds.c',
        'src/matcher_task.c', 'src/matcher.cpp', 'src/match_file.c',
        'src/geometric_verifier_task.c', 'src/geometric_verifier.cpp',
        'src/feature_task.c', 'src/sift_task.c', 'src/precision_features.c',
        'src/feature_store.c',
        'src/visual_index.c',
        'src/visual_index_task.c',
        'src/candidate_pair_gen.c', 'src/candidate_pair_task.c',
        'src/feature_extractor_opencv.cpp',
        'src/task_queue.c',
        'src/import.c',
        'src/import_task.c',
        'src/image_catalog.c',
        'src/image_catalog_persistent.c',
        'src/image_view.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ] + matcher_backend_sources,
    c_args: [
        '-DLARDON3D_PROJECT_DB_TESTING',
        '-DLARDON3D_CHECKPOINT_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv] + matcher_backend_dependencies,
)

test('project', project_test, timeout: 30)

task_kind_registry_test = executable(
    'test-task-kind-registry',
    sources: [
        'tests/test_task_kind_registry.c',
        'src/task_kind_registry.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads],
)

test('task-kind-registry', task_kind_registry_test, timeout: 30)

sequential_task_test = executable(
    'test-sequential-task',
    sources: [
        'tests/test_sequential_task.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads],
)

test('sequential-task', sequential_task_test, timeout: 30)

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

matcher_test = executable(
    'test-matcher',
    sources: [
        'tests/test_matcher.c',
        'src/match_file.c',
        'src/matcher.cpp',
        'src/feature_store.c',
        'src/feature_extractor_opencv.cpp',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
        'src/image_catalog_persistent.c',
        'src/app_state.c',
    ] + matcher_backend_sources,
    c_args: ['-DLARDON3D_PROJECT_DB_TESTING'],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv] + matcher_backend_dependencies,
)

test('matcher', matcher_test, timeout: 60)

if matcher_vulkan_enabled
    orb_vulkan_backend_test = executable(
        'test-orb-vulkan-backend',
        sources: [
            'tests/test_orb_vulkan_backend.cpp',
        ] + matcher_backend_sources,
        cpp_args: ['-DLARDON3D_ORB_VULKAN_TESTING'],
        include_directories: include_directories('include'),
        dependencies: [threads, opencv] + matcher_backend_dependencies,
    )

    test('orb-vulkan-backend', orb_vulkan_backend_test, timeout: 120,
         env: opencv_test_environment)

    executable(
        'test-sift-vulkan-feasibility',
        sources: [
            'tests/test_sift_vulkan_feasibility.cpp',
            'src/match_file.c',
        ] + sift_feasibility_sources,
        cpp_args: ['-DLARDON3D_SIFT_VULKAN_FEASIBILITY'],
        build_by_default: false,
        include_directories: include_directories('include'),
        dependencies: [threads, openssl, opencv_benchmark]
            + matcher_backend_dependencies,
    )

    executable(
        'benchmark-orb-vulkan',
        sources: [
            'tests/benchmark_orb_vulkan.cpp',
        ] + matcher_backend_sources,
        include_directories: include_directories('include'),
        dependencies: [opencv] + matcher_backend_dependencies,
    )

    executable(
        'benchmark-sift-vulkan',
        sources: [
            'tests/benchmark_sift_vulkan.cpp',
        ] + sift_feasibility_sources,
        cpp_args: ['-DLARDON3D_SIFT_VULKAN_FEASIBILITY'],
        build_by_default: false,
        include_directories: include_directories('include'),
        dependencies: [opencv] + matcher_backend_dependencies,
    )
endif

matcher_e2e_test = executable(
    'test-matcher-e2e',
    sources: [
        'tests/test_matcher_e2e.c',
        'src/match_file.c',
        'src/matcher.cpp',
        'src/feature_store.c',
        'src/feature_extractor_opencv.cpp',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
        'src/image_catalog_persistent.c',
        'src/app_state.c',
    ] + matcher_backend_sources,
    c_args: matcher_vulkan_enabled ? ['-DLARDON3D_MATCHER_E2E_VULKAN'] : [],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv] + matcher_backend_dependencies,
)

test('matcher-e2e', matcher_e2e_test, timeout: 60)

executable(
    'benchmark-matcher',
    sources: [
        'tests/benchmark_matcher.cpp',
        'src/match_file.c',
        'src/matcher.cpp',
        'src/feature_store.c',
        'src/feature_extractor_opencv.cpp',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
        'src/image_catalog_persistent.c',
        'src/app_state.c',
    ] + matcher_backend_sources,
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv] + matcher_backend_dependencies,
)

executable(
    'benchmark-geometric-verifier',
    sources: ['tests/benchmark_geometric_verifier.cpp'],
    build_by_default: false,
    dependencies: [opencv_geometry],
)

geometric_verifier_test = executable(
    'test-geometric-verifier-core',
    sources: [
        'tests/test_geometric_verifier_core.cpp',
        'src/geometric_verifier.cpp',
        'src/match_file.c',
        'src/feature_store.c',
        'src/feature_extractor_opencv.cpp',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
        'src/image_catalog_persistent.c',
        'src/app_state.c',
    ],
    cpp_args: ['-DLARDON3D_GEOMETRIC_VERIFIER_TESTING'],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv_geometry],
)

test('geometric-verifier-core', geometric_verifier_test, timeout: 60)

geometric_verifier_task_test = executable(
    'test-geometric-verifier-task',
    sources: [
        'tests/test_geometric_verifier_task.c', 'src/app_state.c', 'src/project.c',
        'src/project_db.c', 'src/project_db_sparse_sfm.c', 'src/task.c', 'src/task_checkpoint.c',
        'src/task_kind_registry.c', 'src/task_kinds.c', 'src/task_queue.c',
        'src/geometric_verifier_task.c', 'src/geometric_verifier.cpp',
        'src/match_file.c', 'src/feature_store.c', 'src/resource_governor.c',
        'src/resource_snapshot.c', 'src/import.c', 'src/import_task.c',
        'src/image_catalog.c', 'src/image_catalog_persistent.c', 'src/image_view.c',
        'src/feature_task.c', 'src/sift_task.c', 'src/precision_features.c',
        'src/feature_extractor_opencv.cpp', 'src/visual_index.c',
        'src/visual_index_task.c', 'src/candidate_pair_gen.c',
        'src/candidate_pair_task.c', 'src/matcher_task.c', 'src/matcher.cpp',
    ] + matcher_backend_sources,
    c_args: [
        '-DLARDON3D_PROJECT_DB_TESTING',
        '-DLARDON3D_GEOMETRIC_VERIFIER_TASK_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv] + matcher_backend_dependencies,
)
test('geometric-verifier-task', geometric_verifier_task_test, timeout: 60,
     env: opencv_test_environment)

track_builder_test = executable(
    'test-track-builder-core',
    sources: ['tests/test_track_builder_core.cpp', 'src/track_builder.cpp'],
    include_directories: include_directories('include'),
    dependencies: [openssl],
)
test('track-builder-core', track_builder_test, timeout: 120)

track_builder_project_test = executable(
    'test-track-builder-project',
    sources: [
        'tests/test_track_builder_project.cpp',
        'src/track_builder.cpp',
        'src/track_builder_project.cpp',
        'src/track_builder_task.cpp',
        'src/project_db.c', 'src/project_db_sparse_sfm.c',
        'src/app_state.c',
        'src/task.c',
        'src/task_kind_registry.c',
        'src/task_checkpoint.c',
        'src/task_queue.c',
        'src/hardware_profile.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
        'src/match_file.c',
    ],
    include_directories: include_directories('include'),
    cpp_args: [
        '-DLARDON3D_TRACK_BUILDER_PROJECT_TESTING',
        '-DLARDON3D_TRACK_BUILDER_TASK_TESTING',
    ],
    dependencies: [threads, sqlite3, openssl],
)
test('track-builder-project', track_builder_project_test, timeout: 60)

executable(
    'benchmark-track-builder',
    sources: ['tests/benchmark_track_builder.cpp', 'src/track_builder.cpp'],
    build_by_default: false,
    include_directories: include_directories('include'),
    dependencies: [openssl],
)
