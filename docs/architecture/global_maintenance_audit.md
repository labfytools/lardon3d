# Global maintenance audit — état consolidé

## Lifecycle et portée

`GLOBAL_MAINTENANCE_AUDIT=PASS/FROZEN`.

Les tranches décrites ci-dessous, y compris l'observatoire TUI, F10 et le
raccordement SSD/Governor, sont implémentées, validées opérationnellement et
relues. Les builds Clang frais portable et Vulkan, les suites 64/64 et 65/65,
les sanitizers applicables, TSan, les probes de headers publics et d'ABI sont
acquis. L'unique revue finale indépendante GPT-5.6 SOL/ULTRA du dépôt consolidé
a ensuite conclu PASS sans finding bloquant. La gate de maintenance est donc
fermée et gelée ; ce statut ne réinterprète aucune science FROZEN et n'exécute
pas la tranche scientifique suivante.

Ce record réconcilie l'état courant sans rouvrir les contrats scientifiques
FROZEN. Les détails normatifs restent dans les documents de domaine :

- [Project Database](project_database.md) pour les migrations, identités et
  transactions ;
- [Resource Governor](resource_governor.md) pour l'admission et la matrice des
  quatorze Task kinds ;
- [Task](task_system.md), [Queue](task_queue.md) et
  [runtime](runtime.md) pour ownership, reprise et concurrence ;
- [parallélisme interne](internal_parallelism.md) et
  [Geometric Verifier](geometric_verifier.md) pour les bornes CPU et la science ;
- [Resource Boundary](resource_boundary.md) pour la séparation des systèmes ;
- [roadmap](../roadmap/roadmap.md) pour l'ordre des tranches.

## Positionnement produit et identité des preuves

Lardon3D est un moteur de photogrammétrie générique, persistant et sensible aux
ressources. Sony A6000, Samsung S21 FE, Ryzen 7 8845HS et Radeon 780M sont des
fixtures, corpus ou hôtes de validation. Ils ne sont ni une identité produit,
ni une branche de code métier, ni une limite portable de caméra, CPU ou GPU.

Les identités restent celles des contrats existants : Capture, Asset,
`image_id`, Task ID, groupe de campagne, profil de boîtier, profil d'objectif,
configuration optique et calibration sont distincts. Un chemin, basename,
SHA-256, champ EXIF ou modèle d'appareil n'est jamais promu implicitement en une
autre identité.

## Project Database v23 et contexte optique générique

La tête de schéma courante est **Project DB v23**. La v23 est une migration
additive au-dessus de la fondation scientifique et de persistance v22 gelée.
Elle ne réécrit aucune ligne scientifique v16–v22 et crée neuf tables vides :

| Table v23 | Concept possédé |
| --- | --- |
| `camera_body_profiles` | profil de boîtier explicite |
| `camera_body_aliases` | alias metadata make/model exact, binaire |
| `lens_profiles` | profil d'objectif manuel, électronique ou intégré |
| `lens_profile_aliases` | alias metadata d'objectif exact et optionnel |
| `optical_configurations` | boîtier + objectif + focale exacte optionnelle |
| `acquisition_campaign_group_optics` | configuration explicite d'un groupe de campagne |
| `capture_optical_configurations` | configuration retenue pour un Capture et sa provenance |
| `optical_calibration_profiles` | calibration existante compatible avec une configuration exacte |
| `capture_calibration_selections` | sélection explicite de calibration pour un Capture |

La migration v22→v23 s'exécute dans la transaction de migration existante. Le
marqueur de version n'est publié qu'après création de toutes les tables ; tout
échec rollbacke l'ensemble. Elle n'infère et ne backfill aucun profil depuis
EXIF, chemin, nom, SHA-256, dimensions, calibration historique ou marque
d'appareil. Une base v22 migre donc avec les neuf tables optiques vides.

Un objectif manuel sans EXIF est un cas normal : un profil explicite peut ne
posséder aucun alias metadata. Le Meike manuel utilisé dans les tests est une
preuve de ce chemin générique, pas un modèle hardcodé. Une campagne peut
assigner des configurations différentes à ses groupes ; lors de la rétention
du Capture, le binding campagne/configuration et l'avancement du curseur sont
atomiques. Un Capture importé hors campagne peut recevoir une affectation
explicite distincte, sans valeur « inconnue » fabriquée.

Une calibration optique référence une calibration sparse immuable existante et
une seule configuration exacte. Il n'existe ni interpolation, ni fallback par
focale proche, ni sélection automatique. Le Capture et le profil doivent
référencer la même configuration ; plusieurs profils compatibles restent
ambigus jusqu'à une sélection explicite. Les retries exacts convergent, les
conflits valides sont des contraintes et les dépendances durables malformées
sont de la corruption.

Les migrations exercées via l'API production sur des copies des projets S21 et
A6000 ont atteint v23 avec `integrity_check` et clés étrangères propres, comptes
scientifiques inchangés et tables optiques vides. Les SHA-256 des projets source
sont restés inchangés. La validation v23 retenue comprend la suite normale
57/57, dix tests focalisés, cinq cibles ASan/UBSan, les contrôles de headers C17
et C++, les scénarios de rollback/retry et une revue indépendante après
correction. Cette preuve ne réinterprète aucune calibration réelle des deux
campagnes.

## Resource Governor portable

Le Resource Governor demeure l'unique propriétaire de l'admission RAM, CPU,
GPU et I/O et le seul orchestrateur des leases scratch de production. Le
contrôleur SSD décrit plus bas est une frontière physique ; il n'est ni un
scheduler, ni un second Governor, ni une source de capacité RAM.

La politique hôte courante réserve d'abord le système :

- quatre CPU logiques lorsque l'hôte le permet, tout en conservant au moins un
  CPU de calcul sur un petit hôte ;
- quand la topologie est fiable, un ensemble déterministe de coeurs physiques
  complets, frères SMT inclus, avec le dépassement minimal de la cible logique ;
- les CPU déjà exclus par une affinité externe comptent dans la réserve hôte,
  afin de ne pas soustraire deux fois la même capacité ;
- sans topologie ou masque fiable, le fallback ne fabrique aucun ID ni masque :
  il conserve seulement un budget de compte portable ;
- environ 3 Gio de `MemAvailable` comme réserve dure sur un hôte capable ; la
  bande 3–4 Gio est une prudence YELLOW qui bloque la croissance mais ne
  soustrait pas 4 Gio à la capacité ;
- sur un petit hôte RAM, une réserve fractionnaire déterministe permet une
  dégradation sûre sans prétendre conserver 3 Gio inexistants ;
- PSI CPU/mémoire/I/O et deltas actifs de swap pilotent la pression ;
  l'occupation historique du swap ne suffit pas ;
- la mémoire UMA est débitée exactement une fois et swap/zram/SSD ne deviennent
  jamais un objectif de travail ni une extension de la RAM admise.

Il n'existe plus de plafond CPU global à 12. Le Governor borne chaque contrat
par le compute-pool réel et par la capacité intrinsèque du Task kind. Les IDs
`0-5,8-13` et la réserve `6,7,14,15` restent la preuve historique correcte de
l'hôte 16-CPU validé, pas une politique portable.

## Capacités des quatorze Task kinds

La matrice chiffrée complète appartient au
[Resource Governor](resource_governor.md#audit-des-14-kinds-de-production).
La classification courante est :

| Kind v1 | CPU et lot courants | Nature de la borne |
| --- | --- | --- |
| `raw.develop` | CPU 1, lot 1 | opération atomique ; OpenCV global appliqué/restauré, allowance RAW 2 Gio |
| `photo_quality.triage` | CPU 1, lot 1 | un groupe borné ; aucune preuve de scaling utile |
| `acquisition_campaign.run` | CPU 1, lot 1 | orchestration d'un groupe ; coût JPEG/RAW exact, aucun Task imbriqué |
| `import.images` | CPU 1, lot 1..32 | copie/hash I/O-bound, réadmission par lot |
| `features.extract` | CPU 1..compute-pool, lot 1 | API OpenCV positive-`int`, plafond hôte Governor |
| `features.extract.sift` | CPU 1..compute-pool, lot 1 | même contrat OpenCV ; historique CPU12/CPU1 accepté exactement |
| `features.extract.rootsift` | CPU 1..compute-pool, lot 1 | même contrat OpenCV ; aucune voie GPU validée |
| `visual_index.update` | CPU 1..16, lot 1..16 | maximum algorithmique du segment de 16 Feature Sets |
| `candidate_pair.generate` | CPU 1..64, lot 1..64 | maximum algorithmique/ressource d'un batch de 64 sources |
| `matcher.run` | CPU sûr 1..12 ; lot AUTO utile 1..8, sûr 1..12 | batch Matcher mesuré ; Vulkan ORB exact, SIFT/RootSIFT CPU |
| `geometric_verifier.run` | CPU utile 1..8, fenêtre CPU sûre 16, lot 1..16 | outer-parallelism opérationnel ; USAC interne reste `isParallel=false` |
| `track_builder.run` | CPU 1, lot 1 | rebuild DSU complet, publication atomique owner-only |
| `sparse_sfm.run` | CPU 1, lot 1 | contrat scientifique/atomique FROZEN, BA à un thread |
| `incremental_reconstruction.run` | CPU 1, lot 1 | recomputation atomique FROZEN depuis entrées immuables |

Candidate utilise au plus `cpu_threads-1` enfants et un handle DB privé par
participant, le callback Queue étant lui-même participant. Ses piles et états
sont facturés à 8 Mio par item ; `batch=1` ne crée pas de travail vide.
Visual Index conserve ses buffers de segment fixes et publie seul après toutes
les jointures. ORB/SIFT/RootSIFT utilisent `INT_MAX` seulement comme borne de
l'API OpenCV : l'admission réelle reste le compute-pool. Les signatures CPU12
historiques sont des formats de reprise exacts et ne réintroduisent aucun
plafond portable.

La même discipline vaut à la frontière géométrique Sparse SfM : les champs
publics `uint32_t` `max_iterations` et `minimum_inliers` de relative pose et PnP
doivent être représentables par le `int` OpenCV, donc être au plus `INT_MAX`
(`max_iterations` reste strictement positif). Une valeur supérieure est
refusée avant narrowing, allocation, appel solveur ou mutation des sorties.
L'encodage F0 reste `u32`, et tous les paramètres FROZEN représentables,
valeurs de référence, fingerprints et résultats scientifiques restent
inchangés : cette borne est opérationnelle/ABI externe, pas une nouvelle
politique scientifique.

Le Geometric Verifier prépare désormais des Match Results indépendants en
parallèle, puis le propriétaire unique publie dans l'ordre et checkpointe un
préfixe contigu. Sur le corpus représentatif réel de 4 113 parents, les sorties,
IDs canoniques et le digest scientifique
`9401ef6168804b6f1d51f4cdf64cd6b33cbebd2934e5294c8feacc87f9c8ce86`
restent identiques. Les débits Task complets CPU1/2/4/8/12 sont
60,7514/83,9556/104,7545/116,6329/119,7606 parents/s. CPU12 n'ajoute que
2,68 % sur CPU8, sous le seuil d'acceptation 5 % : le plafond utile est 8,
la fenêtre sûre et le lot maximal restent 16. La preuve S21 complète reste une
preuve historique CPU1/batch8 valide : 172 741 parents traversés, 172 275
applicables, 24 065 vérifiés et 148 210 rejetés en 3 221,758 s, sans doublon,
avec reprise/SIGKILL validée. Elle n'est pas présentée comme un rerun du
nouveau chemin. Le manifest retenu de la preuve outer-parallel a le SHA-256
`52a4412299c74050a66d5690122a793c9451c79faf47e32b6e65a5958f804856` :
il prouve littéralement les 4 102 lignes applicables, leurs IDs 1..4102,
statuts, modèles, masques et compteurs, pas seulement un wall. Un second run
complet S21 de 3 221 s n'est pas requis pour accepter cette maintenance : le
run S21 acquis porte déjà la science/persistance v3 gelée, tandis que la preuve
réelle bornée et les tests de publication/reprise discriminent exactement le
seul changement, l'ordonnancement externe de préparation.

## Task, Queue, persistance et concurrence

Une seule Queue possède un worker et un callback actif. Le Governor admet la
séquence ; la Task possède son état d'exécution ; la Queue possède seulement
ordre/backpressure et, après retour du callback terminal, une histoire de
64 snapshots maximum. Task et userdata sont détruits hors mutex Queue et
seulement après la fin du callback.

La fermeture Queue ferme d'abord l'ingress, attend le worker et tous les appels
déjà enregistrés, puis détruit exactement une fois. Comme pour tout pointeur C
brut, le propriétaire doit empêcher le début de nouveaux appels après le début
de la destruction. Les callbacks terminaux peuvent observer les API read-only,
mais ne doivent pas retirer synchroniquement leur propre entrée, détruire leur
Queue ou attendre une opération dépendant de leur propre retour.

Les IDs Queue générés sont monotones et leur épuisement à `UINT64_MAX` est
sticky ; ni un ID restauré plus petit, ni l'éviction de l'histoire ne réarme la
génération. La reprise restaure état métier et estimation durable, jamais
thread, callback, pointeur, réservation ou contrat actif. Les signatures de
ressources historiques explicitement reconnues sont normalisées seulement en
mémoire ; aucun checkpoint « estimate-only » n'est publié. Les curseurs métier
et la publication scientifique restent les seules frontières de progression.

Ouvrir, fermer ou changer de projet constitue une frontière de session exacte.
Les vues libèrent leurs borrows, puis la Queue est annulée, jointe et détruite
avant Project DB ; une unique Queue vide est ensuite recréée et les observateurs
sont rebondés. Les callbacks terminaux peuvent donc finir avec leur DB encore
vivante, et histoire/namespace Queue d'un projet ne fuient pas dans le suivant.

## Observatoire et centre de contrôle TUI

**CURRENT / VALIDATED OPERATIONAL.** ncurses, l'entrée et le rendu appartiennent
uniquement au thread principal. Un modèle de vue pur reçoit des copies bornées
et ne mute ni Queue, ni Governor, ni DB, ni science. L'observateur coalesce les
captures ordinaires pendant au moins une seconde et couvre exactement le pire
cas production : 64 pending, une active et 64 historiques, soit 129 Tasks. Les
ABI historiques `TaskSnapshot`, `ResourceSnapshot`, `AppState` et layout sont
préservées ; les identités typées, comptes durables, contrats installés,
SwapTotal et vues riches passent par des types/fonctions additifs.

La progression exacte emploie toujours le préfixe métier durable quand il est
connu. Un terminal 2/7 est une erreur d'intégrité et un terminal typé sans
compteur scientifique reste indéterminé ; aucun des deux ne devient un faux
100 %. L'EWMA/ETA exclut le préfixe repris et exige deux intervalles positifs ;
les états calculating, indeterminate, stalled, throttled et complete restent
explicites. Le pipeline expose Acquisition, RAW, Quality, Features, Visual
Index, Candidate, Matcher, GV, Tracks, Sparse SfM et Dense, avec
`NOT_READY/READY/QUEUED/RUNNING/THROTTLED/BLOCKED/COMPLETE/FAILED/NOT_APPLICABLE`.
Dense future reste `NOT_APPLICABLE` et n'est jamais montrée en cours.

Le panneau ressources présente CPU actif/admis/disponible et sa raison,
GPU/mémoire/busy/backend, RAM/MemAvailable/réserve, swap total/utilisé/deltas,
lot/inflight/helpers/I/O/scratch et Governor GREEN/YELLOW/RED. Le contrat de
l'exacte Task active est prioritaire ; backend/inflight/helpers ou raison
restent `UNKNOWN` lorsqu'une association Task+séquence n'est pas prouvée.
L'espace scratch enregistré auprès du Governor est présenté séparément des
détails physiques contrôleur et n'est jamais additionné à la RAM.

Les layouts validés sont full à partir de 100×30, compact à 72×20 et jusqu'au
minimum 60×15 ; en dessous, seul `Terminal trop petit` est rendu. Vert/jaune/
rouge/cyan/bleu/magenta et dim/bold ont toujours des libellés de repli sans
couleur. `F10 SSD` est un segment littéral garanti à 60 colonnes dans les modes
idle, saisie et import. L'aide est contextuelle : saisie = Enter/Échap/F10,
import actif = X/F10 avec q/Échap désactivés, et les écrans idle n'annoncent que
les touches réellement traitées.

L'écran optique effectue seulement des lookup metadata exacts, accepte le
Meike manuel sans EXIF comme cas normal, crée de nouveaux profils/configurations
immuables et affecte explicitement groupe de campagne ou Capture. Il liste et
sélectionne uniquement les calibrations exactement compatibles ; unresolved,
absence, ambiguïté, incompatibilité, BUSY, I/O et corruption restent visibles.
Les pages de 16 rapportent leur compte local et l'existence exacte d'une suite ;
`[` revient à la première page, `]` avance et `R` retente explicitement.

## Contrôleur SSD externe optionnel

Le contrôleur SSD est la frontière physique synchrone et bornée vers UDisks2
via GIO/GDBus. Il découvre exactement les labels `LARDON_SWAP` et
`LARDON_SCRATCH`, exige des UUID stables et une même identité UDisks Drive, et
ne dépend jamais d'un `/dev/sdX`, modèle, numéro de série ou object path comme
identité produit. Un renommage de device node ne change donc pas l'identité.

Les états publics sont `ABSENT`, `DETECTED`, `ENABLING`, `ENABLED`, `IN_USE`,
`DRAINING`, `SAFE_TO_UNPLUG` et `ERROR`. Les snapshots sont des copies bornées ;
les valeurs inconnues restent explicitement inconnues. Le poll ordinaire est
coalescé autour d'une seconde. Le chemin production n'appelle pas `statvfs`
depuis le poll synchrone : un scratch monté peut donc rapporter total/libre
inconnus.

La conversion vers le Governor valide l'état complet et échoue fermée. Toute
paire ou autorité (`pairing_valid`, allocation, enable/disable/cancel, ou état
`ENABLING/ENABLED/IN_USE/DRAINING/SAFE_TO_UNPLUG`) exige la détection actuelle
du Drive et des deux partitions, les trois identités exactes non vides, des
tailles de partition connues strictement positives et des faits mount/activité/
capacité cohérents. `ABSENT` rejette toute télémétrie active, montée, louée ou
capacitaire ; un `DETECTED` partiel reste visible mais non actionnable. Un
hazard `ERROR` déconnecté peut conserver son tuple pour la sécurité, jamais
pour allouer ; `can_disable` n'est acceptable qu'après reconnexion prouvée de
l'exact tuple original.

L'activation appelle directement UDisks et exige le point de montage exact
`/mnt/lardon-scratch`. Elle ne formate, partitionne, répare, fsck, poweroff ni
supprime rien. Avant toute action potentiellement side-effecting, l'ownership
physique est lié conservativement au tuple Drive+deux UUID. Un timeout ou une
vérification indéterminée laisse un hazard sticky ; aucun disque de remplacement
ne reçoit alors Start/Mount/Stop/Unmount. Seul le même tuple reconnecté peut
être vérifié et drainé jusqu'à `SAFE_TO_UNPLUG`.

Le scratch utilise au plus 64 leases explicites. L'adresse de l'objet lease
fait partie de son ownership process-local : copie, reconstruction, objet
étranger, stale ou double release ne décrémentent rien. `DRAINING` refuse de
nouvelles leases. Le swap n'est arrêté que si les leases sont nulles, que les
PSI et deltas swap sont calmes, et que son usage peut être absorbé tout en
conservant la réserve dure de 3 Gio. La propriété UDisks
`Swapspace.Active` doit être un booléen exact connu ; absence ou mauvais type
échoue fermé. Aucun arrêt forcé n'existe.

Le contrôleur ne crée ni thread de polling, ni scheduler, ni capacité Task
scratch implicite. L'adaptateur TUI possède au plus un thread joinable pendant
un poll ou contrôle UDisks borné ; il ne touche jamais ncurses. F10 consomme
exclusivement `can_enable`, `can_disable` ou `can_cancel_drain` du snapshot
validé, jamais une inférence depuis l'état. Les huit états, identité stable,
modèle, lien, swap, scratch, mount, usage, drain et raison sont présentés ; une
valeur inconnue reste `UNKNOWN`, et `SAFE_TO_UNPLUG` est explicite.

Après chaque snapshot ou contrôle validé, l'adaptateur enregistre l'état
physique auprès du Governor. Un snapshot malformé enregistre `ERROR` et bloque
les nouvelles allocations. Les wrappers Governor sont l'unique voie de lease
scratch de production ; le contrôleur conserve seulement son API physique
basse. Les quatorze Task kinds actuels n'ont aucun consommateur scratch, donc zéro
lease est la vérité courante et une capacité disponible n'est pas un usage.
La génération source peut légalement saturer à `UINT64_MAX`. Une update publique
matériellement différente au même watermark reste stale et ne peut pas rendre
de l'autorité ; seule la fin sérialisée du wrapper exact déjà engagé peut
réconcilier à ce watermark sa propre acquisition/libération et le compte de
leases fondé sur les adresses. Même son erreur ne peut recopier un compte stale.
Le shutdown suit Queue/leases → fermeture projet → join+unregister SSD →
contrôleur → Governor. Tous les tests SSD/TUI utilisent un provider factice et
n'exécutent aucune mutation réelle de l'hôte.

## GPU, build et ABI

Le backend Vulkan ORB Matcher est une voie production validée et GPU-first en
mode AUTO quand l'admission GPU/UMA est sûre. CPU reste le fallback portable.
Le batch AUTO utile est 8 ; batch 12 et inflight 2 restent des bornes de sûreté
et de benchmark rejetées pour la politique normale faute de gain supérieur à
5 %. Candidate, Feature et Visual Index restent CPU ; SIFT/RootSIFT Vulkan est
rejeté par l'évidence disponible ; GV reste CPU et conserve son USAC interne
non parallèle. Sur la Radeon 780M réelle RADV PHOENIX, la feasibility explicite
compare 24 160 requêtes SIFT et 24 161 RootSIFT : chaque mode produit une
divergence d'index, respectivement 20 251 et 20 824 divergences de bits de
distance, mais zéro divergence de décision Lowe. C'est une preuve de
feasibility décisionnelle, pas une équivalence binaire ; le hot path
SIFT/RootSIFT Vulkan reste donc rejeté. Les overrides CPU/Vulkan sont des outils
de debug, benchmark ou reproductibilité, pas un choix backend exigé de
l'utilisateur normal.

Le dépôt est mixte C17/C++17. Les headers publics sous `include/lardon3d/`
restent compilables comme C17, n'exposent aucun type C++ et décrivent ownership,
bounds et erreurs. Aucune exception C++ ne traverse une frontière C. Meson et
Ninja sont le build de référence ; Clang est préféré et GCC reste couvert pour
les contrôles C17 pertinents. GIO/GDBus est la dépendance directe du contrôleur
SSD ; aucune commande shell `mount`, `swapon` ou équivalente n'est construite.

## Évidence finale acquise et revue indépendante

### Builds normaux, Vulkan et ABI

Deux répertoires entièrement frais utilisent Clang/Clang++ 22.1.8, C17 et
C++17 :

- portable `-Dvulkan_orb=disabled` : graphe complet **931/931** puis suite
  sérielle **64/64** ;
- Vulkan `-Dvulkan_orb=enabled` : graphe complet **939/939** puis suite
  sérielle **65/65**, dont `orb-vulkan-backend` sur l'iGPU réel
  `AMD Radeon 780M Graphics (RADV PHOENIX)` ; la dépendance Vulkan détectée
  vaut 1.4.357 ;
- la feasibility SIFT/RootSIFT, cible non enregistrée dans la suite normale,
  a été compilée et exécutée explicitement ; ses divergences exactes sont
  consignées ci-dessus et n'autorisent aucun backend production nouveau ;
- les probes autonomes stricts GCC/Clang C17 et C++17 passent **76/76** sur les
  **19 headers publics modifiés ou nouveaux**, le fixture ABI historique, le
  lien de l'application et `git diff --check` passent, et
  `git status --short -- scan3d` reste vide.

La correction de narrowing Sparse SfM, postérieure à la capture des deux logs
complets, a ensuite passé son test focalisé, 20 répétitions, ASan/UBSan ciblé,
les inclusions C17/C++17 et le lien application. Elle retire les conversions
publiques `uint32_t → int` sans changer les appels valides ni la science.

### Déterminisme post-freeze du fixture Feature

La validation post-freeze a exposé une dépendance exclusivement test à
l'activité de l'hôte : `test-feature-task` construisait des profils et
Governors synthétiques, mais leur admission capturait encore le vrai
`/proc/loadavg`, les PSI et les deltas de swap. Une charge minute suffisante
pouvait donc maintenir correctement la Queue en `WAIT` jusqu'à son délai de
30 s, sans divergence scientifique ni défaut de la politique production.

Le fixture possède désormais un override privé par Governor qui copie sous
mutex un `ResourceSnapshot` complet et ne rafraîchit que son horodatage. Il est
compilé exclusivement dans `test-feature-task` ; la capture réelle et les
symboles production restent inchangés. Une régression directe prouve que la
charge synthétique 5 produit toujours `WAIT`, puis que la charge 0 produit
`START` : le seam contrôle la télémétrie, jamais la politique. La revue bornée
a ensuite identifié le second Governor créé par le helper runtime ; celui-ci
reçoit maintenant le même snapshot avant toute création de Queue, avec cleanup
explicite des échecs.

Après cette correction et sa revue, le test Feature complet passe **100/100**,
la matrice ordonnée Candidate→Matcher→Matcher pipeline→Feature **4/4**, la suite
portable finale **64/64** et la suite Vulkan finale **65/65**. Les exécutions
ciblées ASan/UBSan avec `detect_leaks=0` et TSan passent également. La
qualification LSan externe décrite ci-dessous reste inchangée. Une suite
normale mixte lancée après une reconstruction large a aussi observé un unique
timeout `task` ; le ciblé immédiat et la matrice de revue **100/100** passent.
Ce cas reste qualifié non reproductible/environnemental : ni le timeout ni le
comportement Task n'ont été modifiés.

### ASan, UBSan et LeakSanitizer

Le build frais Clang 22.1.8 `address,undefined`, portable/Vulkan désactivé, a
d'abord été exécuté avec détection de fuites : **57 OK, 6 FAIL, 1 TIMEOUT**.
Cette première passe n'est pas masquée :

- `feature-store`, `visual-index`, `candidate-pair-task`,
  `precision-features` et `precision-consolidation` terminent chacun sur la
  même fuite externe, exactement **3 808 octets en 68 allocations**, avec la
  dernière frame `/opt/cuda/lib64/libOpenCL.so+0x2fd4`, Build ID
  `b3217362255db6f1188e7596454ffe8bc4606b53`, résolue vers
  `/opt/cuda/targets/x86_64-linux/lib/libOpenCL.so.1.0.0`, sans frame projet ;
- `feature-task` atteint une fois son attente adaptative interne de 30 s et
  `task` le timeout Meson de 30 s, sans diagnostic ASan/UBSan/LSan.

Après cette attribution, la matrice complète repasse avec
`detect_leaks=0` mais ASan et UBSan toujours actifs : **64/64**, zéro timeout.
Une matrice séparée de vingt cibles dont `ldd` prouve qu'elles ne chargent ni
OpenCV ni OpenCL passe **20/20** avec LeakSanitizer actif. Les anomalies de
temps initiales ne s'étaient pas reproduites dans le suivi initial : 20/20
exécutions `feature-task` à affinité complète et 100/100 exécutions `task` avec
perturbations allocateur. Le cas Feature a été reproduit plus tard par la suite
portable ordonnée, puis déterminisé comme décrit ci-dessus ; le cas Task reste
non reproductible. La conclusion sanitizer exacte demeure : ASan/UBSan projet
**64/64**, chemins projet loader-free LSan **20/20**, limitation LSan du loader
OpenCL externe conservée explicitement — jamais un faux « LSan global 64/64 ».

### Concurrence

Le build frais GCC/G++ 16.2.1 TSan, volontairement Vulkan-disabled, passe les
quatorze cibles concurrentes Task/Project/Queue/Governor/SSD/TUI/Candidate/
Visual/Feature/Matcher/GV **14/14**, puis **220/220** répétitions déterministes,
soit **234/234** exécutions. `tests/tsan-opencv.supp` ne supprime que les races
issues des objets externes non instrumentés `libopencv_features.so`,
`libopencv_core.so` et `libtbb.so`; aucune frame Lardon3D n'est supprimée.
Cette preuve ne couvre pas Vulkan sous TSan : le backend réel est couvert par
le build/suite Vulkan ci-dessus et ses tests propres.

### Audit des avertissements

Le build Vulkan complet et ses cibles non-default ont produit 785 émissions :
117 dans des sources projet/tests/benchmarks et 668 dans les headers OpenCV
installés. L'audit n'a trouvé qu'une famille matérielle pour la frontière
publique courante : le narrowing Sparse `max_iterations`/
`minimum_inliers → int`, corrigé et revalidé comme décrit plus haut. Les
avertissements restants sont classés :

| Classe | Emplacements | Qualification |
| --- | --- | --- |
| Baseline production, non matériel pour cette maintenance | `src/acquisition_campaign_task.cpp:68`, `src/raw_development.cpp:185`, `src/acquisition_pairing.cpp:549`, `src/feature_extractor_opencv.cpp:231` | conversions sur valeurs déjà bornées par leur structure/validation ; aucune identité ni sortie modifiée dans ce ticket |
| Baseline géométrique bornée | `src/sparse_sfm_geometry.cpp:312-313` | index OpenCV signé sur les deux vecteurs construits avec le même nombre de colonnes ; distinct du narrowing public corrigé |
| Tests/benchmarks seulement | `tests/test_dense_mvs.cpp:45`, `tests/test_precision_consolidation.c:846-847`, shadows dans `tests/test_sparse_sfm_incremental.cpp:441-723`, API d'initialisation OpenCV dépréciée et indices signés dans `tests/benchmark_geometric_verifier.cpp:101-190` | aucun chemin production ni format durable |
| GCC TSan, non matériel | `src/feature_task.c:209-215`, `src/sift_task.c:170-175` | `-Wmaybe-uninitialized` interprocédural ; le callback fournit une Task non nulle et le helper initialise le contrôle avant toute autre sortie d'échec |
| Externe | headers OpenCV 5 installés ; découverte CMake Ceres | conversions/extensions et warnings de generator expressions hors sources Lardon3D |

Les catégories baseline ne sont pas promues en dette scientifique cachée :
elles sont consignées pour une maintenance ultérieure, tandis que le seul
narrowing pouvant accepter une valeur publique non représentable a été traité
avant la revue finale.

### Revue finale et clôture de la gate

Les tests focalisés TUI/runtime/SSD, les répétitions déterministes et la
validation F10 par providers factices restent acquis. La preuve GV finale
comprend 8/8 tests d'intégration focalisés, 60/60 répétitions de stress, 3/3
ASan/UBSan et 3/3 TSan ; les détails Project DB v23 sont consignés plus haut.

L'unique revue finale indépendante GPT-5.6 SOL/ULTRA a conclu **PASS**, avec
zéro finding bloquant. Elle a indépendamment exécuté le build portable, la
suite complète **64/64**, la matrice focalisée **15/15**, les probes strictes
GCC/Clang C17+C++17 **76/76** sur 19 headers publics modifiés/nouveaux, le
fixture ABI, les négatifs de symboles/strings des seams production, la
vérification du SHA-256 du manifest GV retenu et `git diff --check`.
Cette preuve indépendante ferme la gate sans effacer les qualifications
sanitizer consignées ci-dessus. L'ordre autorisé est désormais :

```text
science réelle acquise jusqu'à GV
→ GLOBAL_MAINTENANCE_AUDIT (PASS/FROZEN)
→ prochaine tranche séparée : poursuite réelle depuis Tracks
→ Sparse SfM réel / Dense selon calibration et roadmap
```

`scan3d/` et les contrats scientifiques Tracks/SfM/Dense restent protégés et
n'ont pas été modifiés par cette réconciliation.
