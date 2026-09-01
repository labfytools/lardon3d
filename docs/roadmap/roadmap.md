# Roadmap canonique Lardon3D

Cette feuille de route décrit l'ordre de dépendance actuel. Les contrats détaillés
restent dans `docs/architecture/`; ce document indique ce qui est fermé, ce qui
vient immédiatement ensuite et ce qui demeure volontairement différé.

## Situation actuelle

Lardon3D possède une chaîne scientifique persistante allant des acquisitions
explicites aux frontières Sparse SfM et MVS validées. Les travaux volumineux
doivent rester incrémentaux, bornés, reprenables, durables et admis par l'unique
Resource Governor : aucune étape ne peut supposer qu'une campagne entière tient
en RAM ou termine dans une seule vie de processus.

### Fondations PASS / FROZEN

- Gates A–G Sparse SfM, dont F0, orchestration Task/Project DB et politique
  Governor : [Sparse SfM](../architecture/sparse_sfm.md) et
  [frontière ressource](../architecture/resource_boundary.md).
- Track Model / Track Builder v1 : [Track Model](../architecture/tracks.md) et
  [Track Builder](../architecture/track_builder.md).
- Phase H v1 d'enrichissement incrémental multi-snapshot :
  [Project DB](../architecture/project_database.md).
- MVS-M1, frontière OpenMVS v2.4.0, identité dense et export COLMAP/PLY borné :
  [pipeline de reconstruction](../architecture/reconstruction_pipeline.md).
- Task Runtime, checkpoints atomiques, Queue et Resource Governor :
  [Task](../architecture/task_system.md), [Queue](../architecture/task_queue.md)
  et [Governor](../architecture/resource_governor.md).
- Project DB v22 gelé, overlay optique additif v23 courant, et S1–S3 Capture / Acquisition Ingestion : provenance
  Capture/Asset, import capture-safe, publication dérivée, développement RAW,
  siblings multi-source, évidence S3-D, orchestration S3-E et campagne bornée :
  [Project DB et ingestion](../architecture/project_database.md).

### S3 Capture / Acquisition Ingestion — PASS / FROZEN

```text
S3-A  Derived Image Publication             PASS / FROZEN
S3-B1 Deterministic RAW Development          PASS / FROZEN
S3-C  Multi-Source Capture Association       PASS / FROZEN
S3-D  Acquisition Pairing Evidence v1        PASS / FROZEN
S3-E  Multi-Source Ingestion v1              PASS / FROZEN
       Bounded Campaign Ingestion            PASS / FROZEN
Project DB                                   v19
```

```text
entrées d'acquisition explicites
→ publication SOURCE immuable
→ provenance Capture/Asset
→ métadonnées RAW/JPEG et évidence d'acquisition
→ propositions de campagne déterministes
→ regroupement fort ou explicitement confirmé
→ un Capture par observation physique résolue et siblings SOURCE
→ développement RAW déterministe si demandé
→ image_id scientifique immuable
→ pipeline aval existant
```

Les identités restent distinctes : `Capture != fichier`, `Capture != asset`,
`Capture != image_id`, `Capture != SHA-256` et `Capture != basename`.

### Validation réelle Sony A6000

Campagne `Photogrammetrie/2026-08-26_Baie_Moteur_A6000` :

- 953 ARW et 953 JPEG, soit 1906 sources ;
- métadonnées : ARW 953/953 OK, JPEG 953/953 OK ;
- JPEG Sony reconnus comme conteneurs MPF valides : APP2 `MPF\0`, images JPEG
  secondaires validées, remplissage inter-image/final nul, maximum privé huit ;
- `STRONG_GROUPS=0`, `CANDIDATE_PAIRS=953`, ambiguïtés 0, contradictions 0 ;
- plan identique après inversion de l'ordre des racines.

Le stem commun ne prouve jamais une acquisition. Les 953 propositions doivent
être confirmées et deviennent `CALLER_EXPLICIT`, jamais `STRONG`. La campagne
complète n'a pas encore été matérialisée.

### Limite de reprise DB v19 (historique)

S3-E reprend une matérialisation connue par `resume_capture_id`. Une campagne
peut reconstruire son plan et conserver côté appelant le `capture_id` de chaque
groupe. Si le processus meurt après création du Capture mais avant rétention
durable de cet ID, DB v19 ne peut le redécouvrir sans inventer une identité
interdite. Il n'existe donc pas de garantie whole-campaign exactly-once. Une
future identité durable de requête/campagne pourra fermer cette fenêtre ; elle
ne sera jamais déduite d'un chemin, SHA, basename, timestamp, métadonnée ou
`image_id`.

## Exécution durable de campagne d'acquisition — PASS / FROZEN

Project DB v20 apporte une migration additive v19→v20. Une tâche de campagne
enregistre atomiquement son snapshot générique, sa référence de checkpoint et
sa requête typée immuable. Le Task ID est l'identité opérationnelle de la
campagne ; la requête ne déduit aucune identité depuis les sources. Son codec
v1 est borné, à champs de largeur fixe et little-endian ; il conserve les
sources, les confirmations explicites et les options d'ingestion.

La requête persiste les confirmations comme `CALLER_EXPLICIT`, jamais comme
inférence `STRONG`. Le plan conserve des IDs de groupe un-based et Project DB
persiste la correspondance `(task_id, group_id) → capture_id`, ainsi que le
curseur. Chaque séquence admet et matérialise exactement un groupe via S3-E.
Après le retour de S3-E, une transaction conserve le Capture et avance le
curseur avant la progression générique et le checkpoint. Pause et annulation
sont coopératives aux frontières de groupe ; entre deux groupes,
`sequence_break` rend la réservation et impose une nouvelle admission.

À la réouverture, la registry existante reconstruit la tâche à partir de son
Task ID et de sa requête, puis la Queue et le Governor existants la réadmettent.
Aucun runtime, queue, governor ou mécanisme de persistance parallèle n'est
introduit. La fenêtre résiduelle acceptée demeure l'arrêt après le retour de
S3-E et avant la transaction de rétention : elle ne déclenche aucune inférence
de Capture et ne fournit pas de garantie exactly-once pour ce groupe.

## ENGINE BAY MULTI-CAMPAIGN INTEGRATION — PASS / FROZEN

L'intégration réelle a validé les deux campagnes dans un même projet temporaire
Project DB v20 : A6000 (953 ARW + 953 JPEG, 953 confirmations
`CALLER_EXPLICIT`, plan déterministe) et Samsung S21 FE SM-G990B (3544 JPEG,
groupes singleton déterministes). Un échantillon borné a exercé deux Captures
A6000 (JPEG SOURCE puis RAW dérivé) et trois Captures S21, avec ScanSets isolés,
Queue/Governor, persistance tâche/groupe→Capture et reprise sans duplication.
La capacité bornée des propositions de revue conserve un préfixe déterministe;
elle ne limite jamais l'évaluation complète ni le groupement scientifique.

### État de calibration des campagnes réelles

L'infrastructure Project DB v22, exécution sélectionnée, `raw.develop` et
Calibration Bootstrap v1 est **PASS / FROZEN** : la suite normale 53/53, les
contrôles syntaxiques C17, `git diff --check`, la validation ciblée ASan/UBSan
et l'audit final ont passé. La suite ASan/UBSan complète reste qualifiée par le
comportement LSan du pilote tiers RADV ; elle n'est pas déclarée PASS complet du
dépôt. Ce gel concerne l'infrastructure de persistance et d'import borné, pas
la calibration des campagnes réelles ni leur Sparse SfM.

Les campagnes réelles S21 et A6000 Engine Bay actuellement évaluées sont
`CALIBRATION_UNAVAILABLE` par non-identifiabilité scientifique des données
disponibles pour le contrat Sparse SfM à calibration connue. Ce statut ne
signifie ni échec logiciel, ni défaut des sources, ni rejet de qualité. Sans
pseudo-calibration, interpolation de métadonnées ou import inféré, le Sparse
SfM réel est `BLOCKED_BY_KNOWN_CALIBRATION_DATA`; l'intégration synthétique à
calibration connue a passé. Une acquisition physique dédiée de calibration est
une étape future, décrite dans [Calibration Bootstrap v1](../architecture/calibration_bootstrap.md),
et non une fonctionnalité déjà réalisée.

`CALIBRATION_SCIENCE_V1=PASS/FROZEN` fixe désormais le protocole physique,
les seuils, le regroupement optique, la preuve de coordonnées et le bundle de
provenance requis pour les **futures** campagnes connues-calibrées. Il ne
réhabilite aucune campagne historique : S21 Engine Bay reste définitivement
non rétro-calibrable. Voir [Calibration Science v1](../architecture/calibration_science_v1.md).

## PHOTO QUALITY TRIAGE / ACQUISITION SELECTION — PASS / FROZEN

L'étape qualité canonique implémentée se place après la découverte bornée, les
métadonnées et l'association minimale des sources, mais avant la matérialisation
normale d'une campagne et avant toute représentation scientifique, feature,
matching, SfM ou MVS :

```text
série source explicite
→ découverte bornée / métadonnées
→ candidats d'acquisition et confirmations existantes
→ Photo Quality Triage
→ recommandation GOOD / SUSPECT / REJECT par acquisition physique
→ sélection ou override humain explicite
→ création/exécution durable de campagne pour les groupes retenus
→ représentations scientifiques
→ features → candidats → matching → vérification → tracks → Sparse SfM
```

Le triage consomme les groupes d'acquisition existants et ne redéfinit jamais
Capture, Asset, `image_id`, SHA-256, chemin ou basename. Une paire A6000
RAW+JPEG confirmée est donc une seule unité de triage ; le JPEG caméra valide
est le proxy rapide préféré et l'analyse ne développe pas les RAW à grande
échelle. Un JPEG S21 singleton est la représentation d'analyse naturelle. Le
résultat est non destructif et explicable : `GOOD` est sélectionné par défaut,
`SUSPECT` attend une acceptation explicite et `REJECT` est exclu par défaut mais
peut être forcé par l'humain. Ces états sont une recommandation et une sélection
opérationnelle, jamais une identité ni une suppression de source.

L'implémentation reste déterministe, générique aux appareils, bornée en mémoire
et I/O, et exécutée par les Task, Queue et Resource Governor existants. Le décodage
JPEG applique une limite opérationnelle de 8192 pixels sur le plus grand axe, une
réduction à 1024 pixels et une réservation incluant le contexte retenu plus 20 MiB
de buffers d'analyse par groupe ; ces bornes ne sont pas des limites scientifiques
de campagne ou de dataset.
Elle doit conserver une voie honnête pour les RAW sans proxy plutôt que de
présumer que chaque RAW a un JPEG sibling. La revue interactive détaillée des
recommandations Photo Quality, la guidance de capture et les keyframes vidéo
restent des intégrations ultérieures qui réutiliseront ce même chemin de
qualité, sans second pipeline. Cette limite ne concerne pas l'observatoire
runtime TUI courant.

## SELECTED SCIENTIFIC EXECUTION ON ENGINE BAY INPUTS — PASS / FROZEN

L'intégration réelle opt-in porte uniquement sur les acquisitions sélectionnées
des campagnes Engine Bay et s'arrête à la frontière pré-SfM validable. Pour
chaque campagne, l'exécutable d'évidence a créé un projet temporaire Project DB v22
distinct : Visual Index v1 est borné à 4096 images, tandis que l'ensemble
A6000+S21 en compte 4497. Cette séparation est opérationnelle ; elle ne redéfinit
ni Capture, ni Asset, ni `image_id`, ni l'identité scientifique des acquisitions.

Le runner compose les API durables existantes, dans cet ordre :

```text
qualité
→ campagne d'acquisition
→ snapshot immuable de la sélection
→ représentation (A6000 `raw.develop`, S21 JPEG SOURCE)
→ features ORB
→ Visual Index
→ candidats du même ScanSet
→ matching ORB
→ GV gelée
→ arrêt durable avant Tracks
```

Il rouvre ensuite le Project DB afin de rapporter l'évidence durable produite.
Il n'introduit ni coordinateur réutilisable, ni DAG, ni sidecar, ni scheduler
distinct : Task, Queue et Resource Governor existants conservent leurs
responsabilités. L'overlay optique Project DB v23, ajouté ultérieurement, ne
réinterprète pas cette preuve v22 ; les copies des deux projets migrent avec les
comptes scientifiques inchangés et les nouvelles tables optiques vides.

Le Sparse SfM réel final reste `BLOCKED_BY_KNOWN_CALIBRATION_DATA` pour ces
campagnes ; aucune pseudo-calibration, interpolation de métadonnées ou inférence
d'identité ne le contourne. Dense, mesh et publication aval ne font pas partie de
ce jalon pré-SfM. Ce milestone est une intégration réelle, pas une nouvelle
série de micro-gates S3. Son statut acquis ne rend pas disponibles les données
de calibration physique manquantes et n'autorise pas à devancer la maintenance
globale.

## INTERNAL PARALLELISM + COMPUTE RESOURCES v1 — PASS / FROZEN

La fondation scientifique pré-SfM courante comprend le parallélisme interne
borné des étapes Candidate, Matcher et Visual Index, l'audit du threading des
features, et l'admission/libération des ressources par le Resource Governor
existant. Elle ne crée ni parallélisme inter-Tasks, ni pool global, ni second
Scheduler, ni second Governor, ni persistance parallèle. Le contrat gelé est
celui du [parallélisme interne borné](../architecture/internal_parallelism.md).

L'audit GPU vérifie les coutures réellement disponibles, les coûts UMA, les
réservations et la préservation des sorties canoniques. Il ne présume pas qu'un
GPU soit approprié ni qu'une sortie GPU partage automatiquement l'identité
scientifique CPU. Le gel ne revendique pas de comparaison de débit durable
CPU-versus-GPU sur corpus : la pression hôte a contaminé cette mesure.

Cette limite de preuve v1 n'annule pas l'évidence directe fournie à la tranche
suivante. Pour le hot path ORB Matcher, Vulkan est désormais validé,
déterministe et mesurément supérieur ; il devient donc le workload primaire de
la politique v2 GPU-first, sous admission GPU/UMA, avec fallback CPU. Candidate,
Feature et Visual Index restent explicitement rejetés pour le GPU, et
SIFT/RootSIFT Matcher restent BFMatcher L2 CPU.

**COMPUTE_GOVERNOR_V2 — PASS / FROZEN.**
**ORB_VULKAN_ASYNC_EXECUTION — PASS / FROZEN.** Cette tranche
autorisée fait évoluer l'unique Governor, sans créer de scheduler, Queue,
daemon ou persistance parallèle : télémétrie bornée, admission adaptative par
séquence et `AUTO` GPU-first pour les backends exacts et mesurément supérieurs.
La couture ORB Matcher normale est gelée avec CPU complet en fallback ; les
choix CPU/Vulkan explicites restent des overrides de debug, benchmark et
reproductibilité. Les dimensions retenues et toutes les validations v2 sont
closes.

Le pool CPU12 est validé comme preuve de cet hôte, pas comme plafond produit.
Le Governor dérive désormais le pool lourd depuis le masque
permis et les groupes package/core/SMT. Sur l'hôte unrestricted courant, il
obtient `0-5,8-13` et réserve `6,7,14,15`; un caller déjà précontraint ne subit
pas une seconde réserve. Le worker Queue seul applique/vérifie son propre
masque ; aucun TID auxiliaire énuméré n'est muté, car un pidfd ne stabilise pas
le numéro consommé par `sched_setaffinity(tid)`. Avant tout pthread applicatif
ou driver, le démarrage établit `MESA_SHADER_CACHE_DISABLE=true`; une valeur
absente prend ce défaut, `true`/`1` explicites sont conservées et toute autre
valeur explicite est préservée mais refusée. Cette politique non scientifique
supprime les helpers de cache Mesa observés qui élargissaient leur masque. Les
threads runtime restants héritent le compute-pool ; il n'existe plus de sweep,
latch ou retry auxiliaire et le diagnostic expose la politique réelle.
Creator/main/TUI reste unrestricted. Les CPU déjà exclus de l'affinité du
processus comptent dans la réserve hôte ; le fallback count-only ne fabrique
aucun masque. Le fallback au budget portable ne crée aucune exclusion
inventée. Cette couture est **PASS / FROZEN** sur le profil validé; ces IDs ne
sont pas une politique portable. La cible RAM conserve une réserve dure de
3 GiB de `MemAvailable`; la zone 3–4 GiB est une prudence qui ne soustrait pas
4 GiB à toute capacité. Les petits hôtes dégradent le budget en conservant au
moins une unité de calcul. Les PSI CPU/mémoire/I/O et les deltas
swap-in/swap-out sont des signaux
actifs ; l'occupation totale du swap reste historique. Admission CPU et lot
sont indépendantes. Sur la 780M, Hardware Profile classe conservativement comme
UMA le petit aperture VRAM amdgpu de 512 Mio accompagné d'environ 7,99 Go de
GTT système ; la capacité rapportée reste observable mais ne devient pas un
budget séparé. Les coûts GPU sont débités exactement une fois de la RAM hôte.

L'audit Phase 1 couvre les 14 kinds de production et sépare leurs dimensions
fixes des dimensions réellement adaptables. Il confirme que tous passent par
l'unique Governor, même les formes fixes, et que le contrat reste immutable
pendant une séquence. Cette tranche ferme l'enveloppe privée, la sélection AUTO,
les diagnostics bornés, l'enforcement OpenCV borné au compute-pool, la politique
d'affinité privée et la réconciliation du contexte retenu des campagnes
nouvelles ou restaurées. La création/reprise AUTO ne touche plus Vulkan sur le
main ; le premier begin appartient au worker contraint. Inflight ORB normal est
maintenant fixé à 1, helpers reste 0; depth 2 reste une capacité privée de
sûreté/benchmark. La clôture v2 est acquise. Ces choix ne créent aucune limite
de dataset, identité scientifique ou version Project DB.

La signature durable des nouvelles Tasks ORB normales est maintenant la classe
`MIXED`, sémantiquement réelle pour une politique susceptible d'exécuter CPU ou
Vulkan. Elle reconstruit AUTO ; toutes les signatures CPU anciennes/courantes
restent CPU fixes et Vulkan reste fixe, sans migration DB/codec. La couture
asynchrone est privée, request-bound et nettoie son slot sur toute sortie. Une
preuve événementielle bornée établit la soumission du successeur avant la
publication du prédécesseur pour deux paires 769×769. La rampe ne croît plus sur
la seule santé : l'adaptation générique exige deux observations par fenêtre,
tandis que le lot ORB Vulkan en exige huit, avec retour au palier accepté sans
gain et reset immédiat sous pression. Ces
éléments sont **PASS / FROZEN** dans les limites validées.

Les diagnostics de séquence distinguent maintenant backend sélectionné et
backend réel ; les participants CPU Matcher restent `cpu_threads` et
`helpers=0`. Une paire Vulkan inéligible ou en panne est recalculée entièrement
sur CPU sans preuve partielle. Les extractions ORB/SIFT/RootSIFT testées à
1/2/4/8/12 consomment leur contrat OpenCV immutable avec sorties égales.
Le rolling distingue désormais un handle soumis d'une inéligibilité locale et
n'appelle jamais `finish` sans requête. La panne backend invalide immédiatement
l'admission partagée sur toute sortie précoce ; seules les reprises AUTO peuvent
établir cette disponibilité, indépendamment de l'ordre des reprises fixes ou
historiques. Le statut est **PASS / FROZEN**.

La boucle privée mesure désormais l'utilisation `/proc/stat` du compute-pool,
`MemAvailable`, PSI mémoire/I/O `some/full`, deltas swap actifs, RSS/HWM observé
et GPU busy DRM, avec `unknown` sur absence ou parse non strict. Le backend
Vulkan fournit des compteurs cumulatifs bornés de submit/complétion/fence/
readback/GPU/starvation/panne/discard ; Matcher agrège en plus CPU et publication
par séquence. Le diagnostic est tirable par numéro de série, sans log ncurses ni
histoire persistée. Les CPU réductibles progressent par puissances de deux vers
la capacité exacte du kind/compute-pool après deux observations de baseline et
deux gains d'au moins 5 % ; CPU et lot ne changent
jamais dans le même essai. Sous pression, une admission adaptative encore
permise réserve immédiatement CPU1 et lot minimum. Feature/SIFT/RootSIFT ne
comptent un item qu'après extraction et publication durable propre ; READY,
`ALREADY_PRESENT` et publication incertaine comptent zéro, comme un segment
Visual Index non durable.
Candidate mesure chaque séquence. Un
fallback réel annule l'essai Vulkan au lieu d'empoisonner sa baseline. Cette
implémentation est **PASS / FROZEN** sans ajouter de helper GPU.

L'évidence retenue avant cette implémentation compare le contrôle synchrone à
49,989 paires/s et rolling depth 1 à 55,124 paires/s (+10,27 %), avec le même
digest `7a9dbc38a23a600379167d55e24836b7acbb22eea25573e7440bdc9e4602b3b3`.
La starvation depth 1 (53,847 s sur 74,613 s, GPU busy max 25 %) motive deux
slots bornés mais ne constitue pas une mesure depth 2. Le backend partage
device/pipeline/layout/cache et duplique seulement 640 Kio de payload,
command/fence/descriptors/query par slot. Le payload mappé suit désormais
exactement la capacité de séquence admise : zéro avant initialisation, 640 Kio
à depth 1 et 1,25 Mio uniquement pendant un contrôle privé depth 2, avec retour
à 640 Kio avant la prochaine admission depth 1. L'enveloppe normale n'essaie
plus inflight 2. Les générations de requête ne bouclent pas; un slot épuisé est
retiré définitivement.

Le harness réel a exécuté le corpus 4113 paires pour le contrôle synchrone,
rolling depth 1 et l'A/B forcé depth 1/depth 2. L'ABBA forcé donne
54,661652238 et 55,797311953 paires/s (+2,077617 %, sous le deadband 5 %).
Chaque run porte 4113 paires durables; le débit combiné est
`(2 * 4113 * 1e9) / somme(wall_ns)`, pas la moyenne des débits par run. Les
walls bruts 75326831673/75162582080 et 73662096698/73764360098 ns donnent les
moyennes 75,244706877/73,713228398 s. Fence vaut 6,0684/3,6776 s, starvation 54,4534/50,1465 s,
publication 29,2582/30,0548 s, submit CPU 0,2655/0,3818 s, readback
0,0460/0,0873 s et GPU busy max 23/24 %. Les quatre exécutions conservent le
digest `7a9dbc38a23a600379167d55e24836b7acbb22eea25573e7440bdc9e4602b3b3`, quatre
séquences de fallback local par exécution et zéro panne/discard. Depth 2 est donc
**REJECTED_WITH_MEASURED_REASON** pour la politique normale :
`DEPTH_MAX_VALIDATED_SAFETY=2`, `DEPTH_MAX_USEFUL=1`. Une comparaison
whole-corpus adaptative antérieure (+1,12 %) ne séparait pas les changements de
contrat; l'ABBA ci-dessus la remplace comme décision de profondeur. Le
répertoire retenu est
`/home/fy59/Documents/Lardon/.real-pre-sfm-2026-08-30/governor-v2-evidence/`,
avec `forced-depth1-a.stdout.jsonl`, `forced-depth1-b.stdout.jsonl`,
`forced-depth2-a.stdout.jsonl` et `forced-depth2-b.stdout.jsonl`. Le
mode Matcher par défaut est AUTO/rolling ; CPU et Vulkan explicites restent des
overrides. Un contrôle synchrone fence par fence est compilé seulement dans le
runner/test, absent de `lardon3d`, non persisté et non applicable à une reprise
Matcher pendante. Les fixtures et ces deux exécutions établissent l'égalité
exacte des sorties; les chiffres depth 1 sont rapportés ci-dessus. Le runner émet
des diagnostics JSON échantillonnés sans prétendre voir chaque séquence, plus
un agrégat Governor fixe exact pour les compteurs enregistrés. Il valide la
bijection Candidate Pair/Match Result, les assets et le curseur, puis produit le
digest canonique de comparaison `L3DMRD1`, qui exclut IDs Task, timestamps et
choix opérationnels. Ces preuves ferment **PASS / FROZEN** sans promouvoir
depth 2 dans AUTO normal.

Le runner possède maintenant, dans ce seul target, `--matcher-inflight 1|2` :
AUTO rolling fixe batch 2 et min=max inflight pour permettre la mesure du même
binaire à profondeur 1 puis 2, en n'exposant que la capacité Vulkan forcée.
`--matcher-batch 2|4|8|12`, valable seulement avec inflight et AUTO rolling,
fixe aussi min=max batch pour la prochaine matrice contrôlée.
Synchronous reste depth 1. Le Governor conserve son admission et sa charge UMA;
GPU budget zéro ou capacité GPU/backend/mémoire indisponible échoue au lieu de
sélectionner CPU. La valeur n'est ni persistée ni scientifique,
est refusée sur une Task pendante, restaurée dans l'environnement à toute sortie
et émise comme `1`, `2` ou `null` dans les résumés/agrégats. Le runner valide le
contrat exact et zéro panne/discard/pending; seule l'inéligibilité locale peut
produire un fallback CPU complet. La production compte désormais chaque item
exactement une fois après publication durable dans local-ineligible,
backend-failure ou other; une admission CPU normale reste à zéro. Le compteur
est incrémenté immédiatement et reste visible si une paire suivante échoue ou est
annulée, sans valider ni entraîner la séquence incomplète; son high-water mark
de déduplication reste privé et non persisté. L'égalité
inter-cohortes porte sur les items locaux, pas sur les séquences dont le nombre
dépend du batch. Une panne/raison autre, ou une panne tardive, invalide la
cohorte; la Task de benchmark échoue après checkpoint de toute preuve CPU déjà
durable. Les fixtures
établissent l'égalité exacte rolling1/rolling2/synchrone1 et des batches
2/4/8/12 sur fixture, avec le même compte de 29 items locaux malgré des comptes
de séquences différents. Les logs corpus préliminaires
`forced-batch2-current.stdout.jsonl` et `forced-batch4.stdout.jsonl` couvrent
les mêmes 4113 IDs/digest mais portent quatre contre trois séquences locales :
ils sont conservés comme preuve que l'ancien comparateur était invalide et ne
participent pas à la décision. Les huit runs item-valides
`forced-batch{2,4,8,12}-items{,-b}.stdout.jsonl` publient chacun 4113 paires,
six items locaux, zéro panne/autre et le même digest. Leurs débits combinés
sont 54,180767704, 66,094373197, 74,784998723 et 76,755814095 paires/s. Batch
4 puis 8 gagnent +21,988624373 % et +13,148812987 %; batch 12 ne gagne que
+2,635308425 %, sous le deadband 5 %. AUTO normal suit donc
`BATCH_MAX_USEFUL=8`; batch 12 reste une capacité privée sûre
`REJECTED_WITH_MEASURED_REASON`.
Le contrôle de production sans override
`short-auto-batch8-governor-v2.stdout.jsonl` atteint réellement
`1 → 2 → 4 → 8`, puis publie 4113/4113 résultats à 76,072 paires/s avec le
même digest, six fallbacks locaux, zéro panne/discard, inflight 1 et helpers 0.
La preuve de fermeture S21
`final-s21-auto.stdout.jsonl` exécute ensuite le chemin production normal sur
172 741 Candidate Pairs : 172 741 Match Results, zéro doublon, curseur complet,
digest `e5128a2e599ff593c4f79850e067254b1f249d19e8480a44973306b1af250f70`
et 73,649 résultats durables/s. AUTO choisit Vulkan sur toutes les admissions,
termine batch 8/inflight 1/helpers 0 et ne compte aucune panne/discard/pending.
Une admission YELLOW réduit batch 8 à 1, puis les séquences GREEN rétablissent
1 → 2 → 4 → 8; le gate possède donc aussi une preuve réelle de recovery.

La poursuite S21 ferme ensuite le point Geometric Verifier v3 sans rejouer le
Matcher. La Task 2832 consomme les 172 741 Match Results et publie 172 275 GVR
v3, dont 24 065 acceptés et 148 210 rejetés, avec zéro doublon et curseur
complet. Le digest Matcher reste
`e5128a2e599ff593c4f79850e067254b1f249d19e8480a44973306b1af250f70`.
La seconde reprise crée zéro ligne ; une Task interrompue sur une copie dédiée
reprend le même ID et converge vers les mêmes lignes exactes. Feature,
Candidate et Matcher ne sont pas rejoués. Track Builder et Sparse SfM ne sont
pas exécutés. `REAL_S21_GV_V3=PASS/FROZEN` ; la prochaine tranche scientifique
S21 commencera donc à Tracks maintenant que la gate de maintenance globale
ci-dessous est fermée. Tracks n'est pas acquis.

Les gates de fermeture sont acquis :

```text
GOVERNOR_CONTROLS_ALL_TASKS=PASS
HOST_CPU_RESERVE=PASS
HOST_RAM_RESERVE=PASS
REAL_TIME_TELEMETRY=PASS
SLOW_START=PASS
HYSTERESIS=PASS
PRESSURE_THROTTLE=PASS
PRESSURE_RECOVERY=PASS
GPU_FIRST_AUTO=PASS
ORB_VULKAN_TRUE_ASYNC=PASS
TRUE_GPU_CPU_OVERLAP=PASS
VULKAN_RESOURCE_BOUNDS=PASS
UMA_ACCOUNTING=PASS
SCIENTIFIC_EQUIVALENCE=PASS
RESTART_IDEMPOTENCE=PASS
FINAL_FULL_S21_AUTO_RUN=PASS
FULL_NORMAL_SUITE=PASS
C17=PASS
ASAN_UBSAN=PASS
GIT_DIFF_CHECK=PASS
FINAL_XHIGH_REVIEW=PASS
DOC_CONSISTENCY=PASS
MATCHER_GPU=EXISTING_BACKEND_VALIDATED_AND_PREFERRED
COMPUTE_GOVERNOR_V2=PASS/FROZEN
ORB_VULKAN_ASYNC_EXECUTION=PASS/FROZEN
REAL_S21_GV_V3=PASS/FROZEN
FEATURE_REPLAY=0
CANDIDATE_REPLAY=0
MATCHER_REPLAY=0
TRACKS_EXECUTED=0
SPARSE_SFM_EXECUTED=0
```

L'expérience normale est donc : l'utilisateur lance une Task; l'unique
Governor choisit et explique le contrat borné de sa prochaine séquence. Aucun
réglage CPU/GPU/lot/inflight/helper n'est requis en production ordinaire.

L'ordre de travail autorisé à court terme est :

```text
fondation scientifique pré-SfM courante
→ INTERNAL PARALLELISM + COMPUTE RESOURCES v1
  (Candidate, Matcher, Visual Index, audit feature threading,
   correction progression/Resource Governor, audit GPU)
→ COMPUTE GOVERNOR v2 / ORB VULKAN ASYNC EXECUTION (PASS / FROZEN)
→ REAL S21 GV v3 (PASS / FROZEN; arrêt avant Tracks)
→ GLOBAL MAINTENANCE AUDIT
  (PASS / FROZEN; validation et revue finale indépendante acquises)
→ poursuite pré-SfM réelle de S21 à partir de Tracks
→ acquisition dédiée de calibration
→ Sparse SfM réel
→ Dense / MVS
```

## GLOBAL MAINTENANCE AUDIT — PASS / FROZEN

Le jalon scientifique acquis reste arrêté après GV. Avant Tracks, Sparse SfM
ou Dense/MVS, la gate de maintenance globale a réconcilié l'architecture, les
ressources, la persistance et l'observation TUI. Son état et ses preuves
consolidées sont consignés dans le
[registre canonique de maintenance](../architecture/global_maintenance_audit.md).

Le checkpoint canonique de revue est le tag
`global-maintenance-2026-09-01`, au commit
`b84f860d868c66d9ee84b85ceb1bc6480b95aca5`. Les revues futures sont
strictement delta-based depuis ce point : `git diff
global-maintenance-2026-09-01...HEAD`, puis examen des fichiers modifiés, des
contrats, tests et documents directement affectés, et des frontières de
dépendances traversées. Les systèmes PASS/FROZEN inchangés héritent de la preuve
du [registre de maintenance](../architecture/global_maintenance_audit.md) et ne
sont rouverts que sur preuve concrète ; ne pas répéter un audit global A-à-Z.

Les résultats déjà réglés sont :

- Project DB v23 ajoute neuf relations optiques sans backfill ni inférence ;
  les boîtiers, objectifs manuels/électroniques, configurations et calibrations
  restent distincts, et la sélection de calibration exige une compatibilité
  exacte et explicite ;
- le Governor réserve d'abord l'hôte, sans plafond CPU global 12 : réserve de
  quatre CPU logiques sur hôte capable, groupes cœur/SMT complets lorsque la
  topologie est fiable, au moins une unité de calcul sur petit hôte, réserve
  RAM dure 3 GiB et prudence 3–4 GiB, PSI/swap actif et UMA comptée une fois ;
- Candidate porte une capacité sûre 64, Visual Index 16, ORB/SIFT/RootSIFT sont
  bornés par le compute-pool, Matcher conserve sa borne intrinsèque sûre 12 et
  utile 8, et GV conserve son USAC interne sériel mais admet 16 participants
  sûrs/8 utiles et 16 parents par lot ;
- le contrôleur SSD UDisks2 optionnel est une frontière physique revue, avec
  identité Drive+labels+UUID, leases, drain sûr et latch de danger pour action
  indéterminée. Son état est enregistré auprès du Governor, seul orchestrateur
  des leases de production ; il n'est ni scheduler ni second Governor ;
- la TUI est un observatoire/centre de contrôle validé opérationnellement :
  ncurses main-thread, modèle pur, observation coalescée et bornée, progression
  durable/ETA, pipeline et ressources honnêtes, profils optiques, layouts
  100×30/72×20/60×15, repli texte/couleur et F10 SSD asynchrone toujours
  visible ;
- la frontière de session détruit/joint la Queue avant Project DB, puis recrée
  une seule Queue ; l'arrêt global libère les leases Task avant unregister SSD,
  contrôleur et Governor.

Les validations finales exécutables sont acquises : build Clang portable
931/931 + suite 64/64, build Clang Vulkan 939/939 + suite 65/65 sur Radeon
réelle, ASan/UBSan 64/64 avec la limitation LSan OpenCL externe explicitement
qualifiée, LSan loader-free 20/20, TSan 14/14 + 220 répétitions, headers publics
76/76 sur 19 headers modifiés/nouveaux et contrôles ABI/diff/`scan3d`.
L'unique revue finale indépendante GPT-5.6 SOL/ULTRA a conclu PASS sans finding
bloquant après avoir indépendamment rejoué le build portable, la suite 64/64,
15/15 tests focalisés, les 76/76 probes de headers, l'ABI, les négatifs de seams
production, le SHA du manifest GV retenu et le diff-check. La gate de
maintenance est donc fermée ; la poursuite réelle depuis Tracks devient la
prochaine tranche séparée, sans avoir été exécutée par cette synchronisation.

## REAL S21 TRACKS — PASS / FROZEN

`REAL_S21_TRACKS=PASS/FROZEN`. La preuve part du projet GV v3 immuable
`/home/fy59/Documents/Lardon/.real-pre-sfm-2026-08-31/s21-gv-v3`, dont le
SHA-256 de Project DB est, avant et après les exécutions,
`56aa5ec37624b322e9f77a90b138cc7390ef817a9cec3bef7e4c87609fd2eeed`.
Les reflinks de preuve conservent exactement 2 826 Feature Sets, 172 741
Candidate Pairs/Match Results, 172 275 parents GVR (24 065
`GEOMETRIC_VERIFIED`, 148 210 `GEOMETRIC_REJECTED`). Aucun Feature, Candidate
Pair, Matcher ou GVR n'a été rejoué ni créé ; le source reste intègre. Sparse
SfM et Dense restent à zéro.

Le rejet initial de la Task `track_builder.run` 2835 à 0 % est conservé comme
constat historique : son enveloppe alors utilisée valait 19 546 898 688 octets
(18,204 Gio), au-delà des 12 750 811 136 octets (11,875 Gio) admis après
réserve. Ce n'est pas le modèle opérationnel validé. Le modèle compact actuel,
qui réserve les capacités vivantes du scope et le pic du Match File parent,
est admis par le Governor ; aucun scratch ni lease scratch n'a été utilisé.

La Task fraîche primaire 2837 termine `COMPLETE` à 100 % et publie le seul
Track Set 1 : 912 447 Tracks, 2 495 768 observations, longueurs min/max/moyenne
2/42/2,7352470883240341, zéro doublon et zéro Track à images conflictuelles.
Son digest canonique persistant est
`c30eba192627bf73eaf21ff30d81038d8cc6bbf36a69226f88cdc8c37f7d74a1`.
La reprise exacte suivante réutilise Track Set 1 (`task_track_id=0`) sans
modifier ces Tracks. Le run complet propre de la Task 2837 dure 3 316 s
(1788262322 → 1788265638).

La preuve de récupération est distincte : la Task 2835 est créée pendante sur
un reflink, interrompue par `SIGTERM` sans publication, puis reprise par le
runner corrigé. Elle termine `COMPLETE` à 100 % et retrouve le même Track Set
1, les mêmes comptes et le même digest ; le scénario interrompu avait publié
zéro Track Set. Cette preuve valide la reprise opérationnelle sans rouvrir le
contrat scientifique Tracks gelé.

## NEAR TERM

### Scratch SSD externe et swap optionnel

Le contrôleur physique actuel découvre par UDisks2 une paire exacte de labels
`LARDON_SWAP`/`LARDON_SCRATCH`, exige des UUID stables et la même identité
Drive, et ignore les renommages de nœud `/dev`. Modèle, série et vitesse USB
restent de la télémétrie optionnelle, jamais une identité produit. Les états
bornés sont `ABSENT`, `DETECTED`, `ENABLING`, `ENABLED`, `IN_USE`, `DRAINING`,
`SAFE_TO_UNPLUG` et `ERROR`.

Les usages futurs possibles sont workspace/scratch et intermédiaires
dense/mesh/texturing. Leur consommation devra être explicitement possédée par
les Tasks et passer par les wrappers de lease du Resource Governor ; le
contrôleur physique et le registre actuel n'inventent aucune éligibilité Task.
Les quatorze kinds courants ne consomment aucun scratch. Le swap de sécurité,
lui, reste une fonction du drain physique explicite et jamais un budget de
travail.

- les leases scratch ont une ownership explicite ; `DRAINING` refuse les
  nouveaux leases et attend leur libération exacte ;
- le swap n'est arrêté que si son usage est absorbable tout en conservant la
  réserve hôte de 3 GiB, sans PSI élevé ni swap-in/out actif ;
- SSD/swap ne sont jamais de la RAM ni une extension du budget scientifique ;
- latence et débit USB restent distincts de la mémoire ;
- l'utilisation reste optionnelle ; le Governor demeure l'unique orchestrateur
  de ressources et seul owner des leases scratch de production ;
- une action UDisks potentiellement appliquée mais non vérifiable verrouille le
  tuple physique original ; un remplacement n'obtient aucune autorité ;
- aucun formatage, partitionnement, fsck, réparation, arrêt forcé ou suppression
  n'est permis. La production n'appelle pas `statvfs`; espace total/libre reste
  `UNKNOWN` quand UDisks ne le fournit pas.

Le contrôleur, son registre Governor, le worker joinable unique et sa
présentation/actions F10 sont **CURRENT / VALIDATED OPERATIONAL**. Les tests
emploient un provider factice et ne montent, n'arrêtent ni ne modifient un vrai
disque. Ce statut ne crée aucun consommateur scratch ; c'est la validation
consolidée et la revue indépendante ci-dessus qui ont fermé la gate globale.

### Publication durable dense / mesh

MVS-M1 reste la frontière scientifique gelée. Les prochaines étapes portent sur
exécution, reprise, budgets mémoire/stockage et publication atomique :

```text
dense → mesh → refinement → texturing → consolidation → export
```

Le scratch devient particulièrement pertinent à cette frontière.

### Workflow TUI-first courant

La TUI expose actuellement état projet, Tasks et contrôles disponibles,
progression durable/ETA, Governor/ressources, profils optiques et SSD F10.
ncurses reste au thread principal ; les opérations SSD bornées utilisent un
seul thread joinable sans devenir un scheduler. La découverte/édition optique
ne devine aucune identité : objectif manuel sans EXIF, profils/configurations
immuables, affectations campagne/Capture et sélection de calibration exacte
restent explicites. Le viewer général, la capture guidance, la consommation
dense du scratch et les workflows scientifiques aval restent futurs.

### Sources mixtes et multi-ScanSet

Sony A6000, Samsung S21 et futures sources peuvent varier en device, objectif,
résolution et format. Elles réutilisent le même modèle Capture/Asset/Image, sans
fork par caméra. Plusieurs ScanSets et représentations sélectionnées alimentent
l'enrichissement Phase H existant vers un modèle consolidé ; Phase H n'est pas
redéfini ici.

## LATER

### Ingestion vidéo et keyframes — PLANNED

Une vidéo sera une source d'acquisition portant sa propre provenance, pas un
pipeline scientifique parallèle :

```text
asset vidéo SOURCE
→ timeline et métadonnées déterministes
→ extraction bornée et déterministe de keyframes
→ filtres blur/netteté/redondance
→ diversité de mouvement et de point de vue
→ représentations frame sélectionnées / candidats Capture
→ pipeline scientifique Lardon3D existant
```

Le futur contrat devra lier chaque frame à l'asset vidéo source, définir une
identité d'extraction reproductible et retenir le timestamp exact de chaque
frame. Les sujets de conception incluent espacement temporel, netteté, rejet
des frames redondantes, diversité de mouvement/point de vue, traitement borné,
reprise et admission par le Resource Governor. L'analyse de couverture pourra
ultérieurement contribuer à la sélection :

```text
besoin de couverture actuel + trajectoire caméra / frames vidéo
→ retenir les frames apportant une information géométrique utile
```

Cette relation reste un sujet de recherche et d'ingénierie ; aucune politique
de sélection n'est gelée. Il n'existera **aucun second pipeline SfM propre à la
vidéo** : les keyframes validées rejoindront les mêmes Capture, provenance,
images, features, matching, tracks, Sparse SfM et étapes aval que les photos.

### Capture Guidance / Live Coverage — PLANNED, LONG TERME

Le but à long terme n'est pas seulement de reconstruire ce qui a été
photographié, mais de guider activement l'opérateur vers les photographies qui
manquent encore pour obtenir une reconstruction fiable. Cette capacité est
postérieure à une reconstruction suffisamment mature ; elle ne fait pas partie
de S3 et ne remplace pas le prochain milestone d'intégration multi-campagne
A6000 + S21.

```text
photographies existantes
→ reconstruction Lardon3D
→ analyse de qualité de couverture
→ régions faibles ou manquantes
→ localisation de la caméra courante
→ projection dans la Live View
→ photographies supplémentaires par l'opérateur
→ ingestion / mise à jour de reconstruction
→ mise à jour de couverture ↺
```

#### Dépendances et niveaux de maturité

L'ordre conceptuel est :

```text
Sparse SfM
→ calibration et poses caméra
→ géométrie dense / mesh lorsque nécessaire
→ métriques de couverture
→ viewer et localisation live
→ Capture Guidance / Live Coverage
```

Une première analyse peut s'appuyer sur la géométrie sparse et les tracks ; un
mesh dense n'est donc pas une condition universelle. Les niveaux suivants sont
des paliers de roadmap, **pas de nouveaux Gates** :

1. **Offline Coverage Analysis** — calculer les régions faibles ou manquantes
   depuis une reconstruction existante.
2. **Coverage Viewer** — afficher géométrie, caméras, qualité de couverture et
   zones faibles.
3. **Suggested Supplementary Viewpoints** — associer une région faible à une
   direction ou un cône de points de vue suggéré.
4. **Live Camera Localization** — estimer la pose de la caméra courante par
   rapport à la reconstruction.
5. **Live Coverage Overlay** — reprojeter la couverture dans le flux vidéo.
6. **Closed Acquisition Loop** — guider, capturer, transférer, ingérer,
   reconstruire, réévaluer puis guider à nouveau.

#### Coverage Analysis — PLANNED

L'analyse estimera à quel point une région de surface ou de géométrie
reconstruite est soutenue par des observations photographiques. Ses entrées
potentielles comprennent :

- nombre de Captures observant la région, angle et diversité angulaire ;
- parallaxe disponible et diversité des points de vue ;
- résolution effective projetée sur la surface ;
- netteté, exposition et qualité d'image ;
- support features/tracks et qualité de reprojection/triangulation ;
- confiance de reconstruction, visibilité et occlusions ;
- provenance et historique des ScanSets.

Une expression telle que la suivante n'est qu'une intuition
**NON CONTRACTUELLE** :

```text
coverage_score = f(
    observation_count,
    angle_quality,
    parallax_quality,
    effective_resolution,
    sharpness,
    viewpoint_diversity,
    reconstruction_confidence
)
```

Les métriques exactes, poids, seuils et normalisations nécessitent une
validation expérimentale ultérieure. Aucun score scientifique n'est défini ou
gelé par cette roadmap.

#### Coverage Viewer — PLANNED

Le Coverage Viewer ne sera pas un simple viewer de mesh : il servira à examiner
la qualité d'acquisition et de reconstruction. Les overlays futurs pourront
montrer positions, directions et frustums des caméras, surfaces bien ou mal
couvertes, zones jamais vues, trous de reconstruction, faible nombre
d'observations, diversité angulaire ou parallaxe insuffisante, support
features/tracks faible, reconstruction peu fiable et contribution par ScanSet.

Une heatmap pourrait par exemple utiliser rouge pour une photographie
supplémentaire requise, orange pour un angle médiocre, violet pour une parallaxe
insuffisante, jaune pour un problème de qualité d'image et l'affichage normal
pour une couverture satisfaisante. Ces couleurs, catégories et significations
sont **des exemples seulement** : elles ne sont ni choisies ni gelées.

Lardon3D reste TUI-first. La TUI conserve le contrôle du projet, du workflow et
du runtime. Une frontière visuelle/acquisition séparée pourra posséder
l'affichage vidéo, le rendu points/mesh, les frustums, overlays et indications
live. Son architecture finale n'est pas définie ici ; elle devra préserver
l'isolation du viewer, consommateur de snapshots validés sans accès aux buffers
workers mutables.

#### Suggested Supplementary Viewpoints — PLANNED

Le résultat recherché dépasse la coloration d'une surface défaillante :

```text
région de surface faible + direction caméra / cône de points de vue suggéré
```

Il pourra exprimer « cette région demande des photographies supplémentaires »,
« photographier depuis une direction plus oblique », « le nombre d'images est
suffisant mais la diversité des points de vue ne l'est pas », « cette cavité
est vue par trop peu de Captures » ou « la géométrie d'acquisition fournit une
parallaxe insuffisante ». La recommandation devra dériver d'évidence géométrique
et de reconstruction, jamais du nombre de fichiers, de basenames similaires ou
d'heuristiques arbitraires déconnectées de la géométrie. L'algorithme
d'optimisation du point de vue n'est pas encore défini.

#### Live Camera Localization — PLANNED

Live Coverage dépendra de la localisation d'une vue nouvelle/live par rapport
à une reconstruction existante. Les prérequis probables incluent calibration
caméra, reconstruction sparse et points 3D existants, extraction de features
sur la frame live, correspondances 2D↔3D, estimation de pose calibrée, confiance
de pose et perte/réacquisition gracieuse du tracking. Cette capacité pourra
réutiliser la géométrie Sparse SfM, mais elle n'est ni conçue ni implémentée à
ce jour.

#### Live Coverage et boucle d'acquisition — PLANNED

La couche temps réel est distincte de l'analyse offline. Le déroulé cible est :

1. construire une reconstruction initiale et calculer l'évidence de couverture ;
2. recevoir sur le PC la Live View via une capture HDMI ;
3. estimer la pose de la caméra courante dans la reconstruction ;
4. projeter les régions 3D faibles/manquantes dans la frame vidéo ;
5. indiquer les acquisitions supplémentaires nécessaires pendant que
   l'opérateur déplace la caméra ;
6. déclencher une photographie et transférer RAW/JPEG par le chemin
   d'acquisition ;
7. faire entrer les nouveaux assets/Captures dans la provenance Lardon3D
   existante ;
8. mettre à jour reconstruction et couverture, puis retirer progressivement de
   l'overlay les régions corrigées.

L'expérience visée est conceptuellement : « les observations
photogrammétriques sont insuffisantes ici ; prendre une photographie
supplémentaire approximativement depuis cette direction ».

#### Frontière caméra HDMI / USB

Le Sony A6000 fournit un exemple concret de matériel cible, sans définir une
architecture scientifique propre à Sony :

```text
Sony A6000 ── HDMI → capture device → Live View ───────────┐
           └─ USB  → contrôle / shutter / RAW+JPEG ───────┤
                                                          ↓
                                                     Lardon3D sur PC
                    live camera pose + reconstruction/geometry/coverage
                                                          ↓
                                            reprojection et Live View overlay
```

Le calcul lourd, la reconstruction et le mesh restent sur le PC. La caméra est
principalement le capteur d'image, la source Live View et un dispositif
d'acquisition potentiellement contrôlable ; elle ne transporte ni n'exécute la
reconstruction. HDMI vise une Live View à faible latence. USB pourra selon les
capacités réelles de l'appareil fournir contrôle, déclenchement, métadonnées et
transfert RAW/JPEG. Tous les appareils ne partagent pas le même protocole : les
transports et contrôles spécifiques resteront à la frontière des adaptateurs
d'acquisition, hors de l'identité Capture gelée et du cœur scientifique.

Sony A6000, Samsung S21/mobile et futures caméras consommeront le modèle commun :

```text
Capture physique ↔ assets SOURCE ↔ représentations scientifiques sélectionnées
```

Capture Guidance consommera le modèle projet/reconstruction commun, sans fork
scientifique par device.

#### Boucle multi-ScanSet / Phase H

```text
ScanSet 1
→ reconstruction
→ analyse de couverture
→ zones faibles/manquantes
→ acquisition supplémentaire
→ ScanSet 2
→ ingestion / reconstruction
→ alignement et enrichissement Phase H
→ couverture mise à jour
→ répétition si nécessaire
```

Cette boucle est particulièrement importante quand un objet ne peut être
capturé en une seule passe. Elle réutilise Phase H v1 sans le redéfinir.

Pour une baie moteur, l'opérateur pourra à terme viser avec l'A6000 une bride,
une cavité ou une face mal observée mise en évidence dans la Live View, se
déplacer selon l'indication et ajouter une image avant ingestion et mise à jour
de la couverture. La valeur pratique est forte lorsque l'accès disparaîtra,
qu'un moteur ou composant doit être retiré, ou que le démontage modifiera la
scène : découvrir les photographies manquantes après coup pourrait être coûteux
ou impossible. Ce scénario motive la direction produit ; ce n'est pas un
contrat scientifique.

#### État futur et ordre de dépendance

Cette roadmap ne prétend implémenter aujourd'hui ni capture HDMI, contrôle USB,
pose live, projection de mesh, score ou heatmap de couverture, recommandation
automatique, ingestion vidéo, sélection de keyframes, ni reconstruction live
incrémentale. Ces capacités restent planifiées, ultérieures ou exploratoires.

L'ordre demeure sans ambiguïté :

```text
CURRENT NEXT
  fondation scientifique pré-SfM courante
  → INTERNAL PARALLELISM + COMPUTE RESOURCES v1
  → COMPUTE GOVERNOR v2 / ORB VULKAN ASYNC EXECUTION
    (PASS / FROZEN; AUTO GPU-first ORB et preuve S21 Matcher complète)
  → REAL S21 GV v3 (PASS / FROZEN; Tracks/Sparse SfM non exécutés)
  → GLOBAL MAINTENANCE AUDIT
    (PASS / FROZEN; gate fermée avant la tranche Tracks séparée)
  → poursuite pré-SfM réelle de S21 à partir de Tracks
  → acquisition physique dédiée de calibration
  → calibration connue validée → Sparse SfM réel multi-campagne
  → dense / MVS → publication
→ LATER
  Coverage Analysis → Coverage Viewer → suggestions de points de vue
  → localisation live → intégration HDMI/USB → Capture Guidance / Live Coverage
```

Vidéo/keyframes pourra progresser en parallèle lors d'une phase ultérieure,
mais réutilisera toujours la même provenance Capture et le même pipeline
scientifique.

- **Maintenance projet** : vérification de provenance, assets orphelins,
  scrub/réconciliation et réclamation sûre du scratch. Aucun asset immuable
  partagé n'est supprimé silencieusement.
- **Exports/publication live** : snapshots validés et consommation sans accès
  aux buffers workers mutables.

## DEFERRED / OPTIONAL

- pools multiples CPU/GPU/IO, parallélisme inter-tâches et multi-GPU ;
- DAG général de dépendances et priorités complexes ;
- infrastructure backend générique et distribution de calcul ;
- ALIKED tant que provenance modèle, export ONNX et oracle upstream ne sont pas
  reproductibles.

Ces idées ne doivent pas devancer l'intégration réelle multi-campagne ni créer
un second runtime, Governor ou système de persistance.

## Principes de séquencement

1. stabilité et intégrité scientifique avant débit ;
2. résultats atomiques et durables avant parallélisme ;
3. lots bornés et reprise avant taille de campagne ;
4. un Task Runtime/Queue et un Resource Governor, sans second scheduler ;
5. scratch optionnel sans élargissement implicite des budgets RAM ;
6. TUI de contrôle avant visualisation riche ;
7. documentation canonique alignée sur le code validé.
