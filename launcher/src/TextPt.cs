using System;
using System.Collections.Generic;

namespace AO2Launcher
{
    // The launcher's text in Brazilian Portuguese (Text.cs: L), keyed by the
    // English like the rows of L.Build.
    static partial class L
    {
        static Dictionary<string, string> BuildPt()
        {
            var t = new Dictionary<string, string>();
            Action<string, string> p = (en, pt) => t[en] = pt;

            p("ACHIEVEMENTS", "CONQUISTAS");
            p("Story", "História");
            p("Campaign", "Campanha");
            p("Gear and co-op", "Equipamento e coop");
            p("Weapons", "Armas");
            p("Combat", "Combate");
            p("Versus", "Versus");
            p("SSC Challenge", "SSC Challenge");
            p("Profile {0}: {1} of {2} unlocked, {3} of {4} G", "Perfil {0}: {1} de {2} desbloqueadas, {3} de {4} G");
            p("Unlock notices", "Avisos de conquistas");
            p("Show a notice with a sound in the game when an achievement unlocks. Off: achievements still unlock, silently.",
              "Mostrar no jogo um aviso com som quando uma conquista for desbloqueada. Desativado: as conquistas continuam sendo desbloqueadas, em silêncio.");
            p("Reset achievements", "Redefinir conquistas");
            p("Lock every achievement of the current profile again. Saves and progress in the game are not changed.",
              "Bloquear de novo todas as conquistas do perfil atual. Os saves e o progresso no jogo não são alterados.");
            p("RESET ACHIEVEMENTS", "REDEFINIR CONQUISTAS");
            p("Every achievement of profile {0} will be locked again. This cannot be undone.",
              "Todas as conquistas do perfil {0} serão bloqueadas de novo. Isso não pode ser desfeito.");
            p("Achievements reset", "Conquistas redefinidas");
            p("Could not reset the achievements", "Não foi possível redefinir as conquistas");
            p("Earned {0}", "Obtida em {0}");
            p("The list of achievements comes from the game's files: start the game once to see it here.",
              "A lista de conquistas vem dos arquivos do jogo: inicie o jogo uma vez para vê-la aqui.");
            p("Launcher", "Launcher");
            p("Installation", "Instalação");
            p("Settings", "Configurações");
            p("BACK", "VOLTAR");
            p("SETTINGS", "CONFIGURAÇÕES");
            p("EXIT", "SAIR");
            p("PLAY", "JOGAR");
            p("INSTALL", "INSTALAR");
            p("LOOKING FOR A HOST…", "PROCURANDO ANFITRIÃO…");
            p("Game not found — click to locate armyoftworecomp.exe",
              "Jogo não encontrado — clique para localizar o armyoftworecomp.exe");
            p("Controller connected", "Controle conectado");
            p("No controller", "Sem controle");
            p("Fullscreen", "Tela cheia");
            p("Window {0}", "Janela {0}");
            p("{0} · up to date", "{0} · atualizado");
            p("{0} · update available: {1}", "{0} · atualização disponível: {1}");
            p("UPDATE AVAILABLE", "ATUALIZAÇÃO DISPONÍVEL");
            p("Version {0} is out, you have {1}. Update now?",
              "A versão {0} já saiu, você tem a {1}. Atualizar agora?");
            p("The launcher downloads and installs it by itself. Your game files, settings and saves are kept.",
              "O launcher baixa e instala sozinho. Os arquivos do jogo, as configurações e os saves são mantidos.");
            p("UPDATE", "ATUALIZAR");
            p("WHAT'S NEW", "NOVIDADES");
            p("PATCH NOTES", "NOTAS DA VERSÃO");
            p("CLOSE", "FECHAR");
            p("LATER", "DEPOIS");
            p("RETRY", "TENTAR DE NOVO");
            p("UPDATING TO {0}", "ATUALIZANDO PARA {0}");
            p("Downloading: {0:0.0} of {1:0.0} MB", "Baixando: {0:0.0} de {1:0.0} MB");
            p("Downloading: {0:0.0} MB", "Baixando: {0:0.0} MB");
            p("Installing: {0:0}%", "Instalando: {0:0}%");
            p("The launcher restarts by itself when the update is installed.",
              "O launcher reinicia sozinho quando a atualização for instalada.");
            p("Do not close the launcher.", "Não feche o launcher.");
            p("UPDATE FAILED", "FALHA NA ATUALIZAÇÃO");
            p("Try again, or download the archive from GitHub and extract it over this folder.",
              "Tente de novo, ou baixe o arquivo do GitHub e extraia por cima desta pasta.");
            p("Close the game first.", "Feche o jogo primeiro.");
            p("Could not load the release notes", "Não foi possível carregar as notas da versão");
            p("Could not load the release list", "Não foi possível carregar a lista de versões");
            p("VERSION", "VERSÃO");
            p("VERSIONS", "VERSÕES");
            p("latest", "mais recente");
            p("installed", "instalado");
            p("BACK", "VOLTAR");
            p("DOWNLOAD", "BAIXAR");
            p("INSTALL AN OLDER VERSION", "INSTALAR UMA VERSÃO ANTERIOR");
            p("Go back to {0}? You have {1}.", "Voltar para a {0}? Você tem a {1}.");
            p("AVX1 version", "Versão AVX1");
            p("The game built for processors without AVX2 (downloaded from the release, about 25 MB), a little slower. " +
              "Always on when the processor has no AVX2. Turn it on to play co-op with someone whose processor " +
              "has none: both players need the same version and the same build.",
              "O jogo compilado para processadores sem AVX2 (baixado da versão, cerca de 25 MB), um pouco mais lento. " +
              "Sempre ativado quando o processador não tem AVX2. Ative para jogar coop com alguém cujo processador " +
              "não tem: os dois jogadores precisam da mesma versão e da mesma build.");
            p("AVX1 BUILD", "BUILD AVX1");
            p("Download the build with AVX1 support for {0}?", "Baixar a build com suporte a AVX1 da {0}?");
            p("For processors without AVX2 and co-op with such a player. About 25 MB.",
              "Para processadores sem AVX2 e para coop com um jogador assim. Cerca de 25 MB.");
            p("YOUR PROCESSOR DOES NOT SUPPORT AVX2 INSTRUCTIONS", "SEU PROCESSADOR NÃO SUPORTA AS INSTRUÇÕES AVX2");
            p("On such a processor the game runs only in its AVX1 build. About 25 MB.",
              "Em um processador assim o jogo roda apenas na build AVX1. Cerca de 25 MB.");
            p("{0} is the newest version with the AVX1 build.", "A {0} é a versão mais recente com build AVX1.");
            p("No version has the AVX1 build yet", "Nenhuma versão tem a build AVX1 ainda");
            p("The AVX1 build of this version is installed", "A build AVX1 desta versão está instalada");
            p("The AVX1 build is installed", "A build AVX1 está instalada");
            p("This release has no notes", "Esta versão não tem notas");
            p("Update cancelled", "Atualização cancelada");
            p("Updated to {0}. Start the launcher again: {1}", "Atualizado para a {0}. Abra o launcher de novo: {1}");
            p("Could not open the browser: {0}", "Não foi possível abrir o navegador: {0}");
            p("GRAPHICS", "GRÁFICOS");
            p("CONTROLS", "CONTROLES");
            p("NETWORK", "REDE");
            p("ADVANCED", "AVANÇADO");
            p("Tabs", "Abas");
            p("Select", "Selecionar");
            p("Back", "Voltar");
            p("OFF", "DESLIGADO");
            p("ON", "LIGADO");
            p("NO", "NÃO");
            p("YES", "SIM");
            p("AUTO", "AUTO");
            p("auto", "auto");
            p(" ms", " ms");
            p(" GB", " GB");
            p(" MB", " MB");
            p("Language", "Idioma");
            p("Language of the game (text and voices) and of this launcher. Auto follows the language of Windows. " +
              "Russian and Brazilian Portuguese are text only, with English voices. In co-op both players need the " +
              "same voices: English, Russian and Portuguese play together, the other languages only with the same language.",
              "Idioma do jogo (texto e vozes) e deste launcher. Auto segue o idioma do Windows. " +
              "Russo e português (Brasil) são só texto, com vozes em inglês. No coop os dois jogadores precisam das " +
              "mesmas vozes: inglês, russo e português jogam juntos, os outros idiomas só com o mesmo idioma.");
            p("Display mode", "Modo de tela");
            p("FULLSCREEN", "TELA CHEIA");
            p("WINDOWED", "EM JANELA");
            p("Fullscreen is a borderless window covering the monitor; the desktop mode is not switched. " +
              "A 16:9 picture is letterboxed with black bars.",
              "Tela cheia é uma janela sem bordas que cobre o monitor; o modo da área de trabalho não é alterado. " +
              "Uma imagem 16:9 ganha barras pretas.");
            p("Window size", "Tamanho da janela");
            p("Window size in screen pixels (windowed mode only). " +
              "The game shrinks a window that does not fit the monitor's work area.",
              "Tamanho da janela em pixels da tela (só no modo em janela). " +
              "O jogo reduz uma janela que não cabe na área de trabalho do monitor.");
            p("Monitor", "Monitor");
            p("PRIMARY", "PRINCIPAL");
            p(" (primary)", " (principal)");
            p("Which monitor the game opens on.", "Em qual monitor o jogo abre.");
            p("Aspect ratio", "Proporção da tela");
            p("Shape of the picture. Auto follows the window (the monitor in fullscreen), " +
              "with no black bars. 16:9 is what the console had.",
              "Formato da imagem. Auto segue a janela (o monitor, em tela cheia), " +
              "sem barras pretas. 16:9 é o que o console tinha.");
            p("Render resolution", "Resolução de renderização");
            p("Internal resolution the game is rendered at (the console original is 720p). " +
              "1.5× is exactly Full HD, pixel for pixel on a 1920×1080 monitor; 2× is sharper and smoother but heavier. " +
              "Use 1× on a weak graphics card; 3× and 4× are for powerful ones.",
              "Resolução interna em que o jogo é renderizado (no console original é 720p). " +
              "1.5× é exatamente Full HD, pixel a pixel num monitor 1920×1080; 2× é mais nítido e suave, mas mais pesado. " +
              "Use 1× numa placa de vídeo fraca; 3× e 4× são para as potentes.");
            p("FPS limit", "Limite de FPS");
            p("Maximum frame rate; the speed of the game itself does not depend on it. " +
              "60 is the most tested option; above ~150 FPS the processor is usually the limit.",
              "Taxa máxima de quadros; a velocidade do jogo em si não depende dela. " +
              "60 é a opção mais testada; acima de ~150 FPS o limite costuma ser o processador.");
            p("FPS counter", "Contador de FPS");
            p("DETAILED", "DETALHADO");
            p("Frame rate in the corner of the screen. Detailed adds frame time, graphics card load and temperature, " +
              "processor load, video and system memory. F2 switches it in the game.",
              "Taxa de quadros no canto da tela. O detalhado acrescenta o tempo de quadro, a carga e a temperatura da placa de vídeo, " +
              "a carga do processador, a memória de vídeo e a do sistema. F2 alterna no jogo.");
            p("Vertical sync", "Sincronização vertical");
            p("Frames wait for the monitor's refresh: no tearing, but the frame rate stays at or below the refresh rate " +
              "and the input lag grows a little.",
              "Os quadros esperam a atualização do monitor: sem tearing, mas a taxa de quadros fica igual ou abaixo da taxa do monitor " +
              "e o atraso de entrada aumenta um pouco.");
            p("Anisotropic filtering", "Filtragem anisotrópica");
            p("Anti-aliasing", "Antisserrilhado");
            p("Smooths jagged edges. FXAA is the fastest and slightly soft; SMAA is sharper. " +
              "On top of the render resolution.",
              "Suaviza as bordas serrilhadas. O FXAA é o mais rápido e deixa a imagem um pouco macia; o SMAA é mais nítido. " +
              "Vem somado à resolução de renderização.");
            p("Ambient occlusion", "Oclusão de ambiente");
            p("Soft contact shadows in corners and where objects meet. SSAO is the lightest, HBAO is more defined, " +
              "GTAO is the most accurate and the heaviest.",
              "Sombras de contato suaves nos cantos e onde os objetos se encontram. O SSAO é o mais leve, o HBAO é mais definido, " +
              "o GTAO é o mais preciso e o mais pesado.");
            p("Motion blur", "Desfoque de movimento");
            p("Blur from camera movement, the same at any frame rate. Your character close to the camera stays sharp.",
              "Desfoque pelo movimento da câmera, igual em qualquer taxa de quadros. Seu personagem perto da câmera continua nítido.");
            p("Depth of field", "Profundidade de campo");
            p("What lies far behind the point you aim at goes out of focus; the focus follows the crosshair.",
              "O que está muito atrás do ponto que você mira sai de foco; o foco acompanha a mira.");
            p("Shadow resolution", "Resolução das sombras");
            p("Sharpness of shadow edges. Auto follows the render resolution (the console had 768). " +
              "4096 needs about 150 MB more video memory.",
              "Nitidez das bordas das sombras. Auto segue a resolução de renderização (o console tinha 768). " +
              "4096 precisa de cerca de 150 MB a mais de memória de vídeo.");
            p("LOW", "BAIXO");
            p("HIGH", "ALTO");
            p("Sharper textures at an angle (16×). Costs almost no performance.",
              "Texturas mais nítidas em ângulo (16×). Quase não custa desempenho.");
            p("Mouse aiming", "Mira com o mouse");
            p("The mouse turns the camera directly, like in PC shooters. When off, the mouse acts as the gamepad's right stick.",
              "O mouse gira a câmera diretamente, como nos shooters de PC. Desligado, o mouse funciona como o analógico direito do controle.");
            p("Mouse sensitivity", "Sensibilidade do mouse");
            p("How fast the mouse turns the camera. 1.0 is 0.022° per mouse count (as in Source/Quake); the default is 2.0.",
              "Quão rápido o mouse gira a câmera. 1.0 equivale a 0.022° por contagem do mouse (como em Source/Quake); o padrão é 2.0.");
            p("Sensitivity when aiming", "Sensibilidade ao mirar");
            p("Multiplier while aiming (right button, scopes). Turning already accounts for zoom; 1.0 feels the same as unaimed.",
              "Multiplicador ao mirar (botão direito, lunetas). O giro já leva o zoom em conta; 1.0 dá a mesma sensação de quando não está mirando.");
            p("Invert Y axis", "Inverter eixo Y");
            p("Mouse up looks down (on top of the inversion setting in the game itself).",
              "Mouse para cima olha para baixo (somado à inversão configurada no próprio jogo).");
            p("Aim assist", "Assistência de mira");
            p("While aiming, the camera starts on the nearest target and shots are pulled onto targets near the crosshair. " +
              "In co-op each player keeps their own setting.",
              "Ao mirar, a câmera começa no alvo mais próximo e os tiros são atraídos para alvos perto da mira. " +
              "No coop cada jogador mantém sua própria configuração.");
            p("Toggle aim", "Mira alternada");
            p("One press of aim (right mouse button, LT) starts aiming, the next press ends it, instead of holding " +
              "the button. On foot, on turrets and at a vehicle's gun; not at the wheel.",
              "Um toque em mirar (botão direito do mouse, LT) começa a mirar e o próximo termina, em vez de segurar " +
              "o botão. A pé, em torretas e na arma de um veículo; não ao volante.");
            p("Mouse in vehicles and turrets", "Mouse em veículos e torretas");
            p("Where the camera cannot be turned directly (turrets, vehicles, menus) the mouse acts as the right stick; " +
              "this is its sensitivity.",
              "Onde a câmera não pode ser girada diretamente (torretas, veículos, menus), o mouse funciona como o analógico direito; " +
              "esta é a sensibilidade dele.");
            p("KEYBOARD", "TECLADO");
            p("GAMEPAD", "CONTROLE");
            p("SPLIT-SCREEN", "TELA DIVIDIDA");
            p("Keys and mouse buttons: the Keyboard tab. Gamepad buttons and sticks: the Gamepad tab. " +
              "Two players on one PC: the Split-Screen tab.",
              "Teclas e botões do mouse: aba Teclado. Botões e analógicos do controle: aba Controle. " +
              "Dois jogadores em um PC: aba Tela dividida.");
            p("Enter or click: press the new key or mouse button (hold Shift / Ctrl / Alt for a combination). " +
              "→ or +: add one more key. Delete or ×: clear. Waiting stops after a few seconds.",
              "Enter ou clique: aperte a nova tecla ou botão do mouse (com Shift / Ctrl / Alt: uma combinação). " +
              "→ ou +: adicionar mais uma tecla. Delete ou ×: limpar. A espera termina após alguns segundos.");
            p("←→: choose the gamepad control that presses this button in the game. Enter or click: press it " +
              "on the gamepad (Xbox controllers). Applies to every gamepad; the game still shows the Xbox buttons.",
              "←→: escolha o comando do controle que aciona este botão no jogo. Enter ou clique: aperte-o " +
              "no controle (controles Xbox). Vale para todos os controles; o jogo continua mostrando os botões do Xbox.");
            p("PRESS A KEY… {0}", "APERTE UMA TECLA… {0}");
            p("PRESS A BUTTON… {0}", "APERTE UM BOTÃO… {0}");
            p("LMB", "CLIQUE E");
            p("RMB", "CLIQUE D");
            p("MMB", "CLIQUE C");
            p("MOUSE 4", "MOUSE 4");
            p("MOUSE 5", "MOUSE 5");
            p("Move forward", "Avançar");
            p("Move back", "Recuar");
            p("Move left", "Esquerda");
            p("Move right", "Direita");
            p("Crouch (L3)", "Agachar (L3)");
            p("Fire (RT)", "Atirar (RT)");
            p("Aim (LT)", "Mirar (LT)");
            p("Reload (X)", "Recarregar (X)");
            p("Roll / cover / vault (Y)", "Rolar / cobertura / pular (Y)");
            p("Use / heal / confirm (A)", "Usar / curar / confirmar (A)");
            p("Cancel / back (B)", "Cancelar / voltar (B)");
            p("Inventory (LB)", "Inventário (LB)");
            p("Co-op commands (RB)", "Comandos coop (RB)");
            p("Right stick press (R3)", "Clique do analógico direito (R3)");
            p("D-pad up: advance / primary weapon", "Direcional cima: avançar / arma primária");
            p("D-pad down: partner view / grenades", "Direcional baixo: visão do parceiro / granadas");
            p("D-pad left: regroup / secondary weapon", "Direcional esquerda: reagrupar / arma secundária");
            p("D-pad right: hold position / special weapon", "Direcional direita: manter posição / arma especial");
            p("Look up (right stick)", "Olhar para cima (analógico direito)");
            p("Look down (right stick)", "Olhar para baixo (analógico direito)");
            p("Look left (right stick)", "Olhar para a esquerda (analógico direito)");
            p("Look right (right stick)", "Olhar para a direita (analógico direito)");
            p("GPS (Back)", "GPS (Back)");
            p("Pause (Start)", "Pausa (Start)");
            p("Guide button", "Botão Guide");
            p("Key bindings", "Teclas");
            p("Return every key to the default layout (WASD, mouse).", "Voltar todas as teclas ao layout padrão (WASD, mouse).");
            p("Default keys restored", "Teclas padrão restauradas");
            p("A: use / confirm", "A: usar / confirmar");
            p("B: cancel / back", "B: cancelar / voltar");
            p("X: reload", "X: recarregar");
            p("Y: roll / cover / vault", "Y: rolar / cobertura / pular");
            p("RT: fire", "RT: atirar");
            p("LT: aim", "LT: mirar");
            p("LB: inventory", "LB: inventário");
            p("RB: co-op commands", "RB: comandos coop");
            p("L3: crouch", "L3: agachar");
            p("D-pad up", "Direcional cima");
            p("D-pad down", "Direcional baixo");
            p("D-pad left", "Direcional esquerda");
            p("D-pad right", "Direcional direita");
            p("Back: GPS", "Back: GPS");
            p("Start: pause", "Start: pausa");
            p("LEFT STICK", "ANALÓGICO ESQUERDO");
            p("RIGHT STICK", "ANALÓGICO DIREITO");
            p("Movement (left stick)", "Movimento (analógico esquerdo)");
            p("Camera (right stick)", "Câmera (analógico direito)");
            p("Which stick of the gamepad moves the character. Swap both to play with the sticks swapped.",
              "Qual analógico do controle move o personagem. Troque os dois para jogar com os analógicos invertidos.");
            p("Which stick of the gamepad turns the camera and aims.", "Qual analógico do controle gira a câmera e mira.");
            p("Invert movement stick Y", "Inverter Y do analógico de movimento");
            p("Invert camera stick Y", "Inverter Y do analógico da câmera");
            p("Up on the movement stick walks backwards.", "Para cima no analógico de movimento anda para trás.");
            p("Up on the camera stick looks down (on top of the Invert Y option in the game itself).",
              "Para cima no analógico da câmera olha para baixo (somado à opção Inverter Y do próprio jogo).");
            p("Gamepad layout", "Layout do controle");
            p("Return every gamepad button to the console layout.", "Voltar todos os botões do controle ao layout do console.");
            p("Default gamepad layout restored", "Layout padrão do controle restaurado");
            p("Split-screen", "Tela dividida");
            p("Two players on one PC, each with their own half of the screen. On Play the game starts at once, without " +
              "looking for a network host. Off: every device controls player 1.",
              "Dois jogadores em um PC, cada um com sua metade da tela. Em Jogar, o jogo começa na hora, sem " +
              "procurar um anfitrião na rede. Desligado: todos os dispositivos controlam o jogador 1.");
            p("Player 1 name", "Nome do jogador 1");
            p("Player 2 name", "Nome do jogador 2");
            p("Gamertag of player 1 (up to 15 characters), the same as Network → Player name. Empty: the computer's name.",
              "Gamertag do jogador 1 (até 15 caracteres), a mesma de Rede → Nome do jogador. Vazio: o nome do computador.");
            p("Gamertag of player 2 (up to 15 characters); their own profile and settings. " +
              "Empty: player 1's name with (1) after it.",
              "Gamertag do jogador 2 (até 15 caracteres); com perfil e configurações próprios. " +
              "Vazio: o nome do jogador 1 com (1) depois.");
            p("Player 1 controls", "Controles do jogador 1");
            p("Player 2 controls", "Controles do jogador 2");
            p("The devices of player 1. Gamepads are numbered in the order the game finds them (usually the order " +
              "they were connected): if the players' pads turn out swapped, swap them here. " +
              "Any gamepad: every gamepad the other player does not have.",
              "Os dispositivos do jogador 1. Os controles são numerados na ordem em que o jogo os encontra (normalmente a ordem " +
              "em que foram conectados): se os controles dos jogadores saírem trocados, troque-os aqui. " +
              "Qualquer controle: todos os controles que o outro jogador não tem.");
            p("The devices of player 2. The mouse aims for whoever has the keyboard. " +
              "Any gamepad: every gamepad the other player does not have.",
              "Os dispositivos do jogador 2. O mouse mira para quem estiver com o teclado. " +
              "Qualquer controle: todos os controles que o outro jogador não tem.");
            p("KEYBOARD + MOUSE", "TECLADO + MOUSE");
            p("ANY GAMEPAD", "QUALQUER CONTROLE");
            p("GAMEPAD 1", "CONTROLE 1");
            p("GAMEPAD 2", "CONTROLE 2");
            p("GAMEPAD 3", "CONTROLE 3");
            p("GAMEPAD 4", "CONTROLE 4");
            p("KEYBOARD + ANY PAD", "TECLADO + CONTR.");
            p("KEYBOARD + PAD 1", "TECLADO + CONTR. 1");
            p("KEYBOARD + PAD 2", "TECLADO + CONTR. 2");
            p("GAMEPAD {0}", "CONTROLE {0}");
            p("KEYBOARD + PAD {0}", "TECLADO + CONTR. {0}");
            p("not connected", "não conectado");
            p("Game controller", "Controle de jogo");
            p("No gamepads connected.", "Nenhum controle conectado.");
            p("Connected:", "Conectados:");
            p("gamepad {0} - {1}", "controle {0} - {1}");
            p("Both players have the keyboard: it goes to player 1.", "Os dois jogadores estão com o teclado: ele fica com o jogador 1.");
            p("Both players have gamepad {0}: it goes to player 1.", "Os dois jogadores estão com o controle {0}: ele fica com o jogador 1.");
            p("Any gamepad for both players: they all go to player 1.", "Qualquer controle para os dois jogadores: todos ficam com o jogador 1.");
            p("Gamepad {0} is not connected.", "O controle {0} não está conectado.");
            p("In the game: Campaign Mode → Splitscreen; player 2 signs in with their device when asked.",
              "No jogo: Modo Campanha → Tela dividida; o jogador 2 entra com o dispositivo dele quando o jogo pedir.");
            p("Starting split-screen", "Iniciando tela dividida");
            p("Player name", "Nome do jogador");
            p("The name your partner sees (up to 15 characters). Empty: the computer's name.",
              "O nome que seu parceiro vê (até 15 caracteres). Vazio: o nome do computador.");
            p("Host IP", "IP do anfitrião");
            p("Leave empty: the host is found automatically on the local network or Radmin VPN. " +
              "Enter an address only if the host is not found, for example over the internet.",
              "Deixe vazio: o anfitrião é encontrado automaticamente na rede local ou no Radmin VPN. " +
              "Digite um endereço só se o anfitrião não for encontrado, por exemplo pela internet.");
            p("Input delay margin", "Margem de atraso de entrada");
            p("Added to the network delay. 0 is the minimum. If the game stutters waiting for the partner on an unstable " +
              "connection, set 16-50. Must be the same for both players.",
              "Somada ao atraso da rede. 0 é o mínimo. Se o jogo engasgar esperando o parceiro numa conexão " +
              "instável, ajuste para 16-50. Deve ser igual para os dois jogadores.");
            p("Windows Firewall", "Firewall do Windows");
            p("ALLOW", "PERMITIR");
            p("Allow incoming connections to the game on every network type (Windows treats Radmin as public). " +
              "Needs administrator rights: Windows will ask to confirm. Done once on both PCs.",
              "Permitir conexões de entrada para o jogo em todos os tipos de rede (o Windows trata o Radmin como pública). " +
              "Exige direitos de administrador: o Windows vai pedir confirmação. Feito uma vez nos dois PCs.");
            p("Co-op log", "Registro do coop");
            p("Writes coop_debug.log in the game folder, needed to investigate desyncs. Costs a little performance.",
              "Grava o coop_debug.log na pasta do jogo, necessário para investigar dessincronizações. Custa um pouco de desempenho.");
            p("The role is chosen automatically: on Play the launcher looks for a host on the local network. If one is " +
              "found you join it, otherwise you become the host. The second player presses Play a few seconds after " +
              "the first player's game has started. Then both, in the game: Campaign Mode → Public Co-op Game.",
              "O papel é escolhido automaticamente: ao clicar em Jogar, o launcher procura um anfitrião na rede local. Se encontrar " +
              "um, você entra nele; se não, você vira o anfitrião. O segundo jogador clica em Jogar alguns segundos depois de " +
              "o jogo do primeiro ter iniciado. Depois, os dois, no jogo: Modo Campanha → Jogo Coop Público.");
            p("God mode (cheat)", "Modo deus (cheat)");
            p("The player takes no damage. Has no effect in online co-op.",
              "O jogador não sofre dano. Não tem efeito no coop online.");
            p("Close the launcher", "Fechar o launcher");
            p("Close this window once the game has started.", "Fechar esta janela depois que o jogo iniciar.");
            p("Extra arguments", "Argumentos extras");
            p("Extra command line arguments for the game, appended at the end (for advanced users).",
              "Argumentos extras de linha de comando para o jogo, adicionados no final (para usuários avançados).");
            p("Game folder", "Pasta do jogo");
            p("OPEN", "ABRIR");
            p("Game not found", "Jogo não encontrado");
            p("Open the game folder in Explorer (logs, settings, shader cache).",
              "Abrir a pasta do jogo no Explorador de Arquivos (logs, configurações, cache de shaders).");
            p("Saves", "Saves");
            p("No saves yet: {0}", "Ainda não há saves: {0}");
            p("The folder of the current profile's saves (the main profile's: Documents\\armyoftworecomp_tu2).",
              "A pasta de saves do perfil atual (a do perfil principal: Documents\\armyoftworecomp_tu2).");
            p("Game files and DLC", "Arquivos do jogo e DLC");
            p("Install the game from your own disc image, Title Update 2, or add downloadable content (DLC).",
              "Instale o jogo a partir da sua própria imagem de disco e do Title Update 2, ou adicione conteúdo para download (DLC).");
            p("Desktop shortcut", "Atalho na área de trabalho");
            p("CREATE", "CRIAR");
            p("Shortcut created on the desktop", "Atalho criado na área de trabalho");
            p("Could not create the shortcut", "Não foi possível criar o atalho");
            p("Create an \"Army of Two\" shortcut on the desktop that starts this launcher.",
              "Criar na área de trabalho um atalho \"Army of Two\" que abre este launcher.");
            p("Game path", "Caminho do jogo");
            p("CHANGE", "ALTERAR");
            p("Point to armyoftworecomp.exe by hand if the launcher is not in the game folder.",
              "Indique o armyoftworecomp.exe manualmente se o launcher não estiver na pasta do jogo.");
            p("Launcher settings", "Configurações do launcher");
            p("RESET", "REDEFINIR");
            p("Settings reset to defaults", "Configurações redefinidas para o padrão");
            p("Return every launcher setting to its default. Game files are not changed.",
              "Voltar todas as configurações do launcher ao padrão. Os arquivos do jogo não são alterados.");
            p("Where is armyoftworecomp.exe?", "Onde está o armyoftworecomp.exe?");
            p("Programs", "Programas");
            p("Set the game path first", "Defina primeiro o caminho do jogo");
            p("Done: incoming connections to the game are allowed", "Pronto: as conexões de entrada para o jogo estão permitidas");
            p("Failed (the administrator prompt has to be confirmed)", "Falhou (o pedido de administrador precisa ser confirmado)");
            p("The game is already running", "O jogo já está em execução");
            p("Could not start the game: {0}", "Não foi possível iniciar o jogo: {0}");
            p("Host found: {0} - joining", "Anfitrião encontrado: {0} - entrando");
            p("No host found - hosting a game", "Nenhum anfitrião encontrado - criando a partida");
            p("GAME INSTALLATION", "INSTALAÇÃO DO JOGO");
            p("The launcher contains no game files: you need your own copy of Army of Two for Xbox 360 and " +
              "Title Update 2. Choose the files below or drop them on this window.",
              "O launcher não contém arquivos do jogo: você precisa da sua própria cópia de Army of Two para Xbox 360 e do " +
              "Title Update 2. Escolha os arquivos abaixo ou arraste-os para esta janela.");
            p("Game", "Jogo");
            p("game", "jogo");
            p("installed", "instalado");
            p("not chosen - a disc image (.iso) or the game folder", "não escolhido - uma imagem de disco (.iso) ou a pasta do jogo");
            p("not chosen - required", "não escolhido - obrigatório");
            p("Downloadable content (DLC)", "Conteúdo para download (DLC)");
            p("packages installed: {0}", "pacotes instalados: {0}");
            p("none chosen - can be added later", "nenhum escolhido - pode ser adicionado depois");
            p("created after installation", "criado após a instalação");
            p("CHOOSE", "ESCOLHER");
            p("ADD", "ADICIONAR");
            p("CANCELLING…", "CANCELANDO…");
            p("CANCEL", "CANCELAR");
            p("EVERYTHING IS INSTALLED", "TUDO INSTALADO");
            p("Added: {0}", "Adicionado: {0}");
            p("Army of Two game image (Xbox 360)", "Imagem do jogo Army of Two (Xbox 360)");
            p("Disc image", "Imagem de disco");
            p("Extracted game", "Jogo extraído");
            p("All files", "Todos os arquivos");
            p("DLC packages", "Pacotes de DLC");
            p("Preparing", "Preparando");
            p("Installation cancelled", "Instalação cancelada");
            p("Game installed, shortcut created on the desktop", "Jogo instalado, atalho criado na área de trabalho");
            p("Installation complete", "Instalação concluída");
            p("Your copy of Army of Two for Xbox 360, European release: a disc image (.iso), a Games on Demand package or " +
              "default.xex in a folder with the extracted game. The files are copied to the game folder (about 5.5 GB); " +
              "the image is not needed afterwards.",
              "Sua cópia de Army of Two para Xbox 360, versão europeia: uma imagem de disco (.iso), um pacote Games on Demand ou " +
              "o default.xex em uma pasta com o jogo extraído. Os arquivos são copiados para a pasta do jogo (cerca de 5,5 GB); " +
              "depois disso a imagem não é mais necessária.");
            p("Required: the port is built for Title Update 2. The update file is TU_12K222C_0000004000000.00000000000G1 " +
              "(on the console: Content\\0000000000000000\\4541084C\\000B0000).",
              "Obrigatório: o port foi feito para o Title Update 2. O arquivo da atualização é TU_12K222C_0000004000000.00000000000G1 " +
              "(no console: Content\\0000000000000000\\4541084C\\000B0000).");
            p("Optional: the Veteran Map Pack and SSC Challenge: Versus Mode Pack 1 packages (on the console: " +
              "Content\\0000000000000000\\454107F8\\00000002). Can be added later. In co-op both players need the same content.",
              "Opcional: os pacotes Veteran Map Pack e SSC Challenge: Versus Mode Pack 1 (no console: " +
              "Content\\0000000000000000\\454107F8\\00000002). Podem ser adicionados depois. No coop os dois jogadores precisam do mesmo conteúdo.");
            p("Copy the chosen files. After that the game runs without the image.",
              "Copiar os arquivos escolhidos. Depois disso o jogo roda sem a imagem.");
            p("Checking the game version", "Verificando a versão do jogo");
            p("Copying the game files", "Copiando os arquivos do jogo");
            p("Extracting the game", "Extraindo o jogo");
            p("{0} is missing from the game folder", "{0} não está na pasta do jogo");
            p("the processor is too old: SSE4.2 is required (Intel Core i 1st gen / AMD FX or newer)",
              "o processador é antigo demais: é necessário SSE4.2 (Intel Core i 1ª geração / AMD FX ou mais recente)");
            p("not an Xbox 360 disc image or a TU / DLC package", "não é uma imagem de disco do Xbox 360 nem um pacote de TU / DLC");
            p("the disc image is damaged", "a imagem de disco está danificada");
            p("the package is damaged", "o pacote está danificado");
            p("file not found", "arquivo não encontrado");
            p("This folder has no extracted game (default.xex and AO2Game)",
              "Esta pasta não tem o jogo extraído (default.xex e AO2Game)");
            p("{0}: no game on this disc (default.xex)", "{0}: não há jogo neste disco (default.xex)");
            p("{0}: this package belongs to another game ({1})", "{0}: este pacote é de outro jogo ({1})");
            p("Not enough space on {0}: {1} needed, {2} free", "Espaço insuficiente em {0}: {1} necessários, {2} livres");
            p("This version of the game is not supported: the European release of Army of Two for Xbox 360 is needed (4541084C)",
              "Esta versão do jogo não é compatível: é necessária a versão europeia de Army of Two para Xbox 360 (4541084C)");
            p("This is a different update: Title Update 2 of the European release is needed (TU_12K222C_…)",
              "Esta é outra atualização: é necessário o Title Update 2 da versão europeia (TU_12K222C_…)");
            p("The game files are not installed: choose a game image",
              "Os arquivos do jogo não estão instalados: escolha uma imagem do jogo");
            p("Title Update 2 is not installed: choose the update file",
              "O Title Update 2 não está instalado: escolha o arquivo da atualização");
            p("choose the game first", "escolha o jogo primeiro");
            p("choose Title Update 2 first", "escolha o Title Update 2 primeiro");
            p("Choose the game first, then Title Update 2", "Escolha primeiro o jogo, depois o Title Update 2");
            p("Choose Title Update 2 first, then the DLC", "Escolha primeiro o Title Update 2, depois os DLC");
            p("OK", "OK");
            p("DELETE", "EXCLUIR");
            p("Uninstall the game", "Desinstalar o jogo");
            p("UNINSTALL", "DESINSTALAR");
            p("UNINSTALL THE GAME", "DESINSTALAR O JOGO");
            p("Delete the installed game files (the assets folder). Saves are kept unless you choose otherwise.",
              "Excluir os arquivos do jogo instalados (a pasta assets). Os saves são mantidos, a menos que você escolha o contrário.");
            p("The game files will be deleted from {0} ({1}). To play again, install the game from your copy. Downloadable content stays in the dlc folder.",
              "Os arquivos do jogo serão excluídos de {0} ({1}). Para jogar de novo, instale o jogo a partir da sua cópia. O conteúdo para download fica na pasta dlc.");
            p("Moving the DLC to the game folder...", "Movendo os DLC para a pasta do jogo...");
            p("DLC moved to the game folder: {0}", "DLC movidos para a pasta do jogo: {0}");
            p("Could not move the DLC to the game folder: {0}", "Não foi possível mover os DLC para a pasta do jogo: {0}");
            p("Also delete the saves of every profile", "Excluir também os saves de todos os perfis");
            p("Could not delete everything: {0}", "Não foi possível excluir tudo: {0}");
            p("The game and the saves are deleted", "O jogo e os saves foram excluídos");
            p("The game is uninstalled, the saves are kept", "O jogo foi desinstalado, os saves foram mantidos");
            p("Unlock All (cheat)", "Liberar tudo (cheat)");
            p("UNLOCK ALL", "LIBERAR TUDO");
            p("TURN ON", "LIGAR");
            p("The game will unlock the Professional difficulty and give 9,999,999 money in the current profile. " +
              "This is saved in the profile and stays after the cheat is turned off. Turn it on?",
              "O jogo vai liberar a dificuldade Profissional e dar 9.999.999 de dinheiro no perfil atual. " +
              "Isso é salvo no perfil e continua depois que o cheat for desligado. Ligar?");
            p("Unlocks every difficulty and gives 9,999,999 money (kept at the maximum while on). " +
              "Saved in the current profile. Has no effect in online co-op.",
              "Libera todas as dificuldades e dá 9.999.999 de dinheiro (mantido no máximo enquanto estiver ligado). " +
              "Salvo no perfil atual. Não tem efeito no coop online.");
            p("PROFILES", "PERFIS");
            p("PROFILE", "PERFIL");
            p("Profile", "Perfil");
            p("Main", "Principal");
            p("Profile {0}", "Perfil {0}");
            p("Profile {0}: {1}", "Perfil {0}: {1}");
            p("no saves yet", "ainda sem saves");
            p("none yet", "nenhum ainda");
            p("Last checkpoint: {0}", "Último checkpoint: {0}");
            p("Money: {0}", "Dinheiro: {0}");
            p("The profile the game starts with. Each profile has its own saves, game settings, money and unlocks; the downloadable content is shared.",
              "O perfil com que o jogo inicia. Cada perfil tem seus próprios saves, configurações do jogo, dinheiro e itens liberados; o conteúdo para download é compartilhado.");
            p("New profile name", "Nome do novo perfil");
            p("The name of a new profile. Empty: the suggested one.", "O nome de um novo perfil. Vazio: o sugerido.");
            p("Create profile", "Criar perfil");
            p("Create an empty profile with the name above and switch to it. The game starts it from the beginning.",
              "Criar um perfil vazio com o nome acima e mudar para ele. O jogo começa nele do início.");
            p("Delete profile", "Excluir perfil");
            p("Delete the current profile with all its saves. The main profile cannot be deleted.",
              "Excluir o perfil atual com todos os seus saves. O perfil principal não pode ser excluído.");
            p("Profiles other than the main one are kept in Documents\\armyoftworecomp_profiles, or in AppData\\Local\\armyoftworecomp_profiles if Documents cannot be written to.",
              "Os perfis diferentes do principal ficam em Documents\\armyoftworecomp_profiles, ou em AppData\\Local\\armyoftworecomp_profiles se não for possível gravar em Documents.");
            p("Enter a name for the profile", "Digite um nome para o perfil");
            p("This name cannot be used for a profile", "Este nome não pode ser usado em um perfil");
            p("A profile named {0} already exists", "Já existe um perfil chamado {0}");
            p("Could not create the profile: {0}", "Não foi possível criar o perfil: {0}");
            p("Profile {0} created", "Perfil {0} criado");
            p("DELETE PROFILE", "EXCLUIR PERFIL");
            p("Profile {0} and all its saves will be deleted for good ({1}).", "O perfil {0} e todos os seus saves serão excluídos para sempre ({1}).");
            p("Could not delete the profile: {0}", "Não foi possível excluir o perfil: {0}");
            p("Profile {0} deleted", "Perfil {0} excluído");
            p("Somalia", "Somália");
            p("Afghanistan", "Afeganistão");
            p("Iraq", "Iraque");
            p("Aircraft carrier", "Porta-aviões");
            p("China", "China");
            p("Miami", "Miami");
            p("Kiev", "Kiev");
            p("Miami (alternate ending)", "Miami (final alternativo)");

            return t;
        }
    }
}
