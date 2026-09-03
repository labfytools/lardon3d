# 16 — Mesh, Refinement, Texture and Export

## Status

```text
MESH_TEXTURE_EXPORT=PLANNED
```

## Authority

`docs/product/product_definition.md` and future scoped architecture contracts created before implementation.

## REQUIRED_PRODUCT_TARGET

```text
Dense
-> mesh
-> refinement
-> texturing
-> consolidation
-> export
```

Every stage publishes an explicit immutable generation with provenance.

Minimum target formats:

```text
point cloud: PLY
mesh: PLY, OBJ
textured mesh: OBJ + MTL + texture assets
portable viewer/export: GLB/GLTF
STL: optional
```

Export must preserve scale truth. Never label arbitrary monocular gauge units as millimetres or metres.

Exports must carry a manifest or sidecar sufficient to identify the source reconstruction, mesh/texture generation and scale status.

Consolidation selects an explicit user-facing result from immutable upstream generations. It does not delete those generations; selection remains explicit and reversible until deliberate cleanup.

Intermediate external-tool artifacts must not become authoritative until validated and atomically published.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
