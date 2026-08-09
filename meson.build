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
    modules: ['opencv_core', 'opencv_imgcodecs', 'opencv_features2d'],
)
opencv_benchmark = dependency(
    'opencv5',
    required: true,
    include_type: 'system',
    modules: ['opencv_core', 'opencv_imgcodecs', 'opencv_features2d', 'opencv_imgproc'],
)

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
        'src/image_view.c',
        'src/project.c',
        'src/project_db.c',
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
    ],
    include_directories: include_directories('include'),
    dependencies: [ncursesw, threads, sqlite3, openssl, opencv],
)

import_test = executable(
    'test-import',
    sources: [
        'tests/test_import.c',
        'src/app_state.c',
        'src/import.c',
        'src/image_catalog_persistent.c',
        'src/project_db.c',
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
        'src/project_db.c',
        'src/task.c',
        'src/task_checkpoint.c',
        'src/task_kind_registry.c',
        'src/task_kinds.c',
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
    ],
    c_args: [
        '-DLARDON3D_IMPORT_TASK_TESTING',
        '-DLARDON3D_PROJECT_DB_TESTING',
        '-DLARDON3D_CHECKPOINT_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv],
)

test('import-task', import_task_test, timeout: 30)

image_catalog_test = executable(
    'test-image-catalog',
    sources: [
        'tests/test_image_catalog.c',
        'src/app_state.c',
        'src/image_catalog.c',
        'src/image_catalog_persistent.c',
        'src/project_db.c',
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
        'src/project_db.c',
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
        'src/project_db.c',
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
        'src/project_db.c',
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
        'src/visual_index.c', 'src/project_db.c', 'src/task.c',
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
        'src/project_db.c', 'src/task.c',
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
        'src/project.c', 'src/project_db.c', 'src/task.c',
        'src/task_checkpoint.c', 'src/task_kind_registry.c',
        'src/task_kinds.c', 'src/task_queue.c', 'src/import.c',
        'src/import_task.c', 'src/image_catalog.c',
        'src/image_catalog_persistent.c', 'src/image_view.c',
        'src/feature_task.c', 'src/sift_task.c',
        'src/precision_features.c', 'src/feature_store.c',
        'src/visual_index.c', 'src/visual_index_task.c',
        'src/candidate_pair_gen.c', 'src/candidate_pair_task.c',
        'src/feature_extractor_opencv.cpp',
        'src/resource_governor.c', 'src/resource_snapshot.c',
    ],
    c_args: [
        '-DLARDON3D_PROJECT_DB_TESTING',
        '-DLARDON3D_CANDIDATE_PAIR_TASK_TESTING',
        '-DLARDON3D_VISUAL_INDEX_TASK_TESTING',
        '-DLARDON3D_FEATURE_TASK_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv],
)
test('candidate-pair-task', candidate_pair_task_test, timeout: 60,
     env: opencv_test_environment)

feature_task_test = executable(
    'test-feature-task',
    sources: [
        'tests/test_feature_task.c', 'src/app_state.c', 'src/project.c',
        'src/project_db.c', 'src/task.c', 'src/task_checkpoint.c',
        'src/task_kind_registry.c', 'src/task_kinds.c', 'src/task_queue.c',
        'src/import.c', 'src/import_task.c', 'src/image_catalog.c',
        'src/image_catalog_persistent.c', 'src/image_view.c',
        'src/feature_task.c', 'src/sift_task.c', 'src/precision_features.c',
        'src/feature_store.c', 'src/visual_index.c',
        'src/visual_index_task.c',
        'src/candidate_pair_gen.c', 'src/candidate_pair_task.c',
        'src/feature_extractor_opencv.cpp', 'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    c_args: [
        '-DLARDON3D_PROJECT_DB_TESTING', '-DLARDON3D_FEATURE_TASK_TESTING',
        '-DLARDON3D_VISUAL_INDEX_TASK_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv],
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
        'src/project.c', 'src/project_db.c', 'src/task.c',
        'src/task_checkpoint.c', 'src/task_kind_registry.c',
        'src/task_kinds.c', 'src/task_queue.c', 'src/import.c',
        'src/import_task.c', 'src/image_catalog.c',
        'src/image_catalog_persistent.c', 'src/image_view.c',
        'src/feature_task.c', 'src/sift_task.c',
        'src/precision_features.c', 'src/feature_store.c',
        'src/visual_index.c', 'src/visual_index_task.c',
        'src/candidate_pair_gen.c', 'src/candidate_pair_task.c',
        'src/feature_extractor_opencv.cpp',
        'src/resource_governor.c', 'src/resource_snapshot.c',
    ],
    c_args: [
        '-DLARDON3D_PROJECT_DB_TESTING',
        '-DLARDON3D_FEATURE_TASK_TESTING',
        '-DLARDON3D_VISUAL_INDEX_TASK_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv],
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
        'src/project_db.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    c_args: ['-DLARDON3D_PROJECT_DB_TESTING'],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl],
)

test('project-db', project_db_test, timeout: 30)

match_result_test = executable(
    'test-match-result',
    sources: [
        'tests/test_match_result.c',
        'src/project_db.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
    ],
    c_args: ['-DLARDON3D_PROJECT_DB_TESTING'],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl],
)

test('match-result', match_result_test, timeout: 30)

project_test = executable(
    'test-project',
    sources: [
        'tests/test_project.c',
        'src/app_state.c',
        'src/project.c',
        'src/project_db.c',
        'src/task.c',
        'src/task_checkpoint.c',
        'src/task_kind_registry.c',
        'src/task_kinds.c',
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
    ],
    c_args: [
        '-DLARDON3D_PROJECT_DB_TESTING',
        '-DLARDON3D_CHECKPOINT_TESTING',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv],
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
        'src/project_db.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
        'src/image_catalog_persistent.c',
        'src/app_state.c',
    ],
    c_args: ['-DLARDON3D_PROJECT_DB_TESTING'],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv],
)

test('matcher', matcher_test, timeout: 60)

matcher_e2e_test = executable(
    'test-matcher-e2e',
    sources: [
        'tests/test_matcher_e2e.c',
        'src/match_file.c',
        'src/matcher.cpp',
        'src/feature_store.c',
        'src/feature_extractor_opencv.cpp',
        'src/project_db.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
        'src/image_catalog_persistent.c',
        'src/app_state.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv],
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
        'src/project_db.c',
        'src/task.c',
        'src/resource_governor.c',
        'src/resource_snapshot.c',
        'src/image_catalog_persistent.c',
        'src/app_state.c',
    ],
    include_directories: include_directories('include'),
    dependencies: [threads, sqlite3, openssl, opencv],
)
