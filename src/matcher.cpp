#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

#include <fcntl.h>
#include <math.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>

extern "C" {
#include <lardon3d/feature_extractor.h>
#include <lardon3d/match_file.h>
#include <lardon3d/matcher.h>
}

static const char matcher_orb_str[] = "orb_bf";
static const char matcher_sift_str[] = "sift_bf";
static const char matcher_rootsift_str[] = "rootsift_bf";

static uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
}

static bool join_path(char output[4096], const char *a, const char *b) {
    int n = snprintf(output, 4096, "%s/%s", a, b);
    return n > 0 && (size_t)n < 4096;
}

static bool ensure_directory(const char *path) {
    if (mkdir(path, 0755) == 0) {
        return true;
    }
    if (errno != EEXIST) {
        return false;
    }
    struct stat info;
    return lstat(path, &info) == 0 && S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode);
}

static bool sync_directory(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) {
        ok = false;
    }
    return ok;
}

static void hex_sha(const unsigned char hash[32], char text[65]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        text[2 * i] = digits[hash[i] >> 4];
        text[2 * i + 1] = digits[hash[i] & 15];
    }
    text[64] = '\0';
}

static bool sha256_fd(int fd, unsigned char output[32]) {
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context) {
        return false;
    }
    bool ok = EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    unsigned char buffer[65536];
    off_t offset = 0;
    while (ok) {
        ssize_t n = pread(fd, buffer, sizeof(buffer), offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) {
            break;
        }
        ok = EVP_DigestUpdate(context, buffer, (size_t)n) == 1;
        offset += (off_t)n;
    }
    unsigned int length = 0;
    ok = ok && EVP_DigestFinal_ex(context, output, &length) == 1 && length == 32;
    EVP_MD_CTX_free(context);
    return ok;
}

static bool sha256_file(const char *path, unsigned char output[32]) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    bool ok = sha256_fd(fd, output);
    (void)close(fd);
    return ok;
}

/* --- matcher_kind helpers --- */

extern "C" const char *lardon3d_matcher_kind_string(Lardon3DMatcherKind kind) {
    switch (kind) {
        case LARDON3D_MATCHER_ORB_BF:
            return matcher_orb_str;
        case LARDON3D_MATCHER_SIFT_BF:
            return matcher_sift_str;
        case LARDON3D_MATCHER_ROOTSIFT_BF:
            return matcher_rootsift_str;
        default:
            return "unknown";
    }
}

extern "C" float lardon3d_matcher_default_ratio(Lardon3DMatcherKind kind) {
    switch (kind) {
        case LARDON3D_MATCHER_ORB_BF:
            return 0.75F;
        case LARDON3D_MATCHER_SIFT_BF:
        case LARDON3D_MATCHER_ROOTSIFT_BF:
            return 0.7F;
        default:
            return 0.75F;
    }
}

extern "C" void lardon3d_matcher_fingerprint(const Lardon3DMatcherParams *params,
                                               unsigned char fingerprint[32]) {
    memset(fingerprint, 0, 32);
    if (!params) {
        return;
    }
    const char *kind_str = lardon3d_matcher_kind_string(params->kind);
    size_t kind_len = strlen(kind_str);
    size_t total = kind_len + 4 + 4 + 4 + 4;  /* kind + version + knn_k + ratio + cross_check */
    unsigned char buffer[128];  /* enough for any kind string */
    if (total > sizeof(buffer)) {
        return;
    }
    memcpy(buffer, kind_str, kind_len);
    uint32_t ratio_bits = 0;
    memcpy(&ratio_bits, &params->ratio_threshold, sizeof(ratio_bits));
    auto put_le32 = [](unsigned char *p, uint32_t value) {
        p[0] = (unsigned char)value;
        p[1] = (unsigned char)(value >> 8);
        p[2] = (unsigned char)(value >> 16);
        p[3] = (unsigned char)(value >> 24);
    };
    put_le32(buffer + kind_len, LARDON3D_MATCHER_VERSION);
    put_le32(buffer + kind_len + 4, LARDON3D_MATCHER_KNN_K);
    put_le32(buffer + kind_len + 8, ratio_bits);
    put_le32(buffer + kind_len + 12, 0);

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx) {
        if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) &&
            EVP_DigestUpdate(ctx, buffer, total) &&
            EVP_DigestFinal_ex(ctx, fingerprint, NULL)) {
            /* success */
        }
        EVP_MD_CTX_free(ctx);
    }
}

/* --- Matching logic --- */

struct MatchEntry {
    int query_idx;
    int train_idx;
    float distance;
};

struct FeatureReaderPair {
    Lardon3DFeatureReader *a = nullptr;
    Lardon3DFeatureReader *b = nullptr;

    ~FeatureReaderPair() {
        lardon3d_feature_reader_close(a);
        lardon3d_feature_reader_close(b);
    }
};

static bool accept_knn_match(const std::vector<cv::DMatch> &knn, float threshold,
                             MatchEntry *entry) {
    if (!entry || knn.empty() || !std::isfinite(threshold) || threshold <= 0.0F ||
        threshold >= 1.0F) {
        return false;
    }
    const cv::DMatch &first = knn[0];
    if (first.queryIdx < 0 || first.trainIdx < 0 || !std::isfinite(first.distance) ||
        first.distance < 0.0F) {
        return false;
    }
    if (knn.size() >= 2) {
        const float second = knn[1].distance;
        if (!std::isfinite(second) || second < 0.0F || second == 0.0F ||
            !(first.distance < threshold * second)) {
            return false;
        }
    }
    *entry = {first.queryIdx, first.trainIdx, first.distance};
    return true;
}

static bool operator<(const MatchEntry &a, const MatchEntry &b) {
    if (a.query_idx != b.query_idx) {
        return a.query_idx < b.query_idx;
    }
    if (a.distance != b.distance) {
        return a.distance < b.distance;
    }
    return a.train_idx < b.train_idx;
}

static bool fill_descriptors_u8(std::vector<unsigned char> &buffer,
                                 Lardon3DFeatureReader *reader,
                                 uint32_t feature_count,
                                 uint32_t descriptor_dimension) {
    size_t total = (size_t)feature_count * descriptor_dimension;
    buffer.resize(total);
    size_t offset = 0;
    for (uint32_t start = 0; start < feature_count; start += 256) {
        uint32_t batch = feature_count - start;
        if (batch > 256) {
            batch = 256;
        }
        size_t batch_bytes = (size_t)batch * descriptor_dimension;
        Lardon3DFeatureStoreResult r = lardon3d_feature_reader_descriptors_u8(
            reader, start, buffer.data() + offset, batch, batch_bytes);
        if (r != LARDON3D_FEATURE_STORE_OK) {
            return false;
        }
        offset += batch_bytes;
    }
    return true;
}

static bool fill_descriptors_f32(std::vector<float> &buffer,
                                  Lardon3DFeatureReader *reader,
                                  uint32_t feature_count,
                                  uint32_t descriptor_dimension) {
    size_t total = (size_t)feature_count * descriptor_dimension;
    buffer.resize(total);
    size_t offset = 0;
    for (uint32_t start = 0; start < feature_count; start += 256) {
        uint32_t batch = feature_count - start;
        if (batch > 256) {
            batch = 256;
        }
        size_t batch_bytes = (size_t)batch * descriptor_dimension * sizeof(float);
        Lardon3DFeatureStoreResult r = lardon3d_feature_reader_descriptors_f32(
            reader, start, buffer.data() + offset, batch, batch_bytes);
        if (r != LARDON3D_FEATURE_STORE_OK) {
            return false;
        }
        offset += (size_t)batch * descriptor_dimension;
    }
    return true;
}

static Lardon3DMatcherResult matcher_run_impl(
    const char *project_path,
    const Lardon3DProjectDbFeatureSet *feature_set_a,
    const Lardon3DProjectDbFeatureSet *feature_set_b,
    const Lardon3DMatcherParams *params,
    const char *match_file_path,
    Lardon3DMatcherStats *stats) {
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
    if (!project_path || !feature_set_a || !feature_set_b || !params || !match_file_path ||
        !stats || params->kind < LARDON3D_MATCHER_ORB_BF ||
        params->kind > LARDON3D_MATCHER_ROOTSIFT_BF ||
        !std::isfinite(params->ratio_threshold) || params->ratio_threshold <= 0.0F ||
        params->ratio_threshold >= 1.0F) {
        return LARDON3D_MATCHER_INVALID_ARGUMENT;
    }
    if (feature_set_a->feature_count > LARDON3D_FEATURE_MAX_FEATURES ||
        feature_set_b->feature_count > LARDON3D_FEATURE_MAX_FEATURES) {
        return LARDON3D_MATCHER_INVALID_ARGUMENT;
    }
    if (feature_set_a->feature_set_id == feature_set_b->feature_set_id) {
        return LARDON3D_MATCHER_INVALID_ARGUMENT;
    }

    /* Verify same descriptor type and dimension */
    if (feature_set_a->descriptor_type != feature_set_b->descriptor_type ||
        feature_set_a->descriptor_dimension != feature_set_b->descriptor_dimension) {
        return LARDON3D_MATCHER_TYPE_MISMATCH;
    }

    /* Verify descriptor type matches matcher kind */
    if (params->kind == LARDON3D_MATCHER_ORB_BF) {
        if (strcmp(feature_set_a->extractor_kind, "orb") != 0 ||
            strcmp(feature_set_b->extractor_kind, "orb") != 0 ||
            feature_set_a->descriptor_type != LARDON3D_FEATURE_DESCRIPTOR_U8 ||
            feature_set_a->descriptor_dimension != 32) {
            return LARDON3D_MATCHER_TYPE_MISMATCH;
        }
    } else if (params->kind == LARDON3D_MATCHER_SIFT_BF) {
        if (strcmp(feature_set_a->extractor_kind, "sift") != 0 ||
            strcmp(feature_set_b->extractor_kind, "sift") != 0 ||
            feature_set_a->descriptor_type != LARDON3D_FEATURE_DESCRIPTOR_F32 ||
            feature_set_a->descriptor_dimension != 128) {
            return LARDON3D_MATCHER_TYPE_MISMATCH;
        }
    } else if (strcmp(feature_set_a->extractor_kind, "rootsift") != 0 ||
               strcmp(feature_set_b->extractor_kind, "rootsift") != 0 ||
               feature_set_a->descriptor_type != LARDON3D_FEATURE_DESCRIPTOR_F32 ||
               feature_set_a->descriptor_dimension != 128) {
        return LARDON3D_MATCHER_TYPE_MISMATCH;
    }

    stats->feature_count_a = feature_set_a->feature_count;
    stats->feature_count_b = feature_set_b->feature_count;

    /* Open feature readers */
    FeatureReaderPair readers;
    Lardon3DFeatureFileMetadata meta_a, meta_b;
    auto phase_start = std::chrono::steady_clock::now();
    Lardon3DFeatureStoreResult open_a = lardon3d_feature_reader_open(
        project_path, feature_set_a, &readers.a, &meta_a);
    Lardon3DFeatureStoreResult open_b = lardon3d_feature_reader_open(
        project_path, feature_set_b, &readers.b, &meta_b);
    if (open_a != LARDON3D_FEATURE_STORE_OK || open_b != LARDON3D_FEATURE_STORE_OK) {
        return LARDON3D_MATCHER_IO_ERROR;
    }
    stats->feature_open_ns = elapsed_ns(phase_start);

    Lardon3DMatcherResult result = LARDON3D_MATCHER_OK;
    std::vector<MatchEntry> filtered_matches;
    filtered_matches.reserve(feature_set_a->feature_count);

    if (params->kind == LARDON3D_MATCHER_ORB_BF) {
        std::vector<unsigned char> desc_a, desc_b;
        phase_start = std::chrono::steady_clock::now();
        if (!fill_descriptors_u8(desc_a, readers.a, feature_set_a->feature_count, 32) ||
            !fill_descriptors_u8(desc_b, readers.b, feature_set_b->feature_count, 32)) {
            result = LARDON3D_MATCHER_IO_ERROR;
        } else {
            stats->descriptor_read_ns = elapsed_ns(phase_start);
            cv::Mat mat_a((int)feature_set_a->feature_count, 32, CV_8UC1, desc_a.data());
            cv::Mat mat_b((int)feature_set_b->feature_count, 32, CV_8UC1, desc_b.data());
            cv::BFMatcher matcher(cv::NORM_HAMMING, false);
            std::vector<std::vector<cv::DMatch>> matches;
            phase_start = std::chrono::steady_clock::now();
            matcher.knnMatch(mat_a, mat_b, matches, LARDON3D_MATCHER_KNN_K);
            stats->knn_ns = elapsed_ns(phase_start);
            stats->knn_query_count = (uint32_t)matches.size();
            phase_start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < matches.size(); ++i) {
                const auto &knn = matches[i];
                MatchEntry entry;
                if (accept_knn_match(knn, params->ratio_threshold, &entry))
                    filtered_matches.push_back(entry);
            }
            stats->filter_ns = elapsed_ns(phase_start);
        }
    } else {
        std::vector<float> desc_a, desc_b;
        phase_start = std::chrono::steady_clock::now();
        if (!fill_descriptors_f32(desc_a, readers.a, feature_set_a->feature_count, 128) ||
            !fill_descriptors_f32(desc_b, readers.b, feature_set_b->feature_count, 128)) {
            result = LARDON3D_MATCHER_IO_ERROR;
        } else {
            stats->descriptor_read_ns = elapsed_ns(phase_start);
            cv::Mat mat_a((int)feature_set_a->feature_count, 128, CV_32FC1, desc_a.data());
            cv::Mat mat_b((int)feature_set_b->feature_count, 128, CV_32FC1, desc_b.data());
            cv::BFMatcher matcher(cv::NORM_L2, false);
            std::vector<std::vector<cv::DMatch>> matches;
            phase_start = std::chrono::steady_clock::now();
            matcher.knnMatch(mat_a, mat_b, matches, LARDON3D_MATCHER_KNN_K);
            stats->knn_ns = elapsed_ns(phase_start);
            stats->knn_query_count = (uint32_t)matches.size();
            phase_start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < matches.size(); ++i) {
                const auto &knn = matches[i];
                MatchEntry entry;
                if (accept_knn_match(knn, params->ratio_threshold, &entry))
                    filtered_matches.push_back(entry);
            }
            stats->filter_ns = elapsed_ns(phase_start);
        }
    }

    if (result != LARDON3D_MATCHER_OK) {
        return result;
    }

    phase_start = std::chrono::steady_clock::now();
    /* Sort deterministically */
    std::sort(filtered_matches.begin(), filtered_matches.end());

    for (size_t i = 1; i < filtered_matches.size(); ++i) {
        if (filtered_matches[i].query_idx == filtered_matches[i - 1].query_idx) {
            return LARDON3D_MATCHER_FAILED;
        }
    }

    stats->match_count = (uint32_t)filtered_matches.size();
    stats->canonicalize_ns = elapsed_ns(phase_start);

    if (filtered_matches.size() > LARDON3D_MATCH_FILE_MAX_MATCHES) {
        return LARDON3D_MATCHER_FAILED;
    }

    /* Build entry array and write */
    std::vector<Lardon3DMatchFileEntry> entries(filtered_matches.size());
    for (size_t i = 0; i < filtered_matches.size(); ++i) {
        entries[i].feature_index_a = (uint32_t)filtered_matches[i].query_idx;
        entries[i].feature_index_b = (uint32_t)filtered_matches[i].train_idx;
        entries[i].distance = filtered_matches[i].distance;
    }

    uint8_t descriptor_type = feature_set_a->descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_U8
                                  ? (uint8_t)1
                                  : (uint8_t)2;
    phase_start = std::chrono::steady_clock::now();
    int fd = open(match_file_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return LARDON3D_MATCHER_IO_ERROR;
    }
    Lardon3DMatchFileResult write_result = lardon3d_match_file_write(
        fd, descriptor_type, feature_set_a->descriptor_dimension,
        feature_set_a->feature_set_id, feature_set_b->feature_set_id,
        entries.data(), (uint32_t)entries.size());
    if (write_result == LARDON3D_MATCH_FILE_OK) {
        if (fsync(fd) != 0) {
            write_result = LARDON3D_MATCH_FILE_IO_ERROR;
        }
    }
    (void)close(fd);
    if (write_result != LARDON3D_MATCH_FILE_OK) {
        unlink(match_file_path);
        return LARDON3D_MATCHER_IO_ERROR;
    }
    stats->serialize_ns = elapsed_ns(phase_start);

    return LARDON3D_MATCHER_OK;
}

extern "C" Lardon3DMatcherResult lardon3d_matcher_run(
    const char *project_path,
    const Lardon3DProjectDbFeatureSet *feature_set_a,
    const Lardon3DProjectDbFeatureSet *feature_set_b,
    const Lardon3DMatcherParams *params,
    const char *match_file_path,
    Lardon3DMatcherStats *stats) {
    try {
        return matcher_run_impl(project_path, feature_set_a, feature_set_b, params,
                                match_file_path, stats);
    } catch (const cv::Exception &) {
        return LARDON3D_MATCHER_FAILED;
    } catch (const std::bad_alloc &) {
        return LARDON3D_MATCHER_FAILED;
    }
}

extern "C" Lardon3DMatcherResult lardon3d_matcher_match_and_publish_profiled(
    const char *project_path,
    Lardon3DProjectDb *database,
    const Lardon3DProjectDbCandidatePair *pair,
    const Lardon3DProjectDbFeatureSet *feature_set_a,
    const Lardon3DProjectDbFeatureSet *feature_set_b,
    const Lardon3DMatcherParams *params,
    Lardon3DProjectDbMatchResult *result,
    Lardon3DMatcherStats *profile) {
    auto total_start = std::chrono::steady_clock::now();
    if (profile) memset(profile, 0, sizeof(*profile));
    if (result) {
        memset(result, 0, sizeof(*result));
    }
    if (!project_path || !database || !pair || !feature_set_a || !feature_set_b || !params ||
        !result) {
        return LARDON3D_MATCHER_INVALID_ARGUMENT;
    }

    const char *matcher_kind = lardon3d_matcher_kind_string(params->kind);
    unsigned char fp[32];
    lardon3d_matcher_fingerprint(params, fp);
    uint64_t repair_match_result_id = 0;

    /* Check if Match Result already exists */
    Lardon3DProjectDbResult found = lardon3d_project_db_find_match_result(
        database, pair->candidate_pair_id, feature_set_a->feature_set_id,
        feature_set_b->feature_set_id, matcher_kind, LARDON3D_MATCHER_VERSION, fp, result);
    if (found == LARDON3D_PROJECT_DB_OK) {
        if (result->result_status == LARDON3D_MATCH_RESULT_STATUS_NO_MATCH &&
            result->match_count == 0 && !result->has_match_asset &&
            result->match_asset_path[0] == '\0' && result->match_asset_size_bytes == 0) {
            if (profile) profile->total_ns = elapsed_ns(total_start);
            return LARDON3D_MATCHER_OK;
        }
        if (result->result_status == LARDON3D_MATCH_RESULT_STATUS_MATCHED &&
            result->match_count > 0 && result->has_match_asset &&
            result->match_asset_path[0] != '\0' && result->match_asset_size_bytes > 0) {
            char asset_full[4096];
            if (join_path(asset_full, project_path, result->match_asset_path)) {
                Lardon3DMatchFileHeader header;
                if (lardon3d_match_file_validate_asset(
                        asset_full, result->match_asset_sha256,
                        result->match_asset_size_bytes, &header, feature_set_a->feature_set_id,
                        feature_set_b->feature_set_id, feature_set_a->feature_count,
                        feature_set_b->feature_count) == LARDON3D_MATCH_FILE_OK &&
                    header.match_count == result->match_count &&
                    header.descriptor_type == feature_set_a->descriptor_type &&
                    header.descriptor_dimension == feature_set_a->descriptor_dimension) {
                        if (profile) profile->total_ns = elapsed_ns(total_start);
                        return LARDON3D_MATCHER_OK;
                }
            }
        }
        repair_match_result_id = result->match_result_id;
    } else if (found != LARDON3D_PROJECT_DB_NOT_FOUND) {
        return LARDON3D_MATCHER_IO_ERROR;
    }

    /* Ensure assets/matches directory */
    char assets[4096], matches_dir[4096];
    if (!join_path(assets, project_path, "assets") || !ensure_directory(assets) ||
        !join_path(matches_dir, assets, "matches") || !ensure_directory(matches_dir)) {
        return LARDON3D_MATCHER_IO_ERROR;
    }

    /* Write to temp file */
    char tmp_path[4096];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s/.match-XXXXXX", matches_dir);
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) {
        return LARDON3D_MATCHER_IO_ERROR;
    }
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        return LARDON3D_MATCHER_IO_ERROR;
    }

    /* matcher_run reopens the named temp with O_TRUNC. */
    if (close(tmp_fd) != 0) {
        (void)unlink(tmp_path);
        return LARDON3D_MATCHER_IO_ERROR;
    }

    Lardon3DMatcherStats stats;
    Lardon3DMatcherResult run_result = lardon3d_matcher_run(
        project_path, feature_set_a, feature_set_b, params, tmp_path, &stats);
    if (run_result != LARDON3D_MATCHER_OK) {
        unlink(tmp_path);
        return run_result;
    }
    if (profile) *profile = stats;

    int64_t now = (int64_t)time(NULL);
    if (now < 0) {
        unlink(tmp_path);
        return LARDON3D_MATCHER_IO_ERROR;
    }
    if (stats.match_count == 0) {
        if (unlink(tmp_path) != 0) return LARDON3D_MATCHER_IO_ERROR;
        auto database_start = std::chrono::steady_clock::now();
        Lardon3DProjectDbResult db_result = repair_match_result_id != 0
            ? lardon3d_project_db_repair_match_result(
                  database, repair_match_result_id, LARDON3D_MATCH_RESULT_STATUS_NO_MATCH,
                  0, NULL, NULL, 0, result)
            : lardon3d_project_db_create_match_result(
                  database, pair->candidate_pair_id, feature_set_a->feature_set_id,
                  feature_set_b->feature_set_id, matcher_kind, LARDON3D_MATCHER_VERSION, fp,
                  LARDON3D_MATCH_RESULT_STATUS_NO_MATCH, 0, NULL, NULL, 0, now, result);
        if (db_result == LARDON3D_PROJECT_DB_CONSTRAINT) {
            return lardon3d_matcher_match_and_publish_profiled(
                project_path, database, pair, feature_set_a, feature_set_b, params, result,
                profile);
        }
        if (profile) {
            profile->database_ns = elapsed_ns(database_start);
            profile->total_ns = elapsed_ns(total_start);
        }
        return db_result == LARDON3D_PROJECT_DB_OK ? LARDON3D_MATCHER_OK
                                                   : LARDON3D_MATCHER_FAILED;
    }

    /* Compute SHA-256 of temp file */
    auto sha_start = std::chrono::steady_clock::now();
    unsigned char file_hash[32];
    if (!sha256_file(tmp_path, file_hash)) {
        unlink(tmp_path);
        return LARDON3D_MATCHER_IO_ERROR;
    }
    if (profile) profile->sha256_ns = elapsed_ns(sha_start);

    /* Create content-addressed directory */
    auto publication_start = std::chrono::steady_clock::now();
    char hex[65];
    hex_sha(file_hash, hex);
    char prefix[4096];
    n = snprintf(prefix, sizeof(prefix), "%s/%.2s", matches_dir, hex);
    if (n <= 0 || (size_t)n >= sizeof(prefix) || !ensure_directory(prefix)) {
        unlink(tmp_path);
        return LARDON3D_MATCHER_IO_ERROR;
    }

    /* Build relative and final paths */
    char relative[LARDON3D_PROJECT_DB_PATH_CAPACITY];
    n = snprintf(relative, sizeof(relative), "assets/matches/%.2s/%s", hex, hex);
    if (n <= 0 || (size_t)n >= (int)sizeof(relative)) {
        unlink(tmp_path);
        return LARDON3D_MATCHER_IO_ERROR;
    }
    char final_path[4096];
    if (!join_path(final_path, project_path, relative)) {
        unlink(tmp_path);
        return LARDON3D_MATCHER_IO_ERROR;
    }

    /* Atomic content-addressed publication via link. */
    if (link(tmp_path, final_path) != 0) {
        if (errno != EEXIST) {
            unlink(tmp_path);
            return LARDON3D_MATCHER_IO_ERROR;
        }
        /* EEXIST: verify content match */
        int existing = open(final_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat info;
        unsigned char existing_hash[32];
        bool same = existing >= 0 && fstat(existing, &info) == 0 && S_ISREG(info.st_mode) &&
                    sha256_fd(existing, existing_hash) &&
                    memcmp(existing_hash, file_hash, 32) == 0;
        if (existing >= 0) {
            (void)close(existing);
        }
        if (!same) {
            if (repair_match_result_id == 0 || rename(tmp_path, final_path) != 0) {
                unlink(tmp_path);
                return LARDON3D_MATCHER_IO_ERROR;
            }
            tmp_path[0] = '\0';
        }
    }
    if (tmp_path[0] != '\0' && unlink(tmp_path) != 0) {
        return LARDON3D_MATCHER_IO_ERROR;
    }
    if (!sync_directory(prefix)) {
        return LARDON3D_MATCHER_IO_ERROR;
    }
    if (profile) profile->publication_ns = elapsed_ns(publication_start);

    /* Get match file size */
    struct stat st;
    uint64_t file_size = 0;
    if (stat(final_path, &st) == 0 && S_ISREG(st.st_mode)) {
        file_size = (uint64_t)st.st_size;
    }

    auto database_start = std::chrono::steady_clock::now();
    Lardon3DProjectDbResult db_result = repair_match_result_id != 0
        ? lardon3d_project_db_repair_match_result(
              database, repair_match_result_id, LARDON3D_MATCH_RESULT_STATUS_MATCHED,
              stats.match_count, file_hash, relative, (uint64_t)file_size, result)
        : lardon3d_project_db_create_match_result(
              database, pair->candidate_pair_id, feature_set_a->feature_set_id,
              feature_set_b->feature_set_id, matcher_kind, LARDON3D_MATCHER_VERSION, fp,
              LARDON3D_MATCH_RESULT_STATUS_MATCHED, stats.match_count, file_hash, relative,
              (uint64_t)file_size, now, result);
    if (db_result == LARDON3D_PROJECT_DB_CONSTRAINT) {
        return lardon3d_matcher_match_and_publish_profiled(
            project_path, database, pair, feature_set_a, feature_set_b, params, result, profile);
    }
    if (db_result != LARDON3D_PROJECT_DB_OK) {
        return LARDON3D_MATCHER_FAILED;
    }

    if (profile) {
        profile->database_ns = elapsed_ns(database_start);
        profile->total_ns = elapsed_ns(total_start);
    }
    return LARDON3D_MATCHER_OK;
}

extern "C" Lardon3DMatcherResult lardon3d_matcher_match_and_publish(
    const char *project_path,
    Lardon3DProjectDb *database,
    const Lardon3DProjectDbCandidatePair *pair,
    const Lardon3DProjectDbFeatureSet *feature_set_a,
    const Lardon3DProjectDbFeatureSet *feature_set_b,
    const Lardon3DMatcherParams *params,
    Lardon3DProjectDbMatchResult *result) {
    return lardon3d_matcher_match_and_publish_profiled(
        project_path, database, pair, feature_set_a, feature_set_b, params, result, NULL);
}
