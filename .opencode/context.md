# Lardon3D — contexte OpenCode durable

Lardon3D est un moteur de photogrammétrie C17, Clang/Meson, persistant et
reprenable. La stabilité du système, les ressources bornées, les publications
atomiques et la réactivité de la TUI priment sur le débit.

## Invariants

- ncurses appartient au thread principal ; le layout ne modifie aucun état.
- Le Resource Governor décide des ressources ; le scheduler applique son
  contrat sans le réinterpréter.
- Aucun callback sans estimation immuable et réservation active.
- Une seule écriture de production et une seule validation lourde à la fois.
- Aucun commit, push, nettoyage massif ou modification de `scan3d/`.
- Préserver tout changement existant hors ticket.

## Long run

`ocaway` exporte `LARDON_OPENCODE_LONG_RUN=1` et lance OpenCode avec
auto-approbation des opérations non explicitement interdites. Les interdictions
configurées restent absolues.

Le ticket courant vit dans `.opencode/work/current_ticket.md`. Chaque phase
majeure actualise décisions, fichiers, tests, échecs et travail restant. Une
session qui approche de sa limite crée aussi `.opencode/work/handoff.md` et
termine par `NEXT SESSION START HERE`.

## Validation

Ordre obligatoire : test ciblé, build normal, tests normaux, ASan/UBSan si
pertinent, TSan si pertinent, stress, seconde revue, concurrence si concernée,
documentation, corrections, revalidation ciblée. Un timeout est un échec à
investiguer, jamais un PASS.

La machine cible possède 16 CPU logiques et 14 GiB de RAM ; `-j8` est la limite
normale. Ne jamais lancer normal, ASan et TSan en parallèle.

## Sorties et contexte

Filtrer les sorties volumineuses. Les sous-agents rendent seulement statut,
conclusions, risques, fichiers, erreur ciblée et prochaine action. Ne pas copier
de longs logs, sources ou diffs dans la conversation ou le handoff.

## Modèles

- Orchestration, lecture, tests, documentation et fallback :
  `opencode/mimo-v2.5-free`.
- Architecture, implémentation, concurrence et revue :
  `opencode/deepseek-v4-flash-free`.

Ces identifiants sont présents dans le catalogue OpenCode 1.18.15 de la machine.
Après deux erreurs identiques du build DeepSeek, utiliser une seule fois le
fallback MiMo à partir du handoff. Après un troisième échec identique, consigner
le blocage ; ne jamais sélectionner implicitement un modèle payant ou inconnu.
