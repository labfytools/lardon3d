# 10 — Camera Model

## Status

```text
CAMERA_MODEL_V1=FROZEN
```

## Authority

`docs/architecture/sparse_sfm.md` and calibration contracts.

## FROZEN

Sparse SfM v1 is known-calibration only.

The v1 model is binary64 pinhole with zero skew and OpenCV-compatible distortion:

```text
fx, fy, cx, cy
k1, k2, p1, p2
```

or explicit zero distortion.

Image coordinates: top-left origin, +x right, +y down.

Camera frame: x right, y down, z forward.

Pose is world-to-camera:

```text
Xc = R_cw * Xw + t_cw
Cw = -transpose(R_cw) * t_cw
```

Higher-order lens models are outside this FROZEN v1 boundary unless separately authorized and scientifically contracted.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
