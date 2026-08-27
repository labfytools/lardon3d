#include <lardon3d/acquisition_pairing.h>

#include <libexif/exif-data.h>
#include <libraw/libraw.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

extern "C" void lardon3d_acquisition_test_extract_raw_image_unique_id(
    const libraw_data_t *raw, Lardon3DAcquisitionMetadata *metadata);
extern "C" void lardon3d_acquisition_test_extract_raw_fixed_text(
    const libraw_data_t *raw, Lardon3DAcquisitionMetadata *metadata);

Lardon3DAcquisitionMetadata metadata() {
    Lardon3DAcquisitionMetadata value = {};
    value.policy_version = LARDON3D_ACQUISITION_PAIRING_POLICY_VERSION;
    value.source_kind = LARDON3D_ACQUISITION_SOURCE_RAW;
    return value;
}

void set_text(Lardon3DAcquisitionMetadata &value, uint32_t field, char *destination,
              size_t capacity, const char *text) {
    assert(std::strlen(text) < capacity);
    std::strcpy(destination, text);
    value.present_fields |= field;
}

Lardon3DAcquisitionPairResult compare(const Lardon3DAcquisitionMetadata &left,
                                      const Lardon3DAcquisitionMetadata &right,
                                      bool same_asset = false, bool basename_hint = false) {
    Lardon3DAcquisitionPairResult result = {};
    assert(lardon3d_acquisition_compare(&left, &right, same_asset, basename_hint, &result) ==
           LARDON3D_ACQUISITION_OK);
    return result;
}

void test_pairing_policy() {
    auto left = metadata();
    auto right = metadata();
    set_text(left, LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID, left.image_unique_id,
             sizeof(left.image_unique_id), "UID-1");
    set_text(right, LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID, right.image_unique_id,
             sizeof(right.image_unique_id), "UID-1");
    assert(compare(left, right).decision == LARDON3D_ACQUISITION_SAME_ACQUISITION_STRONG);

    auto result = compare(left, right, false, true);
    assert(result.decision == LARDON3D_ACQUISITION_SAME_ACQUISITION_STRONG);
    assert((result.evidence & LARDON3D_ACQUISITION_EVIDENCE_BASENAME_HINT) != 0u);

    auto empty_left = metadata();
    auto empty_right = metadata();
    result = compare(empty_left, empty_right, false, true); /* DSC03350.ARW / DSC03350.JPG */
    assert(result.decision == LARDON3D_ACQUISITION_INSUFFICIENT);
    assert(result.evidence == LARDON3D_ACQUISITION_EVIDENCE_BASENAME_HINT);

    set_text(empty_left, LARDON3D_ACQUISITION_FIELD_DATETIME_ORIGINAL,
             empty_left.datetime_original, sizeof(empty_left.datetime_original),
             "2026:08:27 12:34:56");
    set_text(empty_right, LARDON3D_ACQUISITION_FIELD_DATETIME_ORIGINAL,
             empty_right.datetime_original, sizeof(empty_right.datetime_original),
             "2026:08:27 12:34:56");
    assert(compare(empty_left, empty_right).decision ==
           LARDON3D_ACQUISITION_SAME_ACQUISITION_CANDIDATE);
    set_text(empty_left, LARDON3D_ACQUISITION_FIELD_MODEL, empty_left.model,
             sizeof(empty_left.model), "ILCE-7M4");
    set_text(empty_right, LARDON3D_ACQUISITION_FIELD_MODEL, empty_right.model,
             sizeof(empty_right.model), "ILCE-7M4");
    assert(compare(empty_left, empty_right).decision ==
           LARDON3D_ACQUISITION_SAME_ACQUISITION_CANDIDATE);

    std::strcpy(right.image_unique_id, "UID-2");
    result = compare(left, right);
    assert(result.decision == LARDON3D_ACQUISITION_DIFFERENT);
    assert(result.contradictions ==
           LARDON3D_ACQUISITION_CONTRADICTION_IMAGE_UNIQUE_ID);

    auto serial_left = metadata();
    auto serial_right = metadata();
    set_text(serial_left, LARDON3D_ACQUISITION_FIELD_BODY_SERIAL, serial_left.body_serial,
             sizeof(serial_left.body_serial), "BODY-A");
    set_text(serial_right, LARDON3D_ACQUISITION_FIELD_BODY_SERIAL, serial_right.body_serial,
             sizeof(serial_right.body_serial), "BODY-B");
    assert(compare(serial_left, serial_right).decision == LARDON3D_ACQUISITION_DIFFERENT);
    serial_right = metadata();
    assert(compare(serial_left, serial_right).decision == LARDON3D_ACQUISITION_INSUFFICIENT);

    auto other_device = metadata();
    set_text(other_device, LARDON3D_ACQUISITION_FIELD_BODY_SERIAL, other_device.body_serial,
             sizeof(other_device.body_serial), "BODY-B");
    set_text(other_device, LARDON3D_ACQUISITION_FIELD_DATETIME_ORIGINAL,
             other_device.datetime_original, sizeof(other_device.datetime_original),
             "2026:08:27 12:34:56");
    set_text(serial_left, LARDON3D_ACQUISITION_FIELD_DATETIME_ORIGINAL,
             serial_left.datetime_original, sizeof(serial_left.datetime_original),
             "2026:08:27 12:34:56");
    assert(compare(serial_left, other_device, false, true).decision ==
           LARDON3D_ACQUISITION_DIFFERENT);
    assert(compare(left, left, true, true).decision == LARDON3D_ACQUISITION_INSUFFICIENT);
}

void test_selector() {
    auto source = metadata();
    auto candidate_b = metadata();
    auto candidate_c = metadata();
    set_text(source, LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID, source.image_unique_id,
             sizeof(source.image_unique_id), "SHARED");
    set_text(candidate_b, LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID,
             candidate_b.image_unique_id, sizeof(candidate_b.image_unique_id), "SHARED");
    set_text(candidate_c, LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID,
             candidate_c.image_unique_id, sizeof(candidate_c.image_unique_id), "SHARED");
    Lardon3DAcquisitionCandidate candidates[2] = {
        {20u, &candidate_b, 0u, 0u, {}},
        {30u, &candidate_c, 0u, 0u, {}},
    };
    Lardon3DAcquisitionSelection selection = {};
    assert(lardon3d_acquisition_select_candidate(&source, candidates, 2u, &selection) ==
           LARDON3D_ACQUISITION_OK);
    assert(selection.decision == LARDON3D_ACQUISITION_AMBIGUOUS);
    assert(selection.top_candidate_count == 2u);
    assert(selection.selected_candidate_id == 0u);
    const auto temporary = candidates[0];
    candidates[0] = candidates[1];
    candidates[1] = temporary;
    assert(lardon3d_acquisition_select_candidate(&source, candidates, 2u, &selection) ==
           LARDON3D_ACQUISITION_OK);
    assert(selection.decision == LARDON3D_ACQUISITION_AMBIGUOUS);
    assert(selection.top_candidate_count == 2u);
    assert(selection.selected_candidate_id == 0u);
}

void test_non_terminated_public_text_is_unavailable() {
    auto left = metadata();
    auto right = metadata();
    std::memset(left.image_unique_id, 'X', sizeof(left.image_unique_id));
    std::memset(right.image_unique_id, 'X', sizeof(right.image_unique_id));
    left.present_fields |= LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID;
    right.present_fields |= LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID;

    auto result = compare(left, right);
    assert(result.decision == LARDON3D_ACQUISITION_INSUFFICIENT);
    assert(result.evidence == 0u);
    assert(result.contradictions == 0u);

    std::memcpy(right.image_unique_id, "VALID", 6u);
    result = compare(left, right);
    assert(result.decision == LARDON3D_ACQUISITION_INSUFFICIENT);
    assert(result.evidence == 0u);
    assert(result.contradictions == 0u);
}

void add_ascii(ExifData *data, ExifIfd ifd, ExifTag tag, const char *value) {
    ExifEntry *entry = exif_entry_new();
    assert(entry != nullptr);
    entry->tag = tag;
    entry->format = EXIF_FORMAT_ASCII;
    const size_t value_size = std::strlen(value) + 2u;
    assert(value_size <= UINT32_MAX);
    entry->components = static_cast<unsigned long>(value_size);
    entry->size = static_cast<unsigned int>(value_size);
    entry->data = static_cast<unsigned char *>(std::malloc(entry->size));
    assert(entry->data != nullptr);
    std::memcpy(entry->data, value, std::strlen(value));
    entry->data[entry->size - 2u] = ' ';
    entry->data[entry->size - 1u] = '\0';
    exif_content_add_entry(data->ifd[ifd], entry);
    exif_entry_unref(entry);
}

std::string temporary_path(const char *suffix) {
    char path[] = "/tmp/lardon3d-acquisition-XXXXXX";
    const int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    close(descriptor);
    std::string result(path);
    result += suffix;
    assert(std::rename(path, result.c_str()) == 0);
    return result;
}

void write_exif_jpeg(const std::string &path) {
    ExifData *data = exif_data_new();
    assert(data != nullptr);
    exif_data_set_byte_order(data, EXIF_BYTE_ORDER_INTEL);
    add_ascii(data, EXIF_IFD_0, EXIF_TAG_MAKE, "Sony");
    add_ascii(data, EXIF_IFD_0, EXIF_TAG_MODEL, "ILCE-7M4");
    add_ascii(data, EXIF_IFD_EXIF, EXIF_TAG_DATE_TIME_ORIGINAL, "2026:08:27 12:34:56");
    add_ascii(data, EXIF_IFD_EXIF, EXIF_TAG_SUB_SEC_TIME_ORIGINAL, "123");
    add_ascii(data, EXIF_IFD_EXIF, EXIF_TAG_OFFSET_TIME_ORIGINAL, "+02:00");
    add_ascii(data, EXIF_IFD_EXIF, EXIF_TAG_BODY_SERIAL_NUMBER, "BODY-42");
    add_ascii(data, EXIF_IFD_EXIF, EXIF_TAG_IMAGE_UNIQUE_ID, "IMAGE-99");
    unsigned char *encoded = nullptr;
    unsigned int encoded_size = 0u;
    exif_data_save_data(data, &encoded, &encoded_size);
    assert(encoded != nullptr && encoded_size > 0u && encoded_size <= UINT16_MAX - 2u);
    FILE *file = std::fopen(path.c_str(), "wb");
    assert(file != nullptr);
    const unsigned char soi[] = {0xffu, 0xd8u, 0xffu, 0xe1u};
    const unsigned int segment_size = encoded_size + 2u;
    const unsigned char length[] = {static_cast<unsigned char>(segment_size >> 8u),
                                    static_cast<unsigned char>(segment_size & 0xffu)};
    const unsigned char eoi[] = {0xffu, 0xd9u};
    assert(std::fwrite(soi, 1u, sizeof(soi), file) == sizeof(soi));
    assert(std::fwrite(length, 1u, sizeof(length), file) == sizeof(length));
    assert(std::fwrite(encoded, 1u, encoded_size, file) == encoded_size);
    assert(std::fwrite(eoi, 1u, sizeof(eoi), file) == sizeof(eoi));
    assert(std::fclose(file) == 0);
    std::free(encoded);
    exif_data_unref(data);
}

void test_extraction() {
    const std::string jpeg = temporary_path(".jpg");
    write_exif_jpeg(jpeg);
    Lardon3DAcquisitionMetadata value = {};
    assert(lardon3d_acquisition_extract_metadata(jpeg.c_str(), &value) ==
           LARDON3D_ACQUISITION_OK);
    assert(value.source_kind == LARDON3D_ACQUISITION_SOURCE_JPEG);
    assert(std::strcmp(value.make, "Sony") == 0);
    assert(std::strcmp(value.model, "ILCE-7M4") == 0);
    assert(std::strcmp(value.datetime_original, "2026:08:27 12:34:56") == 0);
    assert(std::strcmp(value.subsec_original, "123") == 0);
    assert(std::strcmp(value.offset_original, "+02:00") == 0);
    assert(std::strcmp(value.body_serial, "BODY-42") == 0);
    assert(std::strcmp(value.image_unique_id, "IMAGE-99") == 0);
    assert(unlink(jpeg.c_str()) == 0);

    const std::string malformed = temporary_path(".jpg");
    FILE *file = std::fopen(malformed.c_str(), "wb");
    assert(file != nullptr);
    const unsigned char bad[] = {0xffu, 0xd8u, 0x00u};
    assert(std::fwrite(bad, 1u, sizeof(bad), file) == sizeof(bad));
    assert(std::fclose(file) == 0);
    assert(lardon3d_acquisition_extract_metadata(malformed.c_str(), &value) ==
           LARDON3D_ACQUISITION_CORRUPT_SOURCE);
    assert(unlink(malformed.c_str()) == 0);

    const std::string unsupported = temporary_path(".bin");
    file = std::fopen(unsupported.c_str(), "wb");
    assert(file != nullptr);
    assert(std::fwrite("not an image", 1u, 12u, file) == 12u);
    assert(std::fclose(file) == 0);
    assert(lardon3d_acquisition_extract_metadata(unsupported.c_str(), &value) ==
           LARDON3D_ACQUISITION_UNSUPPORTED_FORMAT);
    assert(unlink(unsupported.c_str()) == 0);
}

void test_raw_image_unique_id_mapping() {
    libraw_data_t raw = {};
    Lardon3DAcquisitionMetadata value = {};
    std::memcpy(raw.color.ImageUniqueID, "RAW-UID-42 ", 11u);

    lardon3d_acquisition_test_extract_raw_image_unique_id(&raw, &value);

    assert(std::strcmp(value.image_unique_id, "RAW-UID-42") == 0);
    assert((value.present_fields & LARDON3D_ACQUISITION_FIELD_IMAGE_UNIQUE_ID) != 0u);
}

void test_non_terminated_raw_fixed_text_mapping() {
    libraw_data_t raw = {};
    Lardon3DAcquisitionMetadata value = {};
    std::memset(raw.idata.make, 'M', sizeof(raw.idata.make));
    std::memset(raw.idata.model, 'D', sizeof(raw.idata.model));
    std::memset(raw.shootinginfo.BodySerial, 'S', sizeof(raw.shootinginfo.BodySerial));

    lardon3d_acquisition_test_extract_raw_fixed_text(&raw, &value);

    assert(std::strlen(value.make) == sizeof(raw.idata.make));
    assert(std::strlen(value.model) == sizeof(raw.idata.model));
    assert(std::strlen(value.body_serial) == sizeof(raw.shootinginfo.BodySerial));
    assert((value.present_fields & LARDON3D_ACQUISITION_FIELD_MAKE) != 0u);
    assert((value.present_fields & LARDON3D_ACQUISITION_FIELD_MODEL) != 0u);
    assert((value.present_fields & LARDON3D_ACQUISITION_FIELD_BODY_SERIAL) != 0u);
}

} // namespace

int main() {
    test_pairing_policy();
    test_selector();
    test_non_terminated_public_text_is_unavailable();
    test_extraction();
    test_raw_image_unique_id_mapping();
    test_non_terminated_raw_fixed_text_mapping();
    return 0;
}
