#include <lardon3d/acquisition_pairing.h>

#include <libexif/exif-data.h>
#include <libexif/exif-utils.h>
#include <libraw/libraw.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

namespace {

bool has_field(const Lardon3DAcquisitionMetadata &metadata, uint32_t field) {
    return (metadata.present_fields & field) != 0u;
}

struct BoundedText {
    const char *data;
    size_t length;
    bool available;
};

BoundedText bounded_text(const char *text, size_t capacity) {
    const void *terminator = std::memchr(text, '\0', capacity);
    if (terminator == nullptr || terminator == text) {
        return {text, 0u, false};
    }
    const size_t length = static_cast<size_t>(static_cast<const char *>(terminator) - text);
    return {text, length, true};
}

bool same_text(const char *left, size_t left_capacity, const char *right,
               size_t right_capacity) {
    const BoundedText left_text = bounded_text(left, left_capacity);
    const BoundedText right_text = bounded_text(right, right_capacity);
    return left_text.available && right_text.available &&
           left_text.length == right_text.length &&
           std::memcmp(left_text.data, right_text.data, left_text.length) == 0;
}

bool different_text(const char *left, size_t left_capacity, const char *right,
                    size_t right_capacity) {
    const BoundedText left_text = bounded_text(left, left_capacity);
    const BoundedText right_text = bounded_text(right, right_capacity);
    return left_text.available && right_text.available &&
           (left_text.length != right_text.length ||
            std::memcmp(left_text.data, right_text.data, left_text.length) != 0);
}

bool copy_padded_text(char *destination, size_t capacity, const unsigned char *source,
                      size_t source_size) {
    size_t length = source_size;
    while (length > 0u && (source[length - 1u] == '\0' || source[length - 1u] == ' ')) {
        --length;
    }
    if (length >= capacity) {
        destination[0] = '\0';
        return false;
    }
    if (length > 0u) {
        std::memcpy(destination, source, length);
    }
    destination[length] = '\0';
    return length != 0u;
}

void extract_raw_fixed_text(const libraw_data_t &raw,
                            Lardon3DAcquisitionMetadata &metadata) {
    if (copy_padded_text(metadata.make, sizeof(metadata.make),
                         reinterpret_cast<const unsigned char *>(raw.idata.make),
                         sizeof(raw.idata.make))) {
        metadata.present_fields |= LARDON3D_ACQUISITION_FIELD_MAKE;
    }
    if (copy_padded_text(metadata.model, sizeof(metadata.model),
                         reinterpret_cast<const unsigned char *>(raw.idata.model),
                         sizeof(raw.idata.model))) {
        metadata.present_fields |= LARDON3D_ACQUISITION_FIELD_MODEL;
    }
    if (copy_padded_text(metadata.body_serial, sizeof(metadata.body_serial),
                         reinterpret_cast<const unsigned char *>(raw.shootinginfo.BodySerial),
                         sizeof(raw.shootinginfo.BodySerial))) {
        metadata.present_fields |= LARDON3D_ACQUISITION_FIELD_BODY_SERIAL;
    }
}

void extract_raw_image_unique_id(const libraw_data_t &raw,
                                 Lardon3DAcquisitionMetadata &metadata) {
    if (copy_padded_text(metadata.image_unique_id, sizeof(metadata.image_unique_id),
                         reinterpret_cast<const unsigned char *>(raw.color.ImageUniqueID),
                         sizeof(raw.color.ImageUniqueID))) {
        metadata.present_fields |= LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID;
    }
}

ExifEntry *find_entry(ExifData *data, ExifTag tag) {
    for (unsigned int index = 0u; index < EXIF_IFD_COUNT; ++index) {
        ExifEntry *entry = exif_content_get_entry(data->ifd[index], tag);
        if (entry != nullptr) {
            return entry;
        }
    }
    return nullptr;
}

void extract_ascii(ExifData *data, ExifTag tag, char *destination, size_t capacity,
                   uint32_t field, Lardon3DAcquisitionMetadata &metadata) {
    ExifEntry *entry = find_entry(data, tag);
    if (entry == nullptr || entry->data == nullptr || entry->size == 0u ||
        entry->format != EXIF_FORMAT_ASCII) {
        return;
    }
    if (copy_padded_text(destination, capacity, entry->data, entry->size)) {
        metadata.present_fields |= field;
    }
}

bool extract_unsigned(ExifData *data, ExifTag tag, uint32_t &value) {
    ExifEntry *entry = find_entry(data, tag);
    if (entry == nullptr || entry->data == nullptr) {
        return false;
    }
    const ExifByteOrder order = exif_data_get_byte_order(data);
    if (entry->format == EXIF_FORMAT_SHORT && entry->size >= 2u) {
        value = exif_get_short(entry->data, order);
        return true;
    }
    if (entry->format == EXIF_FORMAT_LONG && entry->size >= 4u) {
        value = exif_get_long(entry->data, order);
        return true;
    }
    return false;
}

enum class JpegValidationResult {
    valid,
    corrupt,
    io_error,
};

constexpr size_t JPEG_MAX_CONTAINER_IMAGES = 8u;

// Structural validation deliberately parses JPEG framing without decoding
// pixels, so metadata acceptance does not depend on decoder permissiveness.
bool read_byte(FILE *file, unsigned char &value) {
    const int byte = std::fgetc(file);
    if (byte == EOF) {
        return false;
    }
    value = static_cast<unsigned char>(byte);
    return true;
}

bool skip_bytes(FILE *file, uint16_t count) {
    unsigned char byte = 0u;
    for (uint16_t index = 0u; index < count; ++index) {
        if (!read_byte(file, byte)) {
            return false;
        }
    }
    return true;
}

bool marker_has_length(unsigned char marker) {
    return (marker >= 0xc0u && marker <= 0xc7u) ||
           (marker >= 0xc9u && marker <= 0xcfu) ||
           (marker >= 0xdau && marker <= 0xdfu) ||
           (marker >= 0xe0u && marker <= 0xfeu);
}

bool read_marker(FILE *file, unsigned char &marker) {
    unsigned char byte = 0u;
    if (!read_byte(file, byte) || byte != 0xffu) {
        return false;
    }
    do {
        if (!read_byte(file, marker)) {
            return false;
        }
    } while (marker == 0xffu);
    return marker != 0x00u;
}

bool skip_marker_payload(FILE *file, unsigned char marker, bool &has_mpf_app2) {
    unsigned char high = 0u;
    unsigned char low = 0u;
    if (!read_byte(file, high) || !read_byte(file, low)) {
        return false;
    }
    const uint16_t length = static_cast<uint16_t>((static_cast<uint16_t>(high) << 8u) |
                                                  static_cast<uint16_t>(low));
    if (marker == 0xdau) {
        unsigned char component_count = 0u;
        if (length < 3u || !read_byte(file, component_count) || component_count == 0u ||
            component_count > 4u) {
            return false;
        }
        const uint16_t expected_length =
            static_cast<uint16_t>(6u + 2u * static_cast<uint16_t>(component_count));
        return length == expected_length &&
               skip_bytes(file, static_cast<uint16_t>(length - 3u));
    }
    if (marker == 0xdcu) {
        unsigned char line_count_high = 0u;
        unsigned char line_count_low = 0u;
        return length == 4u && read_byte(file, line_count_high) &&
               read_byte(file, line_count_low) &&
               (line_count_high != 0u || line_count_low != 0u);
    }
    if (marker == 0xe2u && length >= 6u) {
        unsigned char identifier[4] = {};
        for (unsigned char &byte : identifier) {
            if (!read_byte(file, byte)) {
                return false;
            }
        }
        if (std::memcmp(identifier, "MPF\0", sizeof(identifier)) == 0) {
            has_mpf_app2 = true;
        }
        return skip_bytes(file, static_cast<uint16_t>(length - 6u));
    }
    return length >= 2u && skip_bytes(file, static_cast<uint16_t>(length - 2u));
}

JpegValidationResult validate_jpeg_image(FILE *file, bool soi_already_consumed,
                                         bool &has_mpf_app2) {
    has_mpf_app2 = false;
    if (!soi_already_consumed) {
        unsigned char first = 0u;
        unsigned char second = 0u;
        if (!read_byte(file, first) || !read_byte(file, second) || first != 0xffu ||
            second != 0xd8u) {
            return std::ferror(file) != 0 ? JpegValidationResult::io_error
                                         : JpegValidationResult::corrupt;
        }
    }

    // Entropy bytes are not marker payload: FF 00 is stuffing and RSTn stays
    // within the scan.  Only a real following marker resumes marker parsing.
    bool in_entropy = false;
    for (;;) {
        unsigned char marker = 0u;
        bool marker_from_entropy = false;
        if (in_entropy) {
            unsigned char byte = 0u;
            do {
                if (!read_byte(file, byte)) {
                    return std::ferror(file) != 0 ? JpegValidationResult::io_error
                                                 : JpegValidationResult::corrupt;
                }
            } while (byte != 0xffu);
            do {
                if (!read_byte(file, marker)) {
                    return std::ferror(file) != 0 ? JpegValidationResult::io_error
                                                 : JpegValidationResult::corrupt;
                }
            } while (marker == 0xffu);
            if (marker == 0x00u || (marker >= 0xd0u && marker <= 0xd7u)) {
                continue;
            }
            marker_from_entropy = true;
            in_entropy = false;
        } else if (!read_marker(file, marker)) {
            return std::ferror(file) != 0 ? JpegValidationResult::io_error
                                         : JpegValidationResult::corrupt;
        }

        if (marker == 0xd9u) {
            return JpegValidationResult::valid;
        }
        if (marker == 0xd8u) {
            return JpegValidationResult::corrupt;
        }
        if (marker == 0x01u || (marker >= 0xd0u && marker <= 0xd7u)) {
            continue;
        }
        if (!marker_has_length(marker) ||
            !skip_marker_payload(file, marker, has_mpf_app2)) {
            return std::ferror(file) != 0 ? JpegValidationResult::io_error
                                         : JpegValidationResult::corrupt;
        }
        if (marker == 0xdau || (marker == 0xdcu && marker_from_entropy)) {
            in_entropy = true;
        }
    }
}

JpegValidationResult validate_jpeg_structure(FILE *file) {
    // Secondary JPEGs are allowed only in the bounded private container form:
    // primary APP2 contains MPF\0, every image reaches EOI structurally, and
    // inter-image/final padding is zero only.  No image pixels are decoded.
    bool primary_has_mpf = false;
    for (size_t image_index = 0u; image_index < JPEG_MAX_CONTAINER_IMAGES; ++image_index) {
        bool image_has_mpf = false;
        const JpegValidationResult image_result =
            validate_jpeg_image(file, image_index != 0u, image_has_mpf);
        if (image_result != JpegValidationResult::valid) {
            return image_result;
        }
        if (image_index == 0u) {
            primary_has_mpf = image_has_mpf;
        }

        int byte = EOF;
        do {
            byte = std::fgetc(file);
        } while (byte == 0);
        if (byte == EOF) {
            return std::ferror(file) != 0 ? JpegValidationResult::io_error
                                         : JpegValidationResult::valid;
        }
        if (!primary_has_mpf || byte != 0xff || std::fgetc(file) != 0xd8) {
            return std::ferror(file) != 0 ? JpegValidationResult::io_error
                                         : JpegValidationResult::corrupt;
        }
    }
    return JpegValidationResult::corrupt;
}

Lardon3DAcquisitionResult extract_jpeg(const char *path,
                                       Lardon3DAcquisitionMetadata &metadata) {
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        return errno == ENOENT || errno == EACCES ? LARDON3D_ACQUISITION_IO_ERROR
                                                  : LARDON3D_ACQUISITION_IO_ERROR;
    }
    const JpegValidationResult validation = validate_jpeg_structure(file);
    const bool close_failed = std::fclose(file) != 0;
    if (validation == JpegValidationResult::io_error || close_failed) {
        return LARDON3D_ACQUISITION_IO_ERROR;
    }
    if (validation != JpegValidationResult::valid) {
        return LARDON3D_ACQUISITION_CORRUPT_SOURCE;
    }

    ExifData *data = exif_data_new_from_file(path);
    if (data == nullptr) {
        return LARDON3D_ACQUISITION_METADATA_UNAVAILABLE;
    }
    metadata.source_kind = LARDON3D_ACQUISITION_SOURCE_JPEG;
    extract_ascii(data, EXIF_TAG_MAKE, metadata.make, sizeof(metadata.make),
                  LARDON3D_ACQUISITION_FIELD_MAKE, metadata);
    extract_ascii(data, EXIF_TAG_MODEL, metadata.model, sizeof(metadata.model),
                  LARDON3D_ACQUISITION_FIELD_MODEL, metadata);
    extract_ascii(data, EXIF_TAG_BODY_SERIAL_NUMBER, metadata.body_serial,
                  sizeof(metadata.body_serial), LARDON3D_ACQUISITION_FIELD_BODY_SERIAL,
                  metadata);
    extract_ascii(data, EXIF_TAG_DATE_TIME_ORIGINAL, metadata.datetime_original,
                  sizeof(metadata.datetime_original),
                  LARDON3D_ACQUISITION_FIELD_DATETIME_ORIGINAL, metadata);
    extract_ascii(data, EXIF_TAG_SUB_SEC_TIME_ORIGINAL, metadata.subsec_original,
                  sizeof(metadata.subsec_original), LARDON3D_ACQUISITION_FIELD_SUBSEC_ORIGINAL,
                  metadata);
    extract_ascii(data, EXIF_TAG_OFFSET_TIME_ORIGINAL, metadata.offset_original,
                  sizeof(metadata.offset_original), LARDON3D_ACQUISITION_FIELD_OFFSET_ORIGINAL,
                  metadata);
    extract_ascii(data, EXIF_TAG_IMAGE_UNIQUE_ID, metadata.image_unique_id,
                  sizeof(metadata.image_unique_id), LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID,
                  metadata);

    uint32_t width = 0u;
    uint32_t height = 0u;
    if (extract_unsigned(data, EXIF_TAG_PIXEL_X_DIMENSION, width) &&
        extract_unsigned(data, EXIF_TAG_PIXEL_Y_DIMENSION, height) && width != 0u &&
        height != 0u) {
        metadata.width = width;
        metadata.height = height;
        metadata.present_fields |= LARDON3D_ACQUISITION_FIELD_DIMENSIONS;
    }
    uint32_t orientation = 0u;
    if (extract_unsigned(data, EXIF_TAG_ORIENTATION, orientation) && orientation <= UINT16_MAX) {
        metadata.orientation = static_cast<uint16_t>(orientation);
        metadata.present_fields |= LARDON3D_ACQUISITION_FIELD_ORIENTATION;
    }
    exif_data_unref(data);
    return LARDON3D_ACQUISITION_OK;
}

Lardon3DAcquisitionResult extract_raw(const char *path,
                                      Lardon3DAcquisitionMetadata &metadata) {
    libraw_data_t *raw = libraw_init(0u);
    if (raw == nullptr) {
        return LARDON3D_ACQUISITION_INTERNAL_ERROR;
    }
    const int status = libraw_open_file(raw, path);
    if (status != LIBRAW_SUCCESS) {
        libraw_close(raw);
        if (status == LIBRAW_FILE_UNSUPPORTED || status == LIBRAW_DATA_ERROR ||
            status == LIBRAW_IO_ERROR) {
            return LARDON3D_ACQUISITION_UNSUPPORTED_FORMAT;
        }
        return LARDON3D_ACQUISITION_CORRUPT_SOURCE;
    }
    metadata.source_kind = LARDON3D_ACQUISITION_SOURCE_RAW;
    extract_raw_fixed_text(*raw, metadata);
    extract_raw_image_unique_id(*raw, metadata);
    if (raw->other.timestamp > 0) {
        std::tm time_parts = {};
        if (gmtime_r(&raw->other.timestamp, &time_parts) != nullptr &&
            std::strftime(metadata.datetime_original, sizeof(metadata.datetime_original),
                          "%Y:%m:%d %H:%M:%S", &time_parts) != 0u) {
            metadata.present_fields |= LARDON3D_ACQUISITION_FIELD_DATETIME_ORIGINAL;
        }
    }
    if (raw->sizes.width != 0u && raw->sizes.height != 0u) {
        metadata.width = raw->sizes.width;
        metadata.height = raw->sizes.height;
        metadata.present_fields |= LARDON3D_ACQUISITION_FIELD_DIMENSIONS;
    }
    if (raw->sizes.flip >= 0 && raw->sizes.flip <= UINT16_MAX) {
        metadata.orientation = static_cast<uint16_t>(raw->sizes.flip);
        metadata.present_fields |= LARDON3D_ACQUISITION_FIELD_ORIENTATION;
    }
    libraw_close(raw);
    return LARDON3D_ACQUISITION_OK;
}

void record_matching_text(const Lardon3DAcquisitionMetadata &left,
                          const Lardon3DAcquisitionMetadata &right, uint32_t field,
                          const char *left_text, size_t left_capacity,
                          const char *right_text, size_t right_capacity, uint32_t evidence,
                          Lardon3DAcquisitionPairResult &result) {
    if (has_field(left, field) && has_field(right, field) &&
        same_text(left_text, left_capacity, right_text, right_capacity)) {
        result.evidence |= evidence;
    }
}

unsigned evidence_count(uint32_t value) {
    unsigned count = 0u;
    while (value != 0u) {
        count += value & 1u;
        value >>= 1u;
    }
    return count;
}

} // namespace

#ifdef LARDON3D_ACQUISITION_PAIRING_TESTING
extern "C" void lardon3d_acquisition_test_extract_raw_image_unique_id(
    const libraw_data_t *raw, Lardon3DAcquisitionMetadata *metadata) {
    if (raw != nullptr && metadata != nullptr) {
        extract_raw_image_unique_id(*raw, *metadata);
    }
}

extern "C" void lardon3d_acquisition_test_extract_raw_fixed_text(
    const libraw_data_t *raw, Lardon3DAcquisitionMetadata *metadata) {
    if (raw != nullptr && metadata != nullptr) {
        extract_raw_fixed_text(*raw, *metadata);
    }
}
#endif

extern "C" Lardon3DAcquisitionResult lardon3d_acquisition_extract_metadata(
    const char *path, Lardon3DAcquisitionMetadata *metadata) {
    if (path == nullptr || path[0] == '\0' || metadata == nullptr) {
        return LARDON3D_ACQUISITION_INVALID_ARGUMENT;
    }
    try {
        *metadata = {};
        metadata->policy_version = LARDON3D_ACQUISITION_PAIRING_POLICY_VERSION;
        FILE *file = std::fopen(path, "rb");
        if (file == nullptr) {
            return LARDON3D_ACQUISITION_IO_ERROR;
        }
        unsigned char signature[2] = {};
        const size_t bytes_read = std::fread(signature, 1u, sizeof(signature), file);
        const bool read_error = std::ferror(file) != 0;
        std::fclose(file);
        if (read_error) {
            return LARDON3D_ACQUISITION_IO_ERROR;
        }
        if (bytes_read == sizeof(signature) && signature[0] == 0xffu && signature[1] == 0xd8u) {
            return extract_jpeg(path, *metadata);
        }
        return extract_raw(path, *metadata);
    } catch (const std::bad_alloc &) {
        return LARDON3D_ACQUISITION_INTERNAL_ERROR;
    } catch (...) {
        return LARDON3D_ACQUISITION_INTERNAL_ERROR;
    }
}

extern "C" Lardon3DAcquisitionResult lardon3d_acquisition_compare(
    const Lardon3DAcquisitionMetadata *left, const Lardon3DAcquisitionMetadata *right,
    int same_asset, int basename_hint, Lardon3DAcquisitionPairResult *result) {
    if (left == nullptr || right == nullptr || result == nullptr ||
        left->policy_version != LARDON3D_ACQUISITION_PAIRING_POLICY_VERSION ||
        right->policy_version != LARDON3D_ACQUISITION_PAIRING_POLICY_VERSION) {
        return LARDON3D_ACQUISITION_INVALID_ARGUMENT;
    }
    *result = {};
    result->decision = LARDON3D_ACQUISITION_INSUFFICIENT;
    if (basename_hint != 0) {
        result->evidence |= LARDON3D_ACQUISITION_EVIDENCE_BASENAME_HINT;
    }
    record_matching_text(*left, *right, LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID,
                         left->image_unique_id, sizeof(left->image_unique_id),
                         right->image_unique_id, sizeof(right->image_unique_id),
                         LARDON3D_ACQUISITION_EVIDENCE_IMAGE_UNIQUE_ID, *result);
    record_matching_text(*left, *right, LARDON3D_ACQUISITION_FIELD_BODY_SERIAL,
                         left->body_serial, sizeof(left->body_serial), right->body_serial,
                         sizeof(right->body_serial),
                         LARDON3D_ACQUISITION_EVIDENCE_BODY_SERIAL, *result);
    record_matching_text(*left, *right, LARDON3D_ACQUISITION_FIELD_MAKE, left->make,
                         sizeof(left->make), right->make, sizeof(right->make),
                         LARDON3D_ACQUISITION_EVIDENCE_MAKE, *result);
    record_matching_text(*left, *right, LARDON3D_ACQUISITION_FIELD_MODEL, left->model,
                         sizeof(left->model), right->model, sizeof(right->model),
                         LARDON3D_ACQUISITION_EVIDENCE_MODEL, *result);
    record_matching_text(*left, *right, LARDON3D_ACQUISITION_FIELD_DATETIME_ORIGINAL,
                         left->datetime_original, sizeof(left->datetime_original),
                         right->datetime_original, sizeof(right->datetime_original),
                         LARDON3D_ACQUISITION_EVIDENCE_DATETIME_SECOND, *result);
    if (has_field(*left, LARDON3D_ACQUISITION_FIELD_DIMENSIONS) &&
        has_field(*right, LARDON3D_ACQUISITION_FIELD_DIMENSIONS) &&
        left->width == right->width && left->height == right->height) {
        result->evidence |= LARDON3D_ACQUISITION_EVIDENCE_DIMENSIONS;
    }
    if (has_field(*left, LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID) &&
        has_field(*right, LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID) &&
        different_text(left->image_unique_id, sizeof(left->image_unique_id),
                       right->image_unique_id, sizeof(right->image_unique_id))) {
        result->contradictions |= LARDON3D_ACQUISITION_CONTRADICTION_IMAGE_UNIQUE_ID;
    }
    if (has_field(*left, LARDON3D_ACQUISITION_FIELD_BODY_SERIAL) &&
        has_field(*right, LARDON3D_ACQUISITION_FIELD_BODY_SERIAL) &&
        different_text(left->body_serial, sizeof(left->body_serial), right->body_serial,
                       sizeof(right->body_serial))) {
        result->contradictions |= LARDON3D_ACQUISITION_CONTRADICTION_BODY_SERIAL;
    }
    if (same_asset != 0) {
        result->strength = 0u;
        return LARDON3D_ACQUISITION_OK;
    }
    if (result->contradictions != 0u) {
        result->decision = LARDON3D_ACQUISITION_DIFFERENT;
        return LARDON3D_ACQUISITION_OK;
    }
    if ((result->evidence & LARDON3D_ACQUISITION_EVIDENCE_IMAGE_UNIQUE_ID) != 0u) {
        result->decision = LARDON3D_ACQUISITION_SAME_ACQUISITION_STRONG;
        result->strength = 100u;
        return LARDON3D_ACQUISITION_OK;
    }
    const uint32_t metadata_evidence =
        result->evidence & ~LARDON3D_ACQUISITION_EVIDENCE_BASENAME_HINT;
    if (metadata_evidence != 0u) {
        result->decision = LARDON3D_ACQUISITION_SAME_ACQUISITION_CANDIDATE;
        result->strength = evidence_count(metadata_evidence) +
                           ((result->evidence & LARDON3D_ACQUISITION_EVIDENCE_BASENAME_HINT) != 0u
                                ? 1u
                                : 0u);
    }
    return LARDON3D_ACQUISITION_OK;
}

extern "C" Lardon3DAcquisitionResult lardon3d_acquisition_select_candidate(
    const Lardon3DAcquisitionMetadata *source,
    const Lardon3DAcquisitionCandidate *candidates, size_t candidate_count,
    Lardon3DAcquisitionSelection *selection) {
    if (source == nullptr || selection == nullptr ||
        (candidate_count != 0u && candidates == nullptr)) {
        return LARDON3D_ACQUISITION_INVALID_ARGUMENT;
    }
    if (candidate_count > LARDON3D_ACQUISITION_MAX_CANDIDATES) {
        return LARDON3D_ACQUISITION_LIMIT_EXCEEDED;
    }
    *selection = {};
    selection->decision = LARDON3D_ACQUISITION_INSUFFICIENT;
    uint32_t top_strength = 0u;
    for (size_t index = 0u; index < candidate_count; ++index) {
        if (candidates[index].metadata == nullptr) {
            return LARDON3D_ACQUISITION_INVALID_ARGUMENT;
        }
        Lardon3DAcquisitionPairResult pair = {};
        const Lardon3DAcquisitionResult status = lardon3d_acquisition_compare(
            source, candidates[index].metadata, candidates[index].same_asset,
            candidates[index].basename_hint, &pair);
        if (status != LARDON3D_ACQUISITION_OK) {
            return status;
        }
        if (pair.decision != LARDON3D_ACQUISITION_SAME_ACQUISITION_STRONG &&
            pair.decision != LARDON3D_ACQUISITION_SAME_ACQUISITION_CANDIDATE) {
            continue;
        }
        if (pair.strength > top_strength) {
            top_strength = pair.strength;
            selection->selected_candidate_id = candidates[index].candidate_id;
            selection->selected_pair = pair;
            selection->top_candidate_count = 1u;
        } else if (pair.strength == top_strength) {
            ++selection->top_candidate_count;
        }
    }
    if (selection->top_candidate_count == 0u) {
        return LARDON3D_ACQUISITION_OK;
    }
    if (selection->top_candidate_count > 1u) {
        selection->decision = LARDON3D_ACQUISITION_AMBIGUOUS;
        selection->selected_candidate_id = 0u;
        return LARDON3D_ACQUISITION_OK;
    }
    selection->decision = selection->selected_pair.decision;
    return LARDON3D_ACQUISITION_OK;
}
