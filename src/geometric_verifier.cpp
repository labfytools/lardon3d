#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <new>
#include <opencv2/calib3d.hpp>
#include <openssl/evp.h>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" {
#include <lardon3d/feature_store.h>
#include <lardon3d/geometric_verifier.h>
#include <lardon3d/match_file.h>
}

namespace {

#ifdef LARDON3D_GEOMETRIC_VERIFIER_TESTING
uint32_t estimator_calls;
#endif

void put_u32(unsigned char *bytes, uint32_t value) {
  for (unsigned int index = 0; index < 4; ++index)
    bytes[index] = static_cast<unsigned char>(value >> (8U * index));
}

void put_u64(unsigned char *bytes, uint64_t value) {
  for (unsigned int index = 0; index < 8; ++index)
    bytes[index] = static_cast<unsigned char>(value >> (8U * index));
}

uint64_t double_bits(double value) {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

bool digest(const unsigned char *bytes, size_t size, unsigned char output[32]) {
  unsigned int output_size = 0;
  return EVP_Digest(bytes, size, output, &output_size, EVP_sha256(), nullptr) ==
             1 &&
         output_size == 32;
}

double normalized_zero(double value) { return value == 0.0 ? 0.0 : value; }

bool join_path(char output[4096], const char *root, const char *relative) {
  if (!root || !relative || relative[0] == '/' || std::strstr(relative, ".."))
    return false;
  int length = std::snprintf(output, 4096, "%s/%s", root, relative);
  return length > 0 && length < 4096;
}

bool read_points(const char *project_path,
                 const Lardon3DProjectDbFeatureSet &set,
                 std::vector<Lardon3DFeatureKeypoint> &points) {
  Lardon3DFeatureReader *reader = nullptr;
  Lardon3DFeatureFileMetadata metadata;
  if (lardon3d_feature_reader_open(project_path, &set, &reader, &metadata) !=
      LARDON3D_FEATURE_STORE_OK)
    return false;
  points.resize(set.feature_count);
  bool ok = metadata.feature_count == set.feature_count;
  for (uint32_t start = 0; ok && start < set.feature_count; start += 256) {
    size_t count = std::min<size_t>(256, set.feature_count - start);
    ok = lardon3d_feature_reader_keypoints(reader, start, points.data() + start,
                                           count) == LARDON3D_FEATURE_STORE_OK;
  }
  lardon3d_feature_reader_close(reader);
  return ok;
}

bool estimate_fundamental(const std::vector<cv::Point2d> &points_a,
                          const std::vector<cv::Point2d> &points_b,
                          const Lardon3DGeometricVerifierParameters *parameters,
                          uint32_t seed, cv::Mat &model, cv::Mat &mask) {
#ifdef LARDON3D_GEOMETRIC_VERIFIER_TESTING
  ++estimator_calls;
  const char *behavior = std::getenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR");
  if (behavior && std::strcmp(behavior, "error") == 0)
    return false;
  if (behavior && std::strcmp(behavior, "bad-alloc") == 0)
    throw std::bad_alloc();
  if (behavior && std::strcmp(behavior, "empty") == 0)
    return true;
  if (behavior && std::strcmp(behavior, "malformed-mask") == 0) {
    model = cv::Mat::eye(3, 3, CV_64F);
    mask = cv::Mat::ones(1, 1, CV_8U);
    return true;
  }
  if (behavior && std::strcmp(behavior, "malformed-model") == 0) {
    model = cv::Mat::eye(2, 2, CV_64F);
    mask = cv::Mat::ones(static_cast<int>(points_a.size()), 1, CV_8U);
    return true;
  }
  if (behavior && std::strcmp(behavior, "nan-model") == 0) {
    model = cv::Mat::eye(3, 3, CV_64F);
    model.at<double>(0, 0) = NAN;
    mask = cv::Mat::ones(static_cast<int>(points_a.size()), 1, CV_8U);
    return true;
  }
  if (behavior && std::strcmp(behavior, "controlled") == 0) {
    static const double fundamental[9] = {0.0,  0.0, 0.0, 0.0, 0.0,
                                          -1.0, 0.0, 1.0, 0.0};
    model = cv::Mat(3, 3, CV_64F, const_cast<double *>(fundamental)).clone();
    mask = cv::Mat::zeros(static_cast<int>(points_a.size()), 1, CV_8U);
    const char *bits = std::getenv("LARDON3D_TEST_GEOMETRIC_MASK");
    if (!bits || std::strlen(bits) != points_a.size())
      return false;
    for (size_t index = 0; index < points_a.size(); ++index) {
      if (bits[index] != '0' && bits[index] != '1')
        return false;
      mask.ptr<unsigned char>()[index] =
          static_cast<unsigned char>(bits[index] - '0');
    }
    return true;
  }
#endif
  cv::UsacParams params;
  params.confidence = parameters->confidence;
  params.isParallel = false;
  params.loIterations = 5;
  params.loMethod = cv::LOCAL_OPTIM_INNER_LO;
  params.loSampleSize = 14;
  params.maxIterations = static_cast<int>(parameters->max_iterations);
  params.neighborsSearch = cv::NEIGH_GRID;
  params.randomGeneratorState = static_cast<int>(seed);
  params.sampler = cv::SAMPLING_UNIFORM;
  params.score = cv::SCORE_METHOD_MAGSAC;
  params.threshold = parameters->threshold_pixels;
  params.final_polisher = cv::COV_POLISHER;
  params.final_polisher_iterations = 3;
  model = cv::findFundamentalMat(points_a, points_b, mask, params);
  return true;
}

} // namespace

#ifdef LARDON3D_GEOMETRIC_VERIFIER_TESTING
extern "C" void lardon3d_geometric_verifier_test_reset_estimator_calls(void) {
  estimator_calls = 0;
}

extern "C" uint32_t lardon3d_geometric_verifier_test_estimator_calls(void) {
  return estimator_calls;
}
#endif

extern "C" Lardon3DGeometricVerifierParameters
lardon3d_geometric_verifier_default_parameters(void) {
  return {1.5, 0.999, 5000, 16, 0.20, 1, 1};
}

extern "C" bool lardon3d_geometric_verifier_parameters_valid(
    const Lardon3DGeometricVerifierParameters *p) {
  return p && std::isfinite(p->threshold_pixels) && p->threshold_pixels > 0.0 &&
         std::isfinite(p->confidence) && p->confidence > 0.0 &&
         p->confidence < 1.0 && p->max_iterations > 0 &&
         p->max_iterations <= static_cast<uint32_t>(INT_MAX) &&
         p->min_inlier_count > 0 &&
         p->min_inlier_count <= LARDON3D_MATCH_FILE_MAX_MATCHES &&
         std::isfinite(p->min_inlier_ratio) && p->min_inlier_ratio >= 0.0 &&
         p->min_inlier_ratio <= 1.0 && p->seed_policy_version == 1 &&
         p->canonicalization_version == 1;
}

extern "C" bool lardon3d_geometric_verifier_fingerprint_bytes(
    const Lardon3DGeometricVerifierParameters *p,
    Lardon3DGeometricVerifierAlgorithm algorithm, uint32_t verifier_version,
    unsigned char bytes[LARDON3D_GEOMETRIC_VERIFIER_FINGERPRINT_SIZE]) {
  if (!bytes)
    return false;
  std::memset(bytes, 0, LARDON3D_GEOMETRIC_VERIFIER_FINGERPRINT_SIZE);
  if (!lardon3d_geometric_verifier_parameters_valid(p) ||
      verifier_version == 0 ||
      (algorithm != LARDON3D_GEOMETRIC_ALGORITHM_USAC_MAGSAC &&
       algorithm != LARDON3D_GEOMETRIC_ALGORITHM_USAC_DEFAULT))
    return false;
  std::memcpy(bytes, "L3DGVFP1", 8);
  put_u32(bytes + 8, 1);
  put_u32(bytes + 12, LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL);
  put_u32(bytes + 16, verifier_version);
  put_u32(bytes + 20, static_cast<uint32_t>(algorithm));
  put_u64(bytes + 24, double_bits(p->threshold_pixels));
  put_u64(bytes + 32, double_bits(p->confidence));
  put_u32(bytes + 40, p->max_iterations);
  put_u32(bytes + 44, p->min_inlier_count);
  put_u64(bytes + 48, double_bits(normalized_zero(p->min_inlier_ratio)));
  put_u32(bytes + 56, p->seed_policy_version);
  put_u32(bytes + 60, p->canonicalization_version);
  bytes[64] = 2; // Point2d.
  bytes[65] = cv::SAMPLING_UNIFORM;
  bytes[66] = algorithm == LARDON3D_GEOMETRIC_ALGORITHM_USAC_MAGSAC
                  ? cv::SCORE_METHOD_MAGSAC
                  : cv::SCORE_METHOD_MSAC;
  bytes[67] = 0; // isParallel=false.
  bytes[68] = cv::LOCAL_OPTIM_INNER_LO;
  put_u32(bytes + 69, 5);
  put_u32(bytes + 73, 14);
  bytes[77] = cv::NEIGH_GRID;
  bytes[78] = cv::COV_POLISHER;
  put_u32(bytes + 79, 3);
  bytes[83] = 0;
  return true;
}

extern "C" void lardon3d_geometric_verifier_fingerprint(
    const Lardon3DGeometricVerifierParameters *p, unsigned char output[32]) {
  if (!output)
    return;
  std::memset(output, 0, 32);
  unsigned char bytes[LARDON3D_GEOMETRIC_VERIFIER_FINGERPRINT_SIZE];
  if (!lardon3d_geometric_verifier_fingerprint_bytes(
          p, LARDON3D_GEOMETRIC_ALGORITHM_USAC_MAGSAC,
          LARDON3D_GEOMETRIC_VERIFIER_VERSION, bytes))
    return;
  (void)digest(bytes, sizeof(bytes), output);
}

extern "C" uint32_t
lardon3d_geometric_verifier_seed(const unsigned char match_sha256[32],
                                 const unsigned char fingerprint[32]) {
  if (!match_sha256 || !fingerprint)
    return 0;
  unsigned char bytes[72];
  std::memcpy(bytes, "L3DGVSE1", 8);
  std::memcpy(bytes + 8, match_sha256, 32);
  std::memcpy(bytes + 40, fingerprint, 32);
  unsigned char hash[32];
  if (!digest(bytes, sizeof(bytes), hash))
    return 0;
  uint32_t value = static_cast<uint32_t>(hash[0]) |
                   static_cast<uint32_t>(hash[1]) << 8U |
                   static_cast<uint32_t>(hash[2]) << 16U |
                   static_cast<uint32_t>(hash[3]) << 24U;
  return value & 0x7fffffffU;
}

extern "C" bool lardon3d_geometric_verifier_canonicalize(double model[9]) {
  if (!model)
    return false;
  double norm = 0.0;
  size_t pivot = 0;
  for (size_t index = 0; index < 9; ++index) {
    if (!std::isfinite(model[index]))
      return false;
    norm = std::hypot(norm, model[index]);
    if (std::fabs(model[index]) > std::fabs(model[pivot]))
      pivot = index;
  }
  if (!std::isfinite(norm) || norm == 0.0)
    return false;
  const double sign = model[pivot] < 0.0 ? -1.0 : 1.0;
  for (size_t index = 0; index < 9; ++index)
    model[index] = normalized_zero(sign * model[index] / norm);
  return true;
}

extern "C" Lardon3DGeometricVerifierResult
lardon3d_geometric_verifier_verify_and_publish(
    const char *project_path, Lardon3DProjectDb *db, uint64_t match_id,
    const Lardon3DGeometricVerifierParameters *p,
    Lardon3DProjectDbGeometricVerificationResult *result, bool *reused) {
  if (!project_path || !db || match_id == 0 || !p || !result || !reused ||
      !lardon3d_geometric_verifier_parameters_valid(p))
    return LARDON3D_GEOMETRIC_VERIFIER_INVALID_ARGUMENT;
  *reused = false;
  unsigned char fingerprint[32];
  lardon3d_geometric_verifier_fingerprint(p, fingerprint);
  Lardon3DProjectDbResult found =
      lardon3d_project_db_find_geometric_verification_result(
          db, match_id, LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL,
          LARDON3D_GEOMETRIC_VERIFIER_VERSION, fingerprint, result);
  if (found == LARDON3D_PROJECT_DB_OK) {
    *reused = true;
    return LARDON3D_GEOMETRIC_VERIFIER_OK;
  }
  if (found != LARDON3D_PROJECT_DB_NOT_FOUND)
    return LARDON3D_GEOMETRIC_VERIFIER_DATABASE_ERROR;
  try {
    Lardon3DProjectDbMatchResult parent;
    if (lardon3d_project_db_load_match_result(db, match_id, &parent) !=
            LARDON3D_PROJECT_DB_OK ||
        parent.result_status != LARDON3D_MATCH_RESULT_STATUS_MATCHED ||
        !parent.has_match_asset)
      return LARDON3D_GEOMETRIC_VERIFIER_NOT_FOUND;
    Lardon3DProjectDbFeatureSet set_a, set_b;
    if (lardon3d_project_db_load_feature_set(
            db, parent.feature_set_id_a, &set_a) != LARDON3D_PROJECT_DB_OK ||
        lardon3d_project_db_load_feature_set(db, parent.feature_set_id_b,
                                             &set_b) != LARDON3D_PROJECT_DB_OK)
      return LARDON3D_GEOMETRIC_VERIFIER_NOT_FOUND;
    char path[4096];
    Lardon3DMatchFileHeader header;
    if (!join_path(path, project_path, parent.match_asset_path) ||
        lardon3d_match_file_validate_asset(
            path, parent.match_asset_sha256, parent.match_asset_size_bytes,
            &header, set_a.feature_set_id, set_b.feature_set_id,
            set_a.feature_count,
            set_b.feature_count) != LARDON3D_MATCH_FILE_OK ||
        header.match_count != parent.match_count)
      return LARDON3D_GEOMETRIC_VERIFIER_CORRUPT;
    std::vector<Lardon3DMatchFileEntry> entries(parent.match_count);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    uint32_t count = 0;
    bool read_ok =
        fd >= 0 &&
        lardon3d_match_file_read(fd, &header, entries.data(), entries.size(),
                                 &count, set_a.feature_set_id,
                                 set_b.feature_set_id, set_a.feature_count,
                                 set_b.feature_count) == LARDON3D_MATCH_FILE_OK;
    if (fd >= 0)
      (void)close(fd);
    if (!read_ok || count != parent.match_count)
      return LARDON3D_GEOMETRIC_VERIFIER_CORRUPT;
    std::vector<Lardon3DFeatureKeypoint> keypoints_a, keypoints_b;
    if (!read_points(project_path, set_a, keypoints_a) ||
        !read_points(project_path, set_b, keypoints_b))
      return LARDON3D_GEOMETRIC_VERIFIER_CORRUPT;
    std::vector<cv::Point2d> points_a, points_b;
    points_a.reserve(count);
    points_b.reserve(count);
    for (const auto &entry : entries) {
      points_a.emplace_back(keypoints_a[entry.feature_index_a].x,
                            keypoints_a[entry.feature_index_a].y);
      points_b.emplace_back(keypoints_b[entry.feature_index_b].x,
                            keypoints_b[entry.feature_index_b].y);
    }
    cv::Mat mask;
    cv::Mat model;
    if (count >= LARDON3D_GEOMETRIC_VERIFIER_MINIMUM_MATCHES) {
      uint32_t seed = lardon3d_geometric_verifier_seed(
          parent.match_asset_sha256, fingerprint);
      if (!estimate_fundamental(points_a, points_b, p, seed, model, mask))
        return LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR;
    }
    std::vector<unsigned char> bitset((count + 7U) / 8U, 0);
    uint32_t inliers = 0;
    double canonical[9] = {};
    if (!model.empty()) {
      if (model.rows != 3 || model.cols != 3 || model.channels() != 1 ||
          model.total() != 9 || mask.type() != CV_8U || mask.total() != count)
        return LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR;
      for (uint32_t index = 0; index < count; ++index) {
        unsigned char value = mask.ptr<unsigned char>()[index];
        if (value > 1)
          return LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR;
        if (value) {
          bitset[index / 8U] |= static_cast<unsigned char>(1U << (index % 8U));
          ++inliers;
        }
      }
      cv::Mat doubles;
      model.convertTo(doubles, CV_64F);
      std::memcpy(canonical, doubles.ptr<double>(), sizeof(canonical));
      if (!lardon3d_geometric_verifier_canonicalize(canonical))
        return LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR;
    } else if (!mask.empty()) {
      if (mask.type() != CV_8U || mask.total() != count)
        return LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR;
      for (uint32_t index = 0; index < count; ++index)
        if (mask.ptr<unsigned char>()[index] != 0)
          return LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR;
    }
    bool accepted = !model.empty() && inliers >= p->min_inlier_count &&
                    static_cast<double>(inliers) / count >= p->min_inlier_ratio;
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (now < 0)
      return LARDON3D_GEOMETRIC_VERIFIER_DATABASE_ERROR;
#ifdef LARDON3D_GEOMETRIC_VERIFIER_TESTING
    const char *publication_failure =
        std::getenv("LARDON3D_TEST_GEOMETRIC_PUBLICATION_FAILURE");
    if (publication_failure && std::strcmp(publication_failure, "1") == 0)
      return LARDON3D_GEOMETRIC_VERIFIER_DATABASE_ERROR;
#endif
    Lardon3DProjectDbResult created =
        lardon3d_project_db_create_geometric_verification_result(
            db, match_id, LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL,
            LARDON3D_GEOMETRIC_VERIFIER_VERSION, fingerprint,
            accepted ? LARDON3D_GEOMETRIC_VERIFIED
                     : LARDON3D_GEOMETRIC_REJECTED,
            inliers, bitset.data(), bitset.size(),
            accepted ? canonical : nullptr, now, result);
    if (created == LARDON3D_PROJECT_DB_CONSTRAINT) {
      Lardon3DProjectDbResult concurrent =
          lardon3d_project_db_find_geometric_verification_result(
              db, match_id, LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL,
              LARDON3D_GEOMETRIC_VERIFIER_VERSION, fingerprint, result);
      if (concurrent == LARDON3D_PROJECT_DB_OK) {
        *reused = true;
        return LARDON3D_GEOMETRIC_VERIFIER_OK;
      }
      return LARDON3D_GEOMETRIC_VERIFIER_DATABASE_ERROR;
    }
    return created == LARDON3D_PROJECT_DB_OK
               ? LARDON3D_GEOMETRIC_VERIFIER_OK
               : LARDON3D_GEOMETRIC_VERIFIER_DATABASE_ERROR;
  } catch (const std::bad_alloc &) {
    return LARDON3D_GEOMETRIC_VERIFIER_OUT_OF_MEMORY;
  } catch (const cv::Exception &) {
    return LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR;
  } catch (...) {
    return LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR;
  }
}
