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
signed="$root/third_party/ffx-signed"
cp "$signed/amd_fidelityfx_loader_dx12.dll" "$out/Binaries/Win64/ak-fsr4-linux-amd/amd_fidelityfx_loader_vk.dll"
cp "$signed/amd_fidelityfx_upscaler_dx12.dll" "$out/Binaries/Win64/ak-fsr4-linux-amd/amd_fidelityfx_upscaler_vk.dll"
cp "$root/NEXUS.md" "$out/README-AK-FSR4-Linux-AMD.md"
cp "$root/LICENSE" "$out/LICENSE-AK-FSR4-Linux-AMD.txt"
if find "$out" -iname 'amdvkffx64.dll' -o -iname 'amdxcffx64.dll' | grep -q .; then
  echo "ERREUR : DLL propriétaire AMD dans l'archive" >&2
  exit 1
fi
cd "$root/dist"
rm -f "ak-fsr4-linux-amd-$version.zip"
python3 -m zipfile -c "ak-fsr4-linux-amd-$version.zip" "ak-fsr4-linux-amd-$version"
echo "archive : $root/dist/ak-fsr4-linux-amd-$version.zip"
