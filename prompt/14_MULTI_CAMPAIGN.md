# 14 — Multi-Campaign Reconstruction

## Status

```text
MULTI_CAMPAIGN_REGISTRATION=PLANNED
MULTI_CAMPAIGN_FUSION=PLANNED
RAW_PROJECT_MERGE_WITHOUT_REGISTRATION=REJECTED
```

## Authority

`docs/product/product_definition.md`, Phase H documentation and reconstruction architecture.

## FROZEN

Phase H v1 is used only when its FROZEN lineage prerequisites are actually satisfied. It is not a generic merger of independent campaigns.

## REQUIRED_PRODUCT_TARGET

Independent campaigns such as separate S21 and A6000 reconstructions require:

```text
independent reconstruction A
+
independent reconstruction B
-> explicit registration
-> durable transform
-> registration quality/provenance
-> explicit accepted alignment
-> fusion/consolidation
```

Use rigid or similarity transform according to explicit scale knowledge.

Campaign contribution and provenance must remain inspectable after alignment and fusion.

Registration may use automatic overlap evidence and may offer explicit manual control-point assistance as a fallback. The registration algorithm, acceptance thresholds and quality policy are future science and must be versioned/validated before implementation.

## REJECTED

Naively combining independent Feature Sets, Tracks or reconstructions merely because they depict the same object.

## Canonical-reference rule

When implementation enters this area, read the cited canonical document in full before editing. If this prompt summary and the canonical document appear to conflict, do not silently choose the shorter text: determine whether this file is stale or whether a real contract contradiction exists, then apply the STOP rules where necessary.
