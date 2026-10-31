#pragma once

// M13 §1 — mesures de performance (Steam Deck) : temps GPU par passe (timestamps D3D11 et Vulkan dans la même
// base de temps sous DXVK, d'où les bulles entre les deux), attentes CPU autour de l'interop, événements rares
// (contexte FFX, ressources, pipelines), moyennes glissantes sur 60 frames (ligne à l'écran et log) et journal
// par frame `ak-fsr-frames.csv`.
// Réglages dans ReShade.ini, section [Debug] : FrameLog=on (CSV), PerfOverlay=on (ligne à l'écran),
// PerfLog=on (moyennes dans ReShade.log, actif par défaut dès que l'un des deux autres l'est).

#include <d3d11.h>

#include <cstdint>
#include <filesystem>

namespace reshade::api {
struct effect_runtime;
}

namespace akfsr::perf {

void init(const std::filesystem::path &addon_dir);  // lit [Debug], ouvre le CSV, enregistre l'affichage
void shutdown();
void release_device_objects();  // destruction du device D3D11 : requêtes libérées
bool enabled();   // mesure demandée : timestamps D3D11 posés, lignes produites
uint64_t frame(); // numéro de la frame en cours (incrémenté à chaque present)
void on_present();  // fin de frame : temps CPU de la frame, moyennes, CSV

// Attentes CPU (QueryPerformanceCounter), cumulées sur la frame.
enum class wait : int {
  flush,   // FlushRenderingCommands (synchronisation du thread CS de DXVK)
  lock,    // LockSubmissionQueue : attente du verrou de la file
  held,    // verrou tenu (vkQueueSubmit compris)
  fence,   // vkWaitForFences avant réutilisation d'un command buffer (anneau de 3)
  idle,    // vkDeviceWaitIdle (recréations)
  driver,  // attentes demandées par la DLL AMD (vkWaitForFences / vkQueueWaitIdle / vkDeviceWaitIdle)
  count
};
void add_wait(wait w, double ms);
void add_cpu_run(double ms);       // temps CPU total d'AK-FSR (fsr::run) dans la frame
void note_insertion(bool smaa);    // point d'insertion utilisé : SMAA du jeu ou 0xB2A276FD

// Points D3D11 (contexte immédiat) : début du bloc AK-FSR, copies d'entrée, copie de sortie, fin du bloc.
enum class mark : int { block_start, copy_in_start, copy_in_end, copy_out_start, copy_out_end, block_end, count };
void d3d_mark(ID3D11DeviceContext *ctx, mark m);

// Timestamps de notre command buffer Vulkan (ns absolus, même base que les timestamps D3D11 de DXVK) :
// 0 début (après la barrière d'entrée), 1 fin des vecteurs de mouvement, 2 fin de FSR, 3 fin du command buffer.
void set_vk_stamps(uint64_t frame, const double ns[4], const bool valid[4]);

// Créations comptées par frame (pipelines, modules, allocations) avec leur durée CPU.
enum class creation : int { amd_shader, amd_pipeline, amd_alloc, amd_submit, akfsr_pipeline, akfsr_d3d_shader, count };
void add_creation(creation c, double ms);

// Événement rare : horodaté dans le log et dans la colonne `events` du CSV.
void event(const char *fmt, ...);

// Chronomètre CPU (ms, QueryPerformanceCounter).
double now_ms();

}  // namespace akfsr::perf
