#!/usr/bin/env bash
# Archive de publication (Nexus) : version publiée (AKFSR_DEV_TOOLS=OFF), DLL redistribuables seulement.
# amdvkffx64.dll (pilote AMD, propriétaire) n'est jamais incluse : le script échoue s'il en trouve une.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${AK_RELEASE_BUILD:-$root/build-main}"
version="$(sed -n 's/#define AKFSR_VERSION_\(MAJOR\|MINOR\|PATCH\) //p' "$root/src/version.h" | paste -sd.)"
out="$root/dist/ak-fsr4-linux-amd-$version"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/cmake/clang-cl-xwin.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DAKFSR_DEV_TOOLS=OFF >/dev/null
cmake --build "$build" >/dev/null
rm -rf "$out" && mkdir -p "$out/Binaries/Win64/ak-fsr4-linux-amd"
cp "$build/ak-fsr4-linux-amd.addon64" "$out/Binaries/Win64/"
# FSR 3.1 DX11 natif (Windows sans DXVK) : pont ak-fsr-dx11.dll et backend DX11 de FidelityFX (portage gameplug-labs,
# MIT), à côté de l'addon. Exigés : la publication doit proposer les deux backends.
for dll in ak-fsr-dx11 ffx_backend_dx11_x64 ffx_fsr3upscaler_x64; do
  if [[ ! -f "$build/$dll.dll" ]]; then
    echo "ERREUR : $dll.dll absente de $build (scripts/fetch_ffx_dx11.sh et scripts/fetch_ffx_dx11_gameplug.sh)" >&2
    exit 1
  fi
  cp "$build/$dll.dll" "$out/Binaries/Win64/"
done
cp "$root/third_party/ffx-api/LICENSE.md" "$out/LICENSE-AMD-FidelityFX.md"
signed="$root/third_party/ffx-signed"
cp "$signed/amd_fidelityfx_loader_dx12.dll" "$out/Binaries/Win64/ak-fsr4-linux-amd/amd_fidelityfx_loader_vk.dll"
cp "$signed/amd_fidelityfx_upscaler_dx12.dll" "$out/Binaries/Win64/ak-fsr4-linux-amd/amd_fidelityfx_upscaler_vk.dll"
cp "$root/NEXUS.md" "$out/README-AK-FSR4-Linux-AMD.md"
cp "$root/CHANGELOG.md" "$out/CHANGELOG-AK-FSR4-Linux-AMD.md"
cp "$root/LICENSE" "$out/LICENSE-AK-FSR4-Linux-AMD.txt"
if find "$out" -iname 'amdvkffx64.dll' -o -iname 'amdxcffx64.dll' | grep -q .; then
  echo "ERREUR : DLL propriétaire AMD dans l'archive" >&2
  exit 1
fi
cd "$root/dist"
rm -f "ak-fsr4-linux-amd-$version.zip"
python3 -m zipfile -c "ak-fsr4-linux-amd-$version.zip" "ak-fsr4-linux-amd-$version"
echo "archive : $root/dist/ak-fsr4-linux-amd-$version.zip"
