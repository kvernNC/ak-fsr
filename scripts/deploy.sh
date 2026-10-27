#!/usr/bin/env bash
# Copie l'addon et ses DLL (amd_fidelityfx_vk.dll ; ak-fsr-dx11.dll si compilée) dans le dossier du jeu, et
# prépare ak-fsr/ pour M10.2 : DLL signées du SDK (scripts/fetch_ffx_signed.sh) renommées en _vk, et
# amdvkffx64.dll (pilote AMD, fournie par l'utilisateur) déplacée depuis le dossier du jeu si elle s'y trouve.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
game="${AK_GAME_DIR:-$HOME/.local/share/Steam/steamapps/common/Batman Arkham Knight/Binaries/Win64}"
# Copie puis renommage : sans risque si le jeu tourne (le fichier chargé n'est pas réécrit sur place).
install_file() { cp "$1" "$2.new" && mv -f "$2.new" "$2" && echo "'$1' -> '$2'"; }
install_file "$root/build/ak-fsr.addon64" "$game/ak-fsr.addon64"
install_file "$root/build/amd_fidelityfx_vk.dll" "$game/amd_fidelityfx_vk.dll"
if [[ -f "$root/build/ak-fsr-dx11.dll" ]]; then install_file "$root/build/ak-fsr-dx11.dll" "$game/ak-fsr-dx11.dll"; fi
signed="$root/third_party/ffx-signed"
if [[ -f "$signed/amd_fidelityfx_loader_dx12.dll" ]]; then
  mkdir -p "$game/ak-fsr"
  install_file "$signed/amd_fidelityfx_loader_dx12.dll" "$game/ak-fsr/amd_fidelityfx_loader_vk.dll"
  install_file "$signed/amd_fidelityfx_upscaler_dx12.dll" "$game/ak-fsr/amd_fidelityfx_upscaler_vk.dll"
fi
if [[ -f "$game/amdvkffx64.dll" && ! -f "$game/ak-fsr/amdvkffx64.dll" ]]; then
  mkdir -p "$game/ak-fsr"
  install_file "$game/amdvkffx64.dll" "$game/ak-fsr/amdvkffx64.dll"
fi
