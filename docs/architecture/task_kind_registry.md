# Registry des types métier de tâches

## Feature kinds v1A

La registry statique conserve `features.extract` v1 pour ORB et ajoute
`features.extract.sift` v1 et `features.extract.rootsift` v1. Les reconstructeurs
chargent la table dédiée, revalident le fingerprint et ne capturent aucun
`AppState`.

## Contrat

Une instance possède un **task ID** stable. Son **task kind** décrit son
comportement métier, son **task state** décrit son état d'avancement, la
**checkpoint version** décrit le codec générique et la **task kind version**
versionne les paramètres nécessaires au reconstructeur. Ces identités ne sont
pas interchangeables.

Le kind v1 est une chaîne ASCII de 1 à 64 caractères au format
`[a-z0-9][a-z0-9._-]*`. La version est un entier non nul. Aucun kind n'est
déduit d'un nom, callback ou pointeur et aucune normalisation n'est effectuée.

## Registry et ownership

La registry est une vue bornée à 64 descriptors sur un tableau statique
immutable. Le lookup est linéaire, déterministe, sans allocation et sûr en
lecture concurrente. Elle ne charge aucun code dynamiquement.

Un descriptor contient exactement le kind, sa version et un reconstructeur.
Le reconstructeur produit callback, userdata et destructeur optionnel. Avant le
transfert, la registry nettoie le userdata sur toute erreur ; après restauration
réussie, `Lardon3DTask` en devient propriétaire et le détruit une fois après la
fin de l'exécution. Le constructeur métier n'est jamais appelé sous mutex DB.

## Persistance et legacy

Le checkpoint générique reste en version 1. Project Database v7 conserve le
kind/version ; les lignes migrées depuis v1 restent `NULL/NULL` et sont classées
`LEGACY_UNTYPED`. Un kind inconnu ou une version non supportée reste inspectable
mais inexécutable. Aucun type n'est inventé et aucun code n'est sélectionné par
adresse persistée.

## Statut

**IMPLEMENTED** — identité typée immutable, registry statique, lookup,
migration DB v1→v2, classification recovery et restauration explicite testée.

**IMPLEMENTED** — le descriptor production `import.images`, version 1, charge
le chemin source borné depuis la table dédiée et reconstruit callback et
userdata sans `AppState *` ancien.

**IMPLEMENTED** — `project_open()` utilise la registry production immutable
pour restaurer hors mutex DB et transférer chaque tâche acceptée à la queue.

**NOT_YET_WIRED** — autosave complet et réconciliation orpheline.

**IMPLEMENTED** — `features.extract` version 1 reconstruit une extraction ORB
depuis `image_id` et ses paramètres bornés.

**IMPLEMENTED** — `visual_index.update`, version 1, recharge
`visual_index_id + after_feature_set_id` et reconstruit un contexte neuf.

**IMPLEMENTED** — `candidate_pair.generate`, version 1, recharge
`visual_index_id + after_feature_set_id + top_k + minimum_evidence_count
+ scanset_filter + exclude_same_asset` depuis `candidate_pair_generate_tasks`
et reconstruit un contexte boundé.

**IMPLEMENTED** — `matcher.run`, version 1, recharge la configuration Matcher,
l'identité Feature Set et le curseur `after_candidate_pair_id`. Il traite une
Candidate Pair atomique à la fois dans des lots bornés à huit, checkpoint le
curseur et repasse par le Governor entre les lots. La table durable
`matcher_tasks` est introduite par Project DB v11, après le Match Result v10.

**IMPLEMENTED** — `geometric_verifier.run`, version 1, recharge la configuration
Fundamental immuable, en revalide le fingerprint et reprend `after_match_result_id`.
Project DB v13 ajoute uniquement `geometric_verifier_tasks`, car le checkpoint
générique v1 ne possède aucun payload propre au kind.

**IMPLEMENTED** — `track_builder.run`, version 1, reconstruit un scope explicite
depuis son payload Project DB v15 et son asset little-endian validé. Le callback
réutilise l'orchestration Gate C et le reconstructeur refuse toute corruption,
version, fingerprint, checksum, tri, unicité ou L3DTSIS1 incohérents.

**PASS / FROZEN** — `sparse_sfm.run`, version 1, recharge le
payload scientifique explicite Project DB v17, restaure l'estimation générique
persistée et rejoue D puis E depuis les références Track Set/calibration. Le
fingerprint F0 est recalculé ; le checkpoint générique v1 reste inchangé.

**PASS / FROZEN** — `incremental_reconstruction.run`,
version 1, recharge le payload Project DB v18 composé du prédécesseur, du Track
Set d'extension, du scope de calibration et du fingerprint H. La tâche atomique
recalcule depuis ces entrées après redémarrage, passe par la Queue et le
Governor avec son estimation H immuable, et ne persiste aucun état de solveur.
Elle n'ajoute ni DAG ni dépendance implicite.
