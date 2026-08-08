# Scan Sets & Acquisitions

## Définition

Un **scan set** (ou ensemble de scans) regroupe l'ensemble des images capturées lors d'une session d'acquisition unique, correspondant à un seul objet ou site reconstruit. Chaque scan set constitue l'unité atomique d'entrée du pipeline de photogrammétrie.

Une **acquisition** désigne le processus physique de capture des images : positioning des capteurs, paramètres d'exposition, et conditions d'éclairage. Le scan set est le résultat numérique de cette acquisition.

## Statut

**PLANNED** — Concept définissant la structure d'entrée, pas encore implémenté comme module distinct. Actuellement, le module `import` et `image_catalog` gèrent les images individuellement sans regroupement en scan sets.
Le chemin source durable de `import.images` ne constitue donc pas encore une
identité de ScanSet.

## Place dans le pipeline

```
Acquisition physique
    ↓
Scan Set (import groupé)
    ↓
Image Catalog (indexation)
    ↓
Visual Index (indexation visuelle)
    ↓
Matching & Tracks
    ↓
Reconstruction Layers
```

Le scan set est la première structure organisée après l'import brut. Il fournit le contexte de regroupement nécessaire pour les étapes suivantes.

## Concepts clés

### Structure d'un scan set

| Champ | Type | Description |
|-------|------|-------------|
| `id` | `uint64_t` | Identifiant unique du scan set |
| `name` | `char[]` | Nom lisible (ex: "table_statue_01") |
| `project_ref` | `uint64_t` | Référence au projet parent |
| `image_count` | `uint32_t` | Nombre d'images dans le set |
| `acquisition_date` | `int64_t` | Timestamp de la session |
| `camera_model` | `char[]` | Modèle de caméra utilisé |
| `pixel_size_um` | `double` | Taille de pixel en micromètres |
| `focal_length_mm` | `double` | Distance focale nominale |

### Relations entre images

Au sein d'un scan set, les images entretiennent des relations spatiales :

- **Overlap horizontal** : chevauchement entre images adjacentes d'une même ligne de capture (typiquement 60-80%)
- **Overlap vertical** : chevauchement entre lignes de capture successives (typiquement 30-50%)
- **Baseline** : distance physique entre deux positions de capture consécutives
- **Convergence angle** : angle entre les axes optiques de deux caméras pour un même point

### Types de scan sets

- **Grid scan** : captures organisées en grille régulière, adapté aux objets de taille moyenne
- **Orbital scan** : captures circulaires autour d'un objet, adapté à la sculpture et aux artefacts
- **Linear scan** : captures le long d'un axe linéaire, adapté aux façades et structures longues
- **Free-form scan** : captures sans contrainte géométrique, nécessite plus d'overlap pour compenser

## Relations avec les autres modules

| Module | Relation |
|--------|----------|
| **Project** | Un scan set appartient à un projet. Le projet contient la structure de répertoires pour les images du set. |
| **Import** | L'import copie les images dans le projet. Le scan set représente un groupe logique d'images importées. |
| **Image Catalog** | Le catalogue indexe les métadonnées de chaque image du scan set. |
| **Image View** | Les vues peuvent filtrer ou trier les images par scan set. |
| **Resource Governor** | Le gouverneur estime les ressources nécessaires pour traiter un scan set complet. |
| **Task** | Le traitement d'un scan set est décomposé en tâches unitaires par le scheduler. |

## Contraintes de conception

- Un scan set ne doit jamais être modifié après le début du traitement (immutabilité partielle).
- L'ajout d'images à un scan set existant doit déclencher un recalcul incrémental, pas un retraitement complet.
- La taille maximale d'un scan set est bornée par la RAM disponible : pas plus de N images en mémoire simultanément.
- Chaque image appartient à exactement un scan set (relation 1:N).

## Terminologie

| Terme | Définition |
|-------|------------|
| **Scan set** | Groupe d'images d'une même session d'acquisition |
| **Acquisition** | Processus physique de capture |
| **Overlap** | Pourcentage de superficie commune entre deux images |
| **Baseline** | Distance entre deux positions de capture |
| **Capture session** | Période continue d'acquisition d'images |
| **Footprint** | Zone physique couverte par une image au sol |
