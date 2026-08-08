# ScanSets et catalogue d'images

## Statut

**IMPLEMENTED — ScanSet v1 et Image Catalog persistant v1.**

Un projet contient zéro ou plusieurs acquisitions logiques appelées
`ScanSet`. Un ScanSet vide est valide : il peut être créé avant la capture ou
l'import.

## Modèle durable

```text
Project
  └── ScanSet (acquisition logique)
        └── Image (observation logique)
              └── Image Asset (contenu physique géré)
```

- `scanset_id` identifie durablement une acquisition, indépendamment de son
  nom.
- `image_id` identifie durablement une observation dans un ScanSet,
  indépendamment du nom du fichier et de la tâche d'import.
- `asset_id` identifie un contenu physique. Son SHA-256, sa taille et son
  chemin relatif décrivent le fichier géré par Lardon3D.
- La provenance d'une image conserve son nom original, son chemin source, son
  instant d'import et, s'il existe, le `task_id` importeur.

Un contenu identique n'implique pas une identité logique unique. Dans un même
ScanSet, le couple `(scanset_id, asset_id)` est unique : réimporter le même
contenu retourne `ALREADY_PRESENT`. Dans deux ScanSets distincts, deux images
logiques possèdent deux `image_id`, mais peuvent partager le même `asset_id` et
le même fichier physique.

Un ScanSet peut documenter un objet complet, une sous-zone ou une pièce
démontée. Aucune pose ni transformation 3D entre ScanSets n'est inventée en v1.

Ces trois identifiants utilisent `AUTOINCREMENT`. Un ID issu d'une transaction
validée ne sera jamais réattribué, même après une future suppression physique.
Cette garantie est nécessaire avant que Feature Store, Visual Index, matches et
tracks ne commencent à les référencer.

## Stockage physique

Les assets image sont content-addressed :

```text
assets/images/<2 premiers hex>/<sha256 hex lowercase>
```

Le hash est un SHA-256 binaire de 32 octets dans SQLite et est encodé par le
programme pour construire le chemin. Le nom utilisateur ne participe jamais au
chemin de stockage. Le calcul et la copie utilisent un tampon fixe de 64 Kio ;
une image entière n'est jamais chargée en mémoire.

La publication crée un temporaire, le synchronise, publie sans écrasement puis
synchronise le répertoire. Un asset existant n'est adopté qu'après vérification
complète de sa taille et de son SHA-256. La transaction SQLite vient ensuite.
Un échec SQLite peut donc laisser un fichier orphelin, mais jamais une ligne
`READY` créée par le chemin métier avant publication.

## Import et reprise

`import.images` persiste désormais `source_path + scanset_id`. Le ScanSet est
immuable pour un `task_id`. L'import parcourt le dossier en streaming et traite
des lots bornés. Sa correction ne dépend pas de l'ordre de `readdir()` : la
présence logique est décidée par `(scanset_id, SHA-256)` dans la base.

Une source externe reste nécessaire tant que la tâche est récupérable. Après
`COMPLETED`, le catalogue et l'asset géré ne dépendent plus de sa présence.

Les anciennes tâches v3 sont rattachées par migration à un ScanSet explicite
nommé `Imports antérieurs à ScanSet v1`. Ce rattachement exprime seulement
l'absence historique de regroupement ; aucune provenance de capture n'est
inventée.

La migration ne transforme pas les lignes de `manifest.tsv` en images v4 : elle
n'a ni hash ni transaction catalogue historique permettant de le faire sans
rejouer les fichiers. Elle pose donc l'indicateur durable
`legacy_image_catalog_pending=1` lorsqu'une ancienne tâche d'import existe.

- Si une tâche v3 récupérable retrouve sa source, sa reprise relit les sources,
  publie les assets content-addressed et remplit le catalogue. Les anciennes
  copies sous `images/originals` ne sont ni écrasées ni supprimées.
- Si sa source a disparu, la reconstruction échoue proprement et les anciennes
  données restent uniquement legacy ; aucune provenance ni image v4 n'est
  inventée.
- Un import v3 déjà terminal n'est pas rejoué automatiquement. Ses images du
  manifeste restent accessibles par la projection legacy mais sont marquées
  conceptuellement **LEGACY DATA NOT YET CATALOGUED** via l'indicateur DB.

L'indicateur reste conservateur en v1 et n'est pas effacé automatiquement : une
future commande de migration/reconciliation devra vérifier l'intégralité des
données historiques avant de le lever.

## Accès borné

Les listes de ScanSets et d'images utilisent un curseur par ID et une limite de
1 à 256. Aucun `get_all_images()` persistant n'existe. Des milliers d'images ne
nécessitent donc pas autant de records simultanément en mémoire.

## Transition du manifeste

`images/manifest.tsv` reste pris en charge par l'ancien catalogue en mémoire et
les anciennes API d'import. Le chemin production maintient une projection
best-effort par hardlinks pour que la TUI existante continue d'afficher les
nouvelles images sans seconde copie physique. `project.db + assets/images`
reste toutefois la vérité canonique : le manifeste est legacy et diagnostique,
et n'est plus une condition de reprise. Deux images de ScanSets différents qui
partagent un nom ne peuvent pas toutes deux être représentées dans cette vue
legacy. La migration de la TUI vers les pages SQLite reste donc nécessaire.

## Futures étapes

**NOT_YET_WIRED** — sélection de ScanSet dans la TUI, migration de l'ancienne
vue mémoire, vérification/scrub des assets et réconciliation globale des
orphelins.

**PLANNED** — Feature Store, Visual Index, paires candidates, matching, tracks,
SfM, MVS et relations géométriques entre ScanSets.
