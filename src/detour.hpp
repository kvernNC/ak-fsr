#pragma once

// Détour x64 générique pour une fonction exportée (M10.2 : vkCreateDevice de winevulkan) : décodage
// des premières instructions (prologues courants seulement), déplacées dans un trampoline. Refuse tout
// adressage relatif à rip ou branchement dans les octets déplacés.

#include <string>

namespace akfsr::detour {

// `original` reçoit le trampoline (appel de la fonction d'origine). false + `error` si refusé.
bool install(void *target, const void *replacement, void **original, std::string &error);

}  // namespace akfsr::detour
