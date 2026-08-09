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

Le runtime actuel de Lardon3D possède un worker. Le profil interactif conserve
12 threads OpenCV process-wide, quatre threads logiques pour le desktop et un
seul Matcher actif. Lorsque
les pools multi-workers seront introduits, la cible de départ recommandée est
deux Matchers avec huit threads OpenCV chacun pour une charge mixte. Les mesures
montrent toutefois que quatre Matchers à quatre threads favorisent SIFT et les
cas 4096, tandis que deux à huit favorisent ORB 8192. Le Governor devra donc
choisir à partir de la classe de charge, pas d'une constante universelle. Le
nombre de threads OpenCV devra être réglé une fois au démarrage :
`cv::setNumThreads()` est une configuration globale et ne doit jamais être
modifiée concurremment par des workers.

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

La mémoire permanente directement contrôlée vaut 640 Kio de payload. Les tests
de parité couvrent exactement le top-2 jusqu'à 8192, le Match File complet et le
fallback CPU. Ces nombres décrivent la machine mesurée et ne sont pas un contrat
portable de latence.

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
