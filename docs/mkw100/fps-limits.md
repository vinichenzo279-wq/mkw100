# Frame rate at high racer counts: where it goes and where it stops

Measured 17.11.2026 on branch `perf-high-count` (from `8262bb3`): AMD Ryzen 7 PRO 6850U laptop
(8 cores, Radeon 680M iGPU), windowed 1920x1111, course 0x08, CPU-only races, base game with
low-detail CPUs on (`0939b73`), Retro Rewind with its skins (no low-detail models there). Every
number is a 1,200-frame windowed run (`MKW100_TEST_PERF`), two runs per setting, only runs where the
rest of the machine stayed near idle (the runner logs other processes' CPU).

## Short answer

- **The limit is the game thread, not the GPU.** At 128 and 255 racers the game thread is busy
  85-87% of wall time and never waits for vsync; the GX worker (29-35%), Aurora's FIFO thread
  (24-30%) and render worker (11-15%) all have headroom. A faster GPU would not help.
- **About 40% of the game thread at 255 is per-kart simulation** (physics, wheel physics, course
  collision, kart-kart collision, AI), spread flat over the game's own code: about 50 µs per racer
  per frame on this CPU, linear in the racer count. That part cannot move to another thread
  (guest code, shared state) and cannot be skipped without changing races. **That is the hard
  limit:** on this machine 255 racers can never pass about 65-70 fps even with free rendering and
  sound, and realistically about 40-45.
- **What was left on the table was waiting, and per-kart work nobody sees or hears.** Three
  changes, each behind a setting, take 255 racers from 32.6 to 39.6 fps (+21%), 128 from 48.3 to
  58.9 (+22%), 100 from 49.9 to the 60 fps cap, Retro Rewind 128 from 44.2 to 58.5 (+32%) and
  Retro Rewind 160 from 40.7 to 52.2 (+28%). None changes a race (same trajectory hash with every
  setting on or off), and the 12-racer hash stays `fa906b50b69477e8`.

## Results

frames/s, mean of two runs (A = before; each next column adds one change):

| Race | A: before | B: + layout follow | C: + async CopyTex | S: + far-CPU sound | D: + far-CPU draw 8000 | A → D |
|---|---|---|---|---|---|---|
| base 100 | 49.9 | | | | 60.0 (cap) | +20% |
| base 128 | 48.3 | 48.5 | 50.4 | 52.3 | **58.9** | +22% |
| base 255 | 32.6 | 33.4 | 35.3 | 38.9 | **39.6** | +21% |
| Retro Rewind 128 | 44.2 | | 49.2 | 51.5 | **58.5** | +32% |
| Retro Rewind 160 | 40.7 | | 42.6 | 44.4 | **52.2** | +28% |

Trajectories per race size were identical across A..D (base 100 `077599b6b0c1a255`, 128
`fd4299e581fc19ea`, 255 `3f97f15126723998`; RR 128 `73979561ecc29a9b`, 160 `4bed7fd444d446d4`). One
128 A run and one B run at 32-34 fps were taken while another session's builds ran; the table
uses the clean runs (raw rows: `perf_table.py`, see the end).

Headless oracle (`scripts/mkw100/run-hash.ps1`, stage `play-perf`): 12 racers give
`fa906b50b69477e8` with the defaults, with everything off and with everything on (async CopyTex,
far-CPU 8000); 128 racers give `a230580878163586` with everything on and with everything off (the
128 value depends on the stage's Config.toml, as noted in the build notes).

## Where the game thread's time goes

Share of game-thread samples (`profile_buckets.py`, stack-walking profiler):

| Bucket | base 128, A | base 255, A | base 255, D | RR 160, A | RR 160, D |
|---|---|---|---|---|---|
| sim: kart physics + collision | 27% | 36% | 41% | 33% | 42% |
| sim: CPU AI | 5% | 7% | 7% | 5% | 7% |
| sim: items, objects, course | 5% | 5% | 5% | 5% | 6% |
| wait: GX worker sync | 14.5% | 9% | 1% | 11% | 1% |
| render: model draw (scene gather, materials, display lists) | 14% | 10% | 11% | 14% | 11% |
| render: model calc (animation, world matrices) | 7% | 7% | 9% | 13% | 11% |
| render: HUD (layout animate + draw) | 7% | 8% | 9% | 4% | 7% |
| render: particles (calc + draw) | 7% | 6% | 7% | 5% | 6% |
| audio (race sound logic + sound thread) | 12% | 11% | 8% | 9% | 8% |
| VI pacing, other | 2% | 2% | 1% | 1% | 1% |

Per frame, from the clean A runs (128: 20.7 ms, 255: 30.7 ms), the cost grows by about 79 µs per
racer: kart physics/collision 42 µs, HUD 9 µs, AI 8 µs, audio 6 µs, model calc 4 µs, model draw
2 µs, the rest 8 µs. With D it grows by 65 µs per racer (128: 17.0 ms, 255: 25.3 ms), of which
about 50 µs is simulation. Everything else per frame (course, camera, effects, present) is roughly
8-9 ms.

## What was changed (all on branch `perf-high-count`)

### 1. Display-list and FIFO layout following — `[video] gx_follow_layout`, default on

nw4r's particle code (`DrawStrategyImpl::SetupGP`) asks for the vertex descriptor
(`GXGetVtxDesc`) about 780 times a frame. The game thread keeps a copy so it need not wait for the
GX worker, but marked it unknown whenever written bytes *might* contain a descriptor load: a byte
pair `08 5x..9x` anywhere. Inside vertex data that happens all the time (floats), and the next
query then waited for the worker to finish the whole frame queued so far: 6.6 waits a frame, 11-14%
of the game thread at 128 racers.

The game thread now parses the FIFO command stream it records (`GameFifoBytes`, gx_vertex.cpp):
it steps over vertex data (size from its copy) and XF payloads, applies CP layout loads to its copy,
and follows display lists (also those nw4r calls from the FIFO stream, command 0x40) with a
display-list walker whose result is cached per list (checked with the runtime's guest-write
generations, else an XXH3 digest; gx_dl.cpp). Wherever it cannot be sure of command boundaries
(inline GX work, display-list recording, an unknown opcode) it falls back to the old conservative
scan. Result at 128: 1 sync per race instead of 7,132. `MKW100_TEST_VTX_VERIFY=1` syncs and compares
the copy with the worker's state on every query: 0 differences over 936,462 queries.

On its own this gains little (about +2% at 255): the wait moves to the next sync point, `GXCopyTex`.

### 2. Asynchronous GXCopyTex — `[video] gx_async_copy_tex`, **default off**

`EGG::CapTexture::copyFromEFB` calls `GXCopyTex` once a frame; the HLE synced with the GX worker
and drained Aurora's FIFO thread (`GXDrawDone`) on the game thread first, so the game thread waited
for everything drawn so far. Now it is recorded for the worker like `GXCopyDisp` already was: the
copy still runs after every prior draw (the worker keeps the order), the game thread just does not
wait. +4% at 128 and +6% at 255 on top of 1 (together with 1: +4%, +8%; RR +11%, +5%).

Off by default because it has not been looked at on screen: EFB copies are GPU-side only (except
the lens-flare probe, which was asynchronous before too), so nothing should change, but the copy
destination is now registered on the worker, a little after the game thread moves on. Turn it on
in `UserData/Config.toml` (`[video]` `gx_async_copy_tex = true`) and check the bloom / depth of
field and the item-box / water reflections.

### 3. Far CPU racers — `[video] far_cpu_distance`, **default 0 (off)**

From the camera distance the game already computes per kart each frame (ClipInfoMgr, the kart's
ClipInfo at `Kart::Player +0x6C`), races of more than 12 (far_cpu.cpp):

- **sound** (S): `Audio::KartActor::Update` runs only for every human and the 12 nearest CPUs.
  The others' held sounds (engine, road) stop by themselves; one-shot sounds playing finish.
  +4% at 128, +10% at 255. Audibly: the far field of 116-243 engines goes quiet.
- **draw** (D): a CPU kart's ClipInfo far distance becomes the setting, so the game's own clipping
  hides karts beyond it (median CPU distance at 128 on course 0x08: about 10,000-11,500 units).
  At 8000: +13% at 128, +2% at 255, +14% RR 128, +18% RR 160. Visible: CPUs further than the
  distance pop out.

`MKW100_TEST_FAR_PARTS=sound` / `draw` limits a test to one part; `MKW100_TEST_FAR_LOG` prints the
distance spread. Off by default because both are visible/audible trade-offs for the user to pick.

Not tried as gameplay-changing: cheaper AI or collision for far CPUs. Kart physics and collision
are the floor (below), and updating far CPUs less often changes who wins; that is a game-design
decision, not a free optimisation.

## What is left, and roughly how much

| Lever | Expected gain | Notes |
|---|---|---|
| HUD per racer (9 µs/racer, 8-9% at 255) | 5-8% at 255 | `MainLayout` animate + draw grows with racers (position tracker / map icons). Skipping far CPUs' icons or animating them every other frame. Not investigated further. |
| Remaining fixed syncs (`GXCopyDisp`, VI present, `GetTexObjLODAll`) | 1-3% | about 1-3% of the frame left in `GxWorker::Sync`. |
| Draw cut at 255 | small | With the draw cut on, 255 gains only 2%: the pack around the camera is still on screen, and most far karts were already clipped by the camera frustum. |
| Model calc for hidden CPUs (4 µs/racer) | 3-5% at 255 | nw4r still animates and builds matrices for clipped karts. |
| Adaptive 30 fps rendering (existing, F10) | up to ~1.5x on rendering | halves render + HUD + model draw per simulated frame; simulation unchanged. At 255 rendering is about 35-40% of the frame, so 30 fps rendering gives roughly 1.25x simulation rate, not 2x. |
| Kart simulation (42 µs/racer physics + collision, 8 µs AI) | none without changing races | flat over physics (27%), wheel physics (7%), course collision (10%), kart-kart collision (3%), AI (6%) at 255. Guest code on one thread. |

The hard ceiling, per racer: about 50 µs of simulation per racer per frame on this CPU. At 255
racers that alone is 12.7 ms, plus about 1-2 ms fixed simulation, so even with rendering, HUD and
sound at zero cost 255 racers stop around 65-70 fps here; 160 around 100, 128 around 120. With
rendering as it is after these changes the practical numbers are the D column: about 40 at 255,
about 52 at 160 (RR), 60 at 128 when the far-CPU draw cut is on. A faster single core moves all of
it proportionally; more cores do not, apart from the worker threads that already have headroom.

## Reproducing

- Stages: `MKW100-perf/play-perf` (base) and `play-rr-perf`, copies of the 094 stages.
- `scripts/mkw100/run-perf.ps1 -Stage play-perf -Name x -Players 255 [-Profile] [-Env 'K=V;K=V']`:
  one windowed perf run; refuses to start while a game runs, logs other processes' CPU.
- `scripts/mkw100/run-perf-queue.ps1 -Cases 'stage|name|players|env[|profile]', ...`: waits for 20 s
  with no game, compiler or linker running before each case.
- `scripts/mkw100/perf_table.py play-perf/perf-results [--prefix m255]`: fps, busy ms, load, trajectory.
- `MKW100_TEST_PROFILE` (test_profiler.cpp) now samples the game thread and the GX worker with full
  stack walks (Dawn / driver / ntdll frames keep their caller), and lists every thread's CPU time.
  `scripts/mkw100/profile_report.py PROFILE EXE` (exclusive, inclusive, `--callers`, `--filter`) and
  `profile_buckets.py PROFILE EXE` resolve it; use the exact exe that produced the profile.
- Overrides: `MKW100_TEST_GX_FOLLOW=0/1`, `MKW100_TEST_ASYNC_COPYTEX=0/1`, `MKW100_TEST_FAR_CPU=<d>`,
  `MKW100_TEST_FAR_PARTS`, `MKW100_TEST_VTX_VERIFY=1`.
