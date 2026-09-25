# SOCOM: Combined Assault -- commandable AI teammates in co-op LAN (AI-assisted)

**STATUS: DESIGN AND PROBE PLAN. NOTHING IN THIS DOCUMENT HAS BEEN MEASURED.**

There are no addresses here, because none have been taken. Every address in the companion
scaffold (`SCUS-97545_D7CFDCCF.pnach`) is a named placeholder, and every `patch=` line in it is
commented out. The file is inert until someone fills it in from a debugger session on a real
disc. Nothing here should be quoted as fact about the game; it is a plan for finding the facts.

Serial `SCUS-97545`, CRC `D7CFDCCF` (NTSC-U retail), per `bin/resources/GameIndex.yaml:12797`.

---

## What is being asked for

Two humans in a co-op LAN mission: **Specter** (player 1) and **Jester** (player 2). Add an
AI-controlled **Bravo element** that can be given orders, with command authority held by Specter,
and handed to Jester if Specter goes down.

Two features, not one, and they cost wildly different amounts:

| | Feature | Where it runs | Cost |
|---|---|---|---|
| A | AI element exists and takes orders from Specter | Host only | Cheap **if** the AI already exists in co-op |
| B | Authority fails over to Jester when Specter dies | Crosses the network | Expensive, and the shape depends on a probe result |

---

## Why this is not an emulator change

Nothing in this fork touches game logic. The SOCOM files here -- `bin/SCUS-97545.conf` and its
siblings -- are RTX Remix renderer knobs: froxel distance, hold-empty, sky culling, light scale.
They are read at frame boundaries and pushed through `remixapi_SetConfigVariable`. They cannot
spawn an entity or issue an order, and no amount of work on them will get closer to this.

The lever PCSX2 does give you is the pnach patcher, and for this job it is enough:

- `patch=<place>,EE,<addr>,bytes,<hex>` writes an arbitrary blob (`pcsx2/Patch.h:74`,
  `pcsx2/Patch.cpp:928`). That is the **code injection primitive**: put MIPS into a hole in EE
  memory, then overwrite one instruction at a hook site with a jump to it.
- `place` is `0` once on load, `1` every vsync, `2` both, `3` on load or when enabled
  (`pcsx2/Patch.h:48-56`). Code injection wants `0`; a state machine that must be re-asserted
  wants `1`.
- `extended` gives Action-Replay-style conditional codes (`pcsx2/Patch.cpp:1225`).
- `dpatch` does signature-matched patching (`pcsx2/Patch.cpp:999`), so a patch can survive an
  address moving. Worth switching to once the hook sites are known and stable.

So the deliverable is a `.pnach` file, developed against a disc the maintainers of this repo do
not ship and must not ship. Per `AGENTS.md`: no game images, no ELF dumps, no disc-derived data in
this repository. Addresses and our own injected code are fine; game code is not.

---

## The one unknown that decides whether any of this is possible

**Does Bravo exist as AI entities during a co-op mission at all?**

Single player and co-op almost certainly share one ELF, so the AI and ordering code is linked in
either way. The question is whether the co-op mission scripts *instantiate* the element.

- **If yes** -- co-op spawns Bravo but withholds the command menu -- then feature A is
  re-enabling a gate, and this whole project is a few days of careful work.
- **If no** -- co-op strips the element -- then feature A means making an MP build spawn and
  network-replicate entities it was never built to carry, inside whatever fixed entity and
  network-slot budgets the MP path allocates. That is a different and much larger project, and
  it may simply be a dead end. Find this out before designing anything else.

Step 2 of the probe recipe below answers this, and it is the highest-value twenty minutes in the
whole plan.

---

## The netcode constraint, and why the design has to change shape

SOCOM's multiplayer is host/client, not lockstep: one console is authoritative and the other is a
client receiving replicated entity state. (This is the assumption the whole design rests on --
**needs proper testing**, and step 4 of the probe recipe is how you confirm it.) AI entities are
simulated on the host and appear on the client as ordinary replicated entities.

Consequences, in order of how much they hurt:

1. **Specter commanding is nearly free -- if Specter hosts.** His orders execute on the box where
   the AI actually lives. Make "P1 hosts" a hard requirement of the setup; it removes an entire
   class of problem for free.
2. **Jester cannot command by running the same code locally.** There is no AI on his box to
   command. Orders issued there would either do nothing or diverge from the host's simulation.
3. **So authority failover needs P2's intent to reach the host.** The obvious route -- add a new
   client-to-host message type -- is the expensive one. It risks the version/CRC handshake and
   anything downstream that assumes a packet layout, and the failure mode is a disconnect or a
   desync rather than a clean error.
4. **Both instances must run byte-identical patches, always.** Not "equivalent". Identical.

### The trick that may avoid adding a message type

The host already receives P2's input every tick -- it has to, because it is authoritative over
P2's avatar. **If that stream carries pad bits, the host can read Jester's command-menu selection
straight out of it.** The menu stays local UI on P2's box, decoded entirely host-side, no new
packet, no handshake risk, no change to any structure that goes over the wire.

If the stream carries cooked movement vectors instead of raw buttons, this does not work directly
and the cheap version of feature B dies with it. Probe step 4 settles it. Do not design feature B
before running that step.

### Why the Rainbow Six 3 comparison does not transfer

In RS3 co-op, both players commanding the AI is retail behaviour, so that game already has a
networked order path -- somebody shipped and debugged it. Combined Assault is a different engine
from a different studio, and if its retail co-op never lets a non-host issue an order, then no
such path exists to re-enable. The RS3 precedent proves the *feature* is reasonable. It says
nothing about the amount of work in this game.

---

## Milestone ladder

Each rung is independently useful and independently abandonable. Do not start a rung before the
one above it has produced a result.

**M0 -- Probe.** Answer three questions: does Bravo exist in co-op, where is the order dispatch
function, and what does the host receive from P2. No patch is written. See the recipe below.

**M1 -- Leader swap on death (cheap feature B, no orders).** On Specter's death, host-side, point
the AI element's leader reference at Jester's avatar. Bravo then follows, covers and reacts to P2
instead of standing over a corpse. This is a pointer write inside the host's own AI state: no new
packet, no client-side code, no UI. It delivers most of what the failover is *for* at a small
fraction of the cost, and it is worth having even if M2 and M3 never happen. Start here.

**M2 -- Specter's command menu (feature A).** Un-gate the command UI for the host and route its
orders into the dispatch function found in M0. Host-only. If the M0 probe found Bravo present in
co-op, this is the gate flip; if not, stop -- re-read the unknown above.

**M3 -- Jester's orders (full feature B).** Only if probe step 4 found pad bits in the input
stream. Decode P2's selection host-side and feed the same dispatch function, gated on the
authority state machine.

### The authority state machine (M2/M3)

Lives on the host, and needs its rules pinned down before it is written, because "dies" is not
one state in this game:

- Specter alive -> authority Specter.
- Specter dead or incapacitated -> authority Jester.
- Specter revived or respawned -> **decide and document**: does authority snap back, or stay with
  Jester until he goes down too? Snapping back is simpler to implement and more surprising to
  play. Recommend: authority returns to Specter, because it keeps the state derivable from live
  player state alone with nothing to persist.
- Both down -> mission-failure path probably owns this already; do not fight it.

Derive the state from player state every frame rather than storing it. A stored flag has to be
reset correctly on mission restart, checkpoint reload and host migration, and each of those is a
bug waiting to be written.

---

## The probe recipe (M0)

Everything needed is already in this tree: `pcsx2-qt/Debugger/Memory/MemorySearchView.cpp`,
`pcsx2-qt/Debugger/Breakpoints/`, `pcsx2-qt/Debugger/DisassemblyView.cpp`,
`pcsx2-qt/Debugger/SymbolTree/`, and `pcsx2/DebugTools/Breakpoints.cpp`.

**Step 1 -- find the AI squad state, single player.** Start a mission with Bravo present. Search
memory for a known changing value belonging to a Bravo member (health is easiest: take a known
amount of damage and narrow the search). Keep the address; save state.

**Step 2 -- ANSWER THE BIG UNKNOWN.** Boot the same mission in co-op LAN and look at the same
region, on the host and then on the client. Is the element instantiated? This is the gate on the
entire project. Record the answer in this file before doing anything else.

**Step 3 -- find the order dispatch function.** Back in single player, set a write breakpoint on
the order or state slot inside the AI structure, then issue an order from the command menu. The
breakpoint lands you in, or one frame from, the dispatcher. Disassemble outward to find its entry
point and calling convention. This is the function every later milestone calls.

**Step 4 -- what does the host receive from P2?** On the host, set a write breakpoint on P2's
avatar input field and read what arrives each tick. Raw pad bits mean M3 is viable as designed.
Cooked vectors mean M3 needs a new message type, which is a project of its own -- stop and
re-scope rather than pushing on.

**Step 5 -- find the command menu gate.** Breakpoint the menu-open path in single player, then in
co-op find the branch that suppresses it. That branch is M2's patch site.

**Step 6 -- find a hole for the trampoline.** Scan for a run of zeros, in EE memory, that stays
zero across a whole mission including a checkpoint reload. **Do not guess at a scratch region**:
a hole that is only usually free produces a corruption bug that reproduces once an hour and
costs a week.

### Fill this in as you go

| Placeholder in the pnach | What it is | Value | How it was found |
|---|---|---|---|
| `ADDR_AI_ELEMENT_BASE` | Base of the Bravo element / squad structure | | |
| `ADDR_AI_LEADER_PTR` | The leader/follow reference the element reads | | |
| `ADDR_AI_ORDER_SLOT` | Where an issued order lands | | |
| `ADDR_ORDER_DISPATCH_FN` | Entry point of the order dispatcher | | |
| `ADDR_P1_STATE` | Specter's alive/health/incap state | | |
| `ADDR_P2_AVATAR_PTR` | Jester's avatar pointer, host-side | | |
| `ADDR_P2_INPUT_FIELD` | Where P2's replicated input lands, host-side | | |
| `ADDR_MENU_GATE` | Branch that suppresses the command menu in MP | | |
| `ADDR_SCRATCH` | Verified-free EE region for trampolines | | |

Record for each: how it was found, and whether it survived a checkpoint reload and a full mission
boot. An address that only holds within one session is not an address.

---

## Delivering it: the GUI toggle

Your friend does not need to build anything, edit anything, or read this document. One file, one
checkbox.

1. Drop `SCUS-97545_D7CFDCCF.pnach` into the `cheats/` folder of their PCSX2 install (the Cheats
   folder shown in Settings -> Folders; `EmuFolders::Cheats`).
2. Settings -> Emulation -> **Enable Cheats** must be on (`EmuCore/EnableCheats`).
3. Right-click SOCOM in the game list -> Properties -> **Cheats**. Each labelled group in the
   pnach appears as its own checkbox with its author and description
   (`pcsx2-qt/Settings/SettingsWindow.cpp:142`, `pcsx2-qt/Settings/GameCheatSettingsWidget.cpp:339-345`).
   Labels use `\` to nest, so the groups arrive as a tree under one `SOCOM Co-op AI` parent
   (`pcsx2/Patch.cpp:547-563`).
4. Ticked groups are saved per game as `[Cheats] Enable` in
   `gamesettings/SCUS-97545_D7CFDCCF.ini` (`pcsx2/Patch.cpp:120-121`).

**Both players must tick exactly the same boxes.** Different sets of enabled groups is the same
thing as running different patches, which is the same thing as a desync. Agree the list, then have
both people read it back before launching.

Two things that silently swallow the whole feature:

- **RetroAchievements Hardcore Mode disables every cheat**, with one OSD message and no other
  sign (`pcsx2/VMManager.cpp:3165-3172`). If nothing happens, check this first.
- Patch lines placed *before* any `[Label]` are "unlabelled" and activate automatically whenever
  cheats are on, with no checkbox (`pcsx2/Patch.cpp:408-438`). The scaffold has none, and must
  never gain any -- that is how you end up with a patch running on one box and not the other.

### Hazard: the per-game settings overlay

The Cheats page and the **RTX Remix** page are in the same per-game properties dialog
(`SettingsWindow.cpp:142` and `:162`). Per `bin/SCUS-97545.conf:229-237` and `:505-512`, merely
visiting the Remix page re-serialises the whole GUI knob state into
`gamesettings/SCUS-97545_D7CFDCCF.ini`, that overlay outranks the `.conf` because it reaches the
backend through the environment first, and it has silently re-broken this title's render config
more than once -- `MATSTAGE = 1` gave a fully untextured game, `LIGHTMODE = 2` gave a light
welded to the camera.

**Tick your cheats in the Cheats tab and do not click the RTX Remix tab while you are in there.**
If someone does, check the `[Remix]` section of that overlay afterwards and clear it.

---

## LAN setup notes

- DEV9 needs a PCAP mode -- bridged or switched (`pcsx2/DEV9/pcap_io.cpp:51-53`). The internal
  sockets stack (`pcsx2/DEV9/sockets.cpp`) is a NAT-like shim aimed at reaching the internet, not
  at two PS2s finding each other.
- **Two instances on one PC may never see each other.** `pcsx2/DEV9/pcap_io.cpp:154` carries an
  open TODO: broadcast packets are not looped back to the host PC in switched mode, and LAN game
  discovery is broadcast. Two PCs on one switch is the setup to test on; one PC with two instances
  is a setup where a failure tells you nothing about your patch.
- Distinct MACs and IPs per instance.
- Keep this on LAN. Do not take a modified client onto a fan server: it is somebody else's server,
  the patch changes game logic, and a mismatched client is at best a disconnect for everyone in
  the lobby.

---

## Desync rules, for whoever writes the patches

1. Identical pnach, identical enabled groups, both boxes. Verify per session.
2. **Never change the size or layout of anything that goes over the wire.** Not one byte. The
   handshake, the entity stream, and every offset downstream assume what shipped.
3. Simulate AI on the host only. If a patch makes the client compute AI state, it is wrong, even
   if it looks right on screen.
4. Read authority from live player state; store nothing that a mission restart could leave stale.
5. Test every change against: mission restart, checkpoint reload, Specter dying, Jester dying,
   both dying, and Specter dying while an order is in flight.
6. Preserve registers at every hook site. Check what the displaced instruction and its
   neighbours rely on in the disassembler before choosing scratch registers -- a clobbered
   register is the classic injected-code bug and it surfaces far from the hook.

---

## What could kill this

- Co-op does not instantiate Bravo (the unknown above). Largest risk by far.
- P2's input reaches the host as cooked vectors, not pad bits -- M3 needs a new message type.
- MP entity or network-slot budgets leave no room for AI even if the spawn path can be reached.
- The co-op HUD, scoreboard and respawn logic assume squad slots map to network players, and AI
  in those slots confuses them.
- A data-integrity or version check refuses a modified client to a modified host.

M0 and M1 are worth doing regardless: M0 is pure information, and M1 stands on its own.
