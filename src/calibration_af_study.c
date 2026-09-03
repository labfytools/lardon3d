#include <lardon3d/calibration_af_study.h>

#include <math.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
  kHeaderSize = 72,
  kSampleSize = 232,
  kPairSize = 44,
  kPairFlagSameFocus = 1,
  kPairFlagFitHoldout = 2,
};

static const unsigned char kMagic[8] = {'L','3','D','A','F','S','T','1'};
static const double kProbes[9][2] = {
    {0.0, 0.0},
    {-0.7, 0.0}, {0.7, 0.0}, {0.0, -0.7}, {0.0, 0.7},
    {-0.7, -0.7}, {0.7, -0.7}, {-0.7, 0.7}, {0.7, 0.7},
};

typedef struct {
  size_t original_index;
  size_t token_length;
} CanonicalSample;

typedef struct {
  double center;
  double edge;
  double corner;
  double global;
} PairMetric;

static bool nonzero_digest(const unsigned char value[32]) {
  unsigned char any = 0;
  for (size_t i = 0; i < 32; ++i) any |= value[i];
  return any != 0;
}

static bool bounded_token_length(const char token[128], size_t *length) {
  if (!token || !length) return false;
  for (size_t i = 0; i < 128; ++i) {
    if (token[i] == '\0') {
      if (i == 0) return false;
      *length = i;
      return true;
    }
  }
  return false;
}

static bool finite_parameters(const Lardon3DCalibrationAfStudySample *s,
                              uint32_t width, uint32_t height) {
  const double p[8] = {s->fx,s->fy,s->cx,s->cy,s->k1,s->k2,s->p1,s->p2};
  for (size_t i = 0; i < 8; ++i)
    if (!isfinite(p[i])) return false;
  return s->fx > 0.0 && s->fy > 0.0 && s->cx >= 0.0 && s->cy >= 0.0 &&
         s->cx < (double)width && s->cy < (double)height;
}

static int byte_compare(const unsigned char *a, size_t an,
                        const unsigned char *b, size_t bn) {
  const size_t n = an < bn ? an : bn;
  const int cmp = memcmp(a, b, n);
  if (cmp != 0) return cmp;
  if (an < bn) return -1;
  if (an > bn) return 1;
  return 0;
}

static int canonical_compare(const Lardon3DCalibrationAfStudyInput *input,
                             const CanonicalSample *a,
                             const CanonicalSample *b) {
  const Lardon3DCalibrationAfStudySample *sa = &input->samples[a->original_index];
  const Lardon3DCalibrationAfStudySample *sb = &input->samples[b->original_index];
  int cmp = byte_compare((const unsigned char *)sa->focus_token, a->token_length,
                         (const unsigned char *)sb->focus_token, b->token_length);
  if (cmp != 0) return cmp;
  cmp = memcmp(sa->calibration_evidence_sha256,
               sb->calibration_evidence_sha256, 32);
  if (cmp != 0) return cmp;
  if ((uint32_t)sa->role < (uint32_t)sb->role) return -1;
  if ((uint32_t)sa->role > (uint32_t)sb->role) return 1;
  return 0;
}

static void canonical_sort(const Lardon3DCalibrationAfStudyInput *input,
                           CanonicalSample *values, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    CanonicalSample value = values[i];
    size_t j = i;
    while (j > 0 && canonical_compare(input, &value, &values[j - 1]) < 0) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = value;
  }
}

static void project(const Lardon3DCalibrationAfStudySample *s,
                    double x, double y, double *u, double *v) {
  const double r2 = x*x + y*y;
  const double radial = 1.0 + s->k1*r2 + s->k2*r2*r2;
  const double xd = x*radial + 2.0*s->p1*x*y + s->p2*(r2 + 2.0*x*x);
  const double yd = y*radial + s->p1*(r2 + 2.0*y*y) + 2.0*s->p2*x*y;
  *u = s->fx*xd + s->cx;
  *v = s->fy*yd + s->cy;
}

static bool pair_metric(const Lardon3DCalibrationAfStudySample *a,
                        const Lardon3DCalibrationAfStudySample *b,
                        PairMetric *out) {
  memset(out, 0, sizeof(*out));
  for (size_t i = 0; i < 9; ++i) {
    double au, av, bu, bv;
    project(a, kProbes[i][0], kProbes[i][1], &au, &av);
    project(b, kProbes[i][0], kProbes[i][1], &bu, &bv);
    const double delta = hypot(au - bu, av - bv);
    if (!isfinite(delta)) return false;
    if (i == 0) out->center = delta;
    else if (i <= 4 && delta > out->edge) out->edge = delta;
    else if (i >= 5 && delta > out->corner) out->corner = delta;
    if (delta > out->global) out->global = delta;
  }
  return true;
}

static void put_u32(unsigned char **p, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) (*p)[i] = (unsigned char)(value >> (8u*i));
  *p += 4;
}

static void put_u64(unsigned char **p, uint64_t value) {
  for (size_t i = 0; i < 8; ++i) (*p)[i] = (unsigned char)(value >> (8u*i));
  *p += 8;
}

static void put_f64(unsigned char **p, double value) {
  uint64_t bits = 0;
  if (value == 0.0) value = 0.0;
  memcpy(&bits, &value, sizeof(bits));
  put_u64(p, bits);
}

static bool sha256(const unsigned char *bytes, size_t size, unsigned char out[32]) {
  unsigned int length = 0;
  return EVP_Digest(bytes, size, out, &length, EVP_sha256(), NULL) == 1 && length == 32;
}

Lardon3DCalibrationAfStudyResult lardon3d_calibration_af_study_produce(
    const Lardon3DCalibrationAfStudyInput *input,
    unsigned char *artifact, size_t artifact_capacity, size_t *written,
    unsigned char artifact_sha256[32],
    Lardon3DCalibrationAfStudySummary *summary) {
  if (written) *written = 0;
  if (artifact_sha256) memset(artifact_sha256, 0, 32);
  if (summary) memset(summary, 0, sizeof(*summary));
  if (!input || !artifact || !written || !artifact_sha256 || !summary ||
      !input->samples || input->sample_count < 2 ||
      input->sample_count > LARDON3D_CALIBRATION_AF_STUDY_MAX_SAMPLES ||
      input->width == 0 || input->height == 0 ||
      !nonzero_digest(input->study_context_sha256))
    return LARDON3D_CALIBRATION_AF_STUDY_INVALID_ARGUMENT;

  CanonicalSample canonical[LARDON3D_CALIBRATION_AF_STUDY_MAX_SAMPLES];
  uint32_t fit_count = 0, holdout_count = 0;
  for (size_t i = 0; i < input->sample_count; ++i) {
    const Lardon3DCalibrationAfStudySample *s = &input->samples[i];
    size_t token_length = 0;
    if ((s->role != LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT &&
         s->role != LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT) ||
        !bounded_token_length(s->focus_token, &token_length) ||
        !nonzero_digest(s->calibration_evidence_sha256) ||
        !finite_parameters(s, input->width, input->height))
      return LARDON3D_CALIBRATION_AF_STUDY_INVALID_EVIDENCE;
    canonical[i].original_index = i;
    canonical[i].token_length = token_length;
    if (s->role == LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT) ++fit_count;
    else ++holdout_count;
  }
  canonical_sort(input, canonical, input->sample_count);
  for (size_t i = 1; i < input->sample_count; ++i) {
    const Lardon3DCalibrationAfStudySample *a = &input->samples[canonical[i-1].original_index];
    const Lardon3DCalibrationAfStudySample *b = &input->samples[canonical[i].original_index];
    if (canonical[i-1].token_length == canonical[i].token_length &&
        memcmp(a->focus_token, b->focus_token, canonical[i].token_length) == 0 &&
        memcmp(a->calibration_evidence_sha256, b->calibration_evidence_sha256, 32) == 0)
      return LARDON3D_CALIBRATION_AF_STUDY_INVALID_EVIDENCE;
  }

  const size_t pair_count = input->sample_count * (input->sample_count - 1) / 2;
  if (pair_count > LARDON3D_CALIBRATION_AF_STUDY_MAX_PAIRS)
    return LARDON3D_CALIBRATION_AF_STUDY_CAPACITY;
  const size_t required = (size_t)kHeaderSize + input->sample_count*(size_t)kSampleSize + pair_count*(size_t)kPairSize;
  if (required > LARDON3D_CALIBRATION_AF_STUDY_MAX_ARTIFACT_BYTES || artifact_capacity < required)
    return LARDON3D_CALIBRATION_AF_STUDY_CAPACITY;

  unsigned char *p = artifact;
  memcpy(p, kMagic, 8); p += 8;
  put_u32(&p, LARDON3D_CALIBRATION_AF_STUDY_VERSION);
  put_u32(&p, LARDON3D_CALIBRATION_AF_STUDY_PROBE_MODEL_VERSION);
  memcpy(p, input->study_context_sha256, 32); p += 32;
  put_u32(&p, input->width);
  put_u32(&p, input->height);
  put_u32(&p, (uint32_t)input->sample_count);
  put_u32(&p, (uint32_t)pair_count);
  put_u32(&p, fit_count);
  put_u32(&p, holdout_count);

  for (size_t rank = 0; rank < input->sample_count; ++rank) {
    const CanonicalSample *c = &canonical[rank];
    const Lardon3DCalibrationAfStudySample *s = &input->samples[c->original_index];
    put_u32(&p, (uint32_t)s->role);
    put_u32(&p, (uint32_t)c->token_length);
    memset(p, 0, 128);
    memcpy(p, s->focus_token, c->token_length); p += 128;
    memcpy(p, s->calibration_evidence_sha256, 32); p += 32;
    put_f64(&p, s->fx); put_f64(&p, s->fy); put_f64(&p, s->cx); put_f64(&p, s->cy);
    put_f64(&p, s->k1); put_f64(&p, s->k2); put_f64(&p, s->p1); put_f64(&p, s->p2);
  }

  Lardon3DCalibrationAfStudySummary local_summary = {0};
  local_summary.sample_count = (uint32_t)input->sample_count;
  local_summary.fit_count = fit_count;
  local_summary.holdout_count = holdout_count;
  local_summary.pair_count = (uint32_t)pair_count;

  for (size_t ai = 0; ai < input->sample_count; ++ai) {
    for (size_t bi = ai + 1; bi < input->sample_count; ++bi) {
      const CanonicalSample *ca = &canonical[ai];
      const CanonicalSample *cb = &canonical[bi];
      const Lardon3DCalibrationAfStudySample *a = &input->samples[ca->original_index];
      const Lardon3DCalibrationAfStudySample *b = &input->samples[cb->original_index];
      PairMetric metric;
      if (!pair_metric(a, b, &metric))
        return LARDON3D_CALIBRATION_AF_STUDY_INVALID_EVIDENCE;
      const bool same_focus = ca->token_length == cb->token_length &&
          memcmp(a->focus_token, b->focus_token, ca->token_length) == 0;
      const bool fit_holdout = a->role != b->role;
      uint32_t flags = 0;
      if (same_focus) flags |= kPairFlagSameFocus;
      if (fit_holdout) flags |= kPairFlagFitHoldout;
      put_u32(&p, (uint32_t)ai);
      put_u32(&p, (uint32_t)bi);
      put_u32(&p, flags);
      put_f64(&p, metric.center);
      put_f64(&p, metric.edge);
      put_f64(&p, metric.corner);
      put_f64(&p, metric.global);

      if (same_focus) ++local_summary.same_focus_pair_count;
      else ++local_summary.cross_focus_pair_count;
      if (fit_holdout) ++local_summary.fit_holdout_pair_count;
      if (metric.center > local_summary.all_center_max_px) local_summary.all_center_max_px = metric.center;
      if (metric.edge > local_summary.all_edge_probe_max_px) local_summary.all_edge_probe_max_px = metric.edge;
      if (metric.corner > local_summary.all_corner_probe_max_px) local_summary.all_corner_probe_max_px = metric.corner;
      if (metric.global > local_summary.all_global_probe_max_px) local_summary.all_global_probe_max_px = metric.global;
      if (same_focus && metric.global > local_summary.same_focus_global_probe_max_px)
        local_summary.same_focus_global_probe_max_px = metric.global;
      if (!same_focus && metric.global > local_summary.cross_focus_global_probe_max_px)
        local_summary.cross_focus_global_probe_max_px = metric.global;
      if (fit_holdout && metric.global > local_summary.fit_holdout_global_probe_max_px)
        local_summary.fit_holdout_global_probe_max_px = metric.global;
    }
  }
  if ((size_t)(p - artifact) != required)
    return LARDON3D_CALIBRATION_AF_STUDY_ENCODING_ERROR;
  if (!sha256(artifact, required, artifact_sha256))
    return LARDON3D_CALIBRATION_AF_STUDY_ENCODING_ERROR;
  *summary = local_summary;
  *written = required;
  return LARDON3D_CALIBRATION_AF_STUDY_OK;
}
