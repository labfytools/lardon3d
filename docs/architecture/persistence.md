# Persistance et base de données Lardon3D

## Vision

Lardon3D doit stocker les métadonnées de reconstruction dans une base de données persistante légère, probablement SQLite, tandis que les données numériques massives restent dans des fichiers/binaires adaptés.

## Principes fondamentaux

### Séparation logique/binaire
- État logique, relations, index → base persistante légère
- Données numériques massives → fichiers/artefacts binaires adaptés

### Cycle de publication
```
lot calculé
→ artefact temporaire
→ validation
→ publication atomique
→ transaction de métadonnées
→ état READY
```

### Règle de reprise
Une reprise ne considère jamais un artefact partiellement publié comme valide.

## Concepts de domaine

Les éléments suivants sont des concepts de domaine, PAS des tables SQL imposées :

- project
- scan_set
- image
- feature_set
- visual_signature
- candidate_pair
- verified_pair
- track
- observation
- camera
- pose
- point3d
- reconstruction_layer
- measurement
- document_source
- geometric_constraint
- artifact
- checkpoint

## Invariants

- Chaque publication est atomique
- Les artefacts partiels ne sont jamais considérés comme valides
- La reprise commence à la dernière frontière connue

## Checkpoint durable de tâche v1

### État durable

Le modèle durable versionné contient uniquement l'identifiant stable, le nom,
l'estimation immuable, l'état observé, l'état de reprise, la progression, le
message, les horodatages et le compteur de séquences. Il ne contient aucun gros
artefact numérique. Une future version pourra référencer des identifiants
d'artefacts publiés et validés sans incorporer leur contenu.

Les mutex, conditions, callbacks, userdata, workers, gouverneur, réservations et
contrats d'exécution sont transitoires et ne sont jamais sérialisés.

### Normalisation après arrêt de processus

| État observé | État restauré |
|---|---|
| `TASK_PENDING` | `TASK_PENDING` |
| `TASK_RUNNING` | `TASK_PENDING` |
| `TASK_PAUSED` | `TASK_PENDING` |
| `TASK_COMPLETED` | `TASK_COMPLETED` |
| `TASK_FAILED` | `TASK_FAILED` |
| `TASK_CANCELLED` | `TASK_CANCELLED` |

Une rupture de séquence n'est pas un état : elle est observée comme
`TASK_RUNNING`. Son `sequence_count` est durable, mais la reprise revient à
`TASK_PENDING` et exige une nouvelle admission.

### Stockage minimal

Le codec v1 est indépendant de la future Project Database. Le fichier est de
taille fixe et bornée, encodé champ par champ, avec magie, version, taille et
checksum de payload.
La publication écrit un fichier temporaire unique dans le même répertoire,
effectue `fsync`, renomme atomiquement puis synchronise le répertoire parent.
La lecture distingue absence, corruption, version inconnue et erreur d'I/O.

La sauvegarde distingue trois frontières :

- avant `rename`, toute erreur retourne `IO_ERROR`, supprime le temporaire et
  laisse l'ancien checkpoint publié inchangé ;
- après un `rename` réussi, le nouveau checkpoint est publié et visible et
  n'est jamais présenté comme rollbackable ;
- si le `fsync` du répertoire échoue après ce `rename`, le résultat est
  `PUBLISHED_NOT_DURABLE` : le fichier visible est valide, mais sa présence sous
  ce nom après un crash ou une coupure n'est pas garantie. `OK` garantit que le
  contenu et l'entrée de répertoire ont tous deux été synchronisés avec succès,
  sous réserve des garanties fournies par le système de fichiers et le stockage.

Les tailles persistantes sont refusées avant conversion lorsqu'elles dépassent
`SIZE_MAX`. Les secondes sont des entiers non signés v1 : les timestamps
négatifs ne sont pas sérialisables et une valeur lue doit être représentable
par le `time_t` local avant conversion. Le format reste donc lisible entre
plateformes uniquement pour les valeurs communes à leurs domaines `size_t` et
`time_t`.

## Project Database v2

SQLite contient l'état logique interrogable et les références aux fichiers ;
les checkpoints et artefacts volumineux restent externes. L'enregistrement du
résumé de tâche et de sa référence checkpoint est une transaction unique. Un
artefact est d'abord publié et vérifié comme fichier régulier, puis seulement
marqué `READY` en DB. Le chemin inverse est interdit.

### Protocole checkpoint projet

Le protocole réel n'est pas une transaction distribuée :

1. capture locale du snapshot sous le mutex de tâche puis déverrouillage ;
2. publication atomique du fichier sous
   `.lardon3d/checkpoints/<task_id>.chk` ;
3. transaction SQLite sur `tasks` et `checkpoints` avec chemin relatif.

Une erreur avant publication ne modifie pas la DB. `PUBLISHED_NOT_DURABLE` est
conservé comme tel en DB. Si la publication réussit puis que SQLite retourne
`BUSY` ou une erreur, le fichier valide reste sur disque, la DB conserve son
ancienne vérité et le nouveau fichier est un orphelin à réconcilier plus tard.
Il n'est pas supprimé et aucune atomicité FS+SQLite n'est revendiquée.

L'inventaire distingue checkpoint récupérable durable, récupérable mais publié
non durable, absent, invalide, version inconnue et erreur d'I/O. Aucune réparation
ou suppression silencieuse n'est effectuée.

Le format checkpoint reste en version 1 et ne contient pas de `task_kind`. Le
schéma SQLite v2 conserve `task_kind` et `task_kind_version` dans le résumé
logique interrogable. La migration v1→v2 laisse ces deux colonnes à `NULL` : une
tâche legacy reste inspectable mais ne peut pas être reconstruite ou resoumise.
Un kind inconnu ou une version non supportée est diagnostiqué sans exécuter de
code.

## Statut

**IMPLEMENTED** — modèle durable, codec v1, lecture validée, publication
atomique et restauration sûre d'une tâche isolée.

**IMPLEMENTED** — Project Database v2 pour identité, résumés de tâches typées,
références checkpoint et artefacts génériques.

**IMPLEMENTED** — registry statique bornée et reconstruction explicite avec
ownership du userdata.

**IMPLEMENTED** — API projet de sauvegarde fichier+DB et inventaire validé au
redémarrage.

**NOT_YET_WIRED** — autosave complet, premier type métier de production,
resoumission scheduler et réconciliation des fichiers orphelins.

**PLANNED** — catalogue d'artefacts photogrammétriques réels, migrations v3+ et
reprise globale.
