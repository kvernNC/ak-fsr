#pragma once

// M10.2 — fonctions Vulkan du provider FSR 4 du pilote activées sur le device de DXVK : détour de
// vkCreateDevice (winevulkan) installé au chargement de l'addon, avant que DXVK ne crée son device
// (ReShade charge les addons dans D3D11CreateDevice, avant l'appel réel). Seules les extensions et
// fonctions prises en charge par le GPU sont ajoutées ; rien n'est retiré de la demande de DXVK.

#include <string>
#include <vector>

namespace akfsr::vk_device_hook {

bool install();  // à appeler depuis DllMain, si l'option Fsr4Driver est active

// Diagnostic sans effet (toujours au chargement) : prologue de vkCreateDevice et décision du décodeur.
void log_prologue();

struct status {
  bool installed = false;
  bool device_seen = false;
  std::string message;
  std::vector<std::string> added;    // extensions ajoutées à la demande de DXVK
  std::vector<std::string> missing;  // extensions voulues absentes du GPU
};
status get_status();

}  // namespace akfsr::vk_device_hook
