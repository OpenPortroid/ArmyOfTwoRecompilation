# Online co-op

Army of Two's online co-op works again: one player hosts, the other joins over
a LAN or a virtual network (Radmin VPN, ZeroTier, Tailscale...).

## Playing

Both players need the **same build** (`armyoftworecomp.exe` + `rexruntime.dll`)
and their own game files in `assets/`. Settings live in
[`settings/online.toml`](../settings/online.toml) (or the same names as
`--flags`):

| | Host | Partner |
|---|---|---|
| `ao2_coop_server` | `true` | `false` |
| `ao2_coop_host` | empty | host's IP address |
| `net_gamertag` | a name | a name |

1. Host: *Campaign Mode → Search Co-op Game*, pick map and difficulty → "Finding
   a game session…" → the lobby opens.
2. Partner: the same menu (map/difficulty do not matter) → joins the host's game.
3. Host: *Start Game*.

Ports: host TCP 18480, 18485, 13505 (lobby), UDP 18490 (LAN discovery) and UDP
1000 (game); partner UDP 1000; both UDP 18491 (invites). Allow the game in Windows Firewall *for all network types* (Windows
puts Radmin VPN on the public profile). With a VPN nothing needs forwarding:
the server tells each player the address it sees the other one at, so the
host's Radmin address (26.x.x.x) is all the partner needs.

The lobby server also exists on its own: `ao2_lobby_server.exe` (target
`ao2_lobby_server`, `src/lobby_server_main.cpp`, no ReXGlue runtime, static
CRT) prints this PC's addresses (Radmin VPN marked) and serves the same
protocol; both games then use `ao2_coop_host` (127.0.0.1 on the server's PC).

A self-contained copy for another PC (exe, DLLs incl. the Visual C++ runtime
and D3DCompiler, settings, shader cache, game files, `Host co-op.bat` /
`Join co-op.bat` / firewall launchers, Russian README) is built by
`tools/make_dist.ps1` (launchers and README in `tools/dist/`); it keeps the build's PDB in
`C:\aot2\dist\symbols\` for crash dumps from that PC.

### Create Co-op Game (invites)

*Create Co-op Game* (the disc's Private Co-op Game) and Versus' *Create Match*
(the disc's Custom Match) first ask who finds the game (`src/lobby_visibility.cpp`):
**Private** - by invitation only, a friends-only Steam lobby; **Public** - also
in everyone's server list (F1 → Online → Servers). The game hosts the same way
either time; the pick stands in for its `B-U-Private` in the Steam lobby, the
invites and the Players window, until Search Co-op Game or Quick Match is chosen.
*Search Co-op Game* (the disc's Public Co-op Game) and Versus' *Quick Match*
open, while Steam runs, a list of the public Steam lobbies of their mode
(`src/game_browser.cpp`: host, mission, difficulty or game type, players, ping;
the lobby data's `difficulty` is the game's `B-U-Difficulty`) instead of the
game's own search: a lobby is joined as in F1 → Online → Servers, *Create*
runs Create Co-op Game / Create Match as Public. Without Steam the game's
search runs.
The new names are written into the localization at start (`game_language.cpp`).

1. Host: *Campaign Mode → Create Co-op Game* → Private or Public → the lobby → *Invite*: the
   **Players** window opens (instead of the Xbox guide) with everyone running
   the game on the LAN / VPN → *Invite* next to the partner.
2. Partner (anywhere in the game): a notice "<host> invites you…" → **F6**
   accepts (or **F8** → the Players window → Accept / Decline). The game goes
   to the host's lobby by itself - the partner needs no `ao2_coop_host`.
3. Host: *Start Game*.

Players are found by UDP broadcast (port **18491**, LAN and Radmin VPN);
anyone else by address: `ao2_friends` (settings/online.toml) or the address
field in the Players window. Either player being reachable is enough (the
answer goes back through the address the announcement came from). F8 opens
the window at any time; `ao2_invites = false` turns it all off.

Native mouse aiming works in co-op too: the turn travels with the pad input
(see *Lockstep* below). Where the game does not turn the camera itself
(vehicles, turrets, cutscenes) the mouse drives the right stick, as in single
player.

## How it works

### EA lobby (built-in server, `src/ea_server.cpp`)

The game logs in to EA (FESL, TCP 18480), then uses Theater (TCP 18485) to
create, list and enter games and Messenger (TCP 13505) for presence. The game
asks for the server with `XOnlineGetServiceInfo` (services `45410004`,
`45410805`); the SDK answers with `net_title_server_ip`, which
`ao2_coop_server` / `ao2_coop_host` set. Packets: 4-byte type, u32 BE flags,
u32 BE length, `key=value\n` lines, NUL.

What the game needs (found by tracing it, `--ao2_coop_debug=true`):

- **Entering a game is the same for host and partner**: `EGAM` → server sends
  `EGRQ` to the host → host answers `EGRS` (`ALLOWED`, `REASON`) → `EGEG` with
  the host's address to the player. The host's game manager only starts hosting
  after receiving *its own* `EGEG`; before that it refuses everybody with
  `REASON=2` (state 0, `sub_822424D0`). A versus party follows its leader into
  the match at once and gives up after three refusals a second apart, so the
  server holds such a refusal for an entry younger than 8 s and sends the
  host the same `EGRQ` again every 0.5 s (`kHostNotReadyWait`; an `ECNL` of
  the player ends it).
- The host recognizes its own request by `EGRQ UID` = FESL `userId`, and
  registers the partner in its Xbox Live session with `EGRQ XUID` (else XUID 0).
- A refusal is `EGEG` with status `jden` in the header; an `EGEG` without
  address fields crashes the client.
- `EGEG` for Xbox 360: `PL=xbox360`, `XNADDR` (base64 of the 36-byte XNADDR,
  parsed by `sub_82222348`).
- The client closes a connection silent for 240 s (`conn err -206`): the
  server pings FESL (`fsys Ping`) and Theater (`PING`) every 30 s.
- The server rewrites each player's XNADDR to the address it sees the player
  at, and the host's to the address the partner reached the server at, so LAN
  and VPN work without choosing adapters.

The lobby library is EA DirtySock; its debug output (normally discarded) goes
to `coop_debug.log` with `--ao2_coop_debug=true` (hooks of `sub_82217260` /
`sub_822172B0` in `src/coop_debug.cpp`), together with the join flow, leave
reasons and the lockstep frame state.

### Invites (`src/coop_invite.cpp`)

Presence packets are plain UDP: players share no secret, so a packet proves nothing about who sent it, and its name can
be anybody's. `src/invite_checks.h` limits what one can do (test: `tools/invite_checks_test.cpp`): text fields are cut and
stripped of control characters, the server in an invite is a number that is never looked up and must fit the sender (no
loopback from another PC, only Steam addresses over Steam, no local address from outside the network), the lists and new
invites are limited in number, and a `GAME` answer counts only from the server asked. An invite naming a third machine's
server is legitimate, so it is not refused: the invite card shows the sender's address and that server before the player
accepts. Connecting to a server the player accepted is still connecting to whatever is there.

On the console Live carries the invite: the host's guide (XShowGameInviteUI)
sends it, the partner's guide accepts it and the title gets
`XN_LIVE_INVITE_ACCEPTED` (0x02000002); its notification pump
(`sub_8222D530`) asks `XInviteGetAcceptedInfo` (`sub_82C41388`,
XMsgInProcessCall 0xFC / 0x58023) for `XINVITE_INFO` (+0 invitee XUID, +8
inviter XUID, +16 title, +20 XSESSION_INFO, +80 fFromGameInvite). EA's layer
(`sub_82236E30`) checks the invitee XUID against the signed-in player and
raises the script's `OnXbInviteAccepted`; the game then logs in to EA and
sends **`EGAM` without a GID but with `XUID=<inviter>`** (TYPE=G, then
TYPE=P): the server enters it into the game that player hosts.

Here a presence service in every game (UDP 18491, text packets
`AO2I1 <TYPE>` + `key=value` lines) replaces Live:

- `HELLO` / `HERE` (name, hosting, menu / lobby / game) every 8 s (2 s with
  the window open) to every adapter's broadcast address, `ao2_friends`,
  `ao2_coop_host`, the addresses typed in and the players already known.
- `INVITE` (id, inviter name / XUID, session id, host XNADDR, the inviter's
  `net_title_server_ip`), `ACK`, `ANSWER` (accepted). The session comes from
  the lobby game the inviter hosts (`ea::FindHostedGame`: CGAM `XB360SESS`,
  the host's own EGAM `R-XNADDR`); a host whose lobby server runs in another
  process asks that server's presence service (`GAMEQ` / `GAME`).
- Overrides: the `XamShowGameInviteUI` / `XamShowFriendsUI` thunks
  (`sub_82B07EB0` / `sub_82B07E60`) open the Players window
  (`debug_menu.h` OpenWindow: modal, the game gets no input); the invite
  notice is drawn without input (SetNotice, the frame rate overlay's layer);
  accepting points `net_title_server_ip` at the inviter's server, then
  broadcasts the notification; `sub_82C41388` answers with the invite (the
  invitee XUID read through `sub_82B07E10(user, 5, &info)` as EA reads it).
- The server before the invite comes back once the inviter has hosted
  nothing for 10 s (its `HELLO` says so, or it is gone) and this game is in
  the menus (`RestoreOwnServer`). Without that a game hosted later went to
  the inviter's server: no Steam lobby, "nogame" to a knock, and a whole
  party's matches over a relay to a PC that had left. An invite whose server
  is this PC's own Steam address keeps this PC's own server.
- A second game behind a player's address (a test copy, another build) does
  not take over the entry of the player with this build.

- `HELLO` / `HERE` also carry `sync`, the settings that must match in a
  lockstep game (`SharedSettings`: `ao2_coop_input_margin_ms`, the shared LOD,
  `ao2_coop_god_mode` and the co-op fixes). In the lobby a player with this
  build but other values gets a banner naming them, as `key mine/theirs`. It
  warns and changes nothing.

`invite.log` (with `log_to_files`) shows players found, invites and answers.

### Over Steam (`src/steam_net.cpp`)

With the Steam client running (`ao2_steam`, on by default) the game signs in
as Spacewar (app 480) and needs no VPN or forwarded ports:

- Friends running the game (rich presence `ao2`) show in the Players window
  (address "Steam") and are invited as on a LAN.
- A game a player hosts becomes a Steam lobby (public; friends only for a
  private game; 2 places, 4 in versus) with `ao2`=1, name, build, mode and
  player count. The Players window's **STEAM GAMES** lists them worldwide;
  **JOIN** (or Steam's "Join game" / a lobby invite from **INVITE STEAM
  FRIENDS**) enters the lobby, the host's game invites whoever enters, and the
  joiner's game accepts that invite by itself.
- The list shows host, mission, mode (co-op / Warzone / Bounties /
  Extraction), free places and ping, the lowest ping first: the host puts its
  ping location (`ISteamNetworkingUtils`) in the lobby as `loc`, the others
  estimate the ping from it without connecting.
- A lobby joined is left 10 s after this player's network game ends, or after
  30 s when its game was never entered: it would keep a place of the host's
  lobby taken. Joining another lobby while hosting makes no new lobby for the
  hosted game until the join fails or that game ends.
- **Search Co-op Game / Quick Match** (the game's quick match, `GLST` with
  `FILTER-NOT-PRIVATE=1` from the PC's own game) searches first
  (`ao2_public_game_search`, on by default; off: an empty list at once, so the
  game hosts a public game whose Steam lobby is in everyone's server list). A search that finds
  nothing on the PC's own server asks Steam
  (`ea::SetQuickMatchHook` → `steam::QuickMatch`): the open lobby of the same
  `B-U-Mode` and build with the lowest ping is joined and its invite accepted
  (the list request is left unanswered, the game goes on through the invite);
  with none within ~15 s the list is answered empty and the game hosts.
- **Join with code** (F1 → Online → Join with code), for players who met
  elsewhere and are not Steam friends: the code is the player's Steam account ID in Crockford base
  32 (7 characters, `ABC-DEFG`). Entering the host's code adds the host as a
  peer and sends `knock` on ISteamNetworkingMessages channel 1 every 2 s for
  up to 20 s. A host with a lobby game and a free place invites the knocker
  (`invite::InviteAddress`), which accepts the invite automatically, and
  answers `invited`. Otherwise it answers `nogame`, `full` or `started`. No
  lobby is involved, so a private game can be joined this way too.
- Traffic: every Steam player gets an address in 127.64.0.0/10 made from its
  SteamID, the same on every PC. For each known player the bridge binds that
  address on UDP 1000, UDP 1002 (the link, `src/coop_link.cpp`), UDP 18491 and TCP 18480 / 18485 / 13505 (next to the
  game's own sockets on 0.0.0.0), carries UDP as ISteamNetworkingMessages
  (4-byte header: source and destination port) and each TCP connection as a
  P2P connection on the same virtual port, and on the far side passes it on
  from the sender's address to the PC's own virtual address. The lobby
  server and the invites treat these addresses as other PCs, not loopback.

`steam_api64.dll` is linked to load late: without it (or without Steam) the
game plays as before. Built only when `steamworks/sdk` holds the Steamworks
SDK. `steam.log` shows peers, lobbies and tunnels.

### Automatic role selection (launcher)

When Play is pressed the launcher broadcasts a query on UDP 18490. If a host
answers, the game starts as the partner (`--ao2_coop_host=<ip>`); otherwise it
starts as the host (`--ao2_coop_server`). The responder runs inside the lobby
server (`StartDiscoveryResponder` in `src/ea_server.cpp`), so both the game
with `ao2_coop_server` and `ao2_lobby_server.exe` answer. The packet is 16
bytes, four big-endian uint32: magic `0x414F3244` ("AO2D"), version 1, type
(1 = query, 2 = reply), nonce; the reply echoes the nonce. It works on a LAN
and over Radmin VPN, but not across the internet without a VPN - then enter
the host's IP manually in the launcher settings. The per-program firewall rule
for the game already covers the port.

### Lockstep (`src/coop.h`)

Co-op is a **deterministic lockstep simulation**: both machines simulate the
whole game from the exchanged pad input (VDP UDP port 1000, ~30 packets/s each
way). Every frame carries a checksum - the shared `rand()` stream (MSVC LCG,
`sub_823E1BB8`) and a hash of physics state (`sub_826F7728`) - stored at
+44/+46 of the frame sync object (singleton at `[0x831228C8]`, ticked by
`sub_82964AC8`; its mode at +16 is 1 in a network game). `sub_8295D1B0`
compares them with the partner's; after 600 mismatching frames the client
leaves and both see "Connection Lost".

So nothing may change the simulation on one machine only - any local-only
patch must either go through the exchanged input or be off while
`ao2::coop::OnlineLockstep` is true.

The input path, found by tracing the pads into the frame record:

- The engine polls the pads (`sub_829C93D8`) and sends stick axes to the
  gameplay viewport's `InputAxis` (`sub_82731298`), which stores them in the
  local player's frame record (`sub_82962418` → `sub_82940E50`): key = index in
  a 140-entry key table (`0x830B5D48`), value = `int(axis * 127)`; 4 fixed
  slots for the sticks plus up to 4 more keys (count at record +316).
- Replay (`sub_829478B0`, on both machines, for both players) feeds the records
  back as `InputAxis(controller, key, int / 127)`; the player manager
  interaction (`sub_8268E740`) routes them to the player's controller
  (GEngine `[0x8311F58C]` +772 → viewport client, vtable +268 → local player,
  +64 → PlayerController).
- Sent to the partner, an axis keeps only its **low byte** (signed).

The native mouse aim (`src/mouse_aim.cpp`) used to write the camera rotation
directly, which desynced the game ~15 s into every level. Now, in a network
game, the turn of each frame (with the sender's sensitivity, zoom and invert)
is put into the record as the keys `MouseX` / `MouseY` (key table entries 24 /
25), encoded in one byte: steps of one rotation unit up to 32, then 5 % larger
per code up to ~18 deg per frame; what a code cannot hold is carried to the
next frame. Each machine takes the replayed values in `sub_8268E740` and adds
them to that player's rotation in `UpdateLookingAction` on the same frame.
`--ao2_coop_test_local=true` runs this path in single player (the frame sync
replays the player's own input there too); `--ao2_mouse_debug=true` logs it.

### Finding a desync (`src/coop_sync_trace.cpp`)

The checksum words of a frame: +44 = the shared `rand()` the frame sync
tick draws at its start (XORed in, so it is the stream's state after the
last frame), +46 = the physics hash; the tick builds this machine's record of
the frame with them (`sub_82962528`, r4 = frame), and `sub_8295D1B0` compares
the record with the partner's record of the same frame (record +0 / +2) and
sets +52. +44 / +46 are cleared at the end of every tick. Once the stream or
the physics differ they never agree again, so the first mismatch is long
after the cause.

The lead (input delay) is the same on both machines: `[+260]` (latency) and
`[+212]` (frame ms) are computed in `sub_8295D1B0` from the replayed records
(the largest latency of the frame's records, smoothed over 5; the players'
frame times averaged over 10), so both machines replay a frame's records on
the same simulation frame. Checked on a real two-PC game: identical
latency / lead values on the same frames.

`--ao2_coop_sync_trace=true` (on both machines) writes `sync_trace.log`: per
lockstep frame the checksum words, the `rand()` state, how many numbers the
simulation took (and how many extra draws took - put back by decouple.cpp),
each controller's look rotation and pawn location / rotation / health, and
who took the numbers (caller address, or for UnrealScript's `Rand()` /
`FRand()` the running function and its object's class).
`python tools/sync_diff.py <A sync_trace.log> <B sync_trace.log>` prints the
first frame where anything differs, the lines around it and the callers that
differ.

### Streaming levels and the buoyancy wave (`src/coop_streaming.cpp`)

A desync on the China hovercraft: the streaming level `07_map_river_b_Shell`
is asked for and loaded on the same lockstep frames on both machines, but UE3
adds a loaded level to the world a slice of components at a time against real
time, so it became visible a few frames apart (9078 / 9084). Its floating
`AO2KActor`s' `AO2BuoyancyComponent`s then start ticking (the component tick
loop `sub_827A6410`), and their wave (`sub_823AC368`) draws two `appFRand` per
tick from the lockstep's `rand()` stream: the rand half of the checksum
splits, the physics half stays equal. Forcing the streaming to block
(`bShouldBlockOnLoad` / `bRequestedBlockOnAsyncLoading`) stalled the level
load, so `ao2_coop_block_streaming` is off; instead the wave runs with the
game thread's `rand()` state put back afterwards
(`ao2_coop_isolate_buoyancy_rand`, on). The sync trace's `K` and `L` lines
(floating actors, streaming levels) showed it.

The same thing later split the stream through other actors of a level that
appeared early on one machine (`07_map_canal_lock_m` 9977 / 9973: enemies'
`PostBeginPlay`), so the cause itself is fixed too
(`ao2_coop_whole_level_visibility`, on): `UWorld::AddToWorld`
(`sub_82984A40`, called by `UpdateLevelStreaming` `sub_829B4EE8`) works in
steps - move the actors, update the actors' components against a time budget
(`sub_827C4798`, budget in `f1`, spent time summed in the double at
`0x831F2CD0`), BSP components 50 at a time, the rest - and sets
`LevelStreaming.bIsVisible` (+96, `0x80000000`) after the last one. In a
network game's lockstep it is called again until the level is visible (or
another level holds `UWorld+88`, the level being made visible), as the
engine's `FlushLevelStreaming` does: the level appears whole on the step its
load finished. `RemoveFromWorld` (`sub_82978CD0`) is not time-sliced.

### Garbage collector and the audio listeners (`src/audio_listener_guard.cpp`)

The co-op garbage collector crashes (`sub_82531730`, 2026-10-08..10, with
and without the F1 menu) all led to `XeAudioDevice_0`
(`[[GEngine+756]+152]`): its `Listeners` (+136, `FListener` 52 bytes,
`PortalVolume` at +0) is followed by the collector like any reference, and
on the joining machine it had two listeners, the second one's
`PortalVolume` a dead pointer (6; earlier text and floats such as
`0x42860000`). Before every collection a listener volume that is not a live
`PortalVolume` (`GObjObjects[obj+4] == obj`) is cleared
(`ao2_audio_listener_guard`, on; `audio_listener_guard.log` when it fires).
The crash report (`hang_watchdog.cpp`) also lists the collector's queue
(`collector queue`) to name who queued a bad object.

### Input delay

The lockstep lets each machine collect input ahead of the frame it simulates
by a delay, so every input (stick or mouse) takes effect that much later:
`sub_8294BE68` computes it as latency `[sync+260]` + 50 ms, clamped to
`[+68]..[+72]` (10..500 ms), in frames of `[+212]` ms (16 at 60 Hz) - 3
frames even on one PC, 5-7 over a VPN. `ao2_coop_input_margin_ms` (default
0, `src/coop_input_delay.cpp`) replaces the 50 ms margin; the delay is kept
at one frame or more (`[+212]` follows the real frame time, 16-18 ms, and a
delay rounding down to 0 frames deadlocked both games). On a LAN that is 1
frame. Keep the value the same for both players (a 16/50 run desynced). A
smaller margin means the game waits for the partner when the network
jitters more. `--ao2_coop_debug=true` logs `latency=`, `frame=` and the
resulting `input delay=` on the `sync tick` lines.

The mouse turn is taken from the frame record when it is replayed
(`sub_829478B0`: axes at record +248, 8 bytes each, count at +312; controller
id at +592 online, +588 locally), not from the viewport's input chain: during
the co-op snipe the countdown UI took `MouseX`, so the yaw stayed centered
until both players had aimed.

The F1 debug menu works online through the same input (`src/coop_cmd.cpp`):
its command text ("god 1", "checkpoint 05_04 2") rides in the player's frame
record as key presses - blocks of `[F11] [tag] [seq] [n/start] [sum]
[n symbols]`, every value a key table index, symbols below 100 (never F11
or F12) - and both machines run it when they replay that record. A record is
not always what was put into it (a changing input delay merges or drops
frames: the first test command arrived with two blocks swapped), so one
block is in flight at a time, the next goes once this machine has replayed
its own block (by tag), and it goes again after 30 frames; the receiver
checks the sum and the sequence. The blocks are taken out before the game
and the cutscene skip see the record and put back afterwards. God mode is per
player (that record's controller); a checkpoint loads for both with the
lobby's URL (`Checkpoint?LoadSaveGame?CheckpointToLoad=<name>?Difficulty=<d>?`),
so both PCs need that checkpoint (DLC included). The load runs at the next engine pad poll, not inside the replay: started there it tore the level down while the frame sync was still replaying the other player's record, and the garbage collector crashed a minute later. The load itself goes through the co-op manager (`[0x831F3024]`, `sub_82A16488`), as the game's own co-op checkpoint restart (`sub_82A168B8`, after both players voted) does: the plain script travel event on both machines left references to the old level in the HUD and the audio device. Only while the lockstep
runs; `coop_cmd.log` shows each block, each record with one (key indices)
and what ran on which frame. `--ao2_coop_test_local=true` runs it in single
player.

Other local-only actions (the cutscene skip key, cheats) are off in a network
game: `ao2::coop::InNetworkGame` is true from EA's game manager starting
(DirtySock output `GameManager listening`) until the game is destroyed - a
level intro movie skipped on one machine before the lockstep started
desynced the game.

### Connection quality, adaptive delay and rejoin (`src/coop_net_status.cpp`)

The link (`src/coop_link.cpp`) sends a `Quality` message five times a second
during a network session: a time stamp, the echo of the partner's last one
and how long it was held (so each side measures its own round trip over the
same route as the game's traffic), the frames the sender waited for its
partner in the last second, and its input margin wish.

- **Overlay** (`ao2_coop_net_hud`: 0 off, 1 only on trouble (default), 2
  always), top right in the frame rate overlay's layer: ping, input delay in
  frames (+ the adaptive margin), waits per second here and at the partner;
  warnings for a partner silent over 1.5 s, checksums out of step for 30+
  frames (frame sync +308; the game leaves at 600) and 10+ waits a second.
- **Adaptive margin** (`ao2_coop_input_auto`, on by default,
  `src/coop_input_delay.cpp`): 6+ waits in a second on this PC while the
  partner waits too (when only one PC waits, the other is just slower) wish
  for 8 ms more, at most 64 ms, no more often than every 3 s; 15 s with at
  most one wait a second, 8 ms less. A wish holds from a lockstep frame 120
  frames ahead and goes to the partner with that frame; both PCs use the
  larger wish from its frame on, so the margin stays the same on both, as
  `SharedSettings` asks of `ao2_coop_input_margin_ms`. `coop_delay.log` has
  each wish and each change in force.
- **Rejoin** (`ao2_coop_rejoin`, `ao2_coop_rejoin_minutes` = 5): a game
  that ends its network session says `bye` over the link. When a session
  ends within 5 s of the lockstep running and the partner had been silent
  for 3 s without a bye (a crash, a lost connection), or the checksums had
  been out of step for 300+ frames, the game puts up a notice and
  `ao2::invite::OnPartnerLost` takes over: the host's next two-player co-op
  lobby invites the partner's address (again every 10 s until it is in),
  and the partner's game accepts an invite from the host's address by itself.
  Not for versus. `coop_net.log` shows each session's end and the decision;
  `invite.log` the rejoin.

### SDK fixes this needed

- `WSAWaitForMultipleEvents` passed `fWaitAll` as the NT wait type (inverted:
  NT 0 = all, 1 = any) and the timeout as absolute NT time. DirtySock's receive
  thread waits for any of its events while holding a lock, so closing a socket
  deadlocked the game.
- `XUserMuteListQuery` (`0x0005800E`) is answered.
- `net.log`: wall-clock timestamps, every XGI/XLiveBase message, per-socket
  traffic every 5 s, socket/bind/close details.

### Functions only reached through pointers

Leaving the co-op menus crashed on an indirect call to `0x82275258`, a
callback the recompiler never saw. `scan_missing_funcs.py --data` (dev tools) finds
code addresses referenced from lis/addi pairs, vtables and dispatch tables that
are not function entries; 220 were added to `default_functions.toml`.

### Versus bots (`src/versus_bots.cpp`)

A versus match can start with fewer than 4 players (`ao2_versus_min_players`,
`src/versus_lobby.cpp`); bots play the empty places (`ao2_versus_bots`, on by
default - every player of the match needs the same setting). In the ready
room Swap team goes over at once to a free place of the other team
(`ao2_versus_free_team_swap`, done by the host), also while others are ready
and for each of a split-screen pair (the game refuses a pair every swap, and
its guest is ready at once); with that team full the game's swap stays: two
players of both teams who both ask. Versus is the
same lockstep: a bot is input, made on every machine alike from the
simulation's state and replayed right after the first record of each frame
(only inside the replay does the viewport hand input to the controller). The
bots walk along a network built from the level's navigation points and the
AI's cover nodes (`AO2CoverNode`: the versus maps have few navigation points),
shoot enemies in sight and log to `versus_bots.log`. Its links are checked with
traces against the collision characters walk against: a small box (a bullet's
line goes through invisible walls), a slope's terrain bulge passed over, and a
vault (Y) only through an `AO2GameplayMechanicsVolume` - nothing else can be
vaulted. A bot that gets no nearer to its goal along its path bars that link
for a while and backs off. Y is a context button (vault, roll, slide into
cover; a dodge while aiming): pressed during a roll it can start a vault that
leaves the character rolling for good (`Suppression_Rolling_InRoll` waits for
its animation), so bots never press it while rolling or vaulting. Bots have
infinite ammo (`ao2_versus_bots_infinite_ammo`), wait to be healed when down
and heal partners that lie. Tested on one PC (host with three bots, Iraq); a
match with other players and bots is not tested yet.

### Versus split-screen (`src/versus_guest.cpp`)

The game has split-screen versus the console way: the second player of a
machine is the first one's guest (the lobby's Y, Guest Sign-In; not in ranked
matches). The guest is a player of its own in the lobby's slots (+20 set,
the name in brackets) with its own id, on the host player's team, and the
match splits the screen (`AO2MPGame.IsSplitScreen`, the MP HUD items). The
game's sign-in handler (`sub_82AF4B28`, on XN_SYS_SIGNINCHANGED) takes a user
to the lobby's guest handler (`sub_82A2F190`) only when its
XUserGetSigninInfo (`sub_82B07E10`) has `XUSER_INFO_FLAG_GUEST` (info +8,
2); the runtime signs user 1 in while a device is assigned to it
(`input_player2`) but never as a guest. `ao2_versus_guest` (on) makes the
users 1-3 signed in when the guest sign-in UI opens (`sub_82A209D0`,
`sub_82A055C8`) guests until their sign-in state is 0 or this PC has been out of every
lobby and match for 2 s; `versus_guest.log` says when. On the console the guest brings its own controller; here the
automatic assignment gives every device to player 1 (the launcher's
`input_share_pads`), so with nobody else signed in the sign-in opens the
window of `src/split_join.cpp`: each player presses a button on their device
(`rex_input_split_players`), and the user becomes a guest once the runtime signs
it in (its 250 ms device poll sends XN_SYS_SIGNINCHANGED again). Closing the
window leaves no guest. The devices go back after 2 s out of every lobby and
match (the Plasma lobby's +2793 / +2794, which the guest handler wants set: 0 in
the main menu, 1 in the lobby).

With two views in the frame the aggro meter is shared (only the lower part draws it, at
the seam). Players on different teams (`PlayerReplicationInfo.Team`) each get a whole
screen's meter on their part instead (`src/split_hud.cpp`).

The campaign's split-screen (`src/split_join.cpp`) has no guest. The Splitscreen
button's sequence counts the signed-in players first
(`UIAction_GetLoggedInPlayerCount`, "needs two players"), then maps the second
one (`UIAction_MapSecondController`). The window opens at the count, and player
2 is signed in with the keyboard for now so that the sequence goes on. Campaign
Mode's buttons run `causeevent MissionSetup_<button>`, which reaches the script
function `PlayerController.CauseEvent` of every PlayerController (not
`ULocalPlayer::Exec`; caught at `UObject::ProcessInternal`, `sub_82445C30`, by
the FFrame's Node +4, the name in its Locals +16). Splitscreen is held back and a window opens: player 1 presses
a button, then player 2, and each gets what they pressed on (a key or mouse
button: the keyboard and mouse; a pad button: that pad;
`rex_input_split_players`). The command then runs again and goes through once
player 2 is signed in (up to 600 ms). Another of those buttons or the main menu
after a level gives the devices back. `split_join.log` says what happened.

The match numbers the local players by their places (a guest on a place
before the host's makes the guest's LocalPlayer GamePlayers[0] and gives the
others other ControllerIds), and they stay so after it. At the same 2 s
GamePlayers gets back the order and the ControllerIds it had at the guest
sign-in. The main menu's screen is laid out before that, for the match's
numbering: the LocalPlayer that had the host's id keeps the screen (Origin
0,0) and GamePlayers[0], the keyboard's, is off it (Origin 1,0), and its
scenes are neither drawn nor updated - in the campaign's split-screen
Select Mission / New Game opened a scene with nothing bound and nothing
focused. Until the next level GamePlayers[0] takes the part of the screen
another player has (they swap Origin and Size); laying out again
(`PlayerController.bViewportLayoutDirty`) gave the same layout.
`--ao2_script_trace=true` (`src/script_trace.cpp`) logs the UnrealScript
functions the main menu runs (`UObject::ProcessInternal`, `sub_82445C30`) to
`script_trace.log`: two runs side by side found this. Tested on one PC: host
and guest against two bots (Iraq), then the campaign's split-screen. A guest with
players of other PCs is not tested yet.

Steam Input takes the pad: with `ao2_steam` on (SteamAPI_Init as Spacewar,
480) and Steam Input on for it, SDL lists the Bluetooth Xbox pad but
`SDL_OpenGamepad` fails ("Failed to obtain XInput device capabilities");
without SDL's XInput backend (`SDL_XINPUT_ENABLED=0`) it is a joystick
without a gamepad mapping. Steam's overlay (`gameoverlayrenderer64.dll`,
loaded by SteamAPI_Init) hooks the XInput DLLs by name: XInputGetState says
"not connected" for every slot, and Steam gives a game it did not start no
virtual pad either (`SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=1` was
tried: nothing). RawInput, Windows.Gaming.Input and HIDAPI see nothing too.
A copy of `XInput1_4.dll` under another name is not hooked, so the runtime's
SDL driver (`UseUnhookedXInput`, `hid_xinput_past_steam`, on by default)
copies it to `%TEMP%ex_xinput1_4.dll` and points SDL's XInput entry points
there once SDL's gamepad subsystem is up. With that off, Steam's Spacewar →
Controller → Disable Steam Input, or `ao2_steam=false`, gives the pad back. The runtime's SDL
driver also opens the pads SDL lists whose `SDL_EVENT_GAMEPAD_ADDED` never
came (it pumps SDL's events only for pads it knows, so it had none).

For tests without a second gamepad: `--input_player2=keyboard` signs user 1
in, and `p2` in `input.script` sends the taps after it to user 1.

## Testing on one PC

Dev scripts (in `C:\aot2\tools`, outside this repo): `coop\coop_start.ps1` starts two instances (`C:\aot2\work\runA` hosting on
127.0.0.1, `C:\aot2\work\runB` joining from 127.0.0.2, windowed, scale 1);
`coop_titles.ps1` labels the windows; `coop_snap.ps1 -Inst A -Name x` saves a
front buffer; `coop_flow.ps1` drives both through the menus (scripted input is
unreliable in the second window - driving them by hand works better).
`threadstacks.py <pid>` prints all guest thread stacks (deadlocks).
