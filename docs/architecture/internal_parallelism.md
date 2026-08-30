# Parallélisme interne borné

## Statut

**INTERNAL_PARALLELISM_COMPUTE_RESOURCES_V1 — PASS / FROZEN.**
**COMPUTE_GOVERNOR_V2 — PASS / FROZEN.**
**ORB_VULKAN_ASYNC_EXECUTION — PASS / FROZEN.**

Ce document décrit le contrat validé pour `features.extract`,
`features.extract.sift`, `visual_index.update`, `candidate_pair.generate` et
`matcher.run`. Le gel v1 porte sur le parallélisme interne borné, les
réservations Governor et les sorties canoniques. Il ne préjuge pas du choix
opérationnel CPU/Vulkan ajouté par la tranche v2.

## Frontière runtime

La Task Queue conserve un seul callback actif. Une Task peut exploiter à
l'intérieur de ce callback les `cpu_threads` effectivement admis par le
Resource Governor. Ce parallélisme interne ne crée ni pool global, ni deuxième
scheduler, ni file de travail persistante. Le Governor dérive désormais un
masque privé depuis l'affinité permise et la topologie package/core Linux. Il
réserve des coeurs physiques complets, frères SMT inclus, en ordre décroissant
package/core. Sur l'hôte unrestricted validé, cela donne `0-5,8-13` pour le
calcul lourd et `6,7,14,15` pour le desktop, l'audio et l'interaction. Un masque
caller déjà borné à la capacité de calcul est repris sans seconde réserve ; si
topologie ou affinité manque, seul le budget portable est conservé et aucun ID
n'est inventé.

L'unique worker Queue applique et vérifie son propre masque (`pid=0`) avant les
callbacks ; le creator/main/TUI reste inchangé et les enfants créés depuis le
worker héritent normalement de son affinité. Aucun TID auxiliaire énuméré n'est
passé à `sched_getaffinity()` ou `sched_setaffinity()` : retenir un
`PIDFD_THREAD` ne réserve pas le numéro TID consommé par ces interfaces. Avant
toute création de pthread applicatif et toute initialisation Vulkan, le
démarrage établit donc `MESA_SHADER_CACHE_DISABLE=true`. L'absence de variable
prend ce défaut sûr et une valeur explicite exacte `true` ou `1` est respectée ;
une valeur explicite fausse ou malformée est préservée mais le démarrage est
refusé. Sur la 780M validée, cette politique supprime les helpers de cache
`*:disk$0` qui élargissaient leur masque, et les threads runtime restants
conservent le compute-pool hérité. Elle est inoffensive hors Mesa, n'est ni
persistée ni scientifique, et son état/sa raison sont diagnostiqués sans faux
indicateur de recontrainte auxiliaire.
Le backend Vulkan possède en plus une barrière tardive non mutante : avant son
premier appel Mesa, une requête non vide exige la valeur exacte `true` ou `1`.
Absent, faux ou malformé produit un backend `UNAVAILABLE` mémorisé et aucune
sortie partielle. `backend_info` et les requêtes vides restent non initialisants.
Cette défense couvre les consumers publics ou de feasibility qui ne traversent
pas la Queue ; seuls leurs `main` autonomes peuvent prendre le défaut sûr avant
tout pthread.
Le compute-pool borne `cpu_threads` à l'admission. Un échec d'application est
diagnostiqué sans altérer Task, Queue, durabilité ou science. Cette couture v2
est **PASS / FROZEN**, sans persistance ni ABI publique. Elle complète le gel
v1 sans le redéfinir.

## Audit GPU — 29 août 2026

Cet audit est une constatation de capacité et une recommandation de périmètre.
La tranche v2 en tire une politique GPU-first seulement pour un backend validé,
déterministe et mesurément supérieur. La machine contrôlée
expose `AMD Radeon 780M Graphics (RADV PHOENIX)`, iGPU Vulkan API 1.4.354 sous
Mesa 26.2.1. OpenCV 5.0.0 y indique le chargement dynamique de Vulkan et
d'OpenCL, avec TBB comme framework parallèle ; ses modules CUDA ne sont pas
disponibles. L'outil `clinfo` n'était pas installé : cette absence ne prouve ni
ne valide une exécution OpenCL. Elle ne constitue donc pas une couture GPU
utilisable par Lardon3D.

La 780M est UMA. Les fichiers amdgpu de l'hôte publient un petit aperture VRAM
de 512 Mio mais un GTT système de 7 986 020 352 octets ; Hardware Profile garde
le premier comme capacité de payload observable et le classe shared d'après ces
preuves bornées, sans liste de device IDs. Toute mémoire de travail GPU, toute
double résidence
CPU/GPU et tout staging host-visible consomment la RAM hôte et doivent être
comptés une seule fois par le Resource Governor, conformément à la règle UMA
du [Resource Boundary](resource_boundary.md#selected-gpu) ; ils ne créent pas
un budget VRAM indépendant. L'admission, la durée de réservation et la
libération restent du ressort du Governor et de la Task admise. Les copies ou
synchronisations hôte↔GPU, y compris sur mémoire partagée, restent un coût de
transfert/synchronisation à mesurer : l'UMA ne les rend pas gratuites.

| Étape | Backend courant / couture GPU existante | Bibliothèque ou API requise si étude ultérieure | Compatibilité, sortie et coût | Recommandation de l'audit |
| --- | --- | --- | --- |
| `features.extract` (ORB/SIFT) | Extraction OpenCV CPU ; aucune couture GPU de production. | Aucune API d'extraction GPU n'est validée. Les indicateurs OpenCV Vulkan/OpenCL dynamiques ne fournissent pas à eux seuls un backend d'extraction ; CUDA est indisponible dans ce build. | Il faudrait prouver l'algorithme, les keypoints, descripteurs, ordre et Feature File produits. Les descripteurs et leurs buffers devraient être résidents ou stagés en RAM UMA ; le transfert et la synchronisation risquent de dominer les images bornées. Bénéfice non mesuré, complexité élevée. | Rester CPU. Une étude ne peut commencer qu'avec une API/backend concret et une preuve d'équivalence. |
| `visual_index.update` | Construction et publication CPU ; aucune couture GPU. | Aucun backend/API GPU validé. | Les postings, leur compaction, tri total et publication déterministe sont aujourd'hui CPU. Un backend devrait préserver exactement `table_id,key24,feature_set_id,feature_index`, le segment, SHA-256 et les memberships ; il ajouterait double résidence UMA et synchronisation pour un bénéfice non mesuré. Complexité élevée. | Rester CPU ; aucun chantier GPU n'est justifié par cet audit. |
| `candidate_pair.generate` | Requête Visual Index, top-K et publication CPU ; aucune couture GPU. | Aucun backend/API GPU validé. | Les scores, top-K, tie-breaks, normalisation et ordre de publication sont canoniques. Une accélération devrait rendre ces résultats identiques malgré les accès DB et les petits résultats bornés ; copies/synchronisations UMA et l'accès persistant réduisent le bénéfice attendu. Bénéfice non mesuré, complexité élevée. | Rester CPU ; ne pas introduire un backend GPU ou une seconde politique de sélection. |
| `matcher.run` | BFMatcher CPU jusqu'à 12 participants ; backend Vulkan ORB exact en conversion begin/finish backend-owned. | Vulkan compute existant pour ORB/Hamming top-2 uniquement. Aucun chemin Vulkan/OpenCL/CUDA validé pour SIFT/RootSIFT. | ORB Vulkan prouve la parité top-2 et Match File complète avec le CPU. La 780M UMA impose de compter buffers et staging une fois en RAM hôte ; SIFT/RootSIFT restent CPU. | Workload GPU primaire validé et supérieur : AUTO GPU-first, CPU fallback. Ne pas étendre à SIFT/RootSIFT ni changer l'identité persistante. |

L'attente « sortie identique » est une exigence de preuve, non une présomption
attachée au GPU. Un futur backend ne peut partager une identité, un fingerprint
ou une version scientifique existants que si l'équivalence complète des sorties
canoniques est démontrée à la frontière concernée. Si ses sorties diffèrent, il
doit recevoir un backend/version et une identité scientifique versionnés, avec
une décision et une validation dédiées ; il ne doit jamais être choisi comme un
fallback transparent. Le mode ORB Vulkan sériel existant est l'exception déjà
prouvée à cette règle d'identité commune.

### Décision GPU finale — Radeon 780M

La validation de production de `matcher.run` distingue l'identité scientifique
de son mode opérationnel. La production normale ORB crée une enveloppe AUTO
CPU12/GPU0 et ORB Vulkan CPU1/GPU1; les API explicites conservent une seule
forme depth 1. Sa signature durable `MIXED` reste CPU12/GPU0 et décrit cette
politique, tandis que l'override CPU persiste la classe `CPU`. Chaque slot vaut
640 Kio: buffers A (256 Kio), B (256 Kio) et top-2 (128 Kio). AUTO normal
réserve 640 Kio à inflight 1. La capacité privée de sûreté/benchmark peut
réserver 1,25 Mio à depth 2. Le backend ne mappe aucun slot avant initialisation,
retient exactement la capacité admise pendant la séquence et libère le second
avant l'admission depth 1 suivante; `backend_info` rapporte cette rétention
réelle. Les 10 Mio de stage restent la mémoire CPU par paire.
Une panne de dispatch reprend entièrement le top-2 CPU avant
toute publication ; elle ne publie jamais une sortie GPU partielle et ne
change ni fingerprint, ni identité Match Result, ni SHA du Match File.

Sur l'hôte contrôlé (`AMD Radeon 780M Graphics (RADV PHOENIX)`, Vulkan 1.4.354,
Mesa 26.2.1), le backend a créé le device et exécuté les dispatchs réels. Les
mesures de kernel chaud, distinctes du coût Task/SQLite/checkpoint, sont :

| Paire ORB | CPU BFMatcher | Vulkan | Accélération Vulkan |
| --- | ---: | ---: | ---: |
| 768 × 768 | 0,67 ms | 0,34 ms | 1,97× |
| 4096 × 4096 | 17,00 ms | 1,62 ms | 10,49× |
| 8192 × 8192 | 67,54 ms | 3,91 ms | 17,27× |

La parité top-2, Match File et publication durable est couverte par une Task
Queue réelle à 769 × 769 descriptors, par une répétition déterministe et par
le fallback forcé. La forme explicite CPU12/GPU0, la signature AUTO `MIXED`
CPU12/GPU0 et la forme Vulkan CPU1/GPU1 utilisent des lots `1..12` et 10 Mio
par paire. La reprise accepte
en plus seulement les anciennes signatures entières CPU8/GPU0 et Vulkan
CPU1/GPU1 à lot maximal 8, puis les formes CPU12 antérieures au coût par paire.
Elles sont des signatures historiques de récupération, normalisées
éphémèrement avant admission ; une forme voisine est rejetée. La configuration
portable `-Dvulkan_orb=disabled` refuse Vulkan et conserve le fallback CPU.

Le gel v1 ne revendiquait pas de comparaison de débit corpus saine sous la
pression hôte alors observée. L'évidence directe apportée à la tranche v2
établit désormais ORB Vulkan comme backend déterministe, exact et mesurément
supérieur pour ce hot path. La politique canonique v2 est donc GPU-first pour
ORB Matcher lorsque son contrat GPU/UMA est admissible, avec fallback CPU.
Cette décision opérationnelle ne rouvre pas le gel scientifique v1 et ne
s'étend pas aux trois domaines GPU rejetés ci-dessous.

Les classifications finales sont :

| Domaine | Classification | Évidence déterminante |
| --- | --- | --- |
| `candidate_pair.generate` | `CANDIDATE_GPU=REJECTED_WITH_MEASURED_REASON` | Le coût est Visual Index, filtrage, branchement et publication SQLite ordonnée; aucune primitive GPU existante ne préserve ces identités et le CPU parallèle Candidate a déjà démontré 7,114× à t12. |
| `features.extract` | `FEATURE_GPU=REJECTED_WITH_MEASURED_OR_IMPLEMENTATION_EVIDENCE` | OpenCV 5.0.0 installé n'expose aucun ORB/SIFT Vulkan/OpenCL utilisable, CUDA est absent, et aucun seam existant ne peut prouver les Feature Files byte-identiques. |
| `visual_index.update` | `VISUAL_INDEX_GPU=REJECTED_WITH_MEASURED_REASON` | Postings, tri total, SHA et publication déterministe sont CPU; aucun kernel GPU borné existant ne couvre cette frontière. |
| `matcher.run` ORB | `MATCHER_GPU=EXISTING_BACKEND_VALIDATED_AND_PREFERRED` | Le backend est exact, déterministe et mesurément supérieur pour le hot path validé. AUTO le préfère quand GPU/UMA sont admis et conserve le CPU en fallback. |

Le comportement de production normal est `AUTO`, choisi par le Governor à
chaque admission de séquence. Les modes CPU/Vulkan explicites restent des
overrides de debug, benchmark et reproductibilité. L'enveloppe privée
CPU/Vulkan, le fallback CPU complet et les diagnostics bornés sont
**PASS / FROZEN** dans les limites validées.

## Extraction OpenCV

Le processus conserve une limite OpenCV globale. Sous l'unique callback Queue,
RAW et Photo Quality appliquent/restaurent CPU1. ORB, SIFT et RootSIFT
Extraction appliquent/restaurent exactement le `cpu_threads` immutable admis
dans `1..min(12, compute_pool)`. Matcher applique OpenCV1 à l'intérieur de sa
propre fenêtre et utilise les participants Task admis autour de cette
primitive. La réservation vit pendant l'exécution bornée et aucun second pool
runtime n'est créé.

Le nombre admis est une politique de ressources, pas une identité scientifique.
Les tests OpenCV 5.0.0 à 1/2/4/8/12 threads obtiennent les mêmes keypoints,
descripteurs et métriques ORB, SIFT et RootSIFT. Aucun fingerprint, format ou
schéma n'est modifié. Les utilisateurs imbriqués d'OpenCV ne peuvent pas faire
varier cette configuration process-wide pendant une extraction. Le callback
assure donc qu'une admission CPU est réellement consommée, sans confondre cette
dimension avec le lot admis.

Le Governor slow-start les dimensions CPU validées selon `1/2/4/8/12`. Chaque
palier utilise deux observations et exige au moins 5 % de débit durable en plus.
Un seul essai CPU ou lot est actif à la fois ; après plafonnement ou refus CPU,
un kind dont le lot est adaptable peut seulement alors explorer le lot. Les
callbacks atomiques ORB/SIFT/RootSIFT publient un item seulement après extraction
et publication durable propre ; READY, `ALREADY_PRESENT` ou
`PUBLISHED_NOT_DURABLE` publie une observation zéro qui ne fait progresser aucun
essai. Visual Index suit la même règle par
segment, tandis que Candidate et Matcher observent chaque séquence. Les formes
fixes restent fixes mais conservent un diagnostic d'admission Governor-owned.

Pour Candidate Generation, l'estimation demande jusqu'à douze threads CPU et
un slot I/O. Le callback de Queue compte comme un de ces threads ; il crée donc
au plus `cpu_threads - 1` threads enfants. Tous les enfants sont joints avant
la fin de la séquence, la libération de réservation ou
`lardon3d_task_sequence_break()`.

## Calcul et publication Visual Index

Une séquence sélectionne le même préfixe durable d'au plus seize Feature Sets
qu'en mode sériel. Jusqu'à douze participants effectivement admis lisent les
Feature Files immuables ; chaque reader, son descripteur de fichier et sa
tranche de 256 descripteurs appartiennent à un seul participant. Aucun enfant
n'utilise le handle Project DB partagé.

Chaque Feature Set écrit dans une tranche disjointe du buffer de postings borné
du segment. Après jointure, le callback compacte seul ces tranches dans l'ordre
de sélection, puis le tri total existant impose
`table_id,key24,feature_set_id,feature_index`. Le callback est l'unique
propriétaire de la sérialisation, du hash, de la publication asset et de la
transaction segment + memberships. Le nombre de participants ne change donc
ni octet, SHA-256, chemin, membership, génération, fingerprint, ni résultat de
requête.

Une erreur de lecture interdit toute publication. Si la création d'un enfant
échoue, le callback calcule après les jointures uniquement les tranches restées
sans producteur. Le curseur n'avance qu'après commit du segment ; le checkpoint
Task suit ce commit, et tous les enfants sont joints avant `sequence_break`.
Le buffer de postings n'est pas multiplié : ses tranches privées partitionnent
la capacité fixe existante. Les coûts supplémentaires sont au plus une pile et
un reader/FD Feature File par participant admis.

## Calcul et publication Candidate

**CANDIDATE_PARALLELISM_IMPLEMENTATION=PASS.** Le parallélisme interne
Candidate est validé lorsque le Governor admet à la fois plusieurs CPUs et un
lot contenant plusieurs memberships indépendants. Les benchmarks CPU 1/6/12
antérieurs restent valides pour leurs intervalles mesurés ; une preuve de débit
durable sur machine réelle exige en plus que chaque frontière observée admette
un lot supérieur à un.

L'admission CPU et l'admission de lot sont deux contrats opérationnels
différents. `desired_cpu_threads` borne le fan-out disponible, tandis que
`batch_size` borne strictement le nombre de memberships que la Task peut
prendre dans la séquence courante. Ainsi, `cpu_threads=12` avec
`batch_size=1` est architecturalement valide mais ne contient qu'une source
indépendante : Candidate calcule un seul participant utile. Il ne doit jamais
emprunter du travail à une séquence ultérieure pour remplir les CPUs admis.

Le test Candidate couvre désormais six Feature Sets et trois fenêtres
productives saines : l'admission initiale, puis deux réadmissions productives
après `sequence_break`, admettent chacune deux sources et obtiennent deux
participants utiles. Une troisième réadmission après `sequence_break` observe
le suffixe vide requis pour terminer. La sortie reste canonique, le curseur
durable est contigu et aucune paire n'est dupliquée. Le même test vérifie le
complément sûr : un lot admis d'un seul item, même avec douze CPUs disponibles,
produit exactement un participant par frontière productive. Cette réduction
est `EXPECTED_RESOURCE_SAFETY_BEHAVIOR`, pas une perte de parallélisme
Candidate.

**REAL_HOST_CURRENT_STATE=GOVERNOR_PRESSURE_THROTTLING_ACTIVE.** Le 29 août
2026, un run S21 réel a conservé son estimate Candidate CPU-12 et son format
de checkpoint courant (`256 KiB + 64 KiB/item`, lot `1..64`) après 237
séquences, mais le Governor a admis des lots d'un sous pression globale. Les
faits retenus sont : zram `6059936 / 6291452 KiB` (~96,3 %), swapfile ~1,92
GiB, tandis que Candidate restait ~55 MiB RSS, `VmSwap=0` et
`MemAvailable` ~7,6 GiB. La disparition ultérieure du swap `/dev/sdb1` vers
14:00 a modifié la configuration de swap de l'hôte, mais elle est postérieure
à l'effondrement initial du lot et n'en est pas la cause initiale prouvée.
Cette décision reflète
des signaux de swap/PSI globaux, non la réservation Candidate elle-même. Les
seuils Gate G ne sont pas modifiés pour améliorer un benchmark ; une preuve
réelle soutenue doit attendre un état hôte qui admet répétitivement
`batch_size > 1`.

Les sources sont les memberships durables du Visual Index, pagés en ordre
croissant de `feature_set_id`. Les IDs peuvent être clairsemés. Une fenêtre
contient au plus `2 * cpu_threads` sources et jamais plus de 24. Chaque résultat
conserve au plus le top-K existant de 256 propositions ; l'estimation de Task
reste conservatrice à 256 Kio fixes et 64 Kio par item admis.

Chaque participant possède son propre handle `project.db` pour les lectures et
le ferme avant de terminer. Le calcul parallèle charge le Feature Set source,
exécute exactement la requête Visual Index existante et forme les paires
canoniques en mémoire. Il ne publie rien.

La reprise normalise uniquement l'estimation Candidate v1 sérielle historique
de forme exacte vers la demande courante de douze participants. Cette
estimation effective reste privée et n'est pas checkpointée avant admission ; le Governor peut
toujours la réduire à son budget disponible. Le Task ID, le curseur typé, le
lot 1–64 et les paramètres scientifiques restent inchangés. Les autres kinds
ne passent pas par cette normalisation Candidate. Matcher possède sa propre
normalisation bornée des deux formes CPU12 historiques, décrite ci-dessous.

Après la jointure complète, le thread propriétaire de la Task publie seul, dans
l'ordre croissant des sources puis dans l'ordre des résultats de requête. Il
emploie le comportement existant `find_candidate_pair` puis
`create_candidate_pair`. Les scores, top-K, tie-breaks, normalisation de paire,
fingerprints et identités scientifiques ne changent pas. Le nombre de threads
ne peut donc modifier ni les identités persistées, ni leur ordre de publication,
ni leur cardinalité.

## Échec, reprise et progression

Une erreur de calcul à la source `S` interdit toute publication de `S` et de
toute source suivante déjà calculée dans la fenêtre. Les sources antérieures
entièrement publiées forment le seul préfixe susceptible d'avancer le curseur.
Une reprise revoit éventuellement un préfixe publié avant un crash mais non
checkpointé ; l'idempotence existante fait converger ce rejeu sans deviner
d'identité.

La progression est le rang du curseur parmi les memberships durables ordonnés,
divisé par leur nombre total. Elle n'utilise ni la valeur numérique du
`feature_set_id`, ni un compteur process-local, ni le nombre de séquences. Elle
est donc monotone pendant l'exécution et reconstruite à l'identique après
reprise pour un même état de memberships. Les valeurs incomplètes sont bornées
à 0–99 ; 100 n'est publié qu'après observation d'un suffixe vide et achèvement
de la Task.

## Calcul et publication Matcher

Les sources Matcher sont les Candidate Pairs durables, pagées en ordre
croissant de `candidate_pair_id`. Une fenêtre contient au plus
`2 * cpu_threads` paires et jamais plus de douze. Le callback Queue compte comme
un participant et crée donc au plus `cpu_threads - 1` enfants ; tous sont
joints avant publication, libération de réservation ou `sequence_break`.

Chaque participant CPU calcule un Match File temporaire privé. OpenCV reste à
un thread interne pendant cette séquence afin que `BFMatcher` ne crée pas un
second fan-out. Le Governor sélectionne avant admission soit la forme
CPU1..12/GPU0, soit ORB Vulkan CPU1/GPU1, 640 Kio UMA et inflight 1.
Les participants
CPU sont décrits par `cpu_threads`; `helpers` reste zéro. Le fallback CPU
complet est choisi avant la séquence si GPU/UMA n'est pas admis ; une panne de
backend ou une paire sous le seuil Vulkan pendant une séquence reprend aussi le
top-2 CPU exact dans la réservation immutable. Le diagnostic distingue backend
sélectionné et backend réel, y compris plusieurs paires complètes CPU/Vulkan.
Ce choix ne
modifie ni fingerprint, ni Lowe ratio, ni correspondances brutes, ni identité
Match Result.

La couture v2 emploie une enveloppe privée et le mode `AUTO` Governor-owned.
CPU/Vulkan explicites restent des overrides de
debug, benchmark et reproductibilité. Une fois une séquence admise, son
contrat est immutable jusqu'à libération ; seul le contrat de la séquence
suivante peut changer. Une Task ORB normale nouvelle persiste la classe
`MIXED` avec ses champs CPU12/GPU0 réels ; elle seule reconstruit AUTO. Les
signatures CPU ORB courantes ou historiques reconstruisent CPU fixe, et les
signatures Vulkan reconstruisent Vulkan fixe. Le build portable conserve
`MIXED` mais expose seulement CPU. Les signatures historiques exactes CPU8,
Vulkan à lot maximal 8 et CPU12 antérieures au coût par paire restent reconnues;
toute forme voisine est rejetée.

Le pipeline Vulkan rolling repose uniquement sur la couture privée interne.
Device/pipeline/layout/cache restent partagés; deux slots maximum dupliquent
command/fence/descriptors/buffers/query et portent chacun un handle
`slot+generation`. Toute fin, sortie invalide, annulation ou erreur nettoie le
handle exact ; un échec d'attente condamne la session. Une instrumentation
bornée prouve sans sommeil `SUBMIT(i) < SUBMIT(i+1) < FINISH(i) <
PUBLICATION_START(i) < PUBLICATION_FINISH(i)` à depth 2, ainsi que les chemins
begin/publish/cancel en échec et la réutilisation. Aucun helper GPU n'est créé.
Le payload mappé suit la capacité de séquence admise : un slot à depth 1, deux
seulement pendant depth 2, sans redimensionnement pending et avec libération du
second avant la prochaine admission. Une génération arrivée à `UINT64_MAX` ne
boucle pas; son slot est retiré définitivement avant toute nouvelle soumission.

Le runner `pre-sfm-real-execution` compile en plus un contrôle de benchmark
synchrone privé. Il force le contrat GPU depth 1 et exécute
`top2 synchrone → postprocess canonique → publication` avant la paire suivante.
Le défaut du runner reste rolling et le défaut Matcher normal reste AUTO ; CPU
ou Vulkan explicites demeurent des overrides. Le contrôle synchrone est absent
du binaire et du comportement de production, n'est pas persisté et ne peut pas
être repris depuis une Task pendante. Il sert uniquement à comparer l'overlap à
sortie identique. Le corpus court retenu démontre +10,27 % pour rolling depth 1
face à ce contrôle, avec digest identique. L'A/B forcé ABBA mesure ensuite
54,661652238 paires/s à depth 1 contre 55,797311953 à depth 2 (+2,077617 %),
sous le deadband 5 %, avec digest identique, quatre séquences de fallback local et zéro
panne/discard dans chaque exécution. Depth 2 est donc
**REJECTED_WITH_MEASURED_REASON** pour AUTO normal; il reste une capacité privée
de sûreté/benchmark; le lifecycle normal est **PASS / FROZEN**.

Le target runner/test compile aussi un contrôle A/B `--matcher-inflight 1|2`,
absent de la production. Pour AUTO rolling, il fixe min=max inflight et batch=2
par défaut. `--matcher-batch 2|4|8|12`, valable seulement avec inflight, fixe
aussi min=max batch dans la même enveloppe privée à capacité Vulkan unique;
l'unique
Governor conserve sélection, réservation UMA et contrat immutable. Un budget
GPU nul est refusé avant ouverture du Project, et une indisponibilité
GPU/backend/mémoire ne peut jamais sélectionner CPU à la place. Synchronous
accepte seulement 1.
Cette dimension opérationnelle n'est ni persistée ni scientifique, ne s'applique
pas à une Task pendante et est rapportée explicitement dans le JSON. Les
fixtures comparent rolling 1, rolling 2 et synchrone 1 sur des Projects neufs
avec sorties exactes. Une paire localement inéligible peut toujours produire sa
preuve CPU complète. Matcher l'attribue exactement une fois après publication
durable à un compteur d'items local-ineligible, backend-failure ou other; le
travail CPU sélectionné n'est pas un fallback. Les compteurs de séquences
et l'apprentissage de débit restent séparés : si la paire suivante échoue en
calcul/publication ou si la Task est annulée, le préfixe durable reste compté,
sans transformer la séquence avortée en observation valide. La cause locale
d'une paire n'est jamais écrasée par une panne begin/finish voisine.
Ils restent diagnostiques, mais seul le nombre d'items localement inéligibles doit
être identique entre cohortes de lots différents. Un item backend-failure/other,
un discard, un slot pending, un contrat différent ou toute admission CPU rend
`experiment_valid=false` et fait échouer la Task de benchmark après checkpoint
de l'éventuel fallback déjà durable. Les logs préliminaires batch 2/4 retenus
montrent quatre contre trois séquences locales pour les mêmes 4113 IDs/digest :
ils prouvent seulement que l'ancien comparateur dépendait du regroupement et ne
participent pas à la décision. La matrice item-valide
`forced-batch{2,4,8,12}-items{,-b}.stdout.jsonl` donne respectivement
54,180767704, 66,094373197, 74,784998723 et 76,755814095 paires/s, avec six
items locaux, zéro panne/autre et le même digest dans chaque run. Les gains de
palier sont +21,988624373 %, +13,148812987 % et +2,635308425 %. Le dernier est
sous le deadband 5 % : AUTO normal conserve `BATCH_MAX_USEFUL=8`, tandis que
batch 12 reste une capacité privée sûre
`REJECTED_WITH_MEASURED_REASON`.
Le contrôle de production sans override est conservé dans
`short-auto-batch8-governor-v2.stdout.jsonl` : la cadence durable complète fait
monter les contrats `1 → 2 → 4 → 8`, puis garde 8 jusqu'au résultat 4113/4113,
à 76,072 paires/s avec digest identique, inflight 1, helpers 0 et zéro panne ou
discard.
La preuve S21 finale sans override conserve exactement ces limites sur
172 741 paires : dernier contrat batch 8/inflight 1/helpers 0, 73,649 résultats
durables/s et zéro panne Vulkan. Un seul épisode YELLOW réduit batch 8 à 1;
après retour GREEN, la progression contrôlée 1 → 2 → 4 → 8 est rejouée.
Depth 2, helpers et batch 12 ne sont jamais admis en production. Le thread
Queue lourd reste sur `0-5,8-13`; `6,7,14,15` restent réservés au desktop.

La création et la reprise AUTO ne sondent plus le backend sur le caller/main.
Elles exposent la capacité depuis les seules métadonnées build/backend/GPU ; le
Governor possède le dimensionnement exact et l'admission UMA sur son snapshot. Le premier
`begin` initialise Vulkan seulement sur le worker Queue déjà contraint, après
établissement pré-pthread de la politique de cache Mesa. Aucun sweep auxiliaire
post-init, latch de Task ou retry de recontrainte par TID n'existe. Une paire
localement inéligible n'initialise pas le backend. Une panne d'initialisation
désactive les admissions GPU AUTO
suivantes, tandis qu'une paire sous le seuil reste une inéligibilité locale et
non une panne.
L'état request-bound distingue un handle soumis de ces deux causes : aucune
paire sans handle ne passe par `finish`. Une panne réelle invalide la
disponibilité Governor avant tout fallback ou sortie précoce ; une inéligibilité
locale conserve cette disponibilité. Les restaurations CPU/Vulkan/historiques
fixes ne peuvent pas écraser l'état établi par une restauration AUTO.

Après jointure, seul le callback propriétaire publie les stages, dans l'ordre
croissant de `candidate_pair_id`, par le chemin atomique Match Store existant.
Une erreur de calcul ou de publication interdit la paire fautive et tout son
suffixe déjà calculé. Le curseur avance exclusivement avec le préfixe contigu
durablement publié. Les temporaires non consommés sont supprimés sur succès,
échec et annulation ; les assets partagés ne sont jamais supprimés par ce
nettoyage. Le rejeu conserve l'idempotence Match Result existante.

## Hors périmètre

- parallélisme entre Tasks ou plusieurs callbacks Queue actifs ;
- pool de workers global ou persistant ;
- modification du schéma Project DB ;
- affinité, pinning ou IDs de CPU dans l'API ;
- modification de la sélection/scoring/fingerprint Candidate ;
- changement des décisions FROZEN du Resource Governor.
