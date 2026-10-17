using System;
using System.Collections.Generic;
using System.Globalization;

namespace AO2Launcher
{
    // The launcher's text in the language chosen for the game (the "language"
    // setting; auto: Windows' language when the game has it, else English, as
    // the game decides). Every string is keyed by its English: L.T("Settings")
    // is that row's text in the current language, the English itself when the
    // table has none. The text is translated where it is drawn, so a change
    // of the language shows at once.
    static class L
    {
        // en, then ru, fr, de, es, it.
        static readonly string[] Codes = { "ru", "fr", "de", "es", "it" };
        static int lang = -1; // index into Codes; -1: English

        public static string Code
        {
            get { return lang < 0 ? "en" : Codes[lang]; }
        }

        public static void Set(string setting)
        {
            string code = setting;
            if (Array.IndexOf(Codes, code) < 0 && code != "en")
            {
                code = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;
            }
            lang = Array.IndexOf(Codes, code);
        }

        public static string T(string en)
        {
            string[] row;
            if (en == null || lang < 0 || !Table.TryGetValue(en, out row))
            {
                return en;
            }
            return row[lang];
        }

        public static string F(string en, params object[] args)
        {
            return string.Format(T(en), args);
        }

        static readonly Dictionary<string, string[]> Table = Build();

        static Dictionary<string, string[]> Build()
        {
            var t = new Dictionary<string, string[]>();
            Action<string, string, string, string, string, string> a = (en, ru, fr, de, es, it) =>
                t[en] = new[] { ru, fr, de, es, it };

            // --- Title bar, main screen ---
            a("Launcher", "Лаунчер", "Lanceur", "Launcher", "Lanzador", "Launcher");
            a("Installation", "Установка", "Installation", "Installation", "Instalación", "Installazione");
            a("Settings", "Настройки", "Paramètres", "Einstellungen", "Ajustes", "Impostazioni");
            a("BACK", "НАЗАД", "RETOUR", "ZURÜCK", "ATRÁS", "INDIETRO");
            a("SETTINGS", "НАСТРОЙКИ", "PARAMÈTRES", "EINSTELLUNGEN", "AJUSTES", "IMPOSTAZIONI");
            a("EXIT", "ВЫХОД", "QUITTER", "BEENDEN", "SALIR", "ESCI");
            a("PLAY", "ИГРАТЬ", "JOUER", "SPIELEN", "JUGAR", "GIOCA");
            a("INSTALL", "УСТАНОВИТЬ", "INSTALLER", "INSTALLIEREN", "INSTALAR", "INSTALLA");
            a("LOOKING FOR A HOST…", "ПОИСК ХОСТА…", "RECHERCHE D'UN HÔTE…", "SUCHE HOST…", "BUSCANDO ANFITRIÓN…", "RICERCA HOST…");
            a("Game not found — click to locate armyoftworecomp.exe",
              "Игра не найдена — нажмите, чтобы указать armyoftworecomp.exe",
              "Jeu introuvable — cliquez pour indiquer armyoftworecomp.exe",
              "Spiel nicht gefunden — klicken, um armyoftworecomp.exe zu wählen",
              "Juego no encontrado — haz clic para buscar armyoftworecomp.exe",
              "Gioco non trovato — clicca per indicare armyoftworecomp.exe");
            a("Controller connected", "Геймпад подключён", "Manette connectée", "Controller verbunden", "Mando conectado", "Controller collegato");
            a("No controller", "Геймпада нет", "Pas de manette", "Kein Controller", "Sin mando", "Nessun controller");
            a("Fullscreen", "Полный экран", "Plein écran", "Vollbild", "Pantalla completa", "Schermo intero");
            a("Window {0}", "Окно {0}", "Fenêtre {0}", "Fenster {0}", "Ventana {0}", "Finestra {0}");

            // --- Updates ---
            a("{0} · up to date", "{0} · актуальная версия", "{0} · à jour", "{0} · aktuell", "{0} · actualizado", "{0} · aggiornato");
            a("{0} · update available: {1}", "{0} · доступно обновление: {1}", "{0} · mise à jour disponible : {1}",
              "{0} · Update verfügbar: {1}", "{0} · actualización disponible: {1}", "{0} · aggiornamento disponibile: {1}");
            a("UPDATE AVAILABLE", "ДОСТУПНО ОБНОВЛЕНИЕ", "MISE À JOUR DISPONIBLE", "UPDATE VERFÜGBAR", "ACTUALIZACIÓN DISPONIBLE",
              "AGGIORNAMENTO DISPONIBILE");
            a("Version {0} is out, you have {1}. Update now?",
              "Вышла версия {0}, у вас {1}. Обновить сейчас?",
              "La version {0} est sortie, vous avez la {1}. Mettre à jour maintenant ?",
              "Version {0} ist erschienen, Sie haben {1}. Jetzt aktualisieren?",
              "Ya está disponible la versión {0}; tienes la {1}. ¿Actualizar ahora?",
              "È uscita la versione {0}, hai la {1}. Aggiornare ora?");
            a("The launcher downloads and installs it by itself. Your game files, settings and saves are kept.",
              "Лаунчер сам скачает и установит её. Файлы игры, настройки и сохранения останутся.",
              "Le lanceur la télécharge et l'installe lui-même. Fichiers du jeu, paramètres et sauvegardes sont conservés.",
              "Der Launcher lädt und installiert sie selbst. Spieldateien, Einstellungen und Spielstände bleiben erhalten.",
              "El lanzador la descarga e instala solo. Se conservan los archivos del juego, los ajustes y las partidas.",
              "Il launcher la scarica e la installa da solo. File del gioco, impostazioni e salvataggi restano.");
            a("UPDATE", "ОБНОВИТЬ", "METTRE À JOUR", "AKTUALISIEREN", "ACTUALIZAR", "AGGIORNA");
            a("LATER", "ПОЗЖЕ", "PLUS TARD", "SPÄTER", "MÁS TARDE", "PIÙ TARDI");
            a("RETRY", "ПОВТОРИТЬ", "RÉESSAYER", "WIEDERHOLEN", "REINTENTAR", "RIPROVA");
            a("UPDATING TO {0}", "ОБНОВЛЕНИЕ ДО {0}", "MISE À JOUR VERS {0}", "UPDATE AUF {0}", "ACTUALIZANDO A {0}", "AGGIORNAMENTO A {0}");
            a("Downloading: {0:0.0} of {1:0.0} MB", "Загрузка: {0:0.0} из {1:0.0} МБ", "Téléchargement : {0:0.0} sur {1:0.0} Mo",
              "Download: {0:0.0} von {1:0.0} MB", "Descargando: {0:0.0} de {1:0.0} MB", "Download: {0:0.0} di {1:0.0} MB");
            a("Downloading: {0:0.0} MB", "Загрузка: {0:0.0} МБ", "Téléchargement : {0:0.0} Mo", "Download: {0:0.0} MB",
              "Descargando: {0:0.0} MB", "Download: {0:0.0} MB");
            a("Installing: {0:0}%", "Установка: {0:0}%", "Installation : {0:0} %", "Installation: {0:0} %", "Instalando: {0:0} %",
              "Installazione: {0:0}%");
            a("The launcher restarts by itself when the update is installed.",
              "Когда обновление установится, лаунчер перезапустится сам.",
              "Le lanceur redémarre de lui-même une fois la mise à jour installée.",
              "Der Launcher startet nach der Installation von selbst neu.",
              "El lanzador se reinicia solo cuando la actualización esté instalada.",
              "Il launcher si riavvia da solo a installazione completata.");
            a("Do not close the launcher.", "Не закрывайте лаунчер.", "Ne fermez pas le lanceur.", "Schließen Sie den Launcher nicht.",
              "No cierres el lanzador.", "Non chiudere il launcher.");
            a("UPDATE FAILED", "ОБНОВЛЕНИЕ НЕ УДАЛОСЬ", "ÉCHEC DE LA MISE À JOUR", "UPDATE FEHLGESCHLAGEN", "LA ACTUALIZACIÓN FALLÓ",
              "AGGIORNAMENTO NON RIUSCITO");
            a("Try again, or download the archive from GitHub and extract it over this folder.",
              "Попробуйте ещё раз или скачайте архив с GitHub и распакуйте поверх этой папки.",
              "Réessayez, ou téléchargez l'archive sur GitHub et extrayez-la par-dessus ce dossier.",
              "Erneut versuchen oder das Archiv von GitHub laden und über diesen Ordner entpacken.",
              "Inténtalo de nuevo o descarga el archivo de GitHub y extráelo sobre esta carpeta.",
              "Riprova, oppure scarica l'archivio da GitHub ed estrailo sopra questa cartella.");
            a("Close the game first.", "Сначала закройте игру.", "Fermez d'abord le jeu.", "Schließen Sie zuerst das Spiel.",
              "Cierra primero el juego.", "Chiudi prima il gioco.");
            a("Update cancelled", "Обновление отменено", "Mise à jour annulée", "Update abgebrochen", "Actualización cancelada",
              "Aggiornamento annullato");
            a("Updated to {0}. Start the launcher again: {1}", "Обновлено до {0}. Запустите лаунчер заново: {1}",
              "Mis à jour vers {0}. Relancez le lanceur : {1}", "Auf {0} aktualisiert. Starten Sie den Launcher neu: {1}",
              "Actualizado a {0}. Vuelve a abrir el lanzador: {1}", "Aggiornato a {0}. Riavvia il launcher: {1}");
            a("Could not open the browser: {0}", "Не удалось открыть браузер: {0}", "Impossible d'ouvrir le navigateur : {0}",
              "Browser konnte nicht geöffnet werden: {0}", "No se pudo abrir el navegador: {0}", "Impossibile aprire il browser: {0}");

            // --- Settings: tabs, hints, common values ---
            a("GRAPHICS", "ГРАФИКА", "GRAPHISMES", "GRAFIK", "GRÁFICOS", "GRAFICA");
            a("CONTROLS", "УПРАВЛЕНИЕ", "COMMANDES", "STEUERUNG", "CONTROLES", "COMANDI");
            a("NETWORK", "СЕТЬ", "RÉSEAU", "NETZWERK", "RED", "RETE");
            a("ADVANCED", "ДОПОЛНИТЕЛЬНО", "AVANCÉ", "ERWEITERT", "AVANZADO", "AVANZATE");
            a("Tabs", "Вкладки", "Onglets", "Tabs", "Pestañas", "Schede");
            a("Select", "Выбрать", "Choisir", "Auswählen", "Elegir", "Seleziona");
            a("Back", "Назад", "Retour", "Zurück", "Atrás", "Indietro");
            a("OFF", "ВЫКЛ", "DÉSACTIVÉ", "AUS", "DESACTIVADO", "DISATTIVATO");
            a("ON", "ВКЛ", "ACTIVÉ", "AN", "ACTIVADO", "ATTIVATO");
            a("NO", "НЕТ", "NON", "NEIN", "NO", "NO");
            a("YES", "ДА", "OUI", "JA", "SÍ", "SÌ");
            a("AUTO", "АВТО", "AUTO", "AUTO", "AUTO", "AUTO");
            a("auto", "авто", "auto", "auto", "auto", "auto");
            a(" ms", " мс", " ms", " ms", " ms", " ms");
            a(" GB", " ГБ", " Go", " GB", " GB", " GB");
            a(" MB", " МБ", " Mo", " MB", " MB", " MB");

            // --- Graphics ---
            a("Language", "Язык", "Langue", "Sprache", "Idioma", "Lingua");
            a("Language of the game (text and voices) and of this launcher. Auto follows the language of Windows. " +
              "Russian is text only, with English voices. In co-op both players need the same voices: " +
              "English and Russian play together, the other languages only with the same language.",
              "Язык игры (текст и озвучка) и этого лаунчера. Авто — язык Windows. Русский — только текст, озвучка " +
              "английская. В кооперативе у обоих игроков должна быть одна озвучка: английский и русский совместимы, " +
              "остальные языки — только с таким же.",
              "Langue du jeu (textes et voix) et de ce lanceur. Auto suit la langue de Windows. Le russe ne traduit que " +
              "le texte, les voix restent en anglais. En coop, les deux joueurs doivent avoir les mêmes voix : anglais " +
              "et russe vont ensemble, les autres langues seulement avec la même langue.",
              "Sprache des Spiels (Text und Stimmen) und dieses Launchers. Auto folgt der Sprache von Windows. Russisch " +
              "übersetzt nur den Text, die Stimmen bleiben englisch. Im Koop brauchen beide Spieler dieselben Stimmen: " +
              "Englisch und Russisch passen zusammen, die anderen Sprachen nur mit derselben Sprache.",
              "Idioma del juego (textos y voces) y de este lanzador. Auto sigue el idioma de Windows. El ruso solo " +
              "traduce el texto; las voces siguen en inglés. En cooperativo ambos jugadores necesitan las mismas voces: " +
              "inglés y ruso van juntos, los demás idiomas solo con el mismo idioma.",
              "Lingua del gioco (testi e voci) e di questo launcher. Auto segue la lingua di Windows. Il russo traduce " +
              "solo il testo, le voci restano in inglese. In cooperativa entrambi i giocatori devono avere le stesse " +
              "voci: inglese e russo vanno insieme, le altre lingue solo con la stessa lingua.");
            a("Display mode", "Режим экрана", "Mode d'affichage", "Anzeigemodus", "Modo de pantalla", "Modalità schermo");
            a("FULLSCREEN", "ПОЛНЫЙ ЭКРАН", "PLEIN ÉCRAN", "VOLLBILD", "PANTALLA COMPLETA", "SCHERMO INTERO");
            a("WINDOWED", "В ОКНЕ", "FENÊTRÉ", "FENSTER", "VENTANA", "FINESTRA");
            a("Fullscreen is a borderless window covering the monitor; the desktop mode is not switched. " +
              "A 16:9 picture is letterboxed with black bars.",
              "Полный экран — окно без рамки на весь монитор, режим рабочего стола не переключается. " +
              "Картинка 16:9 дополняется чёрными полосами.",
              "Le plein écran est une fenêtre sans bordure qui couvre le moniteur ; la résolution du bureau ne change " +
              "pas. Une image 16:9 est complétée par des bandes noires.",
              "Vollbild ist ein randloses Fenster über den ganzen Monitor; der Desktopmodus wird nicht umgeschaltet. " +
              "Ein 16:9-Bild bekommt schwarze Balken.",
              "La pantalla completa es una ventana sin bordes que cubre el monitor; el modo del escritorio no cambia. " +
              "Una imagen 16:9 se completa con franjas negras.",
              "Lo schermo intero è una finestra senza bordi che copre il monitor; la modalità del desktop non cambia. " +
              "Un'immagine 16:9 viene completata con bande nere.");
            a("Window size", "Размер окна", "Taille de la fenêtre", "Fenstergröße", "Tamaño de ventana", "Dimensioni finestra");
            a("Window size in screen pixels (windowed mode only). " +
              "The game shrinks a window that does not fit the monitor's work area.",
              "Размер окна в пикселях экрана (только в оконном режиме). Окно, которое не помещается в рабочую " +
              "область монитора, игра уменьшит.",
              "Taille de la fenêtre en pixels d'écran (mode fenêtré uniquement). Le jeu réduit une fenêtre qui ne " +
              "tient pas dans la zone de travail du moniteur.",
              "Fenstergröße in Bildschirmpixeln (nur im Fenstermodus). Ein Fenster, das nicht in den Arbeitsbereich " +
              "des Monitors passt, verkleinert das Spiel.",
              "Tamaño de la ventana en píxeles de pantalla (solo en modo ventana). El juego reduce una ventana que no " +
              "cabe en el área de trabajo del monitor.",
              "Dimensioni della finestra in pixel dello schermo (solo in modalità finestra). Il gioco riduce una " +
              "finestra che non entra nell'area di lavoro del monitor.");
            a("Monitor", "Монитор", "Moniteur", "Monitor", "Monitor", "Monitor");
            a("PRIMARY", "ОСНОВНОЙ", "PRINCIPAL", "PRIMÄR", "PRINCIPAL", "PRINCIPALE");
            a(" (primary)", " (основной)", " (principal)", " (primär)", " (principal)", " (principale)");
            a("Which monitor the game opens on.", "На каком мониторе открывается игра.", "Le moniteur sur lequel le jeu s'ouvre.",
              "Auf welchem Monitor das Spiel startet.", "En qué monitor se abre el juego.", "Su quale monitor si apre il gioco.");
            a("Aspect ratio", "Соотношение сторон", "Format d'image", "Seitenverhältnis", "Relación de aspecto", "Proporzioni");
            a("Shape of the picture. Auto follows the window (the monitor in fullscreen), " +
              "with no black bars. 16:9 is what the console had.",
              "Форма картинки. Авто — по окну (в полноэкранном режиме — по монитору), без чёрных полос. " +
              "16:9 — как на консоли.",
              "Forme de l'image. Auto suit la fenêtre (le moniteur en plein écran), sans bandes noires. " +
              "16:9 est le format de la console.",
              "Form des Bildes. Auto folgt dem Fenster (im Vollbild dem Monitor), ohne schwarze Balken. " +
              "16:9 hatte die Konsole.",
              "Forma de la imagen. Auto sigue la ventana (el monitor en pantalla completa), sin franjas negras. " +
              "16:9 es lo que tenía la consola.",
              "Forma dell'immagine. Auto segue la finestra (il monitor a schermo intero), senza bande nere. " +
              "16:9 è quello della console.");
            a("Render resolution", "Разрешение рендера", "Résolution de rendu", "Renderauflösung", "Resolución de renderizado",
              "Risoluzione di rendering");
            a("Internal resolution the game is rendered at (the console original is 720p). " +
              "1.5× is exactly Full HD, pixel for pixel on a 1920×1080 monitor; 2× is sharper and smoother but heavier. " +
              "Use 1× on a weak graphics card; 3× and 4× are for powerful ones.",
              "Внутреннее разрешение, в котором рисуется игра (на консоли — 720p). 1.5× — ровно Full HD, пиксель в " +
              "пиксель на мониторе 1920×1080; 2× чётче и глаже, но тяжелее. 1× — для слабой видеокарты, 3× и 4× — для мощных.",
              "Résolution interne du rendu (720p sur la console). 1.5× correspond exactement au Full HD, pixel pour " +
              "pixel sur un écran 1920×1080 ; 2× est plus net et plus lisse mais plus lourd. 1× pour une carte " +
              "graphique modeste ; 3× et 4× pour les cartes puissantes.",
              "Interne Auflösung, in der das Spiel gerendert wird (auf der Konsole 720p). 1.5× ist genau Full HD, " +
              "pixelgenau auf einem 1920×1080-Monitor; 2× ist schärfer und glatter, aber aufwendiger. 1× für schwache " +
              "Grafikkarten, 3× und 4× für starke.",
              "Resolución interna a la que se renderiza el juego (en la consola, 720p). 1.5× es exactamente Full HD, " +
              "píxel a píxel en un monitor de 1920×1080; 2× es más nítido y suave, pero más exigente. 1× para una " +
              "tarjeta gráfica modesta; 3× y 4× para las potentes.",
              "Risoluzione interna a cui il gioco viene renderizzato (sulla console 720p). 1.5× è esattamente Full HD, " +
              "pixel per pixel su un monitor 1920×1080; 2× è più nitido e fluido ma più pesante. 1× per una scheda " +
              "video debole; 3× e 4× per quelle potenti.");
            a("FPS limit", "Ограничение FPS", "Limite de FPS", "FPS-Limit", "Límite de FPS", "Limite FPS");
            a("Maximum frame rate; the speed of the game itself does not depend on it. " +
              "60 is the most tested option; above ~150 FPS the processor is usually the limit.",
              "Максимальная частота кадров; скорость самой игры от неё не зависит. 60 — самый проверенный вариант; " +
              "выше ~150 FPS обычно упирается в процессор.",
              "Fréquence d'images maximale ; la vitesse du jeu lui-même n'en dépend pas. 60 est l'option la plus " +
              "testée ; au-delà de ~150 FPS, le processeur est généralement la limite.",
              "Höchste Bildrate; die Geschwindigkeit des Spiels selbst hängt nicht davon ab. 60 ist am besten " +
              "getestet; über ~150 FPS begrenzt meist der Prozessor.",
              "Tasa máxima de fotogramas; la velocidad del juego no depende de ella. 60 es la opción más probada; " +
              "por encima de ~150 FPS el límite suele ser el procesador.",
              "Frequenza massima dei fotogrammi; la velocità del gioco non dipende da essa. 60 è l'opzione più " +
              "collaudata; oltre ~150 FPS di solito il limite è il processore.");
            a("FPS counter", "Счётчик FPS", "Compteur de FPS", "FPS-Anzeige", "Contador de FPS", "Contatore FPS");
            a("DETAILED", "ПОДРОБНО", "DÉTAILLÉ", "DETAILLIERT", "DETALLADO", "DETTAGLIATO");
            a("Frame rate in the corner of the screen. Detailed adds frame time, graphics card load and temperature, " +
              "processor load, video and system memory. F2 switches it in the game.",
              "Частота кадров в углу экрана. «Подробно» добавляет время кадра, загрузку и температуру видеокарты, " +
              "загрузку процессора, видеопамять и оперативную память. В игре переключается клавишей F2.",
              "Fréquence d'images dans le coin de l'écran. Détaillé ajoute le temps d'image, la charge et la " +
              "température de la carte graphique, la charge du processeur, la mémoire vidéo et système. F2 le change en jeu.",
              "Bildrate in der Bildschirmecke. Detailliert zeigt zusätzlich Frametime, Auslastung und Temperatur der " +
              "Grafikkarte, Prozessorauslastung, Video- und Arbeitsspeicher. F2 schaltet im Spiel um.",
              "Tasa de fotogramas en la esquina de la pantalla. Detallado añade el tiempo de fotograma, la carga y " +
              "temperatura de la tarjeta gráfica, la carga del procesador y la memoria de vídeo y del sistema. F2 lo cambia en el juego.",
              "Frequenza dei fotogrammi nell'angolo dello schermo. Dettagliato aggiunge tempo del fotogramma, carico e " +
              "temperatura della scheda video, carico del processore, memoria video e di sistema. F2 lo cambia nel gioco.");
            a("Vertical sync", "Вертикальная синхронизация", "Synchro verticale", "Vertikale Synchronisation",
              "Sincronización vertical", "Sincronizzazione verticale");
            a("Frames wait for the monitor's refresh: no tearing, but the frame rate stays at or below the refresh rate " +
              "and the input lag grows a little.",
              "Кадры ждут обновления монитора: без разрывов, но частота кадров не выше частоты монитора, " +
              "а задержка ввода немного растёт.",
              "Les images attendent le rafraîchissement de l'écran : pas de déchirure, mais la fréquence d'images ne " +
              "dépasse pas celle de l'écran et la latence augmente un peu.",
              "Bilder warten auf die Bildwiederholung des Monitors: kein Tearing, aber die Bildrate bleibt höchstens " +
              "bei der Bildwiederholrate und die Eingabeverzögerung steigt etwas.",
              "Los fotogramas esperan al refresco del monitor: sin tearing, pero la tasa no supera la del monitor " +
              "y la latencia de entrada aumenta un poco.",
              "I fotogrammi attendono l'aggiornamento del monitor: niente tearing, ma la frequenza non supera quella " +
              "del monitor e la latenza dell'input cresce un po'.");
            a("Anisotropic filtering", "Анизотропная фильтрация", "Filtrage anisotrope", "Anisotrope Filterung",
              "Filtrado anisotrópico", "Filtro anisotropico");
            a("Sharper textures at an angle (16×). Costs almost no performance.",
              "Чёткие текстуры под углом (16×). Почти не влияет на производительность.",
              "Textures plus nettes en biais (16×). Ne coûte presque rien en performances.",
              "Schärfere Texturen in Schräglage (16×). Kostet kaum Leistung.",
              "Texturas más nítidas en ángulo (16×). Casi no afecta al rendimiento.",
              "Texture più nitide viste di sbieco (16×). Non costa quasi nulla in prestazioni.");

            // --- Controls ---
            a("Mouse aiming", "Прицеливание мышью", "Visée à la souris", "Zielen mit der Maus", "Apuntar con el ratón",
              "Mira con il mouse");
            a("The mouse turns the camera directly, like in PC shooters. When off, the mouse acts as the gamepad's right stick.",
              "Мышь поворачивает камеру напрямую, как в шутерах на ПК. Если выключено, мышь работает как правый стик геймпада.",
              "La souris tourne directement la caméra, comme dans les jeux de tir sur PC. Désactivé, la souris agit " +
              "comme le stick droit de la manette.",
              "Die Maus dreht die Kamera direkt wie in PC-Shootern. Ausgeschaltet wirkt die Maus wie der rechte Stick " +
              "des Controllers.",
              "El ratón gira la cámara directamente, como en los shooters de PC. Desactivado, el ratón actúa como el " +
              "stick derecho del mando.",
              "Il mouse gira la telecamera direttamente, come negli sparatutto per PC. Se disattivato, il mouse fa da " +
              "levetta destra del controller.");
            a("Mouse sensitivity", "Чувствительность мыши", "Sensibilité de la souris", "Mausempfindlichkeit",
              "Sensibilidad del ratón", "Sensibilità del mouse");
            a("How fast the mouse turns the camera. 1.0 is 0.022° per mouse count (as in Source/Quake); the default is 2.0.",
              "Как быстро мышь поворачивает камеру. 1.0 — 0.022° на отсчёт мыши (как в Source/Quake); по умолчанию 2.0.",
              "Vitesse à laquelle la souris tourne la caméra. 1.0 = 0.022° par impulsion (comme dans Source/Quake) ; " +
              "par défaut 2.0.",
              "Wie schnell die Maus die Kamera dreht. 1.0 sind 0.022° pro Mauszählung (wie in Source/Quake); Standard ist 2.0.",
              "Lo rápido que el ratón gira la cámara. 1.0 son 0.022° por cuenta del ratón (como en Source/Quake); " +
              "por defecto, 2.0.",
              "Quanto velocemente il mouse gira la telecamera. 1.0 equivale a 0.022° per conteggio del mouse (come in " +
              "Source/Quake); il predefinito è 2.0.");
            a("Sensitivity when aiming", "Чувствительность при прицеливании", "Sensibilité en visée",
              "Empfindlichkeit beim Zielen", "Sensibilidad al apuntar", "Sensibilità in mira");
            a("Multiplier while aiming (right button, scopes). Turning already accounts for zoom; 1.0 feels the same as unaimed.",
              "Множитель при прицеливании (правая кнопка, оптика). Поворот уже учитывает зум; 1.0 ощущается так же, " +
              "как без прицеливания.",
              "Multiplicateur en visée (bouton droit, lunettes). La rotation tient déjà compte du zoom ; 1.0 donne la " +
              "même sensation que sans viser.",
              "Faktor beim Zielen (rechte Maustaste, Zielfernrohre). Die Drehung berücksichtigt den Zoom bereits; " +
              "1.0 fühlt sich wie ohne Zielen an.",
              "Multiplicador al apuntar (botón derecho, miras). El giro ya tiene en cuenta el zoom; 1.0 se siente " +
              "igual que sin apuntar.",
              "Moltiplicatore durante la mira (tasto destro, mirini). La rotazione tiene già conto dello zoom; 1.0 si " +
              "sente come senza mirare.");
            a("Invert Y axis", "Инверсия оси Y", "Inverser l'axe Y", "Y-Achse umkehren", "Invertir eje Y", "Inverti asse Y");
            a("Mouse up looks down (on top of the inversion setting in the game itself).",
              "Мышь вверх — взгляд вниз (поверх настройки инверсии в самой игре).",
              "Souris vers le haut = regard vers le bas (en plus du réglage d'inversion du jeu).",
              "Maus nach oben blickt nach unten (zusätzlich zur Invertierung im Spiel selbst).",
              "Ratón hacia arriba mira hacia abajo (además del ajuste de inversión del propio juego).",
              "Mouse in alto guarda in basso (in aggiunta all'inversione impostata nel gioco).");
            a("Mouse in vehicles and turrets", "Мышь в технике и турелях", "Souris dans véhicules et tourelles",
              "Maus in Fahrzeugen und Geschützen", "Ratón en vehículos y torretas", "Mouse su veicoli e torrette");
            a("Where the camera cannot be turned directly (turrets, vehicles, menus) the mouse acts as the right stick; " +
              "this is its sensitivity.",
              "Где камеру нельзя повернуть напрямую (турели, техника, меню), мышь работает как правый стик; это его " +
              "чувствительность.",
              "Là où la caméra ne peut pas être tournée directement (tourelles, véhicules, menus), la souris agit " +
              "comme le stick droit ; voici sa sensibilité.",
              "Wo sich die Kamera nicht direkt drehen lässt (Geschütze, Fahrzeuge, Menüs), wirkt die Maus wie der " +
              "rechte Stick; das ist seine Empfindlichkeit.",
              "Donde la cámara no se puede girar directamente (torretas, vehículos, menús), el ratón actúa como stick " +
              "derecho; esta es su sensibilidad.",
              "Dove la telecamera non si può girare direttamente (torrette, veicoli, menu) il mouse fa da levetta " +
              "destra; questa è la sua sensibilità.");

            // --- Keyboard, Gamepad, Split-screen ---
            a("KEYBOARD", "КЛАВИАТУРА", "CLAVIER", "TASTATUR", "TECLADO", "TASTIERA");
            a("GAMEPAD", "ГЕЙМПАД", "MANETTE", "CONTROLLER", "MANDO", "CONTROLLER");
            a("SPLIT-SCREEN", "РАЗДЕЛЁННЫЙ ЭКРАН", "ÉCRAN PARTAGÉ", "SPLITSCREEN", "PANTALLA DIVIDIDA", "SCHERMO CONDIVISO");
            a("Keys and mouse buttons: the Keyboard tab. Gamepad buttons and sticks: the Gamepad tab. " +
              "Two players on one PC: the Split-Screen tab.",
              "Клавиши и кнопки мыши — вкладка «Клавиатура». Кнопки и стики геймпада — вкладка «Геймпад». " +
              "Двое игроков на одном ПК — вкладка «Разделённый экран».",
              "Touches et boutons de la souris : onglet Clavier. Boutons et sticks de la manette : onglet Manette. " +
              "Deux joueurs sur un PC : onglet Écran partagé.",
              "Tasten und Maustasten: Reiter Tastatur. Controller-Tasten und Sticks: Reiter Controller. " +
              "Zwei Spieler an einem PC: Reiter Splitscreen.",
              "Teclas y botones del ratón: pestaña Teclado. Botones y sticks del mando: pestaña Mando. " +
              "Dos jugadores en un PC: pestaña Pantalla dividida.",
              "Tasti e pulsanti del mouse: scheda Tastiera. Pulsanti e levette del controller: scheda Controller. " +
              "Due giocatori su un PC: scheda Schermo condiviso.");
            a("Enter or click: press the new key or mouse button (hold Shift / Ctrl / Alt for a combination). " +
              "→ or +: add one more key. Delete or ×: clear. Waiting stops after a few seconds.",
              "Enter или щелчок — нажмите новую клавишу или кнопку мыши (с Shift / Ctrl / Alt — сочетание). " +
              "→ или + — добавить ещё одну клавишу. Delete или × — очистить. Ожидание закончится само через несколько секунд.",
              "Entrée ou clic : appuyez sur la nouvelle touche ou le bouton de la souris (avec Maj / Ctrl / Alt : une combinaison). " +
              "→ ou + : ajouter une touche. Suppr ou × : effacer. L'attente s'arrête au bout de quelques secondes.",
              "Enter oder Klick: die neue Taste oder Maustaste drücken (mit Umschalt / Strg / Alt: eine Kombination). " +
              "→ oder +: eine weitere Taste. Entf oder ×: löschen. Das Warten endet nach ein paar Sekunden.",
              "Intro o clic: pulsa la nueva tecla o botón del ratón (con Mayús / Ctrl / Alt: una combinación). " +
              "→ o +: añadir otra tecla. Supr o ×: borrar. La espera termina a los pocos segundos.",
              "Invio o clic: premi il nuovo tasto o pulsante del mouse (con Maiusc / Ctrl / Alt: una combinazione). " +
              "→ o +: aggiungi un altro tasto. Canc o ×: cancella. L'attesa finisce dopo qualche secondo.");
            a("←→: choose the gamepad control that presses this button in the game. Enter or click: press it " +
              "on the gamepad (Xbox controllers). Applies to every gamepad; the game still shows the Xbox buttons.",
              "←→ — выбрать, какая кнопка геймпада нажимает эту кнопку в игре. Enter или щелчок — нажать её " +
              "на геймпаде (контроллеры Xbox). Действует на все геймпады; в игре по-прежнему показываются кнопки Xbox.",
              "←→ : choisir la commande de la manette qui appuie sur ce bouton dans le jeu. Entrée ou clic : l'appuyer " +
              "sur la manette (manettes Xbox). Vaut pour toutes les manettes ; le jeu affiche toujours les boutons Xbox.",
              "←→: wählen, welches Bedienelement des Controllers diese Taste im Spiel drückt. Enter oder Klick: es " +
              "am Controller drücken (Xbox-Controller). Gilt für alle Controller; das Spiel zeigt weiter die Xbox-Tasten.",
              "←→: elige el control del mando que pulsa este botón en el juego. Intro o clic: púlsalo " +
              "en el mando (mandos de Xbox). Vale para todos los mandos; el juego sigue mostrando los botones de Xbox.",
              "←→: scegli il comando del controller che preme questo pulsante nel gioco. Invio o clic: premilo " +
              "sul controller (controller Xbox). Vale per tutti i controller; il gioco mostra ancora i pulsanti Xbox.");
            a("PRESS A KEY… {0}", "НАЖМИТЕ КЛАВИШУ… {0}", "APPUYEZ SUR UNE TOUCHE… {0}", "TASTE DRÜCKEN… {0}",
              "PULSA UNA TECLA… {0}", "PREMI UN TASTO… {0}");
            a("PRESS A BUTTON… {0}", "НАЖМИТЕ КНОПКУ… {0}", "APPUYEZ SUR UN BOUTON… {0}", "TASTE DRÜCKEN… {0}",
              "PULSA UN BOTÓN… {0}", "PREMI UN PULSANTE… {0}");
            a("LMB", "ЛКМ", "CLIC G", "LMT", "CLIC IZQ", "TASTO SIN");
            a("RMB", "ПКМ", "CLIC D", "RMT", "CLIC DER", "TASTO DES");
            a("MMB", "СКМ", "CLIC M", "MMT", "CLIC CEN", "TASTO CEN");
            a("MOUSE 4", "МЫШЬ 4", "SOURIS 4", "MAUS 4", "RATÓN 4", "MOUSE 4");
            a("MOUSE 5", "МЫШЬ 5", "SOURIS 5", "MAUS 5", "RATÓN 5", "MOUSE 5");
            a("Move forward", "Вперёд", "Avancer", "Vorwärts", "Avanzar", "Avanti");
            a("Move back", "Назад", "Reculer", "Rückwärts", "Retroceder", "Indietro");
            a("Move left", "Влево", "Aller à gauche", "Nach links", "Izquierda", "Sinistra");
            a("Move right", "Вправо", "Aller à droite", "Nach rechts", "Derecha", "Destra");
            a("Crouch (L3)", "Присесть (L3)", "S'accroupir (L3)", "Ducken (L3)", "Agacharse (L3)", "Accovacciati (L3)");
            a("Fire (RT)", "Огонь (RT)", "Tirer (RT)", "Feuern (RT)", "Disparar (RT)", "Fuoco (RT)");
            a("Aim (LT)", "Прицел (LT)", "Viser (LT)", "Zielen (LT)", "Apuntar (LT)", "Mira (LT)");
            a("Reload (X)", "Перезарядка (X)", "Recharger (X)", "Nachladen (X)", "Recargar (X)", "Ricarica (X)");
            a("Roll / cover / vault (Y)", "Кувырок / укрытие / перелезть (Y)", "Roulade / couvert / franchir (Y)",
              "Rolle / Deckung / Überspringen (Y)", "Rodar / cobertura / saltar (Y)", "Capriola / copertura / scavalca (Y)");
            a("Use / heal / confirm (A)", "Действие / лечить / выбор (A)", "Utiliser / soigner / valider (A)",
              "Benutzen / heilen / bestätigen (A)", "Usar / curar / aceptar (A)", "Usa / cura / conferma (A)");
            a("Cancel / back (B)", "Отмена / назад (B)", "Annuler / retour (B)", "Abbrechen / zurück (B)",
              "Cancelar / atrás (B)", "Annulla / indietro (B)");
            a("Inventory (LB)", "Инвентарь (LB)", "Inventaire (LB)", "Inventar (LB)", "Inventario (LB)", "Inventario (LB)");
            a("Co-op commands (RB)", "Кооп-команды (RB)", "Commandes coop (RB)", "Koop-Befehle (RB)",
              "Órdenes cooperativas (RB)", "Comandi cooperativa (RB)");
            a("Right stick press (R3)", "Нажатие правого стика (R3)", "Clic du stick droit (R3)", "Rechter Stick drücken (R3)",
              "Pulsar stick derecho (R3)", "Pressione levetta destra (R3)");
            a("D-pad up: advance / primary weapon", "Крестовина вверх: вперёд / основное оружие",
              "Croix haut : avancer / arme principale", "Steuerkreuz oben: vorrücken / Primärwaffe",
              "Cruceta arriba: avanzar / arma principal", "Croce su: avanza / arma primaria");
            a("D-pad down: partner view / grenades", "Крестовина вниз: вид напарника / гранаты",
              "Croix bas : vue du partenaire / grenades", "Steuerkreuz unten: Partnersicht / Granaten",
              "Cruceta abajo: vista del compañero / granadas", "Croce giù: vista compagno / granate");
            a("D-pad left: regroup / secondary weapon", "Крестовина влево: ко мне / дополнительное оружие",
              "Croix gauche : regroupement / arme secondaire", "Steuerkreuz links: sammeln / Sekundärwaffe",
              "Cruceta izquierda: reagruparse / arma secundaria", "Croce sinistra: raggruppa / arma secondaria");
            a("D-pad right: hold position / special weapon", "Крестовина вправо: стоять / особое оружие",
              "Croix droite : tenir la position / arme spéciale", "Steuerkreuz rechts: Position halten / Spezialwaffe",
              "Cruceta derecha: mantener posición / arma especial", "Croce destra: mantieni posizione / arma speciale");
            a("Look up (right stick)", "Взгляд вверх (правый стик)", "Regarder en haut (stick droit)", "Nach oben sehen (rechter Stick)",
              "Mirar arriba (stick derecho)", "Guarda su (levetta destra)");
            a("Look down (right stick)", "Взгляд вниз (правый стик)", "Regarder en bas (stick droit)", "Nach unten sehen (rechter Stick)",
              "Mirar abajo (stick derecho)", "Guarda giù (levetta destra)");
            a("Look left (right stick)", "Взгляд влево (правый стик)", "Regarder à gauche (stick droit)", "Nach links sehen (rechter Stick)",
              "Mirar a la izquierda (stick derecho)", "Guarda a sinistra (levetta destra)");
            a("Look right (right stick)", "Взгляд вправо (правый стик)", "Regarder à droite (stick droit)", "Nach rechts sehen (rechter Stick)",
              "Mirar a la derecha (stick derecho)", "Guarda a destra (levetta destra)");
            a("GPS (Back)", "GPS (Back)", "GPS (Back)", "GPS (Back)", "GPS (Back)", "GPS (Back)");
            a("Pause (Start)", "Пауза (Start)", "Pause (Start)", "Pause (Start)", "Pausa (Start)", "Pausa (Start)");
            a("Guide button", "Кнопка Guide", "Bouton Guide", "Guide-Taste", "Botón Guía", "Pulsante Guida");
            a("Key bindings", "Назначение клавиш", "Touches", "Tastenbelegung", "Teclas", "Tasti");
            a("Return every key to the default layout (WASD, mouse).", "Вернуть все клавиши по умолчанию (WASD, мышь).",
              "Remettre toutes les touches par défaut (WASD, souris).", "Alle Tasten auf die Standardbelegung (WASD, Maus) zurücksetzen.",
              "Devolver todas las teclas a la distribución por defecto (WASD, ratón).",
              "Riporta tutti i tasti alla disposizione predefinita (WASD, mouse).");
            a("Default keys restored", "Клавиши по умолчанию восстановлены", "Touches par défaut rétablies",
              "Standardtasten wiederhergestellt", "Teclas por defecto restablecidas", "Tasti predefiniti ripristinati");
            a("A: use / confirm", "A: действие / выбор", "A : utiliser / valider", "A: benutzen / bestätigen", "A: usar / aceptar",
              "A: usa / conferma");
            a("B: cancel / back", "B: отмена / назад", "B : annuler / retour", "B: abbrechen / zurück", "B: cancelar / atrás",
              "B: annulla / indietro");
            a("X: reload", "X: перезарядка", "X : recharger", "X: nachladen", "X: recargar", "X: ricarica");
            a("Y: roll / cover / vault", "Y: кувырок / укрытие / перелезть", "Y : roulade / couvert / franchir",
              "Y: Rolle / Deckung / Überspringen", "Y: rodar / cobertura / saltar", "Y: capriola / copertura / scavalca");
            a("RT: fire", "RT: огонь", "RT : tirer", "RT: feuern", "RT: disparar", "RT: fuoco");
            a("LT: aim", "LT: прицел", "LT : viser", "LT: zielen", "LT: apuntar", "LT: mira");
            a("LB: inventory", "LB: инвентарь", "LB : inventaire", "LB: Inventar", "LB: inventario", "LB: inventario");
            a("RB: co-op commands", "RB: кооп-команды", "RB : commandes coop", "RB: Koop-Befehle", "RB: órdenes cooperativas",
              "RB: comandi cooperativa");
            a("L3: crouch", "L3: присесть", "L3 : s'accroupir", "L3: ducken", "L3: agacharse", "L3: accovacciati");
            a("D-pad up", "Крестовина вверх", "Croix haut", "Steuerkreuz oben", "Cruceta arriba", "Croce su");
            a("D-pad down", "Крестовина вниз", "Croix bas", "Steuerkreuz unten", "Cruceta abajo", "Croce giù");
            a("D-pad left", "Крестовина влево", "Croix gauche", "Steuerkreuz links", "Cruceta izquierda", "Croce sinistra");
            a("D-pad right", "Крестовина вправо", "Croix droite", "Steuerkreuz rechts", "Cruceta derecha", "Croce destra");
            a("Back: GPS", "Back: GPS", "Back : GPS", "Back: GPS", "Back: GPS", "Back: GPS");
            a("Start: pause", "Start: пауза", "Start : pause", "Start: Pause", "Start: pausa", "Start: pausa");
            a("LEFT STICK", "ЛЕВЫЙ СТИК", "STICK GAUCHE", "LINKER STICK", "STICK IZQUIERDO", "LEVETTA SINISTRA");
            a("RIGHT STICK", "ПРАВЫЙ СТИК", "STICK DROIT", "RECHTER STICK", "STICK DERECHO", "LEVETTA DESTRA");
            a("Movement (left stick)", "Движение (левый стик)", "Déplacement (stick gauche)", "Bewegung (linker Stick)",
              "Movimiento (stick izquierdo)", "Movimento (levetta sinistra)");
            a("Camera (right stick)", "Камера (правый стик)", "Caméra (stick droit)", "Kamera (rechter Stick)",
              "Cámara (stick derecho)", "Telecamera (levetta destra)");
            a("Which stick of the gamepad moves the character. Swap both to play with the sticks swapped.",
              "Какой стик геймпада двигает персонажа. Поменяйте оба, чтобы играть с переставленными стиками.",
              "Quel stick de la manette déplace le personnage. Échangez les deux pour jouer avec les sticks inversés.",
              "Welcher Stick des Controllers die Figur bewegt. Beide tauschen, um mit vertauschten Sticks zu spielen.",
              "Qué stick del mando mueve al personaje. Cambia los dos para jugar con los sticks intercambiados.",
              "Quale levetta del controller muove il personaggio. Scambia entrambe per giocare con le levette invertite.");
            a("Which stick of the gamepad turns the camera and aims.", "Какой стик геймпада поворачивает камеру и целится.",
              "Quel stick de la manette tourne la caméra et vise.", "Welcher Stick des Controllers die Kamera dreht und zielt.",
              "Qué stick del mando gira la cámara y apunta.", "Quale levetta del controller gira la telecamera e mira.");
            a("Invert movement stick Y", "Инверсия Y стика движения", "Inverser l'axe Y du déplacement", "Bewegungsstick Y umkehren",
              "Invertir eje Y del movimiento", "Inverti asse Y del movimento");
            a("Invert camera stick Y", "Инверсия Y стика камеры", "Inverser l'axe Y de la caméra", "Kamerastick Y umkehren",
              "Invertir eje Y de la cámara", "Inverti asse Y della telecamera");
            a("Up on the movement stick walks backwards.", "Стик движения вверх — идти назад.",
              "Pousser le stick de déplacement vers le haut fait reculer.", "Bewegungsstick nach oben geht rückwärts.",
              "Arriba en el stick de movimiento camina hacia atrás.", "Su sulla levetta di movimento cammina all'indietro.");
            a("Up on the camera stick looks down (on top of the Invert Y option in the game itself).",
              "Стик камеры вверх — взгляд вниз (поверх настройки инверсии Y в самой игре).",
              "Pousser le stick de caméra vers le haut fait regarder en bas (en plus de l'option Inverser Y du jeu).",
              "Kamerastick nach oben sieht nach unten (zusätzlich zur Option Y umkehren im Spiel).",
              "Arriba en el stick de cámara mira abajo (además de la opción Invertir Y del propio juego).",
              "Su sulla levetta della telecamera guarda in basso (oltre all'opzione Inverti Y del gioco).");
            a("Gamepad layout", "Раскладка геймпада", "Disposition de la manette", "Controller-Belegung", "Distribución del mando",
              "Disposizione del controller");
            a("Return every gamepad button to the console layout.", "Вернуть все кнопки геймпада как на консоли.",
              "Remettre tous les boutons de la manette comme sur la console.", "Alle Controller-Tasten auf die Konsolenbelegung zurücksetzen.",
              "Devolver todos los botones del mando a la distribución de la consola.",
              "Riporta tutti i pulsanti del controller alla disposizione della console.");
            a("Default gamepad layout restored", "Раскладка геймпада по умолчанию восстановлена", "Disposition de la manette rétablie",
              "Standard-Controllerbelegung wiederhergestellt", "Distribución del mando restablecida",
              "Disposizione del controller ripristinata");
            a("Split-screen", "Разделённый экран", "Écran partagé", "Splitscreen", "Pantalla dividida", "Schermo condiviso");
            a("Two players on one PC, each with their own half of the screen. On Play the game starts at once, without " +
              "looking for a network host. Off: every device controls player 1.",
              "Двое игроков на одном ПК, у каждого своя половина экрана. По «Играть» игра запускается сразу, без " +
              "поиска хоста в сети. Выкл.: все устройства управляют игроком 1.",
              "Deux joueurs sur un PC, chacun avec sa moitié de l'écran. Jouer lance le jeu tout de suite, sans " +
              "chercher d'hôte sur le réseau. Désactivé : tous les périphériques contrôlent le joueur 1.",
              "Zwei Spieler an einem PC, jeder mit seiner Bildschirmhälfte. Spielen startet das Spiel sofort, ohne " +
              "einen Host im Netzwerk zu suchen. Aus: alle Geräte steuern Spieler 1.",
              "Dos jugadores en un PC, cada uno con su mitad de la pantalla. Jugar inicia el juego enseguida, sin " +
              "buscar un anfitrión en la red. Desactivado: todos los dispositivos controlan al jugador 1.",
              "Due giocatori su un PC, ognuno con la sua metà dello schermo. Gioca avvia subito il gioco, senza " +
              "cercare un host in rete. Disattivato: tutti i dispositivi controllano il giocatore 1.");
            a("Player 1 name", "Имя игрока 1", "Nom du joueur 1", "Name von Spieler 1", "Nombre del jugador 1", "Nome giocatore 1");
            a("Player 2 name", "Имя игрока 2", "Nom du joueur 2", "Name von Spieler 2", "Nombre del jugador 2", "Nome giocatore 2");
            a("Gamertag of player 1 (up to 15 characters), the same as Network → Player name. Empty: the computer's name.",
              "Геймертег игрока 1 (до 15 символов), то же, что «Сеть → Имя игрока». Пусто — имя компьютера.",
              "Gamertag du joueur 1 (15 caractères max.), le même que Réseau → Nom du joueur. Vide : le nom de l'ordinateur.",
              "Gamertag von Spieler 1 (bis 15 Zeichen), wie Netzwerk → Spielername. Leer: der Name des Computers.",
              "Gamertag del jugador 1 (hasta 15 caracteres), el mismo que Red → Nombre del jugador. Vacío: el nombre del PC.",
              "Gamertag del giocatore 1 (fino a 15 caratteri), lo stesso di Rete → Nome giocatore. Vuoto: il nome del computer.");
            a("Gamertag of player 2 (up to 15 characters); their own profile and settings. " +
              "Empty: player 1's name with (1) after it.",
              "Геймертег игрока 2 (до 15 символов); у него свой профиль и настройки. " +
              "Пусто — имя игрока 1 с (1) в конце.",
              "Gamertag du joueur 2 (15 caractères max.) ; son propre profil et ses réglages. " +
              "Vide : le nom du joueur 1 suivi de (1).",
              "Gamertag von Spieler 2 (bis 15 Zeichen); eigenes Profil und eigene Einstellungen. " +
              "Leer: der Name von Spieler 1 mit (1) dahinter.",
              "Gamertag del jugador 2 (hasta 15 caracteres); su propio perfil y ajustes. " +
              "Vacío: el nombre del jugador 1 con (1) detrás.",
              "Gamertag del giocatore 2 (fino a 15 caratteri); il suo profilo e le sue impostazioni. " +
              "Vuoto: il nome del giocatore 1 seguito da (1).");
            a("Player 1 controls", "Управление игрока 1", "Commandes du joueur 1", "Steuerung von Spieler 1",
              "Controles del jugador 1", "Comandi del giocatore 1");
            a("Player 2 controls", "Управление игрока 2", "Commandes du joueur 2", "Steuerung von Spieler 2",
              "Controles del jugador 2", "Comandi del giocatore 2");
            a("The devices of player 1. Gamepads are numbered in the order the game finds them (usually the order " +
              "they were connected): if the players' pads turn out swapped, swap them here. " +
              "Any gamepad: every gamepad the other player does not have.",
              "Устройства игрока 1. Геймпады нумеруются в том порядке, в каком их находит игра (обычно в порядке " +
              "подключения): если геймпады игроков перепутались, поменяйте их здесь. " +
              "Любой геймпад — все геймпады, которых нет у другого игрока.",
              "Les périphériques du joueur 1. Les manettes sont numérotées dans l'ordre où le jeu les trouve (en général l'ordre " +
              "de branchement) : si les manettes des joueurs sont inversées, échangez-les ici. " +
              "N'importe quelle manette : toutes celles que l'autre joueur n'a pas.",
              "Die Geräte von Spieler 1. Controller werden in der Reihenfolge nummeriert, in der das Spiel sie findet (meist die " +
              "Reihenfolge des Anschließens): Sind die Controller vertauscht, hier tauschen. " +
              "Beliebiger Controller: jeder, den der andere Spieler nicht hat.",
              "Los dispositivos del jugador 1. Los mandos se numeran en el orden en que el juego los encuentra (normalmente el " +
              "orden de conexión): si los mandos de los jugadores salen cambiados, cámbialos aquí. " +
              "Cualquier mando: todos los que no tiene el otro jugador.",
              "I dispositivi del giocatore 1. I controller sono numerati nell'ordine in cui il gioco li trova (di solito l'ordine " +
              "di collegamento): se i controller dei giocatori risultano scambiati, scambiali qui. " +
              "Qualsiasi controller: tutti quelli che l'altro giocatore non ha.");
            a("The devices of player 2. The mouse aims for whoever has the keyboard. " +
              "Any gamepad: every gamepad the other player does not have.",
              "Устройства игрока 2. Мышь целится за того, у кого клавиатура. " +
              "Любой геймпад — все геймпады, которых нет у другого игрока.",
              "Les périphériques du joueur 2. La souris vise pour celui qui a le clavier. " +
              "N'importe quelle manette : toutes celles que l'autre joueur n'a pas.",
              "Die Geräte von Spieler 2. Die Maus zielt für den, der die Tastatur hat. " +
              "Beliebiger Controller: jeder, den der andere Spieler nicht hat.",
              "Los dispositivos del jugador 2. El ratón apunta para quien tiene el teclado. " +
              "Cualquier mando: todos los que no tiene el otro jugador.",
              "I dispositivi del giocatore 2. Il mouse mira per chi ha la tastiera. " +
              "Qualsiasi controller: tutti quelli che l'altro giocatore non ha.");
            a("KEYBOARD + MOUSE", "КЛАВ. + МЫШЬ", "CLAVIER + SOURIS", "TASTATUR + MAUS", "TECLADO + RATÓN", "TASTIERA + MOUSE");
            a("ANY GAMEPAD", "ЛЮБОЙ ГЕЙМПАД", "TOUTE MANETTE", "JEDER CONTROLLER", "CUALQUIER MANDO", "QUALSIASI CONTROLLER");
            a("GAMEPAD 1", "ГЕЙМПАД 1", "MANETTE 1", "CONTROLLER 1", "MANDO 1", "CONTROLLER 1");
            a("GAMEPAD 2", "ГЕЙМПАД 2", "MANETTE 2", "CONTROLLER 2", "MANDO 2", "CONTROLLER 2");
            a("GAMEPAD 3", "ГЕЙМПАД 3", "MANETTE 3", "CONTROLLER 3", "MANDO 3", "CONTROLLER 3");
            a("GAMEPAD 4", "ГЕЙМПАД 4", "MANETTE 4", "CONTROLLER 4", "MANDO 4", "CONTROLLER 4");
            a("KEYBOARD + ANY PAD", "КЛАВ. + ЛЮБОЙ", "CLAVIER + MANETTE", "TASTATUR + CONTR.", "TECLADO + MANDO", "TASTIERA + CONTR.");
            a("KEYBOARD + PAD 1", "КЛАВ. + ГЕЙМПАД 1", "CLAVIER + MANETTE 1", "TASTATUR + CONTR. 1", "TECLADO + MANDO 1",
              "TASTIERA + CONTR. 1");
            a("KEYBOARD + PAD 2", "КЛАВ. + ГЕЙМПАД 2", "CLAVIER + MANETTE 2", "TASTATUR + CONTR. 2", "TECLADO + MANDO 2",
              "TASTIERA + CONTR. 2");
            a("GAMEPAD {0}", "ГЕЙМПАД {0}", "MANETTE {0}", "CONTROLLER {0}", "MANDO {0}", "CONTROLLER {0}");
            a("KEYBOARD + PAD {0}", "КЛАВ. + ГЕЙМПАД {0}", "CLAVIER + MANETTE {0}", "TASTATUR + CONTR. {0}", "TECLADO + MANDO {0}",
              "TASTIERA + CONTR. {0}");
            a("not connected", "не подключён", "non branchée", "nicht angeschlossen", "no conectado", "non collegato");
            a("Game controller", "Игровой контроллер", "Manette de jeu", "Gamecontroller", "Mando de juego", "Controller di gioco");
            a("No gamepads connected.", "Геймпады не подключены.", "Aucune manette branchée.", "Keine Controller angeschlossen.",
              "No hay mandos conectados.", "Nessun controller collegato.");
            a("Connected:", "Подключены:", "Branchées :", "Angeschlossen:", "Conectados:", "Collegati:");
            a("gamepad {0} - {1}", "геймпад {0} — {1}", "manette {0} - {1}", "Controller {0} - {1}", "mando {0} - {1}",
              "controller {0} - {1}");
            a("Both players have the keyboard: it goes to player 1.", "Клавиатура у обоих игроков — она достанется игроку 1.",
              "Les deux joueurs ont le clavier : il revient au joueur 1.", "Beide Spieler haben die Tastatur: sie geht an Spieler 1.",
              "Los dos jugadores tienen el teclado: será del jugador 1.", "Entrambi i giocatori hanno la tastiera: va al giocatore 1.");
            a("Both players have gamepad {0}: it goes to player 1.", "Геймпад {0} у обоих игроков — он достанется игроку 1.",
              "Les deux joueurs ont la manette {0} : elle revient au joueur 1.", "Beide Spieler haben Controller {0}: er geht an Spieler 1.",
              "Los dos jugadores tienen el mando {0}: será del jugador 1.", "Entrambi i giocatori hanno il controller {0}: va al giocatore 1.");
            a("Any gamepad for both players: they all go to player 1.", "«Любой геймпад» у обоих — все геймпады достанутся игроку 1.",
              "Toute manette pour les deux joueurs : elles reviennent toutes au joueur 1.",
              "Jeder Controller für beide Spieler: alle gehen an Spieler 1.",
              "Cualquier mando para los dos jugadores: todos serán del jugador 1.",
              "Qualsiasi controller per entrambi: vanno tutti al giocatore 1.");
            a("Gamepad {0} is not connected.", "Геймпад {0} не подключён.", "La manette {0} n'est pas branchée.",
              "Controller {0} ist nicht angeschlossen.", "El mando {0} no está conectado.", "Il controller {0} non è collegato.");
            a("In the game: Campaign Mode → Splitscreen; player 2 signs in with their device when asked.",
              "В игре: «Кампания» → «Разделенный экран»; игрок 2 входит со своего устройства, когда игра попросит.",
              "Dans le jeu : Mode Campagne → Écran partagé ; le joueur 2 se connecte avec son périphérique quand le jeu le demande.",
              "Im Spiel: Kampagnenmodus → Splitscreen; Spieler 2 meldet sich mit seinem Gerät an, wenn das Spiel fragt.",
              "En el juego: Modo Campaña → Pantalla dividida; el jugador 2 inicia sesión con su dispositivo cuando se le pida.",
              "Nel gioco: Modalità Campagna → Schermo condiviso; il giocatore 2 accede con il suo dispositivo quando richiesto.");
            a("Starting split-screen", "Запуск на разделённом экране", "Lancement en écran partagé", "Splitscreen wird gestartet",
              "Iniciando pantalla dividida", "Avvio a schermo condiviso");

            // --- Network ---
            a("Player name", "Имя игрока", "Nom du joueur", "Spielername", "Nombre del jugador", "Nome giocatore");
            a("The name your partner sees (up to 15 characters). Empty: the computer's name.",
              "Имя, которое видит напарник (до 15 символов). Пусто — имя компьютера.",
              "Le nom que voit votre partenaire (15 caractères max.). Vide : le nom de l'ordinateur.",
              "Der Name, den dein Partner sieht (bis zu 15 Zeichen). Leer: der Name des Computers.",
              "El nombre que ve tu compañero (hasta 15 caracteres). Vacío: el nombre del equipo.",
              "Il nome che vede il tuo compagno (fino a 15 caratteri). Vuoto: il nome del computer.");
            a("Host IP", "IP хоста", "IP de l'hôte", "Host-IP", "IP del anfitrión", "IP dell'host");
            a("Leave empty: the host is found automatically on the local network or Radmin VPN. " +
              "Enter an address only if the host is not found, for example over the internet.",
              "Оставьте пустым — хост найдётся сам в локальной сети или Radmin VPN. Вводите адрес, только если хост " +
              "не находится, например через интернет.",
              "Laissez vide : l'hôte est trouvé automatiquement sur le réseau local ou Radmin VPN. Saisissez une " +
              "adresse seulement si l'hôte n'est pas trouvé, par exemple via Internet.",
              "Leer lassen: Der Host wird im lokalen Netz oder in Radmin VPN automatisch gefunden. Eine Adresse nur " +
              "eingeben, wenn der Host nicht gefunden wird, etwa über das Internet.",
              "Déjalo vacío: el anfitrión se encuentra solo en la red local o en Radmin VPN. Escribe una dirección " +
              "solo si no se encuentra, por ejemplo por internet.",
              "Lascia vuoto: l'host viene trovato da solo nella rete locale o in Radmin VPN. Inserisci un indirizzo " +
              "solo se l'host non viene trovato, ad esempio via internet.");
            a("Input delay margin", "Запас задержки ввода", "Marge de latence", "Eingabepuffer", "Margen de retardo",
              "Margine di ritardo");
            a("Added to the network delay. 0 is the minimum. If the game stutters waiting for the partner on an unstable " +
              "connection, set 16-50. Must be the same for both players.",
              "Добавляется к сетевой задержке. 0 — минимум. Если на нестабильном соединении игра подтормаживает в " +
              "ожидании напарника, поставьте 16-50. Должен совпадать у обоих игроков.",
              "S'ajoute à la latence réseau. 0 est le minimum. Si le jeu saccade en attendant le partenaire sur une " +
              "connexion instable, mettez 16-50. Doit être identique chez les deux joueurs.",
              "Wird zur Netzwerkverzögerung addiert. 0 ist das Minimum. Wenn das Spiel bei instabiler Verbindung beim " +
              "Warten auf den Partner ruckelt, 16-50 einstellen. Muss bei beiden Spielern gleich sein.",
              "Se suma al retardo de red. 0 es el mínimo. Si el juego da tirones esperando al compañero con una " +
              "conexión inestable, pon 16-50. Debe ser igual para los dos jugadores.",
              "Si aggiunge al ritardo di rete. 0 è il minimo. Se il gioco scatta aspettando il compagno su una " +
              "connessione instabile, imposta 16-50. Deve essere uguale per entrambi i giocatori.");
            a("Windows Firewall", "Брандмауэр Windows", "Pare-feu Windows", "Windows-Firewall", "Firewall de Windows",
              "Firewall di Windows");
            a("ALLOW", "РАЗРЕШИТЬ", "AUTORISER", "ZULASSEN", "PERMITIR", "CONSENTI");
            a("Allow incoming connections to the game on every network type (Windows treats Radmin as public). " +
              "Needs administrator rights: Windows will ask to confirm. Done once on both PCs.",
              "Разрешить входящие подключения к игре во всех типах сетей (Radmin Windows считает общественной). " +
              "Нужны права администратора — Windows попросит подтвердить. Делается один раз на обоих ПК.",
              "Autoriser les connexions entrantes vers le jeu sur tous les types de réseau (Windows considère Radmin " +
              "comme public). Nécessite les droits d'administrateur : Windows demandera une confirmation. À faire une " +
              "fois sur les deux PC.",
              "Eingehende Verbindungen zum Spiel in allen Netzwerktypen zulassen (Windows behandelt Radmin als " +
              "öffentlich). Benötigt Administratorrechte: Windows fragt nach. Einmal auf beiden PCs.",
              "Permitir conexiones entrantes al juego en todos los tipos de red (Windows trata Radmin como pública). " +
              "Requiere permisos de administrador: Windows pedirá confirmación. Se hace una vez en ambos PC.",
              "Consenti le connessioni in entrata al gioco su ogni tipo di rete (Windows considera Radmin pubblica). " +
              "Servono i diritti di amministratore: Windows chiederà conferma. Va fatto una volta su entrambi i PC.");
            a("Co-op log", "Лог кооператива", "Journal coop", "Koop-Protokoll", "Registro cooperativo", "Registro cooperativa");
            a("Writes coop_debug.log in the game folder, needed to investigate desyncs. Costs a little performance.",
              "Пишет coop_debug.log в папку игры — нужен для разбора рассинхронов. Немного снижает производительность.",
              "Écrit coop_debug.log dans le dossier du jeu, utile pour analyser les désynchronisations. Coûte un peu " +
              "de performances.",
              "Schreibt coop_debug.log in den Spielordner, nötig zur Untersuchung von Desyncs. Kostet etwas Leistung.",
              "Escribe coop_debug.log en la carpeta del juego; sirve para investigar desincronizaciones. Cuesta algo " +
              "de rendimiento.",
              "Scrive coop_debug.log nella cartella del gioco, utile per analizzare le desincronizzazioni. Costa un " +
              "po' di prestazioni.");
            // The menu names as the game shows them in each language.
            a("The role is chosen automatically: on Play the launcher looks for a host on the local network. If one is " +
              "found you join it, otherwise you become the host. The second player presses Play a few seconds after " +
              "the first player's game has started. Then both, in the game: Campaign Mode → Public Co-op Game.",
              "Роль выбирается сама: по кнопке «Играть» лаунчер ищет хост в локальной сети. Если находит, вы " +
              "подключаетесь к нему, иначе становитесь хостом. Второй игрок жмёт «Играть» через несколько секунд " +
              "после того, как запустилась игра первого. Затем оба в игре: Кампания → Открытая игра.",
              "Le rôle est choisi automatiquement : avec Jouer, le lanceur cherche un hôte sur le réseau local. S'il " +
              "en trouve un, vous le rejoignez, sinon vous devenez l'hôte. Le second joueur appuie sur Jouer quelques " +
              "secondes après le lancement du jeu du premier. Puis tous les deux, dans le jeu : Mode campagne → " +
              "Partie publique coop.",
              "Die Rolle wird automatisch gewählt: Bei Spielen sucht der Launcher einen Host im lokalen Netz. Wird " +
              "einer gefunden, trittst du bei, sonst wirst du Host. Der zweite Spieler drückt Spielen ein paar " +
              "Sekunden nach dem Start des Spiels des ersten. Dann beide im Spiel: Kampagnenmodus → Öffentl. Coop-Spiel.",
              "El papel se elige solo: al pulsar Jugar, el lanzador busca un anfitrión en la red local. Si lo " +
              "encuentra, te unes; si no, eres el anfitrión. El segundo jugador pulsa Jugar unos segundos después de " +
              "que se inicie el juego del primero. Luego ambos, en el juego: Modo campaña → Partida Coop. Pública.",
              "Il ruolo si sceglie da solo: con Gioca il launcher cerca un host nella rete locale. Se lo trova ti " +
              "unisci, altrimenti diventi l'host. Il secondo giocatore preme Gioca qualche secondo dopo l'avvio del " +
              "gioco del primo. Poi entrambi, nel gioco: Modalità campagna → Partita co-op pubblica.");

            // --- Advanced ---
            a("Skip cutscenes", "Пропуск роликов", "Passer les cinématiques", "Zwischensequenzen überspringen",
              "Saltar cinemáticas", "Salta filmati");
            a("Enter or Space skips movies and in-engine cutscenes. Skipping is off in network games.",
              "Enter или Пробел пропускают видеоролики и катсцены на движке. В сетевой игре пропуск выключен.",
              "Entrée ou Espace passe les vidéos et les cinématiques du moteur. Désactivé en partie en réseau.",
              "Enter oder Leertaste überspringt Videos und Zwischensequenzen. In Netzwerkspielen aus.",
              "Intro o Espacio salta los vídeos y las cinemáticas del motor. Desactivado en partidas en red.",
              "Invio o Spazio salta i filmati e le scene del motore. Disattivato nelle partite in rete.");
            a("Skip unskippable ones", "Пропускать непропускаемые", "Même non passables", "Auch nicht überspringbare",
              "Saltar las no saltables", "Anche quelli non saltabili");
            a("Also allow skipping the cutscenes the original game does not let you skip.",
              "Разрешить пропуск и тех роликов, которые оригинальная игра пропускать не даёт.",
              "Permet aussi de passer les cinématiques que le jeu original ne laisse pas passer.",
              "Auch Zwischensequenzen überspringen, die das Originalspiel nicht überspringen lässt.",
              "Permite saltar también las cinemáticas que el juego original no deja saltar.",
              "Permette di saltare anche i filmati che il gioco originale non lascia saltare.");
            a("God mode (cheat)", "Бессмертие (чит)", "Mode dieu (triche)", "Gottmodus (Cheat)", "Modo dios (truco)",
              "Modalità dio (trucco)");
            a("The player takes no damage. Has no effect in online co-op.",
              "Игрок не получает урона. В сетевом кооперативе не действует.",
              "Le joueur ne subit aucun dégât. Sans effet en coop en ligne.",
              "Der Spieler erleidet keinen Schaden. Wirkt nicht im Online-Koop.",
              "El jugador no recibe daño. No funciona en el cooperativo en línea.",
              "Il giocatore non subisce danni. Non ha effetto nella cooperativa online.");
            a("Close the launcher", "Закрывать лаунчер", "Fermer le lanceur", "Launcher schließen", "Cerrar el lanzador",
              "Chiudi il launcher");
            a("Close this window once the game has started.", "Закрывать это окно после запуска игры.",
              "Fermer cette fenêtre une fois le jeu lancé.", "Dieses Fenster nach dem Spielstart schließen.",
              "Cerrar esta ventana cuando el juego se haya iniciado.", "Chiudi questa finestra dopo l'avvio del gioco.");
            a("Extra arguments", "Доп. аргументы", "Arguments en plus", "Zusätzliche Argumente", "Argumentos extra",
              "Argomenti extra");
            a("Extra command line arguments for the game, appended at the end (for advanced users).",
              "Дополнительные аргументы командной строки игры, добавляются в конец (для опытных).",
              "Arguments de ligne de commande supplémentaires pour le jeu, ajoutés à la fin (utilisateurs avancés).",
              "Zusätzliche Kommandozeilenargumente für das Spiel, am Ende angehängt (für Fortgeschrittene).",
              "Argumentos de línea de comandos adicionales para el juego, añadidos al final (usuarios avanzados).",
              "Argomenti aggiuntivi della riga di comando per il gioco, aggiunti alla fine (utenti esperti).");
            a("Game folder", "Папка игры", "Dossier du jeu", "Spielordner", "Carpeta del juego", "Cartella del gioco");
            a("OPEN", "ОТКРЫТЬ", "OUVRIR", "ÖFFNEN", "ABRIR", "APRI");
            a("Game not found", "Игра не найдена", "Jeu introuvable", "Spiel nicht gefunden", "Juego no encontrado",
              "Gioco non trovato");
            a("Open the game folder in Explorer (logs, settings, shader cache).",
              "Открыть папку игры в проводнике (логи, настройки, кэш шейдеров).",
              "Ouvrir le dossier du jeu dans l'Explorateur (journaux, paramètres, cache de shaders).",
              "Den Spielordner im Explorer öffnen (Logs, Einstellungen, Shader-Cache).",
              "Abrir la carpeta del juego en el Explorador (registros, ajustes, caché de shaders).",
              "Apri la cartella del gioco in Esplora file (log, impostazioni, cache degli shader).");
            a("Saves", "Сохранения", "Sauvegardes", "Spielstände", "Partidas guardadas", "Salvataggi");
            a("No saves yet: {0}", "Сохранений пока нет: {0}", "Pas encore de sauvegardes : {0}", "Noch keine Spielstände: {0}",
              "Aún no hay partidas guardadas: {0}", "Ancora nessun salvataggio: {0}");
            a("Saves and profile folder: Documents\\armyoftworecomp_tu2 (or armyoftworecomp).",
              "Папка сохранений и профиля: Documents\\armyoftworecomp_tu2 (или armyoftworecomp).",
              "Dossier des sauvegardes et du profil : Documents\\armyoftworecomp_tu2 (ou armyoftworecomp).",
              "Ordner für Spielstände und Profil: Documents\\armyoftworecomp_tu2 (oder armyoftworecomp).",
              "Carpeta de partidas y perfil: Documents\\armyoftworecomp_tu2 (o armyoftworecomp).",
              "Cartella dei salvataggi e del profilo: Documents\\armyoftworecomp_tu2 (o armyoftworecomp).");
            a("Game files and DLC", "Файлы игры и DLC", "Fichiers du jeu et DLC", "Spieldateien und DLC",
              "Archivos del juego y DLC", "File del gioco e DLC");
            a("Install the game from your own disc image, Title Update 2, or add downloadable content (DLC).",
              "Установить игру со своего образа диска и Title Update 2 или добавить загружаемый контент (DLC).",
              "Installer le jeu depuis votre propre image disque et le Title Update 2, ou ajouter du contenu " +
              "téléchargeable (DLC).",
              "Das Spiel aus deinem eigenen Disc-Image und Title Update 2 installieren oder Zusatzinhalte (DLC) hinzufügen.",
              "Instalar el juego desde tu propia imagen de disco y el Title Update 2, o añadir contenido descargable (DLC).",
              "Installa il gioco dalla tua immagine disco e dal Title Update 2, o aggiungi contenuti scaricabili (DLC).");
            a("Desktop shortcut", "Ярлык на рабочем столе", "Raccourci sur le bureau", "Desktop-Verknüpfung",
              "Acceso directo", "Collegamento sul desktop");
            a("CREATE", "СОЗДАТЬ", "CRÉER", "ERSTELLEN", "CREAR", "CREA");
            a("Shortcut created on the desktop", "Ярлык создан на рабочем столе", "Raccourci créé sur le bureau",
              "Verknüpfung auf dem Desktop erstellt", "Acceso directo creado en el escritorio", "Collegamento creato sul desktop");
            a("Could not create the shortcut", "Не удалось создать ярлык", "Impossible de créer le raccourci",
              "Verknüpfung konnte nicht erstellt werden", "No se pudo crear el acceso directo", "Impossibile creare il collegamento");
            a("Create an \"Army of Two\" shortcut on the desktop that starts this launcher.",
              "Создать на рабочем столе ярлык «Army of Two», запускающий этот лаунчер.",
              "Créer sur le bureau un raccourci « Army of Two » qui lance ce lanceur.",
              "Eine Verknüpfung „Army of Two“ auf dem Desktop erstellen, die diesen Launcher startet.",
              "Crear en el escritorio un acceso directo «Army of Two» que abre este lanzador.",
              "Crea sul desktop un collegamento «Army of Two» che avvia questo launcher.");
            a("Game path", "Путь к игре", "Chemin du jeu", "Spielpfad", "Ruta del juego", "Percorso del gioco");
            a("CHANGE", "ИЗМЕНИТЬ", "CHANGER", "ÄNDERN", "CAMBIAR", "CAMBIA");
            a("Point to armyoftworecomp.exe by hand if the launcher is not in the game folder.",
              "Указать armyoftworecomp.exe вручную, если лаунчер лежит не в папке игры.",
              "Indiquer armyoftworecomp.exe manuellement si le lanceur n'est pas dans le dossier du jeu.",
              "armyoftworecomp.exe von Hand wählen, wenn der Launcher nicht im Spielordner liegt.",
              "Indicar armyoftworecomp.exe a mano si el lanzador no está en la carpeta del juego.",
              "Indica armyoftworecomp.exe a mano se il launcher non è nella cartella del gioco.");
            a("Launcher settings", "Настройки лаунчера", "Paramètres du lanceur", "Launcher-Einstellungen",
              "Ajustes del lanzador", "Impostazioni launcher");
            a("RESET", "СБРОСИТЬ", "RÉINITIALISER", "ZURÜCKSETZEN", "RESTABLECER", "RIPRISTINA");
            a("Settings reset to defaults", "Настройки сброшены", "Paramètres réinitialisés", "Einstellungen zurückgesetzt",
              "Ajustes restablecidos", "Impostazioni ripristinate");
            a("Return every launcher setting to its default. Game files are not changed.",
              "Вернуть все настройки лаунчера к значениям по умолчанию. Файлы игры не меняются.",
              "Remettre tous les paramètres du lanceur par défaut. Les fichiers du jeu ne sont pas modifiés.",
              "Alle Launcher-Einstellungen auf Standard zurücksetzen. Die Spieldateien bleiben unverändert.",
              "Devolver todos los ajustes del lanzador a sus valores predeterminados. Los archivos del juego no cambian.",
              "Riporta ogni impostazione del launcher al valore predefinito. I file del gioco non cambiano.");

            // --- Messages ---
            a("Where is armyoftworecomp.exe?", "Где armyoftworecomp.exe?", "Où se trouve armyoftworecomp.exe ?",
              "Wo ist armyoftworecomp.exe?", "¿Dónde está armyoftworecomp.exe?", "Dov'è armyoftworecomp.exe?");
            a("Programs", "Программы", "Programmes", "Programme", "Programas", "Programmi");
            a("Set the game path first", "Сначала укажите путь к игре", "Indiquez d'abord le chemin du jeu",
              "Zuerst den Spielpfad festlegen", "Primero indica la ruta del juego", "Prima indica il percorso del gioco");
            a("Done: incoming connections to the game are allowed", "Готово: входящие подключения к игре разрешены",
              "Terminé : les connexions entrantes vers le jeu sont autorisées",
              "Fertig: Eingehende Verbindungen zum Spiel sind zugelassen",
              "Hecho: se permiten las conexiones entrantes al juego",
              "Fatto: le connessioni in entrata al gioco sono consentite");
            a("Failed (the administrator prompt has to be confirmed)", "Не вышло (нужно подтвердить запрос администратора)",
              "Échec (la demande d'administrateur doit être confirmée)",
              "Fehlgeschlagen (die Administratorabfrage muss bestätigt werden)",
              "Error (hay que confirmar la solicitud de administrador)",
              "Non riuscito (bisogna confermare la richiesta di amministratore)");
            a("The game is already running", "Игра уже запущена", "Le jeu est déjà lancé", "Das Spiel läuft bereits",
              "El juego ya está en marcha", "Il gioco è già in esecuzione");
            a("Could not start the game: {0}", "Не удалось запустить игру: {0}", "Impossible de lancer le jeu : {0}",
              "Spiel konnte nicht gestartet werden: {0}", "No se pudo iniciar el juego: {0}", "Impossibile avviare il gioco: {0}");
            a("Host found: {0} - joining", "Хост найден: {0} — подключаемся", "Hôte trouvé : {0} - connexion",
              "Host gefunden: {0} - verbinde", "Anfitrión encontrado: {0} - uniéndose", "Host trovato: {0} - connessione");
            a("No host found - hosting a game", "Хост не найден — создаём игру", "Aucun hôte trouvé - vous hébergez la partie",
              "Kein Host gefunden - du hostest das Spiel", "Ningún anfitrión - creas la partida", "Nessun host trovato - ospiti la partita");

            // --- Installation ---
            a("GAME INSTALLATION", "УСТАНОВКА ИГРЫ", "INSTALLATION DU JEU", "SPIELINSTALLATION", "INSTALACIÓN DEL JUEGO",
              "INSTALLAZIONE DEL GIOCO");
            a("The launcher contains no game files: you need your own copy of Army of Two for Xbox 360 and " +
              "Title Update 2. Choose the files below or drop them on this window.",
              "В лаунчере нет файлов игры: нужна ваша собственная копия Army of Two для Xbox 360 и Title Update 2. " +
              "Выберите файлы ниже или перетащите их в это окно.",
              "Le lanceur ne contient aucun fichier du jeu : il vous faut votre propre exemplaire d'Army of Two pour " +
              "Xbox 360 et le Title Update 2. Choisissez les fichiers ci-dessous ou déposez-les sur cette fenêtre.",
              "Der Launcher enthält keine Spieldateien: Du brauchst deine eigene Kopie von Army of Two für Xbox 360 " +
              "und Title Update 2. Wähle die Dateien unten oder ziehe sie auf dieses Fenster.",
              "El lanzador no incluye archivos del juego: necesitas tu propia copia de Army of Two para Xbox 360 y el " +
              "Title Update 2. Elige los archivos abajo o arrástralos a esta ventana.",
              "Il launcher non contiene file del gioco: serve la tua copia di Army of Two per Xbox 360 e il Title " +
              "Update 2. Scegli i file qui sotto o trascinali su questa finestra.");
            a("Game", "Игра", "Jeu", "Spiel", "Juego", "Gioco");
            a("game", "игра", "jeu", "Spiel", "juego", "gioco");
            a("installed", "установлено", "installé", "installiert", "instalado", "installato");
            a("not chosen - a disc image (.iso) or the game folder", "не выбрано — образ диска (.iso) или папка игры",
              "non choisi - une image disque (.iso) ou le dossier du jeu",
              "nicht gewählt - ein Disc-Image (.iso) oder der Spielordner",
              "sin elegir - una imagen de disco (.iso) o la carpeta del juego",
              "non scelto - un'immagine disco (.iso) o la cartella del gioco");
            a("not chosen - required", "не выбрано — обязательно", "non choisi - obligatoire", "nicht gewählt - erforderlich",
              "sin elegir - obligatorio", "non scelto - obbligatorio");
            a("Downloadable content (DLC)", "Загружаемый контент (DLC)", "Contenu téléchargeable (DLC)", "Zusatzinhalte (DLC)",
              "Contenido descargable (DLC)", "Contenuti scaricabili (DLC)");
            a("packages installed: {0}", "установлено пакетов: {0}", "paquets installés : {0}", "installierte Pakete: {0}",
              "paquetes instalados: {0}", "pacchetti installati: {0}");
            a("none chosen - can be added later", "не выбрано — можно добавить позже", "aucun choisi - ajoutable plus tard",
              "nichts gewählt - später möglich", "nada elegido - se puede añadir después", "nessuno scelto - si può aggiungere dopo");
            a("created after installation", "создаётся после установки", "créé après l'installation",
              "wird nach der Installation erstellt", "se crea tras la instalación", "creato dopo l'installazione");
            a("CHOOSE", "ВЫБРАТЬ", "CHOISIR", "WÄHLEN", "ELEGIR", "SCEGLI");
            a("ADD", "ДОБАВИТЬ", "AJOUTER", "HINZUFÜGEN", "AÑADIR", "AGGIUNGI");
            a("CANCELLING…", "ОТМЕНА…", "ANNULATION…", "ABBRUCH…", "CANCELANDO…", "ANNULLAMENTO…");
            a("CANCEL", "ОТМЕНИТЬ", "ANNULER", "ABBRECHEN", "CANCELAR", "ANNULLA");
            a("EVERYTHING IS INSTALLED", "ВСЁ УСТАНОВЛЕНО", "TOUT EST INSTALLÉ", "ALLES INSTALLIERT", "TODO INSTALADO",
              "TUTTO INSTALLATO");
            a("Added: {0}", "Добавлено: {0}", "Ajouté : {0}", "Hinzugefügt: {0}", "Añadido: {0}", "Aggiunto: {0}");
            a("Army of Two game image (Xbox 360)", "Образ игры Army of Two (Xbox 360)", "Image du jeu Army of Two (Xbox 360)",
              "Spiel-Image von Army of Two (Xbox 360)", "Imagen del juego Army of Two (Xbox 360)",
              "Immagine del gioco Army of Two (Xbox 360)");
            a("Disc image", "Образ диска", "Image disque", "Disc-Image", "Imagen de disco", "Immagine disco");
            a("Extracted game", "Распакованная игра", "Jeu extrait", "Entpacktes Spiel", "Juego extraído", "Gioco estratto");
            a("All files", "Все файлы", "Tous les fichiers", "Alle Dateien", "Todos los archivos", "Tutti i file");
            a("DLC packages", "Пакеты DLC", "Paquets DLC", "DLC-Pakete", "Paquetes DLC", "Pacchetti DLC");
            a("Preparing", "Подготовка", "Préparation", "Vorbereitung", "Preparando", "Preparazione");
            a("Installation cancelled", "Установка отменена", "Installation annulée", "Installation abgebrochen",
              "Instalación cancelada", "Installazione annullata");
            a("Game installed, shortcut created on the desktop", "Игра установлена, ярлык создан на рабочем столе",
              "Jeu installé, raccourci créé sur le bureau", "Spiel installiert, Verknüpfung auf dem Desktop erstellt",
              "Juego instalado, acceso directo creado en el escritorio", "Gioco installato, collegamento creato sul desktop");
            a("Installation complete", "Установка завершена", "Installation terminée", "Installation abgeschlossen",
              "Instalación completada", "Installazione completata");
            a("Your copy of Army of Two for Xbox 360, European release: a disc image (.iso), a Games on Demand package or " +
              "default.xex in a folder with the extracted game. The files are copied to the game folder (about 5.5 GB); " +
              "the image is not needed afterwards.",
              "Ваша копия Army of Two для Xbox 360, европейское издание: образ диска (.iso), пакет Games on Demand или " +
              "default.xex в папке с распакованной игрой. Файлы копируются в папку игры (около 5,5 ГБ); образ потом не нужен.",
              "Votre exemplaire d'Army of Two pour Xbox 360, édition européenne : une image disque (.iso), un paquet " +
              "Games on Demand ou default.xex dans un dossier avec le jeu extrait. Les fichiers sont copiés dans le " +
              "dossier du jeu (environ 5,5 Go) ; l'image n'est plus nécessaire ensuite.",
              "Deine Kopie von Army of Two für Xbox 360, europäische Version: ein Disc-Image (.iso), ein " +
              "Games-on-Demand-Paket oder default.xex in einem Ordner mit dem entpackten Spiel. Die Dateien werden in " +
              "den Spielordner kopiert (etwa 5,5 GB); das Image wird danach nicht mehr gebraucht.",
              "Tu copia de Army of Two para Xbox 360, edición europea: una imagen de disco (.iso), un paquete de Games " +
              "on Demand o default.xex en una carpeta con el juego extraído. Los archivos se copian en la carpeta del " +
              "juego (unos 5,5 GB); después la imagen ya no hace falta.",
              "La tua copia di Army of Two per Xbox 360, edizione europea: un'immagine disco (.iso), un pacchetto Games " +
              "on Demand o default.xex in una cartella con il gioco estratto. I file vengono copiati nella cartella del " +
              "gioco (circa 5,5 GB); poi l'immagine non serve più.");
            a("Required: the port is built for Title Update 2. The update file is TU_12K222C_0000004000000.00000000000G1 " +
              "(on the console: Content\\0000000000000000\\4541084C\\000B0000).",
              "Обязательно: порт собран под Title Update 2. Файл обновления — TU_12K222C_0000004000000.00000000000G1 " +
              "(на консоли: Content\\0000000000000000\\4541084C\\000B0000).",
              "Obligatoire : le portage est conçu pour le Title Update 2. Le fichier de mise à jour est " +
              "TU_12K222C_0000004000000.00000000000G1 (sur la console : Content\\0000000000000000\\4541084C\\000B0000).",
              "Erforderlich: Die Portierung ist für Title Update 2 gebaut. Die Update-Datei ist " +
              "TU_12K222C_0000004000000.00000000000G1 (auf der Konsole: Content\\0000000000000000\\4541084C\\000B0000).",
              "Obligatorio: el port está hecho para el Title Update 2. El archivo de la actualización es " +
              "TU_12K222C_0000004000000.00000000000G1 (en la consola: Content\\0000000000000000\\4541084C\\000B0000).",
              "Obbligatorio: il port è fatto per il Title Update 2. Il file dell'aggiornamento è " +
              "TU_12K222C_0000004000000.00000000000G1 (sulla console: Content\\0000000000000000\\4541084C\\000B0000).");
            a("Optional: the Veteran Map Pack and SSC Challenge: Versus Mode Pack 1 packages (on the console: " +
              "Content\\0000000000000000\\454107F8\\00000002). Can be added later. In co-op both players need the same content.",
              "Необязательно: пакеты Veteran Map Pack и SSC Challenge: Versus Mode Pack 1 (на консоли: " +
              "Content\\0000000000000000\\454107F8\\00000002). Можно добавить позже. В кооперативе у обоих игроков " +
              "должен быть одинаковый контент.",
              "Facultatif : les paquets Veteran Map Pack et SSC Challenge: Versus Mode Pack 1 (sur la console : " +
              "Content\\0000000000000000\\454107F8\\00000002). Ajoutables plus tard. En coop, les deux joueurs doivent " +
              "avoir le même contenu.",
              "Optional: die Pakete Veteran Map Pack und SSC Challenge: Versus Mode Pack 1 (auf der Konsole: " +
              "Content\\0000000000000000\\454107F8\\00000002). Kann später hinzugefügt werden. Im Koop brauchen beide " +
              "Spieler dieselben Inhalte.",
              "Opcional: los paquetes Veteran Map Pack y SSC Challenge: Versus Mode Pack 1 (en la consola: " +
              "Content\\0000000000000000\\454107F8\\00000002). Se pueden añadir más tarde. En cooperativo ambos " +
              "jugadores necesitan el mismo contenido.",
              "Facoltativo: i pacchetti Veteran Map Pack e SSC Challenge: Versus Mode Pack 1 (sulla console: " +
              "Content\\0000000000000000\\454107F8\\00000002). Si possono aggiungere dopo. In cooperativa entrambi i " +
              "giocatori devono avere gli stessi contenuti.");
            a("Copy the chosen files. After that the game runs without the image.",
              "Скопировать выбранные файлы. После этого игра работает без образа.",
              "Copier les fichiers choisis. Ensuite, le jeu fonctionne sans l'image.",
              "Die gewählten Dateien kopieren. Danach läuft das Spiel ohne das Image.",
              "Copiar los archivos elegidos. Después el juego funciona sin la imagen.",
              "Copia i file scelti. Dopo il gioco funziona senza l'immagine.");
            a("Checking the game version", "Проверка версии игры", "Vérification de la version du jeu", "Prüfe die Spielversion",
              "Comprobando la versión del juego", "Verifica della versione del gioco");
            a("Copying the game files", "Копирование файлов игры", "Copie des fichiers du jeu", "Kopiere Spieldateien",
              "Copiando los archivos del juego", "Copia dei file del gioco");
            a("Extracting the game", "Распаковка игры", "Extraction du jeu", "Entpacke das Spiel", "Extrayendo el juego",
              "Estrazione del gioco");

            // --- Installation errors ---
            a("{0} is missing from the game folder", "{0} нет в папке игры", "{0} est absent du dossier du jeu",
              "{0} fehlt im Spielordner", "Falta {0} en la carpeta del juego", "{0} manca nella cartella del gioco");
            a("not an Xbox 360 disc image or a TU / DLC package", "это не образ диска Xbox 360 и не пакет TU / DLC",
              "ce n'est ni une image disque Xbox 360 ni un paquet TU / DLC", "kein Xbox-360-Disc-Image und kein TU-/DLC-Paket",
              "no es una imagen de disco de Xbox 360 ni un paquete TU / DLC",
              "non è un'immagine disco Xbox 360 né un pacchetto TU / DLC");
            a("the disc image is damaged", "образ диска повреждён", "l'image disque est endommagée", "das Disc-Image ist beschädigt",
              "la imagen de disco está dañada", "l'immagine disco è danneggiata");
            a("the package is damaged", "пакет повреждён", "le paquet est endommagé", "das Paket ist beschädigt",
              "el paquete está dañado", "il pacchetto è danneggiato");
            a("file not found", "файл не найден", "fichier introuvable", "Datei nicht gefunden", "archivo no encontrado",
              "file non trovato");
            a("This folder has no extracted game (default.xex and AO2Game)",
              "В этой папке нет распакованной игры (default.xex и AO2Game)",
              "Ce dossier ne contient pas de jeu extrait (default.xex et AO2Game)",
              "Dieser Ordner enthält kein entpacktes Spiel (default.xex und AO2Game)",
              "Esta carpeta no tiene el juego extraído (default.xex y AO2Game)",
              "Questa cartella non contiene il gioco estratto (default.xex e AO2Game)");
            a("{0}: no game on this disc (default.xex)", "{0}: на этом диске нет игры (default.xex)",
              "{0} : aucun jeu sur ce disque (default.xex)", "{0}: kein Spiel auf dieser Disc (default.xex)",
              "{0}: no hay juego en este disco (default.xex)", "{0}: nessun gioco su questo disco (default.xex)");
            a("{0}: this package belongs to another game ({1})", "{0}: этот пакет от другой игры ({1})",
              "{0} : ce paquet appartient à un autre jeu ({1})", "{0}: dieses Paket gehört zu einem anderen Spiel ({1})",
              "{0}: este paquete es de otro juego ({1})", "{0}: questo pacchetto appartiene a un altro gioco ({1})");
            a("Not enough space on {0}: {1} needed, {2} free", "Недостаточно места на {0}: нужно {1}, свободно {2}",
              "Espace insuffisant sur {0} : {1} nécessaires, {2} libres", "Nicht genug Platz auf {0}: {1} nötig, {2} frei",
              "No hay espacio suficiente en {0}: se necesitan {1}, libres {2}", "Spazio insufficiente su {0}: servono {1}, liberi {2}");
            a("This version of the game is not supported: the European release of Army of Two for Xbox 360 is needed (4541084C)",
              "Эта версия игры не поддерживается: нужно европейское издание Army of Two для Xbox 360 (4541084C)",
              "Cette version du jeu n'est pas prise en charge : il faut l'édition européenne d'Army of Two pour Xbox 360 (4541084C)",
              "Diese Spielversion wird nicht unterstützt: Benötigt wird die europäische Version von Army of Two für Xbox 360 (4541084C)",
              "Esta versión del juego no es compatible: se necesita la edición europea de Army of Two para Xbox 360 (4541084C)",
              "Questa versione del gioco non è supportata: serve l'edizione europea di Army of Two per Xbox 360 (4541084C)");
            a("This is a different update: Title Update 2 of the European release is needed (TU_12K222C_…)",
              "Это другое обновление: нужно Title Update 2 европейского издания (TU_12K222C_…)",
              "C'est une autre mise à jour : il faut le Title Update 2 de l'édition européenne (TU_12K222C_…)",
              "Das ist ein anderes Update: Benötigt wird Title Update 2 der europäischen Version (TU_12K222C_…)",
              "Es otra actualización: se necesita el Title Update 2 de la edición europea (TU_12K222C_…)",
              "È un altro aggiornamento: serve il Title Update 2 dell'edizione europea (TU_12K222C_…)");
            a("The game files are not installed: choose a game image", "Файлы игры не установлены: выберите образ игры",
              "Les fichiers du jeu ne sont pas installés : choisissez une image du jeu",
              "Die Spieldateien sind nicht installiert: wähle ein Spiel-Image",
              "Los archivos del juego no están instalados: elige una imagen del juego",
              "I file del gioco non sono installati: scegli un'immagine del gioco");
            a("Title Update 2 is not installed: choose the update file", "Title Update 2 не установлено: выберите файл обновления",
              "Le Title Update 2 n'est pas installé : choisissez le fichier de mise à jour",
              "Title Update 2 ist nicht installiert: wähle die Update-Datei",
              "El Title Update 2 no está instalado: elige el archivo de la actualización",
              "Il Title Update 2 non è installato: scegli il file dell'aggiornamento");
            return t;
        }
    }
}
