# Matching & Tracks

## Définition

Le **matching** est le processus d'appariement de descripteurs visuels entre paires d'images pour identifier les correspondances géométriques. Un **track** (ou piste) est la séquence de correspondances cohérentes d'un même point 3D à travers plusieurs images.

Le matching transforme les features individuelles en relations inter-images. Les tracks connectent ces relations en structures cohérentes qui traversent le scan set, formant le squelette de la reconstruction 3D.

## Statut

**PLANNED** — Étape critique du pipeline, pas encore implémentée. Nécessite le Visual Index comme prérequis.

## Place dans le pipeline

```
Visual Index (candidats)
    ↓
Feature Matching (appariement brut)
    ↓
Geometric Verification (filtrage par géométrie)
    ↓
Track Construction (assemblage en pistes)
    ↓
Track Optimization (raffinement)
    ↓
Reconstruction Layers (triangulation)
```

Le matching est le pont entre les caractéristiques 2D des images et la structure 3D de la scène.

## Concepts clés

### Étapes du matching

#### 1. Feature Matching (appariement brut)

Pour chaque paire d'images candidates :
- Recherche des descripteurs les plus proches (ratio test de Lowe)
- Filtrage par distance seuil
- Résultat : liste de correspondances brutes

```c
// Ratio test de Lowe
if (dist_best / dist_second_best < RATIO_THRESHOLD) {
    // Correspondance acceptée
}
```

#### 2. Geometric Verification (filtrage géométrique)

- Estimation de la **matrice fondamentale** F (cas général) ou **matrice essentielle** E (caméra calibrée)
- Filtrage par **RANSAC** : rejet des outliers
- Validation de la **parallaxe minimale** pour éviter les dégénérescences

#### 3. Track Construction (assemblage)

- Union-find sur les correspondances cohérentes
- Chaînage transitoire : si A↔B et B↔C, alors A↔C potentiellement
- Filtration des tracks trop courts (< 3 images) et trop longs (erreurs de chaînage)

### Structures de données

#### Correspondance (Match)

| Champ | Type | Description |
|-------|------|-------------|
| `image_a` | `uint64_t` | Identifiant de la première image |
| `image_b` | `uint64_t` | Identifiant de la seconde image |
| `keypoint_a` | `Keypoint` | Position (x,y) dans l'image A |
| `keypoint_b` | `Keypoint` | Position (x,y) dans l'image B |
| `descriptor_a` | `Descriptor` | Descripteur dans l'image A |
| `descriptor_b` | `Descriptor` | Descripteur dans l'image B |
| `confidence` | `float` | Score de confiance de l'appariement |

#### Track (Piste)

| Champ | Type | Description |
|-------|------|-------------|
| `id` | `uint64_t` | Identifiant unique du track |
| `observations` | `Observation[]` | Liste des observations (image, keypoint) |
| `point_3d` | `Vec3d` | Position 3D estimée (après triangulation) |
| `status` | `enum` | ACTIVE, OPTIMIZED, REJECTED |

#### Observation

| Champ | Type | Description |
|-------|------|-------------|
| `image_id` | `uint64_t` | Image où le point est observé |
| `keypoint` | `Keypoint` | Position (x,y) dans l'image |
| `track_id` | `uint64_t` | Track auquel appartient l'observation |

### Matrice de co-visibilité

La **co-visibilité** mesure le nombre de tracks partagés entre deux images. Elle forme une matrice symétrique qui influence :

- Le choix des paires à traiter en priorité
- La robustesse de la reconstruction (images bien connectées = meilleur ancrage)
- La détection des clusters disconnected

```
CoVis(A,B) = |Tracks(A) ∩ Tracks(B)|
```

## Relations avec les autres modules

| Module | Relation |
|--------|----------|
| **Visual Index** | Fournit les candidats initiaux pour le matching. |
| **Scan Sets** | Le matching s'effectue au sein d'un scan set ou entre scan sets connectés. |
| **Geometric Constraints** | Les essentiels, fondamentales et homographies sont appliqués pendant la vérification géométrique. |
| **Reconstruction Layers** | Les tracks sont l'entrée du processus de triangulation. |
| **Resource Governor** | Le matching est mémoire-intensive : le gouverneur contrôle la taille des lots de paires traitées. |
| **Task** | Le matching est décomposé en tâches par paire d'images, parallélisables. |
| **Hardware Profile** | Le matching peut être accéléré par GPU pour les calculs de distance entre descripteurs. |

## Contraintes de conception

- Le matching ne doit jamais produire de tracks incohérents (un track = un point 3D unique).
- Les tracks de moins de 3 observations sont rejetés comme non reconstructibles.
- Le filtre de ratio de Lowe (typiquement 0.7-0.8) est immuable après calibration.
- Le RANSAC utilise un nombre d'itérations borné et un seuil de consensus configurable.
- Les tracks rejetés sont conservés pour audit mais exclus de la reconstruction.
- La matrice de co-visibilité ne doit pas dépasser N×N pour N images (bornée par la RAM).

## Terminologie

| Terme | Définition |
|-------|------------|
| **Match** | Correspondance entre deux keypoints de deux images différentes |
| **Track** | Série de matches cohérents d'un même point 3D à travers N images |
| **Observation** | Apparition d'un track dans une image spécifique |
| **Outlier** | Match incorrect rejeté par vérification géométrique |
| **Inlier** | Match correct accepté après vérification |
| **Co-visibilité** | Nombre de tracks partagés entre deux images |
| **Baseline angulaire** | Angle entre les directions de vues de deux images pour un point donné |
| **Parallaxe** | Décalage apparent d'un point entre deux images |
