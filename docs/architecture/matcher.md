# Matcher v1 et Match Store v1

## Statut opérationnel

**INTERNAL_PARALLELISM_COMPUTE_RESOURCES_V1 — PASS / FROZEN.**
**COMPUTE_GOVERNOR_V2 — PASS / FROZEN.**
**ORB_VULKAN_ASYNC_EXECUTION — PASS / FROZEN.**

## Contrat

Le pipeline v1 est `ORB → BFMatcher Hamming` ou `SIFT/RootSIFT → BFMatcher L2`,
puis KNN `k=2`, Lowe ratio, tri canonique, Match File content-addressed et Match
Result. Il s'arrête avant toute vérification géométrique.

Pour chaque descripteur A, OpenCV fournit zéro, un ou deux voisins. Zéro ne
produit rien. Un seul voisin valide est accepté. Avec deux voisins, le premier
est accepté exactement si `d1 < threshold * d2`; une seconde distance nulle,
une égalité, NaN, Inf ou une distance négative est rejetée. `-0.0` est accepté
comme zéro conformément à IEEE-754. Indices invalides et
distances du premier voisin non finies ou négatives sont rejetés.

Un `feature_index_a` produit donc au maximum une correspondance acceptée.
Sans cross-check, plusieurs indices A peuvent viser le même index B. L'ordre
final est `(feature_index_a, distance, feature_index_b)` croissant. BFMatcher
fournit une liste KNN par query et le filtre ne conserve que son premier voisin :
la déduplication est donc mathématiquement inutile. Le Matcher rejette toute
sortie anormale contenant deux fois le même index A.

## Match File v1

Header fixe de 32 octets, tous les entiers en little-endian :

```
0..3    octets ASCII exacts "L3DM"
4       format_version = 1
5       descriptor_type (1=U8, 2=F32)
6..7    reserved = 0
8..11   match_count uint32
12..15  descriptor_dimension uint32 (32 ou 128)
16..23  feature_set_id_a uint64
24..31  feature_set_id_b uint64
```

Chaque entrée fait 12 octets : index A uint32, index B uint32, distance float32.
Le format accepte zéro entrée pour tester reader/writer, mais le Matcher ne
publie pas d'asset vide. La borne est 8192 entrées et la taille maximale exacte
est `32 + 8192 * 12 = 98336` octets, environ 96 Kio.

Le reader valide avant toute allocation : magic physique, version, reserved,
type/dimension, borne du compte, calcul de taille, taille physique exacte,
Feature Set IDs A/B sans permutation, indices contre les comptes attendus et
distance finie non négative. Fichiers tronqués, trailing bytes et versions
futures sont rejetés.

## Publication et reuse

Pour `NO_MATCH`, le fichier temporaire est supprimé et seule une ligne sans
asset est créée. Pour `MATCHED` : temp dans `assets/matches`, écriture complète,
`fsync` du fichier, SHA-256, chemin `assets/matches/<préfixe>/<sha256>`, puis
publication atomique par `link`. Une race de même contenu est validée; un
contenu différent déjà présent n'est jamais écrasé lors d'une publication
fraîche. Lorsqu'une ligne existante prouve que son asset content-addressed est
corrompu, le Matcher recalcule puis remplace atomiquement cet asset par `rename`
et répare les métadonnées de la même ligne sous transaction. Le temp est nettoyé
et le répertoire final synchronisé.

Le reuse `MATCHED` exige métadonnées complètes, fichier régulier non symbolique,
taille, SHA-256, header, type/dimension, IDs A/B, compte et entrées valides. Il
n'existe aucun fallback faisant confiance au chemin ou à l'existence seuls.
La validation de reuse lit le fichier borné une seule fois et calcule le SHA-256
sur ce même buffer.

## Bornes mémoire et performance

Les deux buffers de descripteurs contigus occupent au maximum 512 Kio pour ORB
ou 8 Mio pour SIFT/RootSIFT. Les résultats KNN ne contiennent que deux DMatch
par query, les matches filtrés et le Match File sont chacun bornés à environ
96 Kio. Le working set contrôlé du Matcher est donc inférieur à environ 10 Mio,
hors scratch interne borné par OpenCV; aucune structure `A × B` n'est
matérialisée.

Le Match File complet est sérialisé dans un buffer heap borné à 98336 octets et
écrit par un unique `write_exact`, puis synchronisé une fois. Les mesures locales
restent dans le rapport de session, pas dans ce contrat canonique.
À 8192 features, le coût CPU dominant reste l'évaluation exacte des distances
dans `cv::BFMatcher::knnMatch`. ORB peut remplacer ce seul hot path par Vulkan.
Une feasibility réelle a rejeté Vulkan pour SIFT et RootSIFT : leur accumulation
flottante ne garantit pas le top-2 OpenCV sur les égalités adversariales et le
gain n'est présent que sur les grandes paires carrées. Ils restent intégralement
sur BFMatcher CPU.

**INTERNAL_PARALLELISM_COMPUTE_RESOURCES_V1 — PASS / FROZEN.** `matcher.run` v1
orchestre le Matcher sans connaître son backend interne. La
tâche persiste uniquement la configuration, l'identité des Feature Sets à
sélectionner et un curseur Candidate Pair. Une paire est atomique et publiée
immédiatement. Les lots bornés sont séparés par checkpoint et
`task_sequence_break()`. La tâche utilise l'unique Resource Governor avec une
estimation couvrant ce working set ; aucune seconde logique de budget n'est
introduite. Dans un lot, des fenêtres bornées calculent au plus deux paires par
thread CPU effectivement admis, avec un plafond opérationnel validé de douze.
Le callback Queue
compte comme un participant, joint au plus `cpu_threads - 1` enfants, puis
publie seul les stages en ordre `candidate_pair_id`. OpenCV reste à un thread
interne pendant cette séquence, ce qui interdit un fan-out BFMatcher imbriqué.
Le curseur ne suit que le préfixe contigu durable ; le premier échec scelle tout
le suffixe calculé. Sa ligne durable `matcher_tasks` appartient au schéma
Project DB v11 ; le Match Result reste le contrat publié en v10.

Compute Governor v2 fait évoluer seulement le choix opérationnel et la
télémétrie de cette exécution gelée. Un contrat de séquence admis est immutable
jusqu'à libération ; seuls la séquence suivante et son lot peuvent être
adaptés. Admission CPU et admission de lot sont indépendantes : CPU12 avec un
lot de 1 est valide et ne permet pas d'emprunter une paire à la séquence
suivante. Sous un signal de pression qui autorise encore une admission, la
capacité CPU réductible abandonne l'essai et réserve CPU1/lot minimum dans cette
même décision ; un seuil Gate G qui produit `WAIT` reste sans réservation.

Project DB v12 ajoute un enfant `Geometric Verification Result` référencé par
`match_result_id`. Le Matcher ne calcule, ne stocke et ne valide aucun inlier
géométrique ; seul son Match File canonique définit l'ordre indexé par le masque.

### Backend Vulkan ORB

La frontière évaluée remplace uniquement KNN Hamming par un compute top-2 : un
thread GPU par feature A parcourt B, conserve deux indices/distances et applique
le tie-break du plus petit index. Elle ne matérialise jamais A×B. Lowe,
canonicalisation et persistance restent communs. Sur Radeon 780M, la parité
top-2 avec OpenCV est exacte et le gain warm est supérieur à 90 % à 4096/8192.
Le backend de production conserve exactement cette frontière. Sa parité entière
permet au CPU et à Vulkan de partager l'identité persistante. L'évidence
directe de la tranche v2 établit ORB Vulkan comme déterministe, exact et
mesurément supérieur pour ce hot path : il est le workload GPU primaire de la
politique canonique GPU-first. Le CPU reste le fallback portable et de panne.

Les API normales créent AUTO. Pour ORB, elles exposent Vulkan depuis les faits
build, objet backend, GPU et enveloppe UMA sûre ; création et reprise ne sondent
ni n'initialisent le driver sur le caller/main. Le Governor essaie cette
capacité avant le CPU complet, puis le premier `begin` initialise Vulkan sur le
worker Queue déjà contraint. Avant ce worker, le démarrage a établi la politique
sûre `MESA_SHADER_CACHE_DISABLE=true`; elle supprime sur la 780M validée les
helpers de cache Mesa qui élargissaient leur masque. Une paire sous le seuil
n'initialise toujours pas Vulkan. Aucun sweep/latch Matcher ni appel
`sched_*affinity(tid)` auxiliaire n'existe ; le creator/main/TUI reste
inchangé. La frontière d'initialisation du backend vérifie elle-même, sans
`setenv`, que `MESA_SHADER_CACHE_DISABLE` vaut exactement `true` ou `1` avant
tout appel Mesa. Un consumer public tardif avec une valeur absente, fausse ou
malformée reçoit `UNAVAILABLE`, contexte mémorisé indisponible et aucune sortie
partielle ; la metadata non initialisante reste lisible. Les variantes `*_with_mode`
demandent encore explicitement CPU ou ORB Vulkan et conservent leurs erreurs
historiques. Le mode Vulkan demande CPU1, GPU1 et 640 Kio GPU/UMA, débités
exactement une fois de la RAM hôte sur la 780M. Un échec Vulkan pendant cette
séquence reprend la paire top-2 CPU complète sans changer fingerprint ni
résultat. Une panne d'initialisation retire Vulkan des admissions AUTO
suivantes ; une paire sous le seuil validé reste seulement inéligible. Le
rolling conserve explicitement l'état de soumission de chaque paire : une paire
sans handle exécute directement son stage CPU complet et n'appelle jamais
`finish(NULL)`. Toute panne backend est publiée au Governor dès son observation,
avant le fallback, l'annulation ou la publication qui peuvent encore échouer ;
l'inéligibilité locale ne modifie jamais cette santé partagée et n'est jamais
réétiquetée par l'échec d'une soumission ou d'une finition voisine. Le
contrat détaillé est décrit dans
[vulkan_matcher.md](vulkan_matcher.md).
Les coutures privées renvoient séparément le résultat Matcher et la faute
backend. Lecture/allocation locale avant submit, puis filtrage/allocation ou
staging Match File après un finish réussi, produisent un fallback CPU complet
classé `other`. Elles ne désactivent pas Vulkan et ne jettent pas un successeur
soumis sain. Seul l'échec de la transaction backend exacte produit
`backend-failure` et invalide les slots partagés.

La production normale est `AUTO`, choisi par l'unique Governor avant chaque
séquence depuis une enveloppe privée CPU/Vulkan. Les modes explicites
CPU/Vulkan sont des overrides de debug, benchmark et reproductibilité. Le
contrat installé est immutable jusqu'au prochain `sequence_break`. Cette
couture et ses dimensions retenues sont **PASS / FROZEN**.

Le CPU Matcher compte ses participants uniquement dans `cpu_threads=1..12`;
`helpers=0` tant qu'aucun helper GPU distinct n'est réellement admis. Le
diagnostic borné conserve le backend de capacité sélectionné et le backend réel
de la séquence (`CPU`, `ORB_VULKAN` ou plusieurs paires complètes des deux),
avec la raison d'inéligibilité ou de fallback. Ce retour opérationnel ne change
jamais l'identité ou le contenu scientifique.

La couture backend privée expose un snapshot cumulatif thread-safe :
soumissions, complétions, temps CPU de submit/dispatch, attente de fence,
readback, temps GPU issu des timestamps lorsqu'ils existent, intervalle entre
une complétion observée et la soumission suivante, pannes, discards et nombre
de slots actifs. Les compteurs saturent à `UINT64_MAX` au lieu de boucler. Matcher en
dérive par séquence les soumissions/complétions, temps GPU/fence/starvation,
travail CPU de fallback ou de staging et durée de publication durable. Ces
chronométrages n'ajoutent ni worker, ni attente artificielle, ni changement
d'ordre. Une séquence réellement fallback ne forme pas un échantillon de débit
Vulkan pur.

Une nouvelle Task ORB normale persiste une signature `MIXED` avec les champs
CPU12/GPU0 réels ; elle exprime honnêtement que la politique AUTO peut choisir
CPU ou Vulkan par séquence, sans réserver les deux et sans coder un backend
factice. L'override CPU conserve la classe `CPU`. La forme Vulkan explicite est
CPU1/GPU1/640 Kio. Toutes sont à lot `1..12` et 10 Mio par paire. Les signatures historiques
CPU8/GPU0 et CPU1/GPU1 à lot maximal 8, ainsi que les formes CPU12
pré-estimation-par-paire, restent reconnues uniquement pour la reprise et sont
normalisées éphémèrement sans checkpoint d'estimation seule. Une forme voisine
est rejetée ; aucun champ isolé ne sert à deviner le backend. Seule la nouvelle
classe `MIXED` reconstruit AUTO. Toute forme CPU ORB historique ou courante
reconstruit CPU fixe par compatibilité/sûreté des overrides ; Vulkan reste fixe.
Le build portable conserve la signature AUTO mais son enveloppe n'expose que CPU.
Seule une reconstruction `MIXED`/AUTO établit la disponibilité runtime partagée ;
la co-restauration ultérieure de formes CPU, Vulkan ou historiques fixes ne la
modifie pas. L'ordre de restauration ne peut donc pas déclasser une AUTO avant
son admission.

L'évolution Compute Governor v2 n'ajoute aucune dimension de ressources au
fingerprint. `begin/finish/discard` sont privés à
`src/orb_vulkan_backend_internal.h` et absents du header/ABI public. `begin`
transfère au backend les comptes exacts de la requête et un handle
`slot+generation`; `finish` utilise ces comptes, borne `FEATURE_MAX` et ne
consomme que ce slot sur tout résultat, même une capacité de sortie invalide.
`discard` attend sa fence ; une attente en échec
condamne/détruit la session avant une soumission suivante. Le wrapper public
synchrone top-2 reste stable et s'implémente par cette couture. Les objets
pending, fds et temporaires utilisent un nettoyage structuré, les chemins
tronqués sont refusés et aucune exception C++ ne franchit C.

Le propriétaire Queue conserve seul Lowe, canonicalisation, publication et
avancement du curseur. Device/pipeline/layout/cache sont partagés; deux slots
maximum dupliquent seulement command/fence/descriptors/buffers/query. La trace
déterministe prouve à depth 2, pour deux paires successives 769×769,
`GPU_SUBMIT(i) < GPU_SUBMIT(i+1) < GPU_FINISH(i) < PUBLICATION_START(i) <
PUBLICATION_FINISH(i)`, avec cardinalité/sortie intactes.

Le payload mappé suit le contrat de séquence plutôt que le maximum de
l'enveloppe. Un backend frais retient zéro, depth 1 retient exactement 640 Kio
et depth 2 retient 1,25 Mio jusqu'au nettoyage de la séquence; le second slot est
libéré avant la prochaine admission depth 1. Le redimensionnement est interdit
avec une requête pending et une croissance échouée conserve la capacité
antérieure. `backend_info` rapporte la rétention réelle. Une génération arrivée
à `UINT64_MAX` retire définitivement son slot avant tout nouveau submit; elle ne
boucle jamais vers un ancien handle.
Elle couvre aussi échec du begin successeur, faute locale avant/après backend,
échec de publication, annulation et réutilisation du slot. Le lifecycle est
**ORB_VULKAN_ASYNC_EXECUTION — PASS / FROZEN** et helpers reste 0.

Le runner réel opt-in utilise désormais `AUTO` par défaut ; `cpu` et `vulkan`
restent des overrides explicites de debug/benchmark/reproductibilité. Son
pipeline normal est le rolling Governor-owned inflight 1. Un
contrôle `synchronous` force depth 1 et est
compilé uniquement dans ce runner et le test Matcher dédié : pour une capacité
GPU sélectionnée, chaque paire passe par le wrapper public top-2 synchrone,
puis par la même canonicalisation et la même publication avant la paire
suivante. Ce contrôle historique de mesure n'existe pas dans le binaire
`lardon3d`, ne répond à aucun environnement en production et ne devient ni
identité, ni checkpoint, ni capacité Governor. Il est refusé pour CPU explicite
et pour la reprise d'une Task Matcher pendante, car son choix n'est pas
persisté. Les fixtures Vulkan comparent rolling depth 1 forcé, rolling depth 2
forcé et synchrone depth 1 sur des Projects neufs et obtiennent les mêmes champs
scientifiques, SHA et tailles d'assets.

Pour isoler la profondeur du reste de la boucle adaptative, ce même runner
accepte en contexte `--resume-pre-gv-existing` neuf le contrôle privé
`--matcher-inflight 1|2`. Il est limité à `AUTO`; rolling force une capacité
Vulkan unique et fixe min=max à la valeur demandée, avec un lot fixe de deux
par défaut. Le contrôle additionnel `--matcher-batch 2|4|8|12` exige inflight,
AUTO et rolling, puis fixe aussi min=max batch pour une matrice reproductible.
Le Governor choisit et réserve toujours cette capacité depuis son snapshot
courant, y compris 640 Kio par slot et la charge UMA exacte; le contrôle ne
court-circuite aucune admission et n'expose aucune alternative CPU. GPU budget
zéro est refusé avant accès au Project; une capacité GPU/backend/mémoire non
admissible invalide l'expérience au lieu d'exécuter AUTO CPU. Synchronous reste
depth 1 et refuse la valeur 2. La valeur est absente du binaire `lardon3d`, de l'ABI, du payload, du
checkpoint et de l'identité scientifique; le runner la restaure dans
l'environnement sur toute sortie, la refuse sur une Task pendante et l'émet
comme `1`, `2` ou `null` dans le JSON d'évidence; batch est pareillement émis
comme `2`, `4`, `8`, `12` ou `null`. L'agrégat exige uniquement des contrats
sélectionnés Vulkan batch/depth demandés, réservation hôte et payload exacts,
zéro panne,
discard ou slot pending. Il accepte seulement les fallbacks CPU complets dus à
l'inéligibilité locale. Matcher compte chaque item une seule fois, après sa
publication CPU durable, dans les classes local-ineligible/backend-failure/other;
une séquence peut en contenir plusieurs et le CPU sélectionné normalement
n'entre dans aucune classe. Le commit par item est séparé du feedback de débit
de fin de séquence : une annulation ou panne ultérieure conserve le préfixe
durable, mais n'entraîne jamais la séquence avortée. La déduplication bornée est
éphémère à la Task et n'ajoute aucun état de reprise. L'agrégat conserve aussi
les comptes de séquences
pour diagnostic, mais l'égalité inter-cohortes porte exclusivement sur les
items localement inéligibles; tout item backend-failure/other donne
`experiment_valid=false`. Une panne backend tardive peut conserver la preuve
CPU déjà publiée, mais la Task de benchmark échoue après son checkpoint et la
cohorte ne peut pas être déclarée réussie. La mesure forcée ABBA donne
54,661652238 paires/s à depth 1 et 55,797311953 à depth 2 (+2,077617 %), sous
le deadband 5 %. Pour 4113 paires durables par run, le débit de cohorte est
`(2 * 4113 * 1e9) / somme(wall_ns)`, pas la moyenne des débits par run. Les
walls bruts 75326831673/75162582080 et 73662096698/73764360098 ns donnent les
moyennes 75,244706877/73,713228398 s. Fence vaut 6,0684/3,6776 s, starvation
54,4534/50,1465 s, publication 29,2582/30,0548 s, submit CPU
0,2655/0,3818 s, readback 0,0460/0,0873 s et GPU busy max 23/24 %.
Chaque bras A/B garde le digest
`7a9dbc38a23a600379167d55e24836b7acbb22eea25573e7440bdc9e4602b3b3`, quatre
séquences de fallback local par exécution et zéro panne/discard. Depth 2 reste donc validé pour la
sûreté privée (`DEPTH_MAX_VALIDATED_SAFETY=2`) mais est
**REJECTED_WITH_MEASURED_REASON** pour la politique normale
(`DEPTH_MAX_USEFUL=1`). Le corpus établit par ailleurs +10,27 % pour rolling
depth 1 face au contrôle synchrone, avec le même digest. La matrice de batch
runner-only item-valide donne, pour batch 2/4/8/12, les walls A/B bruts
76150272845/75674818393, 61319835797/63138565155,
55148130595/54847191200 et 53446248173/53724786321 ns. La même formule de débit
combiné donne 54,180767704, 66,094373197, 74,784998723 et 76,755814095 paires/s,
soit +21,988624373 %, +13,148812987 % puis +2,635308425 %. Chaque run conserve
4113 paires durables, six items locaux, zéro item backend-failure/other et le
même digest ci-dessus. Batch 12 est donc
**REJECTED_WITH_MEASURED_REASON** sous le deadband 5 % : la sûreté privée reste
`BATCH_MAX_VALIDATED_SAFETY=12`, mais AUTO normal suit
`BATCH_MAX_USEFUL=8`. Les fichiers sont
`forced-batch{2,4,8,12}-items{,-b}.stdout.jsonl`. Les anciens
`forced-batch2-current.stdout.jsonl` et `forced-batch4.stdout.jsonl` restent
historiques : leur comparateur par séquences était invalide et ne fonde pas la
décision.
Le run normal sans option backend/lot/inflight
`short-auto-batch8-governor-v2.stdout.jsonl` suit effectivement
`1 → 2 → 4 → 8`, conserve batch 8 et publie 4113/4113 résultats en
54,066973393 s (76,072 paires/s). Il garde le même digest `L3DMRD1`, six items
locaux, zéro panne/discard, inflight 1 et helpers 0. Ce run est la preuve du
contrôle AUTO; les cohortes forcées ci-dessus restent la preuve de sélection de
l'enveloppe utile.
Le run S21 final normal
`final-s21-auto.stdout.jsonl` publie 172 741 Match Results pour 172 741
Candidate Pairs en 2 345,444485079 s, avec zéro mapping dupliqué, curseur
contigu/complet et digest `L3DMRD1`
`e5128a2e599ff593c4f79850e067254b1f249d19e8480a44973306b1af250f70`.
AUTO reste Vulkan/rolling; 172 507 soumissions ont 172 507 complétions, zéro
panne/discard/pending, tandis que 234 items localement inéligibles sont
recalculés comme paires CPU complètes. L'agrégat compte 21 550 séquences
Vulkan pures, 77 mixtes et 3 CPU, sans jamais publier d'évidence partielle.
La Task 2831 finit `COMPLETE`, progression 100 et `sequence_count=21629`;
le checkpoint final SHA-256 vaut
`636f4f4a20f27308d90142c495c9f6ffc04b4c0dfcca0fdc75cfeb5366ab50b1`.
Les quatre sources retenues sous
`/home/fy59/Documents/Lardon/.real-pre-sfm-2026-08-30/governor-v2-evidence/`
sont `forced-depth1-a.stdout.jsonl`, `forced-depth1-b.stdout.jsonl`,
`forced-depth2-a.stdout.jsonl` et `forced-depth2-b.stdout.jsonl`.

AUTO Vulkan normal adapte le lot seulement dans `1..8`. Il exige huit
observations pures consécutives pour la référence et huit au palier d'essai,
puis accepte seulement un gain moyen d'au moins 5 %. Pression, fallback ou
travail durable nul abandonnent la fenêtre; un ou deux échantillons ne décident
rien. La durée entraînant AUTO couvre la réadmission réussie, le calcul, les
publications ordonnées et le checkpoint générique durable; la seule durée du
noyau Matcher reste diagnostique et ne décide pas le lot. Une séquence dont le
checkpoint échoue ne peut donc pas entraîner le contrat suivant. CPU Matcher
garde sa rampe et son maximum 12. Helpers reste 0 : la
publication owner-only vaut environ 29,5 s dans ces cohortes, et ni le gain
depth 2 sous 5 % ni une autre mesure ne prouve qu'un helper supplémentaire
surmonterait cette frontière de durabilité ordonnée.

Le même runner audite la bijection ordonnée Candidate Pair/Match Result, le
curseur final contigu et chaque Match File par SHA, taille, header et entrées.
Son digest `L3DMRD1` sérialise explicitement en largeurs fixes les IDs
scientifiques, kind/version/fingerprint, status/cardinalité et SHA/taille
d'asset ; Task IDs, timestamps, chemins et sélection opérationnelle sont
exclus. Ce digest est une preuve de comparaison du harness, pas une nouvelle
identité Project DB ou scientifique.

## Déterminisme et fingerprint

Le fingerprint couvre kind, version, `k=2`, threshold float32 et absence de
cross-check. À Feature Files, configuration, implémentation/version OpenCV et
environnement numérique compatibles identiques, SIFT/RootSIFT garantissent les
mêmes paires attendues, l'ordre et la sérialisation stables. Aucune promesse
bit-à-bit cross-platform n'est faite pour les distances L2 flottantes.
ORB/Hamming bénéficie d'une garantie plus forte grâce à sa distance entière.

NEXT: GEOMETRIC VERIFIER v3
