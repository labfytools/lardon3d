# Backend Vulkan ORB v1

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

Le sélecteur utilise le travail `feature_count_a × feature_count_b`. Sous le
seuil mesuré de `768 × 768` comparaisons, OpenCV reste utilisé afin d'éviter le
coût fixe du dispatch. Le seuil est une politique d'exécution et ne modifie ni
fingerprint ni Match Result. L'initialisation est lazy : une application qui ne
matche aucun grand couple ORB ne paie aucun cold start.

## Mémoire et pannes

Les buffers maximaux contiennent 256 Kio pour A, 256 Kio pour B et 128 Kio pour
8192 sorties top-2, soit 640 Kio de payload, hors petits objets du driver. Une
mémoire host-visible, cohérente et cached est préférée sur UMA, car elle réduit
nettement le coût CPU de copie/readback observé sur RADV. Le backend sait
appliquer flush/invalidate lorsque le type retenu n'est pas cohérent.

Une absence de loader/device/queue ou un échec d'initialisation est mémorisé et
utilise le CPU sans nouvelle tentative par paire. Une panne de soumission ou un
device lost désactive Vulkan pour la session ; la paire courante est reprise sur
CPU avant toute publication. Un Match Result n'est jamais créé depuis une
sortie GPU partielle.

Le job est synchrone au niveau du Matcher. Il soumet sur l'unique queue détenue
par le contexte puis attend cette queue, sans `vkDeviceWaitIdle` sur le chemin
normal. Le Resource Governor admet la tâche avant le callback ; en RED aucun
nouveau batch ne démarre, tandis qu'une paire déjà soumise finit et se publie.
Le worker unique et le mutex interdisent plusieurs dispatchs concurrents en v1.

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
