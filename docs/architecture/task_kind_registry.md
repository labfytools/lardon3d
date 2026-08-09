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
