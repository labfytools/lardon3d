# Calibration Bootstrap v1

**PASS / FROZEN.** Project DB v22 implements the
immutable selected-execution snapshot, explicit cross-task mappings, representation cursor and
calibration-scope attachment described below. The bounded artifact validator/importer produces explicit
immutable `IMPORTED_TRUSTED` calibrations for the existing known-calibration
Sparse SfM contract. It is not self-calibration inside Sparse SfM and never
makes EXIF a scientific calibration source. Solver execution remains outside this importer and is not
claimed as implemented by this stage.

## Boundary and identity

Sparse SfM continues to consume an explicit Track Set and immutable calibration
scope. Bootstrap artifacts can support a `Lardon3DSparseCalibration`; external
poses, tracks, matches and points are diagnostics only and never become Lardon3D
scientific reconstruction results. Each artifact identifies a calibration
evidence run, not a Capture, Asset, `image_id`, path, basename, Task ID or
external reconstruction. It records ordered representation hashes/dimensions,
solver executable identity/model/configuration, initialization evidence,
optimized parameters and validation diagnostics. Its SHA-256 is the immutable
`IMPORTED_TRUSTED` provenance fingerprint.

## Artifact format v1

The import API borrows at most 600000 artifact bytes and requires their SHA-256 to equal an
independently supplied expected digest before parsing. The format is fixed-width little-endian; binary64
uses IEEE-754 bits, rejects non-finite values and canonicalizes negative zero before calibration
publication. Native structs, padding, locale text and trailing bytes are never accepted.

```text
magic                          8 bytes = ASCII "L3DCALB1"
format_version                 u32 = 1
model_kind                     u32 = PINHOLE(1)
model_version                  u32 = 1
entry_count                    u32, 1..4096 and equal to selected item_count
solver_executable_sha256       32 bytes, nonzero
solver_configuration_sha256    32 bytes, nonzero
initialization_evidence_sha256 32 bytes, nonzero
validation_evidence_sha256     32 bytes, nonzero
for each selected item in immutable item_index order:
    image_id                   u64
    representation_sha256      32 bytes
    width, height              u32, u32
    fx, fy, cx, cy             f64, f64, f64, f64
    k1, k2, p1, p2             f64, f64, f64, f64
    support_images             u32, nonzero
    support_observations       u32, nonzero
    reprojection_rmse_px       finite nonnegative f64
    maximum_parameter_delta    finite nonnegative f64
    validation_flags           u32 = 0x0f
```

The four v1 validation bits attest convergence, non-degenerate support, deterministic stability, and
representative-coordinate equivalence to Lardon3D respectively. The importer requires all four; it does
not silently derive missing evidence or invent a calibration threshold. `image_id` and representation
SHA-256 must exactly match the durable selected item and its image asset. Width/height are artifact-owned
geometry evidence and also enter calibration scientific identity. The artifact contains no external pose,
point, track or match field, so those diagnostics cannot cross the import boundary.

## Exact model requirement

Bootstrap v1 accepts only an exact equivalent of Lardon3D's OpenCV-style
pinhole model with binary64 `fx, fy, cx, cy, k1, k2, p1, p2`:

```text
x_d = x * (1 + k1*r2 + k2*r2*r2) + 2*p1*x*y + p2*(r2 + 2*x*x)
y_d = y * (1 + k1*r2 + k2*r2*r2) + p1*(r2 + 2*y*y) + 2*p2*x*y
u = fx*x_d + cx; v = fy*y_d + cy
```

The imported geometry is exactly the downstream Feature Store geometry. Models
with active extra coefficients are rejected, never truncated. Representative
coordinates must validate origin, axes, pixel convention and projection against
the Lardon3D implementation before import.

## Metadata, representations and failure state

Metadata is initialization/grouping evidence only. Orientation does not identify
a physical camera, though coordinates must match downstream decoding. A6000
calibrates deterministic RAW-derived PNG geometry, never camera-JPEG geometry
without a separately proven exact transform. Candidate A6000 groups use RAW
geometry, lens evidence and exact EXIF focal only as solver inputs; zoom groups
remain separate unless optimization proves equivalence. S21 uses its selected
source JPEG geometry and solver-supported module grouping.

Bootstrap requires convergence, finite values, positive focal lengths, valid
principal points, non-degenerate support, model compatibility and deterministic
stability evidence. Unstable groups are expanded or partitioned; metadata
interpolation and guessed calibration are prohibited. An affected selected image
is `CALIBRATION_UNAVAILABLE`, not a quality rejection or corrupt source.

The real S21 and current A6000 Engine Bay selected campaigns are presently
`CALIBRATION_UNAVAILABLE` because the available evidence is scientifically
non-identifiable for the known-calibration contract. This is neither a source
or quality failure nor a software failure. No pseudo-calibration, metadata
interpolation or inferred import is permitted; real Sparse SfM is consequently
`BLOCKED_BY_KNOWN_CALIBRATION_DATA`. The v22 normal suite passed 53/53, C17
syntax checks and `git diff --check` passed, targeted ASan/UBSan validation
passed, and final review passed. The full ASan/UBSan suite remains qualified by
third-party RADV LSan behavior and is not represented as a repository-wide clean
sanitizer pass. This bootstrap infrastructure is therefore `PASS / FROZEN`; that
lifecycle state applies to the bounded importer and persistence infrastructure,
not to calibration of either real campaign or to real Sparse SfM execution.

A dedicated future physical calibration acquisition, with images and evidence
adequate to establish an exact supported model for each relevant representation
group, is recommended before attempting real known-calibration Sparse SfM.
That acquisition is future field work, not an implemented solver, a change to
the scientific contract, or permission to retrofit the present campaigns.

## Execution relationship and resources

```text
SCIENTIFIC_EXECUTABLE = QUALITY_SELECTED
                    intersection REPRESENTATION_READY
                    intersection CALIBRATION_ASSIGNED
```

Quality selection is snapshotted immutably. A durable path must retain explicit
quality-group → campaign-group → Capture mappings: equal numeric group values
from independent Task requests imply no identity. Source photos remain read-only;
RAW bootstrap development processes one image at a time, with no campaign-wide
decoded cache. Ticket-owned artifacts retain source/representation hashes,
output dimensions, policy fingerprint and solver provenance.

Project DB v22 stores the ordered selected rows and advances representation publication one item
at a time. Each row immutably declares either a Capture-owned SOURCE RAW `asset_id` for S3-B1 or
that the representation is a source image and no RAW identity exists. Creation validates
`capture_source_assets.source_kind` as RAW; later execution never resolves it from path, digest, basename,
request source index or group number. An `image_id` is accepted only through the retained Capture's explicit
`capture_images` relation. The calibration scope is attached only after all representations are
durable and only when that existing immutable scope contains every selected image. These DB
transitions are idempotent exact retries; they do not execute a solver or RAW developer and do not
introduce a scheduler, worker pool, sidecar, or parallel resource owner.

Artifact validation completes before any publication. Calibration and scope creation then reuse the
existing short Project DB transactions. A crash or final attachment conflict may therefore retain only
immutable content-addressed calibration evidence; it cannot make the selected execution `READY`. An exact
retry reuses that evidence and converges through the existing scope attachment. The importer has no Task,
Queue or Governor ownership and holds only two arrays bounded by the selected item count; solver execution
and one-at-a-time RAW development must be coordinated by the established runtime when invoked.
The thin durable `raw.develop` Task provides that S3-B1 execution boundary for one exact
`capture_id`/`source_asset_id` pair. It reuses the existing single-active Queue and Governor, never
advances the selected-execution cursor itself, and retains its published `image_id` before terminal
generic Task progress can become durable. Its reservation owns one CPU thread, one I/O slot and a
conservative 2 GiB working allowance for the bounded 40 MP decoder and publication buffers only for
the callback lifetime; this operational admission bound is not a scientific Capture-count limit.

## Calibration Tooling v1

**PASS / FROZEN.** `calibration_tooling.h` is the bounded production bridge
between a completed external `CALIBRATION_SCIENCE_V1` evidence bundle and this
importer. It does not solve calibration, parse an unbounded user document, own
an acquisition session, or add a persistent schema. The caller supplies a
borrowed manifest bounded to 4,096 views and 4,096 selected image bindings,
with hashes for the target, optical state and four required evidence artifacts.
It declares and checks the frozen ChArUco family, `9 x 7` geometry,
`DICT_5X5_100`, 30.000 mm squares, 21.000 mm markers, 270.000 x 210.000 mm
active area and at least a 30.000 mm white border; the target hash binds the
corresponding immutable physical-evidence record.
Each view records an accepted/rejected decision and rejected views retain a
nonzero rejection reason; accepted statistics alone are evaluated. Each entry
carries the same optical-state manifest hash and remains in the exact selected
item order required by the importer (it is not reordered by image id).
The validator applies every hard Science v1 acceptance rule before allocating
or calling Project DB. The producer writes the exact existing `L3DCALB1` v1
little-endian record into caller-owned storage (at most 600,000 bytes), hashes
it deterministically, and invokes only
`lardon3d_calibration_bootstrap_import(...)`.

Invalid evidence therefore creates no calibration row and never reaches
`READY`. A valid exact retry reuses the frozen importer’s immutable
calibrations, scope and selected-execution attachment. Tooling stops at
`READY`; it never creates a Sparse SfM Task, and it cannot retro-calibrate the
historical S21 campaign.
A higher-level selected-execution coordinator remains separate scope.
