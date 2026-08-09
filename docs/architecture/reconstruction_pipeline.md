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

**Statut :** PARTIAL — le Track Model v1 (persistance) est implémenté dans
Project DB v15 (`track_sets`, `tracks`, `track_observations`). Le Track
Builder, la triangulation et le Sparse SfM restent PLANNED.

---

### H. Reconstruction incrémentale

| Aspect | Description |
|--------|-------------|
| **Indexation** | À chaque vague d'images, la reconstruction existante est indexée (positions approximatives des points 3D, orientations des caméras). |
| **Comparaison aux acquisitions précédentes** | Les nouvelles images sont comparées à la reconstruction existante : localisation des caméras, triangulation de nouveaux points, mise à jour des tracks existants. |
| **Enrichissement local** | Seules les régions couvertes par les nouvelles images sont recalculées. Le reste de la reconstruction reste inchangé et valide. |

**Statut :** PLANNED — aucune implémentation existante.

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
Candidate Pair, Matching v1 et Geometric Verification Model v1 sont
**IMPLEMENTED**. Geometric Verifier,
Tracks et SfM sont **PLANNED**.

Ce document décrit la vision architecturale cible du pipeline de
reconstruction. Les modules listés ici ne sont pas tous implémentés.
L'implémentation suit la feuille de route définie dans `.opencode/context.md`
et progresse par tickets successifs en respectant les invariants
d'indépendance et de reprise.
