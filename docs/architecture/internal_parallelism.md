# Parallélisme interne borné

## Statut

**PASS / FROZEN — INTERNAL_PARALLELISM_COMPUTE_RESOURCES_V1.** Ce document
décrit le contrat validé pour `features.extract`, `features.extract.sift`,
`visual_index.update`, `candidate_pair.generate` et `matcher.run`. Ce gel porte
sur le parallélisme interne borné, les réservations Governor et les sorties
canoniques ; il ne transforme pas les résultats de microbenchmarks GPU en une
preuve de débit durable sur corpus sous pression hôte.

## Frontière runtime

La Task Queue conserve un seul callback actif. Une Task peut exploiter à
l'intérieur de ce callback les `cpu_threads` effectivement admis par le
Resource Governor. Ce parallélisme interne ne crée ni pool global, ni deuxième
scheduler, ni file de travail persistante. L'affinité CPU éventuelle appartient
au lanceur ou à l'hôte : la bibliothèque ne choisit et ne persiste aucun ID de
CPU.

## Audit GPU — 29 août 2026

Cet audit est une constatation de capacité et une recommandation de périmètre ;
il n'ajoute aucun backend ni aucune politique du Governor. La machine contrôlée
expose `AMD Radeon 780M Graphics (RADV PHOENIX)`, iGPU Vulkan API 1.4.354 sous
Mesa 26.2.1. OpenCV 5.0.0 y indique le chargement dynamique de Vulkan et
d'OpenCL, avec TBB comme framework parallèle ; ses modules CUDA ne sont pas
disponibles. L'outil `clinfo` n'était pas installé : cette absence ne prouve ni
ne valide une exécution OpenCL. Elle ne constitue donc pas une couture GPU
utilisable par Lardon3D.

La 780M est UMA. Toute mémoire de travail GPU, toute double résidence
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
| `matcher.run` | BFMatcher CPU ; couture existante limitée au backend Vulkan ORB sériel. Le mode parallèle CPU ne réserve ni n'utilise le GPU. | Vulkan compute existant pour ORB/Hamming top-2 uniquement. Aucun chemin Vulkan/OpenCL/CUDA validé pour SIFT/RootSIFT. | ORB Vulkan déjà prouve une parité top-2 et Match File complète avec le CPU, ce qui permet l'identité persistante commune. Les dispatchs restent soumis aux buffers descriptors et à la synchronisation UMA. SIFT/RootSIFT Vulkan n'est pas équivalent sur égalités adversariales et reste CPU. | Conserver le chemin ORB Vulkan sériel explicitement admis et le CPU parallèle séparé. Ne pas étendre à SIFT/RootSIFT ni fusionner les modes sans nouvelle validation. |

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
de son mode opérationnel. Les API historiques créent toujours le Matcher CPU
parallèle (`CPU=8`, `GPU=0`). L'API additive de mode explicite peut créer ORB
Vulkan seulement avant admission, avec `CPU=1`, un slot GPU et 640 Kio UMA.
Cette réservation couvre les buffers persistants A (256 Kio), B (256 Kio) et
top-2 (128 Kio); les 10 Mio de stage Matcher restent la mémoire CPU par paire.
Une panne de dispatch reprend entièrement le top-2 CPU avant toute publication;
elle ne publie jamais une sortie GPU partielle ni ne change fingerprint,
identité Match Result ou SHA du Match File.

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
le fallback forcé. La reprise accepte seulement les signatures entières CPU8,
Vulkan CPU1 et les deux signatures CPU12 historiques; celles-ci sont
normalisées éphémèrement vers leur forme courante avant admission. Une signature voisine
est rejetée. La configuration portable `-Dvulkan_orb=disabled` refuse le mode
Vulkan avant allocation de Task ID et conserve le Matcher CPU.

`HOST_PRESSURE_CONTAMINATED=TRUE` pour une comparaison de débit de corpus : au
moment de l'audit, `MemAvailable` était ~7,6 Gio, 7,6 Gio de swap étaient en
usage, même si la PSI mémoire courante était nulle. Il n'existe donc pas de
mesure défendable CPU t1/t6/t12 contre corpus Vulkan dans cette tranche. Le
Matcher CPU durable actuel plafonne en outre à huit participants utiles; un
CPU t12 ne serait pas une comparaison de Task valide. Les chiffres ci-dessus
sont volontairement limités au kernel commun et ne sélectionnent pas une
politique AUTO ou le backend par défaut.

Les classifications finales sont :

| Domaine | Classification | Évidence déterminante |
| --- | --- | --- |
| `candidate_pair.generate` | `CANDIDATE_GPU=REJECTED_WITH_MEASURED_REASON` | Le coût est Visual Index, filtrage, branchement et publication SQLite ordonnée; aucune primitive GPU existante ne préserve ces identités et le CPU parallèle Candidate a déjà démontré 7,114× à t12. |
| `features.extract` | `FEATURE_GPU=REJECTED_WITH_MEASURED_OR_IMPLEMENTATION_EVIDENCE` | OpenCV 5.0.0 installé n'expose aucun ORB/SIFT Vulkan/OpenCL utilisable, CUDA est absent, et aucun seam existant ne peut prouver les Feature Files byte-identiques. |
| `visual_index.update` | `VISUAL_INDEX_GPU=REJECTED_WITH_MEASURED_REASON` | Postings, tri total, SHA et publication déterministe sont CPU; aucun kernel GPU borné existant ne couvre cette frontière. |
| `matcher.run` ORB | `MATCHER_GPU=EXISTING_BACKEND_VALIDATED_BUT_CPU_PREFERRED` | Le backend est réellement admis, dispatché et exact, mais la pression hôte et l'absence d'une comparaison de débit Task/corpus saine interdisent de préférer Vulkan au CPU parallèle comme défaut. |

Cette dernière classification ne rejette pas le backend : ORB Vulkan reste un
mode explicite validé. Elle interdit seulement de présenter les microbenchmarks
comme une preuve que le mode sériel GPU bat le Matcher CPU parallèle dans un
corpus durable complet.

## Extraction OpenCV

Le processus configure une seule fois la limite de threads interne OpenCV,
avant la création de la Queue, au budget interactif audité
`min(12, logical_cpu_count - system_cpu_reserve)`. Les Tasks ORB et SIFT demandent
le plafond douze au Governor, qui réduit l'admission à ce même budget ; la
réservation vit pendant l'exécution bornée d'une
image et est libérée avec la Task. OpenCV possède son fan-out interne, tandis
que la Queue reste propriétaire de l'unique callback actif.

Le nombre admis est une politique de ressources, pas une identité scientifique.
L'audit OpenCV 5.0.0 contrôlé à 1/2/4/8/12 threads obtient des Feature Files
ORB v1 et SIFT v2 byte-identiques. Aucun fingerprint, format ou schéma n'est
modifié. Les utilisateurs imbriqués d'OpenCV qui réduisent temporairement cette
limite doivent restaurer sa valeur avant de libérer leur réservation ; ils ne
peuvent pas faire varier la configuration pendant une extraction concurrente.

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
`2 * cpu_threads` paires et jamais plus de huit. Le callback Queue compte comme
un participant et crée donc au plus `cpu_threads - 1` enfants ; tous sont
joints avant publication, libération de réservation ou `sequence_break`.

Chaque participant calcule un Match File temporaire privé. OpenCV est configuré
à un thread interne pendant la séquence afin que `BFMatcher` ne crée pas un
second fan-out sous les participants admis. Le backend Vulkan ORB partagé reste
utilisé uniquement par un mode déclaré sériel avant admission, dont l'estimation
réserve un thread CPU et le GPU. Le mode parallèle reste CPU-only même si le
Governor ne lui accorde finalement qu'un participant ; il ne sélectionne donc
jamais tardivement une ressource non réservée. Ce choix runtime ne modifie ni le
fingerprint, ni Lowe ratio, ni les correspondances brutes, ni l'identité Match
Result.

Les APIs historiques sélectionnent toujours le mode CPU parallèle ; le mode
ORB Vulkan est demandé par une API additive explicite et n'est jamais activé
par variable d'environnement ou réduction Governor. Sa création exige ORB, un
GPU sélectionné et un backend présent. Après reprise, la forme immutable GPU
peut conserver le fallback CPU exact si le backend runtime n'est plus
disponible. Les signatures courantes complètes sont CPU8/GPU0 et
CPU1/GPU1/640 Kio ; les signatures historiques CPU12 correspondantes sont
normalisées éphémèrement comme un tout avant admission. Toute forme
voisine est rejetée afin de ne pas inférer un mode depuis un champ isolé.

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
