# Photo Quality Triage / Acquisition Selection

**PASS / FROZEN.** This non-destructive recommendation step runs
after acquisition discovery/minimal association and before materialization or features.
It consumes existing campaign groups and never treats path, basename, SHA-256, Task ID,
or group ID as Capture, Asset, or image identity.

Metrics v1 validates JPEG structure with constant memory and a 64 MiB byte-work ceiling
covering every byte consumed or skipped, including marker payloads and entropy data.
Exceeding that parser-resource ceiling is a decode error; it is an operational protection,
not a JPEG image-size or scientific dataset limit. A structurally valid proxy above the operational
8192-pixel maximum-dimension decode ceiling returns `UNAVAILABLE + SUSPECT`, not a decode
error or REJECT, then accepted input uses OpenCV JPEG 1/2, 1/4, or 1/8 grayscale
reduction so the retained raster is at most 1024 pixels on its longest edge. This ceiling
only makes a proxy metric unavailable; it is not a campaign/source-count or scientific
dataset-size limit. The implementation records raw/normalized Laplacian sharpness,
black/white clipping, raw/normalized contrast, low-texture fraction, and stable reason
codes. Confirmed RAW+JPEG uses JPEG; RAW-only returns
`METRIC_UNAVAILABLE_REQUIRES_JPEG_PROXY` without RAW development.

Recommendations are GOOD, SUSPECT, or REJECT. Default selection includes only GOOD.
Human NONE/INCLUDE/EXCLUDE override is separate and never rewrites measured metrics.

La table de décision exacte Metrics v1 est :

| Mesure/politique | Valeur exacte | Effet |
|---|---:|---|
| Netteté normalisée, très faible | `< 0.00018` | `REJECT`, raison `SHARPNESS_VERY_LOW` |
| Netteté normalisée, faible | `< 0.00045` | `SUSPECT`, raison `SHARPNESS_LOW` |
| Échantillon noir / blanc | `<= 5` / `>= 250` | Définit les fractions de clipping |
| Clipping blanc suspect / sévère | `> 0.08` / `> 0.20` | `SUSPECT` / `REJECT` |
| Clipping noir suspect / sévère | `> 0.15` / `> 0.35` | `SUSPECT` / `REJECT` |
| Contraste normalisé faible | `< 0.025` | `SUSPECT` |
| Magnitude Sobel de basse texture | `< 0.035` | Classe un pixel comme basse texture |
| Fraction de basse texture | `> 0.985` | `SUSPECT` |
| Aucun signal sévère ou suspect | — | `GOOD` |
| Au moins un signal suspect, aucun sévère | — | `SUSPECT` |
| Au moins un signal sévère | — | `REJECT` |
| Plus grand axe JPEG proxy | `8192` maximum | Au-delà : `UNAVAILABLE + SUSPECT` |
| Plus grand axe retenu pour analyse | `1024` maximum | Borne opérationnelle mémoire Metrics v1 |

Les cutoffs de clipping conservent le prototype autorisé `scan3d/tri_photos.py`
(pourcentages 8/20 et 15/35 convertis en fractions). L'ensemble de la politique est une
calibration d'ingénierie déterministe validée par fixtures synthétiques et observations
réelles Sony A6000 et Samsung S21 ; ce n'est pas une science universelle de validité
d'image. Modifier une valeur ou la correspondance GOOD/SUSPECT/REJECT exige une nouvelle
version de politique/metrics et invalide les fingerprints ou caches qui incluent cette
politique ; des lignes v1 existantes ne doivent jamais être réinterprétées silencieusement.
La recommandation et l'override restent révisables et non destructifs.

Project DB v21 additively stores an immutable typed request and results keyed by the
canonical campaign-plan `group_id` in 1..N. `next_group_id` is that same one-based identity:
it starts at 1, advances from completed group `k` to `k+1`, and equals `N+1` at completion.
Only the executor's private `group_index = group_id - 1` is zero-based. Result plus cursor is
atomic and precedes generic Task checkpoint progress. The
Governor estimate charges actual retained Task context capacity plus a 20 MiB one-group
analysis allowance covering the 1024x1024 grayscale raster, `CV_32F` normalized/Laplacian/
gradient buffers, masks, and allocator margin. Decode and analysis buffers belong to one
group and are promptly released. Recovery temporarily uses the existing bounded maximum
codec and source arrays before Queue admission, then releases them; they do not live through
the admitted execution. Execution reuses
Task/Queue/Resource Governor and `sequence_break`; no parallel runtime is introduced.
