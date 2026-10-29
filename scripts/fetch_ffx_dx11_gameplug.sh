#!/usr/bin/env bash
# Backend DX11 de FidelityFX (FSR 3.1.2) corrigé par gameplug-labs (fork MIT de FidelityFX-SDK-DX11, « fixing
# certain rendering issues ») : DLL précompilées de la release fsr3-v3.1.2, empreinte vérifiée, dans
# third_party/ffx-dx11-gameplug (non versionné), avec des bibliothèques d'import générées depuis leurs exports.
# En-têtes : ceux de third_party/ffx-dx11 (scripts/fetch_ffx_dx11.sh), identiques à un const près.
# Le portage de Luma laisse des traînées sur la végétation en mouvement (constaté le 29/10/2026).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
url="https://github.com/gameplug-labs/FidelityFX-SDK/releases/download/fsr3-v3.1.2/FSR3-v3.1.2.zip"
sha="e2e22fe0b0f67aec5f1f699f80259c7d6b294aafacef1900b48656370d38816d"
dest="$root/third_party/ffx-dx11-gameplug"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -sSL -o "$tmp/fsr.zip" "$url"
echo "$sha  $tmp/fsr.zip" | sha256sum -c --quiet -
python3 -I -m zipfile -e "$tmp/fsr.zip" "$tmp/x"
rm -rf "$dest"
mkdir -p "$dest/bin" "$dest/lib"
for name in ffx_backend_dx11_x64 ffx_fsr3upscaler_x64; do
  cp "$tmp/x/FSR3-v3.1.2/$name.dll" "$dest/bin/"
  { echo "LIBRARY $name.dll"; echo "EXPORTS";
    llvm-objdump -p "$dest/bin/$name.dll" | sed -n '/Export Table/,/^$/p' | awk '/^ +[0-9]+ +0x/ {print "  " $NF}'; } > "$tmp/$name.def"
  llvm-dlltool -m i386:x86-64 -d "$tmp/$name.def" -l "$dest/lib/$name.lib" -D "$name.dll"
done
echo "backend DX11 gameplug-labs installé dans $dest"
