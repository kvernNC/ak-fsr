#!/usr/bin/env bash
# Compile shaders/*.comp en SPIR-V et génère src/shaders/<nom>.spv.h (tableau uint32_t).
# Les en-têtes générés sont versionnés : glslang n'est nécessaire que pour modifier un shader.
# GLSLANG : chemin de glslang (défaut : glslang dans le PATH).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
glslang="${GLSLANG:-glslang}"
mkdir -p "$root/src/shaders"
for src in "$root"/shaders/*.comp; do
  name="$(basename "$src" .comp)"
  ident="$(echo "$name" | tr -c 'a-zA-Z0-9\n' '_')"
  "$glslang" -V --target-env vulkan1.1 -Os -o "$root/src/shaders/$name.spv" "$src"
  python3 -I - "$root/src/shaders/$name.spv" "$root/src/shaders/$name.spv.h" "$ident" <<'EOF'
import struct, sys
data = open(sys.argv[1], 'rb').read()
words = struct.unpack('<%dI' % (len(data) // 4), data)
lines = [', '.join('0x%08x' % w for w in words[i:i + 8]) for i in range(0, len(words), 8)]
with open(sys.argv[2], 'w') as f:
    f.write('// Généré par scripts/build_shaders.sh — ne pas modifier.\n#pragma once\n#include <cstdint>\n\n')
    f.write('inline constexpr uint32_t k_%s_spv[] = {\n  %s\n};\n' % (sys.argv[3], ',\n  '.join(lines)))
EOF
  rm "$root/src/shaders/$name.spv"
done
