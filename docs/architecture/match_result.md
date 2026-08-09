# Match Result Model v1

## Rôle et frontière

Un Match Result décrit un calcul descriptor-level réussi entre deux Feature
Sets d'une Candidate Pair. Les correspondances sont dans le Match File; aucune
vérification géométrique, aucun inlier géométrique et aucun masque d'inliers ne
font partie de ce modèle.

Les erreurs d'exécution appartiennent au Task Runtime et aux valeurs de retour.
Elles ne créent pas de Match Result. Le modèle persistant possède deux états :

- `NO_MATCH` (0) : `match_count == 0`, aucun asset;
- `MATCHED` (1) : `match_count > 0`, asset obligatoire.

`match_count` est exactement le nombre de correspondances descriptor-level
persistées après Lowe ratio et canonicalisation. Il n'existe ni `raw_match_count`
ambigu ni `inlier_count` dans Match Result v1.

## Identité et ownership

L'identité immuable est :

```
(candidate_pair_id, feature_set_id_a, feature_set_id_b,
 matcher_kind, matcher_version, parameter_fingerprint)
```

Elle porte une contrainte `UNIQUE`. `feature_set_id_a` appartient à
`candidate_pair.image_id_a` et `feature_set_id_b` à `image_id_b`. Les Feature
Set IDs ne sont jamais triés numériquement.

## Schéma Project DB v10

`match_results` impose en SQL : statut dans `(0,1)`, `0 <= match_count <= 8192`,
SHA de 32 octets, chemins non vides et tailles strictement positives lorsqu'ils
sont présents. Un `CHECK` conjoint impose :

```
NO_MATCH => match_count = 0 et SHA/path/size NULL
MATCHED  => match_count > 0 et SHA/path/size tous présents
```

La création vérifie aussi atomiquement l'ownership A/B avant l'INSERT. Les API
load/find/list conservent l'ordre stable par `match_result_id`; close/reopen ne
change aucune identité ni métadonnée.

## Reuse

`NO_MATCH` est directement réutilisable sans asset. `MATCHED` ne l'est qu'après
validation du chemin, de la taille, du SHA-256 réel, du format complet, du type
et de la dimension des descripteurs, des Feature Set IDs A/B et du compte.
Une ligne existante dont l'asset est absent ou corrompu déclenche un recalcul.
Le fichier est republié atomiquement et les champs résultat/asset de cette même
ligne sont réparés transactionnellement; son ID et son identité scientifique à
six parties restent inchangés. Aucune identité artificielle ne contourne la
contrainte `UNIQUE`.

## Suite

Le prochain modèle possède ses propres statistiques géométriques, dont un
éventuel `inlier_count`.

Geometric Verification Model v1 est désormais l'enfant scientifique persistant
de ce résultat dans Project DB v12.

NEXT: GEOMETRIC VERIFIER v1
