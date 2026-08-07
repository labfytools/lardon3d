---
description: Reprendre rapidement un ticket avec Nemotron à partir du handoff
agent: lardon-orchestrator
subtask: false
---

Reprends le ticket suivant :

$ARGUMENTS

Le contexte permanent de Lardon3D est déjà chargé via
`.opencode/context.md`.

Commence toujours par :

1. lire `.opencode/work/current_ticket.md` ;
2. lire le diff courant.

Ne recharge jamais :

- AGENTS.md ;
- l'overview ;
- la documentation générale ;
- les documents d'architecture.

Ne relance jamais `lardon-explore` ou `lardon-architect` si le handoff contient
déjà :

- l'objectif ;
- les contraintes ;
- les fichiers concernés ;
- les invariants ;
- les validations réalisées ;
- le travail restant.

Ne lance une exploration supplémentaire que si une information indispensable est
absente.

Transmets à `lardon-build-backup` uniquement :

- l'objectif ;
- les contraintes ;
- les fichiers concernés ;
- les API concernées ;
- les invariants ;
- le travail restant ;
- la prochaine action sûre.

Ne renvoie jamais tout le handoff au modèle d'implémentation.

Après l'implémentation :

1. appeler `lardon-tests` une seule fois ;
2. utiliser `lardon-build-light` uniquement pour une première revue locale
   simple ;
3. mettre à jour le handoff.

Ne pas appeler :

- `lardon-review` ;
- `lardon-concurrency` ;

car ils utilisent le même modèle que l'implémentation de secours.

Si le ticket touche notamment :

- pthread ;
- mutex ;
- condition variables ;
- scheduler ;
- task_queue ;
- resource_governor ;
- réservations ;
- synchronisation ;

signaler explicitement :

- qu'aucune revue indépendante forte n'a été réalisée ;
- que la revue de concurrence est différée au prochain passage Codex.

Le handoff doit rester court et contenir uniquement :

- objectif ;
- contraintes ;
- fichiers concernés ;
- travail effectué ;
- validations exécutées ;
- erreurs rencontrées ;
- limites restantes ;
- prochaine action sûre.

Règles :

- un seul agent écrit les sources ;
- aucun sous-agent imbriqué ;
- aucun modèle payant ;
- aucun commit ;
- aucun push ;
- aucune modification de `scan3d/`.

Objectif principal :

Reprendre un ticket en quelques secondes avec le plus petit contexte possible,
sans refaire l'analyse déjà présente dans le handoff.
