#pragma once

/** The first module-local RCDATA identifier owns the bundled default settings document. */
#define IDR_DEFAULT_SETTINGS 101
/** The next module-local RCDATA identifier embeds the required Dear ImGui MIT notice. */
#define IDR_IMGUI_LICENSE 102
/** The next module-local RCDATA identifier embeds the required Microsoft Detours notice. */
#define IDR_DETOURS_LICENSE 103
/** The next module-local RCDATA identifier holds the animated logo sprite sheet, as a PNG. */
#define IDR_LOGO_SHEET 104

/** The four numeric fields of the version resource, in FILEVERSION order. */
#define SUNRISE_VER_MAJOR 0
#define SUNRISE_VER_MINOR 3
#define SUNRISE_VER_PATCH 2
#define SUNRISE_VER_BUILD 0
/** The same version as display text. Windows shows this string, not the four fields. */
#define SUNRISE_VER_STRING "0.3.2.0"

/** Compact UTF-8 hash-to-display-name table used by the in-game item editor. */
#define IDR_ITEM_NAMES 105

/** Packed 96x96 item/plug artwork indexed by definition hash. */
#define IDR_ITEM_ICONS 106

/** Compact weapon-rarity lookup used by catalog filters. */
#define IDR_ITEM_RARITY 107
