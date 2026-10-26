#!/usr/bin/env bash
# Copie l'addon et amd_fidelityfx_vk.dll dans le dossier du jeu (la compilation se fait à part).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
game="${AK_GAME_DIR:-$HOME/.local/share/Steam/steamapps/common/Batman Arkham Knight/Binaries/Win64}"
cp -v "$root/build/ak-fsr.addon64" "$root/build/amd_fidelityfx_vk.dll" "$game/"
