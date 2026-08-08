# Visual Index

## Définition

Un **visual index** (ou index visuel) est une structure de données compacte qui encode les caractéristiques visuelles distinctives de chaque image d'un scan set. Il permet de retrouver rapidement les paires d'images susceptibles de se chevaucher, sans comparaison exhaustive de toutes les combinaisons possibles.

L'index visuel transforme chaque image en un vecteur de descripteurs (features) invariantes à la rotation, à l'échelle et partiellement à l'illumination. Ces descripteurs forment une empreinte numérique qui identifie le contenu visuel de l'image.

## Statut

**IDEA** — Concept fondamental pour l'étape de matching. Pas encore implémenté. Actuellement, le pipeline de photogrammétrie classique utilise une approche exhaustive ou semi-exhaustive qui ne passe pas à l'échelle.

## Place dans le pipeline

```
Scan Set (images brutes)
    ↓
Feature Extraction (descripteurs par image)
    ↓
Visual Index (indexation compacte)
    ↓
Candidate Selection (paires candidats)
    ↓
Matching & Tracks (appariement détaillé)
```

L'index visuel se situe entre l'extraction de features et la sélection de paires candidates. Il réduit drastiquement l'espace de recherche pour l'appariement.

Le futur index référencera exclusivement les `image_id` stables du catalogue
persistant. Un chemin d'asset ou un nom source n'est ni une identité d'image ni
une clé d'index durable. Le partage d'un asset entre deux ScanSets n'empêche pas
leurs deux images logiques d'être des observations distinctes.

## Concepts clés

### Descripteurs visuels

| Type | Caractéristiques | Usage |
|------|------------------|-------|
| **SIFT** | Invariant à l'échelle et rotation, 128 dimensions | Référence académique, lent |
| **SURF** | Plus rapide que SIFT, bonne invariance | Compromis vitesse/qualité |
| **ORB** | Binaire, très rapide, Open source | Usage temps réel |
| **AKAZE** | Non-linéaire, préservation des contours | Bon pour les textures |

### Structure de l'index

```
Image → [Feature 1, Feature 2, ..., Feature N]
                    ↓
         Visual Index (KD-Tree / LSH / PQ)
                    ↓
         Requête : top-K voisins les plus proches
```

### Techniques d'indexation

- **KD-Tree** : arbre binaire de partitionnement spatial, efficace pour KNN en basse dimension
- **Locality-Sensitive Hashing (LSH)** : hashing probabiliste, efficace en haute dimension
- **Product Quantization (PQ)** : quantification par produit, compression agressive des descripteurs
- **IVF (Inverted File)** : indexation par clusters, bon compromis mémoire/recherche

### Paramètres clés

| Paramètre | Description | Impact |
|-----------|-------------|--------|
| `n_features_per_image` | Nombre de descripteurs extraits par image | Plus = meilleure couverture, plus de mémoire |
| `index_type` | Type de structure d'index (KD-Tree, LSH, PQ) | Compromis précision/vitesse |
| `search_radius` | Rayon de recherche pour les voisins | Plus grand = plus de candidats, plus lent |
| `min_match_count` | Seuil minimum d'appariements validés | Filtre les faux positifs |

## Relations avec les autres modules

| Module | Relation |
|--------|----------|
| **Image Catalog** | Le catalogue fournit les métadonnées nécessaires à l'indexation (dimensions, modèle caméra). |
| **Image View** | Les vues peuvent filtrer les images avant indexation (par zone, par qualité). |
| **Resource Governor** | Le gouverneur alloue la mémoire pour la structure d'index et contrôle la taille des lots d'indexation. |
| **Task** | L'indexation visuelle est une tâche candidate pour le scheduler. Elle est CPU-intensive mais parallélisable. |
| **Hardware Profile** | Le profil matériel détermine le type d'index optimal (KD-Tree pour peu de features, LSH pour beaucoup). |

## Contraintes de conception

- L'index doit tenir en mémoire pour un scan set complet typique (100-1000 images).
- La construction de l'index ne doit pas bloquer la TUI : tâche asynchrone avec progression.
- L'index est reconstruction-only : il n'est pas persisté entre les sessions (reconstruit à la demande).
- La taille maximale de l'index est bornée par le budget RAM du Resource Governor.
- Les descripteurs doivent être calculés de manière déterministe (même image → même index).

## Terminologie

| Terme | Définition |
|-------|------------|
| **Feature** | Point d'intérêt local avec descripteur associé |
| **Keypoint** | Position spatiale (x, y) d'un point d'intérêt |
| **Descriptor** | Vecteur numérique décruant l'apparence locale autour d'un keypoint |
| **KNN** | K-Nearest Neighbors, recherche des K voisins les plus proches |
| **Vocabulary visuel** | Dictionnaire de descripteurs typiques pour la bag-of-words |
| **BoW (Bag of Words)** | Représentation histogrammique des features d'une image |
