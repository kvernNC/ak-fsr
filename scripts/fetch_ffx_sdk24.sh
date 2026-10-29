#!/usr/bin/env bash
# AMD FSR SDK v2.4.0 (28/10/2026, licence MIT, dépôt officiel, Kits/FidelityFX/signedbin) : DLL Vulkan signées
# (chargeur + upscaler) qui contiennent FSR 3.1.6 et FSR 4.1.2 (FP8, INT8, INT8 RDNA 2, Lite INT8) sans la DLL
# du pilote. Empreintes vérifiées, dans third_party/ffx-sdk24 (non versionné) ; publiées dans ak-fsr/sdk/.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
base="https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin"
dest="$root/third_party/ffx-sdk24"
files=(
  "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9 amd_fidelityfx_loader_vk.dll"
  "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7 amd_fidelityfx_upscaler_vk.dll"
)
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for entry in "${files[@]}"; do
  read -r sha name <<<"$entry"
  curl -sSL -o "$tmp/$name" "$base/$name"
  echo "$sha  $tmp/$name" | sha256sum -c --quiet -
done
rm -rf "$dest"
mkdir -p "$dest"
cp "$tmp"/*.dll "$dest/"
echo "DLL Vulkan du SDK FSR 2.4.0 installées dans $dest"
