# Bambu Monitor : Guition JC4827W543

Affiche en temps réel (cloud Bambu Lab) l'avancement, le temps restant, la fin prévue
et les températures buse / plateau / caisson, avec des camemberts et des pourcentages.
L'écran s'éteint tout seul après la fin de l'impression et se rallume au démarrage
d'un nouveau travail ou au toucher.

## 1. Flasher

1. Installe VS Code + l'extension **PlatformIO** (ou `pip install platformio`).
2. Branche la carte en USB-C, ouvre ce dossier, clique sur **Upload** (ou `pio run -t upload`).

Si la carte n'est pas détectée : maintiens BOOT en branchant l'USB, puis relance l'upload.
Logs : `pio device monitor`.

## 2. Configurer (au premier démarrage, depuis ton téléphone)

Rien à éditer dans le code. L'écran affiche « Configuration » :

1. Connecte ton téléphone au Wi-Fi **BambuMonitor-Setup** (sans mot de passe).
2. La page s'ouvre toute seule (sinon : http://192.168.4.1).
3. Saisis ton Wi-Fi 2,4 GHz, l'email et le mot de passe de ton compte Bambu Lab.
4. Si Bambu envoie un code de vérification par email, saisis-le sur la page suivante.
5. L'écran redémarre et affiche ton imprimante.

L'identifiant du compte et l'imprimante (la première du compte) sont détectés automatiquement.

**Reconfigurer** : touche l'écran pendant les 2,5 s qui suivent l'allumage.
Si la session expire (environ 3 mois), l'écran se reconnecte seul ; si un code email
est demandé, la page de configuration s'ouvre à nouveau.

## Réglages (`src/config.h`)
- `SLEEP_AFTER_S` : délai avant extinction après la fin (120 s par défaut)
- `TZ_INFO` : fuseau horaire pour l'heure de fin prévue (Paris par défaut)

## Notes
- Le code n'a pas pu être compilé ni testé sur la carte dans mon environnement.
  Si une erreur apparaît (versions de bibliothèques, polices U8g2), envoie-moi le message.
- Bambu peut bloquer la connexion par email depuis un appareil (erreur HTTP 403, protection Cloudflare).
  Dans ce cas, la page de configuration a un champ « Avancé : token » pour coller un token
  (cookie `token` visible sur bambulab.com une fois connecté).
- Le mot de passe est conservé en clair dans la mémoire flash de la carte (non chiffrée).
- Connexion TLS sans vérification de certificat (`setInsecure`) pour simplifier.
- La double authentification par application n'est pas gérée (seul le code par email l'est).
