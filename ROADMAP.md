# Mserver4 Roadmap

## What Mserver4 is

Mserver4 is the unification of the **N_Rawit** ecosystem — a set of separate
ESP32 experiments imported from the `norrawitro` account (see
[`norrawitro/README.md`](./norrawitro/README.md)) — into one working
product: a self-hosted, offline-capable mesh-networking device (the
**"N_Rawit Box"**) that streams content, mines tokens, watches your home,
controls IoT devices, and stores your data — all without an ISP.

This roadmap turns those separate proofs-of-concept into phased,
buildable milestones, and says where each imported repo fits.

## Phase 0 — Foundation & Consolidation
*Goal: one buildable project instead of ten disconnected sketches.*

- Pick one target board and toolchain (source material points at
  ESP32-S3, 16MB flash / 8MB PSRAM) and set up a single PlatformIO project
  at the repo root, seeded from `norrawitro/N_Rawit_server_node/`
  (already PlatformIO-based).
- Reconcile the duplicated vision text between
  `norrawitro/-N_Rawit_OS/README.md` and
  `norrawitro/N_Rawit_server_node/README.md` into one design doc.
- Decide which imported sketches are in scope for v1 vs. reference-only
  (`hello-world`, `Multi-way-Tree-10x10-` are docs/examples, not code to
  ship).

## Phase 1 — Data Box Engine (storage core)
*Goal: the "N_Rawit Data Box" format works end-to-end on one device.*

- Implement the 2,048-byte Data Box format (Header / Security / Payload)
  described in `N_Rawit_server_node/README.md`.
- Build the tiered memory manager (SRAM → PSRAM → Internal Flash → SD
  Card), drawing on the storage work in `norrawitro/NRB_ESP32S3/`.
- Implement the tree-branching / synapse-link system for chaining Data
  Boxes — this is the concept `norrawitro/Branching-type-data-storage`
  was reserved for upstream but never filled in, so it starts from
  scratch here.
- Use the `norrawitro/Multi-way-Tree-10x10-` taxonomy as the tagging
  scheme for classifying stored content.

## Phase 2 — OS & Local UI
*Goal: a device you can operate without the network being up.*

- Bring up the OS/GUI shell from `norrawitro/-N_Rawit_OS/` (menu bar,
  Data Box picker, program runner) on top of the Phase 1 storage engine.
- Add a cut-down build for cheaper hardware using
  `norrawitro/N_RAWIT_EASY_CODE_BOX/` (ESP32-C3 + OLED) as the low-end
  target.
- Wire up local input using `norrawitro/CYD_mouse_and_Ketboard/` (HID
  mouse/keyboard) as the reference input peripheral.

## Phase 3 — Mesh Networking & Web Server
*Goal: two or more nodes can talk, relay, and serve pages to a phone.*

- Bring up the WiFi Mesh + web server from
  `norrawitro/N_Rawit_server_node/` (`web_server.cpp`, `main.cpp`).
- Implement node roles (Gold / Silver / Mobile) and the channel-0
  beacon / negotiate-a-channel handshake for transferring large payloads.
- Implement the RSSI + GPS based routing so multi-hop relay works.

## Phase 4 — Token Economy & Security
*Goal: sharing data has a cost and a reward, and the network can catch
tampering.*

- Implement the token ledger (`db_manager.cpp`/`.h`) — earn/spend rules
  for Host, Relay, Consumer, and Mobile roles.
- Implement the SHA-256 hash-chain integrity check and the
  neighbor-jury self-healing process for detecting a compromised node.
- Add the posting fee / anti-spam and proof-of-stake-for-content rules.

## Phase 5 — Intelligence Layer
*Goal: the box can see and understand, not just store and forward.*

- Bring up `norrawitro/RHEA_BRAIN/` (WiFi AP mode + SD card web server)
  as the vision/AI module, feeding classified results back through the
  Phase 1 taxonomy.
- Add the AI-CCTV feature described in the vision doc (still-image mode
  to limit bandwidth).

## Phase 6 — Companion Devices
*Goal: the ecosystem extends past one box.*

- Bring up `norrawitro/Smart-watch-ESP32-S3-X5-/` as a Mobile Node /
  companion client that can carry data between disconnected zones
  ("Data Mule").

## Phase 7 — Social Layer & Long-Term Vision
*Goal: the feature set the original vision doc describes as the endgame.*

- Social-style feed/composer UI, follow/visibility settings, membership
  tiers.
- Revisit the funding model (token economy proceeds funding future
  hardware — the "N_Rawit Robot" ambition from the source vision doc).
  This phase is intentionally speculative and should be re-scoped once
  Phases 1–4 are real and running.

## Out of scope / reference only

- `norrawitro/hello-world/` — kept as the minimal build/flash smoke test,
  not part of the product.
- `norrawitro/Branching-type-data-storage/` — was empty upstream; folded
  into Phase 1 rather than tracked as its own module.

## How to use this roadmap

Each phase should become its own set of issues/PRs against this repo.
Nothing here is committed to a date — it's an ordering of dependencies
(you need Phase 1's storage format before Phase 3's networking has
anything to send). Update this file as scope changes; it's the source of
truth for "what are we building and in what order."
