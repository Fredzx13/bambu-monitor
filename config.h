#pragma once
// ============================================================
//  Réglages optionnels. Wi-Fi et compte Bambu se saisissent
//  directement depuis ton téléphone au premier démarrage.
// ============================================================

// Nom du Wi-Fi temporaire créé par l'écran pour la configuration
#define SETUP_AP_NAME    "BambuMonitor-Setup"

// Délai (secondes) après la fin d'impression avant d'éteindre l'écran.
// L'écran se rallume tout seul au démarrage d'une impression, ou au toucher.
#define SLEEP_AFTER_S    120

// Heure locale (pour l'heure de fin prévue) : Paris / Marseille
#define TZ_INFO          "CET-1CEST,M3.5.0,M10.5.0/3"
