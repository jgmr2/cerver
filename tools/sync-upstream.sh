#!/bin/sh
# tools/sync-upstream.sh - muestra que cambio en el remoto "upstream"
# (el repo cerver original) desde la ultima vez que este fork lo trajo,
# para decidir si conviene un merge completo o cherry-pickear algo
# puntual. No aplica nada por su cuenta -- ver FORKING.md para el paso
# siguiente (merge/cherry-pick) segun lo que este comando muestre.
#
# Uso, parado en la raiz de un fork de cliente (con el remoto "upstream"
# ya agregado, ver FORKING.md):
#   ./tools/sync-upstream.sh [rama-upstream]   # default: main

set -e
UPSTREAM_BRANCH="${1:-main}"

if ! git remote get-url upstream >/dev/null 2>&1; then
    echo "No hay un remoto 'upstream' configurado en este repo." >&2
    echo "Ver FORKING.md, seccion 'Crear el fork de un cliente nuevo'." >&2
    exit 1
fi

echo "Trayendo cambios de upstream..."
git fetch upstream --tags --quiet

BASE=$(git merge-base HEAD "upstream/$UPSTREAM_BRANCH") || {
    echo "No se encontro un ancestro comun con upstream/$UPSTREAM_BRANCH -- revisa el nombre de rama." >&2
    exit 1
}
NEW_COMMITS=$(git rev-list "$BASE..upstream/$UPSTREAM_BRANCH")

if [ -z "$NEW_COMMITS" ]; then
    echo "Ya estas al dia con upstream/$UPSTREAM_BRANCH."
    exit 0
fi

echo ""
echo "== Commits nuevos en upstream/$UPSTREAM_BRANCH =="
git log --oneline "$BASE..upstream/$UPSTREAM_BRANCH"

echo ""
echo "== Archivos que tocan (ojo si caen en algo que ya personalizaste: =="
echo "== db/init/, src/controllers/, src/models/, src/utils/auth/auth_model.*, auth.c) =="
git diff --stat "$BASE..upstream/$UPSTREAM_BRANCH"

echo ""
echo "Tags disponibles en upstream: $(git tag -l | tr '\n' ' ')"
echo ""
echo "Para traer todo:        git merge upstream/$UPSTREAM_BRANCH"
echo "Para un checkpoint fijo: git merge upstream/<tag>"
echo "Para un fix puntual:     git cherry-pick <hash>"
