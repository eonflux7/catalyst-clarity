Catalyst Clarity - NVIDIA DLSS for Mirror's Edge Catalyst (Steam)
https://github.com/eonflux7/catalyst-clarity

INSTALL
  1. Updating from 0.2.1 or older? Delete version.dll from the game folder.
     The mod is now dinput8.dll.
  2. Extract everything into the game folder, next to MirrorsEdgeCatalyst.exe, e.g.
     C:\Program Files (x86)\Steam\steamapps\common\Mirrors Edge Catalyst
  3. Linux (Steam / Proton) only: set the game's launch options to
     WINEDLLOVERRIDES="dinput8=n,b" %command%
  4. Start the game. DLSS Quality is on by default. Press F8 for the settings menu.

REQUIREMENTS
  NVIDIA RTX GPU with a current driver, Windows 10/11. AMD / Intel GPUs: experimental, use
  OptiScaler (as dxgi.dll) for FSR 3.1 / XeSS. Tested on the Steam version of the game.
  Other versions (Origin / EA app) are untested: try at your own risk. If the game code differs,
  the mod stays inactive and catalyst_clarity.log says so.

SETTINGS
  Change them in the F8 menu (F3 opens a debug window). They are saved to catalyst_clarity.ini in the game folder.
  [general] mode          2 = DLSS, 0 = game TAA
  [dlss] quality_mode     0 = game's resolution slider, 1 = DLAA, 2 = Quality, 3 = Balanced,
                          4 = Performance, 5 = Ultra Performance
  [dlss] preset           0 = DLSS default, 10-13 = presets J-M
  [dlss] sharpness        0-100 (RCAS sharpening, 0 = off)

UNINSTALL
  Delete dinput8.dll, nvngx_dlss.dll, catalyst_clarity.ini, catalyst_clarity.log,
  catalyst_clarity_README.txt and the catalyst_clarity_licenses folder.

PROBLEMS
  catalyst_clarity.log in the game folder says what the mod did. Attach it to bug reports.

Uses the NVIDIA DLSS SDK, Dear ImGui, MinHook and AMD FidelityFX RCAS (licenses in
catalyst_clarity_licenses). NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation. Mirror's Edge is
a trademark of Electronic Arts Inc. Not affiliated with or endorsed by EA, DICE or NVIDIA.
