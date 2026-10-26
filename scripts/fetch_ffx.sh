#!/usr/bin/env bash
# Récupère amd_fidelityfx_vk.dll (FidelityFX SDK v1.1.4, FSR 3.1, MIT) dans third_party/ffx-api/bin.
# Les en-têtes de l'API sont versionnés dans le dépôt ; la DLL (9 Mo, signée AMD) ne l'est pas.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
url="https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v1.1.4/FidelityFX-SDK-v1.1.4.zip"
sha256="a1624cc4238fef046f30c4d80ce3f47be63fc5f5373f49e3ee9edb9960f54c78"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -sSL -o "$tmp/ffx.zip" "$url"
unzip -qj "$tmp/ffx.zip" PrebuiltSignedDLL/amd_fidelityfx_vk.dll -d "$tmp"
echo "$sha256  $tmp/amd_fidelityfx_vk.dll" | sha256sum -c -
mkdir -p "$root/third_party/ffx-api/bin"
cp "$tmp/amd_fidelityfx_vk.dll" "$root/third_party/ffx-api/bin/"
