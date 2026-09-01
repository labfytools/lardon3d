# Exécution et runtime Lardon3D

## Modèle d'exécution

### Threads
- Thread principal : entrée, modèle de vue et rendu TUI ncursesw (exclusif)
- Thread worker Queue : exécution sérielle des tâches métier
- Participants internes : uniquement ceux du contrat Task admis, joints par le
  callback propriétaire avant publication
- Opération SSD : au plus un thread joinable, uniquement pendant une opération
  UDisks bornée ; il ne rend rien et ne devient ni Queue ni scheduler

### Synchronisation
- Mutex pour les accès partagés
- Variables de condition pour la coordination
- Atomicité des opérations critiques

## Cycle de vie d'une tâche

```text
1. Création (PENDING)
2. Soumission à la file
3. Sélection FIFO/adaptative par la Queue
4. Réservation obligatoire
5. Exécution (RUNNING)
   - Pause/reprise coopérative
   - Annulation coopérative
   - Séquences adaptatives
6. Complétion (COMPLETED) ou Échec (FAILED)
7. Nettoyage des ressources
```

## Synchronisation

### Mutex
- Protection des données partagées
- Accès exclusif aux ressources critiques

### Variables de condition
- Coordination entre threads
- Notification de changement d'état
- Attente passive ; timeout borné seulement pour réévaluer un `WAIT` ressources

### Atomicité
- Opérations indivisibles
- État cohérent garanti

## Gestion des erreurs

### Rollback
- Rollback des transactions locales avant publication
- Nettoyage complet des ressources possédées par l'opération
- Une publication fichier réussie suivie d'un échec DB peut laisser un orphelin
  valide ; aucune transaction distribuée fichier+SQLite n'est revendiquée

### Récupération
- Reprise à la dernière frontière connue
- Ignorance des artefacts partiels
- Validation avant publication

## Limites actuelles

- Worker Queue unique (pas de pools inter-Tasks multiples)
- Pas de parallélisme inter-Tasks ; certains kinds possèdent des participants
  internes bornés, comptés par leur contrat Governor
- Reprise automatique limitée aux tâches indépendantes reconstructibles

## Reprise durable

Un snapshot ne conserve que l'état logique d'une tâche. `RUNNING` et `PAUSED`
sont normalisés vers `PENDING`; aucun worker, callback brut, pointeur, contrat
ou réservation n'est restauré. Le propriétaire fournit un nouveau callback et
resoumet la tâche. Les états terminaux sont conservés.

`started_at` désigne le début de la tentative d'exécution courante, pas le
premier démarrage historique. Un checkpoint `RUNNING` restauré en `PENDING`
conserve temporairement l'horodatage de la tentative interrompue pour
l'observation ; lors de `lardon3d_task_start()`, `started_at` est remplacé par le
nouveau démarrage et `finished_at` est remis à zéro. `finished_at` n'est fixé
qu'à la terminaison de cette tentative.

**IMPLEMENTED** — snapshot, codec v1 et restauration isolée.

**IMPLEMENTED** — l'import `import.images` se sauvegarde à chaque fin de lot et
se reconstruit explicitement avec un userdata neuf lié au projet rouvert. Son
intention durable contient `source_path + scanset_id`; le hash/copie et la
transaction catalogue restent hors mutex Task et hors mutex DB pendant l'I/O.

**IMPLEMENTED** — `project_open()` inventorie par pages de 8, restaure puis
resoumet automatiquement les tâches production valides. Il retourne après
l'enqueue et n'attend jamais leur terminaison.

L'ordre d'initialisation production est : politique driver, profil matériel,
Governor, backend, Queue/worker, contrôleur SSD optionnel et binding
Governor, puis TUI. L'ouverture DB/projet et la reprise synchrone sont pilotées
ensuite depuis le thread principal. Une fermeture ne peut commencer qu'après le
retour de `project_open()`. Le worker peut consommer pendant le scan ; chaque
tâche exécutée est néanmoins réadmise normalement.

**NOT_YET_WIRED** — reprise ordonnée par dépendances/DAG. Les kinds de
production reconstructibles checkpointent déjà à leurs frontières métier ;
aucun timer autosave générique ne doit avancer devant leur publication durable.

**IMPLEMENTED** — reprise sélective des kinds reconstructibles via Project DB,
Task Kind Registry et Queue. Les dépendances/DAG restent différées ; il
n'existe pas de scheduler global distinct à restaurer.

## Accès Project Database

**IMPLEMENTED** — une connexion SQLite opaque sérialisée par mutex interne ;
les opérations multi-tables sont transactionnelles et bornées.

**IMPLEMENTED** — le cycle de vie projet ouvre/crée `project.db`, vérifie
l'identité et ferme la connexion. Ouvrir, fermer ou changer de projet est une
frontière exacte : l'observateur et la vue optique libèrent leurs borrows, puis
l'unique Queue est annulée, jointe et détruite, callbacks terminaux inclus,
avant la fermeture de Project DB. Une seule Queue vide est ensuite recréée et
les observateurs sont rebondés. Il n'existe jamais deux schedulers simultanés.
L'historique terminal et l'espace d'IDs Queue sont ainsi propres à la session ;
les mêmes Task IDs durables de deux projets restent indépendants et aucun
historique fourni n'est affiché lorsqu'aucun projet n'est chargé.

**IMPLEMENTED** — la registry reconstruit explicitement callback/userdata hors
mutex DB pour un kind connu ; elle ne soumet aucune tâche.

**IMPLEMENTED** — la queue accepte un identifiant restauré préassigné s'il
n'entre en collision avec aucune tâche connue. L'import production peut donc
être reconstruit puis soumis explicitement.

**IMPLEMENTED** — la resoumission automatique utilise la registry production,
conserve le task ID et laisse le worker obtenir une nouvelle réservation.
Kinds inconnus, tâches legacy, checkpoints invalides et sources absentes ne
bloquent pas l'ouverture.

**IMPLEMENTED** — `visual_index.update` reprend à la dernière membership
commitée. Un segment temporaire n'est jamais visible et un rejeu exclut les
Feature Sets déjà membres.

## Durée de vie terminale et fermeture Queue

Une Task terminale reste vivante jusqu'au retour complet de son callback
terminé. Queue la retire alors de la liste active et la détruit hors de son
mutex ; seule une histoire de 64 snapshots reste observable. Les appels déjà
enregistrés avant `task_queue_destroy()` sont attendus. Le propriétaire doit
empêcher tout nouvel appel dès le début de la destruction, règle nécessaire à
toute API C adressée par pointeur brut.

Un callback terminé peut consulter les vues Queue tant que le propriétaire la
maintient vivante. Il ne peut pas détruire cette Queue, retirer son propre
record ni attendre une opération dépendante de son retour.

## Observatoire TUI actuel

**CURRENT / VALIDATED OPERATIONAL.** Ce statut décrit l'implémentation et ses
tests courants. L'audit global qui contient cette frontière est désormais
`PASS/FROZEN` après revue indépendante ; le statut TUI reste volontairement
opérationnel et n'interdit pas ses évolutions futures sous un ticket distinct.

### Séparation modèle, observation et rendu

Le modèle `tui_model` est pur et testable sans terminal. Le renderer reçoit
seulement des copies bornées et n'interroge ni Queue, ni Governor, ni Project
DB, ni contrôleur SSD. Toutes les fonctions ncurses, l'entrée clavier et le
rendu demeurent sur le thread principal.

L'observateur runtime emprunte Queue et Governor et conserve une seule copie
cohérente. Les captures ordinaires sont coalescées pendant au moins une seconde
monotone ; un échec conserve la dernière vue bornée en la marquant stale.
Il observe au plus 129 Tasks : les 64 pending possibles, l'unique active et les
64 snapshots terminaux récents. L'ordre Queue place le travail vivant du plus
récent au plus ancien, puis l'histoire par terminaison décroissante ; une Task
active ne peut donc pas être masquée par un vieux préfixe historique. Il
n'existe ni scan DB par frame, ni lecture `/proc` volumineuse, ni historique
non borné.

Les ABI historiques restent exactes : `Lardon3DTaskSnapshot`,
`Lardon3DResourceSnapshot`, `Lardon3DAppState` et
`lardon3d_layout_draw()` ne sont pas étendus en place. Les surfaces additives
`Lardon3DTaskObservation`, `lardon3d_task_queue_observe()`,
`Lardon3DResourceObservation`, `Lardon3DRuntimeSnapshot` et
`lardon3d_layout_draw_runtime()` portent les nouveaux champs. De même,
`lardon3d_tui_run()` reste le symbole historique ; l'application utilise
`lardon3d_tui_run_with_ssd_operation()` avec un owner SSD conservé hors de
`Lardon3DAppState`.

### Progression et ETA

Une Task typée publie `completed/total` seulement après son propre commit
métier durable. Quand ces compteurs sont connus, la TUI les affiche toujours et
en dérive le pourcentage sans utiliser le message ou le nom. Une Task marquée
`COMPLETED` avec un préfixe durable incomplet est une erreur d'intégrité
visible, jamais 100 %. Quand les comptes typés sont inconnus, le lifecycle peut
être terminal mais la progression scientifique reste indéterminée. Le
pourcentage générique non typé, lorsqu'il est utile, porte explicitement le
libellé runtime.

Le débit est un EWMA borné. La première observation établit seulement le
préfixe de reprise et ne contribue pas au taux ; une reprise de RUNNING remet
également la fenêtre temporelle à zéro. Deux intervalles strictement positifs
sont nécessaires avant un débit et une ETA connus. Une absence de progrès,
une pression Governor, une régression ou une preuve insuffisante produit
respectivement `STALLED`, `THROTTLED`, reset ou `INDETERMINATE/CALCULATING`.
Seule une complétion cohérente vaut exactement 100 % et ETA zéro ; aucune fausse
précision n'est affichée.

### Pipeline et ressources

La synthèse utilise les étapes Acquisition, RAW, Quality, Features, Visual
Index, Candidate, Matcher, GV, Tracks, Sparse SfM et future Dense. Les états
sont `NOT_READY`, `READY`, `QUEUED`, `RUNNING`, `THROTTLED`, `BLOCKED`,
`COMPLETE`, `FAILED` et `NOT_APPLICABLE`. Dense reste explicitement
`NOT_APPLICABLE` tant qu'aucun Task kind de production n'existe ; une étape
future n'est jamais devinée active depuis un nom ou un message.

Le panneau ressources expose CPU actif/admis/disponible et sa raison, GPU
présent/mémoire/busy/backend lorsqu'ils sont connus, RAM/MemAvailable/réserve,
swap total/utilisé et deltas actifs, lot/inflight/helpers/I/O, scratch et
pression Governor GREEN/YELLOW/RED. Le contrat installé de l'exacte Task active
est l'autorité pour CPU et lot. Un dernier diagnostic privé seulement indexé
par kind peut appartenir à une autre Task ou séquence : backend, inflight,
helpers, utilisation ou raison restent donc `UNKNOWN` sans association exacte
Task+séquence. La mémoire UMA est comptée une seule fois et ni swap ni scratch
ne sont ajoutés à la capacité RAM.

### Dimensions, couleurs et clavier

Le layout complet demande au moins 100×30. Le layout compact est validé à la
frontière 72×20 et reste supporté jusqu'au minimum 60×15. En dessous, le rendu
se réduit au message borné `Terminal trop petit`; un resize recalcule la classe
sans faire travailler un worker. Les rôles sémantiques sont healthy vert,
warning jaune, error rouge, GPU cyan, CPU bleu, SSD magenta, plus dim/bold.
Les libellés textuels demeurent l'autorité lorsqu'il n'y a pas de couleur ou
pas assez de paires terminal.

Les écrans courants sont accueil, projets, import, viewer futur, tâches,
ressources, optique, SSD et aide. `F1..F7` naviguent respectivement vers aide,
projets, import, viewer, tâches, ressources et optique. Le segment littéral
`F10 SSD` est réservé au début du footer et reste visible à 60 colonnes dans
tous les modes pertinents. Les footers dérivent du même mode que le handler :

- saisie active : Enter valide, Échap annule, F10 reste disponible ;
- import actif : `X` demande l'annulation et F10 reste disponible ; `q` et
  Échap sont affichés comme désactivés ;
- mode idle : `q`, Échap/navigation et les commandes propres à l'écran sont
  annoncés seulement lorsqu'ils sont réellement traités ;
- Tasks : flèches/`j`/`k`, `P` pause, `R` reprise, `C` annulation ;
- Optique : Tab change de panneau, flèches/`j`/`k` sélectionnent, `[` revient à
  la première page et `]` charge la suivante ; `B/L/C/V/A/G/K/E` déclenchent
  les opérations indiquées et `R` retente explicitement un bind/chargement.

### Workflow optique

La TUI consomme les API v23 décrites dans
[Project Database](project_database.md), sans SQL direct ni édition d'une ligne
immuable. Elle inspecte une affectation Capture, effectue seulement des lookup
metadata exacts, liste les profils de boîtier/objectif/configuration et accepte
un objectif manuel sans électronique ni alias — le Meike de test est un cas
normal, pas une branche produit. « Modifier » signifie créer un nouveau profil
ou une nouvelle configuration immuable, puis l'assigner explicitement à un
groupe de campagne encore éligible ou à un Capture non affecté. Les
calibrations listées doivent être compatibles avec l'exacte configuration et
la sélection reste explicite ; absence, ambiguïté, incompatibilité, BUSY, I/O
et corruption sont rendues sans profil fabriqué. Les pages ont 16 lignes,
rapportent un compte page-local et un indicateur « suite » exact.

### SSD F10 et lifetime application

La TUI affiche les huit états physiques `ABSENT`, `DETECTED`, `ENABLING`,
`ENABLED`, `IN_USE`, `DRAINING`, `SAFE_TO_UNPLUG` et `ERROR`, avec identité
stable, modèle/télémétrie lorsqu'ils sont connus, swap, scratch, mount, usage,
leases, drain et raison. `UNKNOWN` n'est jamais remplacé par zéro ou par une
supposition ; `SAFE_TO_UNPLUG` est mis en évidence comme endpoint sûr. F10
choisit exclusivement l'une des capacités exactes
`can_enable`, `can_disable` ou `can_cancel_drain` publiée par le contrôleur ; un
état incomplet, une paire de remplacement ou un résultat malformé n'accorde
aucune autorité. L'opération synchrone UDisks s'exécute dans au plus un thread
joinable, tandis que le main continue de rendre et de poller sans blocage.

La validation qui alimente ces capacités est fail-closed par état : toute
autorité exige Drive et deux partitions détectés, identités Drive+UUID exactes,
extents positifs connus et faits mount/activité/drain cohérents. `ABSENT` ne
peut transporter aucun fait actif, `DETECTED` partiel n'a aucune action et un
hazard `ERROR` déconnecté ne peut que retenir l'identité originale sans
allocation. Seule la reconnexion complète de ce tuple peut autoriser son drain.

Après chaque observation ou résultat validé, l'adaptateur enregistre une copie
bornée de l'état physique auprès du Governor. Une copie malformée devient
`ERROR` et interdit les nouvelles allocations ; l'observation ressources lit
cet état Governor-owned, tandis que les détails/permissions F10 restent dans
le snapshot physique. La génération source peut saturer à `UINT64_MAX` : une
update publique égale ne réaccorde jamais une autorité stale ; seule la
complétion du wrapper exact déjà engagé réconcilie son lease adressé. À l'arrêt,
l'ordre est : destruction/join de la Queue et
libération de chaque lease Task, fermeture du projet/DB, join puis unregister
vérifié de l'adaptateur SSD, destruction du contrôleur, puis destruction du
Governor. Les tests utilisent un provider factice et n'exécutent aucune vraie
mutation SSD.

## Invariants

- ncurses appartient exclusivement au thread principal
- Aucune tâche ne démarre sans réservation active
- Les réservations sont libérées exactement une fois
- Les buffers sont strictement bornés
- Le Resource Governor reste l'unique propriétaire de l'admission ; ni Queue,
  ni contrôleur SSD ne constituent un second orchestrateur de ressources

## Statut : CURRENT / VALIDATED OPERATIONAL

La TUI/runtime et son raccordement SSD sont implémentés, testés et relus dans
leur tranche. Le statut global est
`GLOBAL_MAINTENANCE_AUDIT=PASS/FROZEN`. Les builds portables/Vulkan,
sanitizers, contrôles de concurrence et ABI frais sont acquis ; l'unique revue
finale indépendante a conclu PASS sans finding bloquant.
