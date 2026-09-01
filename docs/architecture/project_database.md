# Base de données projet Lardon3D

## Contexte optique générique — Project DB v23

**IMPLEMENTED / VALIDATED / REVIEWED.** La tête courante v23 est une migration
transactionnelle additive au-dessus de la fondation scientifique et de
persistance v22 **PASS / FROZEN**. Elle crée neuf tables sans aucune ligne :
`camera_body_profiles`, `camera_body_aliases`, `lens_profiles`,
`lens_profile_aliases`, `optical_configurations`,
`acquisition_campaign_group_optics`, `capture_optical_configurations`,
`optical_calibration_profiles` et `capture_calibration_selections`.

Boîtier, objectif, configuration optique et calibration sont quatre identités
distinctes. Une configuration lie exactement un boîtier, un objectif et une
focale entière optionnelle en micromètres. Un objectif manuel sans EXIF est un
cas normal : son profil explicite ne requiert aucun alias metadata. Le fixture
Meike vérifie ce chemin générique ; il ne crée aucun branchement produit par
marque ou modèle. Les alias make/model, lorsqu'ils existent, sont des couples
exacts `BINARY`, jamais une recherche fuzzy, une correction de casse ou une
preuve d'identité implicite.

Une campagne peut assigner une configuration différente à chaque groupe avant
sa matérialisation. Lorsque S3-E retourne le Capture, la même transaction
retient la correspondance groupe→Capture, copie l'affectation optique avec sa
provenance campagne et avance le curseur. Un rollback tardif annule les trois
mutations. Une affectation explicite post-import est possible seulement pour un
Capture encore non affecté ; l'absence reste `NOT_FOUND` et ne crée pas de
profil « inconnu ».

Un profil de calibration référence une calibration sparse immuable existante et
une seule configuration exacte. Il ne copie, recalcule, interpole ni fabrique
aucun coefficient. Les listes compatibles filtrent uniquement cette identité
exacte. La sélection sur Capture est toujours explicite et exige la même
configuration des deux côtés ; plusieurs profils compatibles restent ambigus
jusqu'à cette sélection. Retry exact et naturel converge, un conflit durable
valide retourne `CONSTRAINT`, tandis qu'une ligne ou dépendance persistée
malformée retourne `CORRUPT` avant comparaison avec une demande alternative.

La migration v22→v23 n'inspecte ni EXIF, chemin, basename, SHA-256, dimensions,
nom d'appareil, ni calibration historique. Elle ne réinterprète donc aucune
Capture, image ou calibration v16–v22. Le marqueur `schema_version=23` est le
point de publication durable ; un échec de création ou un update de marqueur
qui ne cible pas exactement la v22 rollbacke toute la migration.

La validation a migré via l'API production des copies des projets réels S21 et
A6000 : version 23, `integrity_check` et clés étrangères propres, comptes et
lignes scientifiques inchangés, neuf tables optiques vides et SHA-256 des
sources inchangés. La suite normale 57/57, dix tests focalisés, cinq cibles
ASan/UBSan, les contrôles de headers C17/C++ et la revue indépendante après
correction sont acquis. Cette preuve porte sur l'overlay et sa compatibilité ;
elle ne crée aucune calibration scientifique pour ces campagnes.

### Workflow TUI optique courant

La TUI v23 appelle exclusivement les API publiques bornées ; elle ne modifie
pas directement SQLite. Elle peut inspecter un Capture, rechercher des alias
metadata par égalité exacte, parcourir par pages de 16 les boîtiers, objectifs,
configurations et calibrations compatibles, créer un nouveau profil ou une
nouvelle configuration immuable, puis affecter explicitement un groupe de
campagne ou un Capture lorsque le contrat ci-dessus l'autorise. Un objectif
Meike manuel sans électronique ni alias reste une donnée normale, et un lookup
sans correspondance demeure unresolved.

La sélection d'une calibration reste explicite et exige l'exacte configuration
du Capture. Une absence, plusieurs candidats, une incompatibilité, `BUSY`, I/O
ou corruption sont présentés comme tels ; la TUI ne fabrique jamais de profil
« inconnu », de compatibilité ou d'identité. `R` permet un retry explicite après
un échec de bind/chargement. `[` revient à la première page et `]` charge la
suivante ; chaque libellé annonce seulement le compte de la page courante et
l'existence exacte d'une suite. Le modèle TUI emprunte la DB jusqu'à unbind et
doit être détaché avant la fermeture projet, conformément au
[runtime](runtime.md#observatoire-tui-actuel).

## Snapshot d'exécution scientifique sélectionnée — Project DB v22

**PASS / FROZEN.** La migration additive v21→v22 ajoute
`capture_source_assets`, `selected_executions`, `selected_execution_items` et
`raw_development_tasks`.
`capture_source_assets` retient l'association explicitement publiée par S3-E entre un Capture,
son asset `SOURCE` et le kind JPEG ou RAW déjà validé. La publication de l'association SOURCE et
du kind est transactionnelle et un retry exact converge ; un autre kind est un conflit. Les
Captures provenant d'une v21 restent honnêtement sans mapping : la migration ne déduit rien de
leur chemin, nom, SHA-256, ordre d'attache ou image logique.

Un snapshot ordonné retient explicitement
la relation `quality_task_id/group_id → campaign_task_id/group_id → capture_id`; les deux espaces
de `group_id` restent indépendants et une égalité numérique n'est jamais une identité. Les deux
tâches doivent viser le même ScanSet, le triage doit être terminé et chaque résultat retenu doit
être effectivement inclus au moment du snapshot. Le snapshot et son ordre deviennent ensuite
immuables.

Chaque item reçoit un `image_id` seulement par association explicite déjà durable dans
`capture_images`. La publication de cette représentation et l'avancement du curseur sont
transactionnels ; un retry exact converge, tandis qu'un autre `image_id` est un conflit. Après le
dernier item, un `calibration_scope_id` existant ne peut être attaché que si chaque image retenue
est membre de ce scope. Le stage terminal `READY` représente donc exactement l'intersection
`QUALITY_SELECTED ∩ REPRESENTATION_READY ∩ CALIBRATION_ASSIGNED` sans inférer Capture depuis
Asset, SHA-256, chemin, basename, Task ID, groupe ou `image_id`.

Le snapshot fixe aussi la source de représentation de chaque item. Un item A6000 retient
explicitement l'`asset_id` RAW déjà associé comme `SOURCE` au Capture ; un item dont la
représentation est une image source retient explicitement l'absence d'identité RAW. Cette paire
discriminant/Asset est immuable et fait partie de l'exact retry du snapshot. La création vérifie
l'association RAW dans `capture_source_assets` ; aucune exécution ultérieure ne retrouve le RAW depuis
un chemin, SHA-256, basename, index source ou numéro de groupe.

Le curseur est borné à 4096 items et ne réserve aucune ressource d'exécution. Cette fondation v22
n'ajoute ni scheduler, worker pool, sidecar, cache décodé, ni nouvelle Queue/Governor. Le
coordinateur scientifique et le développement RAW borné restent des étapes d'exécution séparées.
L'importeur Calibration Bootstrap v1 valide un artifact borné contre ce snapshot, crée/réutilise les
calibrations et le scope immuables, puis emploie cette attache v22 ; il n'étend pas le schéma et n'exécute
ni solveur ni développement RAW.

Le développeur RAW expose aussi une entrée bornée par `source_asset_id` explicite. Elle exige la
relation RAW du Capture, charge le chemin géré depuis cet ID, vérifie les octets contre le SHA-256
persisté, puis réutilise sans modification RAW Policy v1. Le chemin géré n'est donc qu'un accès aux
octets après résolution d'identité explicite, jamais une méthode de découverte d'identité.

La v22 amendée ajoute `raw_development_tasks` pour l'exécution durable mince de cette entrée. Une
ligne retient exactement `task_id`, `capture_id`, `source_asset_id`, une phase monotone
`PENDING → PUBLISHED` et, en phase publiée, l'`image_id` dérivée explicitement associée au même
Capture. Le couple Capture/SOURCE RAW est immuable, doit exister dans `capture_source_assets` avec
le kind RAW, et n'est jamais retrouvé depuis un chemin, digest, nom, image ou ID opérationnel.
Snapshot Task générique, référence du checkpoint et ligne typée sont écrits dans une même
transaction. L'asset et l'image immuables peuvent précéder ce checkpoint si le processus meurt :
le retry exact S3-B1 converge par contenu puis publie la phase et l'`image_id` sans deviner
l'identité. Les lectures de reprise rejettent types SQLite, phases, nullabilité et relations
Capture/RAW/image incohérents comme corruption durable.

La suite normale v22 a passé 53/53, les contrôles syntaxiques C17 et `git diff
--check` ont passé, la validation ciblée ASan/UBSan a passé et l'audit final a
conclu au PASS. La suite ASan/UBSan complète reste qualifiée par le comportement
LSan du pilote tiers RADV ; elle ne constitue pas un PASS complet du dépôt.
Cette évidence gèle la frontière de persistance v22 décrite ici, sans changement
de contrat ni de schéma. Les campagnes réelles peuvent donc conserver un snapshot
et leurs représentations sans être scientifiquement exécutables : une absence de
calibration connue reste `CALIBRATION_UNAVAILABLE`, non une raison d'inférer ou
de créer une identité, une calibration ou une migration supplémentaire.

## Photo Quality Triage — Project DB v21

**PASS / FROZEN.** La migration additive v20→v21 ajoute
`photo_quality_triage_tasks` et `photo_quality_triage_results` sans modifier les tables
v20 ni les identités Capture/Asset/image. Chaque résultat conserve le `group_id` canonique
du plan (1..N). `next_group_id` utilise la même identité à base 1 : valeur initiale 1,
avancement de `k` vers `k+1` après publication du résultat `k`, et valeur terminale `N+1`.
Un éventuel `group_index = group_id - 1` reste strictement privé à l'exécuteur. Résultat et
curseur sont atomiques avant le checkpoint Task générique. Les lectures valident les types,
signes, bornes et relations dans les valeurs SQLite 64 bits avant toute conversion vers
les champs C étroits et exigent le dispatch générique exact
`photo_quality.triage/v1`. Recommandation mesurée et override humain restent
distincts.

## Capture / Asset Provenance v1 — Project DB v19 (historique FROZEN)

**PASS / FROZEN.** La migration transactionnelle v18→v19
ajoute seulement une fondation de catalogue : `captures`, `capture_images`,
`capture_assets`, `capture_selections` et `asset_derivations`. Un `Capture`
appartient à un seul ScanSet ; il n'est ni un `image_id` ni une nouvelle identité
scientifique. Les assets source et dérivés peuvent lui être associés sans créer
d'image logique. Une sélection courante optionnelle référence exactement une
image déjà associée au Capture et ne modifie jamais cette image, son asset, ni
les résultats scientifiques existants.

L'association explicite d'un asset `SOURCE` existant à un Capture existant est
idempotente : si cette même association existe déjà avec le rôle `SOURCE`, elle
est acceptée sans nouvelle ligne ni migration de schéma. Elle ne crée aucune
image logique, ne modifie aucune sélection et ne réalise aucun appariement
automatique.

### S3-E — Orchestration d'ingestion multi-source v1

**PASS / FROZEN.** S3-E reçoit un ScanSet explicite et au
plus 64 chemins source explicitement fournis par l'appelant ; il ne parcourt
jamais un répertoire. Chaque fichier est d'abord publié comme asset immuable
géré, puis son évidence S3-D est extraite depuis ces octets publiés. Le
regroupement automatique ne crée un Capture commun que pour une relation
`SAME_ACQUISITION_STRONG` unique et mutuelle. Les noms, chemins, SHA-256,
timestamps, modèle, évidence faible, contradictions et ambiguïtés ne sont
jamais une identité de Capture et ne donnent jamais lieu à un rapprochement
automatique. Deux assets identiques dans une requête sont rejetés de façon
déterministe.

L'appelant peut aussi déclarer explicitement des groupes. Ce résultat porte la
basis `CALLER_EXPLICIT`, distincte de la basis scientifique `STRONG`; cette
provenance est un résultat de l'opération v1, non une nouvelle colonne durable.
Les siblings sont persistés exclusivement avec S3-C. Pour la représentation,
un JPEG caméra reste un asset `SOURCE` et devient une image logique du même
Capture via la publication source-image transactionnelle; un RAW reste
`SOURCE` et son image est produite seulement par S3-B1, avec son PNG `DERIVED`.
RAW et JPEG d'une même acquisition ne créent pas deux Captures. La sélection ne
change que si l'appelant demande explicitement la représentation sélectionnée.

Project DB reste strictement v19, sans migration ni fusion de Captures. Après
qu'un Capture ID a été renvoyé ou durablement retenu, une reprise doit fournir
explicitement `resume_capture_id`: publication, attaches S3-C et représentation
convergent alors sans réhoming ni doublon logique. Une panne avant que
l'appelant ait retenu cet ID ne peut pas offrir une reprise whole-request
exactly-once en v19; S3-E n'infère volontairement jamais une identité de
reprise depuis SHA, chemin, basename, timestamp, métadonnées ou `image_id`.

`asset_derivations` est volontairement limité à un parent asset et un enfant
asset, avec kind/version et fingerprint canonique de 32 octets. Il n'est pas un
DAG générique et ne réalise aucun développement RAW ni extraction vidéo. La
migration crée, par `image_id` croissant, un Capture legacy indépendant pour
chaque image v18, relie son asset comme source et rend cette même image
sélectionnée. Aucun nom de fichier, EXIF ou rapprochement de siblings n'est
interprété ; les IDs et la sémantique v18 restent inchangés.

### Couche finale bornée de découverte et de planification de campagne

**PASS / FROZEN.** Cette couche reçoit de 1 à 64 racines
absolues, lexicalement normalisées et explicitement fournies par l'appelant.
Elle ne parcourt pas récursivement : chaque racine et chacune de ses entrées
immédiates régulières est inspectée sans suivre de lien symbolique. Seuls les
fichiers `.arw`, `.jpg` et `.jpeg`, sans sensibilité à la casse, sont des
sources supportées. La découverte, les groupes et les propositions sont chacun
bornés à 4096 éléments et les chemins sont ordonnés de façon déterministe par
`strcmp`.

La planification appelle S3-D uniquement sur les métadonnées : elle ne lit pas
de pixels, n'écrit pas en base et ne matérialise aucun asset ou Capture. Une
association automatique est admise seulement pour une relation forte unique et
mutuelle. Un stem de nom de fichier identique est une proposition, jamais une
identité. Les contradictions, ambiguïtés, comparaisons insuffisantes et cas non
résolus restent des singletons. L'appelant peut confirmer explicitement de 1 à
64 groupes ; chacun est alors étiqueté `CALLER_EXPLICIT`. Une confirmation
réelle explicite reste nécessaire avant tout regroupement.

Le plan et sa progression appartiennent à l'appelant. Ils retiennent les
`capture_id` et `resume_capture_id` retournés afin de reprendre par Capture ;
aucune garantie whole-request exactly-once n'existe avant cette rétention.
L'exécution applique le lot par groupe, avec exactement un appel S3-E par
groupe figé. S3-E demeure l'unique matérialisateur et les représentations
restent explicites. Project DB demeure v19 : cette couche ne détourne ni Task,
ni checkpoint, et n'ajoute aucun sous-système de persistance.

La validation structurelle JPEG accepte un fichier ordinaire jusqu'à son EOI,
puis uniquement du remplissage nul. Si et seulement si l'image primaire porte
un segment APP2 commençant par `MPF\0`, ce remplissage peut être suivi d'une
nouvelle image JPEG structurellement validée par le même parseur ; jusqu'à huit
images au total sont admises, en mémoire constante. Chaque intervalle et la fin
physique restent limités à des octets nuls. Une image ajoutée sans preuve MPF,
un trailer non nul, une image secondaire tronquée ou sans EOI et un neuvième
élément sont corrompus. Les marqueurs ressemblants dans les payloads APP/EXIF
et les données entropy-coded ne deviennent jamais des frontières de conteneur.

Observation autorisée sur le jeu A6000 réel, en dry run : 953 ARW, 953 JPEG,
soit 1906 sources ; métadonnées OK pour les 1906 sources, indisponibles 0 et
autres erreurs 0. Les fichiers JPEG Sony sont des conteneurs MPF valides avec
une image secondaire et un remplissage final nul. Leurs métadonnées ne portent
pas d'`ImageUniqueID` utilisable en commun avec les RAW : fortes 0,
propositions candidates par stem 953, ambiguïtés 0, contradictions 0,
comparaisons insuffisantes 1814512, non résolus 1906 et groupes automatiques
planifiés 1906. L'inversion de l'ordre des racines a réussi la vérification de
déterminisme. Ce dry run n'a effectué ni écriture DB, ni matérialisation, ni
développement RAW ; il ne prouve donc pas un compte physique de 1906
acquisitions et n'affaiblit pas S3-D. Les 953 propositions nécessitent une
confirmation explicite `CALLER_EXPLICIT` avant tout regroupement. Le fixture
d'intégration couvre l'adaptateur, sa matérialisation et son retry ; aucune
mutation de campagne complète n'a été effectuée.

Restent hors S3 : TUI, scrub d'assets et réconciliation des orphelins, ainsi
que vidéo et reconstruction aval lorsqu'ils sont applicables. L'identité
opérationnelle persistante de campagne, son exécution par Task/Queue/Governor
et sa reprise sont définies par Project DB v20 ci-dessous ; ils ne modifient pas
le contrat S3.

## Exécution durable de campagne d'acquisition — Project DB v20

**PASS / FROZEN.** La migration transactionnelle additive
v19→v20 ajoute `acquisition_campaign_tasks` et
`acquisition_campaign_captures`, sans modifier les tables ni les identités
Capture/Asset de v19. Une base v19 est donc ouverte par la chaîne séquentielle
existante ; un échec de migration laisse la v19 complète et utilisable.

`acquisition_campaign_tasks` est lié une-à-une à la tâche générique par
`task_id`, qui est l'identité opérationnelle de la campagne. La création et les
checkpoints enregistrent dans une même transaction le snapshot Task générique,
sa référence de checkpoint et le record typé de campagne. Ce record conserve le
`scanset_id`, le curseur historique `next_group_id`, le nombre de groupes et
une requête immuable. Pour cette table de campagne, malgré son nom historique,
le curseur est une position de prochain travail à base zéro dans `0..N` : sa
valeur est le nombre de groupes one-based déjà retenus. Un upsert ne peut le
faire progresser que si ScanSet, nombre de groupes et octets de requête sont
identiques.

La requête v1 est un codec borné, déterministe, à magic/version explicites et
champs entiers de largeur fixe little-endian. Elle sérialise les sources, leurs
métadonnées déjà validées, les confirmations et les options d'ingestion ; elle
est refusée si ses bornes, longueurs, énumérations, indices, plan ou octets de
fin ne sont pas valides. Les confirmations sont ainsi persistées explicitement
et ne confèrent que la basis `CALLER_EXPLICIT`, jamais une inférence `STRONG`.

Les groupes de plan gardent leurs IDs un-based. Pour chaque groupe achevé,
`acquisition_campaign_captures` conserve la relation unique
`(task_id, group_id) → capture_id` et interdit qu'un même Capture corresponde à
deux groupes de la même tâche. L'état durable valide contient exactement le
préfixe de mappings `1..next_group_id`, aucun trou et aucun mapping en avance ;
chaque Capture existe dans le ScanSet de la campagne. Après le retour de S3-E,
une transaction unique retient la relation du groupe courant et avance ce
curseur, avant la progression générique et le checkpoint. La reprise peut alors
transmettre le `capture_id` retenu à S3-E, sans réhoming ni inférence depuis un
chemin, SHA, basename, timestamp, métadonnée ou `image_id`. Une storage class,
borne, dispatch kind/version ou relation durable malformée retourne `CORRUPT`
avant toute mutation et ne fournit jamais un faux `resume_capture_id`.

Chaque séquence matérialise un seul groupe. Pause et annulation restent
coopératives aux frontières de groupe ; après un groupe non terminal,
`sequence_break` rend la réservation et force la réadmission par la Queue et le
Resource Governor existants. À l'ouverture, la registry existante reconstruit
la tâche depuis le Task ID et la requête durable, puis le mécanisme de reprise
existant la soumet à la Queue. Cette tranche n'introduit aucun runtime, queue,
governor, scheduler ou stockage parallèle et reste générique pour les sources
mono-source comme multi-source.

La fenêtre résiduelle acceptée est strictement l'arrêt après le retour de S3-E
et avant la transaction de rétention du Capture. Aucun `capture_id` n'est alors
inféré et aucune garantie exactly-once ne lui est attribuée ; les autres
frontières s'appuient sur la relation retenue et le curseur transactionnel.

### S3-D — Acquisition Pairing Evidence v1

**PASS / FROZEN.** S3-D extrait, depuis des
assets source immuables, un ensemble fixe et borné de métadonnées d'évidence
d'appariement. Pour les RAW, l'extraction est metadata-only avec LibRaw ; pour
les JPEG, elle utilise libexif. Cette évidence n'est pas une identité
scientifique et ne modifie ni les identités existantes ni leur sémantique.

La politique d'appariement v1 est la suivante : entre deux assets distincts,
seul un `ImageUniqueID` caméra connu, non vide et exactement partagé est une
évidence forte, éligible à une association automatique dans une future
intégration. Les timestamp, make/model, numéro de série du boîtier concordant,
dimensions/exposition et basename sont faibles ou corroboratifs : ils ne
constituent jamais une identité. Des `ImageUniqueID` connus contradictoires, ou
des numéros de série de boîtier connus contradictoires, signifient que les
assets sont différents. Une valeur absente n'est pas un conflit. Une ambiguïté
n'est jamais résolue par l'ordre des assets.

S3-D n'effectue aucune écriture en base ni changement de schéma : Project DB
reste v19 et S3-C attach demeure l'unique primitive de persistance. S3-E
réutilise cette évidence sans la redéfinir.

## Phase H — décision Project DB v18

**PASS / FROZEN.** La migration transactionnelle v17→v18
ajoute `derivation_identity` au parent `sparse_reconstructions`, puis crée
`incremental_reconstructions` et `incremental_reconstruction_tasks`.
Les lignes Gate F ont `derivation_identity IS NULL` et gardent exactement leur
`parameter_fingerprint`. Leur tuple candidat historique est imposé par un
index UNIQUE partiel. Les lignes dérivées H stockent le vrai fingerprint H
dans `parameter_fingerprint` et l'identité scientifique H canonique dans
`derivation_identity`, elle-même protégée par un second index UNIQUE partiel.
La reconstruction transactionnelle du parent et de ses tables filles conserve
les IDs, lignes, clés étrangères et cascades v17 sans réécrire les données
scientifiques.
La première table associe atomiquement un snapshot Sparse SfM complet à son
prédécesseur, au Track Set d'extension, au scope de calibration, au kind/version
H, au fingerprint H et à son identité scientifique SHA-256 unique. La seconde
conserve le payload immuable de la tâche durable H. Aucun état de réservation,
snapshot de ressources, géométrie partielle, sous-ensemble d'observations,
remappage de composante ou graphe de filiation n'est persisté. Un redémarrage
recalcule depuis les entrées immuables.

La publication H réutilise sans le relâcher le validateur structurel historique
du snapshot complet. Géométrie et métadonnées H sont insérées dans une même
transaction ; une erreur tardive de métadonnées annule aussi toute la nouvelle
géométrie. Les lignes Gate F v16/v17 restent inchangées et leurs recherches
exactes conservent leur sémantique.

## Gate F — décision historique Project DB v17

**PASS / FROZEN.** La migration historique v15→v16 et le
modèle de reconstruction Sparse SfM v16 restent inchangés. Gate F a fait
avancer la tête de schéma alors courante à v17 par une migration strictement
additive contenant une seule table métier de tâche : `sparse_sfm_tasks`.

Cette table suit le modèle des autres Task Kinds durables : une ligne par
`task_id`, clé primaire et clé étrangère vers `tasks(task_id)` avec suppression
en cascade. Elle conserve les références immuables `track_set_id` et
`calibration_scope_id`, `sfm_kind`, `sfm_version`, puis les 27 valeurs effectives
de `Lardon3DSparseIncrementalParameters`, y compris les sous-structures
relative-pose, PnP et refinement. Les entiers suivent leurs largeurs C
canoniques ; les flottants finis sont stockés en SQLite `REAL` et doivent
retrouver exactement leurs bits binary64 après fermeture/réouverture.

Le Task Kind et sa version restent dans `tasks` et versionnent l'interprétation
du payload. Le fingerprint F0 est dérivé au rechargement et n'est pas dupliqué.
Le checkpoint générique v1 n'est pas modifié. L'écriture du résumé générique et
du payload typé est une transaction unique ; une ligne absente, incompatible
ou invalide interdit la reconstruction runtime sans appliquer de valeurs par
défaut.

Les quatre champs `uint64_t` de domaine complet — limites observations/Tracks
et seeds déterministes relative-pose/PnP — sont chacun un BLOB de huit octets
little-endian avec `CHECK(length(...)=8)`. L'API exige la classe BLOB et la
longueur exacte au rechargement. Les métriques globales persistées restent les
diagnostics Gate F calculés sur toutes les observations retenues du résultat
Gate E final ; elles ne participent pas à l'identité.

La publication Gate F est la projection durable du résultat scientifique
complet : une composante est persistée si et seulement si ses nombres d'images
enregistrées et de landmarks sont tous deux strictement positifs. Une
composante dont le BA est rejeté mais dont la géométrie Gate D reste valide est
persistée exactement ; seule une composante de graphe non reconstruite est
omise. Les comptes et métriques globaux décrivent exclusivement la géométrie
effectivement persistée. Aucun placeholder ni table diagnostique n'est ajouté.

> Version publiée par cette section historique : **v20** ; la tête actuelle
> est v23 comme défini en ouverture du document. La migration transactionnelle
> additive v19→v20 ajoute les records de campagne durable décrits ci-dessus
> sans modifier les
> tables ni identités Capture/Asset de v19. La migration transactionnelle
> v18→v19 ajoute la fondation Capture/Asset Provenance sans modifier les tables
> scientifiques historiques. La migration transactionnelle v17→v18 ajoute le
> discriminateur générique nullable et les deux tables H minimales décrites
> ci-dessus. La migration v16→v17 ajoute le
> payload durable typé `sparse_sfm_tasks`. La migration transactionnelle v15→v16 ajoute
> le modèle persistant Sparse SfM (calibrations, scopes et reconstructions).
> La migration transactionnelle v14→v15 ajoute
> `track_builder_tasks` pour le payload durable explicite du Task Builder. La
> migration transactionnelle v13→v14 ajoute
> les tables `track_sets`, `tracks` et `track_observations` pour le Track
> Model v1. La migration v12→v13 ajoute uniquement `geometric_verifier_tasks`
> pour la tâche durable. La migration v11→v12 ajoute le modèle immutable
> `geometric_verification_results`. La migration v10→v11 ajoute
> `matcher_tasks` pour la tâche Matcher durable. La version v10 publiée ajoute
> uniquement `match_results` pour le Match Result Model. La migration v8→v9
> ajoute la table `candidate_pair_generate_tasks` pour la tâche durable
> Candidate Pair. La migration v7→v8 ajoute la table `candidate_pairs` pour
> le sous-système Candidate Pair.
> Les migrations historiques restent ordonnées et les faults d'injection
> vérifient le rollback.

## Vision

La base de données projet stocke les métadonnées de reconstruction et les relations entre les
entités. Elle est conçue pour être légère, persistante et permettre la reprise après interruption.

## Sparse SfM Gate B (v16)

Le modèle Sparse SfM v1 est publié atomiquement dans les tables
`sparse_calibrations`, `sparse_calibration_scopes`,
`sparse_calibration_scope_images`, `sparse_reconstructions`,
`sparse_reconstruction_components`, `sparse_registered_images`,
`sparse_landmarks` et `sparse_landmark_observations`. Les résultats sont
immuables et les collections volumineuses sont lues par curseurs bornés ; la
base ne charge jamais une reconstruction complète par défaut. Les coordonnées
restent dans le repère arbitraire de chaque composant et les références
d'observation ne dupliquent ni descripteurs ni coordonnées de pixels.
Les relations de suppression disposent d'index enfants dédiés, notamment sur
`calibration_id` dans les membres de scope et `calibration_scope_id` dans les
reconstructions, afin que les vérifications FK restent indexées.

## Structure conceptuelle

### Entités principales

#### Project
- Identifiant unique
- Nom et description
- Date de création
- Configuration
- Chemins des répertoires

#### Scan Set
- Identifiant unique
- Nom de l'acquisition
- Date
- Provenance
- État de traitement

#### Image
- Identifiant unique
- Chemin du fichier
- Métadonnées EXIF
- État de traitement
- Appartenance aux scan sets

#### Feature Set
- Identifiant unique
- Type de descripteur
- Paramètres
- Chemin des données

#### Visual Signature
- Identifiant unique
- Type d'index
- Paramètres
- Chemin des données

#### Candidate Pair
- Identifiant unique (`candidate_pair_id`)
- Image source (`image_id_a`)
- Image cible (`image_id_b`)
- Ordre canonique : `image_id_a < image_id_b`
- Self-pairs interdits
- Unicité persistante
- Date de création (`created_at`)

#### Verified Pair
- Identifiant unique
- Candidate pair source
- Statut (validée, rejetée)
- Métriques

#### Track Set (v14)
- Identifiant unique (`track_set_id`)
- Identité de reuse (builder, verifier, scope)
- Nombre de tracks et GVR
- Immutable après publication

#### Track (v14)
- Identifiant unique (`track_id`)
- Track Set parent
- Nombre d'observations (≥ 2)
- Pas de coordonnées 3D en v1

#### Track Observation (v14)
- Identifiant composite `(track_set_id, feature_set_id, feature_index)`
- Track parent
- Position dans le track (`position_in_track`)
- Feature Set et index de feature

#### Camera
- Identifiant unique
- Modèle
- Paramètres intrinsèques
- Distorsion

#### Pose
- Identifiant unique
- Camera
- Translation
- Rotation
- Qualité

#### Point3D
- Identifiant unique
- Position
- Couleur
- Qualité
- Observations

#### Reconstruction Layer
- Identifiant unique
- Type (sparse, dense, mesh, etc.)
- Provenance
- Transformations
- Qualité
- Chemin des données

#### Measurement
- Identifiant unique
- Type
- Valeur
- Incertitude
- Cible géométrique

#### Document Source
- Identifiant unique
- Type (plan, croquis, etc.)
- Chemin
- Métadonnées

#### Geometric Constraint
- Identifiant unique
- Type
- Paramètres
- Sources
- Poids

#### Artifact
- Identifiant unique
- Type
- État (temporaire, publié)
- Chemin
- Métadonnées

#### Checkpoint
- Identifiant unique
- État du pipeline
- Métadonnées
- Date

## Relations

- Project → Scan Set (1:N)
- Scan Set → Image logique (1:N)
- Image logique → Image Asset (N:1)
- Image → Feature Set (1:N)
- Image → Visual Signature (1:N)
- Candidate Pair → Image (2)
- Verified Pair → Candidate Pair (1)
- Track Set → Track (1:N, CASCADE)
- Track → Track Observation (1:N, CASCADE)
- Track Observation → Feature Set (N:1)
- Camera → Pose (1:N)
- Pose → Reconstruction Layer (N:M)
- Reconstruction Layer → Artifact (1:N)
- Measurement → Point3D (N:1)
- Document Source → Geometric Constraint (N:M)
- Geometric Constraint → Point3D (N:M)
- Artifact → Checkpoint (N:1)

## Invariants

- Chaque entité a un identifiant unique stable
- Les relations sont explicitement définies
- Les artefacts partiels ne sont jamais considérés comme valides
- La reprise commence à la dernière frontière connue

## Frontière avec les checkpoints de tâche

Le modèle durable v1 et son codec fichier sont implémentés indépendamment du
stockage. La base réutilise les mêmes règles de normalisation et de
validation ; elle ne stockera jamais les objets pthread, callbacks, pointeurs,
contrats ou réservations. Le fichier par tâche est une fondation, pas une
Project Database miniature.

La stratégie v1 retient un résumé logique interrogable dans SQLite et une
référence vers le fichier checkpoint. Le fichier checkpoint validé reste la
source complète pour `lardon3d_task_restore()` ; la DB seule ne reconstruit
jamais une tâche. La cohérence compare uniquement les champs que la DB stocke
dans `tasks` : `task_id`, nom, états saved/recovery, progression et compteur de
séquences. Elle ne prétend pas comparer l'estimation complète ni les timestamps
du snapshot. Une divergence de ce résumé ou un fichier invalide interdit la
reprise.

## Schéma v7 implémenté

- `metadata(key PRIMARY KEY, value)` contient `schema_version=7` et
  `next_task_id`, prochain ID durable allouable.
- `project(singleton=1, stable_id UNIQUE, name, created_at, updated_at)` décrit
  l'unique identité logique de la DB.
- `tasks(task_id PRIMARY KEY, name, task_kind, task_kind_version, saved_state, recovery_state, progress,
  sequence_count, started_sec/nsec, finished_sec/nsec, updated_at)` contient le
  résumé durable. Les IDs v1 sont compris entre 1 et `INT64_MAX`.
- `checkpoints(task_id PRIMARY KEY REFERENCES tasks ON DELETE CASCADE, path,
  format_version, durability, updated_at)` représente `DURABLE` ou
  `PUBLISHED_NOT_DURABLE`.
- `artifacts(artifact_id PRIMARY KEY, kind, path, state, size_bytes,
  producer_task_id REFERENCES tasks, created_at, updated_at)` inventorie des
  fichiers externes. Les états v1 sont `STAGED` et `READY`.
- `scansets(scanset_id INTEGER PRIMARY KEY AUTOINCREMENT, name, created_at, updated_at)`
  représente les acquisitions logiques, y compris les ScanSets vides.
- `image_assets(asset_id INTEGER PRIMARY KEY AUTOINCREMENT, sha256 UNIQUE, path UNIQUE,
  size_bytes, state, created_at)` décrit les contenus physiques `READY`.
- `images(image_id INTEGER PRIMARY KEY AUTOINCREMENT, scanset_id REFERENCES scansets,
  asset_id REFERENCES image_assets, original_name, source_path,
  producer_task_id REFERENCES tasks, imported_at)` décrit les images logiques.
  `UNIQUE(scanset_id,asset_id)` interdit les doublons de contenu dans une même
  acquisition sans fusionner deux acquisitions différentes.
- `image_import_tasks(task_id PRIMARY KEY REFERENCES tasks ON DELETE CASCADE,
  source_path, scanset_id REFERENCES scansets)` conserve les paramètres
  métier immuables de `import.images`.
- `feature_assets` conserve SHA-256, chemin content-addressed, taille,
  durabilité et date de publication des Feature Files hors SQLite.
- `feature_sets` relie image, extracteur/version/fingerprint, hash source,
  type/dimension descriptor, compte, producteur et métriques légères
  `occupied_cells`, `total_cells`, `coverage_ratio` et densité/Mpx.
- `feature_extract_tasks` conserve les paramètres immuables ORB historiques.
- `sift_extract_tasks` conserve kind `sift`/`rootsift`, version 1, limites,
  paramètres OpenCV binary64, grille et fingerprint exacts.
- `feature_support_sets` identifie une consolidation immutable, son image, ses
  deux Feature Sets sources, son rayon et son fingerprint.
- `feature_support_groups` conserve position représentative, distance locale
  et `support_count`; `feature_support_members` normalise chaque référence
  `feature_set_id + feature_index`, sans descriptor SQLite.
- `visual_indexes` conserve l'identité `AUTOINCREMENT`, la configuration
  Feature homogène, les paramètres LSH et leurs fingerprints.
- `visual_index_segments` conserve identité `AUTOINCREMENT`, génération,
  SHA-256, chemin, taille, compteurs, durabilité et tâche productrice.
- `visual_index_memberships` a pour clé primaire
  `(visual_index_id,feature_set_id)` et référence le segment immutable.
- `visual_index_update_tasks` conserve `visual_index_id` et le curseur durable.

Les indexes v6 sont
`visual_index_segments(visual_index_id,generation)` et
`visual_index_memberships(visual_index_segment_id,feature_set_id)`. Les FKs
ciblent `visual_indexes`, `feature_sets`, `visual_index_segments` et `tasks`.
Les postings ne sont jamais stockés dans SQLite.

`AUTOINCREMENT` couvre les identités publiées catalogue, Feature Store et
Visual Index. Il
empêche la réutilisation d'un ID issu d'une transaction validée même si sa ligne
maximale est supprimée plus tard. Le coût de `sqlite_sequence` est accepté pour
garantir qu'un futur Feature Store, match ou track ne voie jamais son identifiant
désigner un autre objet. Les IDs de transactions rollbackées ne sont pas
considérés publiés et peuvent être réutilisés.

Les indexes ajoutés en v4 sont `images(scanset_id,image_id)`, pagination réelle,
et `images(producer_task_id,image_id)`, recherche par tâche productrice. Le
SHA-256 et le chemin asset sont déjà indexés par leurs contraintes `UNIQUE`.

## Schéma v8 implémenté

La migration v7→v8 ajoute la table `candidate_pairs` :

```sql
CREATE TABLE candidate_pairs(
    candidate_pair_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(candidate_pair_id>0),
    image_id_a INTEGER NOT NULL REFERENCES images(image_id),
    image_id_b INTEGER NOT NULL REFERENCES images(image_id),
    created_at INTEGER NOT NULL CHECK(created_at>=0),
    CHECK(image_id_a < image_id_b),
    UNIQUE(image_id_a, image_id_b)
);
CREATE INDEX candidate_pairs_image_a_idx ON candidate_pairs(image_id_a);
CREATE INDEX candidate_pairs_image_b_idx ON candidate_pairs(image_id_b);
```

**Invariants** :
- `image_id_a < image_id_b` : ordre canonique garanti par CHECK SQL
- Self-pairs interdits (impliqué par `image_id_a < image_id_b`)
- `UNIQUE(image_id_a, image_id_b)` : unicité persistante
- `created_at` : timestamp Unix secondes, non-négatif

**API** :
- `lardon3d_project_db_create_candidate_pair()` — INSERT avec canonicalisation
- `lardon3d_project_db_load_candidate_pair()` — SELECT par ID
- `lardon3d_project_db_find_candidate_pair()` — SELECT par (image_a, image_b)
- `lardon3d_project_db_list_candidate_pairs()` — SELECT paginé ORDER BY id

**Notes** :
- Le score et la source ne sont pas persistés dans cette version
- La génération est déterministe pour mêmes entrées/configuration
- L'idempotence est garantie par find avant create

## Schéma v9 implémenté

La migration v8→v9 ajoute la table `candidate_pair_generate_tasks` :

```sql
CREATE TABLE candidate_pair_generate_tasks(
    task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id) ON DELETE CASCADE,
    visual_index_id INTEGER NOT NULL CHECK(visual_index_id>0),
    after_feature_set_id INTEGER NOT NULL CHECK(after_feature_set_id>=0),
    top_k INTEGER NOT NULL CHECK(top_k>0 AND top_k<=256),
    minimum_evidence_count INTEGER NOT NULL CHECK(minimum_evidence_count>=0
        AND minimum_evidence_count<=1024),
    scanset_filter INTEGER NOT NULL CHECK(scanset_filter>=0 AND scanset_filter<=2),
    exclude_same_asset INTEGER NOT NULL CHECK(exclude_same_asset IN (0,1))
);
```

**Invariants** :
- `task_id` référence `tasks(task_id)` avec ON DELETE CASCADE
- `after_feature_set_id` : curseur de reprise, non-négatif
- `top_k` : borné entre 1 et 256 (LARDON3D_VISUAL_INDEX_TOP_K_MAX)
- `minimum_evidence_count` : borné entre 0 et 1024
- `scanset_filter` : 0=ANY, 1=CURRENT, 2=OTHER

**API** :
- `lardon3d_project_db_record_candidate_pair_generate_task()` — UPSERT checkpoint
- `lardon3d_project_db_load_candidate_pair_generate_task()` — SELECT par task_id

## Schéma v10 publié

La migration v9→v10 ajoute uniquement `match_results` pour le Match Result
Model. Son schéma et ses invariants restent inchangés.

## Schéma v11 implémenté

La migration v10→v11 ajoute `matcher_tasks`. Cette table conserve uniquement
la configuration immutable et le curseur durable :

```sql
CREATE TABLE matcher_tasks(
    task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id) ON DELETE CASCADE,
    after_candidate_pair_id INTEGER NOT NULL
        CHECK(after_candidate_pair_id>=0),
    feature_extractor_kind TEXT NOT NULL,
    feature_extractor_version INTEGER NOT NULL
        CHECK(feature_extractor_version>0),
    feature_parameter_fingerprint BLOB NOT NULL
        CHECK(length(feature_parameter_fingerprint)=32),
    matcher_kind INTEGER NOT NULL CHECK(matcher_kind BETWEEN 0 AND 2),
    ratio_threshold REAL NOT NULL
        CHECK(ratio_threshold>0.0 AND ratio_threshold<1.0)
);
```

Le curseur est le dernier `candidate_pair_id` checkpointé. Il n'implique ni
continuité des IDs ni liste persistée de Candidate Pairs. La configuration ne
peut pas changer lors d'un UPSERT ; seul le curseur avance.

## Schéma v13 implémenté

La migration v12→v13 ajoute uniquement `geometric_verifier_tasks`. Elle porte
le curseur `after_match_result_id`, les sept paramètres scientifiques v1 et le
fingerprint de contrôle. L'UPSERT autorise seulement le curseur à évoluer ; la
configuration reste immuable. La migration est transactionnelle, son rollback
forcé conserve une vraie v12 sans la table et un retry termine en v13.

Le Match Result ci-dessous reste le contrat publié de v10 :

```sql
CREATE TABLE match_results(
    match_result_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(match_result_id>0),
    candidate_pair_id INTEGER NOT NULL
        REFERENCES candidate_pairs(candidate_pair_id),
    feature_set_id_a INTEGER NOT NULL
        REFERENCES feature_sets(feature_set_id),
    feature_set_id_b INTEGER NOT NULL
        REFERENCES feature_sets(feature_set_id),
    matcher_kind TEXT NOT NULL
        CHECK(length(matcher_kind)>0 AND length(matcher_kind)<=64),
    matcher_version INTEGER NOT NULL CHECK(matcher_version>0),
    parameter_fingerprint BLOB NOT NULL
        CHECK(length(parameter_fingerprint)=32),
    result_status INTEGER NOT NULL CHECK(result_status IN (0,1)),
    match_count INTEGER NOT NULL CHECK(match_count>=0 AND match_count<=8192),
    match_asset_sha256 BLOB CHECK(match_asset_sha256 IS NULL OR
        length(match_asset_sha256)=32),
    match_asset_path TEXT CHECK(match_asset_path IS NULL OR
        length(match_asset_path)>0),
    match_asset_size_bytes INTEGER CHECK(match_asset_size_bytes IS NULL OR
        match_asset_size_bytes>0),
    created_at INTEGER NOT NULL CHECK(created_at>=0),
    CHECK((result_status=0 AND match_count=0 AND match_asset_sha256 IS NULL
           AND match_asset_path IS NULL AND match_asset_size_bytes IS NULL)
       OR (result_status=1 AND match_count>0 AND match_asset_sha256 IS NOT NULL
           AND match_asset_path IS NOT NULL AND match_asset_size_bytes IS NOT NULL)),
    UNIQUE(candidate_pair_id, feature_set_id_a, feature_set_id_b,
           matcher_kind, matcher_version, parameter_fingerprint)
);
CREATE INDEX match_results_candidate_pair_idx
    ON match_results(candidate_pair_id);
CREATE INDEX match_results_feature_set_a_idx
    ON match_results(feature_set_id_a);
CREATE INDEX match_results_feature_set_b_idx
    ON match_results(feature_set_id_b);
```

**Invariants** :
- `UNIQUE(candidate_pair_id, feature_set_id_a, feature_set_id_b, matcher_kind, matcher_version,
  parameter_fingerprint)` : identité déterministe 6 parties
- `candidate_pair_id` référence `candidate_pairs(candidate_pair_id)` avec l'action par défaut
  (NO ACTION)
- `feature_set_id_a` et `feature_set_id_b` référencent `feature_sets(feature_set_id)`
- `feature_set_id_a` appartient à `image_id_a` de la Candidate Pair, `feature_set_id_b` appartient
  à `image_id_b` (validé par l'API create)
- `NO_MATCH` impose `match_count=0` et aucun asset
- `MATCHED` impose `match_count>0` et SHA/path/taille complets
- les échecs d'exécution restent dans le Task Runtime et ne créent pas de ligne
- `matcher_kind` borné à 64 caractères
- `parameter_fingerprint` exactement 32 octets (SHA-256)

**API** :
- `lardon3d_project_db_create_match_result()` — INSERT avec validation des contraintes
- `lardon3d_project_db_load_match_result()` — SELECT par ID
- `lardon3d_project_db_find_match_result()` — SELECT par (candidate_pair_id, feature_set_id_a,
  feature_set_id_b, matcher_kind, matcher_version, parameter_fingerprint)
- `lardon3d_project_db_list_match_results()` — SELECT paginé ORDER BY id
- `lardon3d_project_db_record_matcher_task()` — UPSERT configuration/curseur
- `lardon3d_project_db_load_matcher_task()` — SELECT par task_id

## Schéma v12 implémenté

La migration v11→v12 ajoute uniquement `geometric_verification_results`. Le
parent est un Match Result `MATCHED`; sa validation interligne reste dans l'API.

Schéma abrégé (la chaîne SQL exécutable canonique reste dans `src/project_db.c`) :

```sql
CREATE TABLE geometric_verification_results(
    geometric_verification_result_id INTEGER PRIMARY KEY AUTOINCREMENT
        CHECK(geometric_verification_result_id>0),
    match_result_id INTEGER NOT NULL
        REFERENCES match_results(match_result_id) ON DELETE CASCADE,
    verifier_kind INTEGER NOT NULL CHECK(verifier_kind=1),
    verifier_version INTEGER NOT NULL
        CHECK(verifier_version>0 AND verifier_version<=4294967295),
    parameter_fingerprint BLOB NOT NULL
        CHECK(length(parameter_fingerprint)=32),
    status INTEGER NOT NULL CHECK(status IN (1,2)),
    inlier_count INTEGER NOT NULL
        CHECK(inlier_count>=0 AND inlier_count<=8192),
    inlier_mask BLOB NOT NULL
        CHECK(length(inlier_mask)>=1 AND length(inlier_mask)<=1024),
    model_m00 REAL, model_m01 REAL, model_m02 REAL,
    model_m10 REAL, model_m11 REAL, model_m12 REAL,
    model_m20 REAL, model_m21 REAL, model_m22 REAL,
    created_at INTEGER NOT NULL CHECK(created_at>=0),
    CHECK(/* REJECTED: neuf NULL ; VERIFIED: neuf non-NULL */),
    UNIQUE(match_result_id, verifier_kind, verifier_version,
           parameter_fingerprint)
);
CREATE INDEX geometric_verification_results_parent_idx
    ON geometric_verification_results(
        match_result_id, geometric_verification_result_id
    );
```

Les valeurs stables sont FUNDAMENTAL=1, GEOMETRIC_REJECTED=1 et
GEOMETRIC_VERIFIED=2. REJECTED interdit le modèle ; VERIFIED exige neuf valeurs
finies. Pour les deux états, l'API impose taille canonique, padding nul et
popcount exact du masque, ainsi que `inlier_count <= parent.match_count`.
L'index parent sert la liste paginée ; la contrainte UNIQUE sert le find exact.
Le contrat complet, dont l'ordre des bits, est dans
`geometric_verification.md`.

## Schéma v14 implémenté

La migration v13→v14 ajoute les tables `track_sets`, `tracks` et
`track_observations` pour le Track Model v1. Le schéma complet est dans
`tracks.md`. Schéma abrégé (la chaîne SQL exécutable canonique reste dans
`src/project_db.c`) :

```sql
CREATE TABLE track_sets(
    track_set_id INTEGER PRIMARY KEY AUTOINCREMENT
        CHECK(track_set_id > 0),
    builder_kind TEXT NOT NULL
        CHECK(length(builder_kind) > 0 AND length(builder_kind) <= 64),
    builder_version INTEGER NOT NULL CHECK(builder_version > 0),
    parameter_fingerprint BLOB NOT NULL
        CHECK(length(parameter_fingerprint) = 32),
    verifier_kind INTEGER NOT NULL CHECK(verifier_kind > 0),
    verifier_version INTEGER NOT NULL CHECK(verifier_version > 0),
    verifier_fingerprint BLOB NOT NULL
        CHECK(length(verifier_fingerprint) = 32),
    input_scope_hash BLOB NOT NULL
        CHECK(length(input_scope_hash) = 32),
    gvr_count INTEGER NOT NULL CHECK(gvr_count >= 1),
    track_count INTEGER NOT NULL CHECK(track_count >= 0),
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    UNIQUE(builder_kind, builder_version, parameter_fingerprint,
           verifier_kind, verifier_version, verifier_fingerprint,
           input_scope_hash)
);

CREATE TABLE tracks(
    track_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(track_id > 0),
    track_set_id INTEGER NOT NULL
        REFERENCES track_sets(track_set_id) ON DELETE CASCADE,
    observation_count INTEGER NOT NULL CHECK(observation_count >= 2)
);

CREATE INDEX tracks_set_idx
    ON tracks(track_set_id, track_id);

CREATE TABLE track_observations(
    track_set_id INTEGER NOT NULL,
    track_id INTEGER NOT NULL
        REFERENCES tracks(track_id) ON DELETE CASCADE,
    feature_set_id INTEGER NOT NULL
        REFERENCES feature_sets(feature_set_id),
    feature_index INTEGER NOT NULL CHECK(feature_index >= 0),
    position_in_track INTEGER NOT NULL CHECK(position_in_track >= 0),
    PRIMARY KEY(track_set_id, feature_set_id, feature_index),
    UNIQUE(track_id, position_in_track)
);

CREATE INDEX track_observations_lookup_idx
    ON track_observations(feature_set_id, feature_index, track_set_id);
```

**Invariants SQL** :
- `PRIMARY KEY(track_set_id, feature_set_id, feature_index)` : dans un Track
  Set donné, une observation n'apparaît qu'une fois
- `REFERENCES tracks(track_id) ON DELETE CASCADE` : supprimer un track
  supprime ses observations
- `REFERENCES feature_sets(feature_set_id)` : le Feature Set existe
- `CHECK(observation_count >= 2)` : minimum structurel
- `UNIQUE(builder_kind, builder_version, parameter_fingerprint,
  verifier_kind, verifier_version, verifier_fingerprint, input_scope_hash)` :
  identité de reuse sur `track_sets`
- `ON DELETE CASCADE` depuis `track_sets` : supprimer un set supprime tout
- `UNIQUE(track_id, position_in_track)` : chaque position dans un track est
  unique

**Invariants API** (non protégés par le schéma SQL) :
- `track_set_id` dans `track_observations` correspond au `track_set_id` du
  `track_id` parent
- Une seule observation par image par track (validation via
  `feature_sets.image_id`)
- `feature_index < feature_sets.feature_count`
- `observation_count` cohérent avec le nombre réel d'observations
- `track_count` cohérent avec le nombre réel de tracks

**Statut** : les tables sont créées par la migration et validées par les
tests. L'API C (`create_track_set`, `load_track_set`, `find_track_set`,
`list_track_sets`, `load_track`, `list_tracks`, `find_track_by_observation`)
est exposée et implémentée. Le Track Builder algorithmique et sa tâche durable
sont implémentés par les Gates A–E ; la triangulation reste hors périmètre.

## Schéma v15 — payload durable Track Builder

La migration v14→v15 ajoute uniquement `track_builder_tasks`. Elle ne modifie
aucune table du Track Model et ne change aucune identité scientifique. La ligne
référence un fichier de scope atomiquement publié sous
`.lardon3d/checkpoints/<task_id>.scope` et conserve sa taille, son SHA-256,
son format, le sélecteur exact, le fingerprint Builder, `gvr_count` et
`input_scope_hash`. Le fichier contient `L3DTSCP1`, une version explicite, le
nombre d'IDs et des uint64 little-endian triés et uniques. Le contenu est
rejoué depuis le début après interruption ; aucun curseur ne peut perdre des
arêtes transitoires.

La reconstruction vérifie format, taille, checksum, bornes, tri, unicité,
sélecteur, fingerprint et L3DTSIS1 avant de créer un callback neuf. Une
corruption rend la tâche inexécutable sans créer de Track Set. Les tests
couvrent une vraie base v14, migration, rollback injecté, retry,
fermeture/réouverture et payload corrompu.

## Ouverture et migrations

Une DB vide reçoit la chaîne de schémas jusqu'à v23 dans une transaction
`BEGIN IMMEDIATE`. Une DB v1 reçoit transactionnellement les colonnes nullable
`task_kind` et `task_kind_version`, puis les migrations v2→v3. Les anciennes lignes restent
`NULL/NULL`, sans type inventé et sans perte des projets, tâches, checkpoints ou
artefacts. Une interruption ou erreur provoque un rollback complet. Les DB v1
à v22 sont migrées séquentiellement vers v23.
Une v10 publiée est validée comme telle avant que v10→v11 crée
`matcher_tasks` ; son absence n'est donc pas une corruption. Une version future est refusée et une DB contenant
des tables sans métadonnée de version est considérée corrompue. La fonction
interne de migration applique uniquement la chaîne séquentielle connue jusqu'à
v23 ; une valeur hors de 1..23 est refusée. Les failures injectées de chaque
étape rollbackent les objets et le marqueur de version de cette étape. Par
exemple, la failure v12 rollbacke
la table, l'index et le changement de version, laissant une vraie v11 utilisable.
La failure injectée v13 conserve une vraie v12 sans `geometric_verifier_tasks` ;
un retry applique ensuite v12→v13. La failure injectée v14 conserve une vraie
v13 sans `track_sets` ; un retry applique ensuite v13→v14. La failure injectée
v15 conserve une vraie v14 sans `track_builder_tasks` ; un retry applique
ensuite v14→v15. Le même contrat couvre v16→v22 ; en v23, aucune des neuf
tables optiques ni aucun marqueur v23 ne subsiste après la failure injectée, et
un retry exact converge depuis la v22 intacte.

Migration v1→v2 exacte, exécutée entre `BEGIN IMMEDIATE` et `COMMIT` :

```sql
ALTER TABLE tasks ADD COLUMN task_kind TEXT;
ALTER TABLE tasks ADD COLUMN task_kind_version INTEGER
    CHECK(task_kind_version IS NULL OR task_kind_version > 0);
UPDATE metadata SET value=2
    WHERE key='schema_version' AND value=1;
```

Migration v2→v3 exacte, exécutée entre `BEGIN IMMEDIATE` et `COMMIT` :

```sql
CREATE TABLE image_import_tasks(
    task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id) ON DELETE CASCADE,
    source_path TEXT NOT NULL
);
INSERT INTO metadata(key,value)
VALUES('next_task_id',(
    SELECT CASE
        WHEN COALESCE(MAX(task_id),0)>=9223372036854775807 THEN 0
        ELSE COALESCE(MAX(task_id),0)+1
    END FROM tasks
));
UPDATE metadata SET value=3
    WHERE key='schema_version' AND value=2;
```

Migration v3→v4 exacte, dans la même transaction :

```sql
CREATE TABLE scansets(
    scanset_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(scanset_id>0),
    name TEXT NOT NULL CHECK(length(name)>0 AND length(name)<256),
    created_at INTEGER NOT NULL CHECK(created_at>=0),
    updated_at INTEGER NOT NULL CHECK(updated_at>=created_at)
);
CREATE TABLE image_assets(
    asset_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(asset_id>0),
    sha256 BLOB NOT NULL UNIQUE CHECK(length(sha256)=32),
    path TEXT NOT NULL UNIQUE CHECK(length(path)>0 AND length(path)<4096),
    size_bytes INTEGER NOT NULL CHECK(size_bytes>=0),
    state INTEGER NOT NULL CHECK(state=1),
    created_at INTEGER NOT NULL CHECK(created_at>=0)
);
CREATE TABLE images(
    image_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(image_id>0),
    scanset_id INTEGER NOT NULL REFERENCES scansets(scanset_id),
    asset_id INTEGER NOT NULL REFERENCES image_assets(asset_id),
    original_name TEXT NOT NULL CHECK(length(original_name)>0 AND length(original_name)<256),
    source_path TEXT NOT NULL CHECK(length(source_path)>0 AND length(source_path)<4096),
    producer_task_id INTEGER REFERENCES tasks(task_id),
    imported_at INTEGER NOT NULL CHECK(imported_at>=0),
    UNIQUE(scanset_id,asset_id)
);
CREATE INDEX images_scanset_idx ON images(scanset_id,image_id);
CREATE INDEX images_producer_idx ON images(producer_task_id,image_id);
ALTER TABLE image_import_tasks
    ADD COLUMN scanset_id INTEGER REFERENCES scansets(scanset_id);
INSERT INTO scansets(name,created_at,updated_at)
    SELECT 'Imports antérieurs à ScanSet v1',0,0
    WHERE EXISTS(SELECT 1 FROM image_import_tasks);
UPDATE image_import_tasks
    SET scanset_id=(SELECT scanset_id FROM scansets
                    WHERE name='Imports antérieurs à ScanSet v1'
                    ORDER BY scanset_id LIMIT 1)
    WHERE scanset_id IS NULL;
INSERT INTO metadata(key,value)
    VALUES('legacy_image_catalog_pending',
           CASE WHEN EXISTS(SELECT 1 FROM image_import_tasks) THEN 1 ELSE 0 END);
UPDATE metadata SET value=4
    WHERE key='schema_version' AND value=3;
```

Configuration v7 : `foreign_keys=ON`, `journal_mode=DELETE`,
`synchronous=FULL`, `busy_timeout=5000`. Le mode DELETE convient au propriétaire
unique actuel, évite les fichiers WAL/SHM durables et conserve la synchronisation
forte. Le timeout borne l'attente d'un verrou externe à cinq secondes.

## Concurrence et ownership

Une connexion opaque est sérialisée par un mutex interne. Chaque opération
composée possède sa transaction entière ; aucune transaction publique ne peut
rester ouverte entre deux appels. Aucune I/O d'artefact n'a lieu sous le mutex :
le module confirme que le fichier publié et validé par l'appelant est régulier
avant la mise à jour `READY`. Fermer la DB pendant un
appel concurrent est interdit au propriétaire.

Les records et chaînes sont copiés dans des buffers fournis par l'appelant ;
aucun pointeur SQLite n'en sort. Tous les statements sont finalisés dans
l'appel. La liste de reprise utilise des pages fournies par l'appelant, limitées
à 256 entrées. Elle ne retourne que les tâches normalisées `PENDING` possédant
une référence checkpoint ; le fichier doit encore être chargé et validé.

## Branchement au projet

`Lardon3DAppState` est l'instance projet runtime actuelle et possède exactement
une `Lardon3DProjectDb *` pendant que `project_loaded` est vrai. La DB canonique
est `<project_root>/project.db`. Elle est ouverte à la création/ouverture du
projet et fermée une seule fois par `lardon3d_project_close()`. À l'arrêt de
l'application, la task queue est arrêtée avant la fermeture du projet et de sa
DB. Une fermeture concurrente à un appel projet/DB est interdite au propriétaire.

`project.ini` v2 contient `name`, `stable_id` hexadécimal sur 128 bits et
`version=2`. La même identité est enregistrée dans la table `project`. Toute
divergence est une erreur. Un INI v1 sans identité adopte l'identité d'une DB
existante ; sans DB, une identité est générée une seule fois puis l'INI est
migré atomiquement avant de devenir la référence des ouvertures suivantes. Une
DB existante sans ligne projet ne peut être initialisée que si l'INI possède
déjà son identité.

Les checkpoints sont référencés par chemins relatifs portables :
`.lardon3d/checkpoints/<task_id>.chk`. Leur publication générique met d'abord
en durabilité `.chk.next`, commit ensuite SQLite, puis promeut `.next` vers le
canonique sous `.chk.lock`. Le verrou est consultatif et vivant seulement pour
le processus ; il sérialise writer et reprise, mais n'est pas une donnée de
récupération.

L'inventaire projet utilise sa page DB seulement comme découverte. Après avoir
obtenu `.chk.lock`, il recharge la ligne DB, car elle peut avoir changé pendant
l'attente. Il choisit le canonique codec/version valide dont les champs du
résumé DB correspondent exactement. Ce choix est prioritaire et ignore une
`.next` périmée ou corrompue. Si le canonique ne correspond pas, une `.next`
codec/version valide dont le même résumé correspond est promue sous le verrou
puis utilisée. L'absence de `.next` reste normale pour un projet v22 ou antérieur
contenant seulement `.chk`; toute autre absence, corruption, version future ou
divergence classe seulement la tâche comme non récupérable.

Après copie du record hors mutex SQLite, l'inventaire consulte la registry. Il
distingue `LEGACY_UNTYPED`, `UNKNOWN_TASK_KIND` et
`UNSUPPORTED_TASK_KIND_VERSION`. Aucun reconstructeur métier n'est appelé sous
le mutex DB. Un upsert ne peut pas changer le couple kind/version d'un task ID.

À l'ouverture, le projet lit des pages de 8 dans l'ordre croissant des task
IDs. Chaque record est copié hors mutex DB avant reconstruction et enqueue. Une
fenêtre pleine interrompt le scan sans modifier les tâches restantes. Le résumé
borné expose `inspected`, `resumed`, `skipped`, `failed`, le nombre de
checkpoints `PUBLISHED_NOT_DURABLE` repris et la saturation éventuelle.

Les erreurs d'ouverture/migration, de schéma ou d'identité restent fatales.
Les erreurs propres à une tâche — legacy, kind inconnu/futur, checkpoint
absent/invalide/futur, source indisponible ou reconstruction — sont non fatales.
Un `BUSY` après le timeout SQLite arrête le scan sans boucle et laisse le projet
ouvert.

## Statut

**IMPLEMENTED / VALIDATED / REVIEWED** — SQLite système, tête de schéma v23 et
migrations séquentielles v1→…→v23, identité projet, transactions
tâche+checkpoint, pagination de reprise, artefacts génériques et overlay
optique additif. Les contrats scientifiques et de persistance v16–v22 conservent
leurs statuts historiques PASS / FROZEN.

**IMPLEMENTED / VALIDATED / REVIEWED — Project DB v23** : profils boîtier et
objectif data-driven, alias exacts optionnels, configurations multiples par
campagne/Capture, profils de calibration exactement compatibles et sélection
explicite. La migration ne backfill aucune identité ; les projets v22 restent
récupérables et scientifiquement inchangés.

**PASS / FROZEN** — Project DB v20 : migration additive
v19→v20, record typé de campagne lié au Task ID, requête immuable bornée,
confirmations `CALLER_EXPLICIT`, curseur et relation unique
task/groupe→Capture. La reprise reste celle de la registry, de la Queue et du
Resource Governor existants ; aucune identité de Capture n'est inférée dans la
fenêtre résiduelle pré-rétention.

**IMPLEMENTED** — ouverture/fermeture avec le projet, identité INI/DB cohérente,
publication de checkpoints par le projet et inventaire de reprise validé.

**IMPLEMENTED** — kinds persistants, classification par registry et
reconstruction explicite testée hors scheduler.

**IMPLEMENTED** — allocation transactionnelle de task IDs, paramètres
immuables de `import.images` et reconstruction production explicite.

**IMPLEMENTED** — reprise automatique sélective, pagination de 8, ordre par ID,
fenêtre de queue non bloquante et résumé consultable.

**IMPLEMENTED** — ScanSets, images logiques, assets SHA-256 et pagination
bornée à 256. Les identités sont des `INTEGER PRIMARY KEY AUTOINCREMENT` SQLite
allouées sous transaction ; aucun `SELECT MAX()+1` n'est utilisé en
fonctionnement normal et une identité validée n'est jamais réutilisée.

**PASS / FROZEN** — Capture / Asset Provenance v1 :
Capture borné par ScanSet, associations explicites asset/image, sélection
courante optionnelle et dérivation asset à parent unique. S3-B1/S3-D/S3-E et la
campagne bornée réutilisent cette fondation sans redéfinir ses identités ; la
vidéo reste hors de S3.

La migration v3→v4 ne lit pas `manifest.tsv`. Elle crée le ScanSet legacy et
positionne `metadata.legacy_image_catalog_pending=1` dès qu'une ancienne tâche
d'import existe. Cet indicateur signifie « données legacy potentiellement non
cataloguées », pas « images migrées ».

**IMPLEMENTED** — Track Model v1 : tables `track_sets`, `tracks` et
`track_observations` créées par la migration v13→v14, contraintes SQL
(unicité d'identité de reuse, CASCADE, observation unique par set) et
tests de migration/failure validés. Le schéma complet et les invariants
sont documentés dans `tracks.md`.

**IMPLEMENTED** — Track Builder v1 durable : table `track_builder_tasks`, scope
asset atomique, payload v1 validé, migration v14→v15 et rollback/retry testés.

**IMPLEMENTED** — Sparse SfM v1 persistant : modèle v16 immutable, publication
atomique, composants déterministes, lecteurs bornés, corruption/lifecycle
validation, migration v15→v16 et comparateur fresh/migrated validés par Gate B.
Le modèle de persistance est gelé pour v1 ; le solveur numérique reste hors de
Project DB v16.

Gate C ajoute uniquement des primitives numériques pures hors Project DB; aucune
table, migration ou identité v16 supplémentaire n'est introduite.

**IMPLEMENTED** — API C Track Model v1 : header `project_db.h` et
source `project_db.c` exposent `create_track_set`, `load_track_set`,
`find_track_set`, `list_track_sets`, `load_track`, `list_tracks`,
`find_track_by_observation` et `free_track`. Contraintes et
invariants documentés dans `tracks.md`. Limites réelles : pas de
Track Builder algorithmique, pas de tâche dédiée, pas de
triangulation.

**NOT_YET_WIRED** — autosave à toutes les transitions, retry UI des sources
indisponibles, migration de la TUI legacy et réconciliation des fichiers
orphelins et compaction Visual Index. Feature Store et Visual Index v1 sont implémentés.
Visual Index v1 borne un index à 256 segments de 16 memberships, soit 4096 Feature Sets;
la couverture de 50 000 Feature Sets nécessitera la compaction ou une évolution v2.

**PASS / FROZEN** — Phase H v1 : métadonnées d'identité et
de prédécesseur dans `incremental_reconstructions`, payload durable dans
`incremental_reconstruction_tasks`, snapshot complet publié atomiquement et
réutilisation par identité H. Aucun graphe de dépendances n'est introduit.
