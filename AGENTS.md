# Guide des agents Lardon3D

## Validation obligatoire

Avant livraison, exécuter réellement :

```sh
CC=clang meson setup build --wipe
meson compile -C build -j8
meson test -C build --print-errorlogs
git diff --check
```

Pour les changements sensibles à la mémoire ou aux durées de vie, ajouter un
build ASan/UBSan séparé. Pour toute concurrence, exécuter aussi TSan lorsque
disponible. Ne jamais annoncer une vérification non exécutée.

## Git et périmètre

- Ne jamais utiliser `git add -A`.
- Ne jamais ajouter ou modifier `scan3d/`, notamment
  `scan3d/tri_photos.py`.
- Ne faire ni commit ni push depuis le sandbox.
- Préserver les changements existants hors ticket.
- Fournir à la fin la liste exacte des fichiers appartenant au ticket.

## Architecture à préserver

- La TUI orchestre les entrées et ncurses ; le métier et le layout restent
  séparés. Le layout dessine uniquement et ne modifie aucun état.
- ncurses appartient exclusivement au thread principal.
- Toute tâche possède une estimation immuable et une réservation valide avant
  son exécution. Aucun callback ne démarre sans réservation active.
- Le Resource Governor décide des budgets, slots et lots. Le scheduler applique
  le FIFO et exécute le contrat ; il ne réinterprète jamais les ressources.
- Les API à durée de vie complexe restent opaques, avec propriété et nettoyage
  explicites.

## Principes non négociables

- La stabilité du système hôte et la réactivité de la TUI passent avant le
  débit.
- Aucun traitement lourd monolithique : utiliser des séquences adaptatives.
- Budgets, files et buffers doivent être bornés.
- La RAM d'un iGPU partagé appartient au budget RAM système.
- La zram est un filet de sécurité, jamais un budget de travail.
- Publier uniquement des sorties atomiques validées ; effectuer un rollback
  ciblé sans toucher aux données antérieures.
- Préserver des frontières permettant la reprise après interruption.
- Le viewer reste séparé, lecteur de snapshots validés et non bloquant.

## Contraintes de code

- C17, Clang, Meson et ncursesw.
- Aucune variable globale d'état.
- Nettoyer explicitement chaque allocation, descripteur, mutex, condition et
  thread.
- Ne pas utiliser `system()` ni `popen()` dans le code de production.
- Ne laisser aucun `TODO`, code mort ou dépendance inutile dans un ticket fini.
- Préférer l'évolution minimale aux réécritures de modules validés.
- Viser 100 colonnes et ne jamais dépasser 120 sans justification locale.
- Mettre les appels complexes et le SQL sur plusieurs lignes lisibles.
- Éviter le code-golf et garder des fonctions confortables à relire dans Neovim.
- Commenter les invariants non évidents plutôt que paraphraser le code.

## Long run OpenCode

- `.opencode/work/current_ticket.md` est la mémoire durable du ticket et doit
  être actualisé après chaque phase majeure.
- Avant une compaction ou une fin de session incomplète, écrire
  `.opencode/work/handoff.md` avec la prochaine action exacte.
- Une seule validation lourde à la fois : normal, ASan/UBSan, TSan puis stress.
- Un timeout doit être isolé et expliqué ; le répéter jusqu'au vert est interdit.
- Après les tests verts, imposer revue, audit de concurrence si pertinent,
  documentation, corrections et revalidation ciblée.

## État actuel

- Moteur de tâches FIFO, un worker, pause et annulation coopératives.
- Gouverneur thread-safe avec estimations, budgets et réservations opaques.
- Scheduler relié au gouverneur ; réservation obligatoire avant callback.
- Aucun DAG, aucune priorité, aucun pool de workers.
- L'import asynchrone `import.images` utilise le scheduler générique, ses lots
  adaptatifs et ses checkpoints persistants.
- Le viewer Vulkan n'est pas commencé.

## Prochains tickets recommandés

1. Sélectionner une tâche admissible sans blocage par la tête de file ✓
2. Introduire le DAG et les dépendances.
3. Persister les tâches et checkpoints de reprise.
4. Orchestrer et mesurer les séquences adaptatives.
5. Ajouter les pools bornés CPU, IO et GPU.
6. Migrer l'import vers le scheduler générique.
7. Ajouter la publication live validée, puis le viewer Vulkan séparé.

## Règles documentaires

### README.md racine
Le README.md à la racine est le sommaire canonique de la documentation.

### Vérification avant ticket architectural
Avant un ticket architectural important :
1. Identifier le document canonique correspondant dans docs/
2. Le lire
3. Vérifier que le ticket respecte ses invariants

### Mise à jour après ticket
Après un ticket modifiant :
- API
- Architecture
- Ownership
- Concurrence
- Persistance
- Pipeline
- Limites

Appeler lardon-docs avant de considérer le ticket terminé.

### Contradictions code/documentation
Si code et documentation architecturale se contredisent :
1. Ne pas choisir silencieusement
2. Signaler la contradiction
3. Décider quelle spécification doit devenir canonique
4. Mettre la documentation à jour
5. Seulement ensuite poursuivre

### Nouveaux documents
Tout nouveau document docs/** doit être référencé depuis README.md si c'est
un document canonique destiné aux lecteurs du projet.

### Unicité des documents
Ne jamais créer plusieurs documents canoniques décrivant le même contrat.
