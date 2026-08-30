# Backends Vulkan du Matcher

## Statut

- ORB Vulkan v1 : production, gelé ;
- SIFT Vulkan : rejeté après feasibility sur Radeon 780M ;
- RootSIFT Vulkan : rejeté après feasibility sur Radeon 780M.

Le rejet SIFT/RootSIFT ne modifie ni leur contrat CPU, ni leur identité
persistante. Ils restent sur OpenCV BFMatcher L2.

## Backend Vulkan ORB v1

## Frontière de correction

Le backend Vulkan remplace uniquement la recherche exacte des deux plus proches
voisins ORB/Hamming. La lecture Feature Store, le filtre Lowe, l'ordre canonique,
le Match File, le SHA-256, le Match Result et la reprise restent communs au
backend CPU OpenCV.

Pour chaque feature A, le shader parcourt B et conserve seulement les deux
couples `(distance, feature_index_b)` minimaux. La distance est une somme exacte
de huit `bitCount` sur les 32 octets ORB. L'ordre total est distance croissante,
puis index B croissant. Aucune matrice A×B n'est matérialisée. CPU et Vulkan
doivent donc produire le même top-2, la même décision Lowe et les mêmes octets
persistés. Le backend ne participe pas à l'identité scientifique.

## Contexte et sélection

Le runtime possède un contexte opaque, initialement sans device. La première
paire éligible initialise Vulkan une fois ; les paires suivantes réutilisent le
device, la file, le pipeline, le command buffer et trois buffers
bornés. Un mutex impose un dispatch à la fois. Une famille compute sans graphics
est préférée, avec fallback vers toute famille compute compatible.

Le propriétaire du processus doit établir `MESA_SHADER_CACHE_DISABLE` à la
valeur exacte `true` ou `1` avant de créer ses threads. Les exécutables
Lardon3D normaux prennent le défaut sûr si la variable est absente et refusent
une valeur explicite différente. Le backend public peut cependant être appelé
par un autre consumer après son propre démarrage : sa frontière d'initialisation
ne mute donc jamais l'environnement. Avant tout appel Vulkan/Mesa, elle refuse
une valeur absente, fausse ou malformée, mémorise le backend comme indisponible
et retourne `LARDON3D_ORB_VULKAN_UNAVAILABLE` sans sortie partielle. Les appels
vides et la lecture metadata restent non initialisants. Les exécutables autonomes
de benchmark/feasibility établissent le même défaut sûr comme première action de
`main` ; cette politique reste opérationnelle, non persistée et non scientifique.

Le sélecteur utilise le travail `feature_count_a × feature_count_b`. Sous le
seuil mesuré de `768 × 768` comparaisons, OpenCV reste utilisé afin d'éviter le
coût fixe du dispatch. Le seuil est une politique d'exécution et ne modifie ni
fingerprint ni Match Result. L'initialisation est lazy : une application qui ne
matche aucun grand couple ORB ne paie aucun cold start.

## Mémoire et pannes

Chaque slot contient 256 Kio pour A, 256 Kio pour B et 128 Kio pour 8192
sorties top-2, soit 640 Kio. Le contexte fraîchement créé ne mappe aucun
payload. L'initialisation depth 1, rolling AUTO normal et le wrapper public
synchrone retiennent exactement un slot. Seul un contrat privé de
sûreté/benchmark depth 2 mappe le second, soit 1,25 Mio pendant cette séquence.
Device, pipeline,
layouts et cache restent partagés, hors objets opaques du driver. Commandes,
fences, descriptor sets et queries sont des métadonnées bornées à deux slots,
mais ne rendent pas le second payload mappé tant qu'il n'est pas admis. Une
mémoire host-visible, cohérente et cached est préférée sur UMA, car elle réduit
nettement le coût CPU de copie/readback observé sur RADV. Le backend sait
appliquer flush/invalidate lorsque le type retenu n'est pas cohérent.

Une absence de loader/device/queue ou un échec d'initialisation est mémorisé et
utilise le CPU sans nouvelle tentative par paire. Une panne de soumission ou un
device lost désactive Vulkan pour la session ; la paire courante est reprise sur
CPU avant toute publication. Un Match Result n'est jamais créé depuis une
sortie GPU partielle.
La couche Matcher distingue cette faute backend d'une faute locale. Une lecture
Feature/allocation échouée avant `begin` ne consomme aucun slot; une faute de
filtrage/allocation/staging après un `finish` backend réussi a déjà consommé
seulement son handle exact. Ces deux cas recalculent la paire complète sur CPU,
comptent `other` après publication durable et conservent la disponibilité du
backend ainsi que tout successeur soumis sain.

Le Resource Governor fige inflight 1 avant le callback AUTO normal. La couture
benchmark/test peut forcer 1 ou 2. Au début de la séquence, sans requête pending,
le backend alloue la capacité exacte avant le
premier submit ; une croissance échouée conserve la capacité antérieure et
provoque le fallback CPU complet. Le worker unique soumet jusqu'à cette
profondeur sur l'unique queue, attend chaque fence exacte et publie le préfixe
en ordre, sans `vkDeviceWaitIdle` sur le chemin normal. La fin de séquence,
succès, annulation ou erreur nettoyée, libère le second payload avant la
prochaine admission depth 1. En RED aucun nouveau batch ne démarre; la pression
active ramène l'admission courante permise au depth minimum. Le wrapper public
et le contrôle de benchmark synchrone forcent depth 1. Aucun helper hôte n'est
créé.

L'ABBA corpus forcé mesure 54,661652238 paires/s à depth 1 et 55,797311953 à
depth 2, soit +2,077617 %, sous le deadband 5 %, avec digest identique, quatre fallbacks
locaux et zéro panne/discard. Depth 2 est donc
**REJECTED_WITH_MEASURED_REASON** pour AUTO normal; sa capacité maximale 2
reste disponible uniquement pour les preuves privées reproductibles.
Le S21 AUTO final exerce le backend normal sur 172 741 paires : 172 507
soumissions et 172 507 complétions, zéro panne, zéro discard et aucun slot
pending à la sortie. Les 234 items sous le seuil restent des fallbacks locaux
CPU complets; ils ne sont jamais passés à `finish`. Le digest canonique final
est `e5128a2e599ff593c4f79850e067254b1f249d19e8480a44973306b1af250f70`
pour 172 741 mappings contigus et sans doublon. La réservation AUTO reste un
slot de 655 360 octets UMA pendant tout ce run.

Chaque requête privée porte le couple exact `slot+generation`. La génération
ne boucle jamais : après l'usage de `UINT64_MAX`, ce slot est retiré avant toute
nouvelle soumission et ne peut plus faire correspondre un handle ancien. Un
second slot sain peut terminer sa requête indépendamment. `finish` et `discard`
consomment uniquement le handle exact et ne mélangent jamais leurs sorties.

## Shader et déterminisme

Le shader parcourt les indices B dans l'ordre croissant. Il ne remplace le best
ou le second que pour une distance strictement inférieure : la première égalité,
donc le plus petit index, est conservée. Ce contrat a été comparé exactement à
`BFMatcher(NORM_HAMMING).knnMatch(k=2)` pour 0, 1, 2, 16, 64, 256, 1024, 4096 et
8192 descriptors, ainsi que pour les égalités et descriptors identiques. Le
Matcher complet produit les mêmes Match File et SHA-256 sur CPU et Vulkan.

## Build portable

Meson active Vulkan seulement si le loader de développement et `glslc` sont
disponibles. Le GLSL versionné est compilé en SPIR-V puis incorporé dans un
header généré ; le SPIR-V est contrôlé par `spirv-val` pendant la validation
lorsque l'outil est présent. Sans cette chaîne,
le même code compile avec un stub indisponible et tous les Matchers restent CPU.
L'option Meson `-Dvulkan_orb=disabled` force ce build CPU-only ; `auto` est le
défaut portable et `enabled` exige explicitement le loader et `glslc`.

## Feasibility SIFT et RootSIFT rejetée

Le prototype, compilé uniquement dans les exécutables de faisabilité, partage
le contexte ORB et utilise un workgroup par query. Les
lanes parcourent des tranches fixes des candidats B, accumulent 128 différences
carrées et la lane 0 fusionne les top-2 locaux dans l'ordre total
`(distance carrée, train_idx)`. La sortie reste O(A) et aucune matrice A×B
n'existe. Les buffers F32 maximaux représentent 4 Mio par entrée et 128 Kio de
sortie, soit 8,125 Mio de payload lazy en plus des 640 Kio ORB.

Sur 24 160 requêtes SIFT et 24 161 requêtes RootSIFT contrôlées, y compris
des descriptors produits par OpenCV SIFT et les frontières `nextafter` autour
de Lowe 0,7, aucune divergence Lowe n'a été observée. Ce résultat de corpus
n'est pas une garantie universelle. FP32 ne reproduit pas les distances bit à
bit et un corpus d'égalités adversariales démontre
une divergence d'indices reproductible : OpenCV choisit `(0, 1)` et FP32
Vulkan `(7, 14)`. FP64 choisit `(0, 128)` et ne restaure donc pas le contrat
OpenCV. Le recalcul CPU des distances des deux candidats ne peut corriger une
mauvaise sélection d'indices. Cent répétitions ont produit les mêmes octets GPU
sur cette Radeon 780M et cette pile RADV ; aucune garantie cross-GPU ou
cross-driver n'en est déduite.

Le meilleur workgroup mesuré est 64 pour les grandes paires carrées. Après
fermeture d'une lecture vidéo susceptible d'avoir perturbé la première campagne,
cinq campagnes contrôlées confirment à 8192² un total médian de 75,243 ms contre
115,030 ms CPU pour SIFT (1,53×), et 74,744 ms contre 117,484 ms pour RootSIFT
(1,57×). À 4096², les gains ne sont que 1,14× et 1,11×. Le recalcul CPU par
`cv::norm` ne reproduit pas BFMatcher bit à bit. Les
quatre formes asymétriques 256×8192, 1024×4096, 4096×1024 et 8192×256 restent
toutes plus lentes que le CPU. FP64 mesure environ 271 ms à 8192², soit environ
0,44× le CPU. Le gain FP32 ne compense pas une identité backend obligatoire,
une politique de fallback durable distincte et une sélection plus complexe.

Ce rejet concerne Lardon3D v1 sur la cible mesurée, pas Vulkan ou SIFT en
général. Conclusion : aucune Gate B, aucun sélecteur, aucun fallback SIFT Vulkan, aucune
modification du fingerprint et aucune migration DB. CPU OpenCV demeure le seul
backend SIFT/RootSIFT de production. Le shader et ses tests restent des preuves
reproductibles ; ils ne sont ni exposés par l'API normale, ni embarqués dans les
binaires de production.
