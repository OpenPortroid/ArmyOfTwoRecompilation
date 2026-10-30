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

1. Host: *Campaign Mode → Public Co-op Game*, pick map and difficulty → "Finding
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

### Private Co-op Game (invites)

1. Host: *Campaign Mode → Private Co-op Game* → the lobby → *Invite*: the
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
  `REASON=2` (state 0, `sub_822424D0`).
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

`invite.log` (with `log_to_files`) shows players found, invites and answers.

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
default - every player of the match needs the same setting). Versus is the
same lockstep: a bot is input, made on every machine alike from the
simulation's state and replayed right after the first record of each frame
(only inside the replay does the viewport hand input to the controller). The
bots walk along a network built from the level's navigation points and the
AI's cover nodes (`AO2CoverNode`: the versus maps have few navigation points),
shoot enemies in sight and log to `versus_bots.log`. Tested on one PC (host
with three bots, Iraq); a match with other players and bots is not tested yet.

## Testing on one PC

Dev scripts (in `C:\aot2\tools`, outside this repo): `coop\coop_start.ps1` starts two instances (`C:\aot2\work\runA` hosting on
127.0.0.1, `C:\aot2\work\runB` joining from 127.0.0.2, windowed, scale 1);
`coop_titles.ps1` labels the windows; `coop_snap.ps1 -Inst A -Name x` saves a
front buffer; `coop_flow.ps1` drives both through the menus (scripted input is
unreliable in the second window - driving them by hand works better).
`threadstacks.py <pid>` prints all guest thread stacks (deadlocks).
