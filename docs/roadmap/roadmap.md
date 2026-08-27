# Roadmap Lardon3D

## Precision features

- **IMPLEMENTED v1A** : ORB coarse stable, SIFT/RootSIFT OpenCV 5, Feature File
  v2 F32×128, grille/coverage, tâches récupérables et consolidation intra-image.
- **IMPLEMENTED** : Candidate Pair Generator, matching précis et vérification géométrique.
- **PLANNED / BLOCKED** : ALIKED, en attente de provenance modèle et d'un export
  ONNX reproductible validé contre un oracle upstream.

## Direction générale

Lardon3D suit une feuille de route ordonnée qui privilégie la stabilité et la consolidation avant l'ajout de fonctionnalités complexes.

## Étapes terminées (DONE)

### Phase 1 : Fondations
- ✅ TUI modulaire avec ncursesw
- ✅ Gestion persistante des projets
- ✅ Import d'images migré vers le scheduler générique, borné et reconstructible
- ✅ Catalogue d'images et vues
- ✅ Moteur de tâches avec pause/reprise, annulation, checkpoints
- ✅ File FIFO avec sélection adaptative et backpressure
- ✅ Profil matériel et snapshots de ressources
- ✅ Resource Governor avec réservations opaques
- ✅ Intégration scheduler ↔ governor
- ✅ Sélection de la première tâche admissible

## Travaux d'infrastructure en cours (CURRENT FOUNDATION)

### Phase 2 : Consolidation
- 🔄 Documentation architecturale
- 🔄 Tests et validation
- 🔄 Optimisations mémoire

## Prochaines étapes décidées (NEXT)

### Phase 3 : Persistance
- ✅ Fondation versionnée des checkpoints de tâches
- ✅ Project Database v7 (tâches, catalogue, Feature Store, Visual Index et precision features)
- ✅ Branchement Project Database au cycle de vie projet et inventaire de reprise
- ✅ Registry durable des types métier de tâches
- ✅ Premier type métier reconstructible (`import.images`)
- ✅ Resoumission automatique contrôlée et bornée des tâches récupérables
- ✅ ScanSet v1 et Image Catalog persistant v1

### Phase 4 : Pipeline avancé
- ✅ Feature Store v1/v2, ORB, SIFT/RootSIFT et consolidation intra-image
- ✅ Visual Index v1 segmenté et persistant
- ✅ Candidate Pair Generator
- ✅ Matching et vérification géométrique
- ✅ Track Model / Track Builder v1
- ✅ Sparse SfM : primitives géométriques Gate C et noyau incrémental Gate D
  implémentés
- ✅ Sparse SfM Gate E : Bundle Adjustment final par composante PASS / FROZEN
- ✅ Sparse SfM Gate F : orchestration durable et publication atomique PASS / FROZEN
- ✅ Sparse SfM Gate G : politique et cœur **PASS / FROZEN**

### Phase 5 : Reconstruction
- ✅ Orchestration de reconstruction incrémentale H v1 — **PASS / FROZEN**
- ✅ MVS-M1 — **PASS / FROZEN** : identité dense `L3DMDID2`
  v2 (220 octets) liant reconstruction de base, jeu d'images source,
  `calibration_scope_identity` historique, binding numérique exact `L3DMCAL1` v1,
  backend et paramètres ; les octets source restent liés séparément ; frontière
  OpenMVS v2.4.0 externe
  `InterfaceCOLMAP`/`DensifyPointCloud` ; export COLMAP déterministe avec
  undistortion OpenCV, observations transformées et tracks réels ; texte COLMAP
  exporté en flux et tracks déterministes indexés sans rescan quadratique des
  observations. Le binding de calibration trie les `image_id`, encode des champs
  little-endian explicites à largeur fixe, rejette NaN/Inf et canonise `-0`. Chaque
  invocation crée sous le staging appelant un espace de travail privé neuf, sans
  réemployer scène, profondeur, cache ou sortie antérieure. Le PLY par défaut
  OpenMVS v2.4.0, binaire little-endian, est validé avec listes
  `view_indices`/`view_weights` bornées, nombres indépendants de la locale,
  en-tête <= 1 MiB en octets bruts (CRLF = deux octets), lignes <= 64 KiB,
  LF/CRLF acceptés et CR seul malformé rejeté ; le PLY fusionné est requis en mode
  de fusion 0. Une coordonnée source undistordue
  non finie est `INVALID_SOURCE_IMAGE`, une pose snapshot invalide reste
  `INVALID_SNAPSHOT`. Capacité CPU-only conditionnelle et `--max-threads`
  supporté ; bornes locales. Le SHA-256 source est complet et borné à 1 GiB par
  fichier régulier, sans budget agrégé du jeu de sources ; le hashage des deux
  binaires backend relève d'un budget partagé distinct. Sans nouveau sous-système.
  Restent différés : publication Project DB/durable, Task Runtime,
  Queue/Governor, `ResourceEstimate`, annulation, mesh, texturing, viewer,
  scratch/SSD et infrastructure backend généralisée.
- 📋 Mesh
- 📋 Contraintes externes
- 📋 Consolidation

## Étapes futures (LATER)

### Phase 6 : Production
- ⏳ Viewer intégré
- ⏳ Publication live validée
- ⏳ Export multi-formats
- ⏳ Optimisations performances

### Phase 7 : Avancé
- ⏳ Priorités entre tâches
- ⏳ Pools de workers multiples (CPU/GPU/IO)
- ⏳ DAG de dépendances complet
- ⏳ Parallélisme inter-tâches

## Sujets exploratoires (RESEARCH)

- 🔬 Intégration avec des sources de données externes
- 🔬 Support de formats d'entrée variés
- 🔬 Optimisation pour machines à très faible mémoire
- 🔬 Distribution de calcul

## Principes directeurs

1. **Stabilité avant performance** : ne jamais saturer le système hôte
2. **Séquençage avant parallélisme** : lots adaptatifs et workers uniques d'abord
3. **Réservation atomique** : aucune exécution sans contrat valide
4. **Persistance progressive** : chaque résultat doit pouvoir être repris
5. **Documentation vivante** : la documentation suit le code, pas l'inverse

## Vérification

Cette roadmap est vérifiée contre l'état réel du code. Les fonctionnalités déjà implémentées ne sont pas marquées comme NEXT ou LATER.
