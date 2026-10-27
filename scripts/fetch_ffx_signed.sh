#!/usr/bin/env bash
# M10.2 — DLL signées par AMD du FidelityFX SDK v2.3.0 (licence MIT, dépôt officiel, Kits/FidelityFX/signedbin),
# empreintes vérifiées, dans third_party/ffx-signed (non versionné). Le pilote ne donne son provider FSR 4
# qu'en leur présence (PLAN.md §8.1) ; scripts/deploy.sh les copie, renommées en _vk, dans ak-fsr/.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
base="https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.3.0/Kits/FidelityFX/signedbin"
dest="$root/third_party/ffx-signed"
files=(
  "e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa amd_fidelityfx_loader_dx12.dll"
  "d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46 amd_fidelityfx_upscaler_dx12.dll"
)
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for entry in "${files[@]}"; do
  read -r sha name <<<"$entry"
  curl -sSL -o "$tmp/$name" "$base/$name"
  echo "$sha  $tmp/$name" | sha256sum -c --quiet -
done
mkdir -p "$dest"
cp "$tmp"/*.dll "$dest/"
echo "DLL signées du SDK FidelityFX 2.3 installées dans $dest"
