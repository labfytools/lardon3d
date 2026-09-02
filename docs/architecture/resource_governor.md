# Resource Governor Lardon3D

## COMPUTE_GOVERNOR_V2 — PASS / FROZEN

**COMPUTE_GOVERNOR_V2 — PASS / FROZEN.**
**ORB_VULKAN_ASYNC_EXECUTION — PASS / FROZEN.** Gate G core reste
**PASS / FROZEN**. Cette évolution opérationnelle ne change ni le contrat
scientifique Matcher gelé, ni les octets Match File, ni l'ordre du curseur, ni
la durabilité. Sa cible est une boucle fermée à `sequence_break()` : le
Governor observe une télémétrie hôte/Task bornée, choisit un contrat pour la
séquence suivante, puis le maintient immutable pendant toute son exécution.
Cette couture privée est gelée ; seule une séquence ultérieure peut recevoir
un autre contrat. Les dimensions retenues, le corpus S21 complet, les suites
portable/Vulkan, les sanitizers et l'audit XHIGH ferment le statut v2.

**PORTABLE_HOST_POLICY_MAINTENANCE — IMPLEMENTED / VALIDATED / REVIEWED.** La
maintenance ultérieure retire les plafonds matériels globaux issus de l'hôte
de preuve, sans changer la science, les formats, les fingerprints ni la
durabilité v2. Son lifecycle global reste suivi par
[`GLOBAL_MAINTENANCE_AUDIT`](global_maintenance_audit.md).

Le Governor lit le masque d'affinité permis et la topologie package/core Linux.
La politique par défaut demande quatre CPU logiques de réserve lorsque c'est
praticable et conserve au moins un CPU de calcul sur un petit hôte. Avec une
topologie complète, elle réserve des groupes de coeurs physiques complets,
frères SMT inclus, en choisissant le sous-ensemble déterministe dont le
dépassement de la cible logique est minimal. Les CPU déjà exclus par un masque
externe comptent dans la réserve hôte, donc la même capacité n'est jamais
soustraite deux fois. Sans affinité/topologie exploitable, le budget de compte
portable subsiste sans fabriquer de masque ni d'ID CPU.

Sur le profil de preuve Ryzen 7 8845HS unrestricted 0–15, la cible logique de
quatre donne le pool lourd `0-5,8-13` et la réserve `6,7,14,15` pour Arch/Sway,
l'audio et l'interaction ordinaire. Ces IDs sont une observation historique de
cet hôte, pas une politique portable. Aucun ID CPU n'est persisté ni ne devient
une identité scientifique.

Seul le worker lourd de l'unique Queue applique et relit son propre masque
(`pid=0`) avant les callbacks. Le caller/main/TUI reste unrestricted et aucun
autre processus n'est touché. Un pidfd de thread ne stabilise pas l'identifiant
numérique consommé par `sched_setaffinity(tid)` après la sortie du thread : le
Governor n'énumère ni ne mute donc jamais un TID auxiliaire. À la place,
Lardon3D établit `MESA_SHADER_CACHE_DISABLE=true` avant toute création de pthread
applicatif et avant toute initialisation Vulkan. L'absence de variable prend ce
défaut sûr ; les valeurs explicites exactes `true` et `1` sont conservées. Une
valeur explicite fausse ou malformée n'est pas écrasée et fait échouer le
démarrage, car elle permettrait à Mesa de créer ses helpers de cache disque
observés capables d'élargir leur affinité. La variable est inoffensive pour les
drivers non-Mesa, opérationnelle seulement, non persistée et absente de toute
identité scientifique. Sur le profil 780M contrôlé, les helpers `*:disk$0`
disparaissent ; tous les threads runtime restants observés héritent et gardent
`0-5,8-13`. Les diagnostics privés exposent l'activité de cette politique, la
valeur sûre et sa raison, sans prétendre recontraindre des auxiliaires.
Cette garantie est aussi défensive dans le backend public : toute requête
non vide qui devrait initialiser Vulkan vérifie sans mutation la valeur exacte
`true`/`1` avant le premier appel Mesa. Une valeur absente ou différente rend ce
contexte backend indisponible et retourne `UNAVAILABLE` sans sortie partielle.
La lecture metadata demeure non initialisante. Ainsi un consumer direct qui ne
passe ni par l'application ni par le runner ne contourne pas la réserve CPU.
Le nombre de CPUs du pool borne directement l'admission.
CPU Matcher reste validé dans `1..12`, indépendamment du lot :
`cpu_threads=12, batch_size=1` demeure un contrat valide.

La politique par défaut conserve environ 3 GiB de `MemAvailable` comme réserve
dure d'admission sur un hôte capable. Entre 3 et 4 GiB, elle entre en prudence
YELLOW et bloque la croissance, mais ne soustrait jamais 4 GiB à la capacité et
ne transforme pas cette bande stable en RED. Un hôte de moins de 3 GiB dégrade
la réserve en fraction déterministe afin de garder une capacité bornée.
Les entrées de pression actives sont `MemAvailable`, les PSI CPU/mémoire/I/O et
les deltas swap-in/swap-out entre observations. L'occupation totale du swap est
un état historique, pas à elle seule une activité récente. Ces objectifs v2 ne
réécrivent pas rétroactivement les constantes Gate G core gelées ci-dessous.
Les observations saines autorisent une croissance lente. Pour une dimension
CPU réellement réductible, la rampe double depuis 1 et inclut toujours le
maximum exact de la capacité lorsqu'il n'est pas une puissance de deux. Elle
est bornée par l'enveloppe et le compute-pool, sans plafond global 12. Deux
séquences établissent d'abord
une référence de débit durable puis ouvrent un essai borné au palier supérieur.
Les dimensions CPU/génériques conservent deux observations d'essai et un gain
d'au moins 5 % avant une nouvelle croissance. Le lot ORB Vulkan normal exige
désormais huit observations pures consécutives pour sa référence puis huit pour
chaque essai; sa décision compare les moyennes bornées au même deadband de 5 %.
Pour Matcher, cette observation est la cadence durable complète de la séquence :
à partir de la deuxième séquence elle commence juste avant la rupture/réadmission
et se termine seulement après calcul, publication owner-only et checkpoint
générique durable. Le temps interne du calcul reste une métrique séparée, mais
ne peut pas masquer le coût de réadmission que le choix du lot doit amortir.
CPU et lot partagent un unique essai : deux dimensions ne changent jamais dans
la même mesure. L'infrastructure privée sait aussi
borner un essai inflight, mais l'enveloppe ORB AUTO normale le fixe maintenant
à 1 après la mesure A/B décrite ci-dessous. Un échantillon rapide isolé ne
décide rien ; sans gain, le plafond revient au dernier palier accepté et s'y
arrête. PSI ou delta swap actif abandonne immédiatement l'essai ; toute
admission encore permise prend lot minimum, inflight minimum et CPU1
avant réservation. Un `WAIT`/`REJECT` Gate G reste inchangé. Après
l'hystérésis `RED → YELLOW → GREEN`, une nouvelle référence
permet de reprendre des essais contrôlés. Les diagnostics distinguent les
essais/gains/refus CPU, inflight et lot, `backend-fallback-hold` et
`pressure-decrease`. En production normale, ORB est inflight 1 et helpers 0.

La télémétrie hôte privée lit, avec capacités fixes et parse strict, les deltas
`/proc/stat` limités au masque du compute-pool, `MemAvailable`, PSI mémoire et
I/O `some`/`full`, les deltas actifs `pswpin`/`pswpout`, RSS/HWM du processus et
le `gpu_busy_percent` de l'unique index DRM retenu par Hardware Profile, sans
scan ni fallback vers une autre carte. Utilisation CPU et GPU
sont exprimées en basis points avec un bit `known`; régression, overflow,
troncature, token malformé ou signal absent donnent `unknown`. La charge globale
n'est pas rebaptisée utilisation du pool et un Task qui utilise son pool admis
n'est pas réduit pour ce seul fait. RSS/HWM reste une observation du processus,
jamais une réservation mémoire Task. Aucun échec de cette capture optionnelle
ne fait échouer l'exécution scientifique.

Pour ORB Vulkan normal, seul le lot `1..8` reste essayable ; inflight est fixé à
1 et helpers à 0. Huit séquences pures saines construisent la référence, puis
huit séquences au palier d'essai sont nécessaires avant acceptation ou refus.
Fallback, travail durable nul et pression sont exclus et réinitialisent la
fenêtre pertinente. Un backend affamé ou un GPU peu occupé sous hôte sain peut
ouvrir cet essai ; `gpu_busy` inconnu ne l'interdit pas lorsque la starvation
backend est observable. Seul le gain moyen de débit durable accepte le palier.
Une occupation GPU plus haute sans gain le fait revenir au dernier contrat
accepté et arrête cette croissance. Un ou deux échantillons, hauts ou bas, ne
décident donc jamais le lot GPU.

La mesure forcée ABBA du même corpus et du même binaire donne 54,661652238
paires/s à depth 1 et 55,797311953 paires/s à depth 2, soit +2,077617 %, sous
le deadband matériel de 5 %. Ces débits combinés valent
`(2 * 4113 * 1e9) / (wall_ns_a + wall_ns_b)`, et non la moyenne des deux débits
par run. Les `wall_ns` bruts sont 75326831673/75162582080 à depth 1 et
73662096698/73764360098 à depth 2; les walls moyens sont donc
75,244706877/73,713228398 s. Les autres moyennes depth 1/depth 2 sont : fence
6,0684/3,6776 s, starvation 54,4534/50,1465 s,
publication 29,2582/30,0548 s, submit CPU 0,2655/0,3818 s, readback
0,0460/0,0873 s et GPU busy max 23/24 %. Les quatre exécutions ont le digest
`7a9dbc38a23a600379167d55e24836b7acbb22eea25573e7440bdc9e4602b3b3`, quatre
séquences de fallback local par exécution et zéro panne/discard. Conclusion opérationnelle :
`DEPTH_MAX_VALIDATED_SAFETY=2`, mais `DEPTH_MAX_USEFUL=1`; depth 2 est
**REJECTED_WITH_MEASURED_REASON** pour AUTO normal.
Les agrégats retenus sont
`/home/fy59/Documents/Lardon/.real-pre-sfm-2026-08-30/governor-v2-evidence/`
`forced-depth1-a.stdout.jsonl`, `forced-depth1-b.stdout.jsonl`,
`forced-depth2-a.stdout.jsonl` et `forced-depth2-b.stdout.jsonl`.

La Radeon 780M possède un slot GPU et utilise une mémoire UMA : buffers Vulkan,
staging, descripteurs/commandes et readback en vol sont débités exactement une
fois de la RAM hôte. Hardware Profile ne conclut plus « VRAM séparée » au seul
motif qu'amdgpu publie `mem_info_vram_total`. Il conserve la capacité de payload
rapportée, mais classe conservativement shared/UMA lorsqu'un petit aperture
VRAM volé/dédié est accompagné d'un GTT à l'échelle de la RAM système, ou
lorsque cette petite capacité reste incertaine. Sur l'hôte validé, les preuves
exactes sont 512 Mio de VRAM visible et 7 986 020 352 octets de GTT pour environ
16 Gio de RAM. Une classification UMA conservatrice d'un GPU à faible VRAM peut
refuser inutilement une admission ; classer à tort cet iGPU comme mémoire libre
séparée pourrait contourner la réserve dure de 3 Gio et la prudence 3–4 Gio,
ce qui est interdit. La capacité
rapportée ne forme donc aucun second budget VRAM indépendant sur UMA.

La politique canonique v2 est GPU-first lorsqu'un backend est validé,
déterministe et mesurément plus rapide, sous réserve que son contrat GPU/UMA
soit admis. ORB Matcher est aujourd'hui le workload primaire qui satisfait ces
conditions : la frontière top-2 et la sortie complète sont exactes, et
l'évidence contrôlée le classe supérieure au CPU pour ce hot path. CPU demeure
le fallback portable et de panne. Les choix CPU/Vulkan explicites restent des
overrides de debug, benchmark et reproductibilité. La production normale cible
`AUTO`, choisi par le Governor et observable par séquence. Le code courant
implémente ce comportement pour le Matcher ORB normal : GPU validé d'abord,
puis capacité CPU complète si le build, le backend, le GPU ou l'admission UMA
ne sont pas sûrs. La création/reprise AUTO n'initialise ni ne sonde Vulkan sur
le caller : le premier `begin` initialise le driver sur le worker Queue déjà
contraint. Un échec produit une paire CPU complète sans preuve partielle et
désactive l'admission GPU ultérieure lorsqu'il s'agit réellement du backend ;
une paire sous le seuil Vulkan reste une simple inéligibilité, pas une panne.
Le Matcher représente séparément `SUBMITTED`, inéligibilité locale et panne :
il n'appelle `finish` qu'avec le handle soumis correspondant. Une panne réelle
ne reclasse que les requêtes effectivement soumises ou non encore tentées; la
cause locale déjà établie pour une paire voisine reste locale. Elle met la
disponibilité partagée à faux immédiatement, même si le fallback CPU,
l'annulation ou la publication échoue ensuite. Seule une reprise AUTO peut
réétablir l'éligibilité depuis les faits runtime ; les reprises fixes et
historiques ne l'écrasent pas.
Une faute locale avant soumission (lecture Feature, allocation, chemin) ou
après `finish` Vulkan réussi (filtrage, allocation, staging/fsync Match File)
appartient au bucket `other`, jamais à `backend-failure`. La paire est
recalculée entièrement sur CPU; le backend partagé et les successeurs déjà
soumis restent valides. Le booléen privé `backend_fault` n'est vrai que si la
transaction Vulkan `begin` ou `finish` elle-même échoue.
Les API CPU/Vulkan explicites restent fixes. La télémétrie bornée conserve un
dernier diagnostic sérialisé et un état de rampe par kind/backend ; elle
distingue backend sélectionné/réel, contrat complet, pression, mesures hôte,
débit durable et compteurs Matcher/Vulkan. Une couture privée permet un pull
`since(serial)` et un format texte borné ; rien n'est imprimé directement dans
ncurses, persisté ou accumulé en grande histoire. Les callbacks atomiques
Feature/SIFT/RootSIFT enregistrent un item seulement après extraction et
publication durable propre. READY/`ALREADY_PRESENT` réutilisé ou publication non
durable enregistre zéro et n'avance aucun essai. Visual Index applique la même règle par segment ;
Candidate enregistre chaque séquence durable. Un fallback CPU complet d'une
séquence sélectionnée Vulkan annule l'essai et reconstruit ultérieurement une
référence pure ; il n'entraîne jamais la baseline Vulkan.

Le runner opt-in `pre-sfm-real-execution` consomme maintenant cette couture
d'évidence. Ses échantillons JSON sont explicitement qualifiés
`latest-change-coalescing` : le polling peut fusionner des séquences rapides et
ne prétend donc pas les énumérer toutes. En parallèle, le Governor maintient par
kind/backend des compteurs cumulatifs saturants et des extrema de télémétrie de
taille fixe. Le résumé final rapporte ainsi les admissions et séquences
enregistrées, items durables, changements de contrat et sommes
CPU/Vulkan/publication sans histoire non bornée ni double comptage. Ces agrégats
vivent seulement avec l'instance Governor ; ils ne sont ni un budget RSS, ni un
nouveau payload, ni une persistance.
Ils conservent les classifications par séquence et ajoutent les comptes exacts
par item pour les fallbacks Matcher : inéligibilité locale, panne backend ou
raison autre/inconnue. La Task incrémente l'unique compteur de l'item seulement
après publication durable de son fallback CPU complet; une admission CPU
normale n'est pas un fallback. Cet incrément immédiat est séparé du feedback de
fin de séquence : un préfixe déjà durable reste compté si une paire suivante
échoue ou est annulée, sans créer une observation de débit pour la séquence
avortée. Un high-water mark Task borné évite le double comptage in-process; il
n'est ni persisté ni reconstruit au redémarrage. Ces compteurs fixes saturent
avec le drapeau
commun de l'agrégat et, contrairement au nombre de séquences, restent invariants
quand le lot change. Le contrôle matriciel forcé du runner n'expose qu'une
capacité Vulkan aux batch/depth demandés : zéro admission CPU, payload exact,
aucun changement de contrat, panne, discard ou slot pending sont nécessaires à
`experiment_valid=true`. Les items localement inéligibles ne rendent une
comparaison valide que si leurs comptes sont égaux entre cohortes; tout item de
panne ou autre cause échoue fermement.

La matrice forcée item-valide retenue est
`forced-batch{2,4,8,12}-items{,-b}.stdout.jsonl` sous le répertoire d'évidence
ci-dessus. Chaque run publie 4113 paires, six items localement inéligibles, zéro
item backend-failure/other et le digest
`7a9dbc38a23a600379167d55e24836b7acbb22eea25573e7440bdc9e4602b3b3`. Selon
`(2 * 4113 * 1e9) / (wall_ns_a + wall_ns_b)`, les walls bruts
76150272845/75674818393, 61319835797/63138565155,
55148130595/54847191200 et 53446248173/53724786321 ns donnent respectivement
54,180767704, 66,094373197, 74,784998723 et 76,755814095 paires/s. Les gains de
palier sont +21,988624373 %, +13,148812987 % et +2,635308425 %. Le dernier est
sous le deadband 5 % : `BATCH_MAX_VALIDATED_SAFETY=12` reste disponible au
benchmark privé, `BATCH_MAX_USEFUL=8` borne AUTO normal et batch 12 est
**REJECTED_WITH_MEASURED_REASON**. Les anciens
`forced-batch2-current.stdout.jsonl` et `forced-batch4.stdout.jsonl`, comparés
par nombre de séquences au lieu d'items, restent une preuve historique du
comparateur obsolète et ne participent pas à cette décision.
Le run de production sans override
`short-auto-batch8-governor-v2.stdout.jsonl` confirme ensuite la boucle réelle :
contrats `1 → 2 → 4 → 8`, dernier lot 8, inflight 1, helpers 0, 4113 résultats
durables en 54,066973393 s soit 76,072 paires/s, six fallbacks locaux, zéro
panne/discard et le même digest `L3DMRD1`. Le compute-pool contient 12 CPU,
les quatre CPU `6,7,14,15` restent réservés, l'UMA est comptée, le minimum
`MemAvailable` vaut 12 421 971 968 octets et aucun swap-in/out n'est observé.
La preuve S21 finale sans override,
`final-s21-auto.stdout.jsonl`, ferme la boucle sur 172 741 paires : AUTO
sélectionne Vulkan pour les 21 630 admissions, publie 172 741 résultats en
2 345,444485079 s (73,649 paires/s), termine avec batch 8, inflight 1 et
helpers 0, et ne compte aucune panne, aucun discard ni slot pending. La seule
admission classée YELLOW ramène batch 8 à 1; plusieurs séquences GREEN
reconstruisent ensuite la référence et remontent 1 → 2 → 4 → 8, sans
rebond. Le minimum `MemAvailable` est 10 927 390 720 octets, PSI mémoire
maximal 0, swap-in/out maximal 0, GPU busy moyen/médian/maximal 26/27/36 % et
HWM processus maximal 250 658 816 octets. L'UMA admise reste 655 360 octets;
les masques exacts sont compute `0-5,8-13` et reserve `6,7,14,15`. Cette preuve
est opérationnelle : le digest scientifique et la reprise sont documentés par
le contrat Matcher, pas redéfinis ici.
L'ensemble v2 est **PASS / FROZEN**. Le gel porte sur cette architecture et
ses bornes validées, sans rouvrir Gate G ni le Matcher scientifique.

Les décisions GPU négatives existantes restent inchangées : Candidate, Feature
et Visual Index restent CPU; SIFT/RootSIFT Matcher restent BFMatcher L2 CPU. Il
n'est introduit ni second scheduler, ni Queue, ni daemon, ni sous-système de
ressources.

### Audit des 16 kinds de production

Tous les kinds passent par l'unique Queue et l'unique Governor, y compris ceux
dont toutes les dimensions sont fixes. Dans le tableau, `CPU 1..N` décrit la
réduction possible par l'admission; `lot 1..N` décrit la dimension de lot
adaptable. La mémoire réservée vaut `fixe + par_item * batch_size`. Tous les
coûts GPU par item valent zéro; la forme ORB Vulkan normale réserve exactement
un slot inflight de 640 Kio, débité une fois de la RAM sur UMA. La capacité
privée de sûreté/benchmark peut retenir deux slots, soit 1,25 Mio, seulement
sous un contrat forcé depth 2. Device, pipeline, layouts et cache restent partagés; leurs
allocations driver opaques ne reçoivent pas un coût inventé.

| Kind v1 | Estimation courante | Dimension consommée et raison |
| --- | --- | --- |
| `raw.develop` | MIXED; CPU 1; lot 1; hôte `2 Gio + contexte`, 0/item; I/O 1; GPU 0 | Un Capture atomique. Le garde applique/restaure CPU1 au pool OpenCV global. Les 2 Gio sont une allowance de travail opérationnelle, pas une limite de dataset. |
| `raw.develop.batch` | MIXED; CPU 1..8; lot 1..8; hôte 0 fixe + 896 Mio/item (contexte propriétaire, workspace LibRaw 40 MP, copies RGB/BGR, PNG/validation, pile enfant 1 Mio et marge allocateur/codec); I/O 1; GPU 0 | Une fenêtre de Captures indépendants. `896 Mio × 8 = 7 Gio`, donc le budget post-réserve de l'hôte de référence peut admettre la fenêtre sûre complète. OpenCV reste à 1 thread interne, tous les enfants sont joints, puis le propriétaire publie le préfixe ordonné avant `sequence_break`. Le plafond est opérationnel, jamais une limite scientifique de sélection. |
| `photo_quality.triage` | IMPORT; CPU 1; lot 1; hôte `contexte retenu + 20 Mio`, 0/item; I/O 1; GPU 0 | Un groupe par séquence ; garde OpenCV CPU1. Aucun scaling utile déterministe n'est acquis. |
| `acquisition_campaign.run` | JPEG: IMPORT ; RAW: MIXED. CPU 1; lot 1; hôte `contexte retenu + requête transitoire exacte + 256 Kio` + 64 Kio/item ; DEVELOP_RAW ajoute 2 Gio; I/O 1; GPU 0 | Un groupe S3-E par séquence, sans Task imbriqué. La création et la reprise dérivent la même estimation ; la forme historique exacte est normalisée seulement en mémoire. |
| `import.images` | IMPORT; CPU 1; lot 1..32; hôte 128 Kio + `NAME_MAX+64`/item; I/O 1; GPU 0 | Copie/hash I/O-bound. Le callback consomme le lot admis et réadmet entre lots. |
| `features.extract` | CPU; CPU 1..compute-pool; lot 1; hôte 64 Mio + 512 Mio/item; I/O 1; GPU 0 | La demande durable emploie le maximum `int` positif de l'API OpenCV ; le Governor la borne à l'hôte. Le garde applique/restaure exactement le CPU admis. |
| `features.extract.batch` | CPU; CPU 1..12; lot 1..12; hôte 64 Mio + 512 Mio/item; I/O 1; GPU 0 | Images sélectionnées indépendantes, OpenCV CPU1 par participant, enfants joints et publication owner-only ordonnée. Le plafond 12 est une capacité opérationnelle de mesure ; le feedback ≥5 % établit le palier utile sans devenir une limite scientifique. |
| `features.extract.sift` | CPU; CPU 1..compute-pool; lot 1; hôte 64 Mio + 1 Gio/item; I/O 1; GPU 0 | Même contrat OpenCV. Les formes CPU12 et CPU1 historiques complètes sont acceptées et normalisées en mémoire. |
| `features.extract.rootsift` | CPU; CPU 1..compute-pool; lot 1; hôte 64 Mio + 1 Gio/item; I/O 1; GPU 0 | Même contrat que SIFT ; aucune couture GPU scientifiquement compatible n'est validée. |
| `visual_index.update` | CPU; CPU 1..16; lot 1..16; hôte 8 Mio + 2 Mio/item; I/O 1; GPU 0 | Le segment contient au plus 16 Feature Sets indépendants. Au plus `cpu_threads-1` enfants sont joints avant publication owner-only. |
| `candidate_pair.generate` | CPU; CPU 1..64; lot 1..64; hôte 256 Kio + 8 Mio/item; I/O 1; GPU 0 | Le batch de 64 est la borne algorithmique et de ressources. CPU/lot sont couplés pour que chaque palier de feedback exerce ses participants; fenêtre `min(2*CPU, 64, reste_du_lot)`, un handle DB privé par participant, piles enfants de 4 Mio facturées. |
| `matcher.run` | CPU: CPU 1..12, lot sûr 1..12, hôte 0 + 10 Mio/item, I/O 1, GPU 0. ORB Vulkan AUTO: CPU 1, lot utile 1..8, même hôte/item, GPU 1 + 640 Kio, inflight 1. | Le batch/participant 12 est une borne intrinsèque mesurée du Matcher. Batch 12 et depth 2 (1,25 Mio) restent sûrs pour preuves privées, mais insuffisamment utiles en AUTO. CPU complet est le fallback. |
| `geometric_verifier.run` | CPU; CPU utile 1..8, fenêtre sûre 16; lot 1..16; hôte 0 + 8 Mio/item; I/O 1; GPU 0 | Des parents indépendants sont préparés en parallèle, puis publiés/checkpointés en ordre par le propriétaire. L'USAC scientifique conserve `isParallel=false`. |
| `track_builder.run` | CPU; CPU 1; lot 1; fixe `(4 Mio + arêtes * (48 + 2*160)) * facteur`, facteur 2 jusqu'à 400k arêtes puis 8; 0/item; I/O 1; GPU 0 | Rebuild DSU atomique et publication owner-only ; aucune partition scientifiquement validée n'est acquise. |
| `sparse_sfm.run` | CPU; CPU 1; lot 1; fixe `ceil_Mio(128 Mio + 64 Kio/image + 2 Kio/track + 512 octets/observation)`; 0/item; I/O 1; GPU 0 | Exécution atomique FROZEN ; Gate D/E et BA restent à un thread. |
| `incremental_reconstruction.run` | CPU; CPU 1; lot 1; fixe `ceil_Mio(256 Mio + 128 Kio*(caméras base + images extension) + 4 Kio*(landmarks base + tracks extension) + 1 Kio*(observations base + extension))`; 0/item; I/O 1; GPU 0 | Recalcul atomique FROZEN depuis les entrées immuables, sans état solveur persistant. |

Ces bornes sont opérationnelles, externes-library, algorithmiques, mesurées ou
scientifiques selon la dernière colonne. Elles ne créent aucune limite de
cardinalité scientifique, identité ou migration Project DB. Les formes
historiques CPU1/CPU8/CPU12 citées sont des signatures exactes de reprise, pas
des plafonds portables de production.

## SIFT v1A

Une extraction SIFT demande jusqu'au compute-pool hôte, un slot IO, aucun GPU,
pour une image. Sa demande durable utilise `INT_MAX`, borne valide de l'API
OpenCV, mais ce nombre n'est jamais une admission matérielle. L'estimation
structurelle conservatrice est environ 1,06 Gio
(décodage, pyramides, candidats et F32×128), lot 1, pic de record batch zéro.
Le callback applique exactement le contrat positif admis au pool OpenCV
process-wide sous l'unique propriétaire Queue, puis restaure la valeur
précédente sur toutes les sorties. Les tests déterministes ORB, SIFT et
RootSIFT couvrent 1/2/4/8/12 et obtiennent les mêmes keypoints, descripteurs et
métriques ; 12 reste une preuve de l'hôte courant, pas un maximum portable.
Cette dimension opérationnelle ne change ni fingerprint ni Feature Set.

## Responsabilité

Le Resource Governor est l'unique propriétaire des budgets (RAM, GPU, CPU,
IO). Il arbitre les ressources disponibles et calcule les lots adaptatifs pour
chaque tâche.

Le profil interactif par défaut conserve quatre threads logiques lorsque
praticable et au moins un thread de calcul sur un petit hôte. Sur 16 threads,
cela donne 4 threads de headroom ; avec une topologie fiable, la sélection porte
sur des coeurs physiques complets et peut dépasser minimalement la cible
logique. Une nouvelle admission attend également lorsque PSI CPU `some avg10`
atteint 20 %, ou PSI mémoire 1 %. Ces signaux n'interrompent jamais le petit job
déjà réservé.

La réserve dure/admission RAM vaut environ 3 GiB sur un hôte capable. La bande
3–4 GiB place le Governor au minimum en YELLOW et remet la croissance à 1 sans
soustraire 4 GiB de la capacité. La hard floor place immédiatement en RED. Le
premier snapshot swap constitue seulement une baseline ; un delta actif produit
YELLOW, puis RED s'il persiste. L'occupation totale du swap n'est pas un signal
d'activité récente.

La récupération interdit `RED → GREEN` : trois observations saines produisent
RED vers YELLOW, puis trois autres YELLOW vers GREEN. Le plafond reste 1 pendant
ces phases. Une fois GREEN, chaque groupe de trois observations saines double
le plafond : 1, 2, 4, 8, puis les paliers supérieurs utiles aux autres kinds.

## Contrat Gate G gelé

**PASS / FROZEN.** Le contrat d'admission conserve le modèle conservateur
`min(MemAvailable, RAM physique) - réserve hôte - réservations actives`, borné à
zéro. Le double comptage conservateur possible d'une allocation déjà visible
dans `MemAvailable` est accepté : un faux `WAIT` est préféré à un overcommit.

Les snapshots de production emploient `CLOCK_MONOTONIC` et sont valides jusqu'à
un âge exact de 1000 ms inclus. Un snapshot plus ancien ou daté dans le futur
produit `WAIT`, sans réservation ni mutation de l'état de politique du
Governor. La capture synchrone complète impossible reste une erreur
opérationnelle qui fait échouer la tâche avant callback. Une télémétrie PSI ou
vmstat optionnelle absente reste inconnue et ne crée aucune pression fictive.

Gate G core cible un processus Linux natif non contraint et n'est pas cgroup,
systemd `MemoryMax` ou RLIMIT-aware. Il gouverne un seul GPU : le périphérique
DRM de plus petit numéro retenu par Hardware Profile. La capacité et l'usage
doivent provenir de ce même périphérique. La mémoire UMA est débitée exactement
une fois du budget RAM. Le multi-GPU est différé.

Le Governor ne garantit aucune allocation et ne transforme ni swap, ni zram,
ni stockage externe en RAM. Il ne modifie aucun paramètre scientifique. Aucun
scratch, historique RSS long terme, redimensionnement de réservation depuis le
RSS ou monitoring live n'appartient à Gate G core. L'observation courante
bornée RSS/HWM de Compute Governor v2 reste strictement diagnostique.

## Registre de stockage externe et leases scratch

**EXTERNAL_STORAGE_OPERATIONAL_INTEGRATION — CURRENT / VALIDATED
OPERATIONAL.** Cette couture additive postérieure ne modifie pas Gate G core,
`Lardon3DResourceEstimate`, les budgets RAM/GPU/CPU/I/O ni une identité
scientifique. Elle permet au contrôleur physique SSD exact d'enregistrer sous
le mutex Governor une `Lardon3DResourceExternalStorage` bornée : génération
source, état
`ABSENT/DETECTED/AVAILABLE/IN_USE/DRAINING/SAFE/ERROR`, permission de nouvelle
allocation, total/libre scratch connus ou inconnus, swap connu ou inconnu,
nombre de leases, identité stable et raison.

L'enregistrement est exclusif à un objet contrôleur emprunté. Les updates stale
ne peuvent pas restaurer une disponibilité ; une observation malformée ou
indéterminée devient `ERROR` conservateur et bloque les nouvelles allocations.
Un changement matériel incrémente la génération Governor et réveille les
waiters. L'état n'est ni persisté, ni relié à Project DB, ni interprété comme
capacité RAM. Les détails physiques et les capacités F10 restent la propriété
du contrôleur ; le panneau ressources lit l'usage enregistré auprès du
Governor.

La conversion contrôleur→Governor est fail-closed par état. Toute paire ou
autorité exige détection courante du Drive et des deux partitions, identité
Drive+deux UUID exacte/non vide, tailles de partition connues et positives,
ainsi que mount, activité, leases, drain et capacités cohérents. `ABSENT`
interdit tout fait détecté/actif/monté/loué/capacitaire ; `DETECTED` incomplet
reste seulement observable. Un `ERROR` sticky peut conserver le tuple original
malgré la disparition, mais n'alloue rien ; il n'offre un drain qu'après preuve
du même tuple complètement reconnecté. Aucun état amical ni bit
`pairing_valid` ne remplace cette preuve physique.

Les wrappers `lardon3d_resource_governor_acquire_scratch()` et
`lardon3d_resource_governor_release_scratch()` sont l'unique entrée de
production pour les leases. Ils exigent le contrôleur exactement enregistré,
son identité courante et l'autorité explicite d'allocation. `DRAINING`,
`ERROR`, absence, remplacement, état stale ou non-enregistré refusent une
acquisition ; une release exacte reste possible pendant le drain ou l'erreur.
Le Governor ne tient jamais son mutex pendant un appel contrôleur, et le
contrôleur ne rappelle jamais le Governor. La génération et l'état sont
revalidés avant publication afin que la transition ne crée ni inversion de
verrou ni lease non comptée.

`UINT64_MAX` est une saturation source valide, pas un sentinel. L'API publique
continue de refuser une update matériellement différente au même watermark et
ne peut donc pas réaccorder une autorité stale. Seule la complétion du wrapper
exact, déjà sérialisé pour l'objet contrôleur et l'adresse de lease enregistrés,
peut réconcilier à `UINT64_MAX` sa propre acquisition/libération. Le compte
interne fondé sur les adresses reste l'autorité ; une erreur de complétion peut
retirer l'autorité mais ne peut jamais recopier un ancien compte physique.

Le teardown production suit strictement : Queue détruite/jointe et chaque
lease Task rendu, worker SSD joint puis unregister vérifié, contrôleur détruit,
Governor détruit. Un unregister est refusé tant qu'une opération wrapper ou un
lease exact subsiste. Les seize Task kinds courants n'ont aucun consommateur
scratch : le compte normal est donc réellement zéro et la capacité disponible
n'autorise aucun usage implicite. Une future Task consommatrice devra définir
son propre contrat d'éligibilité et son ownership sans transformer scratch ou
swap en RAM.

## API principale

### Création et destruction
- `lardon3d_resource_governor_create()` - Créer un gouverneur
- `lardon3d_resource_governor_destroy()` - Détruire un gouverneur

### Configuration
- `lardon3d_resource_governor_set_policy()` - Définir la politique

### Décision
- `lardon3d_resource_governor_decide()` - Décider de l'admission d'une tâche

### Réservation
- `lardon3d_resource_governor_reserve()` - Réserver des ressources
- `lardon3d_resource_governor_reserve_available()` - Réserver les ressources disponibles
- `lardon3d_resource_governor_release()` - Libérer une réservation
- `lardon3d_resource_governor_reservation_is_valid()` - Vérifier la validité

### Métriques
- `lardon3d_resource_governor_availability()` - Obtenir la disponibilité
- `lardon3d_resource_governor_record_batch()` - Enregistrer les métriques d'un lot
- `lardon3d_resource_governor_generation()` - Obtenir la génération actuelle
- `lardon3d_resource_governor_wait_for_change()` - Attendre un changement
- `lardon3d_resource_governor_pressure()` - Lire GREEN, YELLOW ou RED

### Stockage externe additif
- `lardon3d_resource_governor_register_external_storage()` /
  `lardon3d_resource_governor_update_external_storage()` /
  `lardon3d_resource_governor_unregister_external_storage()` /
  `lardon3d_resource_governor_get_external_storage()` - Copier et observer
  l'état physique borné d'un contrôleur exact
- `lardon3d_resource_governor_acquire_scratch()` /
  `lardon3d_resource_governor_release_scratch()` - Posséder un lease scratch
  de production sous l'orchestration du Governor

## Invariants

1. La Queue/runtime ne décide jamais des ressources
2. Le Resource Governor est l'unique propriétaire des budgets
3. Les réservations sont obligatoires avant toute exécution
4. Les réservations sont libérées exactement une fois
5. Les estimations de ressources sont immuables
6. L'historique des métriques est strictement borné (8 entrées par classe)
7. Un contrat de séquence est immutable jusqu'à sa libération; seule la
   séquence suivante peut être adaptée
8. Swap, scratch et espace SSD n'augmentent jamais la capacité RAM
9. Un lease scratch de production passe par le Governor et reste attaché à
   l'objet lease exact jusqu'à sa libération

## Cycle de vie

```text
1. Capture d'un snapshot de ressources
2. Décision d'admission
3. Réservation opaque
4. Exécution de la tâche
5. Enregistrement des métriques
6. Libération de la réservation
```

## Adaptation dynamique

### Calcul de lots adaptatifs
- Basé sur la consommation mémoire historique
- Mise à jour à chaque exécution
- Conservative (sous-estimation plutôt que sur-estimation)

### Historique borné
- 8 entrées par classe de tâche
- Buffer circulaire
- Mise à jour FIFO

## Réserves

- Sous-estimation temporaire possible avec des estimations statiques
- L'adaptation de débit v2 reste bornée au dernier état par kind/backend ; elle
  ne persiste ni historique volumineux ni décision matérielle.
- L'import `import.images` est admis avec 128 Kio fixes, un coût borné par item,
  un thread CPU, un slot I/O et des lots de 1 à 32. Il enregistre le nombre
  d'images logiques nouvellement enregistrées dans le ScanSet et la durée
  réelle du lot.
  Cela inclut une copie orpheline identique adoptée, même si aucun octet n'est
  recopié. `peak_memory_bytes == 0` signifie explicitement « mesure inconnue » :
  l'échantillon peut conserver taille/durée mais n'alimente jamais l'adaptation
  mémoire.
- Pas de communication inter-classes de tâches
- `features.extract` réserve un lot de 1, demande la borne positive `int`
  d'OpenCV et
  un slot I/O, avec
  64 Mio fixes et 512 Mio par image. Cette estimation conservatrice couvre le
  chemin actuel sans prétendre mesurer les allocations internes d'OpenCV.
  L'admission choisit 1..`compute_pool` et le callback applique ce
  nombre immutable au pool OpenCV. `record_batch` couvre la validation source,
  le décodage, ORB, la publication
  et la finalisation DB ; `peak_memory_bytes == 0` signifie « mesure inconnue ».
- `visual_index.update` demande jusqu'à seize threads CPU, un slot I/O, 8 Mio
  fixes et 2 Mio par Feature Set, par lots de 1 à 16. Le GPU vaut zéro. Le
  callback compte comme participant et crée au plus `cpu_threads - 1` enfants,
  tous joints avant publication et rupture de séquence. Chaque participant
  possède au plus un reader/FD Feature File ; les tranches de postings privées
  partitionnent le buffer borné du segment. `record_batch` compte uniquement
  les memberships commités et conserve la mémoire inconnue à zéro.
- `candidate_pair.generate` demande jusqu'à soixante-quatre threads CPU, un
  slot I/O, 256 Kio fixes et 8 Mio par Feature Set, par lots de 1 à 64. Le GPU vaut
  zéro. La reconstruction reconnaît la forme immédiatement antérieure exacte
  (256 Kio fixes, 64 Kio par item, CPU12) et la plus ancienne forme sérielle
  exacte (128 Kio fixes, 64 Kio par item, CPU1), toutes deux à lot 1..64,
  I/O1, GPU0, classe CPU. Elle les normalise éphémèrement vers la forme courante
  256 Kio + 8 Mio/item, CPU64 ; aucun checkpoint d'estimation seule n'est
  publié et une forme voisine n'est jamais réinterprétée comme legacy.
  `record_batch` compte le nombre de paires générées par séquence et la durée
  réelle du lot ; `peak_memory_bytes == 0` signifie « mesure inconnue ».
  Chaque séquence interroge le Visual Index pour jusqu'à 64 memberships. Le
  calcul emploie des fenêtres internes d'au plus deux sources par thread admis
  et 64 sources au total ; le propriétaire de Task persiste ensuite seul et en
  ordre canonique. Cette estimation opérationnelle ne limite pas la taille
  scientifique du dataset.
- `geometric_verifier.run` réserve 8 Mio par Match Result admis, un slot I/O,
  CPU 1..8 utile et lot 1..16. La fenêtre/participant est indépendamment sûre
  jusqu'à 16, mais CPU12 n'a ajouté que 2,68 % sur CPU8 dans la preuve réelle
  de 4113 parents, sous le deadband 5 %. Les enfants préparent des parents
  indépendants et sont tous joints avant la publication owner-only, le curseur
  contigu, le checkpoint et `sequence_break`. L'USAC interne garde
  `isParallel=false`; CPU/lot ne modifient ni fingerprint ni GVR.
- `matcher.run` demande jusqu'à douze threads CPU, un slot IO et 10 Mio par
  Candidate Pair admise, correspondant au working set contrôlé inférieur à
  environ 10 Mio par paire au
  maximum SIFT/RootSIFT (8 Mio de descripteurs contigus, KNN `k=2`, sorties et
  fichier bornés), hors scratch interne OpenCV. Ses lots CPU restent bornés à
  1..12 Candidate Pairs; ORB Vulkan AUTO normal est borné à 1..8.
  **PASS / FROZEN — P4 / Governor v2 :** une fenêtre
  contient au plus deux paires par thread CPU effectivement admis et douze
  paires au total. Le callback Queue est un participant, crée au plus
  `cpu_threads - 1` enfants et les joint avant publication et libération de la
  réservation. Chaque paire conserve ses buffers dans un stage privé jusqu'à
  sa publication ordonnée ou son nettoyage ; OpenCV reste à un thread interne
  pour éviter une sursouscription imbriquée. Le Governor réserve donc au plus
  120 Mio contrôlés pour un lot de douze ; cette borne opérationnelle ne limite
  pas la cardinalité scientifique du dataset.
  Une Task ORB normale nouvelle persiste la classe honnête `MIXED` avec les
  champs opérationnels CPU12/GPU0, lot 1..12 et 10 Mio par paire : cette classe
  signifie que la politique AUTO peut exécuter une séquence CPU ou Vulkan, pas
  qu'elle réserve les deux simultanément et pas un tag de backend. L'override
  CPU explicite persiste la même forme de ressources en classe `CPU`.
  La signature durable ORB Vulkan courante demande CPU1/GPU1, lot 1..12 et
  640 Kio; son maximum 12 reste une sûreté durable/benchmark, pas le plafond
  utile AUTO. L'enveloppe AUTO normale borne le lot à 8 et fige inflight 1; sur
  UMA ce payload est
  débité une seule fois du budget RAM. Le backend est créé sans payload mappé,
  initialise et conserve exactement 640 Kio pour depth 1. La couture privée de
  sûreté/benchmark peut porter 1,25 Mio pendant un contrat forcé depth 2. Il ne
  redimensionne jamais avec une
  requête pending; la fin de séquence libère le second slot avant la prochaine
  admission depth 1. Une croissance échouée restaure la capacité antérieure,
  et `backend_info` rapporte le payload réellement retenu, non le maximum de
  l'enveloppe. Les anciennes
  formes CPU8/GPU0 et CPU1/GPU1 à lot maximal 8, puis les formes plus anciennes
  CPU12 à 10 Mio fixes, sont seulement des signatures exactes de reprise. Elles
  sont normalisées éphémèrement vers la forme courante correspondante; toute
  forme voisine est rejetée.
  La production normale ORB reconstruit désormais une enveloppe privée
  CPU/Vulkan et demande au Governor un choix `AUTO` avant chaque séquence. Une
  fois admis, ce choix ne varie jamais dans la séquence. La nouvelle signature
  `MIXED` reconstruit AUTO, y compris dans un build portable où son enveloppe
  n'expose que CPU. Pour la compatibilité et la sûreté des overrides, toutes les
  signatures ORB historiques/courantes de classe `CPU` reconstruisent un CPU
  fixe ; une signature Vulkan reste fixe Vulkan. Aucun champ de ressource ne
  sert de faux tag et aucun backend ou matériel n'entre dans l'identité
  scientifique ou le payload Project DB.
  La création et la reconstruction AUTO exposent une capacité Vulkan depuis
  les seules métadonnées build/backend/GPU, sans appeler le driver ni
  pré-dimensionner la RAM. Le Governor évalue ensuite l'enveloppe exacte contre
  son snapshot MemAvailable/PSI/swap et sa charge UMA. Le premier begin et
  donc toute initialisation se déroulent sur le worker Queue après application
  de son affinité. La politique de cache Mesa est déjà établie au démarrage,
  avant ce worker : aucun balayage post-init, latch de Task ou appel d'affinité
  par TID auxiliaire n'existe. Une paire localement inéligible n'initialise
  toujours pas le backend. Le diagnostic de séquence conserve séparément le
  backend
  sélectionné et le backend réel (`CPU`, `ORB_VULKAN` ou mix de paires
  complètes), avec une raison de fallback. Les participants Matcher CPU sont
  comptés uniquement dans `cpu_threads`; `helpers` reste zéro.
  La soumission asynchrone est une couture privée de
  `src/orb_vulkan_backend_internal.h`, absente de l'ABI public. Le backend
  conserve deux slots maximum et un handle exact `slot+generation` par requête;
  `finish` ne peut consommer que ce handle et les comptes soumis. Une génération
  arrivée à `UINT64_MAX` ne boucle jamais : le slot est retiré avant toute
  nouvelle soumission, de sorte qu'un ancien handle ne peut redevenir courant.
  Toute
  sortie/capacité invalide libère son slot, et
  un échec d'attente de fence détruit/met en échec la session avant toute
  nouvelle soumission. Les temporaires et objets pending sont nettoyés sur
  erreur, exception C++, annulation et échec de publication. Une trace de test
  bornée prouve sans temporisation, à depth 2, `SUBMIT(i) < SUBMIT(i+1) <
  FINISH(i) < PUBLICATION_START(i) < PUBLICATION_FINISH(i)` pour deux paires
  769×769, sans modifier leur sortie.
- `track_builder.run` réserve un worker CPU, aucun GPU et aucun fan-out GVR.
  L'estimation est `4 MiB + raw_inlier_edges * (48 + 2*160)` avec facteur 2
  jusqu'à 400000 arêtes inclusivement et facteur 8 au-delà, après vérification
  d'overflow. Le
  facteur élevé protège la transition mémoire observée à grande échelle ; le
  Governor reste l'unique propriétaire de l'admission et de la pression.

## Limites actuelles

- Worker unique (pas de pools multiples)
- Pas de priorités entre tâches
- Pas de persistance des métriques
- Inflight Vulkan normal est fixé à 1, helpers GPU reste 0 et le lot AUTO
  maximal utile est 8. Batch 12 et depth 2 demeurent des capacités privées de
  sûreté/benchmark, chacune rejetée comme politique normale faute du gain de
  débit durable de 5 %. La publication canonique reste owner-only et représente
  environ 29,5 s dans les cohortes contrôlées; avec depth 2 sous 5 %, aucune
  preuve ne justifie un helper supplémentaire face à cette frontière durable.
- Pas de communication avec d'autres gouverneurs

Les corrections dérivables G-D01 (`UINT64_MAX` est le dernier ID valide et la
création suivante échoue sans réservation ni charge comptable),
G-D02 (saturation des compteurs de streak) et G-D03 (identité DRM identique
entre capacité et usage) sont implémentées. Les sept décisions G-B01 à G-B07
sont gelées ; il ne reste aucune décision humaine Gate G.

## Statut

**GATE G — PASS / FROZEN.**

L'intégration opérationnelle SSD/Governor ci-dessus est
**CURRENT / VALIDATED OPERATIONAL**. Le jalon global qui la contient est
`GLOBAL_MAINTENANCE_AUDIT=PASS/FROZEN`. Les validations fraîches
portable/Vulkan, ASan/UBSan, LSan qualifié, TSan et ABI sont acquises ; l'unique
revue finale indépendante a conclu PASS sans finding bloquant. Le gel porte sur
la frontière auditée et ne crée aucun consommateur scratch ni budget RAM.
