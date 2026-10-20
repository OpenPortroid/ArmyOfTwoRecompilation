// ui_text.cpp - The port's own UI text (debug menu, frame rate overlay,
// invites) in the game's language.
//
// The game runs in the language game_language.cpp sets (ao2_game_language:
// the console's language, or Russian text on the English game). The text is
// keyed by its English: Tr("Invite") is the row of "Invite" in the current
// language. The fonts cover Latin-1 and Cyrillic, so no characters beyond
// those.
#include "ui_text.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game_language.h"

REXCVAR_DEFINE_STRING(ao2_ui_language, "auto", "General",
                      "Language of the port's own menus (debug menu, invites, frame rate overlay): auto (the "
                      "game's language), en, ru, de, fr, es, it.");

namespace
{
  using ao2::text::Lang;

  struct Row
  {
    const char *en, *ru, *de, *fr, *es, *it;
  };

  // clang-format off
  const Row kRows[] = {
      // --- Achievements: the unlock toast ---
      {"ACHIEVEMENT UNLOCKED", "ДОСТИЖЕНИЕ ПОЛУЧЕНО", "ERFOLG FREIGESCHALTET", "SUCCÈS DÉBLOQUÉ", "LOGRO DESBLOQUEADO",
       "OBIETTIVO SBLOCCATO"},
      // --- Invites: notices ---
      {"INCOMING INVITE", "ВХОДЯЩЕЕ ПРИГЛАШЕНИЕ", "EINLADUNG", "INVITATION REÇUE", "INVITATION RECIBIDA", "INVITO RICEVUTO"},
      {"invites you to a private co-op game", "зовёт вас в приватную кооперативную игру", "lädt dich zu einem privaten Koop-Spiel ein",
       "vous invite à une partie coop privée", "te invita a una partida cooperativa privada", "ti invita a una partita cooperativa privata"},
      {"invites you to a co-op game", "зовёт вас в кооперативную игру", "lädt dich zu einem Koop-Spiel ein",
       "vous invite à une partie coop", "te invita a una partida cooperativa", "ti invita a una partita cooperativa"},
      {"ACCEPT", "ПРИНЯТЬ", "ANNEHMEN", "ACCEPTER", "ACEPTAR", "ACCETTA"},
      {"LIST / DECLINE", "СПИСОК / ОТКЛОНИТЬ", "LISTE / ABLEHNEN", "LISTE / REFUSER", "LISTA / RECHAZAR", "ELENCO / RIFIUTA"},
      {"ACCEPTED THE INVITE  ·  JOINING", "ПРИНЯЛ ПРИГЛАШЕНИЕ  ·  ПОДКЛЮЧАЕТСЯ", "HAT ANGENOMMEN  ·  VERBINDET",
       "A ACCEPTÉ  ·  CONNEXION", "HA ACEPTADO  ·  CONECTANDO", "HA ACCETTATO  ·  IN CONNESSIONE"},
      {"DECLINED THE INVITE", "ОТКЛОНИЛ ПРИГЛАШЕНИЕ", "HAT ABGELEHNT", "A REFUSÉ", "HA RECHAZADO", "HA RIFIUTATO"},

      // --- Invites: the players window ---
      {"in game", "в игре", "im Spiel", "en jeu", "en partida", "in partita"},
      {"in lobby", "в лобби", "in der Lobby", "dans le salon", "en la sala", "nella lobby"},
      {"in menu", "в меню", "im Menü", "dans le menu", "en el menú", "nel menu"},
      {"CO-OP  //  INVITES", "КООПЕРАТИВ  //  ПРИГЛАШЕНИЯ", "KOOP  //  EINLADUNGEN", "COOP  //  INVITATIONS",
       "COOPERATIVO  //  INVITACIONES", "COOPERATIVA  //  INVITI"},
      {"CO-OP  //  PRIVATE GAME", "КООПЕРАТИВ  //  ПРИВАТНАЯ ИГРА", "KOOP  //  PRIVATES SPIEL", "COOP  //  PARTIE PRIVÉE",
       "COOPERATIVO  //  PARTIDA PRIVADA", "COOPERATIVA  //  PARTITA PRIVATA"},
      {"CO-OP  //  PUBLIC GAME", "КООПЕРАТИВ  //  ОТКРЫТАЯ ИГРА", "KOOP  //  ÖFFENTLICHES SPIEL", "COOP  //  PARTIE PUBLIQUE",
       "COOPERATIVO  //  PARTIDA PÚBLICA", "COOPERATIVA  //  PARTITA PUBBLICA"},
      {"INVITE A PARTNER", "ПРИГЛАСИТЬ НАПАРНИКА", "PARTNER EINLADEN", "INVITER UN PARTENAIRE", "INVITAR A UN COMPAÑERO",
       "INVITA UN COMPAGNO"},
      {"CALLED TO ARMS", "ВАС ЗОВУТ В БОЙ", "DER EINSATZ RUFT", "APPEL AU COMBAT", "TE LLAMAN AL COMBATE", "CHIAMATA ALLE ARMI"},
      {"PLAYERS ONLINE", "ИГРОКИ В СЕТИ", "SPIELER IM NETZ", "JOUEURS EN LIGNE", "JUGADORES EN RED", "GIOCATORI IN RETE"},
      {"GAME CREATED  ·  %d/2", "ИГРА СОЗДАНА  ·  %d/2", "SPIEL ERSTELLT  ·  %d/2", "PARTIE CRÉÉE  ·  %d/2",
       "PARTIDA CREADA  ·  %d/2", "PARTITA CREATA  ·  %d/2"},
      {"NO GAME OF YOURS", "НЕТ СВОЕЙ ИГРЫ", "KEIN EIGENES SPIEL", "AUCUNE PARTIE", "SIN PARTIDA PROPIA", "NESSUNA PARTITA"},
      {"WAITING", "ОЖИДАНИЕ", "WARTEN", "EN ATTENTE", "ESPERANDO", "IN ATTESA"},
      {"PLAYER 1  ·  HOST", "ИГРОК 1  ·  ХОСТ", "SPIELER 1  ·  HOST", "JOUEUR 1  ·  HÔTE", "JUGADOR 1  ·  ANFITRIÓN",
       "GIOCATORE 1  ·  HOST"},
      {"PLAYER 2  ·  PARTNER", "ИГРОК 2  ·  НАПАРНИК", "SPIELER 2  ·  PARTNER", "JOUEUR 2  ·  PARTENAIRE",
       "JUGADOR 2  ·  COMPAÑERO", "GIOCATORE 2  ·  COMPAGNO"},
      {"PARTNER", "НАПАРНИК", "PARTNER", "PARTENAIRE", "COMPAÑERO", "COMPAGNO"},
      {"PLAYER 2  ·  JOINING", "ИГРОК 2  ·  ПОДКЛЮЧАЕТСЯ", "SPIELER 2  ·  VERBINDET", "JOUEUR 2  ·  CONNEXION",
       "JUGADOR 2  ·  CONECTANDO", "GIOCATORE 2  ·  IN CONNESSIONE"},
      {"PLAYER 2", "ИГРОК 2", "SPIELER 2", "JOUEUR 2", "JUGADOR 2", "GIOCATORE 2"},
      {"INCOMING INVITES", "ВХОДЯЩИЕ ПРИГЛАШЕНИЯ", "EINLADUNGEN", "INVITATIONS REÇUES", "INVITACIONES RECIBIDAS",
       "INVITI RICEVUTI"},
      {"DECLINE", "ОТКЛОНИТЬ", "ABLEHNEN", "REFUSER", "RECHAZAR", "RIFIUTA"},
      {"SEARCHING FOR PLAYERS", "ПОИСК ИГРОКОВ", "SUCHE SPIELER", "RECHERCHE DE JOUEURS", "BUSCANDO JUGADORES",
       "RICERCA GIOCATORI"},
      {"Players on the LAN and Radmin VPN are found automatically.", "В локальной сети и Radmin VPN игроки находятся сами.",
       "Spieler im LAN und in Radmin VPN werden automatisch gefunden.",
       "Les joueurs du réseau local et de Radmin VPN sont trouvés automatiquement.",
       "Los jugadores en LAN y Radmin VPN se encuentran solos.", "I giocatori in LAN e Radmin VPN vengono trovati da soli."},
      {"Over the internet: the address below or ao2_friends in settings/online.toml.",
       "Через интернет — адрес ниже или ao2_friends в settings/online.toml.",
       "Über das Internet: Adresse unten oder ao2_friends in settings/online.toml.",
       "Par Internet : l'adresse ci-dessous ou ao2_friends dans settings/online.toml.",
       "Por internet: la dirección de abajo o ao2_friends en settings/online.toml.",
       "Via internet: l'indirizzo qui sotto o ao2_friends in settings/online.toml."},
      {"  ·  hosting a game", "  ·  ведёт игру", "  ·  hostet ein Spiel", "  ·  héberge une partie", "  ·  aloja una partida",
       "  ·  ospita una partita"},
      {"SENDING", "ОТПРАВКА", "SENDEN", "ENVOI", "ENVIANDO", "INVIO"},
      {"AWAITING REPLY", "ЖДЁМ ОТВЕТА", "WARTE AUF ANTWORT", "EN ATTENTE", "ESPERANDO RESPUESTA", "IN ATTESA DI RISPOSTA"},
      {"ACCEPTED", "ПРИНЯЛ", "ANGENOMMEN", "ACCEPTÉ", "ACEPTADO", "ACCETTATO"},
      {"DECLINED", "ОТКЛОНИЛ", "ABGELEHNT", "REFUSÉ", "RECHAZADO", "RIFIUTATO"},
      {"NOT DELIVERED", "НЕ ДОШЛО", "NICHT ZUGESTELLT", "NON REÇU", "NO ENTREGADO", "NON RECAPITATO"},
      {"NOT JOINED", "НЕ ПРИСОЕДИНИЛСЯ", "NICHT BEIGETRETEN", "N'A PAS REJOINT", "NO SE UNIÓ", "NON UNITO"},
      {"AGAIN", "ЕЩЁ РАЗ", "ERNEUT", "RÉESSAYER", "OTRA VEZ", "DI NUOVO"},
      {"INVITE", "ПРИГЛАСИТЬ", "EINLADEN", "INVITER", "INVITAR", "INVITA"},
      {"To invite, create a game: Campaign Mode → Private Co-op Game.",
       "Чтобы пригласить, создайте игру: Кампания → Закрытая игра.",
       "Zum Einladen ein Spiel erstellen: Campaign Mode → Private Co-op Game.",
       "Pour inviter, créez une partie : Campaign Mode → Private Co-op Game.",
       "Para invitar, crea una partida: Campaign Mode → Private Co-op Game.",
       "Per invitare, crea una partita: Campaign Mode → Private Co-op Game."},
      {"BY ADDRESS", "ПО АДРЕСУ", "PER ADRESSE", "PAR ADRESSE", "POR DIRECCIÓN", "PER INDIRIZZO"},
      {"IP or IP:port", "IP или IP:порт", "IP oder IP:Port", "IP ou IP:port", "IP o IP:puerto", "IP o IP:porta"},
      {"FIND", "НАЙТИ", "SUCHEN", "TROUVER", "BUSCAR", "CERCA"},
      {"Couldn't read the address.", "Не понял адрес.", "Adresse nicht erkannt.", "Adresse non reconnue.",
       "Dirección no válida.", "Indirizzo non riconosciuto."},
      {"UDP %u must be open on both PCs (firewall)", "UDP %u должен быть открыт у обоих (брандмауэр)",
       "UDP %u muss bei beiden offen sein (Firewall)", "UDP %u doit être ouvert des deux côtés (pare-feu)",
       "UDP %u debe estar abierto en ambos (cortafuegos)", "UDP %u deve essere aperto su entrambi (firewall)"},
      {"CLOSE", "ЗАКРЫТЬ", "SCHLIESSEN", "FERMER", "CERRAR", "CHIUDI"},
      {"Create a game first: Campaign Mode - Private (or Public) Co-op Game.",
       "Сначала создайте игру: Кампания - Закрытая (или Открытая) игра.",
       "Erst ein Spiel erstellen: Campaign Mode - Private (oder Public) Co-op Game.",
       "Créez d'abord une partie : Campaign Mode - Private (ou Public) Co-op Game.",
       "Primero crea una partida: Campaign Mode - Private (o Public) Co-op Game.",
       "Prima crea una partita: Campaign Mode - Private (o Public) Co-op Game."},

      // --- Debug menu ---
      {"Debug menu", "Дебаг-меню", "Debug-Menü", "Menu de débogage", "Menú de depuración", "Menu di debug"},
      {"Online co-op: actions go to both players.", "Онлайн-кооператив: действия идут обоим игрокам.",
       "Online-Koop: Aktionen gelten für beide Spieler.", "Coop en ligne : les actions s'appliquent aux deux joueurs.",
       "Cooperativo en línea: las acciones van a ambos jugadores.", "Cooperativa online: le azioni valgono per entrambi i giocatori."},
      {"Online co-op: available while a level is running.", "Онлайн-кооператив: доступно, когда идёт уровень.",
       "Online-Koop: verfügbar, während ein Level läuft.", "Coop en ligne : disponible pendant un niveau.",
       "Cooperativo en línea: disponible durante un nivel.", "Cooperativa online: disponibile durante un livello."},
      {"God mode (my character)", "Неуязвимость (моего персонажа)", "Unverwundbar (meine Figur)",
       "Invincibilité (mon personnage)", "Invulnerable (mi personaje)", "Invulnerabilità (il mio personaggio)"},
      {"God mode", "Неуязвимость", "Unverwundbar", "Invincibilité", "Invulnerable", "Invulnerabilità"},
      {"FPS counter", "Счётчик FPS", "FPS-Anzeige", "Compteur FPS", "Contador de FPS", "Contatore FPS"},
      {"Off", "Выкл", "Aus", "Désactivé", "Apagado", "Spento"},
      {"Detailed", "Подробно", "Detailliert", "Détaillé", "Detallado", "Dettagliato"},
      {"Native renderer only (ao2_native_gpu).", "Только с нативным рендерером (ao2_native_gpu).",
       "Nur mit nativem Renderer (ao2_native_gpu).", "Uniquement avec le rendu natif (ao2_native_gpu).",
       "Solo con el renderizador nativo (ao2_native_gpu).", "Solo con il renderer nativo (ao2_native_gpu)."},
      {"%s toggles it in game too. Detailed: frame time,", "%s переключает и в игре. Подробно: время кадра,",
       "%s schaltet auch im Spiel um. Detailliert: Framezeit,", "%s bascule aussi en jeu. Détaillé : temps d'image,",
       "%s también lo cambia en el juego. Detallado: tiempo de fotograma,", "%s lo cambia anche in gioco. Dettagliato: tempo di frame,"},
      {"GPU load and temperature, CPU load, video and system memory.",
       "загрузка и температура GPU, загрузка CPU, видео- и оперативная память.",
       "GPU-Last und -Temperatur, CPU-Last, Video- und Arbeitsspeicher.",
       "charge et température GPU, charge CPU, mémoire vidéo et vive.",
       "carga y temperatura de la GPU, carga de la CPU, memoria de vídeo y RAM.",
       "carico e temperatura GPU, carico CPU, memoria video e RAM."},
      {"Load checkpoint", "Загрузка чекпоинта", "Checkpoint laden", "Charger un checkpoint", "Cargar punto de control",
       "Carica checkpoint"},
      {"Difficulty", "Сложность", "Schwierigkeit", "Difficulté", "Dificultad", "Difficoltà"},
      {"No files in %s", "Нет файлов в %s", "Keine Dateien in %s", "Aucun fichier dans %s", "No hay archivos en %s",
       "Nessun file in %s"},
      {"Mission ", "Миссия ", "Mission ", "Mission ", "Misión ", "Missione "},
      {"  (start)", "  (начало)", "  (Anfang)", "  (début)", "  (inicio)", "  (inizio)"},
      {"Load", "Загрузить", "Laden", "Charger", "Cargar", "Carica"},
      {"Loads for both. Your partner needs the same", "Загрузится у обоих. У напарника должны быть те же",
       "Lädt bei beiden. Dein Partner braucht dieselben", "Se charge chez les deux. Votre partenaire doit avoir les mêmes",
       "Se carga en ambos. Tu compañero necesita los mismos", "Si carica per entrambi. Il compagno deve avere gli stessi"},
      {"files (a DLC checkpoint without the DLC breaks his game).", "файлы (DLC-чекпоинт без DLC у него разорвёт игру).",
       "Dateien (ein DLC-Checkpoint ohne DLC zerstört sein Spiel).", "fichiers (un checkpoint DLC sans le DLC casse sa partie).",
       "archivos (un punto de control DLC sin el DLC rompe su partida).", "file (un checkpoint DLC senza il DLC rompe la sua partita)."},
      {"Double-click loads too. Saving the game writes", "Двойной клик тоже загружает. Сохранение игры запишет",
       "Doppelklick lädt auch. Speichern schreibt", "Un double-clic charge aussi. Sauvegarder écrit",
       "El doble clic también carga. Guardar escribe", "Anche il doppio clic carica. Salvare scrive"},
      {"the loaded checkpoint to the profile as the current one.", "загруженный чекпоинт в профиль как текущий.",
       "den geladenen Checkpoint als aktuellen ins Profil.", "le checkpoint chargé dans le profil comme actuel.",
       "el punto de control cargado en el perfil como el actual.", "il checkpoint caricato nel profilo come quello attuale."},
      {"Somalia", "Сомали", "Somalia", "Somalie", "Somalia", "Somalia"},
      {"Afghanistan", "Афганистан", "Afghanistan", "Afghanistan", "Afganistán", "Afghanistan"},
      {"Iraq", "Ирак", "Irak", "Irak", "Irak", "Iraq"},
      {"Aircraft carrier", "Авианосец", "Flugzeugträger", "Porte-avions", "Portaaviones", "Portaerei"},
      {"China", "Китай", "China", "Chine", "China", "Cina"},
      {"Miami", "Майами", "Miami", "Miami", "Miami", "Miami"},
      {"Kiev (DLC)", "Киев (DLC)", "Kiew (DLC)", "Kiev (DLC)", "Kiev (DLC)", "Kiev (DLC)"},
      {"Miami, alternate ending (DLC)", "Майами, альтернативная концовка (DLC)", "Miami, alternatives Ende (DLC)",
       "Miami, fin alternative (DLC)", "Miami, final alternativo (DLC)", "Miami, finale alternativo (DLC)"},

      // --- Frame rate overlay ---
      {"Frame", "Кадр", "Frame", "Image", "Frame", "Frame"},
      {"%5.1f ms", "%5.1f мс", "%5.1f ms", "%5.1f ms", "%5.1f ms", "%5.1f ms"},
      {"max %.1f", "макс %.1f", "max %.1f", "max %.1f", "máx %.1f", "max %.1f"},
      {"%.2f / %.1f GB", "%.2f / %.1f ГБ", "%.2f / %.1f GB", "%.2f / %.1f Go", "%.2f / %.1f GB", "%.2f / %.1f GB"},
      {"? / %.1f GB", "? / %.1f ГБ", "? / %.1f GB", "? / %.1f Go", "? / %.1f GB", "? / %.1f GB"},
      {"%.1f / %.1f GB", "%.1f / %.1f ГБ", "%.1f / %.1f GB", "%.1f / %.1f Go", "%.1f / %.1f GB", "%.1f / %.1f GB"},
      {"game %.2f", "игра %.2f", "Spiel %.2f", "jeu %.2f", "juego %.2f", "gioco %.2f"},
      {"game %.0f%%", "игра %.0f%%", "Spiel %.0f%%", "jeu %.0f%%", "juego %.0f%%", "gioco %.0f%%"},

      // --- The Graphics page of the game's Options screen (in the game's fonts) ---
      {"Graphics", "Графика", "Grafik", "Graphismes", "Gráficos", "Grafica"},
      {"Open", "Открыть", "Öffnen", "Ouvrir", "Abrir", "Apri"},
      {"Display", "Экран", "Anzeige", "Affichage", "Pantalla", "Schermo"},
      {"Effects", "Эффекты", "Effekte", "Effets", "Efectos", "Effetti"},
      {"Ambient occlusion", "Фоновое затенение", "Umgebungsverdeckung", "Occlusion ambiante", "Oclusión ambiental",
       "Occlusione ambientale"},
      {"Motion blur", "Размытие в движении", "Bewegungsunschärfe", "Flou de mouvement", "Desenfoque de movimiento",
       "Sfocatura di movimento"},
      {"Depth of field", "Глубина резкости", "Tiefenschärfe", "Profondeur de champ", "Profundidad de campo",
       "Profondità di campo"},
      {"Shadow resolution", "Разрешение теней", "Schattenauflösung", "Résolution des ombres", "Resolución de sombras",
       "Risoluzione ombre"},
      {"Low", "Низкое", "Niedrig", "Faible", "Bajo", "Basso"},
      {"High", "Высокое", "Hoch", "Élevé", "Alto", "Alto"},
      {"Graphics and controls", "Графика и управление", "Grafik und Steuerung", "Graphismes et commandes",
       "Gráficos y controles", "Grafica e comandi"},
      // The main menu's last item (menu_mouse.cpp).
      {"Exit", "Выход", "Beenden", "Quitter", "Salir", "Esci"},
      {"Display mode", "Режим экрана", "Anzeigemodus", "Mode d'affichage", "Modo de pantalla", "Modalità schermo"},
      {"Fullscreen", "Весь экран", "Vollbild", "Plein écran", "Pantalla completa", "Schermo intero"},
      {"Windowed", "Окно", "Fenster", "Fenêtré", "Ventana", "Finestra"},
      {"Render resolution", "Разрешение рендера", "Renderauflösung", "Résolution de rendu", "Resolución de render",
       "Risoluzione di rendering"},
      {"Resolution: after restart", "Разрешение: после перезапуска", "Auflösung: nach Neustart",
       "Résolution : au redémarrage", "Resolución: al reiniciar", "Risoluzione: al riavvio"},
      {"Aspect ratio", "Соотношение сторон", "Seitenverhältnis", "Format d'image", "Relación de aspecto", "Proporzioni"},
      {"Aspect ratio: after restart", "Соотношение: после перезапуска", "Seitenverhältnis: nach Neustart",
       "Format : au redémarrage", "Aspecto: al reiniciar", "Proporzioni: al riavvio"},
      {"Auto", "Авто", "Auto", "Auto", "Auto", "Auto"},
      {"Resolution: applies on Accept", "Разрешение: после «Принять»", "Auflösung: nach Übernehmen",
       "Résolution : après Accepter", "Resolución: al Aceptar", "Risoluzione: dopo Accetta"},
      {"Vertical sync", "Вертикальная синхронизация", "Vertikale Synchronisation", "Synchro verticale",
       "Sincronización vertical", "Sincronizzazione verticale"},
      {"Wider view", "Широкий обзор", "Breitere Sicht", "Vue élargie", "Vista ampliada", "Visuale ampia"},
      {"On", "Вкл", "Ein", "Activé", "Encendido", "Acceso"},
      {"FPS limit", "Ограничение FPS", "FPS-Limit", "Limite FPS", "Límite de FPS", "Limite FPS"},
      {"Anisotropic filtering", "Анизотропная фильтрация", "Anisotrope Filterung", "Filtrage anisotrope",
       "Filtrado anisotrópico", "Filtro anisotropico"},
      {"Anti-aliasing", "Сглаживание", "Kantenglättung", "Anticrénelage", "Antialiasing", "Antialiasing"},
      // --- The Controls page (Options > Graphics, in the game's fonts) ---
      {"Controls", "Управление", "Steuerung", "Commandes", "Controles", "Comandi"},
      {"Default keys", "Клавиши по умолчанию", "Standardtasten", "Touches par défaut", "Teclas por defecto", "Tasti predefiniti"},
      {"Press again to reset", "Нажмите ещё раз для сброса", "Zum Zurücksetzen erneut drücken", "Appuyez encore pour réinitialiser",
       "Pulsa otra vez para restablecer", "Premi ancora per ripristinare"},
      {"Press a key...", "Нажмите клавишу...", "Taste drücken...", "Appuyez sur une touche...", "Pulsa una tecla...", "Premi un tasto..."},
      {"Section", "Раздел", "Bereich", "Section", "Sección", "Sezione"},
      {"Movement", "Движение", "Bewegung", "Déplacement", "Movimiento", "Movimento"},
      {"Combat", "Бой", "Kampf", "Combat", "Combate", "Combattimento"},
      {"Other", "Прочее", "Sonstiges", "Autres", "Otros", "Altro"},
      {"Mouse sensitivity", "Чувствительность мыши", "Mausempfindlichkeit", "Sensibilité souris", "Sensibilidad del ratón",
       "Sensibilità mouse"},
      {"Move forward", "Вперёд", "Vorwärts", "Avancer", "Avanzar", "Avanti"},
      {"Move back", "Назад", "Rückwärts", "Reculer", "Retroceder", "Indietro"},
      {"Move left", "Влево", "Links", "Gauche", "Izquierda", "Sinistra"},
      {"Move right", "Вправо", "Rechts", "Droite", "Derecha", "Destra"},
      {"Crouch", "Присесть", "Ducken", "S'accroupir", "Agacharse", "Accovacciarsi"},
      {"Roll / cover / vault", "Кувырок / укрытие", "Rolle / Deckung", "Roulade / couverture", "Rodar / cobertura",
       "Capriola / copertura"},
      {"Use / heal / confirm", "Действие / лечить", "Benutzen / heilen", "Utiliser / soigner", "Usar / curar", "Usa / cura"},
      {"Fire", "Огонь", "Feuer", "Tirer", "Disparar", "Fuoco"},
      {"Aim", "Прицел", "Zielen", "Viser", "Apuntar", "Mira"},
      {"Reload", "Перезарядка", "Nachladen", "Recharger", "Recargar", "Ricarica"},
      {"Inventory", "Инвентарь", "Inventar", "Inventaire", "Inventario", "Inventario"},
      {"Co-op commands", "Команды напарнику", "Koop-Befehle", "Ordres coop", "Órdenes coop", "Ordini coop"},
      {"Cancel / back", "Отмена / назад", "Abbrechen / zurück", "Annuler / retour", "Cancelar / atrás", "Annulla / indietro"},
      {"Right stick (R3)", "Правый стик (R3)", "Rechter Stick (R3)", "Stick droit (R3)", "Stick derecho (R3)", "Levetta destra (R3)"},
      {"Advance", "Наступать", "Vorrücken", "Avancer (ordre)", "Avanzar (orden)", "Avanzare"},
      {"Partner view", "Вид напарника", "Partnersicht", "Vue partenaire", "Vista del compañero", "Vista compagno"},
      {"Regroup", "Ко мне", "Sammeln", "Regroupement", "Reagruparse", "Raggruppare"},
      {"Hold position", "Держать позицию", "Position halten", "Tenir la position", "Mantener posición", "Mantenere posizione"},
      {"GPS", "GPS", "GPS", "GPS", "GPS", "GPS"},
      {"Inventory (hold)", "Инвентарь (удерж.)", "Inventar (halten)", "Inventaire (maintenir)", "Inventario (mantener)",
       "Inventario (tieni)"},
      {"Primary weapon", "Основное оружие", "Primärwaffe", "Arme principale", "Arma principal", "Arma primaria"},
      {"Secondary weapon", "Запасное оружие", "Sekundärwaffe", "Arme secondaire", "Arma secundaria", "Arma secondaria"},
      {"Special weapon", "Спецоружие", "Spezialwaffe", "Arme spéciale", "Arma especial", "Arma speciale"},
      {"Grenades", "Гранаты", "Granaten", "Grenades", "Granadas", "Granate"},
      {"Pause", "Пауза", "Pause", "Pause", "Pausa", "Pausa"},
      // Keys as the page shows them.
      {"LMB", "ЛКМ", "LMT", "Clic G", "Clic izq.", "Tasto sx"},
      {"RMB", "ПКМ", "RMT", "Clic D", "Clic der.", "Tasto dx"},
      {"MMB", "СКМ", "MMT", "Clic M", "Clic central", "Tasto centrale"},
      {"Mouse 4", "Мышь 4", "Maus 4", "Souris 4", "Ratón 4", "Mouse 4"},
      {"Mouse 5", "Мышь 5", "Maus 5", "Souris 5", "Ratón 5", "Mouse 5"},
      {"Up", "Вверх", "Hoch", "Haut", "Arriba", "Su"},
      {"Down", "Вниз", "Runter", "Bas", "Abajo", "Giù"},
      {"Left", "Влево", "Links", "Gauche", "Izquierda", "Sinistra"},
      {"Right", "Вправо", "Rechts", "Droite", "Derecha", "Destra"},
      {"Space", "Пробел", "Leertaste", "Espace", "Espacio", "Spazio"},
  };
  // clang-format on

  std::atomic<Lang> g_game_lang{Lang::kEn};

  const std::unordered_map<std::string_view, const Row *> &Table()
  {
    static const auto table = [] {
      std::unordered_map<std::string_view, const Row *> t;
      for (const Row &row : kRows)
      {
        t.emplace(row.en, &row);
      }
      return t;
    }();
    return table;
  }

  bool ParseLang(std::string code, Lang *out)
  {
    std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    static const std::pair<const char *, Lang> kCodes[] = {
        {"en", Lang::kEn},  {"int", Lang::kEn}, {"eng", Lang::kEn}, {"ru", Lang::kRu}, {"rus", Lang::kRu},
        {"de", Lang::kDe},  {"deu", Lang::kDe}, {"fr", Lang::kFr},  {"fra", Lang::kFr}, {"es", Lang::kEs},
        {"esn", Lang::kEs}, {"esm", Lang::kEs}, {"it", Lang::kIt},  {"ita", Lang::kIt},
    };
    for (const auto &[name, lang] : kCodes)
    {
      if (code == name)
      {
        *out = lang;
        return true;
      }
    }
    return false;
  }
} // namespace

namespace ao2::text
{
  void Init()
  {
    const char *code = ao2::lang::Code();
    Lang lang = Lang::kEn;
    ParseLang(code, &lang);
    g_game_lang = lang;
    REXLOG_INFO("UI language: game '{}', ao2_ui_language '{}'", code, REXCVAR_GET(ao2_ui_language));
  }

  Lang Current()
  {
    Lang lang;
    if (ParseLang(REXCVAR_GET(ao2_ui_language), &lang))
    {
      return lang;
    }
    return g_game_lang.load(std::memory_order_relaxed);
  }

  Lang Game() { return g_game_lang.load(std::memory_order_relaxed); }

  const char *Tr(const char *en) { return TrIn(Current(), en); }

  const char *TrIn(Lang lang, const char *en)
  {
    if (lang == Lang::kEn)
    {
      return en;
    }
    auto it = Table().find(en);
    if (it == Table().end())
    {
      return en;
    }
    const Row &row = *it->second;
    switch (lang)
    {
    case Lang::kRu:
      return row.ru;
    case Lang::kDe:
      return row.de;
    case Lang::kFr:
      return row.fr;
    case Lang::kEs:
      return row.es;
    case Lang::kIt:
      return row.it;
    default:
      return en;
    }
  }
} // namespace ao2::text
