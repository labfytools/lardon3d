# Pipeline de reconstruction Lardon3D

## Vision

Lardon3D ne doit plus être décrit simplement comme « dossier de photos → objet 3D ». La vision cible est :

> « ensemble progressif d'observations et de contraintes
> → reconstruction géométrique persistante, enrichissable et versionnable »

Chaque ajout d'images, de mesures ou de documents enrichit la reconstruction
existante sans détruire les résultats antérieurs. L'utilisateur peut
interrompre le pipeline à tout instant, consulter l'état courant via le
viewer, puis reprendre ultérieurement exactement où il s'était arrêté.

---

## Étapes du pipeline

### A. Scan Sets / acquisitions

| Aspect | Description |
|--------|-------------|
| **Définition** | Un *scan set* (ou acquisition) regroupe un ensemble d'images capturées dans un contexte donné : même lieu, même session, même objectif de reconstruction. |
| **Enrichissement progressif** | Un scan set peut être alimenté par vagues successives : images initiales, images de relèvement, images de contrôle. Chaque vague est horodatée et traçable. |
| **Identification stable** | Chaque ScanSet reçoit un `scanset_id` SQLite positif qui ne dépend pas de son nom. |

**Statut :** IMPLEMENTED v1 — identité, nom et horodatages persistants. Les
vagues et relations géométriques entre acquisitions restent planifiées.

---

### B. Image Catalog

| Aspect | Description |
|--------|-------------|
| **Identité stable** | Chaque image possède un `image_id` stable, distinct du nom et de l'asset physique. |
| **Provenance** | Le catalogue enregistre le ScanSet, la date d'import, le nom et le chemin source, ainsi que la tâche productrice éventuelle. |
| **Asset** | Le contenu physique est identifié par `asset_id`, SHA-256, taille et chemin géré. Plusieurs images logiques peuvent partager cet asset. |

**Statut :** IMPLEMENTED v1 — catalogue SQLite paginé et assets
content-addressed. Les états Feature/Matching/Reconstruction restent planifiés.

---

### C. Feature Extraction et Feature Store

| Aspect | Description |
|--------|-------------|
| **Extraction** | ORB coarse, SIFT précis et RootSIFT, chacun avec sa tâche durable. |
| **Métadonnées persistantes** | `FeatureSet` logique et `FeatureAsset` physique content-addressed dans ProjectDb v7. |
| **Données numériques massives** | ORB U8×32 v1 et SIFT/RootSIFT F32×128 v2 restent hors SQLite. |
| **Lecture bornée** | Le reader lit au plus 256 features par plage et ne charge pas tout le fichier. |

**Statut :** IMPLEMENTED v1 — extraction, publication atomique, reprise et reader borné.

**Extension v1A :** ORB reste la passe coarse et l'unique entrée du Visual
Index ORB-LSH. SIFT/RootSIFT F32×128 sont des passes précises indépendantes,
suivies d'une consolidation spatiale intra-image qui ne mélange jamais les
descriptors.

---

### D. Visual Index

| Aspect | Description |
|--------|-------------|
| **Choix basé sur le contenu visuel** | Le *visual index* permet de retrouver rapidement les images visuellement proches d'une image donnée, sans comparaison exhaustive. Structure type : vocabulaire visuel inversé ou similarité locality-sensitive hashing. |
| **Pipeline conceptuel** | Extraction de features globales → construction de l'index → requête par similarité → retour des K plus proches voisins. |
| **Proximité temporelle comme signal secondaire** | Lorsque les images portent un horodatage EXIF, la proximité temporelle sert de signal complémentaire au contenu visuel, mais ne remplace jamais l'analyse visuelle. |

**Statut :** IMPLEMENTED v1 — LSH binaire déterministe, segments immuables,
updates incrémentales et query top-K bornée. Le générateur de paires reste planifié.

---

### E. Candidate Pair Generator

| Aspect | Description |
|--------|-------------|
| **Sources de paires candidates** | (1) Visual index : paires visuellement proches. (2) Proximité temporelle. (3) Scan set commun. (4) Géométrie approximative (si GPS/IMU disponible). |
| **Matching coûteux limité** | Le nombre de paires soumises au matching géométrique (étape F) doit être borné. Le candidate generator filtre et classe pour ne garder que les paires les plus prometteuses. |
| **Persistance** | Les paires candidates sont persistées dans la table `candidate_pairs` (Project DB v8). Ordre canonique : `image_id_a < image_id_b`. Self-pairs interdits. Unicité garantie. |
| **Déterminisme** | Pour mêmes entrées et configuration, le générateur produit les mêmes paires dans le même ordre. |
| **Idempotence** | L'exécution répétée ne crée pas de doublons. |

**Statut :** IMPLEMENTED v1 — génération single-source et batch depuis Visual
Index, persistance, canonicalisation, idempotence et tâche durable.

---

### F. Matching de descripteurs

| Aspect | Description |
|--------|-------------|
| **Matcher v1** | ORB CPU/Vulkan ; SIFT/RootSIFT L2 CPU (Vulkan rejeté) ; k=2 + Lowe ; sans géométrie. |
| **Persistance** | Match Result NO_MATCH/MATCHED et Match File content-addressed validé. |
| **Orchestration** | `matcher.run` traite les Candidate Pairs par pages et lots durables de 1/2/4/8. |

**Statut :** IMPLEMENTED v1 — Matcher, Match Store, reprise idempotente et Task
durable. Le Geometric Verifier consomme désormais ces résultats.
Le Match Result appartient à Project DB v10 et la tâche durable `matcher.run`
à Project DB v11.

### F2. Geometric Verification

Project DB v12 implémente le modèle scientifique persistant. Chaque
résultat appartient à un Match Result `MATCHED`, possède une identité exacte,
un masque compact borné et, si VERIFIED, une matrice fondamentale 3×3 finie.
La publication est atomique et immutable. Project DB v13 ajoute uniquement la
persistance de `geometric_verifier.run` v1. Le calcul Fundamental emploie
USAC/MAGSAC avec configuration, seed et fingerprint déterministes.

**Statut :** IMPLEMENTED v1 — MODEL, VERIFIER ET TASK DURABLE.

---

### G. Tracks

| Aspect | Description |
|--------|-------------|
| **Observation 2D → track → point3D** | Un *track* est une chaîne d'observations 2D cohérentes d'un même point 3D à travers plusieurs images. Chaque observation est un keypoint indexé par image. |
| **Lien avec le catalogue** | Les tracks référenceront les images par `image_id`, jamais par nom ou chemin. |
| **Persistance** | Les tracks sont persistés entre les sessions de traitement. Un track ne peut être détruit que par une action explicite de l'utilisateur. |

**Statut :** COMPLETED/FROZEN — le Track Builder v1 direct et durable est
implémenté dans Project DB v15 (`track_sets`, `tracks`, `track_observations` et
le payload de tâche). Les primitives de géométrie calibrée Gate C sont
implémentées et le noyau Sparse SfM incrémental Gate D est IMPLEMENTED / PASS ;
le Bundle Adjustment final Gate E est PASS / FROZEN ; l'orchestration projet
et tâche Gate F est PASS / FROZEN.
Le modèle de persistance Sparse SfM v16 est gelé après Gate B.

**Sparse SfM Gate A : PASS.** Le contrat géométrique, la stratégie
incremental, la triangulation candidate, le gauge, les conventions de pose,
les limites BA et l'enveloppe matérielle sont documentés dans
`architecture/sparse_sfm.md`. Ses primitives pures calibrées Gate C sont
**IMPLEMENTED / PASS** et son noyau incrémental synchrone en mémoire Gate D est
**IMPLEMENTED / PASS**. Le Bundle Adjustment final par composante Gate E est
**PASS / FROZEN**. L'orchestration Gate F est **PASS / FROZEN**. Le modèle de
reconstruction v16 et ses lecteurs bornés restent ceux de B2 ; le payload de
tâche Gate F est l'ajout v17.

---

### H. Reconstruction incrémentale

La phase H est une étape du pipeline, pas un « Gate H ». Sa version 1 enrichit
scientifiquement un snapshot Sparse SfM publié, sans réexécuter Gate F depuis
zéro. Ses entrées immuables sont l'identité de la reconstruction de base,
l'identité d'un Track Set d'extension et l'identité d'un scope de calibration.
Son identité scientifique est :

```
(base_reconstruction_identity, extension_track_set_identity,
 calibration_scope_identity, incremental_kind, incremental_version,
 parameter_fingerprint)
```

L'enregistrement canonique `L3DHIDV1` est encodé little-endian puis condensé
par SHA-256. Le fingerprint H v1, distinct de F0, encode `L3DHPRM1`, sa version
et les politiques versionnées suivantes : filiation stricte par identité
d'observation `(feature_set_id, feature_index)`, absence de split/merge,
`STRICT_DESCENDANT_REGISTERED_OBSERVATIONS_V1`,
`PRESERVE_BASE_COMPONENT_KEY_V1`, absence de fusion de composantes de base,
politiques scientifiques Gate D v1 pour PnP/triangulation/raffinement, BA
complète Gate E v1 de chaque composante affectée, calibration historique
exacte, préservation du gauge historique, snapshot complet immuable,
ordre canonique et absence de nouvelle composante déconnectée. Matériel,
ressources, tâche, chemins, horodatages et télémétrie sont exclus.
L'enregistrement de fingerprint fait exactement 80 octets ; son vecteur doré
SHA-256 v1 est
`f44a89b23b520481701848ecf175638e82f2c9e25b70a01f3eb767bf28446cd8`.
Après le magic et les deux versions `u32`, ses seize champs `u32` versionnés
sont, dans l'ordre : `STRICT_OBSERVATION_LINEAGE`, `NO_SPLIT`, `NO_MERGE`,
`STRICT_DESCENDANT_REGISTERED_OBSERVATIONS`, `PRESERVE_BASE_COMPONENT_KEY`,
`NO_BASE_COMPONENT_MERGE`, `GATE_D_PNP_REGISTRATION`,
`GATE_D_TRIANGULATION_REFINEMENT`, `FULL_AFFECTED_COMPONENT_BA`,
`GATE_E_POLICY`, `EXACT_HISTORICAL_CALIBRATION`, `HISTORICAL_BASE_GAUGE`,
`COMPLETE_IMMUTABLE_SNAPSHOT`, `CANONICAL_ID_ORDER`,
`NO_DISCONNECTED_COMPONENT` et `TERMINAL_BA_FAILURE`. Chaque valeur vaut 1
en H v1 ; la position identifie la politique sans chaîne dépendante de locale.

La filiation est dérivée par indexation déterministe des observations, jamais
par égalité de `track_id` entre générations. Chaque Track historique publié a
un unique descendant contenant exactement toutes ses observations historiques.
Un historique manquant, dupliqué, scindé, fusionné ou reliant deux composantes
de base invalide toute la mise à jour. Une observation ajoutée à un descendant
historique dont l'image nouvelle reste non enregistrée dans sa composante
invalide également toute la mise à jour ; une image non enregistrée qui ne
porte que des Tracks nouveaux reste admissible et ne crée pas de composante.

Chaque caméra nouvelle est localisée dans exactement une composante historique.
Les composantes de base ne fusionnent pas et gardent leur `component_key`, égal
au plus petit `image_id` historique enregistré. Une caméra géométriquement
enregistrée dont l'ID est inférieur à cette clé rend la mise à jour
incompatible ; elle n'est pas maquillée en rejet géométrique. Les landmarks
existants reçoivent leurs observations descendantes admissibles, les Tracks
nouveaux entièrement ancrés peuvent être triangulés, puis chaque composante
affectée subit une BA complète avec les ancres historiques. Une composante non
affectée est copiée sans modification scientifique.

La sortie est un snapshot complet, autonome et immuable. Le prédécesseur est
une provenance, jamais une dépendance de lecture en chaîne. Publication et
métadonnées H sont atomiques ; toute erreur de filiation, calibration,
compatibilité de clé, géométrie, BA, annulation ou base de données ne publie
rien et ne modifie pas le prédécesseur. Une identité H déjà publiée est
réutilisée avant matérialisation lourde. Une extension sans effet scientifique
retourne explicitement le prédécesseur sans publier une génération factice.
Les métriques globales sont recalculées après la BA sur chaque observation
finalement retenue, avec la projection pixel Gate F. Une profondeur invalide,
un résidu non fini ou un ensemble vide rend la publication impossible.

L'API scientifique publique valide aussi la cohérence complète du snapshot de
base : landmark, Track, caméra, composante et position canonique de chaque
observation doivent correspondre. Son résultat doit être initialisé à zéro ou
avoir été préalablement détruit avant un nouvel appel.

L'orchestration appartient à la tâche durable `incremental_reconstruction.run`
v1, atomique (lot 1..1), sans DAG ni état scientifique intermédiaire durable.
Après redémarrage elle recommence depuis les entrées immuables. Son estimation
opérationnelle H v1 est, avec arithmétique `uint64_t` vérifiée et arrondi au MiB
supérieur :

```
raw = 268435456
    + base_cameras * 131072
    + base_landmarks * 4096
    + base_observations * 1024
    + extension_images * 131072
    + extension_tracks * 4096
    + extension_observations * 1024
```

Elle est CPU, fixe, `min_batch_size=max_batch_size=1`, demande un thread CPU,
zéro slot GPU et un slot IO. Elle est persistée avec la tâche, exclue de
l'identité et admise uniquement par Queue → Governor → Reservation. Pression
mémoire et ressources ne modifient jamais la science.

Project DB v18 ajoute seulement les métadonnées d'identité/prédécesseur H et
le payload de tâche H. Le parent générique conserve le vrai fingerprint de
paramètres et porte l'identité H dans son `derivation_identity` nullable ; les
lignes Gate F gardent ce champ à `NULL` et leur unicité historique. La
géométrie reste un snapshot complet conforme au
modèle historique ; aucun sous-ensemble d'observations, remappage de clé ou
graphe de filiation générique n'est introduit.

**Statut :** PASS / FROZEN — contrat H v1 implémenté et validé. Validation
finale : suite normale 42/42 PASS ; suite ciblée ASan/UBSan/LSan 5/5 PASS ;
revue finale indépendante PASS ; `git diff --check` PASS. Restent différés :
généalogie complexe, split/merge légitime,
fusion de composantes, BA locale, DAG, étapes scientifiques intermédiaires
durables, scratch/SSD, GPU SfM/BA et nouvelles composantes déconnectées.

---

### I. Reconstruction Layers

| Aspect | Description |
|--------|-------------|
| **Conservation de la provenance** | Chaque point 3D, chaque caméra, chaque track conserve la trace de son origine : quel scan set, quelle vague, quelle session. |
| **Consolidation distincte** | La fusion de layers (consolidation) est un processus séparé de l'ajout de données. L'utilisateur décide quand consolider. La consolidation ne détruit pas les layers d'origine. |

**Statut :** PLANNED — aucune implémentation existante.

---

### J. Sources géométriques externes

| Aspect | Description |
|--------|-------------|
| **PHOTO** | Images du scan set (source principale). |
| **MEASUREMENT** | Mesures directes : distances, orientations, coordonnées GPS, nuages de points LiDAR. Intègrent le graphe de contraintes comme edges géométriques supplémentaires. |
| **DOCUMENT** | Plans, relevés, fiches techniques. Métadonnées contextuelles qui enrichissent le projet sans contribuer directement au calcul géométrique. |

**Statut :** PLANNED — aucune implémentation existante.

---

### K. Viewer intégré

| Aspect | Description |
|--------|-------------|
| **Consommateur passif** | Le viewer ne calcule jamais. Il lit les snapshots validés publiés par le pipeline et les affiche. |
| **Isolation fonctionnelle** | Le viewer s'exécute dans un thread dédié ou un processus séparé. Il ne partage aucun buffer mutable avec les workers de calcul. |
| **Reactive** | L'utilisateur voit la reconstruction apparaître progressivement pendant les calculs, sans attendre la fin de l'étape courante. |

**Statut :** PLANNED — le viewer Vulkan séparé n'est pas encore commencé.

---

## Invariants

Ces invariants s'appliquent à toutes les étapes du pipeline :

1. **Chaque étape est indépendante et reprenable.**
   L'exécution peut être interrompue à n'importe quelle frontière de lot
   et reprise sans perte de données.

2. **Résultats atomiques et validés uniquement.**
   Un résultat n'est publié (rendu visible aux étapes suivantes et au
   viewer) que lorsqu'il est entièrement calculé, vérifié et persisté.

3. **Pas de destruction silencieuse des données sources.**
   Les images originales, les features extraites, les tracks et les
   points 3D existants ne jamais supprimés implicitement. Toute
   suppression est une action explicite et traçable.

4. **Le scheduler ne décide jamais des ressources.**
   Seul le Resource Governor arbitre les budgets, les lots et les
   réservations.

5. **Les réservations sont obligatoires.**
   Aucune tâche ne s'exécute sans réservation active préalablement
   accordée par le governor.

6. **ncurses appartient exclusivement au thread principal.**
   Aucun worker ne touche à l'interface TUI.

---

## Diagramme conceptuel

```
Scan Sets (A)
    │
    ▼
Image Catalog (B) ──► Feature Store (C)
                          │
                          ▼
                    Visual Index (D)
                          │
                          ▼
              Candidate Pair Generator (E)
                          │
                          ▼
              Matching / Geometric Verification (F)
                          │
                          ▼
                      Tracks (G)
                          │
                          ▼
              Reconstruction Incrémentale (H)
                     │          │
                     ▼          ▼
           Reconstruction     Sources
             Layers (I)     Externes (J)
                     │          │
                     └────┬─────┘
                          ▼
                  Viewer Intégré (K)
```

---

## Statut du pipeline

Import, Image Catalog, Feature Extraction, Feature Store, Visual Index,
Candidate Pair, Matching v1, Geometric Verification, Track Model/Builder v1
and Sparse SfM Gate C geometry are **IMPLEMENTED**. The synchronous in-memory
incremental Sparse SfM Gate D core is **IMPLEMENTED / PASS**, and final
per-component Gate E BA is **PASS / FROZEN**. Gate F orchestration and Gate G
Governor integration are **PASS / FROZEN**. Phase H v1 is **PASS / FROZEN**.
MVS, mesh, texturing and viewer remain **PLANNED**.

Ce document décrit la vision architecturale cible du pipeline de
reconstruction. Les modules listés ici ne sont pas tous implémentés.
L'implémentation suit la feuille de route canonique de `docs/roadmap/roadmap.md`
et progresse par phases respectant les invariants d'indépendance et de reprise.
