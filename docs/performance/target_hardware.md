# Profil de performance de la machine cible

Ce document décrit une politique de performance mesurée. Il ne modifie aucun
contrat de correction du Matcher, du Match Store ou du Match Result.

## Cible principale actuelle

- AMD Ryzen 7 8845HS, Zen 4, 8 cœurs et 16 threads SMT ;
- Radeon 780M à mémoire système partagée ;
- 16 Gio de RAM et zram d'environ 6 Gio ;
- Arch Linux, Clang 22, OpenCV 5.0.0.

Le build OpenCV observé emploie TBB 2023.1, expose 16 threads par défaut et les
chemins SIMD jusqu'à AVX512-SKX. Il a été compilé avec OpenCL, mais
`cv::ocl::haveOpenCL()` retourne faux. Vulkan énumère en revanche
`AMD Radeon 780M Graphics (RADV PHOENIX)`, API 1.4.354, avec une file compute
dédiée et de la mémoire UMA host-visible/cohérente.

Le sysfs amdgpu de cet hôte expose 512 Mio dans `mem_info_vram_total` et
7 986 020 352 octets dans `mem_info_gtt_total`. Le profil matériel conserve les
512 Mio comme capacité de payload observable, mais la combinaison petit
aperture/GTT à l'échelle de la RAM suffit à classer ce GPU shared/UMA sans
hardcoder son device ID. Le Governor débite alors les ressources GPU exactement
une fois de `MemAvailable` et n'utilise jamais cet aperture comme mémoire libre
séparée pour contourner les objectifs 3 Gio/2 Gio.

Le runtime actuel de Lardon3D possède un worker lourd. Le profil interactif
établit au démarrage une baseline/plafond OpenCV de 12 threads et réserve quatre
threads logiques au desktop. Pour chaque séquence, l'unique callback Queue
applique temporairement le compte CPU immuable admis dans `1..12`, le vérifie et
restaure la baseline sur toute sortie. `cv::setNumThreads()` reste une
configuration process-wide : sa mutation concurrente par plusieurs workers
n'est pas supportée. Un futur pool multi-worker devrait donc changer ce modèle
explicitement, pas multiplier silencieusement ces mutations globales.

Le worker Queue applique à lui-même le compute-pool `0-5,8-13`, tandis que le
creator/main reste unrestricted `0-15`. Certains helpers de cache disque Mesa
observés avaient réélargi leur affinité après l'initialisation lazy. Comme un
pidfd ne stabilise pas le TID numérique pour `sched_setaffinity(tid)`, Lardon3D
ne tente aucune mutation auxiliaire. Il établit plutôt
`MESA_SHADER_CACHE_DISABLE=true` avant toute création de pthread applicatif et
toute initialisation Vulkan. Une absence prend ce défaut sûr ; les valeurs
explicites exactes `true`/`1` sont respectées, tandis qu'une valeur explicite
fausse ou malformée reste inchangée mais interdit le démarrage. Sur la 780M
validée, les helpers `*:disk$0` sont absents et tous les threads runtime vivants
observés gardent `0-5,8-13`. Cette politique est opérationnelle et inoffensive
hors Mesa ; elle n'ajoute ni identité scientifique, ni état durable, ni sweep
post-init.
Le backend public ne suppose pas que tout consumer traverse ce démarrage : sa
première requête Vulkan non vide vérifie `true`/`1` sans modifier
l'environnement, avant tout appel Mesa. Une valeur absente ou différente
retourne `UNAVAILABLE`, mémorise l'indisponibilité et ne produit aucune sortie.
Les benchmarks et la feasibility autonomes établissent le défaut sûr comme
première action de `main` ; les lectures metadata restent non initialisantes.

Les anciennes mesures exploratoires montrent que quatre Matchers à quatre
threads favorisent SIFT et les cas 4096, tandis que deux à huit favorisent ORB
8192. Elles n'établissent pas un pool de production actuel. Tout futur modèle
devrait être choisi par le Governor à partir de la classe de charge et prouver
un contrôle de concurrence compatible avec la configuration globale OpenCV.

Quatre Matchers avec un ou deux threads chacun dégradent fortement les grands
cas ORB. Quatre fois quatre threads augmente le working set et la variance, mais
peut améliorer SIFT soutenu. Le Governor devra compter les threads OpenCV dans
le budget CPU afin d'éviter `workers × threads` supérieur aux 16 threads
matériels.

Le working set directement contrôlé d'un Matcher SIFT/RootSIFT reste inférieur
à environ 10 Mio, hors scratch TBB/OpenCV. Même plusieurs Matchers restent loin
de la pression mémoire sur 16 Gio ; les mesures n'ont produit aucun swap-in ni
swap-out. La limite pratique observée est le CPU, pas la RAM.

## Fallback portable

Le build portable conserve les réglages Meson génériques et ne force ni
`-march=native`, ni OpenCL, ni un nombre de threads spécifique au 8845HS. Sur une
autre machine, laisser OpenCV choisir son backend et limiter l'orchestration à
un Matcher reste le fallback sûr. Une future configuration multi-worker devra
être dérivée du profil matériel par le Resource Governor, sans second scheduler.

## Méthode et portée

`benchmark-matcher` utilise des Feature Sets synthétiques persistés, un warm-up
et sept répétitions dont il rapporte la médiane. Les mesures absolues sont
locales et sensibles au boost et à la température ; la décision repose surtout
sur le scaling et le débit soutenu. La campagne a été exécutée avec le governor
Linux `powersave`; après charge soutenue, 61–72 °C ont été observés et aucun
swap-in/swap-out. Le coût dominant reste l'évaluation exacte des distances dans
`cv::BFMatcher::knnMatch`.

Le backend Vulkan ORB de production est borné, utilise une invocation par query
et ne matérialise aucune matrice A×B. Sur la 780M, le workgroup 32 est le meilleur
des quatre candidats 32/64/128/256 à 4096 et 8192. Le chemin complet warm mesure
environ 0,18 ms à 256, 0,45 ms à 1024, 1,6 ms à 4096 et 4,0 ms à 8192, contre
environ 0,10, 0,96, 14,7 et 59,6 ms pour BFMatcher CPU lors de la campagne
production. L'initialisation lazy mesurée vaut environ 129–136 ms. Le seuil
`feature_count_a × feature_count_b >= 768²` évite le GPU pour les petits travaux.

La mémoire directement contrôlée vaut 640 Kio par slot; rolling AUTO peut
réserver un ou deux slots, soit au plus 1,25 Mio, débités une fois de la RAM sur
la 780M UMA. Le backend mappe exactement cette capacité pendant la séquence et
rend le second slot avant une admission depth 1 suivante. Les tests de parité
couvrent exactement le top-2 jusqu'à 8192, le Match File complet et le fallback
CPU. Ces nombres décrivent la machine mesurée et ne sont pas un contrat portable
de latence.

Le run S21 AUTO final sur cette cible mesure 172 741 résultats durables en
2 345,444485079 s, soit 73,649/s. Sur les échantillons connus, GPU busy vaut
26 % en moyenne, 27 % en médiane et 36 % au maximum; l'utilisation moyenne
connue du compute-pool vaut 9,68 % de ses 12 CPU logiques. Le RSS observé
culmine à 168 980 480 octets et le HWM à 250 658 816 octets. Le minimum
`MemAvailable` reste 10 927 390 720 octets, PSI mémoire maximal et deltas swap
restent nuls. Après le run, Sway répond, Firefox et son flux PipeWire vers le
casque actif restent présents; PSI mémoire avg10/60/300 est nul. Ces signaux
objectifs attestent la conservation du desktop, sans prétendre mesurer une
perception subjective.

## Geometric Verifier Fundamental — Gate A

Le 9 août 2026, `benchmark-geometric-verifier` a comparé FM_RANSAC, USAC_DEFAULT, USAC_MAGSAC,
USAC_ACCURATE et MAGSAC à seed locale sur le Ryzen 7 8845HS, OpenCV 5.0.0 et Clang 22.1.8. Sur
8192 correspondances, 70 % d'outliers et bruit 0,75 px, le candidat local mesure
10,80/11,04/11,35 ms médiane/p95/pire. FM_RANSAC mesure 316,44/318,87/319,66 ms avec seulement
0,413 de recall.

Avant campagne, PSI CPU/mémoire/I/O avg10 était nul, `MemAvailable` environ 9,72 Gio et
`pswpin/pswpout` nul. Après 30,4 s à environ 99 % d'un CPU, PSI et swap restaient nuls,
`MemAvailable` environ 9,74 Gio et le capteur CPU observé passait d'environ 54,6 à 55 °C. Ce sont
des proxies objectifs, pas une mesure subjective de fluidité.

Le coût court et le faible working set rendent Vulkan `NOT_JUSTIFIED` pour ce verifier sur la
Radeon 780M. Le GPU reste utilisé uniquement par le Matcher ORB existant.

Le profil production réserve un CPU logique, 4 Mio et un worker, avec lots 1/2/4/8. Un run de la
vraie Task sur 1000 parents, suivi de ses chemins de reprise/configuration, a traversé environ
2001 parents réutilisés en 5,870 s (environ 341/s). Ce chiffre inclut DB, checkpoints et fixture ;
il ne remplace pas la latence estimator-only Gate A. Le processus de test a culminé à 25 964 Kio
RSS. `MemAvailable` a varié de 10 702 988 à 10 692 916 Kio, sans swap ; PSI avg10 final était
0,34 % CPU et 0 % mémoire/I/O. Les latences médiane/p95 par parent et le RSS début/fin n'étaient
pas mesurables avec ce harness et ne sont donc pas extrapolés.

## Feasibility Vulkan SIFT / RootSIFT

Sur le même RADV PHOENIX, `shaderFloat64`, les timestamps compute, un subgroup
de 64 lanes et les workgroups jusqu'à 1024 invocations sont disponibles. La
meilleure variante SIFT FP32 utilise 64 lanes. Après fermeture d'une lecture
vidéo et retour à un PSI avg10 nul, cinq campagnes donnent à 8192² : SIFT
115,030 ms CPU contre 75,243 ms total potentiel (1,53×), et RootSIFT
117,484 ms contre 74,744 ms (1,57×). À 4096², les gains totaux tombent à 1,14×
et 1,11×. Les quatre paires asymétriques testées perdent face au CPU. FP64
atteint environ 271 ms à 8192².

La campagne numérique trouve une divergence top-2 sur des sommes égales
adversariales pour FP32 comme FP64, des distances et Match Files différents,
mais aucune divergence Lowe sur le corpus testé. Le backend devrait donc porter
une identité scientifique propre et ne pourrait pas employer le fallback CPU
transparent d'ORB. Cette complexité n'est pas justifiée par le profil de
performance : SIFT et RootSIFT Vulkan sont rejetés pour Lardon3D v1 sur cette
cible, OpenCV reste la référence production.

Un run soutenu de 5000 dispatchs mixtes 1024/4096/8192/4096 a traité environ
1232 paires/s en 4,06 s. Le processus de benchmark complet a culminé à environ
202 Mio RSS ; les compteurs `pswpin` et `pswpout` sont restés à zéro. Après le
run, PSI CPU `some avg10` valait 0,14 %, PSI mémoire et I/O 0 %, avec 76 °C CPU
et 64 °C au bord GPU. Ces mesures sont des observations ponctuelles, pas des
seuils du Resource Governor.
