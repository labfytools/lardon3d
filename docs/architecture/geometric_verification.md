# Geometric Verification

## Scope

Geometric Verification Model v1 est le contrat scientifique persistant placé après le Matcher.
Il stocke un résultat terminé, compact et immutable. Il n'est ni un moteur de calcul ni une tâche.
Aucun RANSAC, USAC, MAGSAC, calcul d'inliers ou backend géométrique n'appartient à ce ticket.

## Position in reconstruction pipeline

La chaîne d'ownership est :

```text
Feature Set → Candidate Pair → Match Result → Geometric Verification Result
```

Le masque indexe exclusivement l'ordre des entrées du Match File canonique du Match Result. Il
n'indexe directement ni les features, ni la Candidate Pair, ni un ordre temporaire de backend.

## Scientific ownership

Le parent scientifique est `match_result_id`. L'API accepte uniquement un Match Result existant,
`MATCHED`, avec `match_count` strictement positif. `NO_MATCH` et les erreurs runtime ne peuvent pas
produire de résultat géométrique.

## Parent Match Result

Le Match Store reste propriétaire de la validation du Match File. La création consulte le parent
et son `match_count` en DB ; elle ne relit pas l'asset. Un load valide aussi l'existence et l'état du
parent afin qu'une ligne corrompue ne soit jamais rendue comme résultat valide.

## Persistent identity

L'identité demandée et unique est :

```text
(match_result_id, verifier_kind, verifier_version, parameter_fingerprint)
```

Le fingerprint est le SHA-256 opaque de 32 octets déjà standard dans le projet. Il représentera
un encodage de paramètres versionné, stable, à ordre de champs explicite et, pour les nombres
binaires, little-endian. Aucun timestamp, résultat, PID, durée ou identifiant matériel n'y entre.

## Verifier kind

v1 supporte uniquement `FUNDAMENTAL`, valeur persistante stable 1. Aucun comportement fictif
`ESSENTIAL` ou `HOMOGRAPHY` n'est réservé dans l'API publique.

## Persistent states

- `GEOMETRIC_REJECTED=1` : calcul scientifique terminé, critère non satisfait ;
- `GEOMETRIC_VERIFIED=2` : calcul scientifique terminé, critère satisfait.

`FAILED`, `RUNNING`, `PAUSED` et `CANCELLED` appartiennent au Task Runtime. REJECTED peut conserver
un nombre d'inliers non nul.

## Model representation

FUNDAMENTAL utilise neuf colonnes SQLite `REAL`, en ordre ligne-major `m00` à `m22`. SQLite
convertit les valeurs numériques en binary64 sans exposer une ABI C. VERIFIED exige les neuf
valeurs présentes et finies. REJECTED exige les neuf valeurs NULL. Le modèle n'impose ni rang 2,
ni déterminant, ni normalisation ou échelle canonique ; ces règles relèvent du futur verifier.

## Inlier representation

Le masque est un BLOB SQLite obligatoire de taille exacte `ceil(match_count / 8)`. Pour l'entrée
`i`, `byte_index=i/8`, `bit_index=i%8` et le masque vaut `1u << bit_index`. Le bit 0 est donc le bit
de poids faible de l'octet 0. Cette convention est indépendante de l'endianness CPU et de l'ABI.
Les bits de padding du dernier octet valent zéro et le popcount est exactement `inlier_count`.

Le masque existe pour REJECTED comme pour VERIFIED. Avec 8192 matches, il mesure au maximum
1024 octets. Un BLOB SQLite évite les milliers de lignes secondaires et la publication, le hash,
le nettoyage et la récupération d'un asset externe d'environ 1 Kio. Une liste `uint32_t` serait
jusqu'à 32 fois plus grande au cas dense et aurait un encodage supplémentaire à versionner.

## Invariants

- `0 <= inlier_count <= parent.match_count <= 8192` ;
- longueur, padding et popcount du masque sont canoniques ;
- REJECTED possède un masque cohérent et aucun modèle ;
- VERIFIED possède un masque cohérent et exactement neuf valeurs finies ;
- kind, version et fingerprint ont une sérialisation stable ;
- une ligne publiée est complète et immutable.

Exemple : pour 100 matches, FUNDAMENTAL v1/fingerprint X peut publier REJECTED avec 23 inliers,
un masque de 13 octets et aucun modèle. Une autre identité peut publier VERIFIED avec 67 inliers,
le même format de masque et une matrice 3×3 finie.

## Persistence semantics

Une création valide puis insère identité, état, masque et modèle dans une transaction courte. Le
calcul futur se fera entièrement avant cette transaction. SQLite fournit l'atomicité ; aucun asset
ou journal secondaire n'est créé.

## Reuse

Le reuse cherche uniquement l'identité exacte, jamais le résultat le plus récent. Une identité
existante retourne une erreur de contrainte à `create`; le runtime fera `find`, validera puis
réutilisera. `INSERT OR REPLACE` est interdit, même si le nouveau contenu semble identique.

## Invalidations

Un nouveau Match Result possède un nouvel ID et ne réutilise donc aucun ancien résultat
géométrique. La FK emploie `ON DELETE CASCADE` : supprimer explicitement le parent supprime ses
enfants et ne crée pas d'orphelin. Aucun moteur d'invalidation parallèle n'est nécessaire.

## Project DB schema

Project DB v12 ajoute `geometric_verification_results`, une contrainte UNIQUE sur l'identité et un
index de pagination `(match_result_id, geometric_verification_result_id)`. Les CHECK SQL portent
les bornes scalaires, tailles locales et nullabilité modèle/état. La cohérence avec le parent, le
padding, le popcount et la finitude restent validés en C.

## API

L'API publique implémente :

- `lardon3d_project_db_create_geometric_verification_result()` ;
- `lardon3d_project_db_load_geometric_verification_result()` ;
- `lardon3d_project_db_find_geometric_verification_result()` ;
- `lardon3d_project_db_list_geometric_verification_results()`.

La liste est bornée à 256 entrées, filtrée par parent puis ordonnée par ID croissant avec curseur.
Le résultat en mémoire contient son `created_at` et son masque dans une capacité fixe de 1024
octets : aucun ownership dynamique ni fonction de destruction. Les fonctions copient fingerprint,
masque et neuf coefficients ; l'appelant conserve ses entrées.

Parent absent retourne `NOT_FOUND`; parent NO_MATCH ou parent incohérent retourne `CONSTRAINT` à
la création. Masque, modèle ou arguments locaux invalides retournent `INVALID_ARGUMENT`; duplicate
identity retourne `CONSTRAINT`. Un loader qui rencontre une ligne ou un parent incohérent retourne
`CORRUPT`, sans résultat partiel.

## Resource bounds

Un résultat contient au plus 1024 octets de masque et 72 octets de valeurs numériques, plus de
petites métadonnées. Une page est bornée. Le loader vérifie les entiers et tailles SQLite avant
tout cast ou copie. Il n'existe ni cache global, ni lecture non bornée, ni Content Store associé.
Le Match File parent mesure au plus 98 336 octets ; le futur job peut donc rester une petite unité.

## Error ownership

Seuls les résultats scientifiques terminés sont persistés. OOM, exception, annulation, timeout,
device lost, I/O transitoire ou panne de thread appartiennent à l'exécution de tâche. État du modèle
et état d'exécution sont deux contrats distincts.

## Recovery semantics

Après commit, le résultat est complet et réutilisable après réouverture. Avant commit, le rollback
ne laisse aucune ligne partielle. Un loader rejette toute ligne incohérente comme corruption au
lieu de réparer ou d'interpréter au mieux.

## Future verifier execution contract

Le prochain ticket prendra un Match Result et son Match File borné. L'accès nécessaire existe via
`lardon3d_feature_reader_keypoints()`, borné à 256 keypoints par appel ; l'intégration devra relier
les deux Feature Sets et les indices du Match File sans modifier le Feature Store. Le verifier
estimera hors transaction, dérivera état/masque/modèle, publiera en une courte transaction,
checkpoint puis libérera les buffers. Une paire est l'unité atomique. Task Runtime et Resource
Governor décideront admission, threads et lots ; zram/swap ne sont jamais un budget.

Un backend reste hors identité seulement s'il est scientifiquement transparent. Sinon son
algorithme ou contrat doit apparaître dans kind/version/fingerprint avant publication. Toute seed
influençant le résultat doit avoir une politique déterministe versionnée ou être couverte par le
fingerprint. Aucun nombre de threads ou hardware ID n'est un paramètre scientifique par défaut.

## Explicitly out of scope

Le calcul géométrique, le choix RANSAC/USAC/MAGSAC, OpenCV geometry, GPU, Vulkan, OpenCL, shader,
task kind, worker, checkpoint et nouvelle orchestration sont explicitement hors périmètre.

## Versioning

Project DB schema version 12 décrit le stockage. `verifier_version` décrit indépendamment le
contrat scientifique. Changer un algorithme n'impose une migration DB que si la représentation
persistante change.
