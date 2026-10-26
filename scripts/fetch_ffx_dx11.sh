#!/usr/bin/env bash
# M8.2 — récupère le backend DX11 de FidelityFX (FSR 3 upscaler, en-têtes © AMD, licence MIT) tel que
# fourni précompilé par Luma Framework (Filippo Tarpini et contributeurs, MIT), commit épinglé, empreintes
# vérifiées, dans third_party/ffx-dx11 (non versionné). Bibliothèques MSVC /MD : liées dans la DLL séparée
# ak-fsr-dx11.dll (CRT dynamique de Wine), jamais dans l'addon (CRT statique).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
base="https://raw.githubusercontent.com/Filoppi/Luma-Framework/2a8261307f/Source/External/FidelityFX"
dest="$root/third_party/ffx-dx11"
files=(
  "9af4d595128bd77e5de58213f20d47b7990ce806e6053085f304516aa9ba7228 libs/ffx_backend_dx11_x64.lib lib/ffx_backend_dx11_x64.lib"
  "7590ce91a0590aa7a16897dff0f25b1e154b889270c0bd2b45fd579390dda49a libs/ffx_fsr3upscaler_x64.lib lib/ffx_fsr3upscaler_x64.lib"
  "0582c35f37ffc873f6a2773ce4339fd8e99e34eb6c40d63c1820a65ad17a03cc FidelityFX/host/ffx_assert.h include/FidelityFX/host/ffx_assert.h"
  "8bbcb81e27eb4d83ff088985e9718665d0a3178ce10ada2c63db3265c324a580 FidelityFX/host/ffx_error.h include/FidelityFX/host/ffx_error.h"
  "5a0277261d3cf5e30726e3411c3b1a58b57e4f318d3abd721e25d158394c8552 FidelityFX/host/ffx_fsr3upscaler.h include/FidelityFX/host/ffx_fsr3upscaler.h"
  "85f797fa313ccacff5551129b2e187fe98382afa6f3a09935e04c4fc4eceb898 FidelityFX/host/ffx_fx.h include/FidelityFX/host/ffx_fx.h"
  "a53733e10b23d6132b076e7f1dd83c20c8d951d3c164acb4fac1a2f25ce423b6 FidelityFX/host/ffx_interface.h include/FidelityFX/host/ffx_interface.h"
  "3c4cd77c2932ba18dbf99731351f6a36d623e386a7d91259b41d873f723cb772 FidelityFX/host/ffx_types.h include/FidelityFX/host/ffx_types.h"
  "4f174191c58157119b86987f789d161a86ba3aedd2750bbca0b04502b25e6769 FidelityFX/host/ffx_util.h include/FidelityFX/host/ffx_util.h"
  "c347457ae82d40aff5234e4ddef7c881ee72c7d4289e9da0ee469a5b470d13ab FidelityFX/host/backends/dx11/ffx_dx11.h include/FidelityFX/host/backends/dx11/ffx_dx11.h"
)
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for entry in "${files[@]}"; do
  read -r sha src out <<<"$entry"
  mkdir -p "$tmp/$(dirname "$out")"
  curl -sSL -o "$tmp/$out" "$base/$src"
  echo "$sha  $tmp/$out" | sha256sum -c --quiet -
done
rm -rf "$dest"
mkdir -p "$dest"
cp -r "$tmp/lib" "$tmp/include" "$dest/"
echo "backend DX11 de FidelityFX installé dans $dest"
