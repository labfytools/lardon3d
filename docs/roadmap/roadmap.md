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

### Phase 5 : Reconstruction (PLANNED)
- 📋 Orchestration de reconstruction incrémentale
- 📋 MVS / dense
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
