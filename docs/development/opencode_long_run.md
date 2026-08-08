# OpenCode long run

`ocaway` lance OpenCode 1.18.15 pour un ticket autonome tout en conservant les
interdictions destructives du projet. Le mode normal `opencode` reste inchangé.

## Lancement

Le wrapper suivi par le dépôt est `.opencode/bin/opencode-away`. Il doit être
installé sous `~/.local/bin/opencode-away`, puis l'alias contenu dans
`.opencode/zshrc.snippet` doit être ajouté à `~/.zshrc`. Ces deux destinations
sont des symlinks vers les dotfiles utilisateur sur cette machine.

Depuis la racine d'un dépôt Git :

```sh
ocaway
```

Les arguments sont transmis tels quels à OpenCode. Le wrapper exporte
`LARDON_OPENCODE_LONG_RUN=1`, suspend uniquement les processus `swayidle` qui
étaient actifs, puis lance :

```sh
systemd-inhibit \
  --what=sleep:idle:handle-lid-switch \
  --who="OpenCode Long Run" \
  --why="Autonomous Lardon3D development ticket" \
  --mode=block \
  opencode --auto
```

systemd 261 supporte les trois inhibitions demandées. La configuration locale
utilise `HandleLidSwitch=suspend` et laisse `LidSwitchIgnoreInhibited` à sa
valeur par défaut `yes`, compatible avec l'inhibiteur `block`. Aucun fichier
systemd ou logind n'est modifié.

Le wrapper enregistre les PID `swayidle` qui n'étaient pas déjà stoppés, leur
envoie `SIGSTOP`, puis restaure exactement ces PID avec `SIGCONT` à la sortie,
sur INT, TERM ou HUP. Le code retour du processus OpenCode est conservé.

`opencode-away --check` vérifie les préconditions sans lancer OpenCode ni
suspendre `swayidle`. `OPENCODE_AWAY_COMMAND` est un seam de test réservé aux
validations du wrapper ; en usage normal il est absent.

## Permissions et sécurité

La configuration est une allowlist : les outils et commandes inconnus sont
refusés, et `--auto` ne peut pas contourner un refus. Restent interdits :
privilèges root, gestion de paquets, Git destructif ou publiant,
suppression, arrêt de processus ou machine, écriture externe, `.git/` et
`scan3d/`. Les écritures normales restent dans le worktree courant.

Les sous-agents approuvés sont autorisés explicitement par leur identifiant.
Chaque agent possède ses propres règles ; il n'hérite pas implicitement d'une
permission plus large du parent.

## Mémoire et handoff

Le ticket courant est résumé dans `.opencode/work/current_ticket.md` après
chaque phase. Avant une session fraîche, `.opencode/work/handoff.md` contient
l'architecture pertinente, les décisions, fichiers, tests, échecs, travail
restant et la prochaine action exacte.

Les sorties d'outils sont plafonnées et compactées. Les validations lourdes
sont séquentielles : ciblé, normal, ASan/UBSan, TSan, stress, revue et docs.

## Limites

Une inhibition empêche sleep, idle et la réaction logind au capot tant que le
wrapper et sa session utilisateur vivent. Elle ne garantit pas la survie du
processus si le terminal ou toute la session graphique est détruite. Aucun tmux
n'est créé automatiquement.

Si une dépendance externe manque, si un secret est requis ou si une opération
explicitement interdite devient nécessaire, le ticket s'arrête avec un handoff
au lieu de contourner la restriction.
