// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixSubmit.h"
#include "GS/Remix/RemixMaterials.h"
#include "GS/Remix/RemixPaths.h"
#include "GS/Remix/RemixRuntime.h"
#include "GS/Remix/RemixTransforms.h"
#include "GS/Remix/RemixVU1Capture.h"

#include "MemoryTypes.h"
#include "GS/Renderers/HW/GSRendererHW.h"

#include "Config.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/WindowInfo.h"

#include "fmt/format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RemixSubmit
{
	namespace
	{
		// How often the counter block is emitted, in frames. Env-tunable so a short or
		// crashing session still reports something before it ends.
		u64 stats_interval_frames()
		{
			static const u64 value =
				static_cast<u64>(std::max<s64>(1, remix_ps2::read_env_int(L"PCSX2_REMIX_STATSFRAMES", 300)));
			return value;
		}

		// Sanity ceilings on a single submitted mesh, from RPCS3. Guards against a malformed
		// draw turning into a multi-gigabyte allocation.
		constexpr u32 s_max_vertices_per_mesh = 0x40000;
		constexpr u32 s_max_indices_per_mesh = 0x100000;

		// Positions further out than this are the FLT_MIN/m_max Q guards leaking through
		// (GSState.cpp:1399/:1403 replace a zero Q with FLT_MIN, whose reciprocal is ~8.5e37).
		//
		// 1e9 was the original ceiling and it is far too generous for a path tracer: a
		// triangle spanning a billion units in the same acceleration structure as one
		// spanning ten collapses float precision and produces degenerate BVH nodes. The
		// ceiling is now the frustum the camera actually spans -- anything past the far plane
		// cannot contribute to the image anyway.
		float max_position_magnitude();

		constexpr remixapi_Transform s_identity_transform =
			{{
				{1.f, 0.f, 0.f, 0.f},
				{0.f, 1.f, 0.f, 0.f},
				{0.f, 0.f, 1.f, 0.f},
			}};

		constexpr u64 fnv_seed = 0xCBF29CE484222325ULL;
		constexpr u64 fnv_prime = 0x100000001B3ULL;

		__fi u64 fnv_mix(u64 hash, u64 value)
		{
			for (u32 i = 0; i < 8; ++i)
			{
				hash ^= (value >> (i * 8)) & 0xFF;
				hash *= fnv_prime;
			}

			return hash;
		}

		// Every reject has its own counter, so a "submitted == 0" run names the gate that
		// refused rather than leaving it to guesswork (RPCS3's stat_counters pattern).
		struct stat_counters
		{
			u64 draws_seen = 0;
			u64 draws_submitted = 0;
			u64 skip_not_triangle = 0; // sprite / line / point class -- never 3D world geometry
			u64 skip_untextured = 0; // !m_process_texture: nothing pins Q to anything
			u64 skip_fst = 0; // PRIM->FST: UVs, so Q is not the perspective divisor
			u64 fst_recovered = 0; // FST=1 draws submitted with depth recovered from Z
			// Untextured draws submitted with depth recovered from Z. Measured on SOCOM Combined
			// Assault in-mission: untex was 4,395,585 of 8,417,784 draws seen -- 52% of the scene
			// and 96% of every skip -- so this counter is the one that says whether half the world
			// is reaching Remix or not.
			u64 untex_recovered = 0;
			u64 fst_flat = 0; // FST=1 draws rejected for constant Z (no depth to recover)
			u64 skip_const_q = 0; // m_vt.m_eq.q: one Q for the whole draw => 2D/HUD
			// Draws whose relative w spread is below w_flat_limit(): 2D the exact const-Q test
			// missed because their Q varies in the last few bits. See w_flat_limit().
			u64 skip_w_flat = 0;
			u64 skip_clear_alpha = 0; // every vertex alpha 0: the guest drew nothing
			u64 warn_inaccurate_stq = 0; // m_vt.m_accurate_stq: Q precision already suspect
			u64 skip_no_target = 0; // no colour target, so no viewport to un-project against
			u64 skip_empty = 0;
			u64 skip_too_large = 0;
			u64 skip_nonfinite = 0; // Q guard values poisoned the un-projected positions
			u64 skip_poisoned = 0; // hash quarantined by an earlier faulting runtime call
			u64 skip_mesh_budget = 0; // over the per-frame CreateMesh budget
			u64 skip_fbmsk = 0; // partial colour write mask: a multi-pass modulation term
			u64 skip_coincident = 0; // identical geometry already submitted this frame
			// Draws whose GEOMETRY matches one already submitted this frame but whose MATERIAL
			// differs -- the PS2 multitexture pattern. The GS has one texture unit, so a title
			// lays a base pass and then re-draws the same triangles with the lightmap bound.
			//
			// These are NOT in skip_coincident: material.content_hash is folded into the identity
			// hash on both the stable and legacy paths (deliberately -- Remix binds the material at
			// CreateMesh time), so a different texture means a different hash and the pass is
			// submitted. It therefore reaches the path tracer as a second surface sitting exactly on
			// the first, which is z-fighting rather than lighting.
			//
			// Counted, never rejected: this number sizes the opportunity to fold those passes into
			// one surface with a combined material, which is where the level's baked lighting is.
			u64 multipass_overlay = 0;
			u64 cam_sky = 0; // frames a REMIXAPI_CAMERA_TYPE_SKY camera was submitted for
			u64 cam_viewmodel = 0; // ditto for REMIXAPI_CAMERA_TYPE_VIEW_MODEL
			u64 skip_minw = 0; // draw sits at or inside the eye
			// Draw that merely REACHES the eye: its furthest vertex is fine, its nearest is not.
			// skip_minw cannot see these, because that gate tests max w.
			u64 skip_min_vertex_w = 0;
			u64 skip_maxw = 0; // draw sits entirely beyond the far-field cap (PCSX2_REMIX_MAXW)
			// Hyperextended draws: the ones whose AABB diagonal exceeds explode_ratio_limit()
			// times their own furthest w, i.e. "vertex explosion" turned into a number. Counted,
			// never rejected -- this is the diagnostic that decides whether the reported explosions
			// are geometry this backend submits or an artefact of Remix's Geometry Hash debug view,
			// and a gate that removed the evidence would answer the wrong question. 'peak' is the
			// session's largest ratio whatever the limit, so it retunes the limit from measurement:
			// legitimate off-screen geometry reaches ~17x, the one wrong-matrix scatter ever
			// measured on this project reached 1,450x.
			u64 hyperextended = 0;
			float hyperextended_peak = 0.f;
			// Histogram of each submitted draw's smallest w, by decade, so the min-w gate can be
			// set from the measured distribution instead of a guess. Buckets are
			// w < 1e-3, < 1e-2, < 1e-1, < 1, < 10, < 100, >= 100.
			u64 w_histogram[7] = {};
			u64 meshes_created = 0;
			u64 meshes_destroyed = 0;
			// Mesh creation rate, the strongest remaining lead on the device-loss exit: it is
			// reported to happen while walking, i.e. exactly when new geometry streams in.
			u64 meshes_created_frame = 0;
			u64 meshes_created_peak = 0;
			u64 sky_tagged = 0; // instances categorised REMIXAPI_INSTANCE_CATEGORY_BIT_SKY
			// Draws forced to the sky solver by a rtx.skyBoxTextures hash tag rather than by
			// classify_sky's geometry rules. Separate from sky_tagged because that one counts
			// instances that ended up with the SKY bit however they got it -- this counts the
			// draws the hash path is responsible for, which is the number that says whether the
			// tag list is doing anything. Expect ~1-2 per frame on a title with one backdrop draw.
			u64 sky_hash = 0;
			u64 cutout_tagged = 0; // instances categorised ALPHA_BLEND_TO_CUTOUT
			u64 skip_submit_delay = 0; // withheld by PCSX2_REMIX_SUBMITDELAY
			u64 skip_inst_budget = 0; // over the per-frame DrawInstance budget
			// Evictions per frame. A mesh cache pegged at its ceiling churns its *live set*
			// even when the geometry is static, so instances can reference handles the LRU is
			// reaping in the same frame -- a lifetime problem that would masquerade as a rate
			// effect under an instance cap. These tell the two apart.
			u64 meshes_destroyed_frame = 0;
			u64 meshes_destroyed_peak = 0;
			// Distinct mesh handles instanced in one frame -- the success metric for stable
			// mesh identity. Today this tracks the draw count, because identity is hashed over
			// camera-derived positions; if identity becomes stable it should collapse toward
			// the number of genuinely distinct objects and stay flat while the camera moves.
			u64 distinct_instanced_peak = 0;
			u64 distinct_instanced_total = 0;
			u64 distinct_instanced_frames = 0;
			// Stable-identity accounting. 'reuse' is a draw that registered onto a handle
			// created in an earlier frame -- the whole point of the feature. 'rebuild' is a key
			// whose slots were all occupied by geometry that did not fit, i.e. a genuine
			// collision or a deforming object, and is the number that says whether the identity
			// key discriminates well enough.
			u64 id_reuse = 0;
			u64 id_create = 0;
			u64 id_rebuild = 0;
			u64 id_probe_collisions = 0; // slots stepped over before a fit was found
			// Frame batching. 'groups' is the load-bearing one: it is how many distinct meshes a
			// batched frame references, and the survival dose-response is a function of exactly
			// that number.
			u64 batch_groups_peak = 0;
			u64 batch_groups_total = 0;
			u64 batch_frames = 0;
			u64 batch_surfaces_peak = 0;
			u64 batch_meshes_created = 0;
			u64 batch_vertices_peak = 0;
			// Hold-previous-window (PCSX2_REMIX_HOLDEMPTY, step 4B). See hold_empty_mode() for what
			// this is for and what it is INFERRED rather than measured to fix.
			//
			// 'empty' is the measurement the whole branch rests on and is counted whether the knob
			// is on or off: presented windows that submitted no geometry at all. On SOCOM CA it
			// should read almost exactly a third of the frame count.
			//
			// 'offcadence' is the misdetection tell named in the plan's risk register. This title's
			// empty windows arrive every third window without exception (56 of 56 measured gaps),
			// so a hold whose gap from the previous hold is anything other than 3 is either a
			// legitimate drought (load, cinematic, menu) being repeated or the detection firing on
			// something it should not. Watch it against 'gap3'.
			u64 hold_empty_windows = 0;
			u64 hold_windows = 0; // empty windows that re-presented the previous window's batch
			// ...and of those, the ones that also re-submitted the previous window's CAMERA
			// (HOLDEMPTY = 2). Separate from hold_windows because the difference between them is
			// exactly the difference between "the eye moved over stale geometry" and "the frame was
			// repeated": under mode 2 these two must track each other, and under mode 1 this is 0.
			u64 hold_cameras = 0;
			// HOLDEMPTY = 3: empty windows whose Present was SKIPPED outright, so the display keeps
			// showing the previous frame and the present rate becomes the game's own 40 Hz. These
			// are mutually exclusive with hold_windows -- a skipped window draws nothing at all.
			u64 hold_skipped_presents = 0;
			u64 hold_instances = 0; // instances re-presented, summed over those windows
			u64 hold_gap3 = 0; // holds exactly 3 windows after the previous hold -- the cadence
			u64 hold_offcadence = 0; // holds at any other spacing -- THE TELL
			u64 hold_gap_last = 0;
			u64 hold_skip_consecutive = 0; // refused: the previous window was itself a hold
			u64 hold_skip_nocam = 0; // refused: no live world camera, or a space mismatch
			u64 hold_skip_stale = 0; // refused: the retained handles had aged out (BATCHRETAIN)
			u64 hold_failed = 0; // DrawInstance refused a re-presented instance
			u64 degenerate_triangles = 0; // zero-area triangles dropped before CreateMesh
			u64 skip_all_degenerate = 0; // draws where every triangle was degenerate
			u64 cam_world = 0;
			u64 cam_fallback = 0;
			u64 cam_held_gap = 0;  // frames that reused the last resolved camera instead of the origin
			u64 cam_hold_expired = 0; // holds abandoned because the solve stayed broken, not gapped
			// SetupCamera calls that came back non-SUCCESS (or faulted through the SEH guard).
			//
			// cam_world/cam_fallback/cam_sky/cam_viewmodel count *attempts*: they are incremented
			// unconditionally next to a guarded_setup_camera() whose return value used to be
			// discarded. So "cam world 366 / fallback 0" said only that we called SetupCamera 366
			// times, never that Remix accepted one -- and Remix's own developer menu was reporting
			// an empty MAIN camera on roughly two frames in three while that counter looked clean.
			// A rejected camera leaves the runtime on its previous one, which is what a scene that
			// teleports between two viewpoints looks like. Count the failures separately.
			u64 cam_failed = 0;
			// Draws rejected for rendering into a small off-screen target (PCSX2_REMIX_MINRT).
			u64 skip_offscreen_rt = 0;
			u64 skip_shadow_pass = 0; // depth-detached silhouette pass, not world geometry
			// World-anchor (step 9) accounting. Every one of these has to be readable in a
			// null result: "no candidate" and "candidates that never split" and "splits that
			// scored zero" are three completely different findings.
			u64 vu_kicks = 0;
			u64 vu_kicks_scanned = 0;
			u64 vu_windows = 0;
			u64 vu_survivors = 0; // passed the VU-side shape prefilter
			u64 vu_reentrant = 0; // kicks dropped because two threads executed VU1 at once
			u64 vu_sliced = 0; // matrices the ucode back-slice located
			u64 vu_sliced_published = 0; // of those, read out of VU1 memory and offered

			// The back-slice program cache. A refusal here is not a rejected candidate, it is an
			// entry point that was never sliced at all -- the deterministic path, source 7 and the
			// title-specific fixed block are all inside the block program_for() gates. It was
			// silent, and it is what removed SOCOM's one camera-tracking matrix (start_pc 0x2040)
			// from an entire session's table. Anything non-zero here means matrices exist in VU1
			// that this backend has never looked at.
			u32 vu_programs_used = 0;
			u32 vu_programs_limit = 0;
			u64 vu_programs_refused = 0;
			u32 vu_refused_start_pc = 0;
			u64 vu_refused_ucode = 0;
			u64 cam_accept_sliced = 0; // accepted cameras that came from the back-slice
			u64 cam_candidates = 0; // offered to the GS side, summed over frames
			u64 cam_reject_split = 0; // split_view_projection_direct refused
			// Same rejections, broken down by which step inside the split refused.
			u64 cam_split_stage[static_cast<u32>(remix_ps2::split_stage::count)]{};
			u64 cam_reject_score = 0; // split worked, score_perspective refused
			u64 cam_accept = 0;
			u64 cam_reject_degenerate = 0; // split and scored, but refuted by the geometry
			u64 cam_reject_scale = 0; // the un-projection's unit disagrees with the guest's w
			u64 cam_reject_extent = 0; // world space changed the scene's size, so it is not rigid
			u32 cam_last_candidates = 0;
			// Per-draw camera placement (PCSX2_REMIX_PERDRAWCAM). Every stage separately visible,
			// same reason as the block above: "no draw was placed per-draw" has four completely
			// different causes -- the knob is off, the ring held nothing, the ring camera already
			// WAS the frame camera, or the acceptance pipeline refused it -- and one opaque count
			// could not tell them apart. See per_draw_solver() for what these are measuring.
			//
			// perdraw_match + perdraw_placed + perdraw_fallback == world-mode draws while the knob
			// is on; anything left over is the knob being off for part of the run.
			u64 perdraw_match = 0; // ring camera hash == the frame camera's; nothing to place
			u64 perdraw_same_solver = 0; // ring hash differed but NORMALISED to the frame solver
			u64 perdraw_distinct = 0; // ring camera solved to a genuinely different solver
			u64 perdraw_solved = 0; // cache misses that ran the acceptance pipeline
			u64 perdraw_refused = 0; // of those, refused by it -- the risk-register tell for 4A
			u64 perdraw_fallback = 0; // ring miss / refuted / refused -> frame solver ("never guess")
			// The decisive per-WINDOW census, and the reason it counts normalised solvers rather
			// than ring hashes: a ring hash is the raw VU1 matrix, and this title emits the same
			// camera at two different column scales, so a raw-hash count overstates how many
			// cameras a window really used. 'solvers 1' would mean every draw in the window shares
			// one camera and per-draw placement is a dead end; 'solvers >= 2' means it is not.
			u32 perdraw_solvers_last = 0;
			u32 perdraw_solvers_peak = 0;
			u32 perdraw_rings_last = 0; // distinct RAW ring hashes in the same window, for contrast
			u32 perdraw_rings_peak = 0;
			u64 perdraw_windows = 0; // windows with at least one world-mode draw
			u64 perdraw_multi_windows = 0; // ...of which had 2 or more distinct normalised solvers
			u64 perdraw_census_overflow = 0; // windows that exceeded the census's slot count
		};

		struct mesh_entry
		{
			remixapi_MeshHandle handle = nullptr;
			u64 last_used_frame = 0;

			// Stable-identity bookkeeping; empty on the legacy path.
			//
			// 'local' is the geometry as it was uploaded: the draw's positions relative to their
			// own centroid at the moment the mesh was created. Every later frame registers its
			// own centred positions against this to recover the rigid transform that places the
			// mesh, so the handle survives camera motion instead of being re-minted.
			std::vector<float> local;
			// Set when a draw claimed this entry but did not fit it, i.e. the identity key
			// collided or the geometry is deforming. The rebuild happens at the frame boundary,
			// never mid-frame: the handle may already be referenced by an instance submitted
			// earlier in this same frame, and destroying it under the runtime is exactly the
			// class of fault this whole change exists to remove.
			bool rebuild_pending = false;
		};

		// The un-projection inputs of the frame's dominant 3D draw. Recorded per draw and
		// consumed at VSync, because the screen-clip normaliser needs exactly the constants
		// the vertex un-projection used.
		struct viewport_constants
		{
			bool valid = false;
			float ofx = 0.f;
			float ofy = 0.f;
			int width = 0;
			int height = 0;
			u32 weight = 0; // vertex count of the draw that supplied it
		};

		struct world_camera
		{
			bool valid = false;
			remix_ps2::mat4 view = remix_ps2::mat4_identity();
			remix_ps2::mat4 projection = remix_ps2::mat4_identity();
			remix_ps2::clip_solver solver{};
			float position[3] = {0.f, 0.f, 0.f};
			float near_plane = 0.1f;
			float far_plane = 1000.f;
			u64 matrix_hash = 0;
			float score = 0.f;
			// Measured by the depth-scale gate and reported, so the number the gate turns on is
			// visible rather than implied.
			float depth_scale = 0.f;
			float depth_anisotropy = 0.f;
		};

		remix_ps2::runtime s_remix;

		// Set once the runtime started and the debug scene exists. Nothing is submitted before.
		bool s_live = false;

		// One-shot: a failed init must not be retried every frame.
		bool s_init_attempted = false;

		// True while the Remix renderer is the selected renderer, set before the device opens.
		bool s_renderer_is_remix = false;

		// The real game-window HWND, stashed by OnAcquireWindow before the device may have
		// been told the surface is Surfaceless.
		HWND s_hwnd = nullptr;
		int s_window_width = 0;
		int s_window_height = 0;

		// The window Startup() actually bound to. PCSX2 destroys and recreates its render
		// window on a fullscreen toggle, which leaves the runtime presenting into a dead HWND:
		// observed as a ~170s delay and then an unbounded storm of FAULTED Present calls that
		// takes the process down. The runtime is a process-lifetime singleton and a second
		// Startup after shutdown is unproven in dxvk-remix, so a recreated window cannot be
		// re-bound -- it is detected here and the renderer stops cleanly instead.
		HWND s_active_hwnd = nullptr;
		bool s_window_lost = false;

		remixapi_MeshHandle s_debug_mesh = nullptr;
		remixapi_LightHandle s_debug_light = nullptr;

		u64 s_frame_counter = 0;

		// Last knob generation the mesh cache was flushed for. ~0 means "never observed", which
		// is distinct from generation 0 and keeps the first frame from flushing an empty cache.
		u64 s_knob_generation_seen = ~0ull;
		u64 s_submitted_this_frame = 0;
		u64 s_drawdump_frames_left = 0;
		u64 s_drawdump_skipped = 0; // qualifying frames passed over while DRAWDUMPAFTER counts down
		bool s_drawdump_started = false;
		bool s_drawdump_world_armed = false;

		// Consecutive frames that submitted nothing, and how many of them the beacon waits for.
		// ~2 seconds at 60Hz: long enough that a load or an all-2D stretch never triggers it.
		u64 s_empty_frame_streak = 0;
		constexpr u64 s_beacon_after_empty_frames = 120;
		stat_counters s_stats{};

		std::unordered_map<u64, mesh_entry> s_meshes;
		std::unordered_set<u64> s_poisoned;

		// Mesh hashes already submitted this frame, cleared at each VSync. See the dedupe gate.
		std::unordered_set<u64> s_frame_submitted_hashes;

		// The same, with the material contribution removed: "these triangles in this place",
		// whatever texture is bound. A second hit here that was NOT a dedupe hit is a multitexture
		// pass. Diagnostic only; see stat_counters::multipass_overlay.
		std::unordered_set<u64> s_frame_geometry_hashes;

		// Distinct mesh *keys* instanced this frame. Under stable identity the dedupe set above
		// also carries a quantized centroid, so two instances of one object appear twice in it
		// while referencing a single handle; this set is what the success metric counts.
		std::unordered_set<u64> s_frame_instanced_keys;

		// Current draw's positions relative to their own centroid, reused across draws.
		std::vector<float> s_scratch_local;

		// --- frame batching -------------------------------------------------------------------
		//
		// The only quantity that has ever moved survival on the device loss is the number of
		// DISTINCT MESHES a frame references: 0-1 gives 20/20, 5 gives 7/20, ~190-850 gives 1-4/20
		// (SOCOM slot 2, 20 launches per arm). Handle age does not matter -- 850 pinned long-lived
		// handles is 2/20 -- nor does BLAS routing or opacity micromaps. So the thing to attack is
		// the count itself.
		//
		// remixapi_MeshInfo takes an array of surfaces, each with its own material, so a whole
		// frame's geometry can go into a handful of meshes instead of hundreds. What it CANNOT
		// share is the instance: remixapi_InstanceInfoBlendEXT and categoryFlags hang off
		// remixapi_InstanceInfo, not off the surface. So draws are grouped by exactly those two,
		// and it is the GROUP count -- not the draw count -- that decides whether this reaches the
		// safe operating point at all. That number is in the counter block.
		struct batch_surface
		{
			u64 material_hash = 0;
			remixapi_MaterialHandle material = nullptr;
			std::vector<remixapi_HardcodedVertex> vertices;
			std::vector<u32> indices;
		};

		struct batch_group
		{
			remixapi_InstanceInfoBlendEXT blend{};
			remixapi_InstanceCategoryFlags categories = 0;
			std::unordered_map<u64, size_t> surface_of_material;
			std::vector<batch_surface> surfaces;
			size_t surfaces_used = 0;
			// Content key for mesh reuse. The flush used to hash s_frame_counter and the group index,
			// which is unique per frame BY CONSTRUCTION -- so Remix received a new mesh handle for
			// identical geometry every frame and nothing could ever have a stable identity. Each draw
			// mixes its own (camera-invariant, see the stable_id branch) hash in as it joins.
			u64 content = fnv_seed;
		};

		// Kept allocated across frames and cleared rather than destroyed -- a frame's worth of
		// geometry is tens of thousands of vertices and reallocating it every frame on the GS
		// thread would be its own problem.
		std::vector<batch_group> s_batch_groups;
		std::unordered_map<u64, size_t> s_batch_group_of_key;
		size_t s_batch_groups_used = 0;

		// Meshes built by the flush, with the frame they were created in. Batched geometry is
		// derived from the camera, so it is new every frame and these are one-shot by nature --
		// which is fine, because mesh creation on its own survives 20/20 (phase 3k). They are
		// retained for a couple of frames before release so the runtime is never handed a
		// destroyed handle it might still be reading.
		struct batch_mesh
		{
			remixapi_MeshHandle handle = nullptr;
			u64 created_frame = 0;
		};

		std::vector<batch_mesh> s_batch_meshes;
		// Reuse a batch group's mesh across frames instead of recreating it. The comment at the
		// CreateMesh call justified per-frame creation with "the geometry is camera-derived and
		// genuinely new every frame" -- true before the EE camera, false after it: MESHTRACK measures
		// static geometry holding its world position to 1.4 units while the player turns. Recreating
		// it hands Remix a new mesh handle every frame, so nothing has a stable identity, the denoiser
		// gets no temporal history, and the texture-categorisation UI has nothing to hover.
		std::unordered_map<u64, batch_mesh> s_batch_mesh_cache;
		u64 s_batch_reused = 0;
		std::vector<remixapi_MeshInfoSurfaceTriangles> s_batch_surface_scratch;

		// What one flush actually instanced, kept so an empty window can re-present it instead of
		// presenting nothing (PCSX2_REMIX_HOLDEMPTY -- see hold_empty_mode()).
		//
		// Everything DrawInstance needs and nothing else: the mesh handle (which stays alive on its
		// own under BATCHRETAIN, so this holds no ownership and destroys nothing), the blend struct
		// by value because instance.pNext has to point at storage that outlives the call, the
		// category flags, and the transform. The batch path's transform is always the identity --
		// batched vertices are already in the submitted camera's space -- but it is stored rather
		// than assumed so this cannot silently rot if that ever changes.
		struct held_instance
		{
			remixapi_MeshHandle handle = nullptr;
			remixapi_InstanceInfoBlendEXT blend{};
			remixapi_InstanceCategoryFlags categories = 0;
			remixapi_Transform transform{};
			u64 mesh_hash = 0;
		};

		std::vector<held_instance> s_held_instances;
		u64 s_held_frame = 0; // the window whose flush built them
		bool s_held_world = false; // that window submitted in world space
		bool s_held_used = false; // already re-presented once -- never twice in a row
		u64 s_last_hold_window = 0;
		bool s_have_held_before = false;

		// The camera that window submitted, which is also the camera its geometry was un-projected
		// with -- submit_camera() runs at the top of OnVSync and resolve_world_camera() at the
		// bottom, so s_active_camera is that one matrix for the whole window. Mode 2 re-submits it
		// so a held window is a true duplicate of the window it repeats. See hold_empty_mode().
		world_camera s_held_camera{};

		// Decided ONCE per window, at the top of OnVSync, BEFORE submit_camera() -- which is the
		// only place it can be decided, because mode 2 has to change what that call submits.
		bool s_hold_pending = false;

		// Distinct (blend state, category) groups seen this frame, counted even when batching is
		// off so the feasibility question can be answered without turning it on.
		std::unordered_set<u64> s_frame_group_keys;

		int batch_reuse_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_BATCHREUSE", 0), 0, 1));
			return value;
		}

		int batch_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_BATCH", 0), 0, 1));
			return value;
		}

		u64 batch_retain_frames()
		{
			static const u64 value =
				static_cast<u64>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_BATCHRETAIN", 3), 1, 60));
			return value;
		}

		// Matrices already written to the PCSX2_REMIX_DUMP file, so the diagnostic stays one
		// line per distinct matrix rather than one per frame per matrix.
		std::unordered_set<u64> s_dumped;

		// Reused across draws to keep the hot path allocation free.
		std::vector<remixapi_HardcodedVertex> s_scratch_vertices;
		std::vector<u32> s_scratch_indices;
		// NDC x/y per scratch vertex, kept for the 2D overlay rasteriser.
		std::vector<float> s_scratch_ndc;

		// Un-projection constants: accumulating for the frame in flight, and the finished
		// frame's winner that resolve_world_camera() normalises against.
		viewport_constants s_frame_viewport{};
		viewport_constants s_last_viewport{};

		world_camera s_active_camera{};

		// Frames the last accepted camera may be re-used for when a frame fails to resolve.
		// Short on purpose: holding a stale camera while the player moves reproduces exactly
		// the failure the milestone test looks for (geometry gliding with the view), so this
		// is only long enough to ride out a single-frame hiccup.
		constexpr u64 s_camera_hold_frames = 3;
		u64 s_camera_last_accept_frame = 0;
		bool s_logged_world_camera = false;
		// One-shot WORLDFIX projection-sign line. Declared here rather than beside the rest of the
		// worldfix state further down because resolve_world_camera(), which writes it, is defined
		// above that block.
		bool s_worldfix_logged_projection = false;

		float s_light_position[3] = {0.f, -1.f, 0.f};
		float s_light_radius = 0.1f;
		float s_light_radiance = 100.f;
		bool s_light_placed = false;

		remixapi_LightHandle s_sun_light = nullptr;
		float s_sun_direction[3] = {0.f, 0.f, 1.f};
		bool s_sun_placed = false;

		// --- scene lighting -------------------------------------------------------------------
		//
		// Neither of these titles ships a light the backend can see, so the scene is lit entirely
		// by whatever we add. The original placeholder was a sphere light attached to the camera,
		// and its 1/d^2 falloff is not a tuning problem, it is the wrong shape: the first-person
		// weapon sits at d ~ 0.5 and a wall at d ~ 6, so the weapon receives about 100x the
		// irradiance and blows to flat white while the room stays legible but dim. That is
		// exactly the "flat white weapon against grainy walls" the user reported, and it is also
		// why both verification tasks -- the rotate/walk protocol and any look at SOCOM -- came
		// back as captures too dark to judge.
		//
		// A dome light and a distant light have no distance falloff at all, so one setting works
		// whether the frame's scene radius is 4 (SOCOM slot 3) or 11,785 (the user's outdoor
		// save). They are also position-independent, so unlike the sphere they never need
		// re-placing and never chase the camera.
		remixapi_LightHandle s_dome_light = nullptr;

		// Defined below with the other env helpers.
		float env_float(const wchar_t* name, float fallback);

		// A knob that really is live, as opposed to one the settings page merely claims is.
		//
		// The `static const int value = read_env_int(...)` idiom below used to be applied to knobs
		// the knob table marks latched=false, so the settings page offered them without a
		// "(restart)" note and changing them did nothing until the emulator was restarted. Reading
		// the environment on every call instead is not an option -- these sit in the per-draw path.
		//
		// So: parse once, then re-parse only when paths::apply_live_knobs() reports that the value
		// behind the variable actually changed. Single-threaded by construction; every caller is on
		// the GS thread, which is also the thread that bumps the generation.
		class live_int
		{
		public:
			constexpr live_int(const wchar_t* name, s64 fallback, s64 lo, s64 hi)
				: m_name(name), m_fallback(fallback), m_lo(lo), m_hi(hi)
			{
			}

			int get()
			{
				if (const u64 generation = remix_ps2::paths::knob_generation(); generation != m_generation)
				{
					m_generation = generation;
					m_value = static_cast<int>(
						std::clamp<s64>(remix_ps2::read_env_int(m_name, m_fallback), m_lo, m_hi));
				}

				return m_value;
			}

		private:
			const wchar_t* m_name;
			s64 m_fallback;
			s64 m_lo;
			s64 m_hi;
			// Deliberately not 0: generation starts at 0, so this forces the first get() to parse.
			u64 m_generation = ~0ull;
			int m_value = 0;
		};

		// The float twin of live_int. Same contract: parse once, re-parse only when the settings
		// page has actually moved the value.
		//
		// CURRENTLY UNUSED, and left here on purpose as the documented idiom for a float knob that
		// really does sit on a hot path. The lighting knobs it was written for have moved to
		// env_float_signed(): "when the settings page moves it" is not the same as "when the value
		// moves", because the per-game .conf writes the environment without touching
		// knob_generation() -- see the note there.
		class live_float
		{
		public:
			constexpr live_float(const wchar_t* name, float fallback)
				: m_name(name), m_fallback(fallback)
			{
			}

			float get()
			{
				if (const u64 generation = remix_ps2::paths::knob_generation(); generation != m_generation)
				{
					m_generation = generation;
					m_value = env_float(m_name, m_fallback);
				}

				return m_value;
			}

		private:
			const wchar_t* m_name;
			float m_fallback;
			u64 m_generation = ~0ull;
			float m_value = 0.f;
		};


		//   0 = no lights at all
		//   1 = distance-independent fill: dome ambient + distant key. The default.
		//   2 = the legacy camera-attached sphere light, kept for comparison
		int light_mode()
		{
			static live_int value(L"PCSX2_REMIX_LIGHTMODE", 1, 0, 2);
			return value.get();
		}

		// PCSX2_REMIX_WORLDROT -- the world-anchor correction.
		//
		// This remains diagnostic until MESHTRACK proves that one stable mesh moves with the view.
		// Aggregate scene bounds cannot prove that on SOCOM because the game frustum-culls draws.
		//
		//   0 = off (shipped behaviour, geometry stays view-aligned)
		//   1 = rotate by the view->world rotation, about the camera
		//   2 = rotate by its transpose, about the camera
		//
		// Mode 2 exists because the row-vector convention here is asserted, not proven: for an
		// orthonormal basis the transpose is the inverse, so if 1 over-rotates (the scene centre
		// swings twice as far instead of going flat) 2 is the answer, with no second build.
		// VERIFY WITH CAMTRACK: stand still, turn 360. Correct = scene centre goes CONSTANT.
		int world_rot_mode()
		{
			static live_int value(L"PCSX2_REMIX_WORLDROT", 0, 0, 2);
			return value.get();
		}

		// MESHTRACK. The un-confounded world-anchor test.
		//
		// The scene-bounds centroid CANNOT answer "is our geometry world-anchored?", because
		// SOCOM frustum-culls on the EE: turning changes WHICH objects are submitted, so the
		// aggregate centre swings with view direction even when every object is perfectly
		// anchored. That confound produced a spurious r = 0.994 and a retracted root cause.
		//
		// This follows ONE object instead. Latch the first sufficiently large stable-identity
		// mesh, then report that same mesh's world centroid every time it is resubmitted. Stand
		// still and turn: a world-anchored mesh's centroid is CONSTANT, full stop. Culling can
		// only stop it updating (which `seen` reveals) -- it can never move it.
		u64 s_meshtrack_hash = 0;
		float s_meshtrack_centroid[3] = {0.f, 0.f, 0.f};
		u64 s_meshtrack_seen = 0;
		u32 s_meshtrack_verts = 0;
		float s_meshtrack_dist = 0.f;
		float s_meshtrack_normal[3] = {0.f, 0.f, 0.f};
		float s_meshtrack_facing = 0.f; // dot(normal, eye - centroid), normalised

		// LIGHTFIT. The projection null: an authored lamp and the fixture geometry it sits in must
		// occupy the same world point. Geometry re-projects to the guest's own clip coordinates
		// whatever projection we use, so only a world-space light exposes an error in it -- and
		// the DIRECTION of the residual names the wrong parameter: perpendicular to the view means
		// FOV or aspect, along it means depth scale.
		float s_lightfit_light[3] = {0.f, 0.f, 0.f};
		float s_lightfit_best_light[3] = {0.f, 0.f, 0.f};
		float s_lightfit_vert[3] = {0.f, 0.f, 0.f};
		float s_lightfit_best_d2 = 1e30f;
		bool s_lightfit_armed = false;

		// PCSX2_REMIX_UIMODE -- make menus and HUD render.
		//
		// The screen-UI path already existed, but `fallback_screen_ui` required
		// !s_active_camera.valid -- it only opened when NO world camera had been elected. In a
		// menu the camera from the last gameplay frame is still latched and still "valid", so
		// the gate stays shut, the const-Q gate then rejects every 2D draw (measured: constq
		// 40,626 in one session, wflat 0) and the menu is simply absent. That is why the game
		// can only be driven from save states.
		//
		//   0 = off: screen UI only when no camera exists (shipped behaviour)
		//   1 = treat a 2D draw as screen UI whatever the camera is doing
		//
		// Under mode 1 such draws take the VIEW-SPACE tier (w = 1, positions straight from NDC)
		// and are placed with the camera's own view->world transform, so they land in front of
		// the eye as an overlay instead of being un-projected into the world at w = 1, which
		// would bury them in the camera's face.
		int ui_mode()
		{
			static live_int value(L"PCSX2_REMIX_UIMODE", 0, 0, 1);
			return value.get();
		}

		// --- 2D overlay rasteriser -----------------------------------------------------------
		//
		// SOCOM's menus and HUD are 2D draws. Every route through the geometry pipeline is wrong
		// for them: un-projected they land in the world, and view-space they still need a camera
		// to place them. The runtime exposes remixapi_DrawScreenOverlay(pixels, w, h, format,
		// opacity), which composites a CPU bitmap over the final image -- which is what a HUD
		// actually is. So rasterise the 2D draws ourselves and hand over one buffer per frame.
		//
		// Deliberately minimal: nearest sampling, source-alpha blending, no perspective
		// correction (a 2D draw has none to correct). That is enough for text, icons and menus.
		std::vector<u8> s_overlay;         // BGRA8, matching the decoded material payload
		u32 s_overlay_w = 0, s_overlay_h = 0;
		bool s_overlay_used = false;
		u64 s_overlay_draws = 0;      // draws that reached the rasteriser
		u64 s_overlay_nopixels = 0;   // ... but had no CPU texture to sample
		u64 s_overlay_texels = 0;     // texels actually written
		u64 s_overlay_presents = 0;   // frames handed to DrawScreenOverlay
		u64 s_overlay_fullscreen = 0; // sprites refused as full-target blits, not UI
		u64 s_screen_ui_seen = 0;     // draws classified as screen UI
		u64 s_screen_ui_nomat = 0;    // ... dropped: no material bound
		u64 s_screen_ui_nondc = 0;    // ... dropped: no NDC captured
		// Frame the overlay was last rebuilt on. The HUD is NOT submitted every frame -- it rides
		// the same 1-in-3 empty-window cadence that HOLDEMPTY exists for -- so clearing at end of
		// frame presented an empty buffer on every gap and the HUD strobed. Clear lazily instead:
		// on the first UI draw OF A FRAME. A frame with no UI draws then keeps the previous
		// content and is presented again, which is exactly what the geometry path already does.
		u64 s_overlay_frame = ~0ull;

		int ui_raster_mode()
		{
			static live_int value(L"PCSX2_REMIX_UIRASTER", 0, 0, 1);
			return value.get();
		}

		void overlay_reset(u32 w, u32 h)
		{
			if (w == 0 || h == 0)
				return;

			if (w != s_overlay_w || h != s_overlay_h || s_overlay.size() != (size_t)w * h * 4)
			{
				s_overlay_w = w;
				s_overlay_h = h;
				s_overlay.assign((size_t)w * h * 4, 0);
			}
			else if (s_overlay_used)
			{
				std::fill(s_overlay.begin(), s_overlay.end(), (u8)0);
			}

			s_overlay_used = false;
		}

		// Rasterise the current scratch draw (positions in s_scratch_ndc, UVs and colour in
		// s_scratch_vertices, triangles in s_scratch_indices) into the overlay.
		void overlay_raster(u64 content_hash)
		{
			const u8* px = nullptr;
			u32 tw = 0, th = 0;
			if (!remix_ps2::materials::cpu_pixels(content_hash, px, tw, th))
			{
				++s_overlay_nopixels;
				return;
			}
			if (s_overlay.empty() || s_scratch_ndc.size() < s_scratch_vertices.size() * 2)
				return;

			const float fw = (float)s_overlay_w;
			const float fh = (float)s_overlay_h;

			for (size_t tri = 0; tri + 2 < s_scratch_indices.size(); tri += 3)
			{
				const u32 i0 = s_scratch_indices[tri], i1 = s_scratch_indices[tri + 1],
						  i2 = s_scratch_indices[tri + 2];
				if (i0 >= s_scratch_vertices.size() || i1 >= s_scratch_vertices.size() ||
					i2 >= s_scratch_vertices.size())
					continue;

				// NDC -> pixel. ndc_y is +up (the vertex loop negates it), screen y is +down.
				const float x0 = (s_scratch_ndc[i0 * 2] * 0.5f + 0.5f) * fw;
				const float y0 = (0.5f - s_scratch_ndc[i0 * 2 + 1] * 0.5f) * fh;
				const float x1 = (s_scratch_ndc[i1 * 2] * 0.5f + 0.5f) * fw;
				const float y1 = (0.5f - s_scratch_ndc[i1 * 2 + 1] * 0.5f) * fh;
				const float x2 = (s_scratch_ndc[i2 * 2] * 0.5f + 0.5f) * fw;
				const float y2 = (0.5f - s_scratch_ndc[i2 * 2 + 1] * 0.5f) * fh;

				const float area = ((x1 - x0) * (y2 - y0)) - ((x2 - x0) * (y1 - y0));
				if (!std::isfinite(area) || std::abs(area) < 1e-6f)
					continue;

				const float inv_area = 1.0f / area;
				int minx = (int)std::floor(std::min(std::min(x0, x1), x2));
				int maxx = (int)std::ceil(std::max(std::max(x0, x1), x2));
				int miny = (int)std::floor(std::min(std::min(y0, y1), y2));
				int maxy = (int)std::ceil(std::max(std::max(y0, y1), y2));
				minx = std::max(minx, 0); miny = std::max(miny, 0);
				maxx = std::min(maxx, (int)s_overlay_w - 1);
				maxy = std::min(maxy, (int)s_overlay_h - 1);

				for (int y = miny; y <= maxy; ++y)
				{
					for (int x = minx; x <= maxx; ++x)
					{
						const float pxc = (float)x + 0.5f, pyc = (float)y + 0.5f;
						float w0 = (((x1 - pxc) * (y2 - pyc)) - ((x2 - pxc) * (y1 - pyc))) * inv_area;
						float w1 = (((x2 - pxc) * (y0 - pyc)) - ((x0 - pxc) * (y2 - pyc))) * inv_area;
						float w2 = 1.0f - w0 - w1;
						if (w0 < 0.f || w1 < 0.f || w2 < 0.f)
							continue;

						const float u = (w0 * s_scratch_vertices[i0].texcoord[0]) +
										(w1 * s_scratch_vertices[i1].texcoord[0]) +
										(w2 * s_scratch_vertices[i2].texcoord[0]);
						const float v = (w0 * s_scratch_vertices[i0].texcoord[1]) +
										(w1 * s_scratch_vertices[i1].texcoord[1]) +
										(w2 * s_scratch_vertices[i2].texcoord[1]);
						if (!std::isfinite(u) || !std::isfinite(v))
							continue;

						// Wrap, matching the GS default.
						int su = (int)(u * (float)tw); su %= (int)tw; if (su < 0) su += (int)tw;
						int sv = (int)(v * (float)th); sv %= (int)th; if (sv < 0) sv += (int)th;

						const u8* texel = px + (((size_t)sv * tw) + su) * 4;

						// Vertex colour modulates, and PS2 alpha already scaled to 0..255.
						const u32 vc = s_scratch_vertices[i0].color;
						const u32 va = (vc >> 24) & 0xFF;
						const u32 a = (texel[3] * va) / 255u;
						if (a == 0)
							continue;

						u8* dst = s_overlay.data() + (((size_t)y * s_overlay_w) + x) * 4;
						for (u32 k = 0; k < 3; ++k)
						{
							const u32 src = ((u32)texel[k] * ((vc >> (k * 8)) & 0xFF)) / 255u;
							dst[k] = (u8)(((src * a) + ((u32)dst[k] * (255u - a))) / 255u);
						}
						dst[3] = (u8)std::min(255u, (u32)dst[3] + a);
						s_overlay_used = true;
						++s_overlay_texels;
					}
				}
			}

			++s_overlay_draws;
		}

		// PCSX2_REMIX_UIWMAX -- separates real screen UI from world-space billboards.
		//
		// UIMODE treats any constant-Q textured draw as screen UI, and that is too broad: smoke,
		// dirt and muzzle-flash billboards are camera-facing quads at ONE depth, so they satisfy
		// the same test and get rasterised into the HUD overlay. Mortar dirt filling the screen
		// was exactly this.
		//
		// The discriminator is depth. This title's genuine UI draws sit at w = 6.0 (measured while
		// calibrating WFLAT: menu quads reported w = [6.0, 6.0]); a world billboard's w is its
		// real distance from the eye, orders of magnitude larger. So cap it.
		// 0 disables the cap and restores the over-broad behaviour.
		float ui_w_max()
		{
			static live_float value(L"PCSX2_REMIX_UIWMAX", 50.f);
			return value.get();
		}

		// PCSX2_REMIX_DROPCLEAR -- discard draws the guest authored as fully transparent.
		//
		// Found from the developer menu's own debug views: a large polygon reads BLACK in Vertex
		// Alpha (alpha 0 on every vertex) and PURE WHITE in Diffuse Albedo. The game intended it
		// to be invisible; we render it opaque white because ALPHASTATE = 0 forces opacity. The
		// result is a giant white bounce card in the middle of the level, which is what made
		// everything read as uniformly, emissively bright.
		//
		// ALPHASTATE cannot fix this: mode 2 attaches the real per-draw blend and, because this
		// title sets ABE=1 on 100% of its draws, turns the entire world translucent; mode 0 is
		// what we have. Rather than pick a bad global, drop only the draws that contribute
		// NOTHING in the original -- alpha 0 across every vertex is unambiguous.
		//
		// 1 = drop them (default), 0 = keep the shipped behaviour.
		int drop_clear_mode()
		{
			static live_int value(L"PCSX2_REMIX_DROPCLEAR", 1, 0, 1);
			return value.get();
		}

		bool socom_winterblade_lighting()
		{
			static live_int value(L"PCSX2_REMIX_SOCOM_WINTERBLADE_LIGHTING", 0, 0, 1);
			return value.get() != 0;
		}

		// Signed live read, for the lighting knobs only.
		//
		// env_float() CANNOT be used for these. Its last line is
		// `return (std::isfinite(parsed) && parsed > 0.f) ? parsed : fallback;`
		// -- anything <= 0 is treated as "unset" and silently replaced by the default. That is the
		// right rule for a near plane and the wrong one for every knob whose zero and whose
		// negatives are meaningful:
		//
		//   * `PCSX2_REMIX_KEY = 0` did not turn the key light off, it re-read 100. That is one
		//     half of why the light bisect that believed it was running with the key off was in
		//     fact running at full strength; the other half is the create-once problem documented
		//     on refresh_fill_lights() below.
		//   * elevation 0 (sun on the horizon) and negative elevation (lit from underneath) are
		//     values a user will legitimately ask for, and azimuth 0 is the documented compass
		//     zero -- all three would have been swallowed.
		//
		// READ LIVE ON EVERY CALL. Not `static const`, and deliberately NOT live_float either.
		// live_float re-parses only when paths::knob_generation() moves, and that counter is bumped
		// in exactly one place: apply_knob() (RemixPaths.cpp:150), which pushes the PCSX2 settings
		// .ini into the environment. The per-game <SERIAL>.conf does NOT go through it -- it calls
		// SetEnvironmentVariableW directly (RemixMaterials.cpp, refresh_game_config) and bumps
		// nothing -- so a conf-delivered value moves no generation and a live_float cache never
		// re-reads it. A latched knob is a silent no-op that looks like a broken feature; see
		// frametrace_frames() for the run that cost. These are read once per presented frame, from
		// refresh_fill_lights(), never per draw, so a GetEnvironmentVariableW each is free.
		float env_float_signed(const wchar_t* name, float fallback)
		{
			const std::wstring env = remix_ps2::read_env(name);
			if (env.empty())
				return fallback;

			const float parsed = static_cast<float>(::_wtof(env.c_str()));
			return std::isfinite(parsed) ? parsed : fallback;
		}

		// Integer twin of env_float_signed, and read live for exactly the same two reasons.
		//
		// NOT `static const`: the renderer goes live well before the per-game .conf is applied
		// (t = 9.774 vs t = 10.008 on the 2026-08-15 session), so a value latched at first call is
		// the pre-conf value forever. NOT live_int either: that re-parses only when
		// paths::knob_generation() moves, and the per-game .conf never moves it -- it calls
		// SetEnvironmentVariableW directly. Both of those were real, separately-diagnosed silent
		// no-ops. Callers below are once per camera resolve and once per presented window.
		int env_int_live(const wchar_t* name, int fallback)
		{
			const std::wstring env = remix_ps2::read_env(name);
			if (env.empty())
				return fallback;

			return static_cast<int>(::_wtoi(env.c_str()));
		}

		// PCSX2_REMIX_WORLDFIX -- the world-space un-projection audit, plus the one correction the
		// hypothesis set structurally cannot express. See the worldfix_ block further down for what
		// is measured and why WORLDPROBE cannot answer it.
		//
		//   0 = OFF. The default, and nothing in this file behaves differently.
		//   1 = AUDIT ONLY. No behaviour change; logs alpha = d(bearing)/d(yaw) for tracked static
		//       geometry, and the sign/aspect terms of the recovered projection.
		//   2 = audit, and force the recovered world to the handedness with projection m[0][0] > 0.
		//   3 = audit, and force the OPPOSITE handedness (m[0][0] < 0).
		//
		// 2 and 3 are the same one-bit change in opposite directions; exactly one of them is a
		// no-op on any given camera. Neither can alter the rendered image -- see the correction
		// site in resolve_world_camera() for why that is structural rather than hopeful.
		int worldfix_mode()
		{
			return std::clamp(env_int_live(L"PCSX2_REMIX_WORLDFIX", 0), 0, 3);
		}

		// Where a candidate came from. Kept in one place because it is printed from two, and the
		// two had already drifted -- camtest_report knew about the probe source and the per-frame
		// dump did not, so the same candidate was labelled differently in the two logs.
		const char* candidate_source_name(u8 source)
		{
			switch (source)
			{
				case 0:
					return "scan"; // heuristic 16 KB window sweep
				case 1:
					return "slice"; // back-slice, live VI base, plain LQ + immediate
				case 2:
					return "slice-tops"; // back-slice against the VIF1 TOPS bank
				case 3:
					return "pinned"; // re-read of an address a camera was recovered from
				case 4:
					return "socom-fixed"; // title-specific fixed VU block
				case 5:
					return "probe"; // GS-side synthetic, from the VU1 neighbourhood probe
				case 6:
					return "slice-auto"; // back-slice whose base came from an LQI/LQD chain
				case 7:
					return "slice-vf"; // matrix living in the VF register file
				default:
					return "?";
			}
		}

		// PCSX2_REMIX_DIVCAM -- what to do with the back-slice's own statement about which matrix
		// is the projection.
		//
		// A matrix whose result feeds the perspective divide produced the divide's denominator.
		// That is what a projection IS, and no amount of shape scoring is equivalent to it. The
		// slicer records it per matrix (RemixVU1Slice::Matrix::feeds_div) and the capture side now
		// carries it through on every candidate.
		//
		//   0 = OFF, the default. Candidates from the two new back-slice sources (slice-auto and
		//       slice-vf) are scored, split, logged and measured by CAMTEST exactly like any
		//       other, but they can never become the elected camera. Nothing about the rendered
		//       image depends on this mode. NOTE that merely PUBLISHING those candidates is a
		//       separate decision made on the VU side (PCSX2_REMIX_SLICEAUTO, PCSX2_REMIX_SLICEVF)
		//       and that one CAN change the picture -- see the note at their read site.
		//   1 = the new sources may be elected, and a feeds_div candidate gets the same +100 the
		//       title-specific fixed block gets. CHANGES THE PICTURE.
		//   2 = as 1, and every candidate that does NOT feed a divide is pushed below every one
		//       that does. CHANGES THE PICTURE, and hard: on a title whose projection the slicer
		//       cannot see, this elects nothing at all rather than the best available guess.
		//
		// Read live, and NOT through live_int, for the reasons on env_int_live. Called once per
		// presented window's camera resolve, never per draw.
		int divcam_mode()
		{
			return std::clamp(env_int_live(L"PCSX2_REMIX_DIVCAM", 0), 0, 2);
		}

		// Uniform ambient from every direction.
		//
		// DEFAULTED OFF, and by measurement rather than preference: a dome light with no
		// colorTexture contributes nothing through this API in this runtime. Rainbow Six 3
		// -state 9 renders pure black with the dome alone at radiance 20 and fully lit with the
		// distant key alone. The creation path is kept because a dome is the right shape for
		// ambient and a future runtime (or a colour texture) may honour it -- but it must not be
		// the default, because a silent no-op that looks like a tuning problem is exactly what
		// cost this project two rounds of "captures too dark to judge".
		float ambient_radiance()
		{
			return std::max(0.f, env_float_signed(L"PCSX2_REMIX_AMBIENT", 0.f));
		}

		// Directional key, and the light that actually carries the scene. Fixed in world space
		// rather than attached to the camera, so shading stays put as the player turns -- which is
		// precisely what the rotate/walk protocol has to be able to see -- and with no distance
		// falloff at all, so nothing near the camera blows out and nothing far from it is crushed.
		//
		// The default is set from the geometry, not by taste. A distant light of angular diameter
		// t delivers irradiance E = radiance * pi * (t/2)^2; at 8 degrees that solid angle is
		// 0.0153 sr, so E = 1 -- the exposure the old sphere light was calibrated to deliver at
		// the scene radius -- needs radiance ~65. 200 was measured as legible but washed out, so
		// 100 sits just above correct exposure, which is the right side to err on for a title
		// with no lights of its own.
		float key_radiance()
		{
			return std::max(0.f, env_float_signed(L"PCSX2_REMIX_KEY", 100.f));
		}

		float key_angle_degrees()
		{
			// Clamped low rather than allowed to reach 0: a distant light of angular diameter 0 is
			// a degenerate delta and the runtime is handed it verbatim. 180 is the whole sky.
			return std::clamp(env_float_signed(L"PCSX2_REMIX_KEYANGLE", 8.f), 0.01f, 180.f);
		}

		// --- where the key light is ---------------------------------------------------------
		//
		// The direction the key light TRAVELS, as shipped, kept verbatim as the default:
		//
		//     {0.35f, -0.86f, 0.37f}
		//
		// Its comment said it was "deliberately off-axis on all three so no wall, floor or ceiling
		// is lit flat-on", and nobody ever worked out the elevation that implied. MEASURED
		// 2026-08-15, and it is the whole reason these two knobs exist:
		//
		//   |d|     = sqrt(0.35^2 + 0.86^2 + 0.37^2) = sqrt(0.999) = 0.999500
		//   d/|d|   = { 0.350175, -0.860430,  0.370185}
		//   sun     = -d/|d|                            = {-0.350175, 0.860430, -0.370185}
		//   ELEV    = asin(0.860430)                    = 59.365 degrees above the horizon
		//   AZIM    = atan2(-0.350175, -0.370185)       = 223.409 degrees from +Z toward +X
		//
		// 59.4 degrees up is nearly overhead, and at that elevation a fixed light CANNOT swing in
		// front of or behind the player when they turn. Over a real 360-degree capture (1046 camera
		// samples, pitch dead constant at -9.2 to -9.1 degrees) the angle between camera forward and
		// this direction only ever moved between 50.2 and 111.5 degrees: it circles high overhead
		// and sits up-and-behind whichever way you face. Combined with angularDiameterDegrees 8
		// (sixteen times the real sun's ~0.5) and no distance falloff at all, that reads to the user
		// as "a light that follows the camera" -- the same appearance WORLDPROBE was built to
		// attribute to a rotating world, and this is the simpler explanation for it. It does not
		// refute the un-projection question WORLDPROBE asks; it removes the light from the list of
		// things that question has to explain.
		//
		// So the sun is placed in degrees, not as a normalized triple, because elevation and azimuth
		// are what a person can actually reason about ("lower, and behind me").
		//
		// THE CONVENTION, stated once so nothing has to guess:
		//   ELEV -- degrees the sun SITS ABOVE THE HORIZON. +90 is straight overhead (light travels
		//           straight down), 0 is on the horizon, negative is below it. Clamped to [-90, 90].
		//   AZIM -- degrees around the horizon, measured FROM THE +Z AXIS and turning TOWARD +X.
		//           So 0 = the sun is on +Z, 90 = on +X, 180 = on -Z, 270 = on -X. Wraps freely;
		//           360 and -0.0 are the same place.
		//
		//   sun_dir = { cos(ELEV)*sin(AZIM), sin(ELEV), cos(ELEV)*cos(AZIM) }, and the value handed
		//   to Remix is its negation, because remixapi_LightInfoDistantEXT::direction is the
		//   direction the light travels, not the direction it comes from.
		//
		// Both default to the numbers derived above, so an install that sets neither renders exactly
		// as it did before this existed -- and when both are at their defaults the shipped literal
		// vector is used directly rather than reconstructed, so it is bit-exact and not
		// almost-exact. (Reconstructing 59.365/223.409 lands within 3e-6 per component, which is
		// invisible, but "invisible" is not the promise being made here.)
		constexpr float s_key_shipped_direction[3] = {0.35f, -0.86f, 0.37f};

		// SOCOM Winterblade's authored key, for reference: elevation 30.2, azimuth 119.1.
		constexpr float s_key_winterblade_direction[3] = {-0.9f, -0.6f, 0.5f};

		constexpr float s_key_elevation_default = 59.365f;
		constexpr float s_key_azimuth_default = 223.409f;

		float key_elevation_degrees()
		{
			return std::clamp(
				env_float_signed(L"PCSX2_REMIX_KEYELEV", s_key_elevation_default), -90.f, 90.f);
		}

		float key_azimuth_degrees()
		{
			return env_float_signed(L"PCSX2_REMIX_KEYAZIM", s_key_azimuth_default);
		}

		// Elevation/azimuth -> the direction the light travels. See the block above for the
		// convention and for why the sign is flipped.
		void key_direction_from_angles(float elevation_deg, float azimuth_deg, float (&out)[3])
		{
			constexpr float to_radians = 3.14159265358979323846f / 180.f;

			const float elevation = elevation_deg * to_radians;
			const float azimuth = azimuth_deg * to_radians;
			const float cos_elevation = std::cos(elevation);

			out[0] = -(cos_elevation * std::sin(azimuth));
			out[1] = -std::sin(elevation);
			out[2] = -(cos_elevation * std::cos(azimuth));
		}

		// The world-space (or view-space, whichever is being submitted) bounding box of the
		// geometry submitted this frame, and the finished frame's copy. This is the measurement
		// the debug light is scaled from: the previous fixed radius/radiance were tuned for a
		// synthetic camera with near 0.1, and a title whose world spans thousands of units was
		// lit by a pinprick whose 1/d^2 falloff made everything past a few metres black.
		struct scene_bounds
		{
			bool valid = false;
			float min[3] = {0.f, 0.f, 0.f};
			float max[3] = {0.f, 0.f, 0.f};

			void add(const float (&p)[3])
			{
				if (!valid)
				{
					valid = true;
					for (u32 i = 0; i < 3; ++i)
						min[i] = max[i] = p[i];

					return;
				}

				for (u32 i = 0; i < 3; ++i)
				{
					min[i] = std::min(min[i], p[i]);
					max[i] = std::max(max[i], p[i]);
				}
			}

			// Half the diagonal: the radius of the sphere the frame's geometry fits in.
			float radius() const
			{
				if (!valid)
					return 1.f;

				const float dx = max[0] - min[0];
				const float dy = max[1] - min[1];
				const float dz = max[2] - min[2];
				const float diag = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
				return std::isfinite(diag) ? std::max(0.5f * diag, 1e-3f) : 1.f;
			}

			// The raw diagonal, unclamped. radius() floors at 1e-3 because it feeds light and
			// gate scaling where a zero would divide; the extent ratio below has to see genuinely
			// tiny primitives as tiny, so it cannot use that.
			float diagonal() const
			{
				if (!valid)
					return 0.f;

				const float dx = max[0] - min[0];
				const float dy = max[1] - min[1];
				const float dz = max[2] - min[2];
				const float diag = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
				return std::isfinite(diag) ? diag : 0.f;
			}
		};

		scene_bounds s_frame_bounds{};
		scene_bounds s_last_bounds{};

		// DEPTHDIAG (2026-08-27). The EE camera installs every frame but geometry un-projects
		// to a 4-8 unit ball, so the guest's clip w and our recovered world units disagree by
		// some factor. Guessing it twice made things worse; measure it instead. Accumulated
		// per frame in the vertex loop, reported and reset once a second.
		world_camera s_ee_pending_camera{};
		bool s_ee_pending_valid = false;
		u64 s_ee_installed = 0;

		float s_dd_w_min = 1e30f, s_dd_w_max = -1e30f;
		float s_dd_p_min[3] = {1e30f, 1e30f, 1e30f};
		float s_dd_p_max[3] = {-1e30f, -1e30f, -1e30f};
		u64 s_dd_verts = 0;

		// Whether s_last_bounds was captured while world-anchored. Without this the drift guard
		// cannot tell world-space bounds from view-space ones, and comparing a world-space eye
		// against view-space bounds is a units error -- the bug that locked Rainbow Six 3 out of
		// world mode and, once "fixed" by restricting when the guard runs, let SOCOM's bad camera
		// through unchallenged.
		bool s_frame_bounds_world = false;
		bool s_last_bounds_world = false;

		// Diagnostic (phase 1 of per-draw camera association): the kick sequence the VU side had
		// reached when the candidate set this frame is drawing from was published. Compared against
		// a live RemixVU1Capture::KickSeq() at draw time, the difference is the GS thread's lag in
		// kicks -- and therefore whether a live read can attribute a draw to its own kick at all.
		u64 s_latched_kick_seq = 0;

		// Distinct per-kick cameras already written to the draw dump. Small fixed set: a title
		// rendering with more than a handful of view matrices per frame would be a finding in
		// itself, and the count on the stats line stays exact regardless of this cap.
		u64 s_logged_kick_cams[8] = {};
		u32 s_logged_kick_cam_count = 0;

		// The largest eye-space depth submitted this frame, and the hashes of world matrices
		// refuted by the extent check below.
		//
		// This is the gate that catches a camera which is internally perfectly consistent and
		// still wrong. w = 1/Q is the guest's own eye-space depth, so every vertex it submits
		// lies within w of the camera -- the whole visible scene fits in a sphere of a few times
		// the frame's largest w. A recovered camera whose un-projection scatters that same
		// geometry across a thousand times that distance is not the camera, whatever its internal
		// numbers say.
		//
		// Measured: SOCOM's accepted camera has depth scale 0.9999 and anisotropy 2.6x -- both
		// exactly right -- and still submits scene radius 5,807 from geometry whose largest w is
		// about 6. Rainbow Six 3 submits radius 9 against w in the same range. The w row of the
		// solve is right and its x/y rows are not, which no self-consistency test can see.
		float s_frame_max_w = 0.f;
		std::unordered_set<u64> s_refuted_matrices;

		// Largest and smallest per-draw world-space AABB diagonal submitted this frame.
		//
		// This is the quantity the device-loss hypothesis is actually about, and nothing has ever
		// measured it. maxpos measures absolute magnitude; this measures the *ratio* between the
		// biggest and smallest thing in one frame, which is what decides whether a BVH can
		// separate them. A TLAS of single-triangle instances whose bounds span three orders of
		// magnitude cannot be partitioned, so most rays descend most of the tree -- which is the
		// only remaining explanation for a few thousand rays hanging a 4070 Ti (phase 5).
		//
		// Per draw rather than per triangle: a draw is one instance and one BLAS, so the draw's
		// AABB is exactly what the TLAS sees.
		float s_frame_max_extent = 0.f;
		float s_frame_min_extent = std::numeric_limits<float>::max();
		float s_extent_ratio_peak = 0.f;
		double s_extent_ratio_total = 0.0;
		u64 s_extent_ratio_frames = 0;

		// --- Z -> w calibration, for FST=1 recovery -----------------------------------------
		//
		// PRIM->FST=1 means the guest supplied direct UV texels instead of ST/Q, so Q is not the
		// perspective divisor and w = 1/Q does not exist. Those draws are gated off, and on
		// SOCOM that is 37-42% of everything (Rainbow Six 3: ~2%, which is why it never mattered
		// until a second title arrived).
		//
		// Every vertex still carries Z. The question is whether Z maps to depth by a rule we can
		// *learn* rather than assume -- and the draws where TME=1 && FST=0 give it away for
		// free, because they carry Z and the true w on the same vertices. That is a calibration
		// set costing nothing.
		//
		// Two models are fitted, because Rainbow Six 3 is the documented cautionary case: its
		// VU1 emits z = w + const (col2 == col3, m[3][2] = 2715.9), so Z is linear in w, whereas
		// a textbook perspective depth buffer is linear in 1/w. Assuming either one globally is
		// exactly the mistake that produced a 1.1e7-deep frustum in phase 2.
		//
		//   model A:  Q = a*zn + b   ->  w = 1 / (a*zn + b)     (perspective depth)
		//   model B:  w = a*zn + b   ->  w = a*zn + b           (R6 3's fused w + const)
		//
		// zn is Z normalised by the ZBUF format's maximum, which differs for 32/24/16-bit Z
		// (GSRendererHW.cpp:2181). R^2 for both is reported so a title whose Z is unusable says
		// so in the counters instead of rendering garbage.
		struct z_fit
		{
			double n = 0.0, sx = 0.0, sxx = 0.0;
			double sy_a = 0.0, sxy_a = 0.0, syy_a = 0.0; // y = Q = 1/w
			double sy_b = 0.0, sxy_b = 0.0, syy_b = 0.0; // y = w

			void add(double zn, double w)
			{
				if (!(w > 0.0) || !std::isfinite(w) || !std::isfinite(zn))
					return;

				const double q = 1.0 / w;

				n += 1.0;
				sx += zn;
				sxx += zn * zn;
				sy_a += q;
				sxy_a += zn * q;
				syy_a += q * q;
				sy_b += w;
				sxy_b += zn * w;
				syy_b += w * w;
			}

			void merge(const z_fit& o)
			{
				n += o.n; sx += o.sx; sxx += o.sxx;
				sy_a += o.sy_a; sxy_a += o.sxy_a; syy_a += o.syy_a;
				sy_b += o.sy_b; sxy_b += o.sxy_b; syy_b += o.syy_b;
			}

			// Ages the accumulator once per frame.
			//
			// The Z -> w mapping is a property of the *current projection*, and the projection
			// changes -- between scenes, and when SOCOM's world camera engages part way through
			// a session. A session-wide accumulator therefore fits a mixture and drifts: the
			// same state measured R^2 = 1.00000 over 1.5 M vertices from one scene and 0.99856
			// over 10.6 M spanning several. Decaying makes the fit describe the last few dozen
			// frames, which is the only window in which "the projection" is a single thing.
			void decay(double factor)
			{
				n *= factor; sx *= factor; sxx *= factor;
				sy_a *= factor; sxy_a *= factor; syy_a *= factor;
				sy_b *= factor; sxy_b *= factor; syy_b *= factor;
			}

			// Ordinary least squares of y on zn, plus the coefficient of determination.
			bool solve(bool model_a, double& a, double& b, double& r2) const
			{
				if (n < 32.0)
					return false;

				const double sy = model_a ? sy_a : sy_b;
				const double sxy = model_a ? sxy_a : sxy_b;
				const double syy = model_a ? syy_a : syy_b;

				const double sxx_c = sxx - (sx * sx / n);
				const double syy_c = syy - (sy * sy / n);
				const double sxy_c = sxy - (sx * sy / n);

				if (!(sxx_c > 0.0) || !(syy_c > 0.0))
					return false;

				a = sxy_c / sxx_c;
				b = (sy / n) - (a * (sx / n));
				r2 = (sxy_c * sxy_c) / (sxx_c * syy_c);

				return std::isfinite(a) && std::isfinite(b) && std::isfinite(r2);
			}
		};

		z_fit s_zfit;

		// --- vertex-colour distribution ------------------------------------------------------
		//
		// The question this answers: does this title bake its lighting into per-vertex colour?
		//
		// The PS2 GS has one texture unit, so a lightmap costs a second blended pass. Titles that
		// do not pay for that pass usually bake into vertex colours instead. We have both idioms
		// in evidence: Rainbow Six 3 multi-passes (the 30/30/30 per-channel FBMSK we identify and
		// skip), SOCOM does not multi-pass at all.
		//
		// The discriminator is NOT "are the colours non-zero" -- it is whether they *vary*. On the
		// PS2 a vertex colour of 128 is unity for modulation, so an unlit title emits a constant
		// 0x80808080 and a title with baked lighting emits a spread. Both look like "there is
		// colour data" to a naive check, which is why `neutral` and `draws const/vary` are counted
		// separately from the means.
		struct vcolor_stats
		{
			u64 vertices = 0;
			u64 neutral = 0; // exactly (128,128,128): PS2 unity, i.e. "no bake"
			u64 draws_constant = 0; // every vertex in the draw shares one colour
			u64 draws_varying = 0;
			double sum_lum = 0.0;
			double sum_lum2 = 0.0;
			u64 hist[8] = {}; // luminance, 32-wide buckets over 0..255

			void add(u32 r, u32 g, u32 b)
			{
				++vertices;
				if (r == 128 && g == 128 && b == 128)
					++neutral;

				const double lum = (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
				sum_lum += lum;
				sum_lum2 += lum * lum;
				++hist[std::min<u32>(7, static_cast<u32>(lum) >> 5)];
			}

			void merge(const vcolor_stats& o)
			{
				vertices += o.vertices;
				neutral += o.neutral;
				draws_constant += o.draws_constant;
				draws_varying += o.draws_varying;
				sum_lum += o.sum_lum;
				sum_lum2 += o.sum_lum2;
				for (u32 i = 0; i < 8; ++i)
					hist[i] += o.hist[i];
			}

			double mean() const { return (vertices > 0) ? (sum_lum / static_cast<double>(vertices)) : 0.0; }

			double stddev() const
			{
				if (vertices < 2)
					return 0.0;

				const double m = mean();
				const double var = (sum_lum2 / static_cast<double>(vertices)) - (m * m);
				return (var > 0.0) ? std::sqrt(var) : 0.0;
			}
		};

		vcolor_stats s_vcolor;

		// FST=1 recovery, and the two knobs that decide whether it is safe to use.
		//
		// FSTZ: 1 (default) = recover FST=1 draws from Z once the calibration is good enough.
		// FSTZR2: the R^2 model A must reach. Deliberately severe -- the measured split between
		// the two titles is not marginal, so there is no reason to sit near a boundary:
		//   SOCOM (SCUS-97545), 1,548,714 vertices : R^2 = 1.00000
		//   Rainbow Six 3 (SLUS-20883), 6,228,855 : R^2 = 0.13231
		// A title whose Z is unusable therefore degrades to today's behaviour rather than
		// rendering garbage, which is the whole point of gating on fit rather than on a list.
		int fst_z_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_FSTZ", 1), 0, 1));

			return value;
		}

		// Relative spread of w across a draw, below which the draw has no perspective and is 2D.
		//
		// The existing const-Q gate is r.m_vt.m_eq.q, an EXACT equality test, and 2D quads leak
		// straight through it because their Q varies in the last few bits. MEASURED on SOCOM's
		// deploy menu, every submitted draw: w=[6.0,6.0], z=[38275,38275], px rects 20x20 within a
		// 640x448 target, depth(r=1 w=0) -- so classify_sky cannot catch them either, because depth
		// testing is on. They were being submitted as world geometry, which is why the 2D main menu
		// renders in-world as glowing boxes now that untextured draws get albedo.
		//
		// (max_w - min_w) / max_w is scale-free, so one threshold serves a scene radius of 4 and one
		// of 11,785. 0.001 is deliberately far below anything with real depth: the menu measures 0.
		// It makes the same trade the const-Q gate always made -- geometry viewed exactly head-on is
		// lost -- but on a robust test rather than an exact one. 0 disables.
		float w_flat_limit()
		{
			static const float value = []() -> float {
				const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_WFLAT");
				if (env.empty())
					return 0.001f;

				const float parsed = static_cast<float>(::_wtof(env.c_str()));
				return (std::isfinite(parsed) && parsed >= 0.f) ? parsed : 0.001f;
			}();

			return value;
		}

		// Tell Remix that per-vertex colour IS baked lighting, via
		// remixapi_InstanceInfoBlendEXT::isVertexColorBakedLighting.
		//
		// This is the semantically correct answer for a title that bakes, and it took a dead end to
		// find. The plan was to recover the multitexture lightmap passes -- the PS2 has one texture
		// unit, so titles lay a base pass then re-draw with the lightmap bound. MEASURED on SOCOM
		// in-mission: multipass_overlay = 0 out of 1,038,746 draws seen, and fbmsk 2,686 (0.26%).
		// SOCOM does not do it at all; that pattern is Rainbow Six 3's (90 of 231 dumped draws split
		// 30/30/30 across FBMSK masks). So there is no lightmap in SOCOM's draw stream to fold in.
		//
		// Its baked lighting is entirely in the vertex colours: 43,825,580 vertices at mean luminance
		// 45.7/255, 0.0% at PS2 unity (128), 75% of draws carrying a gradient. Using that as albedo
		// double-darkens (albedo x baked shadow, then path-traced and lit again), which is why
		// VCOLOR=0 discards it -- but discarding it throws the level's only lighting away.
		//
		// This field is the third option: submit the colour AND tell the runtime what it means, so it
		// is treated as irradiance rather than as surface albedo. Pair with VCOLOR=1.
		// Requires alpha_state_mode()==2, since that is what chains the blend struct at all.
		int vcolor_baked_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_VCBAKED", 0), 0, 1));

			return value;
		}

		// Whether per-vertex RGB reaches Remix as vertex colour.
		//
		// 1 (default) = submit it. 0 = force white and let the path tracer do all the lighting.
		//
		// The distinction is whether the title bakes its lighting into vertex colour, and that is
		// measured, not guessed. SOCOM Combined Assault, over 43,825,580 vertices in-mission:
		//   mean luminance 45.7 of 255, neutral(128,128,128) 0.0%, 83% of vertices in the two
		//   darkest buckets, and 1,200,752 draws with *varying* colour against 397,110 constant.
		// PS2 unity is 128, so essentially nothing is at full brightness and three quarters of
		// draws carry a gradient across the surface. That is baked lighting, not a material tint.
		//
		// Submitting it means albedo gets multiplied by the game's pre-baked shadows and then path
		// traced and lit again -- doubly darkened, which is what left the mission at mean luminance
		// 31 once the volumetric wash was removed. A remaster wants the albedo and its own lighting.
		//
		// Defaults to 1 so nothing changes for titles that do not bake; set per-game where the
		// vertex-colour measurement above says otherwise.
		int vcolor_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_VCOLOR", 1), 0, 1));

			return value;
		}

		// UNTEXZ: 1 (default) = recover *untextured* draws from Z as well, on the same calibration
		// and the same R^2 gate as FSTZ above.
		//
		// This is the largest single visual lever measured on this backend. An untextured draw has
		// no Q at all -- TME=0 means nothing ever wrote one -- so before this it was dropped
		// outright, and on SOCOM Combined Assault in-mission that was 4,395,585 of 8,417,784 draws
		// seen: 52% of the scene, and 96% of every skip. Half the world was missing, which is what
		// the user was seeing as geometry "shredded" into disconnected shards -- the surviving
		// textured fragments floating in the gaps left by the dropped half. The submitted geometry
		// itself was never displaced: the hyperextension counter read peak 2.6x against a 32x line
		// through all of it.
		//
		// Safe to default on for the same reason FSTZ is: the gate is the *fit*, not a title list.
		// SOCOM's Z predicts Q with R^2 = 1.00000 over 1.5M vertices, so recovery is essentially
		// exact there, while Rainbow Six 3 sits at 0.13231 and therefore keeps today's behaviour
		// without anything having to know which title it is.
		int untex_z_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_UNTEXZ", 1), 0, 1));

			return value;
		}

		// Per-frame retention of the Z->w accumulator. 0.9 gives an effective window of roughly
		// ten frames, which at thousands of calibration vertices per frame is still an enormous
		// sample. 1.0 restores the session-wide behaviour for comparison.
		double fst_z_decay()
		{
			static const double value = []() -> double {
				const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_FSTZDECAY");
				if (env.empty())
					return 0.9;

				const double parsed = ::_wtof(env.c_str());
				return (std::isfinite(parsed) && parsed > 0.0 && parsed <= 1.0) ? parsed : 0.9;
			}();

			return value;
		}

		// Whether to submit FST draws whose Z is constant across the draw.
		//
		// Measured on SOCOM: *every* FST draw is flat in Z -- 1,935 of 1,935 in one session, and
		// the same in the indoor state. With one depth for the whole draw the un-projection can
		// place it correctly but cannot give it shape, so what comes out is a camera-facing quad
		// at the right distance. That is the right answer for a sprite, billboard or particle
		// and the wrong answer for anything else, and nobody has looked yet.
		//
		// Defaulted OFF because "FST draws that render in the wrong place would be worse than
		// not rendering them", and because a flat quad at the wrong depth is exactly the kind of
		// full-screen occluder that made the developer menu unusable in phase 3d.
		int fst_flat_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_FSTFLAT", 0), 0, 1));

			return value;
		}

		double fst_z_min_r2()
		{
			static const double value = []() -> double {
				const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_FSTZR2");
				if (env.empty())
					return 0.999;

				const double parsed = ::_wtof(env.c_str());
				return (std::isfinite(parsed) && parsed > 0.0 && parsed <= 1.0) ? parsed : 0.999;
			}();

			return value;
		}

		// Minimum accumulated vertices before the calibration may be *used*. z_fit::solve only
		// requires 32, which is one small draw's worth -- and a single near-planar draw fits a
		// line at R^2 ~ 1 trivially, so that floor lets a burst through before any real evidence
		// exists. Measured: Rainbow Six 3, whose Z is genuinely unusable (R^2 0.48 over 30,210
		// vertices), still recovered 12 untextured draws through exactly that hole.
		//
		// 4096 is well above one draw but reached within a frame or two of real geometry, and the
		// accumulator decays at 0.9/frame so steady state is ~10x the per-frame sample count. For
		// scale: SOCOM fits over 1,548,714 vertices, Rainbow Six 3 over 6,228,855.
		double fst_z_min_samples()
		{
			static const double value = []() -> double {
				const double parsed = static_cast<double>(
					remix_ps2::read_env_int(L"PCSX2_REMIX_FSTZMINN", 4096));
				return (parsed >= 32.0) ? parsed : 4096.0;
			}();

			return value;
		}

		// The live calibration, or false while it is not yet good enough. Re-solved per draw;
		// it is a handful of flops against an accumulator that only grows.
		bool fst_z_solution(double& a, double& b)
		{
			if (fst_z_mode() == 0)
				return false;

			if (s_zfit.n < fst_z_min_samples())
				return false;

			double r2 = 0.0;
			if (!s_zfit.solve(true, a, b, r2))
				return false;

			return r2 >= fst_z_min_r2();
		}

		// Explicit absolute overrides. When unset (the default) radius and radiance are derived
		// from the frame's measured extent instead, which is scale-free -- see place_debug_light.
		float debug_light_radius_override()
		{
			static const float value = []() -> float {
				if (const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_LIGHTRADIUS"); !env.empty())
				{
					const float parsed = static_cast<float>(::_wtof(env.c_str()));
					if (std::isfinite(parsed) && parsed > 0.f)
						return parsed;
				}

				return 0.f;
			}();

			return value;
		}

		float debug_light_radiance_override()
		{
			static const float value = []() -> float {
				if (const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_LIGHTRADIANCE"); !env.empty())
				{
					const float parsed = static_cast<float>(::_wtof(env.c_str()));
					if (std::isfinite(parsed) && parsed > 0.f)
						return parsed;
				}

				return 0.f;
			}();

			return value;
		}

		// Target irradiance at the scene radius, in Remix's units. The whole point of deriving
		// radius and radiance from the measured extent is that this number then means the same
		// thing whether a guest world unit is a metre or a centimetre.
		float debug_light_exposure()
		{
			static const float value = []() -> float {
				if (const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_LIGHTEXPOSURE"); !env.empty())
				{
					const float parsed = static_cast<float>(::_wtof(env.c_str()));
					if (std::isfinite(parsed) && parsed > 0.f)
						return parsed;
				}

				return 1.f;
			}();

			return value;
		}

		// Volumetric (in-scattering) contribution of the debug light. Off by default: it is a
		// per-froxel integration that a placeholder light has no business paying for, the
		// runtime already warns that the froxel volume is larger than this camera's frustum,
		// and it is one of the two terms that scale with how strong the light is.
		float debug_light_volumetric()
		{
			static const float value = []() -> float {
				if (const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_LIGHTVOLUMETRIC"); !env.empty())
				{
					const float parsed = static_cast<float>(::_wtof(env.c_str()));
					if (std::isfinite(parsed) && parsed >= 0.f)
						return parsed;
				}

				return 0.f;
			}();

			return value;
		}

		// Fraction of the scene radius the sphere light's own radius is set to. Bigger means
		// softer shadows and a flatter falloff across the scene; the radiance compensates so
		// the irradiance delivered at the scene radius is unchanged (it is exactly the exposure
		// above, for any fraction).
		//
		// MEASURED, and load-bearing: the emitter's *surface radiance* -- not the light's total
		// power, which this parameterisation holds fixed -- is what decides whether the session
		// survives. Once real albedo textures exist, a small very bright emitter reproducibly
		// drives the GPU to VK_ERROR_DEVICE_LOST within about six seconds:
		//   fraction 0.05 -> radiance 382    dies at ~6 s (twice)
		//   radius 1.0, radiance 10000       dies at ~6 s (the pre-material default)
		//   radiance 100                     survives 45 s+
		// At 0.1 the same illumination is delivered at radiance ~100. Do not lower this
		// fraction without re-running that comparison.
		constexpr float s_light_radius_fraction = 0.1f;

		// A distant (directional) light along the camera forward -- a headlight with no
		// distance falloff at all. Off by default because its radiance units are not the same
		// as a sphere light's and the right value has to be found by looking.
		float sun_radiance()
		{
			static const float value = []() -> float {
				if (const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_SUN"); !env.empty())
				{
					const float parsed = static_cast<float>(::_wtof(env.c_str()));
					if (std::isfinite(parsed) && parsed > 0.f)
						return parsed;
				}

				return 0.f;
			}();

			return value;
		}

		float sun_angle_degrees()
		{
			static const float value = []() -> float {
				if (const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_SUNANGLE"); !env.empty())
				{
					const float parsed = static_cast<float>(::_wtof(env.c_str()));
					if (std::isfinite(parsed) && parsed > 0.f && parsed < 180.f)
						return parsed;
				}

				return 30.f;
			}();

			return value;
		}

		// Field of view of the synthetic camera the view-space tier un-projects through. The
		// same number is handed to the parameterized camera, so the two agree by construction.
		constexpr float s_debug_fov_y_degrees = 70.f;

		float window_aspect()
		{
			return (s_window_width > 0 && s_window_height > 0) ?
			           (static_cast<float>(s_window_width) / static_cast<float>(s_window_height)) :
			           (16.f / 9.f);
		}

		// The device's cached surface size goes stale in Remix mode: GSDevice11::ResizeWindow
		// returns early when there is no swapchain (GSDevice11.cpp:1040-1041), so it never
		// updates m_window_info. Ask the window itself instead, once per frame.
		void refresh_window_size()
		{
			if (!s_hwnd)
				return;

			RECT rc{};
			if (!GetClientRect(s_hwnd, &rc))
				return;

			const int width = static_cast<int>(rc.right - rc.left);
			const int height = static_cast<int>(rc.bottom - rc.top);
			if (width > 0 && height > 0)
			{
				s_window_width = width;
				s_window_height = height;
			}
		}

		// The debug sphere light, sized from the frame's measured extent rather than from a
		// fixed constant.
		//
		// A sphere light of radius R and radiance L delivers irradiance ~ L*pi*(R/d)^2 at
		// distance d. Setting R = f*S (S = scene radius) makes L = E/(pi*f^2) -- independent of
		// S -- so the same numbers work whether a guest world unit is a metre or a centimetre.
		// Getting this wrong is what made world mode dark past a few metres: with R fixed at
		// 0.1 in a world thousands of units across, everything but the nearest surface was
		// receiving essentially nothing.
		// Crash-bisection arms. Both default off, so they change nothing until asked for.
		//
		//   PCSX2_REMIX_SUBMITDELAY  frames of game geometry to withhold after the renderer goes
		//                            live -- isolates "is it the geometry we submit?"
		//   PCSX2_REMIX_NODEBUGSCENE build no debug mesh and no debug light at all -- isolates
		//                            "is it the debug scene itself?". The debug light is a
		//                            camera-attached emitter in a scene that has no other
		//                            lights, which is an unusual thing to hand a path tracer,
		//                            and its radiance and radius have been tested but its
		//                            *existence* never has.
		u64 submit_delay_frames()
		{
			static const u64 value =
				static_cast<u64>(std::max<s64>(0, remix_ps2::read_env_int(L"PCSX2_REMIX_SUBMITDELAY", 0)));
			return value;
		}

		// Edge length, as a fraction of the scene radius, below which a triangle is treated as
		// having no area. Squared into a |cross| threshold at the use site.
		float degenerate_area_epsilon()
		{
			static const float value = []() -> float {
				const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_DEGENEPS");
				if (env.empty())
					return 1e-4f;

				const float parsed = static_cast<float>(::_wtof(env.c_str()));
				return (std::isfinite(parsed) && parsed >= 0.f) ? parsed : 1e-4f;
			}();

			return value;
		}

		// Crash bisection arm 1: build every mesh exactly as normal, but never DrawInstance.
		// Splits the fault across the one boundary nothing has tested -- acceleration-structure
		// build from the geometry, versus instance/TLAS submission and everything the runtime
		// does with it per frame.
		// Ceiling on DrawInstance calls per frame; 0 = unlimited. Bisects instancing *volume*
		// against instancing *content* without needing a hypothesis about either.
		u64 instance_budget()
		{
			static const u64 value =
				static_cast<u64>(std::max<s64>(0, remix_ps2::read_env_int(L"PCSX2_REMIX_INSTBUDGET", 0)));
			return value;
		}

		// Crash bisection: instance ONE reused mesh handle, at the normal per-frame count.
		// Mesh creation churn is untouched, so paired with the no-instancing arm (which
		// survives 20/20 with full creation churn) this separates instancing *handle diversity*
		// from instancing *call count* -- the two explanations the dose-response cannot tell
		// apart.
		bool reuse_one_handle()
		{
			static const bool value = remix_ps2::read_env_int(L"PCSX2_REMIX_REUSEHANDLE", 0) != 0;
			return value;
		}

		remixapi_MeshHandle s_reuse_handle = nullptr;

		// The decisive follow-up to the reused-handle arm. That arm instanced ONE long-lived
		// handle ~190 times a frame and survived 20/20 against a 1/20 control, which was read as
		// "the runtime cannot sustain many DISTINCT BLASes per frame". But it changed two things
		// at once: handle *diversity* fell from ~190 to 1, and handle *age* went from
		// "created this frame" to "created once and held".
		//
		// Only one of those is fixable by stable mesh identity. Stable identity keeps a handle
		// alive across frames -- it does NOT reduce how many distinct handles a frame references,
		// because a frame still draws the same number of distinct objects. So if diversity is the
		// fault, the whole feature is a dead end and must not be built.
		//
		// This arm separates them: instance N distinct, long-lived, pinned handles round-robin at
		// the normal per-frame rate, with mesh creation churn untouched. N=1 reproduces the
		// reused-handle arm; N at the frame's natural draw count holds diversity at its real
		// value while making every instanced BLAS old.
		u64 reuse_pool_size()
		{
			static const u64 value =
				static_cast<u64>(std::max<s64>(0, remix_ps2::read_env_int(L"PCSX2_REMIX_REUSEPOOL", 0)));
			return value;
		}

		std::vector<u64> s_pool_hashes;
		std::unordered_set<u64> s_pinned_hashes;

		bool no_draw_instance()
		{
			static const bool value = remix_ps2::read_env_int(L"PCSX2_REMIX_NODRAWINSTANCE", 0) != 0;
			return value;
		}

		bool no_debug_scene()
		{
			static const bool value = remix_ps2::read_env_int(L"PCSX2_REMIX_NODEBUGSCENE", 0) != 0;
			return value;
		}

		// Defined with the rest of the world-camera parameters further down.
		float world_near_plane();
		float world_far_plane(float near_plane);
		float camera_distance_limit();
		float camera_scale_limit();
		float camera_anisotropy_limit();
		float camera_extent_limit();

		// --- authored level lights ------------------------------------------------------------
		//
		// R6 3's levels are Unreal packages: Maps/Alcatraz.rsm (v118, licensee 21) carries 212
		// light actors -- 211 Light + 1 Sunlight -- with real world Locations, LightBrightness,
		// LightRadius, LightHue and LightSaturation. Extracted offline (the .rsm lives on the Xbox
		// release, not in the PS2 install, so runtime parsing is not an option) into
		//   RemixGames\<SERIAL>\lights_<LEVEL>.txt
		//
		// OFF by default. LIGHTMODE stays exactly as it was so the single distant fill remains
		// available for A/B -- 212 uncalibrated lights will not be right on the first try.
		int level_lights_mode()
		{
			return std::clamp(env_int_live(L"PCSX2_REMIX_LEVELLIGHTS", 0), 0, 1);
		}

		// Brightness scale. UE2 brightness is ~0-255 and Remix wants radiance, so the conversion
		// needs one calibration constant. Live so it can be tuned without a rebuild.
		float level_light_scale()
		{
			static live_float value(L"PCSX2_REMIX_LEVELLIGHTSCALE", 1.f);
			return std::max(0.f, value.get());
		}

		// The map's lights are at TRUE world scale (hundreds of units apart, radius 42-128),
		// but the un-projection submits geometry compressed to a ~6 unit blob at roughly the
		// right world centre. Measured 2026-08-27: `scene r 6` with `maxpos 20559`, lights
		// spread over ~12,000 units. So every bulb sits astronomically far from the geometry
		// with a radius far too small to reach it -- accepted by the runtime (Light Statistics
		// showed 174 sphere lights live) and contributing nothing.
		//
		// Shrink the rig about the eye by the same factor the world is compressed by, radius
		// included. 1.0 is the raw map scale.
		float level_light_pos_scale()
		{
			static live_float value(L"PCSX2_REMIX_LEVELLIGHTPOSSCALE", 1.f);
			return std::max(1e-6f, value.get());
		}

		// Radius around the camera outside which an authored light is not submitted. 0 disables the
		// cull. Only the geometry the guest actually drew is submitted, so a light in another room
		// has no walls to occlude it and lights this one straight through them.
		// Physical radius of an authored sphere emitter, in world units. 0 keeps the authored value.
		// UE2's LightRadius is a FALLOFF DISTANCE, not an emitter size, and remixapi's sphere radius
		// is the emitter itself -- passing one into the other turns every lamp into a ball up to 256
		// units across inside a ~400-unit corridor, which then intersects and occludes the walls.
		float level_light_radius()
		{
			static live_float value(L"PCSX2_REMIX_LEVELLIGHTRADIUS", 0.f);
			return std::max(0.f, value.get());
		}

		float level_light_range()
		{
			static live_float value(L"PCSX2_REMIX_LEVELLIGHTRANGE", 0.f);
			return std::max(0.f, value.get());
		}

		struct level_light
		{
			remixapi_LightHandle handle;
			float position[3];
			bool distant; // a directional light has no position to cull against
		};
		std::vector<level_light> s_level_lights;
		bool s_level_lights_built = false;

		// UE2 hue/saturation -> linear RGB. Saturation is INVERTED in UE2: 255 is white and 0 is
		// fully saturated, which is the opposite of every other engine and easy to get backwards.
		void ue2_hue_sat_to_rgb(u32 hue, u32 sat, float (&rgb)[3])
		{
			const float h = (static_cast<float>(hue & 0xFF) / 255.f) * 6.f;
			const float s = 1.f - (static_cast<float>(sat & 0xFF) / 255.f);
			const int i = static_cast<int>(h) % 6;
			const float f = h - std::floor(h);
			const float q = 1.f - (s * f);
			const float t = 1.f - (s * (1.f - f));
			const float n = 1.f - s;
			switch (i)
			{
				case 0:  rgb[0]=1.f; rgb[1]=t;   rgb[2]=n;   break;
				case 1:  rgb[0]=q;   rgb[1]=1.f; rgb[2]=n;   break;
				case 2:  rgb[0]=n;   rgb[1]=1.f; rgb[2]=t;   break;
				case 3:  rgb[0]=n;   rgb[1]=q;   rgb[2]=1.f; break;
				case 4:  rgb[0]=t;   rgb[1]=n;   rgb[2]=1.f; break;
				default: rgb[0]=1.f; rgb[1]=n;   rgb[2]=q;   break;
			}
		}

		void build_level_lights()
		{
			if (s_level_lights_built || level_lights_mode() == 0)
				return;

			// Anchor the rig on the eye, so wait for a camera. Without one there is nothing to
			// shrink about and every light would land in the wrong place permanently.
			if (!s_active_camera.valid)
				return;
			s_level_lights_built = true;

			const float pscale = level_light_pos_scale();
			const float ax = s_active_camera.position[0];
			const float ay = s_active_camera.position[1];
			const float az = s_active_camera.position[2];

			const std::string dir = remix_ps2::paths::game_dir();
			if (dir.empty())
			{
				INFO_LOG("Remix: LEVELLIGHTS on but no per-game dir -- nothing loaded");
				return;
			}
			const std::string path = Path::Combine(dir, "lights_ALCATRAZ.txt");
			std::FILE* f = FileSystem::OpenCFile(path.c_str(), "r");
			if (!f)
			{
				INFO_LOG("Remix: LEVELLIGHTS on but '{}' not found -- no authored lights loaded", path);
				return;
			}

			const remixapi_Interface& api = s_remix.api();
			const float scale = level_light_scale();
			u32 made = 0, failed = 0, suns = 0, skipped_dark = 0;
			char line[512];
			while (std::fgets(line, sizeof(line), f))
			{
				if (line[0] == '#' || line[0] == 0x0A || line[0] == 0x0D)
					continue;

				float x, y, z, dx, dy, dz, bright, radius;
				u32 hue = 0, sat = 255;
				remixapi_LightInfo info{};
				info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
				info.isDynamic = 1; // MUST be set or the runtime sleeps analytical lights

				float lpos[3] = {0.f, 0.f, 0.f};
				float radiance_boost = 1.f;
				bool distant_light = false;
				remixapi_LightInfoSphereEXT sphere{};
				remixapi_LightInfoDistantEXT distant{};
				float rgb[3] = {1.f, 1.f, 1.f};

				if (std::sscanf(line, "SUN %f %f %f %f %f %f %f %u %u",
						&x, &y, &z, &dx, &dy, &dz, &bright, &hue, &sat) == 9)
				{
					ue2_hue_sat_to_rgb(hue, sat, rgb);
					distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
					distant.direction = {dx, dy, dz};
					distant.angularDiameterDegrees = 0.5f;
					info.pNext = &distant;
					{
						u64 lh = fnv_seed;
						for (const char* c = line; *c && *c != 10 && *c != 13; ++c)
							lh = fnv_mix(lh, static_cast<u32>(static_cast<unsigned char>(*c)));
						info.hash = 0x9C5241B210000000ull ^ (lh & 0x00FFFFFFFFFFFFFFull);
					}
					distant_light = true;
					++suns;
				}
				else if (std::sscanf(line, "LIGHT %f %f %f %f %f %u %u",
						&x, &y, &z, &bright, &radius, &hue, &sat) == 7)
				{
					ue2_hue_sat_to_rgb(hue, sat, rgb);
					sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
					lpos[0] = ax + (x - ax) * pscale;
					lpos[1] = ay + (y - ay) * pscale;
					lpos[2] = az + (z - az) * pscale;
					sphere.position = {lpos[0], lpos[1], lpos[2]};
					const float authored_radius = std::max(0.01f, radius * pscale);
					const float override_radius = level_light_radius();
					sphere.radius = (override_radius > 0.f) ? override_radius : authored_radius;
					// Emitted power is radiance x area, so shrinking the emitter without this would dim
					// the lamp by the square of the change.
					radiance_boost = (authored_radius * authored_radius) / (sphere.radius * sphere.radius);
					sphere.shaping_hasvalue = 0;
					info.pNext = &sphere;
					// Identity from the actor's own line in lights_<LEVEL>.txt, not its ordinal. A hash
					// of "how many lights came before this one" changes for every later lamp the moment
					// anything is skipped or the file is edited -- which would silently re-bind a
					// modder's placements in mod.usda to different lamps. The line is stable across
					// runs and independent of the skip rules and of POSSCALE.
					{
						u64 lh = fnv_seed;
						for (const char* c = line; *c && *c != 10 && *c != 13; ++c)
							lh = fnv_mix(lh, static_cast<u32>(static_cast<unsigned char>(*c)));
						info.hash = 0x9C5241B220000000ull ^ (lh & 0x00FFFFFFFFFFFFFFull);
					}
					// 37 of this map's 211 Light actors carry brightness 0 -- 29 of them radius 0 too,
					// meaning the .rsm property walk found neither field. They emit nothing and exist
					// only as clutter in the debug overlay.
					if (bright <= 0.f)
					{
						++skipped_dark;
						continue;
					}
				}
				else
					continue;

				const float r = (bright / 255.f) * scale * radiance_boost;
				info.radiance = {rgb[0] * r, rgb[1] * r, rgb[2] * r};

				remixapi_LightHandle h = nullptr;
				if (remix_ps2::guarded_create_light(api.CreateLight, &info, &h) ==
						REMIXAPI_ERROR_CODE_SUCCESS && h)
				{
					s_level_lights.push_back({h, {lpos[0], lpos[1], lpos[2]}, distant_light});
					++made;
				}
				else
					++failed;
			}
			std::fclose(f);

			INFO_LOG("Remix: LEVELLIGHTS loaded '{}' -- created {} ({} distant) failed {} skipped-dark {} scale {:.3f} posscale {:.5f} anchor {:.0f} {:.0f} {:.0f}",
				path, made, suns, failed, skipped_dark, scale, pscale, ax, ay, az);
		}
		// --- EE-resident camera ---------------------------------------------------------------
		// FOUND 2026-08-27 by diffing three save states (still / turn in place / walk+turn):
		//   VIEW  Location @ 0x00F0F670   Rotation @ 0x00F0F680  (FRotator, 3 x int32)
		//   PAWN  Location @ 0x019AE190   Rotation @ 0x019AE1A0
		// They differ by exactly 75 units of Z (BaseEyeHeight); the view carries a -12.1 degree
		// pitch and the pawn none -- the standard Unreal pawn/view split. Verified over all three
		// states: position frozen standing still, moved when walking, yaw tracking +99.0 then +85.4.
		// This is the transform VU1 never contains: VIFMAP (7.8M unpacks, 360 shapes, dropped=0)
		// proved no standalone camera is ever uploaded there.
		// 0 = off, 1 = read and LOG only, 2 = also submit as the world camera.
		int ee_cam_mode()
		{
			return std::clamp(env_int_live(L"PCSX2_REMIX_EECAM", 0), 0, 2);
		}
		// The EE gives Location + Rotation, i.e. the VIEW half only. The projection half is
		// synthesised: make_perspective() builds the same row-vector convention the clip solver
		// expects, and the solver reads only columns 0/1/3 so the depth column never matters.
		// Defaults are the values the old solve reported for this title (fovY 48.1, 10:7).
		// Read live, refreshed once per frame alongside the depth scale. These decide where a
		// WORLD-SPACE light lands relative to the geometry: geometry re-projects to the guest's own
		// clip coordinates by construction whatever projection we use, but a light is projected
		// directly, so an error here separates the two -- proportionally to distance from screen
		// centre and changing sign across it. Never validated against the guest's real projection.
		float s_ee_fov = -1.f;
		float s_ee_aspect = -1.f;
		float ee_cam_fov()
		{
			if (s_ee_fov < 0.f)
				s_ee_fov = env_float_signed(L"PCSX2_REMIX_EECAMFOV", 48.1f);
			return s_ee_fov;
		}
		float ee_cam_aspect()
		{
			if (s_ee_aspect < 0.f)
				s_ee_aspect = env_float_signed(L"PCSX2_REMIX_EECAMASPECT", 1.429f);
			return s_ee_aspect;
		}

		// Depth calibration. The solver reads fused columns {0,1,3}; column 3 is what maps view
		// depth to clip w, and the guest's Q is scaled by whatever projection IT used. A
		// synthetic make_perspective() puts +-1 there, which is only right by luck. Measured
		// 2026-08-27: with EECAM=2 the camera is valid every frame (fallback 0, held 0) but the
		// scene still un-projects to a 4-8 unit ball, so recovered depth is ~100x too small and
		// the eye sweeps past the geometry. This scales that column so depth can be calibrated
		// against the real scene extent instead of guessed.
		float ee_cam_w_scale() { static live_float v(L"PCSX2_REMIX_EECAMWSCALE", 1.f); return v.get(); }

		// Depth calibration, applied to the CLIP VECTOR rather than the matrix.
		//
		// Scaling a fused column (EECAMWSCALE) changes the conditioning of the 3x3 the solver
		// inverts and made the camera wobble; scaling clip uniformly leaves B untouched and is
		// numerically safe. DEPTHDIAG measured why it is needed: the guest supplies
		// clip w in [0.0008, 8.04] while the solver's bias terms are ~16,000, so (clip - bias)
		// is all bias and every vertex collapses onto the eye -- world bounds ran from the
		// origin to the camera on all three axes. A correct clip w IS view-space depth, which
		// for this corridor should be tens to hundreds, so the guest's w is short by ~50-100x.
		// Read live, and cached per frame rather than per vertex: this is the dial that decides
		// where the recovered world sits relative to the map's own light coordinates, so it has to
		// be tunable while the game runs. live_float cannot do that -- it re-parses only when
		// knob_generation() moves, and the per-game .conf never moves it (see env_float_signed).
		float s_ee_depth_scale = -1.f;
		float ee_cam_depth_scale()
		{
			if (s_ee_depth_scale < 0.f)
				s_ee_depth_scale = env_float_signed(L"PCSX2_REMIX_EECAMDEPTH", 1.f);
			return s_ee_depth_scale;
		}

		u32 ee_cam_loc_addr() { return static_cast<u32>(env_int_live(L"PCSX2_REMIX_EECAMLOC", 0x00F0F670)); }
		u32 ee_cam_rot_addr() { return static_cast<u32>(env_int_live(L"PCSX2_REMIX_EECAMROT", 0x00F0F680)); }

		// Location + FRotator -> row-vector world->view. Unreal is Z-up, X forward, 65536 == 360 deg.
		bool read_ee_camera(float (&pos)[3], remix_ps2::mat4& view, float (&angles_deg)[3])
		{
			if (!eeMem)
				return false;
			const u32 loc = ee_cam_loc_addr() & 0x01FFFFFCu;
			const u32 rot = ee_cam_rot_addr() & 0x01FFFFFCu;
			if ((loc + 12u) > Ps2MemSize::MainRam || (rot + 12u) > Ps2MemSize::MainRam)
				return false;
			float p[3];
			s32 r[3];
			std::memcpy(p, eeMem->Main + loc, sizeof(p));
			std::memcpy(r, eeMem->Main + rot, sizeof(r));
			for (float v : p)
			{
				if (!std::isfinite(v) || std::abs(v) > 1e7f)
					return false;
			}
			constexpr float k = 6.2831853f / 65536.f;
			const float pitch = static_cast<float>(r[0]) * k;
			const float yaw   = static_cast<float>(r[1]) * k;
			const float roll  = static_cast<float>(r[2]) * k;
			angles_deg[0] = static_cast<float>(r[0]) * (360.f / 65536.f);
			angles_deg[1] = static_cast<float>(r[1]) * (360.f / 65536.f);
			angles_deg[2] = static_cast<float>(r[2]) * (360.f / 65536.f);
			const float cp = std::cos(pitch), sp = std::sin(pitch);
			const float cy = std::cos(yaw),   sy = std::sin(yaw);
			const float cr = std::cos(roll),  sr = std::sin(roll);
			const float fwd[3]   = { cp * cy, cp * sy, sp };
			const float right[3] = { (sr * sp * cy) - (cr * sy), (sr * sp * sy) + (cr * cy), -sr * cp };
			const float up[3]    = { -((cr * sp * cy) + (sr * sy)), (cy * sr) - (cr * sp * sy), cr * cp };
			pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2];
			view = remix_ps2::mat4_identity();
			for (u32 i = 0; i < 3; ++i)
			{
				view.m[i][0] = right[i];
				view.m[i][1] = up[i];
				view.m[i][2] = fwd[i];
				view.m[i][3] = 0.f;
			}
			view.m[3][0] = -((p[0]*right[0]) + (p[1]*right[1]) + (p[2]*right[2]));
			view.m[3][1] = -((p[0]*up[0])    + (p[1]*up[1])    + (p[2]*up[2]));
			view.m[3][2] = -((p[0]*fwd[0])   + (p[1]*fwd[1])   + (p[2]*fwd[2]));
			view.m[3][3] = 1.f;
			return true;
		}

		// Assembles a complete world_camera from the EE view plus a synthetic projection,
		// including the clip solver the geometry un-projection runs on. Without the solver
		// the renderer would draw from the right eye while geometry stayed un-projected
		// against the old broken one, which looks worse than leaving it alone.
		bool build_ee_world_camera(world_camera& out)
		{
			float pos[3], ang[3];
			remix_ps2::mat4 view{};
			if (!read_ee_camera(pos, view, ang))
				return false;

			const float near_plane = 1.f;
			const float far_plane = 100000.f;
			remix_ps2::mat4 proj =
				remix_ps2::make_perspective(ee_cam_fov(), ee_cam_aspect(), near_plane, far_plane);

			// Scale the depth->w column. Anything non-unity here means the guest's projection
			// disagreed with the synthetic one about how far a unit of view depth is.
			const float wscale = ee_cam_w_scale();
			if (wscale != 1.f && wscale > 0.f)
			{
				for (u32 i = 0; i < 4; ++i)
					proj.m[i][3] *= wscale;
			}

			const remix_ps2::mat4 fused = remix_ps2::mat4_multiply(view, proj);

			remix_ps2::clip_solver solver{};
			if (!remix_ps2::make_clip_solver(fused, solver))
				return false;

			out = world_camera{};
			out.valid = true;
			out.view = view;
			out.projection = proj;
			out.solver = solver;
			out.position[0] = pos[0];
			out.position[1] = pos[1];
			out.position[2] = pos[2];
			out.near_plane = near_plane;
			out.far_plane = far_plane;
			// hash_floats() is declared further down this file; fnv_mix is already in scope.
			u64 h = fnv_seed;
			for (u32 i = 0; i < 16; ++i)
			{
				u32 bits;
				std::memcpy(&bits, &fused.m[i / 4][i % 4], sizeof(bits));
				h = fnv_mix(h, bits);
			}
			out.matrix_hash = h;
			out.score = 1.f;
			return true;
		}
		void place_debug_light(const float (&position)[3], float scene_radius)
		{
			if (no_debug_scene())
				return;

			const remixapi_Interface& api = s_remix.api();

			const float radius_override = debug_light_radius_override();
			const float radiance_override = debug_light_radiance_override();

			// Clamped against the frustum so a single outlier draw cannot turn one frame's
			// measured extent into a light the size of the level.
			const float bounded_radius = std::clamp(scene_radius, world_near_plane(), world_far_plane(world_near_plane()));

			const float radius = (radius_override > 0.f) ? radius_override :
			                                               std::max(s_light_radius_fraction * bounded_radius, 1e-4f);
			const float radiance =
				(radiance_override > 0.f) ?
					radiance_override :
					(debug_light_exposure() / (3.14159265f * s_light_radius_fraction * s_light_radius_fraction));

			// Recreating the light every frame is a Remix state change for nothing; only do it
			// when something actually moved or changed size. 2% hysteresis on the radius keeps
			// a frame-to-frame wobble in the measured extent from thrashing it.
			if (s_light_placed &&
				s_light_position[0] == position[0] && s_light_position[1] == position[1] &&
				s_light_position[2] == position[2] && s_light_radiance == radiance &&
				std::abs(radius - s_light_radius) <= (0.02f * s_light_radius))
			{
				return;
			}

			s_light_position[0] = position[0];
			s_light_position[1] = position[1];
			s_light_position[2] = position[2];
			s_light_radius = radius;
			s_light_radiance = radiance;
			s_light_placed = true;

			if (s_debug_light)
			{
				remix_ps2::guarded_destroy_light(api.DestroyLight, s_debug_light);
				s_debug_light = nullptr;
			}

			remixapi_LightInfoSphereEXT sphere_light{};
			sphere_light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
			sphere_light.pNext = nullptr;
			sphere_light.position = {position[0], position[1], position[2]};
			sphere_light.radius = radius;
			sphere_light.shaping_hasvalue = 0;
			sphere_light.volumetricRadianceScale = debug_light_volumetric();

			remixapi_LightInfo light_info{};
			light_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			light_info.pNext = &sphere_light;
			light_info.hash = 0x9C5241B200000003ull; // sphere (debug/LIGHTMODE=2)

			// MUST be set. remixapi_CreateLight does `rtLight->isDynamic = info->isDynamic`,
			// and a light left static is put to sleep after getNumFramesToPutLightsToSleep()
			// frames -- it leaves the active list and LIGHT STATISTICS drops to 0 with no error
			// anywhere. The fork documents this on its own sun (rtx_fork_atmosphere.cpp:
			// "Without this the light is treated as static and put to sleep"). Dome lights are
			// unaffected because they never enter the RtLight sleep path, which is exactly why
			// the dome lit the scene while the key and the sphere silently vanished.
			light_info.isDynamic = 1;
			// Neutral white; the runtime gives radiance no per-channel semantics.
			light_info.radiance = {radiance, radiance, radiance};

			const u32 status = remix_ps2::guarded_create_light(api.CreateLight, &light_info, &s_debug_light);
			if (status != REMIXAPI_ERROR_CODE_SUCCESS || !s_debug_light)
			{
				ERROR_LOG("Remix: CreateLight failed ({})", remix_ps2::error_name(status));
				s_debug_light = nullptr;
			}
		}

		// A distant light along 'direction' -- no distance falloff, so the far end of a big
		// scene is lit exactly as brightly as the near end. Opt-in via PCSX2_REMIX_SUN.
		void place_sun_light(const float (&direction)[3])
		{
			const float radiance = sun_radiance();
			if (radiance <= 0.f || no_debug_scene())
				return;

			const remixapi_Interface& api = s_remix.api();

			if (s_sun_placed && s_sun_direction[0] == direction[0] && s_sun_direction[1] == direction[1] &&
				s_sun_direction[2] == direction[2])
			{
				return;
			}

			const float len = std::sqrt((direction[0] * direction[0]) + (direction[1] * direction[1]) +
										(direction[2] * direction[2]));
			if (!std::isfinite(len) || len < 1e-6f)
				return;

			s_sun_direction[0] = direction[0];
			s_sun_direction[1] = direction[1];
			s_sun_direction[2] = direction[2];
			s_sun_placed = true;

			if (s_sun_light)
			{
				remix_ps2::guarded_destroy_light(api.DestroyLight, s_sun_light);
				s_sun_light = nullptr;
			}

			remixapi_LightInfoDistantEXT distant{};
			distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
			distant.pNext = nullptr;
			distant.direction = {direction[0] / len, direction[1] / len, direction[2] / len};
			distant.angularDiameterDegrees = sun_angle_degrees();
			distant.volumetricRadianceScale = 1.f;

			remixapi_LightInfo light_info{};
			light_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			light_info.pNext = &distant;
			light_info.hash = 0x9C5241B200000004ull; // distant (place_sun_light)

			// MUST be set. remixapi_CreateLight does `rtLight->isDynamic = info->isDynamic`,
			// and a light left static is put to sleep after getNumFramesToPutLightsToSleep()
			// frames -- it leaves the active list and LIGHT STATISTICS drops to 0 with no error
			// anywhere. The fork documents this on its own sun (rtx_fork_atmosphere.cpp:
			// "Without this the light is treated as static and put to sleep"). Dome lights are
			// unaffected because they never enter the RtLight sleep path, which is exactly why
			// the dome lit the scene while the key and the sphere silently vanished.
			light_info.isDynamic = 1;
			light_info.radiance = {radiance, radiance, radiance};

			const u32 status = remix_ps2::guarded_create_light(api.CreateLight, &light_info, &s_sun_light);
			if (status != REMIXAPI_ERROR_CODE_SUCCESS || !s_sun_light)
			{
				ERROR_LOG("Remix: CreateLight failed for the distant light ({})", remix_ps2::error_name(status));
				s_sun_light = nullptr;
			}
		}

		// Everything that decides what the two fill lights ARE. Anything in here changing means the
		// lights have to be destroyed and rebuilt, because the Remix API has no "modify a light":
		// remixapi_LightInfo is consumed by CreateLight and the handle is immutable thereafter.
		struct fill_light_params
		{
			bool key_wanted = false;
			bool dome_wanted = false;
			bool winterblade = false;
			float key_radiance[3] = {0.f, 0.f, 0.f};
			float key_angle = 0.f;
			float key_direction[3] = {0.f, 0.f, 0.f};
			float key_elevation = 0.f;
			float key_azimuth = 0.f;
			float dome_radiance[3] = {0.f, 0.f, 0.f};

			bool operator==(const fill_light_params& o) const
			{
				return key_wanted == o.key_wanted && dome_wanted == o.dome_wanted &&
					   winterblade == o.winterblade && key_radiance[0] == o.key_radiance[0] &&
					   key_radiance[1] == o.key_radiance[1] && key_radiance[2] == o.key_radiance[2] &&
					   key_angle == o.key_angle && key_direction[0] == o.key_direction[0] &&
					   key_direction[1] == o.key_direction[1] && key_direction[2] == o.key_direction[2] &&
					   key_elevation == o.key_elevation && key_azimuth == o.key_azimuth &&
					   dome_radiance[0] == o.dome_radiance[0] && dome_radiance[1] == o.dome_radiance[1] &&
					   dome_radiance[2] == o.dome_radiance[2];
			}

			bool operator!=(const fill_light_params& o) const { return !(*this == o); }
		};

		// What the lights that currently EXIST were built from. `resolved` is separate from the
		// values because "no lights, because LIGHTMODE=0" is a legitimate resolved state that
		// compares equal to the default-constructed struct, and the first pass must not mistake
		// itself for a no-op.
		fill_light_params s_fill_params{};
		bool s_fill_params_resolved = false;

		fill_light_params resolve_fill_light_params()
		{
			fill_light_params want{};

			// Everything off. Returned as a normal parameter set rather than as an early exit from
			// the caller, so that turning LIGHTMODE to 0 (or 2) while the game runs actually
			// destroys the fill instead of leaving it burning -- which is what it did before, and
			// is exactly how a bisect arm that "ran with the key light off" ran at full strength.
			if (no_debug_scene() || light_mode() != 1)
				return want;

			want.winterblade = socom_winterblade_lighting();

			const float ambient = ambient_radiance();
			want.dome_wanted = (ambient > 0.f) || want.winterblade;
			if (want.winterblade)
			{
				want.dome_radiance[0] = 0.0980392f;
				want.dome_radiance[1] = 0.0980392f;
				want.dome_radiance[2] = 0.1176471f;
			}
			else
			{
				want.dome_radiance[0] = want.dome_radiance[1] = want.dome_radiance[2] = ambient;
			}

			const float key = key_radiance();
			want.key_wanted = key > 0.f;
			want.key_angle = key_angle_degrees();
			want.key_elevation = key_elevation_degrees();
			want.key_azimuth = key_azimuth_degrees();

			// At the defaults, the shipped literal is used rather than reconstructed from the
			// angles, so "no knobs set" is bit-exact and not merely indistinguishable. Off the
			// defaults the knobs win over the Winterblade authored direction too: asking for a
			// specific sun and being given someone else's is the worse surprise.
			float direction[3];
			if (want.key_elevation == s_key_elevation_default && want.key_azimuth == s_key_azimuth_default)
			{
				const float* shipped =
					want.winterblade ? s_key_winterblade_direction : s_key_shipped_direction;
				direction[0] = shipped[0];
				direction[1] = shipped[1];
				direction[2] = shipped[2];
			}
			else
			{
				key_direction_from_angles(want.key_elevation, want.key_azimuth, direction);
			}

			// Normalised with the same expression the shipped code used, so the default path
			// reproduces the same floats rather than merely the same angle. The angle path is
			// already unit length to float precision; dividing it by ~1.0 costs nothing and keeps
			// one code path.
			const float direction_length = std::sqrt((direction[0] * direction[0]) +
				(direction[1] * direction[1]) + (direction[2] * direction[2]));

			if (!std::isfinite(direction_length) || direction_length < 1e-6f)
			{
				// Unreachable from either source above -- both produce unit-ish vectors -- but a
				// zero-length direction handed to the runtime is not a failure mode worth finding
				// out about on screen.
				want.key_direction[0] = s_key_shipped_direction[0];
				want.key_direction[1] = s_key_shipped_direction[1];
				want.key_direction[2] = s_key_shipped_direction[2];
			}
			else
			{
				want.key_direction[0] = direction[0] / direction_length;
				want.key_direction[1] = direction[1] / direction_length;
				want.key_direction[2] = direction[2] / direction_length;
			}

			if (want.winterblade)
			{
				want.key_radiance[0] = key * 0.5647059f;
				want.key_radiance[1] = key * 0.5529412f;
				want.key_radiance[2] = key * 0.3960784f;
			}
			else
			{
				want.key_radiance[0] = want.key_radiance[1] = want.key_radiance[2] = key;
			}

			return want;
		}

		// The distance-independent fill: never moved, never rescaled -- a dome and a distant light
		// have no position and no falloff, so nothing about them depends on the scene's size or on
		// where the camera is -- but REBUILT whenever a knob behind them moves.
		//
		// THE REBUILD IS THE POINT, and this is what it fixes. This function used to create each
		// light only `if (!s_dome_light)` / `if (!s_sun_light)`, i.e. exactly once, from
		// create_debug_scene(). The run log puts that call at t=9.774 s ("renderer is live") and the
		// per-game <SERIAL>.conf at t=10.008 s (refresh_game_config, which runs at the END of
		// OnVSync). So every KEY, KEYANGLE, AMBIENT, LIGHTMODE -- and now KEYELEV/KEYAZIM -- value a
		// conf ever delivered arrived 234 ms after the only light that would ever exist had already
		// been built from the defaults. The knobs were not weak, they were UNREACHABLE. That is what
		// invalidated the light bisect: an arm believed to be running with the key light off was
		// running at 100.
		//
		// WHERE IT RUNS, and why that point is safe: the top of OnVSync, before decide_hold_window()
		// and therefore before submit_camera(), batch_flush(), DrawLightInstance and Present. The
		// only place either handle is ever submitted is the DrawLightInstance pair ~40 lines further
		// down that same function, immediately before Present -- so at the moment this runs, the
		// previous window's use of the handle has already been consumed by that Present (the two are
		// in the same `if (!skip_present)` block and cannot separate), and this window has not
		// referenced it yet. Nothing else in the frame touches a light handle: draws reference
		// meshes and materials only. The handle created here is therefore the one this very window
		// draws, so a knob takes effect in the frame that noticed it rather than the one after.
		// The precedent is already in the file: place_debug_light() destroys and recreates its
		// sphere from submit_camera(), which is LATER in the same window than this point, and does
		// it on any frame the camera moved.
		//
		// GS thread only, like every other light call and every knob read in this file.
		void refresh_fill_lights()
		{
			const remixapi_Interface& api = s_remix.api();
			if (light_mode() != 2 && s_debug_light)
			{
				remix_ps2::guarded_destroy_light(api.DestroyLight, s_debug_light);
				s_debug_light = nullptr;
			}

			const fill_light_params want = resolve_fill_light_params();
			if (s_fill_params_resolved && want == s_fill_params)
				return;

			// Both are torn down whichever one moved. They are two lights, not two independent
			// features, and a partial rebuild is how you end up with a dome from one conf and a key
			// from another.
			if (s_dome_light)
			{
				remix_ps2::guarded_destroy_light(api.DestroyLight, s_dome_light);
				s_dome_light = nullptr;
			}

			if (s_sun_light)
			{
				remix_ps2::guarded_destroy_light(api.DestroyLight, s_sun_light);
				s_sun_light = nullptr;
			}

			s_fill_params = want;
			s_fill_params_resolved = true;

			if (want.dome_wanted)
			{
				remixapi_LightInfoDomeEXT dome{};
				dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
				dome.pNext = nullptr;
				dome.transform = s_identity_transform;
				dome.colorTexture = nullptr; // flat radiance rather than an environment map

				remixapi_LightInfo light_info{};
				light_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
				light_info.pNext = &dome;
				light_info.hash = 0x9C5241B200000005ull; // dome (fill)

				// MUST be set. remixapi_CreateLight does `rtLight->isDynamic = info->isDynamic`,
				// and a light left static is put to sleep after getNumFramesToPutLightsToSleep()
				// frames -- it leaves the active list and LIGHT STATISTICS drops to 0 with no error
				// anywhere. The fork documents this on its own sun (rtx_fork_atmosphere.cpp:
				// "Without this the light is treated as static and put to sleep"). Dome lights are
				// unaffected because they never enter the RtLight sleep path, which is exactly why
				// the dome lit the scene while the key and the sphere silently vanished.
				light_info.isDynamic = 1;
				light_info.radiance = {want.dome_radiance[0], want.dome_radiance[1], want.dome_radiance[2]};

				const u32 status = remix_ps2::guarded_create_light(api.CreateLight, &light_info, &s_dome_light);
				if (status != REMIXAPI_ERROR_CODE_SUCCESS || !s_dome_light)
				{
					ERROR_LOG("Remix: CreateLight failed for the dome light ({})", remix_ps2::error_name(status));
					s_dome_light = nullptr;
				}
			}

			if (want.key_wanted)
			{
				remixapi_LightInfoDistantEXT distant{};
				distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
				distant.pNext = nullptr;
				distant.direction = {want.key_direction[0], want.key_direction[1], want.key_direction[2]};
				distant.angularDiameterDegrees = want.key_angle;
				distant.volumetricRadianceScale = 1.f;

				remixapi_LightInfo light_info{};
				light_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
				light_info.pNext = &distant;
				light_info.hash = 0x9C5241B200000006ull; // distant key (fill)

				// MUST be set. remixapi_CreateLight does `rtLight->isDynamic = info->isDynamic`,
				// and a light left static is put to sleep after getNumFramesToPutLightsToSleep()
				// frames -- it leaves the active list and LIGHT STATISTICS drops to 0 with no error
				// anywhere. The fork documents this on its own sun (rtx_fork_atmosphere.cpp:
				// "Without this the light is treated as static and put to sleep"). Dome lights are
				// unaffected because they never enter the RtLight sleep path, which is exactly why
				// the dome lit the scene while the key and the sphere silently vanished.
				light_info.isDynamic = 1;
				light_info.radiance = {want.key_radiance[0], want.key_radiance[1], want.key_radiance[2]};

				const u32 status = remix_ps2::guarded_create_light(api.CreateLight, &light_info, &s_sun_light);
				if (status != REMIXAPI_ERROR_CODE_SUCCESS || !s_sun_light)
				{
					ERROR_LOG("Remix: CreateLight failed for the key light ({})", remix_ps2::error_name(status));
					s_sun_light = nullptr;
				}
				else if (want.winterblade)
				{
					INFO_LOG("Remix: SOCOM Winterblade authored key light submitted.");
				}
			}

			// Tracks whether the fill OWNS the handle, so a LIGHTMODE 1 -> 2 change hands
			// place_sun_light() a clean slate instead of leaving it convinced a light it did not
			// build is already placed.
			s_sun_placed = (s_sun_light != nullptr);

			// LOGGED ON EVERY CHANGE, never once at startup. A one-shot line here prints the
			// pre-conf defaults and then lies for the rest of the session -- the same trap that cost
			// a FRAMETRACE run today -- and this line is the user's only proof a conf value took.
			// hold_empty_mode() logs on change for the identical reason.
			INFO_LOG("Remix: scene lighting -- distance-independent fill (key {:g} at {:g} deg, "
					 "elev {:g} deg, azim {:g} deg, dir {:.4f} {:.4f} {:.4f}, dome {:g}){}. "
					 "No 1/d^2 falloff, so it is scale-free: the same numbers work at scene radius 4 "
					 "and 11,785. PCSX2_REMIX_KEY / KEYANGLE / AMBIENT tune it, KEYELEV / KEYAZIM "
					 "aim it (elevation above the horizon; azimuth from +Z toward +X), LIGHTMODE=2 "
					 "restores the camera-attached sphere.",
				want.key_wanted ? want.key_radiance[0] : 0.f, want.key_angle, want.key_elevation,
				want.key_azimuth, want.key_direction[0], want.key_direction[1], want.key_direction[2],
				want.dome_wanted ? want.dome_radiance[0] : 0.f,
				(want.key_wanted || want.dome_wanted) ? "" : " -- no fill lights exist");
		}

		bool create_debug_scene()
		{
			const remixapi_Interface& api = s_remix.api();

			// Scene lighting. The default is the fill above; mode 2 is the old camera-attached
			// sphere light, straight from the official remixapi_example_c.c, whose scene radius
			// is a placeholder until the first frame measures one.
			//
			// This first pass necessarily runs on the pre-conf defaults -- it is t=9.774 s and the
			// conf lands at t=10.008 s -- which is precisely why OnVSync calls the same function
			// every window and rebuilds when the resolved parameters move.
			refresh_fill_lights();

			if (light_mode() == 2)
			{
				const float origin_light[3] = {0.f, -1.f, 0.f};
				place_debug_light(origin_light, 2.f);
			}

			// A missing light is only a failure when a light was asked for. LIGHTMODE=0 asks for
			// exactly none, and an unlit scene is a legitimate scene -- the path tracer still
			// traces primary and indirect rays through it.
			//
			// This is the second time this check has been wrong in the same way. It first tested
			// s_debug_light specifically, which broke when the fill replaced the sphere; widening
			// it to "any light" left LIGHTMODE=0 failing, which degraded the whole renderer to a
			// no-op. That mattered far more than it looks: a no-op renderer submits nothing, and
			// an arm that submits nothing survives 20/20 for reasons that have nothing to do with
			// what it was testing. See the "arms that survive by not rendering" note in notes.md.
			//
			// THIRD correction, and it is the same shape as the first two: LIGHTMODE=0 is no longer
			// the only way to ask for no lights. `PCSX2_REMIX_KEY = 0` now genuinely means off --
			// it used to be swallowed by env_float()'s "<= 0 means unset" rule and silently re-read
			// as 100 -- so mode 1 with KEY=0 and AMBIENT=0 is a deliberate, reachable request for
			// an empty fill, and answering it by degrading the whole renderer to a no-op would
			// reproduce exactly the failure this comment already warns about. The resolved fill is
			// therefore what decides whether a light was asked for; refresh_fill_lights() above has
			// just run, so s_fill_params describes this frame.
			const bool fill_wanted_a_light = s_fill_params.key_wanted || s_fill_params.dome_wanted;
			const bool lights_requested = !no_debug_scene() && light_mode() != 0 &&
										  (light_mode() != 1 || fill_wanted_a_light);
			if (lights_requested && !s_debug_light && !s_dome_light && !s_sun_light)
				return false;

			// Debug triangle at z = 10, in front of the hardcoded camera at the origin. This is
			// the beacon that says "the runtime is alive" when no PS2 geometry passed the gates.
			const auto make_vertex = [](float x, float y, float z) {
				remixapi_HardcodedVertex v{};
				v.position[0] = x;
				v.position[1] = y;
				v.position[2] = z;
				v.normal[0] = 0.f;
				v.normal[1] = 0.f;
				v.normal[2] = -1.f;
				v.texcoord[0] = 0.f;
				v.texcoord[1] = 0.f;
				v.color = 0xFFFFFFFF;
				return v;
			};

			const remixapi_HardcodedVertex verts[3] =
				{
					make_vertex(5.f, -5.f, 10.f),
					make_vertex(0.f, 5.f, 10.f),
					make_vertex(-5.f, -5.f, 10.f),
				};

			remixapi_MeshInfoSurfaceTriangles triangles{};
			triangles.vertices_values = verts;
			triangles.vertices_count = 3;
			triangles.indices_values = nullptr;
			triangles.indices_count = 0;
			triangles.skinning_hasvalue = 0;
			triangles.material = nullptr;

			remixapi_MeshInfo mesh_info{};
			mesh_info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			mesh_info.pNext = nullptr;
			mesh_info.hash = 0x1;
			mesh_info.surfaces_values = &triangles;
			mesh_info.surfaces_count = 1;

			const u32 status = remix_ps2::guarded_create_mesh(api.CreateMesh, &mesh_info, &s_debug_mesh);
			if (status != REMIXAPI_ERROR_CODE_SUCCESS || !s_debug_mesh)
			{
				ERROR_LOG("Remix: CreateMesh failed for the debug triangle ({})", remix_ps2::error_name(status));
				return false;
			}

			return true;
		}

		// The parameterized-camera fallback: a fixed camera at the origin looking down +Z, in
		// Remix's left-handed +Y-up convention. Every position the view-space tier submits is
		// expressed in exactly this camera's space, so the two agree by construction.
		// Submit a camera and actually look at what the runtime said.
		//
		// Every call site used to discard this return code, so a camera Remix refused was
		// indistinguishable from one it accepted. Remix keeps the *previous* camera of that type
		// when a call fails, so the frame renders from wherever the eye last was -- the scene
		// appears to snap back and forth between two viewpoints. Name the first failure of each
		// camera type once; the counter carries the rate.
		void setup_camera_checked(const remixapi_CameraInfo* info, const char* what, bool& logged)
		{
			const u32 code = remix_ps2::guarded_setup_camera(s_remix.api().SetupCamera, info);
			if (code == REMIXAPI_ERROR_CODE_SUCCESS)
				return;

			++s_stats.cam_failed;

			if (!logged)
			{
				logged = true;
				ERROR_LOG("Remix: SetupCamera({}) refused with {} -- the runtime keeps its previous "
						  "camera of this type, so the scene renders from a stale eye. This is what "
						  "geometry 'teleporting' between two viewpoints looks like.",
					what, remix_ps2::error_name(code));
			}
		}

		bool s_logged_camera_fail_world = false;
		bool s_logged_camera_fail_fallback = false;
		bool s_logged_camera_fail_sky = false;
		bool s_logged_camera_fail_viewmodel = false;

		void submit_fallback_camera()
		{
			const remixapi_Interface& api = s_remix.api();

			remixapi_CameraInfoParameterizedEXT camera_ext{};
			camera_ext.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
			camera_ext.pNext = nullptr;
			camera_ext.position = {0.f, 0.f, 0.f};
			camera_ext.forward = {0.f, 0.f, 1.f};
			camera_ext.up = {0.f, 1.f, 0.f};
			camera_ext.right = {1.f, 0.f, 0.f};
			camera_ext.fovYInDegrees = s_debug_fov_y_degrees;
			camera_ext.aspect = window_aspect();
			camera_ext.nearPlane = 0.1f;
			camera_ext.farPlane = remix_ps2::hardcoded_far_plane();

			remixapi_CameraInfo camera_info{};
			camera_info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
			camera_info.pNext = &camera_ext;

			setup_camera_checked(&camera_info, "fallback", s_logged_camera_fail_fallback);
		}

		// --- the world-anchored camera ----------------------------------------------------

		float env_float(const wchar_t* name, float fallback)
		{
			const std::wstring env = remix_ps2::read_env(name);
			if (env.empty())
				return fallback;

			const float parsed = static_cast<float>(::_wtof(env.c_str()));
			return (std::isfinite(parsed) && parsed > 0.f) ? parsed : fallback;
		}

		// Near/far for a recovered world camera, in the guest's own world units. These are
		// constants rather than measurements -- see the note at the accept site for why the
		// matrix cannot supply them for this title.
		float world_near_plane()
		{
			static const float value = env_float(L"PCSX2_REMIX_NEARPLANE", 1.f);
			return value;
		}

		// Unless PCSX2_REMIX_FARPLANE is set explicitly this is a wide default: a PS2 title's
		// world unit could be a metre or a centimetre, and the 1000 the view-space tier uses
		// would clip a level whose coordinates run into the thousands, which R6 3's do.
		float world_far_plane(float near_plane)
		{
			static const bool explicit_far = !remix_ps2::read_env(L"PCSX2_REMIX_FARPLANE").empty();
			if (explicit_far)
				return remix_ps2::hardcoded_far_plane();

			return std::max(near_plane * 4096.f, 100000.f);
		}

		// The ceiling on a submitted world position. This is a ray-tracing constraint, not a
		// correctness one: a triangle thousands of units across sitting beside one a few units
		// across collapses float precision in the BVH, and the GPU can take long enough on the
		// resulting acceleration structure to trip the driver watchdog -- surfacing as
		// VK_ERROR_DEVICE_LOST and dxvk-remix calling exit(). Derived from the far plane by
		// default, but overridable so the usable range can be bisected without a rebuild.
		float max_position_magnitude()
		{
			static const float value = []() -> float {
				const float derived =
					std::max(world_far_plane(world_near_plane()), remix_ps2::hardcoded_far_plane()) * 4.f;
				return env_float(L"PCSX2_REMIX_POSLIMIT", derived);
			}();

			return value;
		}

		// Largest |position| actually submitted this frame, so the ceiling can be set from a
		// measurement rather than a guess.
		float s_max_seen_position = 0.f;

		u64 hash_floats(const float* values, u32 count)
		{
			u64 hash = fnv_seed;

			for (u32 i = 0; i < count; ++i)
			{
				u32 bits;
				std::memcpy(&bits, &values[i], sizeof(bits));
				hash = (hash ^ bits) * fnv_prime;
			}

			return hash;
		}

		// PCSX2_REMIX_DUMP=1: one line per unique scanned matrix, in its own file because the
		// emulator log is noisy and gets rotated. This file is the deliverable when the world
		// tier does not resolve -- it is what distinguishes "the scan found nothing" from
		// "found matrices that would not split" from "split but the perspective filter
		// rejected every one".
		void dump_write(const std::string& line)
		{
			static constexpr u32 max_lines = 20000;
			static u32 written = 0;
			static bool tried = false;
			static std::FILE* file = nullptr;

			if (!tried)
			{
				tried = true;

				const std::string& dir = EmuFolders::Logs.empty() ? EmuFolders::AppRoot : EmuFolders::Logs;
				const std::string path = Path::Combine(dir, "remix_matrices.txt");
				file = FileSystem::OpenCFile(path.c_str(), "w");

				if (file)
					INFO_LOG("Remix: writing scanned-matrix diagnostics to '{}'", path);
				else
					ERROR_LOG("Remix: could not open '{}' for the matrix dump", path);
			}

			if (!file || written >= max_lines)
				return;

			++written;
			std::fputs(line.c_str(), file);
			std::fputc('\n', file);
			std::fflush(file);
		}

		// Whether to submit a second camera as REMIXAPI_CAMERA_TYPE_SKY.
		//
		// Remix keeps one camera per type and renders instances tagged
		// REMIXAPI_INSTANCE_CATEGORY_BIT_SKY with the sky one. We have been tagging sky instances
		// since the classifier landed but never submitting the camera to go with them, so the
		// developer menu's SKY entry reads empty and sky geometry is rendered with the world
		// camera -- which puts the backdrop at whatever distance the un-projection happened to
		// give it, a few units in front of the player rather than at infinity.
		bool sky_camera_enabled()
		{
			static const bool value = remix_ps2::read_env_int(L"PCSX2_REMIX_SKYCAM", 1) != 0;
			return value;
		}

		// Distance from the eye at which a sky draw is planted, in world units. 0 disables the push
		// and leaves the old behaviour. The SKY instance category is delete-on-sight on the remixapi
		// path, so pushing is the only way to keep a backdrop at all: the draw is placed on a sphere
		// around the camera and submitted as ordinary geometry.
		float sky_distance()
		{
			static live_float value(L"PCSX2_REMIX_SKYDIST", 0.f);
			return std::max(0.f, value.get());
		}

		bool s_logged_sky_camera = false;

		// Whether to submit a REMIXAPI_CAMERA_TYPE_VIEW_MODEL camera. Harmless on its own: the
		// runtime's viewmodel path early-outs on rtx.viewModel.enable (default False) and then on
		// an empty candidate list, and the portal-only paths a valid viewmodel camera unlocks are
		// inert without two active ray portals.
		bool view_model_camera_enabled()
		{
			static const bool value = remix_ps2::read_env_int(L"PCSX2_REMIX_VMCAM", 1) != 0;
			return value;
		}

		bool s_logged_view_model_camera = false;

		// `cam` is the frame-latched s_active_camera on every ordinary window. On a HELD window under
		// PCSX2_REMIX_HOLDEMPTY = 2 it is instead the PREVIOUS window's camera, so that the repeated
		// geometry is repeated from the eye it was built for -- a true duplicate frame rather than a
		// new viewpoint onto stale geometry. Passed rather than read from the global so there is
		// exactly one SetupCamera per present and no ordering question against DrawInstance; see
		// hold_empty_mode() for the measurement that made this necessary.
		// Taken BY VALUE so an unresolved camera can be replaced with the last resolved one
		// before the body runs; every use of `cam` below then refers to the substitute.
		static world_camera s_last_good_camera{};

		void submit_camera(world_camera cam)
		{
			// A frame the solver could not resolve is a GAP, not a new viewpoint. The origin
			// fallback (0,0,0 down +Z, FOV 70, left-handed) is a completely different viewpoint
			// from the real one, and it also skips the sky and view-model cameras entirely, so
			// alternating between them flashes the whole screen. Measured at 26% of frames on
			// R6 3 (cam world 7338 / fallback 2562), concentrated where the shadow
			// render-target passes perturb the camera solve -- which is exactly where the user
			// reported violent flicker. Re-submitting the last camera we resolved is at worst
			// one frame stale, which is imperceptible next to a full-screen viewpoint change.
			// The origin fallback is kept for the genuine cold start, before any camera exists.
			// BOUNDED (2026-08-26). Holding without a bound was a mistake: this title refuses 97%
			// of its camera solves (solves 849 / refused 827, singular 1,062,494), so an unbounded
			// hold latched ONE stale camera and re-submitted it for essentially every frame
			// (held 8439 of world 15187 and climbing 1:1). The world then moves while the eye does
			// not, which reads as the screen going black for longer the further you play, and it
			// survived a mission restart because the static never cleared.
			//
			// A hold is only honest across a SHORT gap. Past that the solve is not gapping, it is
			// broken, and the origin fallback -- ugly but self-correcting every frame -- is the
			// truthful thing to show. Clearing the cache as well stops a stale eye reappearing
			// later as though it were fresh.
			static u32 s_consecutive_holds = 0;
			constexpr u32 k_max_consecutive_holds = 4;

			if (cam.valid)
			{
				s_last_good_camera = cam;
				s_consecutive_holds = 0;
			}
			else if (s_last_good_camera.valid && s_consecutive_holds < k_max_consecutive_holds)
			{
				cam = s_last_good_camera;
				++s_consecutive_holds;
				++s_stats.cam_held_gap;
			}
			else if (s_consecutive_holds >= k_max_consecutive_holds)
			{
				s_last_good_camera = world_camera{};
				++s_stats.cam_hold_expired;
			}
			// CAMTRACK. Decides whether the light "following the camera" is a light bug or a SPACE
			// bug, and those need opposite fixes. If the submitted camera's forward vector CHANGES
			// as the player turns, the camera is real and our positions are world-anchored -- so a
			// world-fixed distant light cannot track the view and the fault is elsewhere. If
			// forward is CONSTANT while the player turns, the view transform is baked into the
			// geometry (which is exactly why the camera search was falsified), our "world" is
			// eye-relative, and EVERY world-space light will appear welded to the camera by
			// construction. No amount of light tuning fixes the second case.
			{
				static u32 s_camtrack_n = 0;
				if ((s_camtrack_n++ % 120) == 0)
				{
					// THE WORLD-ANCHOR TEST. `centre` is the midpoint of everything submitted this
					// window, in the space we hand Remix. Stand still and TURN:
					//   centre CONSTANT  -> geometry is world-anchored. A fixed-direction distant
					//                       light then physically cannot change shading as you
					//                       turn, and the fault is downstream of us.
					//   centre ORBITS the camera -> the un-projection carries the camera's
					//                       translation but NOT its rotation. The world spins with
					//                       the view, so EVERY world-space light sweeps across the
					//                       scene as you turn. That is a space bug, not a light bug,
					//                       and it also explains why MOONFIT returned azimuth 137
					//                       in one session and 59 in another.
					float cx = 0.f, cy = 0.f, cz = 0.f;
					if (s_last_bounds.valid)
					{
						cx = (s_last_bounds.min[0] + s_last_bounds.max[0]) * 0.5f;
						cy = (s_last_bounds.min[1] + s_last_bounds.max[1]) * 0.5f;
						cz = (s_last_bounds.min[2] + s_last_bounds.max[2]) * 0.5f;
					}

					INFO_LOG("Remix: CAMTRACK fwd {:+.4f} {:+.4f} {:+.4f} | right {:+.4f} {:+.4f} {:+.4f} "
							 "| pos {:.1f} {:.1f} {:.1f} | scene centre {:.1f} {:.1f} {:.1f}"
							 " | MESHTRACK id {:08X} {} verts seen {} centroid {:.1f} {:.1f} {:.1f} dist {:.0f} n {:+.3f} {:+.3f} {:+.3f} facing {:+.3f}",
						cam.view.m[0][2], cam.view.m[1][2], cam.view.m[2][2],
						cam.view.m[0][0], cam.view.m[1][0], cam.view.m[2][0],
						cam.position[0], cam.position[1], cam.position[2],
						cx, cy, cz,
						static_cast<u32>(s_meshtrack_hash & 0xFFFFFFFFull),
						s_meshtrack_verts, s_meshtrack_seen,
						s_meshtrack_centroid[0], s_meshtrack_centroid[1], s_meshtrack_centroid[2],
						s_meshtrack_dist, s_meshtrack_normal[0], s_meshtrack_normal[1],
						s_meshtrack_normal[2], s_meshtrack_facing);
					// Re-elect next window so the biggest CURRENTLY VISIBLE mesh is reported.
					s_meshtrack_verts = 0;
				}
			}

			// The extent the previous frame actually submitted, in whichever space is in use.
			// Both tiers get the same treatment: view-space positions are in guest eye-depth
			// units, which are just as far from "near 0.1" as world-space ones are.
			const float scene_radius = s_last_bounds.radius();

			if (!cam.valid)
			{
				submit_fallback_camera();
				++s_stats.cam_fallback;

				// Legacy mode only: the fallback camera sits at the origin looking down +Z, so
				// the sphere light rides with it. The fill lights need none of this -- they have
				// no position, which is the entire point of them.
				if (light_mode() == 2)
				{
					const float origin_light[3] = {0.f, 0.f, 0.f};
					place_debug_light(origin_light, scene_radius);

					const float forward[3] = {0.f, 0.f, 1.f};
					place_sun_light(forward);
				}

				return;
			}

			remixapi_CameraInfo camera_info{};
			camera_info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
			camera_info.pNext = nullptr;
			camera_info.type = REMIXAPI_CAMERA_TYPE_WORLD;
			// Straight copies: remixapi_CameraInfo's matrices are row-vector, the same
			// convention as everything in RemixTransforms. remixapi_Transform is the one
			// exception and is not involved here.
			remix_ps2::to_camera_matrix(cam.view, camera_info.view);
			remix_ps2::to_camera_matrix(cam.projection, camera_info.projection);

			setup_camera_checked(&camera_info, "world", s_logged_camera_fail_world);
			++s_stats.cam_world;

			if (sky_camera_enabled())
			{
				// Same orientation, same projection, no translation. A skybox is a direction, not
				// a place: stripping the eye position is what makes it stay at infinity while the
				// player walks, instead of sliding past like scenery. The view matrix is
				// row-vector (see to_camera_matrix above), so the eye offset lives in row 3.
				remixapi_CameraInfo sky_info = camera_info;
				sky_info.type = REMIXAPI_CAMERA_TYPE_SKY;
				sky_info.view[3][0] = 0.f;
				sky_info.view[3][1] = 0.f;
				sky_info.view[3][2] = 0.f;

				setup_camera_checked(&sky_info, "sky", s_logged_camera_fail_sky);
				++s_stats.cam_sky;

				if (!s_logged_sky_camera)
				{
					s_logged_sky_camera = true;
					INFO_LOG("Remix: sky camera submitted -- world orientation, translation "
							 "stripped. PCSX2_REMIX_SKYCAM=0 disables it and restores rendering "
							 "sky instances with the world camera.");
				}
			}

			if (view_model_camera_enabled())
			{
				// The world camera verbatim. A viewmodel is at the eye, so unlike the sky its
				// translation stays; the runtime derives its own perspective correction from
				// whatever we send here (createViewModelInstances), and sending the world camera
				// makes that correction neutral. A narrower viewmodel FOV would go here if the
				// weapon ever needs to stop clipping into walls.
				remixapi_CameraInfo vm_info = camera_info;
				vm_info.type = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;

				setup_camera_checked(&vm_info, "viewmodel", s_logged_camera_fail_viewmodel);
				++s_stats.cam_viewmodel;

				if (!s_logged_view_model_camera)
				{
					s_logged_view_model_camera = true;
					INFO_LOG("Remix: view-model camera submitted. Instances only reach it if they "
							 "carry REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL -- tag them by hash "
							 "with rtx.viewModelTextures -- and the runtime also needs "
							 "rtx.viewModel.enable, which defaults False.");
				}
			}

			if (light_mode() == 2)
			{
				// The sphere light rides the camera, as in RPCS3: a world-space scene lit from
				// wherever the origin happens to be is usually a black scene. This is also what
				// blows out the first-person weapon, which is why it is no longer the default.
				place_debug_light(cam.position, scene_radius);

				// Camera forward in world space. p_view = p_world * V (row-vector), so the
				// gradient of view z with respect to world position is V's third column.
				const float forward[3] = {
					cam.view.m[0][2], cam.view.m[1][2], cam.view.m[2][2]};
				place_sun_light(forward);
			}
		}

		void drop_stale_camera()
		{
			if (s_active_camera.valid && (s_frame_counter - s_camera_last_accept_frame) > s_camera_hold_frames)
			{
				s_active_camera.valid = false;
				s_active_camera.matrix_hash = 0;
			}
		}

		// --- camera hypothesis enumeration ---------------------------------------------------
		//
		// One candidate matrix, one majorness, and every post-divide output space the matrix might
		// be emitting into. Lifted OUT of resolve_world_camera 2026-08-15 so the per-draw solver
		// below runs literally the same enumeration rather than a second copy of it that can drift:
		// the derived-offset derivation is the subtle part, and a per-draw camera solved through a
		// slightly different hypothesis set would place its draws in a slightly different world
		// than the frame camera places the rest -- which is the disease, not the cure.
		//
		// Order is load-bearing: the four fixed hypotheses first, then the derived ones, because
		// every caller ranks with a strict `>` and therefore keeps the FIRST hypothesis that
		// achieves the best score.
		struct camera_hypothesis
		{
			const char* name;
			float scale_x;
			float offset_x;
			float scale_y;
			float offset_y;

			// --- the depth/w half, added 2026-08-16 -------------------------------------------
			// Applied BEFORE the x/y normalisation above (apply_camera_hypothesis composes them
			// in that order, and every call site goes through it). Both default to the identity,
			// so a hypothesis written with the old five-field initialiser is unchanged.
			float scale_w = 1.f; // divides all sixteen terms; see normalize_clip_depth
			bool swap_zw = false; // exchanges columns 2 and 3 before anything else

			// --- the Y-flip half, added 2026-08-17 --------------------------------------------
			// NOT a matrix term, and that is the finding rather than an implementation detail:
			// there is no matrix that produces a projection with m[1][1] < 0, because
			// try_split_once builds `up` out of column 1 (see split_view_projection_direct's
			// header note for the arithmetic). So this rides on the hypothesis but is consumed by
			// the SPLIT, not by apply_camera_hypothesis -- the normalised matrix a flipped
			// hypothesis produces is bit-identical to its parent's.
			bool flip_y = false;
		};

		// Four fixed, plus at most three derived.
		constexpr u32 hypothesis_slots = 7;

		// Two depth families at most, never more (see build_camera_hypotheses), each optionally
		// doubled by the Y-flip twin (PCSX2_REMIX_CAMYFLIP). 4 x 7 = 28.
		inline constexpr u32 max_camera_hypotheses = 4 * hypothesis_slots;

		// One name per (depth family, slot). The name IS part of the CAMTEST identity --
		// camtest_identity folds the string in -- so these must be distinct per family and
		// STABLE across runs. Family 0's entries are the exact literals that shipped, which is
		// what makes PCSX2_REMIX_CAMDEPTH = 0 bit-identical to the behaviour before this
		// existed, accumulated drift histories included.
		//
		// Rows 3..5 are rows 0..2 with ":yf" appended -- the Y-flipped twin of each. A twin has to
		// carry its own name or camtest_identity folds to the same key as its parent and the two
		// share one drift history, which would destroy exactly the comparison the twin exists for.
		constexpr const char* hypothesis_names[6][hypothesis_slots] = {
			{"gs", "px", "ndc", "ndcY", "auto", "autoY", "r6"},
			{"gs:w", "px:w", "ndc:w", "ndcY:w", "auto:w", "autoY:w", "r6:w"},
			{"gs:zw", "px:zw", "ndc:zw", "ndcY:zw", "auto:zw", "autoY:zw", "r6:zw"},
			{"gs:yf", "px:yf", "ndc:yf", "ndcY:yf", "auto:yf", "autoY:yf", "r6:yf"},
			{"gs:w:yf", "px:w:yf", "ndc:w:yf", "ndcY:w:yf", "auto:w:yf", "autoY:w:yf", "r6:w:yf"},
			{"gs:zw:yf", "px:zw:yf", "ndc:zw:yf", "ndcY:zw:yf", "auto:zw:yf", "autoY:zw:yf", "r6:zw:yf"},
		};

		// PCSX2_REMIX_CAMDEPTH -- the w-column half of the hypothesis set
		//
		// WHY THIS EXISTS, and it is a measurement rather than a hunch. CAMTEST's one candidate
		// whose own rotation tracks the player's view is `src=slice-vf pc=0x2040 hyp=auto/C`
		// (own-turn 19.70 deg against a 23 deg pair turn -- every other non-frozen row is a
		// matrix that does not turn with the eye). It is reported INELIGIBLE, at dscale
		// 7.232e-08. That number is not a screen-space problem and no existing hypothesis can
		// move it: make_clip_solver reads columns {0, 1, 3}, normalize_screen_clip rewrites
		// columns 0 and 1, and the recovered world's unit is set by column 3 -- exactly the
		// column nothing touches. 1/7.232e-08 = 1.383e7, so this matrix's w column is ~1.4e7
		// times longer than a rigid view transform's.
		//
		// WHAT IS AND IS NOT EXPECTED TO MOVE, written down before the run so it cannot be
		// rationalised afterwards:
		//   * dscale       -- fixed by construction. Family 1 divides the whole matrix by
		//                     |column 3 xyz|, so the recovered w becomes eye-space depth in the
		//                     guest's own units and dscale lands at ~1. The row becomes ELIGIBLE.
		//   * alpha        -- a whole-matrix scale is a scale of the recovered world ABOUT THE
		//                     RECOVERED EYE, and alpha is drift / (range x turn) with drift and
		//                     range carrying the same scale. So alpha is GAUGE-INVARIANT under
		//                     family 1 and should NOT move, with one real exception: the alpha
		//                     accumulator skips a witness whose recovered range is under 1e-3,
		//                     and at a world compressed 1.4e7x that gate is currently throwing
		//                     away every witness closer than ~1.4e4 guest units. Correcting the
		//                     scale re-admits them, so alpha can move because the SAMPLE moved.
		//                     If it lands near 0, the scale was the obstacle. If it stays at
		//                     ~1.48 over a visibly larger witness count, it was not, and the
		//                     answer is that this matrix is camera-attached but is not the
		//                     view-projection.
		//   * family 2     -- the z/w SWAP is the one addition here that is not a gauge change.
		//                     It asserts the guest divided by the column this code calls depth,
		//                     which is the reading under which a column 3 of magnitude 1.4e7 (a
		//                     24-bit-ish depth row: 2^24 = 1.678e7) makes sense in the first
		//                     place. It feeds a different third equation to make_clip_solver and
		//                     therefore recovers a genuinely different world, so it CAN move
		//                     alpha on its own merits.
		//
		//   0 = OFF, THE DEFAULT. The family is the seven that shipped, under the seven names
		//       that shipped. Nothing enumerated, nothing renamed, table size unchanged.
		//   1 = the seven, PLUS the same seven with the whole matrix divided by |column 3 xyz|
		//       (names suffixed ":w"). Both are present so the gauge claim above is demonstrated
		//       by the table rather than asserted by this comment.
		//   2 = the seven, PLUS the seven z/w-SWAPPED-and-normalised ones (":zw").
		//   3 = the ":w" seven plus the ":zw" seven, with no baseline -- a clean table once 1
		//       and 2 have each been read.
		//
		// THIS CAN CHANGE THE PICTURE at any non-zero setting, and the mechanism is named rather
		// than hedged: the added hypotheses go through the SAME election loop as the originals,
		// so one of them can out-score the incumbent and be installed. A ":w" winner scales the
		// recovered world by ~1.4e7 about the eye, which moves scene radius, every injected
		// light's placement and the depth-scale gate's verdict. A ":zw" winner recovers a
		// different world outright. Do not describe any setting of this knob as invisible.
		//
		// Read LIVE, once per camera resolve, and NOT through live_int -- see env_int_live.
		int camdepth_mode()
		{
			return std::clamp(env_int_live(L"PCSX2_REMIX_CAMDEPTH", 0), 0, 3);
		}

		// PCSX2_REMIX_CAMDEPTHSNAP -- snap the derived divisor to the nearest power of two when
		// it is within 2% of one.
		//
		// DEFAULT 0, i.e. do not snap, and that is a judgement about this title's numbers rather
		// than caution. The leading guess was a 24-bit GS depth convention, 2^-24 = 5.96e-08
		// against a measured 7.232e-08. But the measured divisor is 1.383e7 = 0.824 x 2^24 =
		// 1.648 x 2^23, which is not a power of two and not within snapping distance of one, so
		// snapping would REINTRODUCE a 21% scale error that the derived factor does not have.
		// The knob exists because a title whose divisor really is 2^24 to within float noise is
		// worth pinning exactly, and because the claim "it is not a power of two" should be
		// falsifiable by the same instrument that made it.
		int camdepth_snap()
		{
			return std::clamp(env_int_live(L"PCSX2_REMIX_CAMDEPTHSNAP", 0), 0, 1);
		}

		// PCSX2_REMIX_CAMYFLIP -- the Y-flip half of the hypothesis set, and the sign policy that
		// makes it scorable
		//
		// WHAT WAS MISSING, and it is a proof rather than a hunch. m[1][1] of the recovered
		// projection is up . col1, and try_split_once builds `up` as the component of up_hint
		// perpendicular to forward, where up_hint is unprojected from ndc (0, +1, 0.5) -- i.e. it
		// IS the world direction in which clip y increases. So up . col1 > 0 identically, for every
		// matrix, under every hypothesis. m[1][1] < 0 was unreachable, and so was the -1.f dock
		// score_perspective applies to it. The three routes that look like they should reach it do
		// not, each for an arithmetic reason (all three are worked through in
		// split_view_projection_direct's header note):
		//   * negating scale_y forms M . diag(1,-1,1,1), which flips up_hint along with column 1;
		//     the two cancel in m[1][1] and what actually flips is m[0][0]. THIS PROJECT'S OWN DUMP
		//     IS THE EVIDENCE: `ndc/R=5.00 ndcY/R=4.50`, a gap of exactly the 0.5 that
		//     score_perspective docks for m[0][0] < 0 and nothing else.
		//   * negating column 1 of the normalised matrix after the fact is that same product.
		//   * negating up_hint flips right and up together, so m[0][0] and m[1][1] together -- the
		//     composition of the flip below with a sign the family already spans, adding nothing.
		// The one mechanism left is the BASIS: take `up` with the opposite sign. That is what
		// split_view_projection_direct's flip_up does and what a ":yf" hypothesis carries.
		//
		// WHAT MOVES AND WHAT CANNOT, written before the run:
		//   * the recovered WORLD             -- CANNOT MOVE. flip_up does not touch the fused
		//     matrix, so make_clip_solver's inverse is bit-identical to the parent's and every
		//     un-projected vertex lands on the same float. A ":yf" CAMTEST row must therefore
		//     report the SAME alpha as its parent, to the last digit. That equality is the check
		//     on this whole change: if a ":yf" alpha differs from its twin's, this comment is
		//     wrong about the code.
		//   * the published CAMERA            -- moves. The view becomes a vertical mirror
		//     (viewToWorld determinant -1) and the projection's m[1][1] negates to compensate.
		//     view * projection == fused still holds identically, so what the runtime does with a
		//     mirrored camera pair is a question about the runtime.
		//   * score_perspective               -- moves at modes 2 and 3 only, and for every
		//     hypothesis rather than just the twins. See sign_policy_* in RemixTransforms.h.
		//
		//   0 = OFF, THE DEFAULT. No twin is enumerated, the family is exactly what it was, and
		//       score_perspective is called with sign_policy_shipped -- the same two docks, the
		//       same weights, term for term.
		//   1 = enumerate the ":yf" twins, keep the shipped sign weights. AUDIT ONLY: a twin is
		//       refused by the election outright at this mode (see election_eligible in
		//       resolve_world_camera, and the matching guard in solve_per_draw_clip). The twin also
		//       scores exactly 1.0 below its parent -- the m[1][1] dock, reachable for the first
		//       time -- but that gap is NOT what makes this safe and must not be relied on: under
		//       PCSX2_REMIX_CAMTEST = 2 the election ranks by measured drift, parent and twin hold
		//       separate drift histories, and a twin whose history filled first would outrank its
		//       parent by four orders of magnitude on a race rather than on a measurement.
		//       What this mode buys: the twins appear in the CAMTEST table with their own rows.
		//   2 = enumerate the twins, and score NEITHER sign. Handedness and parity stop being
		//       tie-breaks. CAN CHANGE THE PICTURE: with the 0.5 m[0][0] dock gone, two
		//       hypotheses that differed only by that dock now tie, and the tie goes to whichever
		//       comes first in the family -- which is not necessarily the one that was winning.
		//   3 = enumerate the twins, and dock a POSITIVE m[1][1] instead. The reading under which
		//       PS2 screen-down clip y makes the mirrored factorisation the correct one. CHANGES
		//       THE PICTURE BY DESIGN: every ":yf" twin now outranks its parent by 1.0, so the
		//       elected camera is published vertically mirrored.
		//
		// Read LIVE, once per camera resolve, and NOT through live_int -- see env_int_live.
		int camyflip_mode()
		{
			return std::clamp(env_int_live(L"PCSX2_REMIX_CAMYFLIP", 0), 0, 3);
		}

		// The sign weighting a given CAMYFLIP mode selects. One function so the frame election, the
		// per-draw solver and the CAMTESTALL probe sweep cannot disagree about how a candidate is
		// scored -- the same reason build_camera_hypotheses is shared rather than copied.
		int camyflip_sign_policy(int mode)
		{
			switch (mode)
			{
				case 2:
					return remix_ps2::sign_policy_neutral;
				case 3:
					return remix_ps2::sign_policy_prefer_flipped_y;
				default:
					return remix_ps2::sign_policy_shipped;
			}
		}

		// The mode in force for the window being resolved, refreshed once per resolve by
		// resolve_world_camera and read by the per-draw solver. The per-draw path must NOT read
		// the environment (this title submits ~830 draws a window) and must NOT disagree with the
		// frame camera about what space the guest emits into -- a draw placed through a different
		// hypothesis set than the rest of the frame is the disease, not the cure.
		int s_camdepth_active = 0;
		int s_camdepth_snap_active = 0;
		int s_camdepth_logged_mode = -1;

		// PCSX2_REMIX_CAMYFLIP, cached beside them and for exactly the same two reasons: the draw
		// path must not touch the environment, and a per-draw camera enumerated or scored
		// differently from the frame camera places its draws in a different world than the rest of
		// the frame. s_camyflip_sign_active is derived once here so no call site re-derives it.
		int s_camyflip_active = 0;
		int s_camyflip_sign_active = remix_ps2::sign_policy_shipped;
		int s_camyflip_logged_mode = -1;

		// PCSX2_REMIX_CAMSCALE, cached the same way and for a second reason.
		//
		// It was `static const float value = env_float(...)`, i.e. LATCHED AT FIRST USE -- and the
		// renderer's first use happens ~0.2 s before the per-game .conf is applied, so a CAMSCALE
		// line in the .conf has never once taken effect. That is the exact trap that has cost four
		// separate measurements on this project, sitting on the one gate that decides whether the
		// candidate this whole change is aimed at is reported ELIGIBLE. Widening the gate to look
		// at it would have been a silent no-op.
		//
		// It cannot be read per call: camera_scale_limit() is called once per scored triple (up to
		// 2604 a window) and once per draw from the per-draw solver. So it is refreshed once per
		// resolve, beside s_camdepth_active, and every call site reads the cached float.
		//
		// SAY WHAT THIS CHANGES: nothing today, because no .conf on this title sets CAMSCALE. From
		// now on, adding that line will actually move the gate -- and the gate decides which
		// cameras are accepted, so it can change the picture. That is the knob working as it is
		// documented to, not a new behaviour, but it is a difference from what shipped.
		float s_camera_scale_limit = 8.f;

		// The seven screen-space hypotheses, built against whatever matrix will actually be
		// normalised -- which under a z/w swap is the SWAPPED matrix, because the derived
		// offsets below read its column 3. `variant` selects only the naming.
		u32 build_screen_hypotheses(const remix_ps2::mat4& oriented, const viewport_constants& vp,
			float reference_aspect, bool column_major, u8 source, u64 ucode_hash, u32 variant,
			camera_hypothesis (&out)[hypothesis_slots])
		{
			const char* const* const names = hypothesis_names[std::min(variant, 2u)];
			const float width = static_cast<float>(vp.width);
			const float height = static_cast<float>(vp.height);

			u32 count = 0;

			// The guest's post-divide output space is not knowable in advance: the fused
			// matrix may already carry the full 12.4 viewport fold, or emit pixels, or emit
			// plain NDC and leave the viewport to a post-divide multiply-add in the VU. Try
			// each, both majorness ways round, and let score_perspective decide. That is a
			// handful of 4x4 inversions per frame.
			//
			// The slot index is fixed per hypothesis KIND, not derived from `count`, so the name
			// table above stays aligned no matter which entries a given matrix qualifies for --
			// and so the name lookup is never sequenced against `count++`, which is unspecified.
			//
			// GS 12.4 subpixels: the exact inverse of the per-vertex un-projection.
			out[count++] = {names[0], width * 8.f, vp.ofx + (width * 8.f) - 8.f + 0.05f,
				-(height * 8.f), vp.ofy + (height * 8.f) - 8.f + 0.05f};
			// Pixels, origin top-left. XYOFFSET cancels: it is added after this stage.
			out[count++] = {names[1], width * 0.5f, (width * 0.5f) - 0.5f, -(height * 0.5f), (height * 0.5f) - 0.5f};
			// Already NDC, +Y up like Remix.
			out[count++] = {names[2], 1.f, 0.f, 1.f, 0.f};
			// Already NDC, +Y down like the GS.
			out[count++] = {names[3], 1.f, 0.f, -1.f, 0.f};

			// The four fixed hypotheses above assume the guest's post-divide space is
			// one of the obvious ones. Rainbow Six 3's is not: it emits a [0,1]-style
			// normalised space and keeps the aspect correction in a separate
			// post-divide scale vector, so its offsets are 0.5 and nothing in the
			// table matches.
			//
			// The offsets do not have to be guessed. For a fused matrix the viewport
			// offset is *determined*: u = col0 - ox*col3 must be the view rotation's
			// column 0 times a scalar, and that is only orthogonal to col3 for
			// ox = dot(col0, col3) / |col3|^2. Same for oy. Any output range of the
			// form NDC[-1,1] -> [0, R] additionally has scale == offset, which is
			// what fixes sx.
			//
			// sy is then the one genuinely free parameter, and it is chosen so the
			// recovered aspect equals the display's. Be clear about what that costs:
			// score_perspective's aspect test is constructed rather than measured for
			// these two hypotheses, so it no longer discriminates. Its fovY window
			// still does, and the split itself -- which is what actually decides
			// whether the matrix is a camera -- is untouched.
			const float c3[3] = {oriented.m[0][3], oriented.m[1][3], oriented.m[2][3]};
			const float len_sq = (c3[0] * c3[0]) + (c3[1] * c3[1]) + (c3[2] * c3[2]);

			if (len_sq > 1e-8f && std::isfinite(len_sq))
			{
				const float c0[3] = {oriented.m[0][0], oriented.m[1][0], oriented.m[2][0]};
				const float c1[3] = {oriented.m[0][1], oriented.m[1][1], oriented.m[2][1]};

				const float ox = ((c0[0] * c3[0]) + (c0[1] * c3[1]) + (c0[2] * c3[2])) / len_sq;
				const float oy = ((c1[0] * c3[0]) + (c1[1] * c3[1]) + (c1[2] * c3[2])) / len_sq;

				const float u[3] = {c0[0] - (ox * c3[0]), c0[1] - (ox * c3[1]), c0[2] - (ox * c3[2])};
				const float v[3] = {c1[0] - (oy * c3[0]), c1[1] - (oy * c3[1]), c1[2] - (oy * c3[2])};

				const float len_u = std::sqrt((u[0] * u[0]) + (u[1] * u[1]) + (u[2] * u[2]));
				const float len_v = std::sqrt((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2]));

				if (len_u > 1e-6f && len_v > 1e-6f && std::abs(ox) > 1e-6f && std::abs(oy) > 1e-6f)
				{
					const float sy_mag = std::abs(oy) * (len_v / len_u) / reference_aspect;

					out[count++] = {names[4], ox, ox, -sy_mag, oy};
					out[count++] = {names[5], ox, ox, sy_mag, oy};

					// Rainbow Six 3 keeps its 4:3-to-display aspect correction outside this
					// VU1 clip matrix. vi06 selects the matrix address dynamically, so the
					// offset is not stable across loading and gameplay; use its exact ucode.
					if (!column_major && (source == 1 || source == 3) &&
						ucode_hash == 0x86d066e65a57ece2ULL)
						out[count++] = {names[6], reference_aspect, ox, -1.f, oy};
				}
			}

			return count;
		}

		// The full family: up to two depth treatments of the seven screen hypotheses above, each
		// optionally doubled by its Y-flipped twin.
		//
		// ORDER IS STILL LOAD-BEARING for exactly the reason recorded above -- every caller ranks
		// with a strict `>` and therefore keeps the FIRST hypothesis achieving the best score --
		// so the baseline family, when present, is emitted first and the depth-corrected family
		// after it. At PCSX2_REMIX_CAMDEPTH = 0 the emitted list is byte-for-byte what it was
		// before this parameter existed.
		//
		// The Y-flip twins are collected separately and appended AFTER every unflipped hypothesis,
		// so (a) the index of every existing entry is unchanged, and (b) at CAMYFLIP = 2, where the
		// neutral sign policy makes a twin score exactly EQUAL to its parent, the parent is reached
		// first and the strict `>` keeps it. At CAMYFLIP = 3 the twin outscores its parent
		// outright, which is that mode's whole purpose. At CAMYFLIP = 1 the ordering is not what
		// protects the parent -- an explicit election gate is; see the knob's own note.
		u32 build_camera_hypotheses(const remix_ps2::mat4& oriented, const viewport_constants& vp,
			float reference_aspect, bool column_major, u8 source, u64 ucode_hash, int depth_mode,
			int snap_mode, int yflip_mode, camera_hypothesis (&out)[max_camera_hypotheses])
		{
			const int mode = std::clamp(depth_mode, 0, 3);
			const bool want_flipped = std::clamp(yflip_mode, 0, 3) > 0;
			u32 count = 0;

			// The twins, held back until every unflipped hypothesis has been emitted.
			camera_hypothesis flipped[max_camera_hypotheses];
			u32 flipped_count = 0;

			// One emitted hypothesis, plus its twin when armed. `variant` is the depth family
			// (0 = baseline, 1 = ":w", 2 = ":zw") and `slot` indexes the name table -- which is why
			// build_screen_hypotheses emits in slot order and never derives a name from `count`.
			const auto take = [&](const camera_hypothesis& hyp, u32 variant, u32 slot,
								  float divisor, bool swap) {
				if (count < max_camera_hypotheses)
				{
					out[count] = hyp;
					out[count].scale_w = divisor;
					out[count].swap_zw = swap;
					++count;
				}

				if (want_flipped && flipped_count < max_camera_hypotheses)
				{
					flipped[flipped_count] = hyp;
					flipped[flipped_count].name = hypothesis_names[variant + 3][slot];
					flipped[flipped_count].scale_w = divisor;
					flipped[flipped_count].swap_zw = swap;
					flipped[flipped_count].flip_y = true;
					++flipped_count;
				}
			};

			// Appends the held-back twins. Called at every return path, because a family with no
			// twins behind it is not the family this function was asked for.
			const auto finish = [&]() {
				for (u32 i = 0; i < flipped_count && count < max_camera_hypotheses; ++i)
					out[count++] = flipped[i];

				return count;
			};

			// Family 0: the columns exactly as published. Present at modes 0, 1 and 2.
			if (mode != 3)
			{
				camera_hypothesis base[hypothesis_slots];
				const u32 n = build_screen_hypotheses(oriented, vp, reference_aspect, column_major,
					source, ucode_hash, 0, base);

				for (u32 i = 0; i < n; ++i)
					take(base[i], 0, i, 1.f, false);
			}

			if (mode == 0)
				return finish();

			// Family 1 (":w"), at modes 1 and 3: same columns, whole matrix divided by the
			// MEASURED |column 3 xyz| so the recovered w is eye-space depth in guest units.
			// Derived, not enumerated, and deliberately: the quantity is directly measurable from
			// the matrix in front of us, an enumerated 2^24 would leave whatever residual the
			// title's real convention has (0.824x on this one), and a derived factor needs no new
			// table entry the next time a title emits w in some third unit.
			//
			// Family 2 (":zw"), at modes 2 and 3: columns 2 and 3 exchanged FIRST, then the same
			// derived normalisation of the new column 3. The swap has to precede the screen
			// hypotheses, not follow them, because the derived ox/oy read column 3.
			const bool want_plain_scaled = (mode == 1) || (mode == 3);
			const bool want_swapped = (mode == 2) || (mode == 3);
			const float snap_tolerance = (snap_mode > 0) ? 0.02f : 0.f;

			const auto emit = [&](bool swap, u32 variant) {
				const remix_ps2::mat4 swapped =
					swap ? remix_ps2::normalize_clip_depth(oriented, 1.f, true) : oriented;

				const float measured = remix_ps2::clip_w_scale(swapped);
				const float divisor = (snap_tolerance > 0.f)
										  ? remix_ps2::snap_power_of_two(measured, snap_tolerance)
										  : measured;

				if (!std::isfinite(divisor) || !(divisor > 0.f))
					return;

				const remix_ps2::mat4 corrected = remix_ps2::normalize_clip_depth(swapped, divisor, false);

				camera_hypothesis base[hypothesis_slots];
				const u32 n = build_screen_hypotheses(corrected, vp, reference_aspect, column_major,
					source, ucode_hash, variant, base);

				for (u32 i = 0; i < n; ++i)
					take(base[i], variant, i, divisor, swap);
			};

			if (want_plain_scaled)
				emit(false, 1);

			if (want_swapped)
				emit(true, 2);

			return finish();
		}

		// The one place a hypothesis is turned into a matrix. Three call sites compose these two
		// normalisations -- the frame election, the per-draw solver and the CAMTESTALL probe
		// sweep -- and the file's own note on build_camera_hypotheses says why they must not each
		// keep their own copy: a per-draw camera solved through a slightly different hypothesis
		// set places its draws in a slightly different world than the frame camera places the
		// rest. The depth correction runs FIRST; normalize_screen_clip's offsets are expressed
		// against the column 3 that normalize_clip_depth has already settled.
		//
		// hyp.flip_y is DELIBERATELY NOT READ HERE. There is no matrix that produces a projection
		// with m[1][1] < 0 -- try_split_once derives `up` from column 1, so up . col1 > 0 for every
		// input -- so the Y-flip is consumed by split_view_projection_direct's flip_up instead. The
		// consequence is worth stating where someone will hit it: a ":yf" hypothesis and its parent
		// produce the SAME normalised matrix, hence the same clip solver, hence the same recovered
		// world and the same CAMTEST alpha. Only the published camera differs.
		remix_ps2::mat4 apply_camera_hypothesis(const remix_ps2::mat4& oriented, const camera_hypothesis& hyp)
		{
			return remix_ps2::normalize_screen_clip(
				remix_ps2::normalize_clip_depth(oriented, hyp.scale_w, hyp.swap_zw),
				hyp.scale_x, hyp.offset_x, hyp.scale_y, hyp.offset_y);
		}

		// -----------------------------------------------------------------------------------------
		// PCSX2_REMIX_CAMTEST -- the falsification test the camera election has never had
		// -----------------------------------------------------------------------------------------
		//
		// WHAT IS UNTESTED, stated precisely because no counter in this file shows it.
		// try_split_once DEFINES the projection as viewToWorld * fused (RemixTransforms.cpp:184) and
		// the view as that matrix's inverse (:180). So view * projection == fused identically and the
		// l1_error it reports (:191) is STRUCTURALLY ZERO -- it measures nothing. Geometry is then
		// un-projected through the same fused matrix (solve_world_position in OnDrawPrims). World ->
		// NDC therefore round-trips exactly for ANY invertible matrix, which means a correct picture
		// is zero evidence that the elected matrix is the camera. score_perspective only asks whether
		// the recovered projection is SHAPED like a projection. Nothing on any path has ever tested an
		// elected matrix against the geometry it un-projects.
		//
		// THE ONE THING THAT MUST BE TRUE: static world geometry has a STABLE RECOVERED WORLD
		// POSITION while the camera moves. That is what this measures, for EVERY candidate rather
		// than only the elected one -- the useful output is "candidate X drifts 4 units, the elected
		// candidate drifts 900".
		//
		// WHY IT IS IMMUNE TO THE CLIPPING CONFOUND, which wrecked the centroid test, the AABB-extent
		// test and the bearing test in turn. The PS2 clips geometry to its frustum, so a partly
		// visible object presents a DIFFERENT vertex set frame to frame and every positional summary
		// of it moves for a reason that has nothing to do with the camera. Two independent defences,
		// both required:
		//   1. The witness key is (material content hash, vertex count, index count). It has NO
		//      positional term, so the quantity being measured cannot move the key, and a draw whose
		//      set changed simply fails to pair instead of being compared. The material content hash
		//      comes from guest TEX0/TEXA/CLUT state and is untouched by any camera.
		//   2. The draw's screen-space bounding box must lie STRICTLY INSIDE the viewport, touching
		//      no edge, so the geometry provably was not clipped. Same px/rt values the per-draw dump
		//      prints, from the same locals.
		//
		// WHAT IT IS BLIND TO, said plainly so a zero is not over-read: an error that is CONSTANT IN
		// WORLD SPACE. If the recovered world is a fixed rotation or offset of the true one, every
		// static point holds still and the drift is zero -- which is the right answer, because a
		// global gauge is unobservable and harmless to lighting. What it does see is a CAMERA-ATTACHED
		// error (the world riding the view, or mirrored about a plane that turns with it), and that is
		// exactly the reported symptom: a parked vehicle's shadow swinging under a fixed light.
		//
		// It also survives the pipeline's known one-frame camera staleness. Un-projecting window k
		// with the matrix of window k-1 rotates the recovered world by one window of camera motion;
		// doing the same to window k+1 rotates it by ~the same amount, so the DIFFERENCE is second
		// order in the camera's motion while a camera-attached error is first order in it.

		// PCSX2_REMIX_CAMTEST
		//   0 = OFF, the default. Nothing in this file behaves differently and nothing is sampled.
		//   1 = AUDIT ONLY. No behaviour change of any kind; logs a ranked drift table per round.
		//   2 = ELECT BY DRIFT. resolve_world_camera picks the lowest-drift candidate instead of the
		//       highest shape score, and falls back to the shape election when no candidate has
		//       enough evidence. This CAN change the picture -- see camtest_rank_base.
		//
		// READ LIVE ON EVERY CALL, never latched and deliberately not through live_int. Two separate
		// caching bugs were paid for on this project: a `static const` caches the pre-conf value
		// forever (the renderer goes live at t = 9.774 s, the per-game .conf applies at t = 10.008 s),
		// and live_int/live_float only re-parse when paths::knob_generation() moves, which the
		// per-game .conf never bumps because it calls SetEnvironmentVariableW directly. Called once
		// per presented window, never per draw -- the draw path reads the cached s_camtest_active.
		int camtest_mode()
		{
			return std::clamp(env_int_live(L"PCSX2_REMIX_CAMTEST", 0), 0, 2);
		}

		// PCSX2_REMIX_CAMTESTALL -- score EVERY sliced matrix, not just the shape-filter survivors
		//
		// WHY. The drift test above scores only triples that already cleared
		// split_view_projection_direct AND score_perspective, and those two filters decide what
		// "looks like a camera" by SHAPE. On SOCOM: Combined Assault they reject 47,838 triples
		// (39,403 of them at `notpersp`) and CAMTEST is left with TWO rows -- the same matrix under
		// two hypotheses, alpha 1.7-3.0, nothing anywhere near 0. A shape filter cannot be the
		// arbiter of a question the drift test answers directly: whether the matrix makes static
		// geometry hold still. So let the measurement do the electing.
		//
		//   0 = OFF, the default. The table is exactly what it is today.
		//   1 = score every published candidate x majorness x hypothesis whose clip solver inverts,
		//       whether or not it splits and whether or not it scores as a perspective.
		//   2 = as 1, plus the 61 sliding 4-qword windows of the VU1 transform probe (the 64-qword
		//       neighbourhood RemixVU1Capture snapshots around the back-sliced object MVP). This is
		//       the "the true VP is at an address the slicer never reads" hypothesis, measured.
		//
		// WHAT IT CANNOT DO, stated because a "structurally invisible" claim was wrong once on this
		// project and will not be repeated loosely:
		//   * A matrix that fails the split has no vp_split to install, and the election loop's
		//     `continue` on split failure is upstream of every rank computation. So no matrix this
		//     mode adds can ever become the camera, under any CAMTEST mode. That is a control-flow
		//     fact, not an argument about matrix algebra.
		//   * Under CAMTEST = 1 nothing but the log changes.
		//   * Under CAMTEST = 2 the ELECTION reads measured drift, and the enlarged candidate table
		//     below changes which histories survive eviction. That CAN change the picture. It is
		//     also true that CAMTEST = 2 is already a mode in which a measurement picks the camera.
		//   * It costs GS-thread time on every window that pairs (see camtest_candidate_slots). It
		//     changes no pixel, but it can change frame PACING, which changes how fast the eye turns
		//     between two windows. Read the round header's "mean turn" before comparing runs.
		//
		// Read live for the same two reasons camtest_mode() is, and cached into
		// s_camtest_all_active once per window because the resolve loop below runs per candidate.
		int camtest_all_mode()
		{
			return std::clamp(env_int_live(L"PCSX2_REMIX_CAMTESTALL", 0), 0, 2);
		}

		// Qualifying windows to let pass before the test arms. Default 0 = arm immediately.
		//
		// Mirrors drawdump_after_frames()/frametrace_after_frames()/worldprobe_after_frames(), and
		// for the reason recorded there: a diagnostic that arms on "the first frame that qualifies"
		// measures the LOADING phase, which on this title is mission start. That mistake cost three
		// separate measurements in one session. The symptom this test is aimed at is something the
		// player sees while turning in-mission, so the measurement has to be delayed to in-mission.
		u64 camtest_after_frames()
		{
			return static_cast<u64>(std::max(0, env_int_live(L"PCSX2_REMIX_CAMTESTAFTER", 0)));
		}

		// Witness draws followed at once. Wider than WORLDFIX's 16 because the strictly-inside-the-
		// viewport rule is a hard filter -- most of a frame's draws touch an edge -- and a median is
		// only worth taking over a handful of survivors.
		//
		// RAISED 32 -> 128, and this is a suspect in the silence rather than a comfort margin. The
		// one session that ever produced a table reported 17-20 witnesses per pair against 32 slots.
		// This title submits ~830 draws a window; the moment the number clearing the strictly-inside
		// rule exceeds the slot count, the table cannot hold a window's own draws, let alone hold
		// them into the NEXT window -- and pairing requires exactly that. A witness evicted before it
		// draws again pairs with nothing, silently. 128 slots cost ~30 KB and remove the ceiling.
		constexpr u32 camtest_witness_slots = 128;

		// Clip samples kept per witness. Taken at a fixed stride over a vertex count the key has
		// already pinned, so sample i is the same vertex in both windows of a pair.
		constexpr u32 camtest_samples = 8;

		// (candidate x majorness x hypothesis) identities tracked at once. An eviction resets a
		// slot's history, and a table that thrashes measures nothing -- so this is sized from the
		// worst case rather than from what today's filters happen to let through.
		//
		//   published candidates            32   (RemixVU1Capture::max_candidates)
		//   x majorness                      2
		//   x hypotheses                    28   (max_camera_hypotheses)
		//                               = 1792   triples per window under CAMTESTALL = 1
		//   + probe windows                 61   (64-qword snapshot, 4-qword sliding window)
		//     x 2 x 28                  = 3416   additional triples under CAMTESTALL = 2
		//                               = 5208   worst case
		//
		// RAISED 2048 -> 8192 alongside PCSX2_REMIX_CAMDEPTH, and not merely to clear the 2604 that
		// was the worst case then. That session's own heartbeat read "Candidates 2048 of 2048, 5721
		// evicted with history" -- the table was ALREADY full and thrashing at the old capacity,
		// because the identity folds mem_offset and start_pc and the scan/slice sources churn both.
		// Every one of those 5721 evictions reset a drift history, which is the measurement that is
		// supposed to be judging the hypotheses.
		//
		// RAISED AGAIN 8192 -> 16384 alongside PCSX2_REMIX_CAMYFLIP, for that same reason and not
		// for comfort. The Y-flip twins double max_camera_hypotheses from 14 to 28, which doubles
		// the worst case from 2604 to 5208 -- against 8192 that is only 1.6x of headroom, where the
		// figure judged sufficient last time was 3.1x, and the churn the eviction counter measured
		// is unchanged. Adding hypotheses on top of a table that cannot hold them corrupts the very
		// drift measurement the new hypotheses exist to be judged by. 16384 restores 3.1x.
		//
		// ~8.5 MB of zero-initialised BSS; pages are only touched by the slots actually used, so at
		// CAMYFLIP = 0 (the default, no twins) the resident cost is what it was.
		constexpr u32 camtest_candidate_slots = 16384;

		// Per-pair medians retained per candidate; the reported score is the median of these, so one
		// bad pair cannot decide a ranking.
		constexpr u32 camtest_history = 64;

		// Qualifying window pairs before the FIRST report. Was a flat 60, and 60 was too high a bar:
		// four sessions produced one table between them, and three produced complete silence over
		// hundreds of armed windows. A noisy early answer that sharpens is worth far more than
		// nothing, and nothing is what 60 delivered.
		//
		// The per-candidate drift history is a 64-deep ring that is NEVER reset by a report, so
		// every later table is computed over strictly more evidence than the one before it. The
		// target therefore doubles after each report -- 8, 16, 32, 60, 60, ... -- which gives a
		// first reading almost immediately and settles back to the original cadence.
		//
		// Read live, and NOT through live_int: see env_int_live's note.
		u32 camtest_first_round_pairs()
		{
			return static_cast<u32>(std::clamp(env_int_live(L"PCSX2_REMIX_CAMTESTPAIRS", 8), 1, 600));
		}

		constexpr u32 camtest_round_pairs_max = 60;

		// PCSX2_REMIX_CAMTESTROWS -- how many ranked rows the table prints.
		//
		// WHY THIS IS NOT COSMETIC. The table is sorted by drift ASCENDING and printed top-down,
		// and a FROZEN candidate has low drift for free -- that is the entire reason the FROZEN tag
		// exists. So frozen rows monopolise the printed window and the rows worth reading are
		// pushed below it. MEASURED on the 2026-08-16 10:19 session: 4,569 ranked rows, 32 printed,
		// and the best non-frozen eligible row was at rank #238 and #281 on the two rounds. Every
		// non-frozen slice-vf row was in the table and none of them was ever printed, which read
		// from the outside as "source 7 publishes only frozen matrices".
		//
		// Default 32, i.e. exactly what the CAMTESTALL table printed before. Log-only: this cannot
		// change what is measured, ranked or elected.
		u32 camtest_row_limit()
		{
			return static_cast<u32>(std::clamp(env_int_live(L"PCSX2_REMIX_CAMTESTROWS", 32), 1, 512));
		}

		// PCSX2_REMIX_CAMTESTSRC -- print only rows from one source (see candidate_source_name).
		//
		// -1 (default) prints every source, which is today's behaviour. 7 prints only the
		// register-resident slice-vf rows, 1 only the plain back-slice, and so on. The RANKING is
		// untouched -- a filtered row keeps the rank number it holds in the full table, so "#238"
		// still means 238th of 4,569 -- only which rows reach the log changes. Log-only.
		int camtest_source_filter()
		{
			return std::clamp(env_int_live(L"PCSX2_REMIX_CAMTESTSRC", -1), -1, 7);
		}

		// --- THE LEVER ARM -----------------------------------------------------------------------
		//
		// A pair is only informative if the eye actually TURNED, and alpha divides the measured
		// drift by (range x turn in radians) -- so the turn is the denominator and a small one
		// amplifies every source of noise in the numerator. The first round this instrument ever
		// produced made that concrete:
		//
		//   the user turned a full 360 in ~30 s at FULL STICK  = 12 deg/s, the game's maximum
		//   at 60 windows/s                                    = 0.2 deg per window
		//   pairs were formed against the PREVIOUS window, 2-3 back (1 window in 3 draws nothing)
		//                                                      = 0.4-0.6 deg of turn per pair
		//   measured own-turn on the elected row               = 0.44 deg  <-- exact match
		//
		// 0.44 deg is 0.0077 rad. Every row came back with an alpha between 0.07 and 0.18 and the
		// ranking was noise: matrices that had frozen entirely scored the lowest drift of all, for
		// free. The instrument was correct and the signal was too small to read.
		//
		// THE USER CANNOT TURN FASTER. So the pair is no longer formed against the most recent
		// window: an ANCHOR window is held and the current window is paired against it, letting the
		// turn ACCUMULATE across the span. At 0.2 deg/window a 30-window anchor yields ~6 deg -- a
		// 14x better lever arm for half a second of separation, which is far too short for real
		// scene change to contaminate a witness whose vertex and index counts must match exactly.
		//
		// All three halves of the pair re-anchor on the SAME window or the measurement is
		// incoherent: the witness clip samples (camtest_sample), the candidate solvers
		// (camtest_roll) and the window's eye/forward. s_camtest_anchor_arm is the single flag.
		//
		// Windows an anchor may be held for. Bounded because a span is also a window in which the
		// scene may genuinely change, and because an anchor held forever measures nothing.
		u32 camtest_span_windows()
		{
			return static_cast<u32>(std::clamp(env_int_live(L"PCSX2_REMIX_CAMTESTSPAN", 30), 1, 600));
		}

		// Minimum ACCUMULATED turn, anchor to now, for a pair to count. In MILLI-DEGREES, because
		// env_int_live parses integers.
		//
		// RAISED 0.25 -> 2.0 deg, and this is the whole point of the span. A pair with 0.4 deg of
		// turn is measuring mostly noise no matter how many witnesses it has, so admitting it was
		// never doing any good. 2.0 deg is 4.5x the 0.44 that failed, and at the default 30-window
		// span it is reached in ~10 windows at full stick and ~20 at half stick -- so pairs still
		// arrive at 2-4 per second rather than being rationed. The round line now reports the turn
		// actually achieved, so this can be raised on evidence instead of guessed at.
		float camtest_min_turn_degrees()
		{
			return 0.001f * static_cast<float>(std::clamp(env_int_live(L"PCSX2_REMIX_CAMTESTTURNMIN", 2000), 1, 90000));
		}

		// Upper bound: a camera CUT is not a turn, and averaging one in only adds noise.
		//
		// Left at 45 deg deliberately, now that the span makes it a real question. It is not close
		// to binding: the span is bounded at 30 windows and the game's own maximum is 12 deg/s, so
		// the most a legitimate pair can accumulate is 30/60 * 12 = 6 deg -- 7.5x of headroom. And
		// a pair that does exceed it is no longer merely discarded: it re-anchors immediately, so
		// an actual cut costs one anchor cycle instead of jamming the instrument until the span
		// runs out. Raising it further would start admitting real cuts, which is the one thing it
		// exists to exclude.
		float camtest_max_turn_degrees()
		{
			return 0.001f * static_cast<float>(std::clamp(env_int_live(L"PCSX2_REMIX_CAMTESTTURNMAX", 45000), 1, 180000));
		}

		// Windows between heartbeats. THE INSTRUMENT'S FAILURE MODE WAS SILENCE, and its one hint
		// ("the eye never turned by X between two windows that shared a witness") conflated two
		// opposite causes that need opposite responses. 0 disables it.
		u32 camtest_heartbeat_windows()
		{
			return static_cast<u32>(std::clamp(env_int_live(L"PCSX2_REMIX_CAMTESTHEARTBEAT", 120), 0, 100000));
		}

		// Mode 2 will not elect on less evidence than this.
		constexpr u32 camtest_min_pairs = 8;
		constexpr u32 camtest_min_witnesses = 3;

		// Mode 2's rank for a candidate with a drift measurement is camtest_rank_base / (1 + drift),
		// which is strictly decreasing in drift and always far above any shape score (the maximum
		// score_perspective can return is 6, plus the +100 source bonus). So the rule is: any
		// candidate with enough evidence outranks any candidate without, lowest drift wins among
		// those that have it, and with no measurements at all the shape election stands unchanged.
		constexpr float camtest_rank_base = 1e6f;

		struct camtest_witness
		{
			u64 key = 0;
			u64 frame = 0; // window this slot's `now` came from
			u32 count = 0;
			float now[camtest_samples][3] = {}; // clip (x, y, w) -- camera-independent guest output
			u64 prev_frame = 0;
			u32 prev_count = 0;
			float prev[camtest_samples][3] = {};
			bool prev_valid = false;

			// Has this witness ever contributed to a pair? A slot that is evicted having never
			// paired is an ORPHAN -- it drew once, was followed, and never came back. A high orphan
			// count with a healthy turn is the signature of "witnesses do not survive", which is one
			// of the two causes the old log could not distinguish from "the user did not turn".
			bool ever_paired = false;
		};

		struct camtest_candidate
		{
			u64 key = 0;

			// Identity, printed so a row can be matched to the candidate lines in the matrix dump.
			u8 source = 0;
			u32 mem_offset = 0;
			u32 start_pc = 0;
			u64 ucode_hash = 0;
			const char* hypothesis = "";
			bool column_major = false;
			float fov_y = 0.f;

			// The depth treatment the hypothesis carried (PCSX2_REMIX_CAMDEPTH). `w_divisor` is
			// the factor the WHOLE matrix was divided by before the screen normalisation, i.e.
			// the measured |column 3 xyz| -- 1 at the default. Printed beside depth_scale
			// because the two answer different questions: depth_scale is what the recovered
			// world's unit came out as AFTER the correction (which is what CAMSCALE judges),
			// w_divisor is how big the correction had to be, and a divisor of 1.4e7 is itself
			// the finding.
			float w_divisor = 1.f;
			bool swap_zw = false;

			// Content hash of the RAW 16 floats, before any hypothesis was composed in. This is the
			// degeneracy detector: if every row of the table shares one content hash, the whole
			// ranked table is one matrix wearing several hats and no amount of re-ranking it will
			// produce a camera. Reported as a distinct-count in the round header.
			u64 content_hash = 0;

			// This triple never cleared split_view_projection_direct + score_perspective, so it
			// exists in the table only because CAMTESTALL put it there. Printed, because a low-drift
			// row that cannot split is a different finding from a low-drift row that can.
			bool raw_only = false;

			// The window each solver un-projected, and the solver itself. Stamped at resolve time
			// with s_frame_counter + 1, because a camera resolved at the bottom of window k is the
			// one window k+1's draws are un-projected with.
			u64 now_frame = 0;
			u64 prev_frame = 0;
			bool prev_valid = false;
			remix_ps2::clip_solver now_solver{};
			remix_ps2::clip_solver prev_solver{};
			float now_eye[3] = {};
			float prev_eye[3] = {};
			float now_forward[3] = {};
			float prev_forward[3] = {};

			// |un-projected NDC centre at w = 1, minus the eye|. It is 1 for a camera whose recovered
			// unit is the guest's own, which is what resolve's depth-scale gate already demands. The
			// drift is divided by it so one camera emitted at two column scales ranks the same, and a
			// candidate outside that gate's window is reported but is not electable.
			float depth_scale = 0.f;
			bool eligible = false;

			float history[camtest_history] = {};
			u32 history_count = 0;
			u32 history_next = 0;
			u64 pairs = 0;
			u32 last_witnesses = 0;

			// How many of those witnesses actually reached the ALPHA median. The accumulator
			// skips any witness whose recovered range is under 1e-3, and that threshold is
			// ABSOLUTE while the recovered space is not: at this title's dscale of 7.232e-08 a
			// witness has to be ~1.4e4 guest units from the eye to clear it, so alpha may be
			// being computed over the handful of most distant draws in the frame rather than
			// over all of them. Printed beside `wit` because "alpha 1.477 over 40 witnesses" and
			// "alpha 1.477 over 2" are not the same claim, and nothing said which it was.
			u32 last_alpha_witnesses = 0;
			float last_drift = 0.f;
			float last_centroid_drift = 0.f;

			// drift / (range from the eye x the eye's rotation in radians). Dimensionless, and its
			// value under each hypothesis is known BEFORE any data exists: ~0 if this matrix is the
			// camera, ~1 if the recovered world rides the eye, ~2 if it is mirrored about a
			// camera-fixed plane. Those are WORLDFIX's three alphas, read off absolute recovered
			// positions instead of bearings -- which is why eye translation does not have to be
			// rejected here, and why the number is comparable between titles and world scales.
			float last_alpha = 0.f;
		};

		// The constants that turn a guest vertex back into the clip triple the vertex loop
		// un-projected. Passed rather than recomputed so the two can never disagree.
		struct camtest_clip_map
		{
			float scale_x = 0.f;
			float scale_y = 0.f;
			float offset_x = 0.f;
			float offset_y = 0.f;
			double z_scale = 0.0;
			double z_a = 0.0;
			double z_b = 0.0;
			bool screen_ui = false;
			bool z_depth = false;
		};

		struct camtest_screen_box
		{
			float min_x = 0.f;
			float max_x = 0.f;
			float min_y = 0.f;
			float max_y = 0.f;
			int width = 0;
			int height = 0;
		};

		camtest_witness s_camtest_witnesses[camtest_witness_slots];
		camtest_candidate s_camtest_candidates[camtest_candidate_slots];

		// The mode the DRAW path tests, refreshed from the environment once per presented window by
		// camtest_evaluate(). The draw path must not read the environment itself: this title submits
		// ~830 draws a window.
		int s_camtest_active = 0;

		// PCSX2_REMIX_CAMTESTALL, refreshed beside s_camtest_active and read by the resolve loop.
		// Never read from the draw path -- CAMTESTALL changes nothing about how a draw is sampled.
		int s_camtest_all_active = 0;
		int s_camtest_all_logged_mode = -1;

		// How many triples were actually handed to the drift test in the window that just ended,
		// so the round header can answer "how many matrices per window got scored?" with a number
		// instead of a capacity. Split three ways because the three populations answer different
		// questions: `split` is what the shape filters would have allowed, `raw` is what they were
		// rejecting, and `probe` is the neighbourhood the slicer never reads at all.
		u32 s_camtest_noted_split = 0;
		u32 s_camtest_noted_raw = 0;
		u32 s_camtest_noted_probe = 0;

		int s_camtest_logged_mode = -1;
		bool s_camtest_started = false;
		u64 s_camtest_skipped = 0;
		u64 s_camtest_elected_key = 0;

		// The ANCHOR: the older window every later window is paired against, held for up to
		// camtest_span_windows() so the turn can accumulate into a usable lever arm. Named `prev`
		// throughout for continuity with the fields it moves in lockstep with (witness.prev_*,
		// candidate.prev_*), all of which are now anchor state rather than last-window state.
		u64 s_camtest_prev_window = 0;
		bool s_camtest_prev_valid = false;
		float s_camtest_prev_eye[3] = {};
		float s_camtest_prev_forward[3] = {};

		// Up = the next window that carries geometry becomes the new anchor. Read by camtest_sample
		// on the DRAW path (so witnesses capture during that window) and consumed by
		// camtest_evaluate at the resolve that ends it (so the solvers and the eye capture with
		// them). One flag for all three halves, because an anchor that is not simultaneous is not
		// an anchor.
		bool s_camtest_anchor_arm = true;
		bool s_camtest_cycle_paired = false; // this anchor cycle has produced at least one pair
		u64 s_camtest_anchor_cycles = 0;
		u64 s_camtest_reanchor_harvest = 0; // cycles that ran their full span having produced pairs
		u64 s_camtest_reanchor_span = 0; // re-anchored because the span ran out (turn too slow)
		u64 s_camtest_reanchor_jump = 0; // re-anchored on an over-cap turn: a cut, not a turn

		u32 s_camtest_inside_draws = 0; // draws this window that cleared the strictly-inside rule
		u64 s_camtest_clipped_draws = 0; // draws rejected for touching a viewport edge
		u64 s_camtest_pairs = 0;
		u64 s_camtest_reject_turn = 0; // the eye did not turn enough for the pair to mean anything
		u64 s_camtest_reject_jump = 0; // the eye "turned" further than any real turn: a cut or glitch
		u64 s_camtest_witness_overflow = 0; // draws dropped: every witness slot already used this window
		u64 s_camtest_orphans = 0; // witnesses evicted having never once paired
		u64 s_camtest_evictions = 0; // candidate identities evicted WITH a drift history attached
		u32 s_camtest_rounds_done = 0;

		// ---- heartbeat ---------------------------------------------------------------------
		// Everything here exists to answer ONE question that the old log could not: when no round
		// fires, is it because the eye never turned, or because no witness ever survived into a
		// second window? Those need opposite responses -- "turn more" versus "the sampler is
		// broken" -- and they were reported by the same sentence.
		u64 s_camtest_hb_window = 0; // window the last heartbeat was emitted at
		u32 s_camtest_hb_windows = 0; // windows observed since then
		double s_camtest_hb_inside_sum = 0.0;
		u32 s_camtest_hb_inside_max = 0;
		u32 s_camtest_hb_survivors_last = 0; // witnesses that paired into the most recent window
		u32 s_camtest_hb_survivors_max = 0;
		double s_camtest_hb_turn_sum = 0.0;
		double s_camtest_hb_turn_max = 0.0; // THE number that separates the two causes
		u32 s_camtest_hb_turn_count = 0;
		u64 s_camtest_hb_pairs_at = 0;
		u64 s_camtest_hb_noturn_at = 0;
		u64 s_camtest_hb_jump_at = 0;
		u64 s_camtest_hb_clipped_at = 0;
		u64 s_camtest_hb_overflow_at = 0;
		u64 s_camtest_hb_orphans_at = 0;

		u32 s_camtest_round = 0;
		double s_camtest_round_turn = 0.0;
		double s_camtest_round_move = 0.0;
		double s_camtest_round_witnesses = 0.0;
		double s_camtest_round_inside = 0.0;
		u32 s_camtest_round_witness_min = 0;
		double s_camtest_round_scored = 0.0;
		double s_camtest_round_scored_split = 0.0;
		double s_camtest_round_scored_probe = 0.0;

		// The lever arm actually achieved, reported on the round line and the heartbeat. Without it
		// there is no way to tell a readable table from an unreadable one until the alphas are
		// already nonsense.
		double s_camtest_round_turn_min = 0.0;
		double s_camtest_round_turn_max = 0.0;
		double s_camtest_round_span = 0.0;
		u32 s_camtest_round_span_max = 0;
		double s_camtest_hb_pair_turn_sum = 0.0;
		double s_camtest_hb_pair_turn_max = 0.0;
		double s_camtest_hb_pair_span_sum = 0.0;
		u32 s_camtest_hb_pair_count = 0;

		float camtest_median(float* values, u32 count)
		{
			if (count == 0)
				return 0.f;

			std::sort(values, values + count);
			return (count & 1u) ? values[count / 2]
							    : (0.5f * (values[(count / 2) - 1] + values[count / 2]));
		}

		// One (candidate, majorness, hypothesis) triple -- i.e. exactly how a candidate would be
		// USED if it won, which is what has to be scored. The address the matrix came from and the
		// microprogram that wrote it are both in the key: a different ucode at the same offset is a
		// different quantity, not the same camera.
		u64 camtest_identity(const RemixVU1Capture::Candidate& candidate, bool column_major,
			const char* hypothesis)
		{
			u64 key = fnv_mix(fnv_seed, candidate.source);
			key = fnv_mix(key, candidate.mem_offset);
			key = fnv_mix(key, candidate.start_pc);
			key = fnv_mix(key, candidate.ucode_hash);
			key = fnv_mix(key, column_major ? 1u : 0u);

			for (const char* c = hypothesis; c && *c; ++c)
				key = fnv_mix(key, static_cast<u64>(static_cast<unsigned char>(*c)));

			return (key == 0) ? 1 : key;
		}

		// Draw-side sampler. Same call site and same gating as worldfix_sample -- accepted,
		// world-mode, non-sky draws -- plus the clipping defence this test depends on.
		//
		// It stores CLIP samples, not world positions, precisely because clip is the guest's own
		// output and carries no camera in it: the same stored samples can then be un-projected with
		// every candidate, which is the whole point of the test.
		void camtest_sample(u64 content_hash, u32 vertex_count, u32 index_count,
			const GSVertex* verts, const camtest_clip_map& map, const camtest_screen_box& box)
		{
			if (!verts || vertex_count == 0 || box.width <= 0 || box.height <= 0)
				return;

			// DEFENCE 2. A draw whose screen box reaches an edge may have been clipped, so its
			// vertex set is not provably the same set next window even when the counts match. One
			// pixel of margin rather than zero: the box is built from 12.4 fixed-point coordinates
			// and a shape resting exactly on the boundary must not qualify.
			constexpr float margin = 1.f;
			if (!(box.min_x > margin) || !(box.max_x < (static_cast<float>(box.width) - margin)) ||
				!(box.min_y > margin) || !(box.max_y < (static_cast<float>(box.height) - margin)))
			{
				++s_camtest_clipped_draws;
				return;
			}

			// DEFENCE 1. No positional term anywhere in the key, so the thing being measured cannot
			// move it, and the counts change the instant the drawn set does.
			const u64 key = fnv_mix(fnv_mix(content_hash, vertex_count), index_count);

			camtest_witness* slot = nullptr;
			camtest_witness* oldest = nullptr;

			for (camtest_witness& candidate : s_camtest_witnesses)
			{
				if (candidate.key == key)
					slot = &candidate;

				// A SLOT ALREADY REFRESHED IN THIS WINDOW IS NOT EVICTABLE. The old rule kept the
				// first slot holding the minimum `frame`, and once every slot had been written this
				// window they all held the same maximum -- so `candidate.frame < oldest->frame` was
				// never true, `oldest` stayed pinned at slot 0, and every further draw in the window
				// clobbered it. Worse, the slots it did evict were this window's own witnesses,
				// destroying the `now` half of a pair that was already half-formed. A draw that
				// finds no evictable slot is now DROPPED and counted, which is a visible ceiling
				// instead of a silent one.
				if (candidate.frame != s_frame_counter &&
					(!oldest || candidate.frame < oldest->frame))
					oldest = &candidate;
			}

			if (!slot)
			{
				if (!oldest)
				{
					++s_camtest_witness_overflow;
					return;
				}

				// Evicted having never paired: it drew once and never came back, or the table is
				// churning faster than the geometry repeats. Either way it is the number that says
				// "witnesses do not survive" independently of whether the eye turned.
				if (oldest->key != 0 && !oldest->ever_paired)
					++s_camtest_orphans;

				slot = oldest;
				*slot = camtest_witness{};
				slot->key = key;
			}
			else if (slot->frame == s_frame_counter)
			{
				// Two draws in one window with an identical fingerprint are two instances of one
				// shape, not one object seen twice. Keep the first, so which one is kept cannot
				// depend on submission order.
				return;
			}

			++s_camtest_inside_draws;

			slot->frame = s_frame_counter;

			// Fixed stride over a vertex count the key has pinned, so sample i is the same vertex in
			// both windows. Reproduces the vertex loop's own arithmetic verbatim rather than
			// approximating it -- these are the exact values that were un-projected.
			const u32 stride = std::max(1u, vertex_count / camtest_samples);
			u32 count = 0;

			for (u32 i = 0; i < vertex_count && count < camtest_samples; i += stride)
			{
				const GSVertex& v = verts[i];

				const float q = map.screen_ui ? 1.f : (map.z_depth ?
					static_cast<float>((static_cast<double>(v.XYZ.Z) * map.z_scale * map.z_a) + map.z_b) :
					v.RGBAQ.Q);
				const float w = 1.0f / q;

				if (!std::isfinite(w) || !(w > 0.f))
				{
					count = 0;
					break;
				}

				const float ndc_x = ((static_cast<float>(v.XYZ.X) - 0.05f) * map.scale_x) - map.offset_x;
				const float ndc_y = -(((static_cast<float>(v.XYZ.Y) - 0.05f) * map.scale_y) - map.offset_y);

				slot->now[count][0] = ndc_x * w;
				slot->now[count][1] = ndc_y * w;
				slot->now[count][2] = w;
				++count;
			}

			slot->count = count;

			// ANCHOR CAPTURE. `prev` is no longer "the last window this witness drew in" -- it is
			// the ANCHOR, and it only moves on a window the anchor arm is up for. Done AFTER the
			// samples are written, not before, because the anchor has to be THIS window's geometry:
			// the old shift copied the PREVIOUS window's samples, which is the right thing to do
			// only when every window is an anchor.
			//
			// camtest_evaluate consumes the arm at the resolve that ends this same window, so the
			// witness samples, the candidate solvers and the window's eye all anchor together. A
			// witness that draws in several armed windows before the arm is consumed simply
			// re-captures, which keeps it in step rather than stranding it.
			if (s_camtest_anchor_arm)
			{
				slot->prev_frame = slot->frame;
				slot->prev_count = count;
				std::memcpy(slot->prev, slot->now, sizeof(slot->now));
				slot->prev_valid = (count != 0);
			}
		}

		// Find-or-allocate, shared by both note paths so one key can never occupy two slots with
		// two histories.
		//
		// WAS A LINEAR SCAN of the used prefix, and that had to go before the table could grow.
		// The scan is O(used) per call and is called once per (candidate x majorness x
		// hypothesis) triple, i.e. O(triples x used) per window -- quadratic in exactly the two
		// numbers this change increases. At the old 2048 slots and ~1300 triples it was already
		// touching ~170 KB per call; at 8192 slots and 2604 triples it would have been ~3.5 GB of
		// pointer-chasing per window, tens of milliseconds on the GS thread -- and at today's
		// 16384 slots against 5208 triples, four times that again. That is not merely
		// slow: CAMTESTALL's own note says GS-thread cost moves frame pacing, frame pacing moves
		// how far the eye turns between two windows, and the turn is alpha's denominator. The
		// instrument would have been perturbing its own input.
		//
		// So: an open-addressed index, 4x the slot count (power of two, load factor <= 0.25,
		// linear probing stays a step or two). Two sentinels -- EMPTY, which terminates a probe,
		// and DEAD, which does not, so a key evicted out of the middle of a cluster cannot hide
		// the keys behind it. Insert only ever runs after a miss, so reusing a DEAD entry can
		// never duplicate a live key.
		u32 s_camtest_used = 0;

		constexpr u32 camtest_index_slots = 4 * camtest_candidate_slots;
		constexpr u32 camtest_index_empty = 0xFFFFFFFFu;
		constexpr u32 camtest_index_dead = 0xFFFFFFFEu;

		u32 s_camtest_index[camtest_index_slots];
		bool s_camtest_index_ready = false;

		// LRU replaced by a CLOCK HAND, for the same reason: finding the globally-oldest slot is
		// another O(used) scan. The hand advances one slot per allocation and takes the first slot
		// it lands on that was not written during the window being resolved, which is the property
		// that actually matters (evicting a slot this window already noted would destroy the `now`
		// half of a pair that is half-formed). At 16384 slots against 5208 triples the hand cannot
		// lap a window, so that guard never has to fire more than a few steps -- the ratio is 3.1x,
		// unchanged from the 8192-against-2604 it was sized at, which is why doubling the family
		// and doubling the table had to happen together.
		u32 s_camtest_hand = 0;

		void camtest_index_reset()
		{
			for (u32& entry : s_camtest_index)
				entry = camtest_index_empty;

			s_camtest_index_ready = true;
			s_camtest_hand = 0;
		}

		u32 camtest_index_home(u64 key)
		{
			return static_cast<u32>(key ^ (key >> 32)) & (camtest_index_slots - 1);
		}

		camtest_candidate* camtest_index_find(u64 key)
		{
			u32 i = camtest_index_home(key);

			for (u32 probe = 0; probe < camtest_index_slots; ++probe)
			{
				const u32 entry = s_camtest_index[i];

				if (entry == camtest_index_empty)
					return nullptr;

				if (entry != camtest_index_dead && s_camtest_candidates[entry].key == key)
					return &s_camtest_candidates[entry];

				i = (i + 1) & (camtest_index_slots - 1);
			}

			return nullptr;
		}

		void camtest_index_insert(u64 key, u32 slot)
		{
			u32 i = camtest_index_home(key);

			for (u32 probe = 0; probe < camtest_index_slots; ++probe)
			{
				const u32 entry = s_camtest_index[i];

				if (entry == camtest_index_empty || entry == camtest_index_dead)
				{
					s_camtest_index[i] = slot;
					return;
				}

				i = (i + 1) & (camtest_index_slots - 1);
			}
		}

		void camtest_index_erase(u64 key)
		{
			u32 i = camtest_index_home(key);

			for (u32 probe = 0; probe < camtest_index_slots; ++probe)
			{
				const u32 entry = s_camtest_index[i];

				if (entry == camtest_index_empty)
					return;

				if (entry != camtest_index_dead && s_camtest_candidates[entry].key == key)
				{
					s_camtest_index[i] = camtest_index_dead;
					return;
				}

				i = (i + 1) & (camtest_index_slots - 1);
			}
		}

		camtest_candidate* camtest_slot_for(u64 key)
		{
			if (!s_camtest_index_ready)
				camtest_index_reset();

			if (camtest_candidate* const found = camtest_index_find(key))
				return found;

			u32 slot = 0;

			if (s_camtest_used < camtest_candidate_slots)
			{
				slot = s_camtest_used++;
			}
			else
			{
				// Clock. `now_frame` is stamped s_frame_counter + 1 by both note paths, so a slot
				// written during the window being resolved is exactly the one that must survive.
				const u64 protect = s_frame_counter + 1;

				for (u32 step = 0; step < camtest_candidate_slots; ++step)
				{
					slot = s_camtest_hand;
					s_camtest_hand = (s_camtest_hand + 1) % camtest_candidate_slots;

					if (s_camtest_candidates[slot].now_frame != protect)
						break;
				}

				camtest_candidate& victim = s_camtest_candidates[slot];

				if (victim.key != 0)
				{
					camtest_index_erase(victim.key);

					if (victim.history_count > 0)
						++s_camtest_evictions; // an identity lost measured drift -- the heartbeat reports it
				}
			}

			s_camtest_candidates[slot] = camtest_candidate{};
			s_camtest_candidates[slot].key = key;
			camtest_index_insert(key, slot);
			return &s_camtest_candidates[slot];
		}

		// PCSX2_REMIX_CAMTESTALL's note. Everything the drift test needs, derived from the
		// NORMALISED FUSED MATRIX ALONE -- no split, no perspective classification, no shape score.
		//
		// That is not a shortcut, it is the point. make_clip_solver inverts the 3x3 built from
		// columns {0, 1, 3} of the fused matrix, and that inverse IS the un-projection the vertex
		// loop runs; nothing about it needs the matrix to look like a projection. The eye falls out
		// of the same solve: the camera is by definition the world point whose clip x, y and w all
		// vanish, i.e. solve_world_position(solver, 0, 0, 0) -- which is literally the system
		// solve_camera_position() solves inside try_split_once. So a triple that DOES split gets a
		// bit-identical eye from either path and the two can never disagree.
		//
		// The one rejection kept is singularity, and it is not a heuristic: a matrix whose clip
		// solver does not invert un-projects nothing, so there is no recovered world to measure the
		// drift of.
		bool camtest_note_raw(u64 key, u8 source, u32 mem_offset, u32 start_pc, u64 ucode_hash,
			u64 content_hash, bool column_major, const char* hypothesis, float w_divisor,
			bool swap_zw, const remix_ps2::mat4& normalized)
		{
			remix_ps2::clip_solver solver{};
			if (!remix_ps2::make_clip_solver(normalized, solver))
				return false;

			float eye[3];
			remix_ps2::solve_world_position(solver, 0.f, 0.f, 0.f, eye);

			float probe[3];
			remix_ps2::solve_world_position(solver, 0.f, 0.f, 1.f, probe);

			float forward[3] = {probe[0] - eye[0], probe[1] - eye[1], probe[2] - eye[2]};
			const float scale =
				std::sqrt((forward[0] * forward[0]) + (forward[1] * forward[1]) + (forward[2] * forward[2]));

			if (!std::isfinite(eye[0]) || !std::isfinite(eye[1]) || !std::isfinite(eye[2]) ||
				!std::isfinite(scale) || !(scale > 1e-12f))
				return false;

			camtest_candidate* const slot = camtest_slot_for(key);

			slot->source = source;
			slot->mem_offset = mem_offset;
			slot->start_pc = start_pc;
			slot->ucode_hash = ucode_hash;
			slot->content_hash = content_hash;
			slot->hypothesis = hypothesis;
			slot->column_major = column_major;
			slot->w_divisor = w_divisor;
			slot->swap_zw = swap_zw;

			// Cleared again by camtest_note_candidate if this same triple also splits and scores.
			slot->raw_only = true;
			slot->fov_y = 0.f;

			slot->now_frame = s_frame_counter + 1;
			slot->now_solver = solver;

			const float inv = 1.f / scale;
			for (u32 k = 0; k < 3; ++k)
			{
				slot->now_eye[k] = eye[k];
				slot->now_forward[k] = forward[k] * inv;
			}

			// Same eligibility gate as the split path, and the same quantity: |un-projected NDC
			// centre at w = 1, minus the eye|. Reported either way; only mode 2's election reads it.
			//
			// MEASURED POST-NORMALISATION, and structurally so rather than by remembering to: the
			// solver above is built from `normalized`, which apply_camera_hypothesis has already
			// put through normalize_clip_depth. So a hypothesis that divides the matrix by 1.4e7
			// is judged on the ~1 it recovers, not on the 7e-08 it started from. Getting this
			// backwards would have hidden precisely the candidate PCSX2_REMIX_CAMDEPTH exists to
			// bring into range.
			const float limit = camera_scale_limit();
			slot->depth_scale = scale;
			slot->eligible = (scale >= (1.f / limit)) && (scale <= limit);

			if (source == 5)
				++s_camtest_noted_probe;
			else
				++s_camtest_noted_raw;

			return true;
		}

		// CAMTESTALL = 2. Scores the 64-qword VU1 neighbourhood RemixVU1Capture snapshots around the
		// back-sliced object MVP, as 61 overlapping 4-qword windows.
		//
		// THE HYPOTHESIS. For row-vectors an object's matrix is M_obj * VP, so every object in a
		// frame shares the right factor VP -- and level geometry conventionally has M_obj = identity,
		// in which case its MVP *is* VP. The back-slice resolves an object MVP at VI05+7..10
		// (RemixVU1Capture.h). If the shared VP is uploaded to a VU1 address that no LQ in the
		// sliced chain reads, the slicer never publishes it and no amount of re-ranking the
		// published set can reach it. The transform probe exists to hold exactly that
		// neighbourhood, and until now it was only dumped as text.
		//
		// SCORING ONLY, and structurally so: these synthetic candidates are not in frame.items[],
		// so the election loop never enumerates their keys and camtest_ranked_drift is never asked
		// about them. They cannot be elected under any mode.
		void camtest_scan_probe(const RemixVU1Capture::Frame& frame, const viewport_constants& vp,
			float reference_aspect)
		{
			if (!frame.transform_probe_valid)
				return;

			constexpr u32 probe_qwords = 64;
			constexpr u32 probe_windows = probe_qwords - 3; // a matrix is 4 consecutive qwords

			// De-duplicated by content within the sweep: long runs of VU1 memory are zeros or
			// repeats, and a duplicate costs a table slot for nothing.
			u64 seen[probe_windows];
			u32 seen_count = 0;

			for (u32 base = 0; base < probe_windows; ++base)
			{
				remix_ps2::mat4 raw{};
				std::memcpy(&raw.m[0][0], &frame.transform_probe[base * 4], sizeof(float) * 16);

				if (!remix_ps2::mat4_is_finite(raw))
					continue;

				const u64 content = hash_floats(&raw.m[0][0], 16);

				bool duplicate = false;
				for (u32 i = 0; i < seen_count && !duplicate; ++i)
					duplicate = (seen[i] == content);

				if (duplicate)
					continue;

				seen[seen_count++] = content;

				// The address this window really sits at in VU1 data memory, so a winner can be
				// found again: the probe is a ring-wrapped copy starting at transform_probe_base.
				const u32 offset = ((frame.transform_probe_base + base) & 0x3FFu) * 16u;

				RemixVU1Capture::Candidate synthetic{};
				std::memcpy(synthetic.m, &raw.m[0][0], sizeof(synthetic.m));
				synthetic.mem_offset = offset;
				synthetic.start_pc = 0;
				synthetic.ucode_hash = frame.transform_probe_ucode;
				synthetic.source = 5; // "probe" -- see camtest_report's source names

				for (u32 major = 0; major < 2; ++major)
				{
					const remix_ps2::mat4 oriented = (major == 0) ? raw : remix_ps2::mat4_transpose(raw);

					camera_hypothesis hypotheses[max_camera_hypotheses];
					const u32 hypothesis_count = build_camera_hypotheses(oriented, vp,
						reference_aspect, major != 0, synthetic.source, synthetic.ucode_hash,
						s_camdepth_active, s_camdepth_snap_active, s_camyflip_active, hypotheses);

					for (u32 h = 0; h < hypothesis_count; ++h)
					{
						const camera_hypothesis& hyp = hypotheses[h];
						const remix_ps2::mat4 normalized = apply_camera_hypothesis(oriented, hyp);

						camtest_note_raw(camtest_identity(synthetic, major != 0, hyp.name),
							synthetic.source, offset, synthetic.start_pc, synthetic.ucode_hash,
							content, major != 0, hyp.name, hyp.scale_w, hyp.swap_zw, normalized);
					}
				}
			}
		}

		// Candidate-side capture, called from inside resolve_world_camera's own hypothesis loop so
		// the solver recorded here is built by the SAME machinery, on the same normalised matrix, as
		// the one the election would install. Nothing is re-implemented and the two cannot drift.
		void camtest_note_candidate(u64 key, const RemixVU1Capture::Candidate& candidate,
			bool column_major, const char* hypothesis, float w_divisor, bool swap_zw,
			u64 content_hash, const remix_ps2::mat4& normalized, const remix_ps2::vp_split& split)
		{
			remix_ps2::clip_solver solver{};
			remix_ps2::mat4 view_to_world{};

			if (!remix_ps2::make_clip_solver(normalized, solver) ||
				!remix_ps2::mat4_invert(split.view, view_to_world))
				return;

			camtest_candidate* const slot = camtest_slot_for(key);

			++s_camtest_noted_split;
			slot->content_hash = content_hash;
			slot->raw_only = false;
			slot->source = candidate.source;
			slot->mem_offset = candidate.mem_offset;
			slot->start_pc = candidate.start_pc;
			slot->ucode_hash = candidate.ucode_hash;
			slot->hypothesis = hypothesis;
			slot->column_major = column_major;
			slot->w_divisor = w_divisor;
			slot->swap_zw = swap_zw;

			remix_ps2::projection_params params{};
			slot->fov_y = remix_ps2::describe_projection(split.projection, params) ? params.fov_y_degrees : 0.f;

			slot->now_frame = s_frame_counter + 1;
			slot->now_solver = solver;

			for (u32 k = 0; k < 3; ++k)
			{
				slot->now_eye[k] = view_to_world.m[3][k];
				slot->now_forward[k] = split.view.m[k][2];
			}

			float probe[3];
			remix_ps2::solve_world_position(solver, 0.f, 0.f, 1.f, probe);

			const float dx = probe[0] - slot->now_eye[0];
			const float dy = probe[1] - slot->now_eye[1];
			const float dz = probe[2] - slot->now_eye[2];
			const float scale = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
			const float limit = camera_scale_limit();

			slot->depth_scale = scale;
			slot->eligible = std::isfinite(scale) && (scale >= (1.f / limit)) && (scale <= limit);
		}

		// Ages every tracked candidate one window. Called once per resolve, after the evaluation has
		// consumed the current pair and before the loop writes the new solvers.
		void camtest_roll()
		{
			for (camtest_candidate& slot : s_camtest_candidates)
			{
				if (slot.key == 0)
					continue;

				slot.prev_valid = (slot.now_frame != 0);
				slot.prev_frame = slot.now_frame;
				slot.prev_solver = slot.now_solver;

				for (u32 k = 0; k < 3; ++k)
				{
					slot.prev_eye[k] = slot.now_eye[k];
					slot.prev_forward[k] = slot.now_forward[k];
				}
			}
		}

		// Mode 2's ranking input. False means "not enough evidence to elect on", which is the
		// documented fallback to the shape election rather than a refusal to elect at all.
		bool camtest_ranked_drift(u64 key, float& out)
		{
			for (camtest_candidate& slot : s_camtest_candidates)
			{
				if (slot.key != key)
					continue;

				if (!slot.eligible || slot.history_count < camtest_min_pairs ||
					slot.last_witnesses < camtest_min_witnesses)
					return false;

				float scratch[camtest_history];
				std::memcpy(scratch, slot.history, sizeof(float) * slot.history_count);
				out = camtest_median(scratch, slot.history_count);
				return std::isfinite(out);
			}

			return false;
		}

		void camtest_report(u64 window)
		{
			struct camtest_row
			{
				const camtest_candidate* entry;
				float drift;

				// Precomputed in one pass after the sort rather than inside the print loop, because
				// the per-source summary below needs them for rows the print loop never reaches.
				double turn;
				bool frozen;
			};

			// static, not a local: camtest_row is 32 bytes after padding, so at
			// camtest_candidate_slots = 16384 this is a 512 KB array and the GS thread's stack is
			// not the place for it. Called from one thread, once per round. (The figure here read
			// "128 KB at 8192 slots" and was wrong in both terms even then -- 8192 x 32 is 256 KB.)
			static camtest_row rows[camtest_candidate_slots];
			u32 count = 0;

			// THE DEGENERACY CHECK. Distinct RAW matrices behind the ranked rows, and distinct VU1
			// addresses. If the table is 200 rows over ONE content hash, every row is the same
			// matrix under a different hypothesis and the ranking is decorative -- which is exactly
			// the shape of the two-row result that motivated CAMTESTALL. Counted with a small
			// bounded set: 64 is far more variety than a degenerate answer would ever show, and the
			// count saturates rather than lying.
			constexpr u32 distinct_cap = 64;
			u64 distinct_content[distinct_cap];
			u32 distinct_offsets[distinct_cap];
			u32 distinct_content_count = 0;
			u32 distinct_offset_count = 0;
			bool distinct_content_saturated = false;
			bool distinct_offset_saturated = false;

			for (const camtest_candidate& slot : s_camtest_candidates)
			{
				if (slot.key == 0 || slot.history_count == 0)
					continue;

				float scratch[camtest_history];
				std::memcpy(scratch, slot.history, sizeof(float) * slot.history_count);
				rows[count].entry = &slot;
				rows[count].drift = camtest_median(scratch, slot.history_count);
				++count;

				bool seen = false;
				for (u32 i = 0; i < distinct_content_count && !seen; ++i)
					seen = (distinct_content[i] == slot.content_hash);

				if (!seen)
				{
					if (distinct_content_count < distinct_cap)
						distinct_content[distinct_content_count++] = slot.content_hash;
					else
						distinct_content_saturated = true;
				}

				seen = false;
				for (u32 i = 0; i < distinct_offset_count && !seen; ++i)
					seen = (distinct_offsets[i] == slot.mem_offset);

				if (!seen)
				{
					if (distinct_offset_count < distinct_cap)
						distinct_offsets[distinct_offset_count++] = slot.mem_offset;
					else
						distinct_offset_saturated = true;
				}
			}

			std::sort(rows, rows + count, [](const camtest_row& a, const camtest_row& b) {
				return a.drift < b.drift;
			});

			const double inv_round = (s_camtest_round > 0) ? (1.0 / static_cast<double>(s_camtest_round)) : 0.0;

			INFO_LOG("Remix: CAMTEST round at frame {} -- {} qualifying pairs, {:.1f} witnesses per "
					 "pair (min {}), {:.1f} witness draws per window, mean eye move {:.1f}. "
					 "LEVER ARM: turn per pair mean {:.2f} deg, min {:.2f}, max {:.2f}, over a window "
					 "span of mean {:.1f} max {} (cap {}). Session: {} pairs, rejected {} for no turn "
					 "and {} as a jump; {} draws dropped for touching a viewport edge. Drift is the "
					 "median over witnesses of how far a static draw's recovered world position moved "
					 "between the two windows, per unit of guest depth. ALPHA is the scale-free "
					 "reading of the same thing: ~0 = this matrix IS the camera, ~1 = the recovered "
					 "world rides the eye, ~2 = it is mirrored about a camera-fixed plane. READ THE "
					 "LEVER ARM FIRST: alpha divides by the turn, so under ~1 deg the ranking is "
					 "noise and a frozen matrix wins it for free.",
				window, s_camtest_round, s_camtest_round_witnesses * inv_round,
				s_camtest_round_witness_min, s_camtest_round_inside * inv_round,
				s_camtest_round_move * inv_round,
				s_camtest_round_turn * inv_round, s_camtest_round_turn_min, s_camtest_round_turn_max,
				s_camtest_round_span * inv_round, s_camtest_round_span_max, camtest_span_windows(),
				s_camtest_pairs, s_camtest_reject_turn, s_camtest_reject_jump,
				s_camtest_clipped_draws);

			// Second header line: WHAT WAS SCORED, and how much of it is actually different.
			// Read 'distinct matrices' first. 1 or 2 means the ranked table below is one matrix
			// under several hypotheses and the true view-projection is not in the set at all --
			// go to the probe sweep (PCSX2_REMIX_CAMTESTALL = 2), not to more re-ranking.
			// The family actually enumerated this window. Was
			// `(s_camdepth_active == 0) ? hypothesis_slots : max_camera_hypotheses`, which stopped
			// being the emitted count the moment max_camera_hypotheses grew a Y-flip dimension --
			// a header that overstates the family makes the "scored per window approaches capacity"
			// warning below unreadable, which is the one line that says the table is thrashing.
			const u32 camtest_family_size = ((s_camdepth_active == 0) ? 1u : 2u) *
											hypothesis_slots * ((s_camyflip_active > 0) ? 2u : 1u);

			INFO_LOG("Remix: CAMTEST set -- PCSX2_REMIX_CAMDEPTH = {} / CAMYFLIP = {} ({} hypotheses "
					 "per candidate x majorness), PCSX2_REMIX_CAMTESTALL = {} ({}). {} ranked rows over {}{} "
					 "distinct matrices at {}{} distinct VU1 addresses. Scored per window: {:.0f} "
					 "triples ({:.0f} published-candidate, {:.0f} probe-window); of those {:.0f} also "
					 "cleared split+score, which is all the table held before CAMTESTALL. Table "
					 "capacity {} -- if 'scored per window' approaches it, rows are being evicted and "
					 "their histories reset.",
				s_camdepth_active, s_camyflip_active, camtest_family_size,
				s_camtest_all_active,
				(s_camtest_all_active == 0) ? "split+score survivors only" :
					((s_camtest_all_active == 1) ? "every published candidate" :
						"every published candidate + VU1 probe neighbourhood"),
				count, distinct_content_saturated ? ">=" : "", distinct_content_count,
				distinct_offset_saturated ? ">=" : "", distinct_offset_count,
				s_camtest_round_scored * inv_round,
				(s_camtest_round_scored - s_camtest_round_scored_probe) * inv_round,
				s_camtest_round_scored_probe * inv_round,
				s_camtest_round_scored_split * inv_round,
				camtest_candidate_slots);

			// Rows printed. Wider under CAMTESTALL because the whole point is to see past the two
			// rows the shape filters were leaving; the elected row is always printed on top of it.
			// Now a live knob, because 32 of 4,569 was hiding every row worth reading -- see
			// camtest_row_limit().
			const u32 row_limit = (s_camtest_all_active > 0) ? camtest_row_limit() : 8u;
			const int source_filter = camtest_source_filter();

			// FROZEN. A candidate whose own recovered forward barely rotated over the pair cannot be
			// the camera when the real camera turned several degrees -- it is a matrix that stopped
			// updating, and a matrix that does not move has no drift for free. These dominated the
			// top of the first real table (own-turn 0.00-0.03 deg, depth scale 3.7e5) and made the
			// ranking unreadable. They are TAGGED and still printed, never hidden: a suppressed row
			// cannot be checked, and the whole point of this instrument is that nothing is decided
			// by a filter. The summary line below reports the best row that is neither frozen nor
			// ineligible, which is the one a reader actually wants.
			const double pair_turn = s_camtest_round_turn * inv_round;
			const double frozen_turn = 0.25 * pair_turn;

			const camtest_candidate* best_usable = nullptr;
			float best_usable_drift = 0.f;
			u32 best_usable_rank = 0;

			// ONE PASS FOR THE WHOLE TABLE, before anything is printed. The freeze verdict used to
			// be computed inside the print loop, which meant it existed only for rows that were
			// printed -- and the per-source summary below needs it for the ~4,500 rows that are not.
			for (u32 i = 0; i < count; ++i)
			{
				const camtest_candidate& slot = *rows[i].entry;

				// This candidate's OWN last turn, in degrees. A candidate whose drift is low because
				// it never moves is not a camera, and this is the column that says so.
				double dot = 0.0;
				for (u32 k = 0; k < 3; ++k)
					dot += static_cast<double>(slot.now_forward[k]) * static_cast<double>(slot.prev_forward[k]);

				rows[i].turn = std::acos(std::clamp(dot, -1.0, 1.0)) * (180.0 / 3.14159265358979323846);
				rows[i].frozen = (pair_turn > 0.0) && (rows[i].turn < frozen_turn);

				if (!best_usable && !rows[i].frozen && slot.eligible)
				{
					best_usable = &slot;
					best_usable_drift = rows[i].drift;
					best_usable_rank = i + 1;
				}
			}

			u32 printed = 0;

			for (u32 i = 0; i < count; ++i)
			{
				const camtest_candidate& slot = *rows[i].entry;
				const bool elected = (slot.key == s_camtest_elected_key);
				const double turn = rows[i].turn;
				const bool frozen = rows[i].frozen;

				// PCSX2_REMIX_CAMTESTSRC. Filters what is PRINTED, never the rank: #238 still means
				// 238th of the full table.
				if (source_filter >= 0 && static_cast<int>(slot.source) != source_filter && !elected)
					continue;

				// The top few plus the elected one, always, so "is the elected candidate the best
				// available?" is answered on every round even when it ranks last. Counted over rows
				// that PASSED the filter, so restricting to one source yields row_limit rows OF THAT
				// SOURCE rather than whatever fraction of it happens to sit in the global top N.
				if (printed >= row_limit && !elected)
					continue;

				++printed;

				INFO_LOG("Remix: CAMTEST   #{} drift {:.4g} alpha {:.3f} (centroid {:.4g}) pairs {} "
						 "wit {} (alphaN {}) | src={} off=0x{:04x} pc=0x{:04x} ucode=0x{:016x} hyp={}/{} "
						 "content=0x{:016x}{} fovY {:.2f} dscale {:.4g}{}{} own-turn {:.2f} deg{}",
					i + 1, rows[i].drift, slot.last_alpha, slot.last_centroid_drift, slot.pairs,
					slot.last_witnesses, slot.last_alpha_witnesses,
					candidate_source_name(slot.source),
					slot.mem_offset, slot.start_pc, slot.ucode_hash, slot.hypothesis,
					slot.column_major ? 'C' : 'R', slot.content_hash,
					slot.raw_only ? " RAW(does not split/score -- can never be elected)" : "",
					slot.fov_y, slot.depth_scale,
					// The correction that produced that dscale, so the reading is auditable: a
					// row at dscale ~1 off a wdiv of 1.4e7 and a row at dscale ~1 with no
					// correction at all are very different findings.
					(slot.w_divisor != 1.f || slot.swap_zw)
						? fmt::format(" [wdiv {:.6g} x2^{:.2f}{}]", slot.w_divisor,
							  (slot.w_divisor > 0.f) ? std::log2(slot.w_divisor) : 0.f,
							  slot.swap_zw ? ", z/w SWAPPED" : "")
						: std::string(),
					slot.eligible ? "" : " (INELIGIBLE: depth scale outside PCSX2_REMIX_CAMSCALE)",
					turn, frozen ? " FROZEN(own-turn under a quarter of the pair's)" : "",
					elected ? "  <-- ELECTED" : "");
			}

			// --- PER SOURCE, and this is the block that closes the visibility hole ---------------
			//
			// The ranked table is sorted by drift and a FROZEN row has low drift for free, so the
			// printed window fills with frozen rows and everything else is invisible no matter how
			// many rows are printed. On the 2026-08-16 10:19 session that produced a completely
			// wrong conclusion: source 7's ten PRINTED rows were all frozen, which read as "the
			// register-resident matrix does not rotate", while the table held 4,569 rows and the
			// best non-frozen eligible one sat at rank #238.
			//
			// So every source gets one line, every round, naming its best NON-FROZEN row and how
			// many of its rows froze. A source whose rows are all frozen says so explicitly instead
			// of being indistinguishable from a source with no rows at all. Log-only.
			for (u32 source = 0; source <= 7; ++source)
			{
				u32 total = 0;
				u32 frozen_count = 0;
				u32 eligible_count = 0;
				const camtest_row* best = nullptr;
				u32 best_rank = 0;

				for (u32 i = 0; i < count; ++i)
				{
					if (rows[i].entry->source != source)
						continue;

					++total;
					frozen_count += rows[i].frozen ? 1u : 0u;
					eligible_count += rows[i].entry->eligible ? 1u : 0u;

					if (!best && !rows[i].frozen)
					{
						best = &rows[i];
						best_rank = i + 1;
					}
				}

				if (total == 0)
					continue;

				if (best)
				{
					const camtest_candidate& slot = *best->entry;
					INFO_LOG("Remix: CAMTEST src={} -- {} rows ({} frozen, {} eligible). Best "
							 "NON-FROZEN: #{} of {} drift {:.4g} alpha {:.3f} wit {} (alphaN {}) "
							 "own-turn {:.2f} deg dscale {:.4g} wdiv {:.6g}{} | off=0x{:04x} "
							 "pc=0x{:04x} hyp={}/{} content=0x{:016x}{}{}",
						candidate_source_name(source), total, frozen_count, eligible_count,
						best_rank, count, best->drift, slot.last_alpha, slot.last_witnesses,
						slot.last_alpha_witnesses, best->turn, slot.depth_scale, slot.w_divisor,
						slot.swap_zw ? " z/w SWAPPED" : "",
						slot.mem_offset, slot.start_pc, slot.hypothesis,
						slot.column_major ? 'C' : 'R', slot.content_hash,
						slot.eligible ? "" : " (INELIGIBLE: depth scale outside PCSX2_REMIX_CAMSCALE)",
						slot.raw_only ? " RAW(does not split/score)" : "");
				}
				else
				{
					INFO_LOG("Remix: CAMTEST src={} -- {} rows, ALL FROZEN (own-turn under a quarter "
							 "of the pair's {:.2f} deg). Nothing this source published rotated with "
							 "the eye over the measured pairs, so none of its drift figures mean "
							 "anything.",
						candidate_source_name(source), total, pair_turn);
				}
			}

			// The one row a reader wants: lowest drift among candidates that both rotated with the
			// eye and recover the guest's own unit. Printed separately rather than by re-sorting,
			// so the raw drift ranking above is never quietly reordered under anyone.
			if (best_usable)
			{
				INFO_LOG("Remix: CAMTEST best non-frozen, eligible row -- #{} drift {:.4g} alpha "
						 "{:.3f} dscale {:.4g} wdiv {:.6g}{} | src={} off=0x{:04x} pc=0x{:04x} "
						 "ucode=0x{:016x} hyp={}/{} content=0x{:016x}{}. Alpha ~0 here with a lever "
						 "arm above ~1 deg is the answer; alpha ~1 or ~2 means the search has still "
						 "not reached the camera. If a ':w' row is eligible at alpha ~1.5 where its "
						 "baseline twin was ineligible at the same alpha, the depth scale was NOT "
						 "the obstacle -- the matrix is camera-attached but is not the "
						 "view-projection, and the next question is the z/w swap "
						 "(PCSX2_REMIX_CAMDEPTH = 2), not another scale.",
					best_usable_rank, best_usable_drift, best_usable->last_alpha,
					best_usable->depth_scale, best_usable->w_divisor,
					best_usable->swap_zw ? " z/w SWAPPED" : "",
					candidate_source_name(best_usable->source), best_usable->mem_offset,
					best_usable->start_pc, best_usable->ucode_hash, best_usable->hypothesis,
					best_usable->column_major ? 'C' : 'R', best_usable->content_hash,
					best_usable->raw_only ? " RAW(does not split/score)" : "");
			}
			else
			{
				INFO_LOG("Remix: CAMTEST best non-frozen, eligible row -- NONE. Every ranked row "
						 "either froze (own-turn under a quarter of the pair's {:.2f} deg) or "
						 "recovers a depth scale outside PCSX2_REMIX_CAMSCALE. The table is not a "
						 "ranking of cameras; do not read the top of it as one.",
					pair_turn);
			}
		}

		// Window-side half. Called at the TOP of resolve_world_camera, before anything replaces the
		// camera or the candidate solvers, for the same reason WORLDPROBE and WORLDFIX sample their
		// eye there: the camera being paired with this window's geometry has to be the camera that
		// geometry was un-projected against.
		void camtest_evaluate()
		{
			const int mode = camtest_mode();
			const int all_mode = camtest_all_mode();

			if (all_mode != s_camtest_all_logged_mode)
			{
				s_camtest_all_logged_mode = all_mode;
				INFO_LOG("Remix: PCSX2_REMIX_CAMTESTALL = {} ({}). It widens WHAT IS MEASURED, never "
						 "what can be installed: a matrix that fails split_view_projection_direct has "
						 "no view/projection to elect and the election loop skips it before any rank "
						 "is computed, so no matrix this knob adds can become the camera under any "
						 "mode. Under PCSX2_REMIX_CAMTEST = 1 it changes the log only. Under "
						 "PCSX2_REMIX_CAMTEST = 2 the camera is already chosen by measurement, and "
						 "this changes which measurements exist. It also costs GS-thread time on "
						 "pairing windows, which can move frame pacing and therefore the turn rate -- "
						 "compare the round header's mean turn before comparing runs.",
					all_mode,
					(all_mode == 0) ? "off -- only triples that split and score are measured" :
						((all_mode == 1) ? "measure every published candidate x majorness x hypothesis" :
							"measure every published candidate, plus the VU1 transform-probe neighbourhood"));
			}

			if (mode != s_camtest_logged_mode)
			{
				s_camtest_logged_mode = mode;
				INFO_LOG("Remix: PCSX2_REMIX_CAMTEST = {} ({}). Drift is how far a static draw's "
						 "RECOVERED WORLD POSITION moves between two windows while the eye turns: it "
						 "is ~0 for the real camera and grows with distance for a wrong view "
						 "rotation. Set PCSX2_REMIX_CAMTESTAFTER to delay arming into gameplay.",
					mode,
					(mode == 0) ? "off" : ((mode == 1) ? "audit only, no behaviour change" :
						"elect the lowest-drift candidate instead of the highest shape score"));
			}

			if (mode == 0)
			{
				// OFF DOES NOT CONSUME THE ARM. The per-draw dump sets s_drawdump_started = true with
				// a zero budget on the first qualifying frame whatever its own knob says, which makes
				// it impossible to arm mid-run; nothing here latches while the feature is off.
				s_camtest_active = 0;
				s_camtest_all_active = 0;
				return;
			}

			if (!s_camtest_started)
			{
				// A qualifying window submitted geometry with a world camera live -- the same
				// condition FRAMETRACE and WORLDPROBE arm on. IT RETRIES: one presented window in
				// three on this title submits nothing at all (the game's 40 fps against our 60 Hz
				// present), so a one-shot arm has a one-in-three chance of landing on an empty one,
				// which has already silently produced a whole run of nothing once.
				if (s_submitted_this_frame == 0 || !s_active_camera.valid)
					return;

				if (s_camtest_skipped < camtest_after_frames())
				{
					++s_camtest_skipped;
					return;
				}

				s_camtest_started = true;
				s_camtest_hb_window = s_frame_counter;
				INFO_LOG("Remix: CAMTEST armed at frame {} after skipping {} qualifying window(s). "
						 "First table at {} qualifying pairs (PCSX2_REMIX_CAMTESTPAIRS), then 2x that, "
						 "up to {}. Pairs are formed against an ANCHOR window held for up to {} windows "
						 "(PCSX2_REMIX_CAMTESTSPAN) so the turn accumulates to at least {:.2f} deg "
						 "before the pair counts -- pairing against the previous window gave only 0.44 "
						 "deg at full stick, which is too small to rank anything. A 'CAMTEST heartbeat' "
						 "follows every {} windows whether or not a table fires; read its LEVER ARM "
						 "and ACCUMULATED TURN first.",
					s_frame_counter, s_camtest_skipped, camtest_first_round_pairs(),
					camtest_round_pairs_max, camtest_span_windows(), camtest_min_turn_degrees(),
					camtest_heartbeat_windows());
			}

			s_camtest_active = mode;
			s_camtest_all_active = all_mode;

			const u64 window = s_frame_counter;
			const u32 inside = s_camtest_inside_draws;
			s_camtest_inside_draws = 0;

			// The notes describing the solvers about to be evaluated were made by the PREVIOUS
			// resolve pass, which is the one that un-projected this window -- so these counters are
			// read and cleared here, before the loop below writes the next pass's.
			const u32 noted_split = s_camtest_noted_split;
			const u32 noted_raw = s_camtest_noted_raw;
			const u32 noted_probe = s_camtest_noted_probe;
			s_camtest_noted_split = 0;
			s_camtest_noted_raw = 0;
			s_camtest_noted_probe = 0;

			const bool camera_valid = s_active_camera.valid;
			float forward[3] = {0.f, 0.f, 0.f};

			if (camera_valid)
			{
				// Camera forward in world space, exactly as submit_camera() and WORLDPROBE compute
				// it: p_view = p_world * V is row-vector, so view z's gradient is V's third column.
				forward[0] = s_active_camera.view.m[0][2];
				forward[1] = s_active_camera.view.m[1][2];
				forward[2] = s_active_camera.view.m[2][2];
			}

			// Heartbeat accounting for this window, gathered whether or not anything qualifies.
			++s_camtest_hb_windows;
			s_camtest_hb_inside_sum += static_cast<double>(inside);
			s_camtest_hb_inside_max = std::max(s_camtest_hb_inside_max, inside);

			// ---- ANCHOR ADVANCE ------------------------------------------------------------
			// The witnesses captured their anchor during THIS window's draws (camtest_sample read
			// the same flag), and the candidate solvers still hold the set in force for this window
			// -- the resolve below has not written the next set yet. This is therefore the one
			// instant at which all three halves of an anchor are simultaneously consistent, which
			// is why the roll moved here from the top of resolve_world_camera.
			//
			// Requires geometry: an anchor on a window that drew nothing has no witnesses attached
			// to it, and one presented window in three on this title is exactly that window. The arm
			// simply stays up until a window with draws arrives.
			if (s_camtest_anchor_arm && camera_valid && inside > 0)
			{
				camtest_roll();

				s_camtest_anchor_arm = false;
				s_camtest_cycle_paired = false;
				++s_camtest_anchor_cycles;
				s_camtest_prev_valid = true;
				s_camtest_prev_window = window;

				for (u32 k = 0; k < 3; ++k)
				{
					s_camtest_prev_eye[k] = s_active_camera.position[k];
					s_camtest_prev_forward[k] = forward[k];
				}
			}
			else if (camera_valid && s_camtest_prev_valid && window > s_camtest_prev_window)
			{
				double dot = 0.0;
				double move = 0.0;

				for (u32 k = 0; k < 3; ++k)
				{
					dot += static_cast<double>(forward[k]) * static_cast<double>(s_camtest_prev_forward[k]);
					const double d = static_cast<double>(s_active_camera.position[k] - s_camtest_prev_eye[k]);
					move += d * d;
				}

				const double turn = std::acos(std::clamp(dot, -1.0, 1.0)) * (180.0 / 3.14159265358979323846);
				move = std::sqrt(move);

				// BEFORE the turn gate, deliberately. The turn statistic has to describe what the eye
				// actually did, not what the eye did on windows that already passed a turn test --
				// that circularity is exactly why the old log could not tell the two faults apart.
				s_camtest_hb_turn_sum += turn;
				s_camtest_hb_turn_max = std::max(s_camtest_hb_turn_max, turn);
				++s_camtest_hb_turn_count;

				// Also before the gate: how many witnesses are paired across THIS window and the
				// previous one, which is the witness-side precondition on its own. A turn of 20 deg
				// with survivors 0 and a turn of 0.01 deg with survivors 19 are opposite problems and
				// produced an identical (empty) log before this.
				{
					u32 survivors = 0;
					for (const camtest_witness& witness : s_camtest_witnesses)
					{
						if (witness.key != 0 && witness.prev_valid && witness.count != 0 &&
							witness.frame == window && witness.prev_frame == s_camtest_prev_window &&
							witness.count == witness.prev_count)
							++survivors;
					}

					s_camtest_hb_survivors_last = survivors;
					s_camtest_hb_survivors_max = std::max(s_camtest_hb_survivors_max, survivors);
				}

				const float turn_min = camtest_min_turn_degrees();
				const float turn_max = camtest_max_turn_degrees();
				const u64 span = window - s_camtest_prev_window;

				// RE-ANCHOR POLICY. The anchor is HELD to the full span rather than released the
				// moment the turn threshold is met, and every window in between that clears the
				// threshold counts as its own pair. Releasing early was the obvious design and it is
				// the worse one: it pins every pair at exactly turn_min. Per unit of play time,
				//
				//   release at 2 deg  -> ~1 pair per 11 windows, lever arm 2 deg
				//   hold to 30        -> ~20 pairs per 30 windows, lever arm 2..6 deg
				//
				// and alpha's noise falls with turn x sqrt(pairs), so holding wins on both terms at
				// once. The pairs within one cycle share an anchor and are therefore correlated --
				// that is the cost, and it is why the round line reports turn MIN and MAX rather
				// than the mean alone, so a table built mostly from short-arm pairs is visible.
				//
				//   jump -- over the cap. Re-anchoring IMMEDIATELY is the point: without it a cut
				//           would poison every remaining window of the cycle behind a stale anchor.
				//   span -- the normal end of a cycle.
				const bool over_cap = turn > static_cast<double>(turn_max);
				const bool span_done = span >= static_cast<u64>(camtest_span_windows());

				if (over_cap)
					++s_camtest_reanchor_jump;
				else if (span_done)
					++s_camtest_reanchor_span;

				if (span_done && s_camtest_cycle_paired)
					++s_camtest_reanchor_harvest;

				s_camtest_anchor_arm = over_cap || span_done;

				if (turn < static_cast<double>(turn_min))
				{
					++s_camtest_reject_turn;
				}
				else if (turn > static_cast<double>(turn_max))
				{
					++s_camtest_reject_jump;
				}
				else
				{
					const u64 previous = s_camtest_prev_window;
					const double turn_radians = turn * (3.14159265358979323846 / 180.0);
					u32 witnesses_used = 0;

					for (camtest_candidate& slot : s_camtest_candidates)
					{
						// A solver's stamp is the FIRST window it un-projected, and it stays in force
						// until the next resolve replaces it -- which on this title is not every
						// window: one presented window in three carries no new candidate matrices at
						// all (the game's 40 fps against our 60 Hz present), and the camera is simply
						// held through it. So the test is containment, not equality: `now` must
						// already have been in force at `window`, and `prev` must have been the one
						// in force at `previous`. Requiring equality here silently threw away every
						// pair that straddled a held window, which is two thirds of them.
						if (slot.key == 0 || !slot.prev_valid || slot.now_frame > window ||
							slot.prev_frame > previous || previous >= slot.now_frame)
							continue;

						float drifts[camtest_witness_slots];
						float centroids[camtest_witness_slots];
						float alphas[camtest_witness_slots];
						u32 used = 0;
						u32 alpha_used = 0;

						for (camtest_witness& witness : s_camtest_witnesses)
						{
							if (witness.key == 0 || !witness.prev_valid || witness.count == 0 ||
								witness.frame != window || witness.prev_frame != previous ||
								witness.count != witness.prev_count)
								continue;

							// Idempotent, and set here rather than after the maths: this witness has
							// met every precondition for a pair, which is what "not an orphan" means.
							witness.ever_paired = true;

							double sum = 0.0;
							double centre_now[3] = {0.0, 0.0, 0.0};
							double centre_prev[3] = {0.0, 0.0, 0.0};
							bool finite = true;

							for (u32 i = 0; i < witness.count; ++i)
							{
								float now[3];
								float prev[3];
								remix_ps2::solve_world_position(slot.now_solver, witness.now[i][0],
									witness.now[i][1], witness.now[i][2], now);
								remix_ps2::solve_world_position(slot.prev_solver, witness.prev[i][0],
									witness.prev[i][1], witness.prev[i][2], prev);

								double squared = 0.0;
								for (u32 k = 0; k < 3; ++k)
								{
									const double d = static_cast<double>(now[k]) - static_cast<double>(prev[k]);
									squared += d * d;
									centre_now[k] += static_cast<double>(now[k]);
									centre_prev[k] += static_cast<double>(prev[k]);
								}

								if (!std::isfinite(squared))
								{
									finite = false;
									break;
								}

								sum += std::sqrt(squared);
							}

							if (!finite)
								continue;

							const double inv = 1.0 / static_cast<double>(witness.count);
							double centre_squared = 0.0;
							double range_squared = 0.0;

							for (u32 k = 0; k < 3; ++k)
							{
								const double d = (centre_now[k] - centre_prev[k]) * inv;
								centre_squared += d * d;

								// How far this witness is from the eye THIS candidate recovers. A
								// wrong view rotation displaces a static point by range x rotation,
								// so the drift only means something beside the range it happened at.
								const double r = (centre_now[k] * inv) - static_cast<double>(slot.now_eye[k]);
								range_squared += r * r;
							}

							// Per unit of guest depth, so one camera emitted at two column scales --
							// which this title does, an exact 10x on columns 0 and 1 -- ranks the
							// same instead of the larger one being penalised for its scale.
							//
							// THE GUARD WAS `> 1e-6f` AND THAT SILENTLY BROKE THE THING IT EXISTS FOR.
							// A candidate at this title's dscale of 7.232e-08 does not clear 1e-6, so
							// `scale` fell back to 1.0 and its drift was reported RAW -- in a recovered
							// space compressed ~1.4e7x against every other row's. Its drift column read
							// 0.002402 not because it was steady but because its whole world is small,
							// which is the precise failure the division exists to prevent, applied
							// backwards. It is also the column PCSX2_REMIX_CAMTEST = 2 elects on.
							//
							// 1e-30 is a numerical floor, not a policy: 7.232e-08 is a perfectly well
							// conditioned divisor in double, and the only value that must not be
							// divided by is one that is zero or denormal. EXPECT THE REPORTED DRIFT OF
							// ANY ROW WITH dscale < 1e-6 TO JUMP BY 1/dscale AGAINST EARLIER SESSIONS
							// -- that is the correction, not a regression. Alpha is unaffected: it
							// never went through `scale` at all, which is why alpha and drift
							// disagreed about the same row.
							const double scale = (std::isfinite(slot.depth_scale) && slot.depth_scale > 1e-30f)
													 ? static_cast<double>(slot.depth_scale)
													 : 1.0;

							drifts[used] = static_cast<float>((sum * inv) / scale);
							centroids[used] = static_cast<float>(std::sqrt(centre_squared) / scale);
							++used;

							// Scale-free, and computed from the RAW recovered values: the range
							// carries the same recovered scale as the drift, so it cancels.
							const double range = std::sqrt(range_squared);
							if (range > 1e-3 && turn_radians > 1e-6)
							{
								alphas[alpha_used] = static_cast<float>((sum * inv) / (range * turn_radians));
								++alpha_used;
							}
						}

						if (used == 0)
							continue;

						// MEDIAN, not mean: some witnesses are genuinely dynamic geometry -- a
						// character, a door, a vehicle in motion -- and those move in world space
						// under the correct camera too.
						witnesses_used = std::max(witnesses_used, used);

						slot.last_witnesses = used;
						slot.last_alpha_witnesses = alpha_used;
						slot.last_drift = camtest_median(drifts, used);
						slot.last_centroid_drift = camtest_median(centroids, used);
						slot.last_alpha = (alpha_used > 0) ? camtest_median(alphas, alpha_used) : 0.f;
						++slot.pairs;

						slot.history[slot.history_next] = slot.last_drift;
						slot.history_next = (slot.history_next + 1) % camtest_history;
						slot.history_count = std::min(slot.history_count + 1, camtest_history);
					}

					if (witnesses_used > 0)
					{
						++s_camtest_pairs;
						s_camtest_cycle_paired = true;

						if (s_camtest_round == 0 || witnesses_used < s_camtest_round_witness_min)
							s_camtest_round_witness_min = witnesses_used;

						// The lever arm this pair actually delivered. Reported as mean/min/max
						// rather than mean alone: the mean hid that every pair in the first round
						// was under half a degree.
						if (s_camtest_round == 0 || turn < s_camtest_round_turn_min)
							s_camtest_round_turn_min = turn;

						s_camtest_round_turn_max = std::max(s_camtest_round_turn_max, turn);
						s_camtest_round_span += static_cast<double>(span);
						s_camtest_round_span_max =
							std::max(s_camtest_round_span_max, static_cast<u32>(span));

						s_camtest_hb_pair_turn_sum += turn;
						s_camtest_hb_pair_turn_max = std::max(s_camtest_hb_pair_turn_max, turn);
						s_camtest_hb_pair_span_sum += static_cast<double>(span);
						++s_camtest_hb_pair_count;

						++s_camtest_round;
						s_camtest_round_turn += turn;
						s_camtest_round_move += move;
						s_camtest_round_witnesses += static_cast<double>(witnesses_used);
						s_camtest_round_inside += static_cast<double>(inside);

						// max(), not a sum: under CAMTESTALL every splitting triple is noted by both
						// paths, so `raw` already contains `split` and adding them would double-count.
						s_camtest_round_scored +=
							static_cast<double>(std::max(noted_split, noted_raw) + noted_probe);
						s_camtest_round_scored_split += static_cast<double>(noted_split);
						s_camtest_round_scored_probe += static_cast<double>(noted_probe);

						// Adaptive target: 8, 16, 32, 60, 60, ... The per-candidate drift history is a
						// 64-deep ring that a report does not clear, so table N+1 is always computed
						// over more evidence than table N -- an early table is noisy, never wrong in a
						// way a later one does not correct.
						u32 target = camtest_first_round_pairs();
						for (u32 i = 0; i < s_camtest_rounds_done && target < camtest_round_pairs_max; ++i)
							target *= 2;

						target = std::min(target, camtest_round_pairs_max);

						if (s_camtest_round >= target)
						{
							++s_camtest_rounds_done;
							camtest_report(window);
							s_camtest_round = 0;
							s_camtest_round_turn = 0.0;
							s_camtest_round_move = 0.0;
							s_camtest_round_witnesses = 0.0;
							s_camtest_round_inside = 0.0;
							s_camtest_round_witness_min = 0;
							s_camtest_round_scored = 0.0;
							s_camtest_round_scored_split = 0.0;
							s_camtest_round_scored_probe = 0.0;
							s_camtest_round_turn_min = 0.0;
							s_camtest_round_turn_max = 0.0;
							s_camtest_round_span = 0.0;
							s_camtest_round_span_max = 0;
						}
					}
				}
			}

			// NOTE: the anchor is NOT advanced here any more. It moves only in the arm branch above,
			// which is what lets the turn accumulate across a span instead of being reset to the
			// previous window every time. The old unconditional advance is what capped the lever arm
			// at one window of rotation -- 0.44 deg on this title at full stick.

			// ---- HEARTBEAT -----------------------------------------------------------------
			// SILENCE IS NOT AN ACCEPTABLE OUTPUT. An armed instrument that produces nothing for
			// 551 windows has told the user nothing about which of its preconditions failed, and
			// three sessions were spent that way. This fires on a fixed window cadence whether or
			// not a round does, and the two numbers that separate the two failure causes --
			// 'eye turn max' and 'survivors' -- are printed first.
			if (const u32 cadence = camtest_heartbeat_windows();
				cadence > 0 && window >= (s_camtest_hb_window + cadence))
			{
				const double inv = (s_camtest_hb_windows > 0)
									   ? (1.0 / static_cast<double>(s_camtest_hb_windows)) : 0.0;
				const double inv_turn = (s_camtest_hb_turn_count > 0)
											? (1.0 / static_cast<double>(s_camtest_hb_turn_count)) : 0.0;

				u32 target = camtest_first_round_pairs();
				for (u32 i = 0; i < s_camtest_rounds_done && target < camtest_round_pairs_max; ++i)
					target *= 2;

				target = std::min(target, camtest_round_pairs_max);

				u32 witnesses_live = 0;
				for (const camtest_witness& witness : s_camtest_witnesses)
					witnesses_live += (witness.key != 0) ? 1u : 0u;

				const double inv_pair = (s_camtest_hb_pair_count > 0)
											? (1.0 / static_cast<double>(s_camtest_hb_pair_count)) : 0.0;

				INFO_LOG("Remix: CAMTEST heartbeat @ frame {} -- {} rounds so far, {}/{} pairs toward "
						 "the next. Over the last {} windows: LEVER ARM on counted pairs mean {:.2f} "
						 "deg max {:.2f} deg over a mean span of {:.1f} windows ({} pairs); "
						 "ACCUMULATED TURN anchor->now max {:.3f} deg mean {:.3f} deg over {} "
						 "evaluations (need {:.3f}..{:.3f}, span cap {}); WITNESSES SURVIVING to the "
						 "anchor: last {} max {}. Witness draws/window mean {:.1f} max {}, table holds "
						 "{} of {}. Anchors: {} cycles, re-anchored {} on harvest {} on span {} on "
						 "jump. Since last beat: pairs +{}, rejected +{} no-turn +{} jump, dropped +{} "
						 "at a viewport edge, +{} for a full witness table, +{} orphaned witnesses "
						 "(drew once, never paired). Candidates {} of {} slots, {} evicted with "
						 "history. Session totals: {} pairs, {} no-turn, {} jump, {} edge-dropped, {} "
						 "overflow, {} orphans.",
					window, s_camtest_rounds_done, s_camtest_round, target,
					s_camtest_hb_windows,
					s_camtest_hb_pair_turn_sum * inv_pair, s_camtest_hb_pair_turn_max,
					s_camtest_hb_pair_span_sum * inv_pair, s_camtest_hb_pair_count,
					s_camtest_hb_turn_max, s_camtest_hb_turn_sum * inv_turn, s_camtest_hb_turn_count,
					camtest_min_turn_degrees(), camtest_max_turn_degrees(), camtest_span_windows(),
					s_camtest_hb_survivors_last, s_camtest_hb_survivors_max,
					s_camtest_hb_inside_sum * inv, s_camtest_hb_inside_max,
					witnesses_live, camtest_witness_slots,
					s_camtest_anchor_cycles, s_camtest_reanchor_harvest, s_camtest_reanchor_span,
					s_camtest_reanchor_jump,
					s_camtest_pairs - s_camtest_hb_pairs_at,
					s_camtest_reject_turn - s_camtest_hb_noturn_at,
					s_camtest_reject_jump - s_camtest_hb_jump_at,
					s_camtest_clipped_draws - s_camtest_hb_clipped_at,
					s_camtest_witness_overflow - s_camtest_hb_overflow_at,
					s_camtest_orphans - s_camtest_hb_orphans_at,
					s_camtest_used, camtest_candidate_slots, s_camtest_evictions,
					s_camtest_pairs, s_camtest_reject_turn, s_camtest_reject_jump,
					s_camtest_clipped_draws, s_camtest_witness_overflow, s_camtest_orphans);

				// The one-line verdict, so the reading does not have to be re-derived each time.
				if (s_camtest_pairs == s_camtest_hb_pairs_at)
				{
					if (s_camtest_hb_turn_count == 0)
						INFO_LOG("Remix: CAMTEST heartbeat verdict -- NO PAIRS, AND NO WINDOW STEP WAS "
								 "EVER FORMED. Not a turn problem and not a witness problem: no window "
								 "in the last {} carried an accepted world-mode non-sky draw with a "
								 "valid world camera, so there was never a `previous` to pair against. "
								 "Witness draws/window reads {:.1f}. Look at whether the world camera "
								 "is alive at all (the 'camera world score' field on the vu stats line) "
								 "before looking at anything here.",
							s_camtest_hb_windows, s_camtest_hb_inside_sum * inv);
					else if (s_camtest_hb_turn_max < static_cast<double>(camtest_min_turn_degrees()))
						INFO_LOG("Remix: CAMTEST heartbeat verdict -- NO PAIRS: THE ACCUMULATED TURN "
								 "NEVER REACHED THE THRESHOLD (max {:.3f} deg over a span of up to {} "
								 "windows, need {:.3f}). Either the eye is not turning, or the span is "
								 "too short for how slowly it is turning -- 'ACCUMULATED TURN ... over "
								 "N evaluations' above says which, since a still eye gives max ~0 at "
								 "every span. Raise PCSX2_REMIX_CAMTESTSPAN to accumulate longer, or "
								 "lower PCSX2_REMIX_CAMTESTTURNMIN (both milli-degrees / windows) to "
								 "accept a shorter lever arm -- the second buys pairs at the cost of "
								 "the ranking being noisier.",
							s_camtest_hb_turn_max, camtest_span_windows(), camtest_min_turn_degrees());
					else if (s_camtest_hb_survivors_max == 0)
						INFO_LOG("Remix: CAMTEST heartbeat verdict -- NO PAIRS, BUT THE EYE DID TURN "
								 "(max {:.3f} deg). NO WITNESS EVER REACHED A SECOND WINDOW, so the "
								 "fault is in the sampler, not the input: check 'orphaned' and "
								 "'full witness table' above, and note that a witness must draw in two "
								 "CONSECUTIVE geometry-carrying windows with an identical (material, "
								 "vertex count, index count) key and a screen box strictly inside the "
								 "viewport.",
							s_camtest_hb_turn_max);
					else
						INFO_LOG("Remix: CAMTEST heartbeat verdict -- NO PAIRS, but the eye turned "
								 "(max {:.3f} deg) AND {} witnesses did survive. The two halves are "
								 "not landing on the same window pair: turn CONTINUOUSLY rather than "
								 "in bursts, and check the jump rejections above against "
								 "PCSX2_REMIX_CAMTESTTURNMAX.",
							s_camtest_hb_turn_max, s_camtest_hb_survivors_max);
				}

				s_camtest_hb_window = window;
				s_camtest_hb_windows = 0;
				s_camtest_hb_inside_sum = 0.0;
				s_camtest_hb_inside_max = 0;
				s_camtest_hb_survivors_max = 0;
				s_camtest_hb_turn_sum = 0.0;
				s_camtest_hb_turn_max = 0.0;
				s_camtest_hb_turn_count = 0;
				s_camtest_hb_pair_turn_sum = 0.0;
				s_camtest_hb_pair_turn_max = 0.0;
				s_camtest_hb_pair_span_sum = 0.0;
				s_camtest_hb_pair_count = 0;
				s_camtest_hb_pairs_at = s_camtest_pairs;
				s_camtest_hb_noturn_at = s_camtest_reject_turn;
				s_camtest_hb_jump_at = s_camtest_reject_jump;
				s_camtest_hb_clipped_at = s_camtest_clipped_draws;
				s_camtest_hb_overflow_at = s_camtest_witness_overflow;
				s_camtest_hb_orphans_at = s_camtest_orphans;
			}
		}

		// Cleared alongside worldfix_reset_all(), and for the reason recorded there: s_frame_counter
		// is zeroed on GS close, so slots carrying frame indices from the previous session would sit
		// permanently in the future and the test would silently pair nothing.
		void camtest_reset_all()
		{
			for (camtest_witness& slot : s_camtest_witnesses)
				slot = camtest_witness{};

			for (camtest_candidate& slot : s_camtest_candidates)
				slot = camtest_candidate{};

			// The index points into the array that was just cleared; leaving it populated would
			// hand a stale slot back for a key whose history no longer exists.
			camtest_index_reset();

			s_camtest_used = 0;
			s_camtest_started = false;
			s_camtest_skipped = 0;
			s_camtest_elected_key = 0;
			s_camtest_prev_window = 0;
			s_camtest_prev_valid = false;
			s_camtest_inside_draws = 0;
			s_camtest_clipped_draws = 0;
			s_camtest_pairs = 0;
			s_camtest_reject_turn = 0;
			s_camtest_reject_jump = 0;
			s_camtest_round = 0;
			s_camtest_round_turn = 0.0;
			s_camtest_round_move = 0.0;
			s_camtest_round_witnesses = 0.0;
			s_camtest_round_inside = 0.0;
			s_camtest_round_witness_min = 0;
			s_camtest_round_scored = 0.0;
			s_camtest_round_scored_split = 0.0;
			s_camtest_round_scored_probe = 0.0;
			s_camtest_noted_split = 0;
			s_camtest_noted_raw = 0;
			s_camtest_noted_probe = 0;
			s_camtest_witness_overflow = 0;
			s_camtest_orphans = 0;
			s_camtest_evictions = 0;
			s_camtest_rounds_done = 0;
			s_camtest_hb_window = 0;
			s_camtest_hb_windows = 0;
			s_camtest_hb_inside_sum = 0.0;
			s_camtest_hb_inside_max = 0;
			s_camtest_hb_survivors_last = 0;
			s_camtest_hb_survivors_max = 0;
			s_camtest_hb_turn_sum = 0.0;
			s_camtest_hb_turn_max = 0.0;
			s_camtest_hb_turn_count = 0;
			s_camtest_hb_pairs_at = 0;
			s_camtest_hb_noturn_at = 0;
			s_camtest_hb_jump_at = 0;
			s_camtest_hb_clipped_at = 0;
			s_camtest_hb_overflow_at = 0;
			s_camtest_hb_orphans_at = 0;
			s_camtest_hb_pair_turn_sum = 0.0;
			s_camtest_hb_pair_turn_max = 0.0;
			s_camtest_hb_pair_span_sum = 0.0;
			s_camtest_hb_pair_count = 0;
			s_camtest_round_turn_min = 0.0;
			s_camtest_round_turn_max = 0.0;
			s_camtest_round_span = 0.0;
			s_camtest_round_span_max = 0;
			s_camtest_anchor_arm = true;
			s_camtest_cycle_paired = false;
			s_camtest_anchor_cycles = 0;
			s_camtest_reanchor_harvest = 0;
			s_camtest_reanchor_span = 0;
			s_camtest_reanchor_jump = 0;
		}

		// Latches the VU thread's candidates and turns the best one into a world camera.
		// Runs after Present, so the camera resolved here is the one both the next frame's
		// draws and the next frame's SetupCamera use -- geometry and camera always reference
		// the same matrix (RPCS3's one-frame latch).
		void resolve_world_camera()
		{
			// PCSX2_REMIX_CAMTEST scores the window that just ended against the candidate solvers
			// that actually un-projected it, so it has to run before anything below replaces either
			// of them. It is also where the mode the draw path reads is refreshed.
			camtest_evaluate();

			// PCSX2_REMIX_CAMDEPTH, refreshed once per resolve and cached for the whole window --
			// the same discipline, and for the same two reasons, as s_camtest_active above: the
			// environment must not be read from the draw path, and the frame election and the
			// per-draw solver must enumerate the SAME family or they place geometry in two
			// different worlds. Read live through env_int_live, never latched: the renderer goes
			// live ~0.2 s before the per-game .conf is applied, and live_int would never see the
			// .conf at all because it re-parses only on a generation counter the .conf does not
			// bump. Four measurements on this project have already been lost to that.
			{
				const int depth_mode = camdepth_mode();
				const int snap_mode = camdepth_snap();

				if (depth_mode != s_camdepth_logged_mode)
				{
					s_camdepth_logged_mode = depth_mode;
					INFO_LOG("Remix: PCSX2_REMIX_CAMDEPTH = {} ({}), snap {}. This is the w-column "
							 "half of the hypothesis set. normalize_screen_clip only ever rewrote "
							 "columns 0 and 1, and make_clip_solver reads columns {{0, 1, 3}} -- so "
							 "the recovered world's UNIT is set by column 3 alone and NO hypothesis "
							 "could change it. That is why the one candidate whose own rotation "
							 "tracks the view (slice-vf pc=0x2040, own-turn 19.7 deg) reports "
							 "dscale 7.232e-08 and is refused by PCSX2_REMIX_CAMSCALE. A ':w' "
							 "hypothesis divides ALL SIXTEEN terms by the measured |column 3 xyz|, "
							 "which is projectively invariant (same NDC) and lands dscale at ~1; a "
							 "':zw' hypothesis exchanges columns 2 and 3 first, which is NOT "
							 "invariant and recovers a different world. AT ANY NON-ZERO SETTING "
							 "THIS CAN CHANGE THE PICTURE: the added hypotheses go through the same "
							 "election as the originals, and a ':w' winner rescales the recovered "
							 "world about the eye (scene radius, light placement, the depth-scale "
							 "gate's verdict all move). Expect alpha to be UNCHANGED by ':w' except "
							 "through the alpha accumulator's range > 1e-3 gate re-admitting near "
							 "witnesses -- alpha divides drift by range and both carry the scale.",
						depth_mode,
						(depth_mode == 0) ? "off -- the seven screen hypotheses that shipped, unrenamed" :
							((depth_mode == 1) ? "seven baseline + seven ':w' (whole matrix / |col3|)" :
								((depth_mode == 2) ? "seven baseline + seven ':zw' (columns 2,3 swapped, then normalised)" :
									"seven ':w' + seven ':zw', no baseline")),
						(snap_mode > 0) ? "ON (divisor snapped to the nearest power of two within 2%)"
										: "off (divisor used as measured)");
				}

				s_camdepth_active = depth_mode;
				s_camdepth_snap_active = snap_mode;

				// The eligibility gate, refreshed here for the same reason and read from the cache
				// by every call site. Below 1 the gate (scale >= 1/limit && scale <= limit) is the
				// empty set, so that is refused rather than honoured.
				const float scale_limit = env_float_signed(L"PCSX2_REMIX_CAMSCALE", 8.f);
				s_camera_scale_limit =
					(std::isfinite(scale_limit) && scale_limit >= 1.f) ? scale_limit : 8.f;

				// PCSX2_REMIX_CAMYFLIP, refreshed here and NOT anywhere on the draw path, for the
				// same two reasons: the environment must not be touched per draw, and the frame
				// election and the per-draw solver must enumerate and score the same family.
				// env_int_live, never `static const` and never live_int -- the renderer goes live
				// ~0.2 s before the per-game .conf is applied and live_int re-parses only on a
				// generation counter the .conf does not bump. Six measurements on this project have
				// been lost to that; this is not a place to be clever.
				const int yflip_mode = camyflip_mode();

				if (yflip_mode != s_camyflip_logged_mode)
				{
					s_camyflip_logged_mode = yflip_mode;
					INFO_LOG("Remix: PCSX2_REMIX_CAMYFLIP = {} ({}). m[1][1] of the recovered "
							 "projection was POSITIVE BY CONSTRUCTION: try_split_once builds `up` "
							 "as the perpendicular component of up_hint, and up_hint is unprojected "
							 "from ndc (0,+1,0.5), so up . col1 > 0 for EVERY matrix under EVERY "
							 "hypothesis -- and score_perspective's m[1][1] < 0 dock had therefore "
							 "never fired once. Negating scale_y does not reach it (it flips col1 "
							 "and up_hint together, so what actually moves is m[0][0] -- that is "
							 "the whole of the ndc/R=5.00 vs ndcY/R=4.50 gap in this project's own "
							 "dump). The bit lives in the BASIS instead: a ':yf' twin takes `up` "
							 "with the opposite sign, which negates row 1 of the projection and "
							 "nothing else. WHAT CANNOT MOVE: the fused matrix is untouched, so a "
							 "':yf' row's clip solver, recovered world and CAMTEST alpha are "
							 "bit-identical to its parent's -- if they differ, this claim is wrong. "
							 "WHAT DOES MOVE: the published camera becomes a vertical mirror with a "
							 "negated m[1][1] compensating it. AT MODES 2 AND 3 THIS CAN CHANGE THE "
							 "PICTURE -- 2 removes both sign docks so hypotheses that differed only "
							 "by the 0.5 handedness dock now tie, and 3 makes every ':yf' twin "
							 "outrank its parent by 1.0 and be elected.",
						yflip_mode,
						(yflip_mode == 0)
							? "off -- no twins, shipped sign weights, family unchanged"
							: ((yflip_mode == 1)
									  ? "twins enumerated, shipped sign weights (a twin scores 1.0 "
										"below its parent and provably cannot be elected)"
									  : ((yflip_mode == 2)
												? "twins enumerated, NEITHER sign scored"
												: "twins enumerated, a POSITIVE m[1][1] docked 1.0 "
												  "-- the twins win")));
				}

				s_camyflip_active = yflip_mode;
				s_camyflip_sign_active = camyflip_sign_policy(yflip_mode);
			}

			RemixVU1Capture::Frame frame{};
			const bool have = RemixVU1Capture::Latch(frame);

			// The producer is on another thread behind a seqlock; never let its count index
			// this side's loop without a bound of our own.
			frame.count = std::min(frame.count, RemixVU1Capture::max_candidates);

			s_latched_kick_seq = frame.kick_seq_end;

			s_stats.vu_kicks += frame.kicks_seen;
			s_stats.vu_kicks_scanned += frame.kicks_scanned;
			s_stats.vu_windows += frame.windows_examined;
			s_stats.vu_survivors += frame.windows_survived;
			s_stats.vu_reentrant += frame.kicks_reentrant;
			s_stats.vu_sliced += frame.sliced_matrices;
			s_stats.vu_sliced_published += frame.sliced_published;

			s_stats.vu_programs_used = std::max(s_stats.vu_programs_used, frame.programs_used);
			if (frame.programs_limit != 0)
				s_stats.vu_programs_limit = frame.programs_limit;

			s_stats.vu_programs_refused += frame.programs_refused;
			if (frame.programs_refused != 0)
			{
				s_stats.vu_refused_start_pc = frame.refused_start_pc;
				s_stats.vu_refused_ucode = frame.refused_ucode;
			}
			s_stats.cam_candidates += frame.count;
			s_stats.cam_last_candidates = frame.count;

			const viewport_constants vp = s_last_viewport;

			static u32 transform_probes_written = 0;
			if (remix_ps2::dump_enabled() && frame.transform_probe_valid && transform_probes_written < 8)
			{
				++transform_probes_written;
				std::string probe = fmt::format("VU-PROBE f={} ucode=0x{:016x} base_qw={} matrix_qw={}",
					s_frame_counter, frame.transform_probe_ucode, frame.transform_probe_base,
					frame.transform_probe_matrix);
				for (u32 qword = 0; qword < 64; ++qword)
				{
					const float* const v = &frame.transform_probe[qword * 4];
					fmt::format_to(std::back_inserter(probe), " | q{:03d} [{:.6g} {:.6g} {:.6g} {:.6g}]",
						(frame.transform_probe_base + qword) & 0x3FFu, v[0], v[1], v[2], v[3]);
				}
				dump_write(probe);
			}

			if (remix_ps2::nocam_enabled() || !have || frame.count == 0 || !vp.valid)
			{
				drop_stale_camera();
				return;
			}

			const float width = static_cast<float>(vp.width);
			const float height = static_cast<float>(vp.height);
			const float reference_aspect = (height > 0.f) ? (width / height) : (4.f / 3.f);

			// The hypothesis set (fixed + derived) is enumerated by build_camera_hypotheses above,
			// which the per-draw solver shares so the two can never disagree about what space the
			// guest emits into.
			float best_score = 0.f;
			remix_ps2::vp_split best_split{};
			remix_ps2::mat4 best_normalized = remix_ps2::mat4_identity();
			u64 best_hash = 0;
			u32 best_offset = 0;
			u8 best_source = 0;
			const char* best_name = "";
			bool best_transposed = false;

			// The elected hypothesis' Y-flip, carried so the WORLDFIX re-split below reproduces the
			// SAME factorisation it is correcting. Re-splitting a flipped winner without it would
			// silently un-flip the published camera while WORLDFIX's log claimed only handedness
			// had moved.
			bool best_flip_y = false;

			// The quantity the election maximises. It IS best_score under every mode but
			// PCSX2_REMIX_CAMTEST = 2, which replaces it with a measured-drift rank; best_score is
			// still recorded either way, because the hysteresis and the logging read it.
			float best_rank = 0.f;
			u64 best_camtest_key = 0;

			// camtest_roll() USED TO BE CALLED HERE, unconditionally, once per window. It now lives
			// inside camtest_evaluate() and fires only when the anchor advances -- rolling it every
			// window is exactly what pinned the pair to a one-window separation and held the
			// measurable turn down to 0.44 deg at full stick. Its new home is also the only instant
			// at which the candidate solvers, the witness clip samples and the window's eye all
			// describe the same window. Do not restore this call.

			// Once per resolve, not per candidate and never per draw.
			const int divcam = divcam_mode();

			for (u32 c = 0; c < frame.count; ++c)
			{
				const RemixVU1Capture::Candidate& candidate = frame.items[c];

				remix_ps2::mat4 raw{};
				std::memcpy(&raw.m[0][0], candidate.m, sizeof(candidate.m));

				const u64 content = hash_floats(candidate.m, 16);
				const bool dump = remix_ps2::dump_enabled() && s_dumped.insert(content).second;

				std::string detail;
				float candidate_best = 0.f;
				const char* candidate_name = "none";
				remix_ps2::projection_params candidate_params{};

				for (u32 major = 0; major < 2; ++major)
				{
					const remix_ps2::mat4 oriented = (major == 0) ? raw : remix_ps2::mat4_transpose(raw);

					camera_hypothesis hypotheses[max_camera_hypotheses];
					const u32 hypothesis_count = build_camera_hypotheses(oriented, vp, reference_aspect,
						major != 0, candidate.source, candidate.ucode_hash, s_camdepth_active,
						s_camdepth_snap_active, s_camyflip_active, hypotheses);

					for (u32 h = 0; h < hypothesis_count; ++h)
					{
						const camera_hypothesis& hyp = hypotheses[h];

						const remix_ps2::mat4 normalized = apply_camera_hypothesis(oriented, hyp);

						// PCSX2_REMIX_CAMTESTALL. Noted HERE, above the split and above the shape
						// score, because those two are exactly what this mode exists to stop being
						// the arbiter. camtest_note_candidate below re-notes the same key for a
						// triple that does split -- same normalised matrix, so the same solver and
						// a bit-identical eye -- and adds the fovY the split makes available.
						if (s_camtest_all_active > 0)
						{
							camtest_note_raw(camtest_identity(candidate, major != 0, hyp.name),
								candidate.source, candidate.mem_offset, candidate.start_pc,
								candidate.ucode_hash, content, major != 0, hyp.name, hyp.scale_w,
								hyp.swap_zw, normalized);
						}

						remix_ps2::vp_split split{};
						remix_ps2::split_stage stage = remix_ps2::split_stage::accepted;
						if (!remix_ps2::split_view_projection_direct(normalized, split, &stage, hyp.flip_y))
						{
							++s_stats.cam_reject_split;
							++s_stats.cam_split_stage[static_cast<u32>(stage)];
							if (dump)
								fmt::format_to(std::back_inserter(detail), " {}/{}=-", hyp.name, major ? 'C' : 'R');

							continue;
						}

						float score = remix_ps2::score_perspective(split.projection, reference_aspect,
							s_camyflip_sign_active);
						if (dump)
							fmt::format_to(std::back_inserter(detail), " {}/{}={:.2f}", hyp.name, major ? 'C' : 'R', score);

						if (!(score > 0.f))
						{
							++s_stats.cam_reject_score;
							continue;
						}

						// Latch hysteresis: prefer the matrix that already won, so a tie does
						// not make the world snap between two equally plausible anchors.
						if (content == s_active_camera.matrix_hash)
							score += 0.5f;

						// A matrix the microcode itself pointed at outranks anything the
						// window scan turned up. The split and the perspective filter above
						// have already agreed it is a camera; this only settles which of
						// several accepted candidates the frame anchors to.
						if (candidate.source == 4)
							score += 100.f;
						else if (candidate.source != 0)
							score += 10.f;

						// PCSX2_REMIX_DIVCAM. The microprogram's own statement about which matrix
						// produced the perspective divide's denominator, which is the one piece of
						// evidence here that is a definition rather than a heuristic.
						const bool feeds_div =
							(candidate.flags & RemixVU1Capture::candidate_flag_feeds_div) != 0;

						if (divcam > 0 && feeds_div)
							score += 100.f;
						if (divcam >= 2 && !feeds_div)
							score -= 1000.f;

						if (score > candidate_best)
						{
							candidate_best = score;
							candidate_name = hyp.name;
							remix_ps2::describe_projection(split.projection, candidate_params);
						}

						// PCSX2_REMIX_CAMTEST. The identity is the whole (candidate, majorness,
						// hypothesis) triple -- exactly what would be installed if this won -- so the
						// drift measured for it is the drift of the thing being elected.
						const u64 camtest_key = (s_camtest_active > 0)
													? camtest_identity(candidate, major != 0, hyp.name)
													: 0;

						if (s_camtest_active > 0)
							camtest_note_candidate(camtest_key, candidate, major != 0, hyp.name,
								hyp.scale_w, hyp.swap_zw, content, normalized, split);

						// Mode 2 ranks by measured drift instead of shape score. See
						// camtest_rank_base for the ordering rule and for the fallback when nothing
						// has been measured yet.
						float rank = score;

						if (s_camtest_active >= 2)
						{
							float drift = 0.f;
							if (camtest_ranked_drift(camtest_key, drift))
								rank = camtest_rank_base / (1.f + std::max(0.f, drift));
						}

						// The two new back-slice sources are AUDIT-ONLY until DIVCAM says otherwise.
						// Everything above still ran for them -- the split, the score, the CAMTEST
						// note and therefore the measured drift -- so they appear in the CAMTEST
						// table with a real alpha and can be compared against the incumbent before
						// anything is staked on them. They simply cannot win.
						//
						// PCSX2_REMIX_CAMYFLIP = 1 is audit-only in the same sense, and it is gated
						// EXPLICITLY here rather than left to the 1.0 score gap between a ":yf" twin
						// and its parent. The gap does hold under shape ranking -- and it does NOT
						// hold under PCSX2_REMIX_CAMTEST = 2, where `rank` above comes from the
						// drift table (~1e6) instead of `score` (~100). Parent and twin are separate
						// camtest identities with separate histories, so a window in which the twin
						// has reached camtest_min_pairs and the parent has not (or has just been
						// evicted by the CLOCK hand) hands the twin a 1e6 rank against the parent's
						// 100 and elects it. That is a race, not a measurement, and CAMTEST = 2 is
						// exactly the mode a CAMYFLIP = 1 audit would be run in. Modes 2 and 3 lift
						// the gate: at those the twin is meant to be electable.
						const bool election_eligible = ((candidate.source < 6) || (divcam > 0)) &&
													   (!hyp.flip_y || s_camyflip_active >= 2);

						if (election_eligible && rank > best_rank)
						{
							best_rank = rank;
							best_score = score;
							best_split = split;
							best_normalized = normalized;
							best_hash = content;
							best_offset = candidate.mem_offset;
							best_source = candidate.source;
							best_name = hyp.name;
							best_transposed = (major != 0);
							best_flip_y = hyp.flip_y;
							best_camtest_key = camtest_key;
						}
					}
				}

				if (dump)
				{
					dump_write(fmt::format(
						"f={} src={} off=0x{:04x} pc=0x{:04x} ucode=0x{:016x} shape={:.2f} res=[{} ] best={} "
						"score={:.2f} fovY={:.2f} aspect={:.3f} near={:.5g} M={}",
						s_frame_counter, candidate_source_name(candidate.source),
						candidate.mem_offset, candidate.start_pc, candidate.ucode_hash,
						candidate.score, detail, candidate_name, candidate_best,
						candidate_params.fov_y_degrees, candidate_params.aspect, candidate_params.near_plane,
						remix_ps2::format_matrix(raw)));
				}
			}

			// PCSX2_REMIX_CAMTESTALL = 2. Deliberately OUTSIDE the loop above, and after it: these
			// are not candidates, they are the VU1 neighbourhood the slicer never reads. Nothing
			// below this point can see them -- best_rank, best_split and s_camtest_elected_key are
			// already decided from frame.items[] alone -- so they are measured and nothing else.
			if (s_camtest_all_active >= 2)
				camtest_scan_probe(frame, vp, reference_aspect);

			if (!(best_score > 0.f))
			{
				drop_stale_camera();
				return;
			}

			// The identity of the triple the election just installed, so the CAMTEST table can mark
			// its row. Updated only on an actual election: a window that elects nothing is still
			// drawing with the previous camera, and so is still described by its row.
			s_camtest_elected_key = best_camtest_key;

			// --- PCSX2_REMIX_WORLDFIX: the one sign the hypothesis MATRIX cannot reach -------------
			//
			// m[1][1] of the recovered projection is POSITIVE BY CONSTRUCTION and no hypothesis
			// MATRIX can change it. try_split_once takes up = forward x (up_hint x forward), which is
			// just the component of up_hint perpendicular to forward, and up_hint is by definition
			// the world direction in which clip y increases -- so up . col1 > 0 always. Negating
			// scale_y does flip col1, but it also flips up_hint, hence right and up, and the two
			// flips cancel there. What does NOT cancel is m[0][0]: the sign of scale_x or scale_y
			// flips it either way. So the hypothesis set spans exactly TWO of the four sign
			// combinations, m[0][0] is the only free bit here, and score_perspective merely docks
			// 0.5 for it -- a penalty the +100 source bonus above swamps completely.
			//
			// AMENDED 2026-08-17: the `m[1][1] < 0` dock is no longer unreachable. The bit it needs
			// is not in the matrix at all, it is in the BASIS -- split_view_projection_direct's
			// flip_up takes `up` with the opposite sign, which negates row 1 of the projection and
			// nothing else. PCSX2_REMIX_CAMYFLIP enumerates that as a ":yf" twin of every
			// hypothesis. It is a different quantity from the flip below and the two compose: this
			// one mirrors the recovered WORLD (a real change to every un-projected vertex), the
			// Y-flip mirrors the published CAMERA and leaves the world untouched.
			//
			// That bit is the recovered world's HANDEDNESS. Getting it wrong mirrors the recovered
			// world about the plane spanned by the camera's forward and up axes -- a plane whose
			// normal is `right`, which is horizontal, and which TURNS WITH THE EYE. A mirror plane
			// that rotates re-mirrors the world every frame, so static geometry counter-rotates at
			// twice the yaw rate and a fixed directional light's shadow swings as the player turns.
			// The rendered image cannot show it: negating column 0 of the normalised fused matrix
			// negates the recovered world's lateral axis AND the projection's m[0][0], the camera is
			// re-derived from that same matrix, and view * projection == fused still holds exactly.
			// The picture is bit-identical; only the world coordinates move.
			//
			// Which sign is correct cannot be decided from one frame -- that is precisely what the
			// WORLDFIX audit measures across frames. So this offers both directions and neither is
			// the default.
			if (const int worldfix = worldfix_mode(); worldfix >= 2)
			{
				const bool want_positive = (worldfix == 2);

				if ((best_split.projection.m[0][0] < 0.f) == want_positive)
				{
					// Negating column 0 of the normalised fused matrix IS negating the winning
					// hypothesis' scale_x: normalize_screen_clip divides column 0 by it. Doing it
					// here rather than re-running the hypothesis loop keeps every other term of the
					// election -- candidate, majorness, offsets -- exactly as it was scored.
					remix_ps2::mat4 flipped = best_normalized;
					for (u32 i = 0; i < 4; ++i)
						flipped.m[i][0] = -flipped.m[i][0];

					// best_flip_y, not `false`: the re-split has to reproduce the elected
					// hypothesis' own factorisation, or a WORLDFIX correction applied to a ":yf"
					// winner would quietly un-mirror the published camera as a side effect.
					remix_ps2::vp_split reflected{};
					if (remix_ps2::split_view_projection_direct(flipped, reflected, nullptr, best_flip_y))
					{
						best_normalized = flipped;
						best_split = reflected;

						static bool logged = false;
						if (!logged)
						{
							logged = true;
							INFO_LOG("Remix: WORLDFIX {} flipped the recovered world's handedness -- "
									 "projection m00 is now {:.5g}. The image is unchanged by "
									 "construction; only the recovered world coordinates move. If "
									 "this is the right direction, WORLDFIX audit alpha falls to ~0 "
									 "and a static object's shadow stops swinging as you turn.",
								worldfix, best_split.projection.m[0][0]);
						}
					}
					else
					{
						static bool warned = false;
						if (!warned)
						{
							warned = true;
							WARNING_LOG("Remix: WORLDFIX {} could not re-split the handedness-flipped "
										"matrix -- leaving the camera as elected", worldfix);
						}
					}
				}
			}

			world_camera camera{};
			camera.view = best_split.view;

			remix_ps2::projection_params params{};
			const bool described = remix_ps2::describe_projection(best_split.projection, params);

			// The near plane is NOT taken from the matrix, and the reason is specific rather
			// than cautious. |m[3][2] / m[2][2]| is only a distance when the z column is a
			// projective depth. Rainbow Six 3's is not: its z output is w plus a constant
			// (col2 == col3 across rows 0-2, with m[3][2] carrying the offset), so that ratio
			// comes back as ~2716 -- the offset, not a near plane. Measuring it and believing
			// it produced a 1.1e7-deep frustum and a debug light scaled by 27000.
			//
			// So near/far are world-unit constants, env-overridable, and the recovered value
			// is logged for information only.
			camera.near_plane = world_near_plane();
			camera.far_plane = world_far_plane(camera.near_plane);
			camera.projection = remix_ps2::rebuild_projection_z(best_split.projection, camera.near_plane, camera.far_plane);

			remix_ps2::mat4 view_to_world{};
			if (!remix_ps2::make_clip_solver(best_normalized, camera.solver) ||
				!remix_ps2::mat4_invert(camera.view, view_to_world))
			{
				++s_stats.cam_reject_split;
				drop_stale_camera();
				return;
			}

			camera.position[0] = view_to_world.m[3][0];
			camera.position[1] = view_to_world.m[3][1];
			camera.position[2] = view_to_world.m[3][2];
			camera.matrix_hash = best_hash;
			camera.score = best_score;
			camera.valid = true;

			// --- refutation against the geometry, not against ground truth --------------------
			//
			// cam_accept counts *acceptances*, not correctness, and a camera that splits and
			// scores cleanly can still be nonsense. SOCOM produced eye (0,0,0) exactly, while
			// earlier sessions on the same title gave (1930,210,3920) and (-582,-9,618) -- so
			// the solve is not stable, and a camera at the origin looking at geometry that is
			// somewhere else renders a black frame while every counter still reads healthy.
			// That is the worst possible failure mode: silent.
			//
			// The frame's own submitted geometry refutes it without needing ground truth. If the
			// eye is nowhere near what it is supposedly looking at, it is not the camera, and
			// falling back to view-space at least renders something.
			if (!std::isfinite(camera.position[0]) || !std::isfinite(camera.position[1]) ||
				!std::isfinite(camera.position[2]))
			{
				++s_stats.cam_reject_degenerate;
				drop_stale_camera();
				return;
			}

			// An eye at *exactly* the origin is a solver degeneracy, not a camera: the split
			// collapsed and solve_camera_position returned the trivial answer. Checked
			// unconditionally, because the very first resolve of a session happens before any
			// geometry has been submitted -- which is precisely when SOCOM produced
			// eye (0,0,0) -- so the geometry cross-check below cannot catch it.
			{
				const float eye_magnitude = std::sqrt((camera.position[0] * camera.position[0]) +
													  (camera.position[1] * camera.position[1]) +
													  (camera.position[2] * camera.position[2]));

				if (eye_magnitude < 1e-4f)
				{
					++s_stats.cam_reject_degenerate;

					if (s_stats.cam_reject_degenerate == 1)
						WARNING_LOG("Remix: refusing a world camera whose eye solved to the origin -- staying in view-space");

					drop_stale_camera();
					return;
				}
			}

			// --- depth-scale consistency ---------------------------------------------------
			//
			// The gate that matters, and the one the geometry cross-check below structurally
			// could not be.
			//
			// w = 1/Q is the eye-space depth the guest itself divided by, so it is ground truth
			// in the guest's own units. Every engine we have looked at uses a rigid view
			// transform, which means |col3| of the fused matrix is 1 and the recovered world
			// space shares those units. So un-projecting the NDC centre at w = 1 must land
			// exactly one unit from the eye, and the frame's corners must land at
			// 1/cos(half-diagonal fov) -- 1.11 at Rainbow Six 3's 36 degrees, ~2.9 even at 113.
			//
			// This needs no geometry, so unlike the cross-check below it can run on the FIRST
			// acceptance, which is the only one that matters: once a wrong camera is accepted,
			// the geometry it un-projects becomes the bounds the cross-check compares against,
			// and a wrong camera is perfectly self-consistent with its own output.
			//
			// That single flaw produced two opposite symptoms. Comparing a world-space eye
			// against view-space bounds is a units error, so before 0fa6d10fc it rejected every
			// camera Rainbow Six 3 resolved. Restricting it to "world mode already established"
			// fixed that and simultaneously made it unable to ever catch a bad first
			// acceptance -- SOCOM went from falling back to view space to running world-anchored
			// on a camera whose recovered unit is ~1000x the guest's, which is the vertex
			// explosion the user photographed (scene radius 5,959 against a real extent of ~4).
			{
				static constexpr float ndc_samples[5][2] = {
					{0.f, 0.f}, {-1.f, -1.f}, {1.f, -1.f}, {-1.f, 1.f}, {1.f, 1.f}};

				float centre_scale = 0.f;
				float min_scale = std::numeric_limits<float>::max();
				float max_scale = 0.f;
				bool finite = true;

				for (u32 i = 0; i < std::size(ndc_samples); ++i)
				{
					float probe[3];
					// clip = (ndc_x * w, ndc_y * w, w) with w = 1.
					remix_ps2::solve_world_position(camera.solver, ndc_samples[i][0], ndc_samples[i][1], 1.f, probe);

					const float dx = probe[0] - camera.position[0];
					const float dy = probe[1] - camera.position[1];
					const float dz = probe[2] - camera.position[2];
					const float distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));

					if (!std::isfinite(distance) || !(distance > 0.f))
					{
						finite = false;
						break;
					}

					if (i == 0)
						centre_scale = distance;

					min_scale = std::min(min_scale, distance);
					max_scale = std::max(max_scale, distance);
				}

				const float scale_limit = camera_scale_limit();
				const bool scale_ok = finite && (centre_scale >= (1.f / scale_limit)) &&
				                      (centre_scale <= scale_limit);
				const bool aniso_ok = finite && (max_scale <= (camera_anisotropy_limit() * min_scale));

				if (!scale_ok || !aniso_ok)
				{
					++s_stats.cam_reject_scale;

					if (s_stats.cam_reject_scale == 1)
					{
						WARNING_LOG("Remix: refusing a world camera whose un-projection does not match the "
									"guest's own depth -- a point at w=1 lands {:.4g} units from the eye "
									"(expected ~1, limit {:g}x) and varies {:.3g}x across the frame "
									"(limit {:g}x). Staying in view-space; raise PCSX2_REMIX_CAMSCALE / "
									"PCSX2_REMIX_CAMANISO if this title really does scale its view transform.",
							centre_scale, scale_limit, (min_scale > 0.f) ? (max_scale / min_scale) : 0.f,
							camera_anisotropy_limit());
					}

					drop_stale_camera();
					return;
				}

				camera.depth_scale = centre_scale;
				camera.depth_anisotropy = (min_scale > 0.f) ? (max_scale / min_scale) : 0.f;
			}

			// Drift guard, secondary to the check above. It compares the candidate eye against
			// the previous frame's submitted geometry, so it is only meaningful when that frame
			// was itself world-anchored -- otherwise the bounds are view-space and the eye is
			// world-space, which is a units error. Keyed on what space the bounds were actually
			// captured in rather than on the current camera's validity, so it no longer skips
			// exactly the acceptance that needs guarding.
			if (s_last_bounds.valid && s_last_bounds_world)
			{
				const float cx = 0.5f * (s_last_bounds.min[0] + s_last_bounds.max[0]);
				const float cy = 0.5f * (s_last_bounds.min[1] + s_last_bounds.max[1]);
				const float cz = 0.5f * (s_last_bounds.min[2] + s_last_bounds.max[2]);
				const float dx = camera.position[0] - cx;
				const float dy = camera.position[1] - cy;
				const float dz = camera.position[2] - cz;
				const float distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));

				// The previous frame's geometry was submitted through the previous camera, so
				// this is a loose consistency check, not a tight one: only a camera that is
				// orders of magnitude away from the scene it is meant to be inside is refused.
				const float limit = std::max(s_last_bounds.radius(), world_near_plane()) * camera_distance_limit();

				if (distance > limit)
				{
					++s_stats.cam_reject_degenerate;

					if (s_stats.cam_reject_degenerate == 1)
					{
						WARNING_LOG("Remix: refusing a degenerate world camera -- eye ({:.1f}, {:.1f}, {:.1f}) is "
									"{:.0f} units from the submitted geometry's centre ({:.1f}, {:.1f}, {:.1f}), "
									"limit {:.0f}. Staying in view-space; raise PCSX2_REMIX_CAMDIST if this is wrong.",
							camera.position[0], camera.position[1], camera.position[2], distance, cx, cy, cz, limit);
					}

					drop_stale_camera();
					return;
				}
			}

			if (s_refuted_matrices.count(camera.matrix_hash) != 0)
			{
				// Refuted by the extent check on a previous frame. Without this the camera is
				// re-accepted every frame, submits one bad frame, is refuted again, and the
				// scene strobes between the two spaces.
				++s_stats.cam_reject_extent;
				drop_stale_camera();
				return;
			}

			// EECAM 2 owns s_active_camera: it is installed in OnVSync from the game's actor
			// Location/Rotation, and a VU1 winner accepted here would govern the next window's
			// draws instead, alternating the scene between two spaces.
			if (ee_cam_mode() == 2)
				return;

			s_active_camera = camera;
			s_camera_last_accept_frame = s_frame_counter;
			++s_stats.cam_accept;
			if (best_source != 0)
				++s_stats.cam_accept_sliced;
			if (!s_drawdump_world_armed)
			{
				s_drawdump_world_armed = true;
				s_drawdump_started = false;
				s_drawdump_frames_left = 0;
				s_drawdump_skipped = 0;
			}

			// Pin the address the winner came from. A camera actually recovered from an
			// offset is stronger evidence than any shape score, and without it the true
			// matrix has to out-rank thousands of shape-plausible windows every frame.
			//
			// A register-resident winner (source 7) has no address to pin -- its rows are VF
			// registers -- and it publishes 0xFFFFFFFF for exactly that reason. Leave the
			// existing pin alone rather than clearing it: the per-kick ring behind it is a
			// separate mechanism and there is no reason to retire a working address.
			if (best_source != 7)
				RemixVU1Capture::SetPinnedOffset(best_offset);

			if (!s_logged_world_camera)
			{
				s_logged_world_camera = true;
				INFO_LOG("Remix: world camera resolved -- source {}, hypothesis {}/{}, score {:.2f}, "
						 "fovY {:.1f} deg, near {:.5g}, far {:.5g}, eye ({:.3f}, {:.3f}, {:.3f}), "
						 "depth scale {:.4g} (anisotropy {:.3g}x) "
						 "[matrix-implied near {:.5g}, not used]",
					(best_source == 0) ? "window scan" :
						((best_source == 1) ? "ucode back-slice" :
							((best_source == 2) ? "ucode back-slice (TOPS)" :
								((best_source == 3) ? "pinned back-slice address" : "SOCOM CA fixed VU block"))),
					best_name, best_transposed ? "column-major" : "row-major", best_score,
					params.fov_y_degrees, camera.near_plane, camera.far_plane,
					camera.position[0], camera.position[1], camera.position[2],
					camera.depth_scale, camera.depth_anisotropy,
					described ? params.near_plane : 0.f);
			}

			// The three numbers that decide the WORLDFIX diagnosis and that no existing line prints.
			// m00's SIGN is the recovered world's handedness (see the correction site above); its
			// magnitude with m11 gives the recovered aspect, and the gap between that and the
			// display's is the only term of score_perspective that can be wrong without any other
			// symptom -- on this title the elected hypothesis scores 5.00 out of a possible 6.00 and
			// the aspect term is the one that was docked.
			if (worldfix_mode() > 0 && !s_worldfix_logged_projection)
			{
				s_worldfix_logged_projection = true;
				const float aspect_recovered = (std::abs(best_split.projection.m[0][0]) > 1e-6f)
												   ? (std::abs(best_split.projection.m[1][1]) / std::abs(best_split.projection.m[0][0]))
												   : 0.f;
				INFO_LOG("Remix: WORLDFIX projection -- m00 {:.5g} (sign {}), m11 {:.5g}, recovered "
						 "aspect {:.4f} vs display {:.4f} (delta {:.4f}), fovY {:.2f} deg, fovX "
						 "{:.2f} deg. m00 < 0 means the recovered world is a mirror image; m11 is "
						 "positive by construction.",
					best_split.projection.m[0][0], (best_split.projection.m[0][0] < 0.f) ? "-" : "+",
					best_split.projection.m[1][1], aspect_recovered, reference_aspect,
					std::abs(aspect_recovered - reference_aspect), params.fov_y_degrees,
					(std::abs(best_split.projection.m[0][0]) > 1e-6f)
						? static_cast<float>(2.0 * std::atan(1.0 / std::abs(static_cast<double>(best_split.projection.m[0][0]))) * (180.0 / 3.14159265358979323846))
						: 0.f);
			}
		}

		// --- per-draw camera placement (step 4A) ----------------------------------------------
		//
		// WHAT THIS FIXES, and it is measured, not inferred. PCSX2_REMIX_FRAMETRACE=600 on the
		// user's own SOCOM CA gameplay run (logs\remix_matrices.txt, 600 FRAME lines) says:
		//
		//   * 200 of 600 presented windows submit ZERO geometry, and the spacing between empty
		//     windows is exactly 3 in 199 of 199 cases. That is the game's 40 fps against our
		//     60 Hz present. It is real, it is expected, and it is NOT the defect.
		//   * On the 400 non-empty windows, ringN/draws has a median of 0.999 -- essentially
		//     every draw in the window carries a kick camera DIFFERENT from the window's first
		//     draw. Exactly one window in 400 had ringN == 0.
		//   * Window N's first draw carries window N-1's second camera in 398 of 399 cases
		//     (99.7%): draw 0 is one game-frame stale, and the rest of the window is current.
		//   * The frame-latched camera that un-projects the WHOLE window equals that stale
		//     first-draw camera in 394 of 400 windows (98.5%), and equals the camera ~99.9% of
		//     the draws were actually built under in 4 of 400 (1%).
		//
		// So the backend un-projects ~830 draws per frame with a camera belonging to exactly one
		// of them -- the first -- and that one is a game-frame out of date. Un-projection inverts
		// a view-projection, so applying the wrong inverse is a PROJECTIVE error, not a rigid one:
		// near vertices barely move, far vertices swing hard, and a triangle spanning depth
		// becomes a spike. That is the shattering in the user's clip, and it is why the damage
		// stays inside scene scale (extent-reject reads 0 in every stats block) instead of
		// flinging geometry to infinity.
		//
		// REFUTED, with the measurement, so none of these is re-chased:
		//   * Camera REFUSAL. `REFUSED 0` and `cam world 897 fallback 3` at frame 900; accept
		//     climbs +300 per 300-frame stats block. resolve_world_camera accepts on essentially
		//     every frame. Do not re-derive this.
		//   * Camera DROPOUT / stale-hold. fresh=0 on ZERO of the 600 traced windows -- a camera
		//     is accepted for every single one -- and `age` is exactly 200 windows at 1 and 200 at
		//     2, which is the 40/60 pacing and nothing else. drop_stale_camera() never fired
		//     in-mission. The seed's "one frame in three latches ZERO candidates" is dead: the
		//     matrix dump's period-3 gaps are windows in which the game built no frame at all
		//     (no new matrices of ANY source, object matrices included).
		//   * The RING FAILING TO RECORD. miss totals 823 across all 600 windows and every one of
		//     them is in the very first traced window, before the pin settled. The kick ring
		//     records reliably; branch C of the plan is ruled out.
		//
		// THE TRAP THAT NEARLY SANK THIS, AND WHY IT IS NOT ONE. The four figures above hash the
		// RAW 16 floats the ring returns, and this title emits its camera at TWO COLUMN SCALES: the
		// gameplay draw dump (logs\remix_draws.txt, frames 1834/1836/1837/1839) shows exactly two
		// matrices per frame at the same VU1 offset 0x0080, one at d=0 and one at d=22/23, related
		// by an EXACT uniform scale of 10.0000 on columns 0 and 1 with columns 2 and 3 bit-equal --
		// unanimous over all four frames and all four rows. If the acceptance pipeline normalised
		// that scale away, both would produce ONE solver, every raw-hash counter above would still
		// read 0.999, and per-draw placement would change precisely nothing on screen. That is this
		// project's oldest failure mode: a counter that passes while the picture does not move.
		//
		// MEASURED 2026-08-15, and it settles it: THE PIPELINE DOES NOT ABSORB THE SCALE. Over the
		// 1301 `src=socom-fixed` candidate lines in remix_matrices.txt --
		//   * The winning hypothesis is `ndc` in 1301 of 1301. normalize_screen_clip(m, 1, 0, 1, 0)
		//     composes with the IDENTITY, so the matrix reaches the split completely unmodified and
		//     there is no stage left that could divide a scale out.
		//   * The derived `auto`/`autoY` hypotheses -- the only ones that WOULD absorb it, being
		//     derived from the matrix and therefore scaling with it -- are not even generated for
		//     this title's row-major orientation. The dump's own res= field says so:
		//     `res=[ gs/R=- px/R=- ndc/R=5.00 ndcY/R=4.50 gs/C=- px/C=- ndc/C=- ndcY/C=- auto/C=-
		//     autoY/C=- ]`. auto/R and autoY/R are absent because oy computes to ~0 here and
		//     build_camera_hypotheses requires |oy| > 1e-6.
		//   * The recovered fovY tracks the column scale ONE FOR ONE. The fovY histogram is
		//     112.64 (n=803), 112.11 (246), 28.08 (111), 17.07 (61); tan(fovY/2) of those groups
		//     stands in ratio 1.000 : 1.010 : 6.001 : 9.999 to the base group -- the 10x variant is
		//     right there as its own fovY. Across 20 independent candidate pairs that share
		//     columns 2&3 to within 1% while columns 0&1 differ by a uniform factor k (k from 0.77
		//     to 1.18), tan(fovY/2) moves by that same k every time, to three decimals.
		//   * `aspect` reads 1.244 on 1296 of 1301 and is the field that must NOT be used to answer
		//     this: aspect is the ratio of the two scaled terms, so a UNIFORM (k,k) scale cancels
		//     out of it by construction. Aspect stability is not evidence of absorption. It was
		//     read as such once; do not repeat it.
		//
		// So the 10x pair is two genuinely different un-projections, and the difference has exactly
		// the shape of the artefact: world' - world = 0.9 * w * B^-1 * (ndc_x, ndc_y, 0), which is
		// ZERO at the screen centre and grows with both depth and screen offset. Centre stays,
		// edges at distance fly. That is the spike.
		//
		// Because the raw hash and the normalised solver therefore separate the same cameras, the
		// per-draw cache below is keyed on the RAW ring hash -- the cheap key -- and a per-window
		// census counts the DISTINCT NORMALISED SOLVERS beside the distinct raw hashes so the two
		// can never again be conflated. If `solvers` ever reads 1 on a window while `rings` reads
		// 2+, the absorption story is back and per-draw placement is a dead end for that title;
		// that is what the census is for and it costs one 12-float compare per draw.
		//
		// WHAT THIS DELIBERATELY DOES NOT CHANGE. SetupCamera stays frame-latched: the Remix
		// camera is one per present by construction, and the frame camera is a perfectly good
		// choice for it. Only the GEOMETRY moves to per-draw placement, which is the half that was
		// wrong -- a draw's vertices must be un-projected through the matrix the guest actually
		// projected them with, and nothing else.
		//
		// The contained fallback, if this ever proves unstable: latch the frame camera to the
		// window's LAST/majority ring camera instead of its first. That fixes ~99.9% of the draws
		// with none of this machinery, at the cost of the one straddling draw.

		// PCSX2_REMIX_PERDRAWCAM: 1 places each draw with its own kick camera, 0 (DEFAULT) is the
		// old frame-latched behaviour.
		//
		// DEFAULT 0, deliberately, even though the measurement above says the fix is sound. The
		// per-window census this change also adds runs UNCONDITIONALLY, so the very next run
		// reports whether a window's draws really do need more than one solver without altering a
		// single pixel -- measurement first, behaviour second, one variable per arm, which is this
		// project's standing rule after three separate arms passed their counter and failed the
		// screenshot. The knob is live and the per-game .conf is re-polled about once a second, so
		// the user can turn placement on MID-RUN by adding one conf line; no rebuild, no relaunch.
		//
		// READ LIVE, NEVER LATCHED. This is not a style preference, it is the trap that cost this
		// project a whole measured run three hours ago: PCSX2_REMIX_FRAMETRACE shipped as
		// `static const` and produced ZERO output on a run whose own log confirmed the knob was
		// applied, because the renderer goes live at t=9.774 s and the per-game .conf is applied
		// at t=10.008 s -- the latch cached 0 before the value existed and never re-read it. See
		// frametrace_frames() for the full account. Any knob a .conf delivers MUST be live.
		//
		// Logged whenever the resolved value CHANGES rather than once at first use (the ALPHASTATE
		// precedent, adapted for exactly the reason above): the first get() legitimately happens
		// before the conf is applied, so a one-shot log would report the default and be wrong in
		// the file the user reads to check the arm took.
		int per_draw_camera_mode()
		{
			static live_int value(L"PCSX2_REMIX_PERDRAWCAM", 0, 0, 1);
			static int logged = -1;

			const int mode = value.get();
			if (mode != logged)
			{
				logged = mode;
				INFO_LOG("Remix: PERDRAWCAM = {} -- {} (the per-window camera census runs either way; "
						 "read 'per-draw camera' on the stats line)",
					mode,
					(mode != 0) ? "each draw is un-projected with the camera its own XGKICK carried; "
								  "the frame-latched camera is the fallback and still drives SetupCamera" :
								  "every draw is un-projected with the frame-latched camera (pre-4A behaviour)");
			}

			return mode;
		}

		// Two clip solvers are "the same camera" when every coefficient agrees to a relative
		// tolerance. Relative to the whole block rather than per entry, because the coefficients
		// span orders of magnitude and legitimately pass through zero.
		//
		// 1e-3 is set against what it has to separate, and the gap is four orders of magnitude:
		// two cameras one game frame apart at 40 fps differ by whole percent of their rotation
		// coefficients even at a gentle turn rate, while float rounding from an exact rescale
		// differs by ~1e-7 relative. Nothing about this threshold is delicate.
		bool solvers_equivalent(const remix_ps2::clip_solver& a, const remix_ps2::clip_solver& b)
		{
			constexpr float tol = 1e-3f;

			float scale = 0.f;
			for (u32 k = 0; k < 3; ++k)
			{
				for (u32 i = 0; i < 3; ++i)
					scale = std::max(scale, std::max(std::abs(a.inverse[k][i]), std::abs(b.inverse[k][i])));
			}

			// A zero or non-finite block is not a camera and must not compare equal to anything,
			// including another zero block.
			if (!(scale > 0.f) || !std::isfinite(scale))
				return false;

			for (u32 k = 0; k < 3; ++k)
			{
				for (u32 i = 0; i < 3; ++i)
				{
					if (!(std::abs(a.inverse[k][i] - b.inverse[k][i]) <= (tol * scale)))
						return false;
				}
			}

			float bias_scale = 0.f;
			for (u32 i = 0; i < 3; ++i)
				bias_scale = std::max(bias_scale, std::max(std::abs(a.bias[i]), std::abs(b.bias[i])));

			for (u32 i = 0; i < 3; ++i)
			{
				if (!(std::abs(a.bias[i] - b.bias[i]) <= (tol * std::max(bias_scale, 1e-6f))))
					return false;
			}

			return true;
		}

		// One solved clip solver per distinct ring camera. Tiny on purpose: the trace says a window
		// contains at most two distinct kick cameras (the stale first draw's, and the current game
		// frame's), so eight slots hold several windows of history and the solve runs about twice
		// per frame -- against the 32-candidate x 14-hypothesis search resolve_world_camera already
		// does every frame, that is noise.
		//
		// A REFUSED matrix is cached too, as valid = false. Without that, a ring hash the pipeline
		// rejects would re-run the full hypothesis search on every one of the ~830 draws that carry
		// it, which would turn a fallback into a frame-time cliff.
		struct per_draw_camera_entry
		{
			u64 hash = 0; // ring matrix content hash; 0 = empty slot
			bool solved = false; // false = the acceptance pipeline refused this matrix
			u64 last_used = 0; // s_frame_counter of the last hit, for eviction
			remix_ps2::clip_solver solver{};
		};

		inline constexpr u32 per_draw_camera_slots = 8;
		per_draw_camera_entry s_per_draw_cameras[per_draw_camera_slots]{};

		// --- the per-window camera census -----------------------------------------------------
		//
		// The single most decisive number this change produces, and the answer to a question the
		// ring-hash counters structurally cannot answer: how many DISTINCT CAMERAS -- after
		// normalisation, i.e. as actual un-projections rather than as raw VU1 matrices -- did the
		// draws of one presented window get built under?
		//
		//   solvers == 1  -> every draw in the window shares one camera. Per-draw placement can
		//                    then change nothing, whatever the raw ring hashes say, and the
		//                    shattering has some other cause. Stop and re-diagnose.
		//   solvers >= 2  -> the window genuinely straddles two cameras. Branch A stands.
		//
		// `rings` is the same census over the RAW ring hashes, kept beside it on purpose: rings
		// above solvers is exactly the "same camera at two column scales" case this title turned
		// out to have (see the block comment above), and having both numbers side by side is what
		// stops the raw count ever being read as proof of multi-camera rendering again.
		//
		// Accumulated on the GS thread during the window, read and reset once in OnVSync.
		struct window_camera_census
		{
			u32 solver_count = 0;
			u32 ring_count = 0;
			bool overflowed = false; // more than the slots below could hold, in either census
			remix_ps2::clip_solver solvers[per_draw_camera_slots];
			u64 rings[per_draw_camera_slots];
		};

		window_camera_census s_window_cameras{};

		void census_note_solver(const remix_ps2::clip_solver& solver)
		{
			for (u32 i = 0; i < s_window_cameras.solver_count; ++i)
			{
				if (solvers_equivalent(s_window_cameras.solvers[i], solver))
					return;
			}

			if (s_window_cameras.solver_count >= per_draw_camera_slots)
			{
				s_window_cameras.overflowed = true;
				return;
			}

			s_window_cameras.solvers[s_window_cameras.solver_count++] = solver;
		}

		void census_note_ring(u64 ring_hash)
		{
			if (ring_hash == 0)
				return;

			for (u32 i = 0; i < s_window_cameras.ring_count; ++i)
			{
				if (s_window_cameras.rings[i] == ring_hash)
					return;
			}

			if (s_window_cameras.ring_count >= per_draw_camera_slots)
			{
				s_window_cameras.overflowed = true;
				return;
			}

			s_window_cameras.rings[s_window_cameras.ring_count++] = ring_hash;
		}

		// One-entry memo over LookupKickCamera, keyed on the transported kick sequence.
		//
		// GSKickSeq() is stamped once per completed GS PACKET (MTGS.cpp:1071 Gif_SendRemixKickSeq),
		// not once per draw, so a run of consecutive draws shares one sequence and therefore one
		// ring answer. Without this the ring is walked once per draw, and the walk steps up to 2048
		// slots on a valid-gap (RemixVU1Capture.cpp:269) -- at ~830 draws a frame that is the one
		// part of 4A that could plausibly cost GS frame time, and it is the cost the plan's risk
		// register flagged when this lookup was diagnostic-only and gated off.
		//
		// Safe by construction: the ring is append-only from the VU side, so the same sequence
		// always yields the same answer, and a slot recycled between two lookups of one sequence
		// would only make the second answer staler than the first.
		struct kick_camera_memo
		{
			u64 seq = ~0ull; // unreachable sentinel: g_kick_seq starts at 0 and only climbs
			u64 hash = 0; // 0 = the ring held nothing usable for this sequence
			float m[16] = {};
		};

		kick_camera_memo s_kick_camera_memo{};

		// GS thread. LookupKickCamera plus the content fold, memoised. Returns 0 on a ring miss.
		u64 lookup_draw_kick_camera(float (&m)[16])
		{
			const u64 seq = RemixVU1Capture::GSKickSeq();

			if (seq != s_kick_camera_memo.seq)
			{
				s_kick_camera_memo.seq = seq;
				s_kick_camera_memo.hash = 0;

				u32 offset = 0;
				if (RemixVU1Capture::LookupKickCamera(seq, s_kick_camera_memo.m, offset))
				{
					// hash_floats IS the fold the CAMERA/cm= lines and the FRAME lines use (same
					// fnv_seed and fnv_prime, same 16 floats in the same order), so a ring hash is
					// the same number everywhere and the two dump files join on it.
					s_kick_camera_memo.hash = hash_floats(s_kick_camera_memo.m, 16);
				}
			}

			std::memcpy(m, s_kick_camera_memo.m, sizeof(m));
			return s_kick_camera_memo.hash;
		}

		// Runs the SAME acceptance pipeline resolve_world_camera runs -- build_camera_hypotheses
		// (shared code, not a copy) x majorness, split_view_projection_direct, score_perspective,
		// make_clip_solver -- on one ring matrix, and applies the gates that do not need a frame's
		// worth of context.
		//
		// DEVIATION FROM THE PLAN'S LETTER, recorded because it is a deliberate one: the plan named
		// four steps and this also applies resolve's finite-eye, origin-eye and depth-scale
		// /anisotropy gates, plus the refuted-matrix set. The direction of the deviation is what
		// makes it safe -- every added gate can only REJECT, and a rejection falls back to the
		// frame solver, i.e. to exactly today's behaviour. The reason is in resolve's own comment:
		// the depth-scale check is "the gate that matters" and the only one that can catch a
		// wrong-UNIT camera on its first acceptance, and a per-draw camera gets none of the
		// frame-level corroboration (the drift guard, the extent refutation) that the frame camera
		// accumulates over frames. ~830 draws per frame ride on this; a weaker gate here than on
		// the frame camera is the "per-draw scatter worse than the disease" risk the plan names.
		bool solve_per_draw_clip(const float (&m)[16], remix_ps2::clip_solver& out)
		{
			const viewport_constants vp = s_last_viewport;
			if (!vp.valid)
				return false;

			const float width = static_cast<float>(vp.width);
			const float height = static_cast<float>(vp.height);
			const float reference_aspect = (height > 0.f) ? (width / height) : (4.f / 3.f);

			remix_ps2::mat4 raw{};
			std::memcpy(&raw.m[0][0], m, sizeof(float) * 16);

			float best_score = 0.f;
			remix_ps2::vp_split best_split{};
			remix_ps2::mat4 best_normalized = remix_ps2::mat4_identity();

			for (u32 major = 0; major < 2; ++major)
			{
				const remix_ps2::mat4 oriented = (major == 0) ? raw : remix_ps2::mat4_transpose(raw);

				// source 0 / ucode 0: the ring stores raw VU1 memory read at the pinned offset and
				// carries no ucode identity with it, so the "r6" derived hypothesis -- which is
				// keyed on one exact Rainbow Six 3 ucode hash -- is correctly not offered here.
				// On this title it never applies anyway; SOCOM's winner is the "gs" hypothesis.
				// s_camdepth_active / s_camdepth_snap_active, NOT camdepth_mode(): this runs on the
				// draw path (~830 draws a window on this title) and must not touch the environment,
				// and it must agree with the frame election about which family was enumerated.
				// resolve_world_camera refreshes both once per window, at the same instant it
				// refreshes s_camtest_active.
				camera_hypothesis hypotheses[max_camera_hypotheses];
				const u32 hypothesis_count = build_camera_hypotheses(oriented, vp, reference_aspect,
					major != 0, 0, 0, s_camdepth_active, s_camdepth_snap_active, s_camyflip_active,
					hypotheses);

				for (u32 h = 0; h < hypothesis_count; ++h)
				{
					const camera_hypothesis& hyp = hypotheses[h];

					const remix_ps2::mat4 normalized = apply_camera_hypothesis(oriented, hyp);

					// PCSX2_REMIX_CAMYFLIP = 1 is audit-only, and the frame election gates that
					// explicitly rather than trusting the score gap. This path ranks by score alone,
					// so the gap would in fact hold here -- but the two paths must not disagree
					// about which hypotheses are installable, and an invariant that holds "for now,
					// on this path" is what let the twin slip past the election under CAMTEST = 2.
					if (hyp.flip_y && s_camyflip_active < 2)
						continue;

					remix_ps2::vp_split split{};
					if (!remix_ps2::split_view_projection_direct(normalized, split, nullptr, hyp.flip_y))
						continue;

					// s_camyflip_sign_active, not camyflip_sign_policy(camyflip_mode()): same rule
					// as s_camdepth_active above -- the draw path must not read the environment, and
					// a per-draw camera scored under a different sign policy than the frame camera
					// can prefer a different hypothesis and place its draws in a different world.
					const float score = remix_ps2::score_perspective(split.projection, reference_aspect,
						s_camyflip_sign_active);
					if (!(score > 0.f))
						continue;

					// Strict >, so the FIRST hypothesis achieving the best score wins -- the same
					// tie-break resolve_world_camera uses, over the same ordering.
					if (score > best_score)
					{
						best_score = score;
						best_split = split;
						best_normalized = normalized;
					}
				}
			}

			if (!(best_score > 0.f))
				return false;

			remix_ps2::clip_solver solver{};
			remix_ps2::mat4 view_to_world{};
			if (!remix_ps2::make_clip_solver(best_normalized, solver) ||
				!remix_ps2::mat4_invert(best_split.view, view_to_world))
				return false;

			const float eye[3] = {view_to_world.m[3][0], view_to_world.m[3][1], view_to_world.m[3][2]};
			if (!std::isfinite(eye[0]) || !std::isfinite(eye[1]) || !std::isfinite(eye[2]))
				return false;

			// An eye at exactly the origin is a solver degeneracy, not a camera (resolve's own
			// finding: SOCOM produced (0,0,0) exactly when the split collapsed).
			if (std::sqrt((eye[0] * eye[0]) + (eye[1] * eye[1]) + (eye[2] * eye[2])) < 1e-4f)
				return false;

			// Depth-scale consistency, verbatim in intent from resolve_world_camera: un-projecting
			// the NDC centre at w = 1 must land ~1 unit from the eye, and the corners must not vary
			// by more than the anisotropy limit. This is the check that catches a camera whose
			// recovered world unit is 1000x the guest's -- the vertex explosion this title has
			// already produced once -- and it needs no geometry, so it works on the first solve.
			{
				static constexpr float ndc_samples[5][2] = {
					{0.f, 0.f}, {-1.f, -1.f}, {1.f, -1.f}, {-1.f, 1.f}, {1.f, 1.f}};

				float centre_scale = 0.f;
				float min_scale = std::numeric_limits<float>::max();
				float max_scale = 0.f;

				for (u32 i = 0; i < std::size(ndc_samples); ++i)
				{
					float probe[3];
					remix_ps2::solve_world_position(solver, ndc_samples[i][0], ndc_samples[i][1], 1.f, probe);

					const float dx = probe[0] - eye[0];
					const float dy = probe[1] - eye[1];
					const float dz = probe[2] - eye[2];
					const float distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));

					if (!std::isfinite(distance) || !(distance > 0.f))
						return false;

					if (i == 0)
						centre_scale = distance;

					min_scale = std::min(min_scale, distance);
					max_scale = std::max(max_scale, distance);
				}

				const float scale_limit = camera_scale_limit();
				if (!(centre_scale >= (1.f / scale_limit)) || !(centre_scale <= scale_limit))
					return false;

				if (!(max_scale <= (camera_anisotropy_limit() * min_scale)))
					return false;
			}

			out = solver;
			return true;
		}

		// The cache lookup on its own: raw ring hash -> solved solver, at most one solve per hash.
		// nullptr means the pipeline refused this matrix (a verdict that is cached too -- without
		// that, the ~830 draws carrying a refused hash would each re-run the whole hypothesis
		// search and turn a fallback into a frame-time cliff).
		const remix_ps2::clip_solver* cached_per_draw_solver(u64 ring_hash, const float (&ring_m)[16])
		{
			per_draw_camera_entry* victim = &s_per_draw_cameras[0];

			for (u32 i = 0; i < per_draw_camera_slots; ++i)
			{
				per_draw_camera_entry& entry = s_per_draw_cameras[i];

				if (entry.hash == ring_hash)
				{
					entry.last_used = s_frame_counter;
					return entry.solved ? &entry.solver : nullptr;
				}

				// Empty slots first, then least-recently-used. hash == 0 can only mean an unused
				// slot, because a ring hash of 0 is rejected by the caller and never reaches here.
				if ((entry.hash == 0 && victim->hash != 0) ||
					(entry.hash != 0 && victim->hash != 0 && entry.last_used < victim->last_used))
					victim = &entry;
			}

			++s_stats.perdraw_solved;

			victim->hash = ring_hash;
			victim->last_used = s_frame_counter;
			// Cleared before the solve so an evicted tenant's solver can never sit under a new
			// hash: solve_per_draw_clip only writes `out` on success.
			victim->solver = remix_ps2::clip_solver{};
			victim->solved = solve_per_draw_clip(ring_m, victim->solver);

			if (!victim->solved)
			{
				++s_stats.perdraw_refused;
				return nullptr;
			}

			// Canonicalise against an equivalent solver already in the cache, so two raw hashes
			// that normalise to the same camera end up holding bit-identical solvers rather than
			// two nearly-equal ones. Costs at most eight 12-float compares, once per new hash, and
			// it is what makes "one camera, one solver" true of the cache and not just of the
			// census.
			for (u32 i = 0; i < per_draw_camera_slots; ++i)
			{
				per_draw_camera_entry& entry = s_per_draw_cameras[i];

				if (&entry == victim || !entry.solved || entry.hash == 0)
					continue;

				if (solvers_equivalent(entry.solver, victim->solver))
				{
					victim->solver = entry.solver;
					break;
				}
			}

			return &victim->solver;
		}

		// GS thread, called for EVERY world-mode draw whatever PCSX2_REMIX_PERDRAWCAM says.
		//
		// The census is unconditional on purpose -- it is the measurement the next run has to
		// produce, and it must describe the frame the user is actually looking at, not a frame
		// rendered differently because measuring was switched on. Only the PLACEMENT is knob-gated.
		//
		// `ring_m` is the matrix the caller's memoised LookupKickCamera returned and `ring_hash`
		// its hash_floats fold; the caller passes both so the ring is walked at most once per draw.
		//
		// Returns the solver to un-project this draw with, or nullptr meaning "the frame solver" --
		// which is what a ring miss, a refuted matrix, a refused solve, a ring camera that
		// normalises to the frame camera, and a disabled knob all return. Falling back rather than
		// guessing is the ring's standing contract (RemixVU1Capture.h:127-129): a wrong camera
		// places geometry in view space, which is the defect this whole mechanism exists to remove.
		const remix_ps2::clip_solver* per_draw_solver(u64 ring_hash, const float (&ring_m)[16])
		{
			census_note_ring(ring_hash);

			const remix_ps2::clip_solver* candidate = nullptr;
			bool counted = false;

			if (ring_hash == 0)
			{
				// Ring miss.
				++s_stats.perdraw_fallback;
				counted = true;
			}
			else if (ring_hash == s_active_camera.matrix_hash)
			{
				// The draw was built under the camera the frame latched, so the frame solver
				// already IS its own solver. On the measured run this is about one draw per
				// window: the stale first one.
				++s_stats.perdraw_match;
				counted = true;
			}
			else if (s_refuted_matrices.count(ring_hash) != 0)
			{
				// Refuted by the extent check on an earlier frame. A matrix not trustworthy as the
				// frame camera is not trustworthy for 830 draws either.
				++s_stats.perdraw_fallback;
				counted = true;
			}
			else
			{
				candidate = cached_per_draw_solver(ring_hash, ring_m);

				if (!candidate)
				{
					++s_stats.perdraw_fallback;
					counted = true;
				}
				else if (solvers_equivalent(*candidate, s_active_camera.solver))
				{
					// Different raw matrix, SAME un-projection. This is the trap-detector: on a
					// title whose acceptance pipeline normalises the difference away, this counter
					// carries the draws and per-draw placement is a no-op that a raw ring-hash
					// count would have reported as a 99.9% hit rate. Use the frame solver, which
					// is the identical answer and costs nothing.
					++s_stats.perdraw_same_solver;
					candidate = nullptr;
					counted = true;
				}
			}

			if (!counted)
				++s_stats.perdraw_distinct;

			census_note_solver(candidate ? *candidate : s_active_camera.solver);

			// Measured either way; placed only when asked.
			return (candidate && per_draw_camera_mode() != 0) ? candidate : nullptr;
		}

		void submit_debug_triangle()
		{
			const remixapi_Interface& api = s_remix.api();

			if (!s_debug_mesh)
				return;

			remixapi_InstanceInfo instance{};
			instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
			instance.pNext = nullptr;
			instance.categoryFlags = 0;
			instance.mesh = s_debug_mesh;
			instance.transform = s_identity_transform;
			instance.doubleSided = 1;

			remix_ps2::guarded_draw_instance(api.DrawInstance, &instance);
		}

		// Lazy first-frame init. Runs on the GS thread, which is where every later submit and
		// the Present also run.
		// Consecutive frames the render window's client rect must be unchanged before Remix is
		// allowed to bind it.
		//
		// MEASURED AND DEFAULTED OFF. The theory below is plausible and was the best remaining
		// lead, but a 10-launch-per-arm A/B on the worst-case state (SOCOM slot 2) came back
		// 1/10 survived at delay 0 versus 2/10 at delay 60 -- indistinguishable from noise. The
		// knob is kept because it is the only way to re-test the idea cheaply, but do NOT treat
		// it as a fix and do not re-run a three-launch version of this experiment.
		//
		// We force the device Surfaceless and hand Remix the raw HWND. If Qt has not finished
		// realising and sizing that window when Startup() runs, the swapchain dxvk-remix builds
		// against it is poisoned from birth: two observed deaths printed
		// "[D3D9WindowProc] Swapchain handle is invalid" before exiting, and *every* device-loss
		// exit lands within a fraction of a second of "renderer is live", never later. Sessions
		// that clear that window run for minutes.
		u32 startup_stable_frames()
		{
			static const u32 value =
				static_cast<u32>(std::max<s64>(0, remix_ps2::read_env_int(L"PCSX2_REMIX_STARTUPDELAY", 0)));
			return value;
		}

		RECT s_last_startup_rect{};
		u32 s_stable_frames = 0;

		// True once the window has held one size for startup_stable_frames() consecutive calls.
		bool window_settled()
		{
			const u32 required = startup_stable_frames();
			if (required == 0)
				return true;

			RECT rc{};
			if (!s_hwnd || !GetClientRect(s_hwnd, &rc) || (rc.right - rc.left) <= 0 || (rc.bottom - rc.top) <= 0)
			{
				s_stable_frames = 0;
				return false;
			}

			// A window Qt has not shown yet is not a window Remix should bind.
			if (!IsWindowVisible(s_hwnd))
			{
				s_stable_frames = 0;
				return false;
			}

			if (rc.left != s_last_startup_rect.left || rc.top != s_last_startup_rect.top ||
				rc.right != s_last_startup_rect.right || rc.bottom != s_last_startup_rect.bottom)
			{
				s_last_startup_rect = rc;
				s_stable_frames = 0;
				return false;
			}

			return (++s_stable_frames) >= required;
		}

		void ensure_initialized()
		{
			if (s_init_attempted)
				return;

			if (!s_hwnd)
			{
				s_init_attempted = true;
				ERROR_LOG("Remix: no game window was stashed, rendering is disabled");
				return;
			}

			// Deliberately before s_init_attempted is set: this is a "not yet", not a failure.
			if (!window_settled())
				return;

			s_init_attempted = true;

			INFO_LOG("Remix: window settled at {}x{} after {} stable frames",
				s_last_startup_rect.right - s_last_startup_rect.left,
				s_last_startup_rect.bottom - s_last_startup_rect.top, s_stable_frames);

			// The close path clears s_init_attempted so a reopen rebuilds the scene, but
			// runtime::initialize() short-circuits once started -- it would still be bound to the
			// window that has since been destroyed. Refusing here keeps that from coming back to
			// life as a fault storm.
			if (s_window_lost)
				return;

			if (!s_remix.initialize(s_hwnd))
			{
				ERROR_LOG("Remix: runtime unavailable, degrading to a no-op");
				// Nothing will ever latch the VU1 candidates now, so stop paying for them.
				RemixVU1Capture::SetArmed(false);
				return;
			}

			// Load the title environment before create_debug_scene() creates any lights. The first
			// frame used to build the GUI-default key, then apply SCUS-97545.conf only after Present;
			// DestroyLight is asynchronous, so avoiding that transient handle entirely is safer than
			// relying on its removal from the runtime's next scene.
			remix_ps2::materials::refresh_categories();
			remix_ps2::materials::refresh_game_config(s_remix);
			remix_ps2::paths::apply_live_knobs();

			if (no_debug_scene())
			{
				WARNING_LOG("Remix: PCSX2_REMIX_NODEBUGSCENE -- no debug mesh and no debug light "
							"will be created (crash bisection arm C)");
			}
			else if (!create_debug_scene())
			{
				ERROR_LOG("Remix: debug scene setup failed, degrading to a no-op");
				RemixVU1Capture::SetArmed(false);
				return;
			}

			s_active_hwnd = s_hwnd;
			s_live = true;
			remix_ps2::materials::begin_frame();
			INFO_LOG("Remix: renderer is live (far plane {}, materials {})",
				remix_ps2::hardcoded_far_plane(),
				s_remix.fork_features() ? "on" : "off (fork features unavailable)");
		}

		bool armed()
		{
			return g_armed;
		}

		// Frames a mesh may go unreferenced, and a ceiling on how many may be resident.
		//
		// Both exist because of a confirmed failure, not as tuning: with the stock RPCS3
		// numbers the Remix runtime reaches VK_ERROR_DEVICE_LOST and exits (its own log says
		// "DxvkSubmissionQueue: Exiting after GPU device loss", process exit 0x60D0DEAD).
		// A PS2 frame submits an order of magnitude more draws than an RSX one, so 300 frames
		// of retention means thousands of live acceleration structures and tens of thousands
		// of builds per second.
		u64 mesh_idle_frames()
		{
			static const u64 value =
				static_cast<u64>(std::max<s64>(1, remix_ps2::read_env_int(L"PCSX2_REMIX_MESHIDLE", 120)));
			return value;
		}

		// Smallest w (= 1/Q, the guest's own perspective divisor) a draw's furthest vertex may
		// have and still be submitted.
		//
		// Set from the measured distribution, not guessed. Rainbow Six 3, 2,100 frames, max w
		// per submitted draw:
		//   <1e-3: 7   <1e-2: 111   <0.1: 1350   <1: 5760   <10: 28094   <100: 2679   >=100: 0
		// The body of the scene is 1-10 and the first-person weapon -- the thing that must not
		// be culled by accident -- sits in the 0.1-1 bucket. 0.01 is an order of magnitude below
		// that and removes only ~118 draws in 2,100 frames, which are the ones collapsing onto
		// the eye plane. 0 disables the gate.
		float min_submitted_w()
		{
			static const float value = []() -> float {
				const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_MINW");
				if (env.empty())
					return 0.01f;

				const float parsed = static_cast<float>(::_wtof(env.c_str()));
				return (std::isfinite(parsed) && parsed >= 0.f) ? parsed : 0.01f;
			}();

			return value;
		}

		// The near-vertex twin of min_submitted_w(): reject a draw any of whose vertices falls
		// below this w, rather than one whose furthest vertex does. A draw can span [0.007, 3.3]
		// -- fine at the far end, degenerate at the near end -- and the max-w gate above passes it
		// untouched.
		//
		// 0 (the default) disables it. It is off by default because it rejects whole draws and the
		// first-person weapon genuinely sits near the eye; the per-draw dump's w range is what a
		// value should be set from.
		float min_vertex_w()
		{
			static const float value = []() -> float {
				const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_MINVW");
				if (env.empty())
					return 0.f;

				const float parsed = static_cast<float>(::_wtof(env.c_str()));
				return (std::isfinite(parsed) && parsed >= 0.f) ? parsed : 0.f;
			}();

			return value;
		}

		// The far-field twin of min_submitted_w(): reject a draw whose *nearest* vertex is beyond
		// this w, so nothing in the frame sits further out than the cap. 0 (the default) disables
		// it and nothing changes.
		//
		// This exists to move the one variable no arm has ever moved. SOCOM slot 2 submits 838
		// draws in a frame with a median of ONE triangle each and w spanning 16.7 to 3,440 -- a
		// TLAS of single-triangle instances whose bounds overlap across a 200x depth range,
		// including single triangles hundreds of screen pixels wide at w ~ 2,000 sitting beside
		// geometry at w ~ 17. Capping w collapses that spread while leaving vertex data,
		// packaging, identity, instancing and materials untouched, which is what makes it a clean
		// arm rather than another confound.
		//
		// Deliberately gated on min_w, not max_w: a draw that straddles the cap is kept, so the
		// gate removes only geometry lying entirely in the far field.
		float max_submitted_w()
		{
			static const float value = []() -> float {
				const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_MAXW");
				if (env.empty())
					return 0.f;

				const float parsed = static_cast<float>(::_wtof(env.c_str()));
				return (std::isfinite(parsed) && parsed >= 0.f) ? parsed : 0.f;
			}();

			return value;
		}

		// How many eye-depths across a draw may be before it is called hyperextended: the ratio of
		// its AABB diagonal to its own furthest w. Nothing is rejected on this -- it only counts and
		// dumps -- because it exists to answer whether the "vertex explosions" the user reports are
		// geometry we submit at all, and a gate would delete the evidence it was set to gather.
		//
		// 32 is the default, and it sits between two measurements rather than being picked. Ordinary
		// geometry legitimately reaches ~17x: GS XY is 12.4 fixed point spanning +-4096 px against a
		// 640-wide render target, so a wall whose vertices lie several screens off to the side is
		// genuinely many times wider than the depth it sits at. The only real explosion this project
		// has ever measured -- a wrong VU1 matrix accepted as the world camera -- scattered the scene
		// 1,450x. Anything between those two is ambiguous, and the session peak printed on the stats
		// line is what moves the line without a rebuild. 0 disables the counter.
		float explode_ratio_limit()
		{
			static const float value = []() -> float {
				const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_EXPLODEK");
				if (env.empty())
					return 32.f;

				const float parsed = static_cast<float>(::_wtof(env.c_str()));
				return (std::isfinite(parsed) && parsed >= 0.f) ? parsed : 32.f;
			}();

			return value;
		}

		// Quantum, in world units, that positions are snapped to before they are hashed into a
		// mesh identity. 0 disables quantization and restores exact-bit hashing, which is the
		// A/B handle for the measurement. The default is deliberately coarse relative to the
		// camera's solve jitter but fine relative to real geometry: this title's submitted
		// scene spans ~6-8 world units, so 0.01 is ~0.15% of the scene.
		// --- stable mesh identity -----------------------------------------------------------
		//
		// The legacy identity hashes the submitted positions, which are derived from the
		// recovered camera. That mints a fresh handle for a static surface whenever the camera
		// moves, so every BLAS is one-shot -- measured at 20/20 survival when instancing is
		// switched off entirely against 1/20 with it on (SOCOM slot 2, phase 3k-3m).
		//
		// Quantizing the positions, which is what shipped, cannot fix it: rounding is not a
		// stable function under noise. A vertex sitting near a bucket boundary flips buckets for
		// an arbitrarily small perturbation, and with a hundred vertices per draw the chance that
		// at least one flips is close to certain. Identity has to come from something that is
		// exactly reproducible, with the geometry compared by a metric rather than hashed.
		//
		// So: key on what the guest emits bit-exactly (vertex and index counts, the index
		// buffer, vertex colours, the material) plus a log-quantized size, then confirm the match
		// by rigidly registering the candidate's stored geometry against the draw's.
		//
		//   0 = legacy position hashing (the default), 1 = stable identity.
		//
		// DEFAULTED OFF, and the reason is a measurement rather than caution. This was built to
		// fix the device loss, on the phase-3m reading that the runtime cannot sustain instancing
		// many one-shot BLASes. That reading is wrong -- see the pinned-pool arm in notes.md --
		// and 20 launches on SOCOM slot 2 give 1/20 with it on against a 4/20 control in the same
		// sitting. It does what it says (mesh creations fall ~2.6x on Rainbow Six 3, 80% of draws
		// re-instance an existing handle) and it is what gives the runtime usable motion vectors,
		// but it does not buy survival, and it has not been visually verified beyond "the scene
		// still renders in the right place". Turn it on deliberately.
		int stable_identity()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_STABLEID", 0), 0, 1));
			return value;
		}

		// Registration residual, as a fraction of the draw's own radius of gyration, below which
		// two point sets are called the same geometry. Relative rather than absolute because a
		// draw may be a 0.1-unit decal or a 20-unit wall in the same frame, and because the
		// un-projection's error scales with depth.
		float identity_tolerance()
		{
			static const float value = []() -> float {
				const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_IDTOL");
				if (env.empty())
					return 0.02f;

				const float parsed = static_cast<float>(::_wtof(env.c_str()));
				return (std::isfinite(parsed) && parsed > 0.f) ? parsed : 0.02f;
			}();

			return value;
		}

		// Open-addressing depth for the identity key. Two genuinely different objects can share
		// counts, indices and colours -- untextured white vertex colours make that common -- so
		// a key needs somewhere to put the loser instead of thrashing one entry.
		u64 identity_slots()
		{
			static const u64 value = static_cast<u64>(
				std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_IDSLOTS", 4), 1, 16));
			return value;
		}

		// Whether vertex colours take part in the key. They come straight out of GSVertex and are
		// bit-exact, but a title that recomputes per-vertex lighting or fog every frame would
		// re-key everything through them. Kept as a handle for that case.
		bool identity_use_color()
		{
			static const bool value = remix_ps2::read_env_int(L"PCSX2_REMIX_IDCOLOR", 1) != 0;
			return value;
		}

		float mesh_hash_quantum()
		{
			// Not env_float(): 0 is a meaningful value here (exact hashing) and env_float
			// treats anything <= 0 as "unset".
			static const float value = []() -> float {
				const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_MESHQUANT");
				if (env.empty())
					return 0.01f;

				const float parsed = static_cast<float>(::_wtof(env.c_str()));
				return (std::isfinite(parsed) && parsed >= 0.f) ? parsed : 0.01f;
			}();

			return value;
		}

		// How the PS2's per-draw alpha state is handed to the runtime.
		//
		//   0 = tell the runtime to ignore draw-call alpha state entirely
		//   1 = claim to supply it and then not (what shipped, and it is a bug -- see below)
		//   2 = supply a real remixapi_InstanceInfoBlendEXT translated from the GS registers
		//
		// The material sets useDrawCallAlphaState = 1, which remix_c.h:255-256 documents as
		// "InstanceInfoBlendEXT is used as a source for alpha state" -- but that struct was
		// never chained into the instance's pNext. Every draw in this title has ABE=1, so every
		// surface was being blended against whatever the runtime found in place of a struct we
		// never provided. 0 and 1 exist to A/B that; 2 is the destination, because PS2 blending
		// genuinely matters for glass, decals and effects.
		// See the use site: separates the texture-stage fields of InstanceInfoBlendEXT from its
		// blend fields, so colour can be chased without giving up the alpha-state fix.
		// How many scene radii the recovered eye may be from the submitted geometry's centre
		// before the camera is refused. Deliberately generous: the geometry it is checked
		// against was submitted through the *previous* camera, so this is a sanity gate against
		// a degenerate solve, not a precision test.
		float camera_distance_limit()
		{
			static const float value = env_float(L"PCSX2_REMIX_CAMDIST", 50.f);
			return value;
		}

		// How far the recovered world space's unit may be from the guest's own, before the
		// camera is refused. See check_depth_scale: for a rigid view transform the two are the
		// same, so this is a wide tolerance around 1 rather than a fitted number.
		//
		// Returns the value resolve_world_camera refreshed at the top of this window. It is NOT a
		// `static const` env read any more -- see s_camera_scale_limit for why that had never once
		// let a per-game .conf move this gate.
		float camera_scale_limit()
		{
			return s_camera_scale_limit;
		}

		// How much that unit may vary between the centre of the frame and its corners. A well
		// conditioned perspective un-projection varies by 1/cos(half-diagonal-fov) -- 1.11 at
		// Rainbow Six 3's 36 degrees, about 2.9 even at 113 degrees. Anything beyond this is a
		// near-singular solve, which is what smears geometry into bands.
		float camera_anisotropy_limit()
		{
			static const float value = env_float(L"PCSX2_REMIX_CAMANISO", 8.f);
			return value;
		}

		// How many times the frame's largest eye-space depth the submitted scene's radius may
		// reach before the world camera is refused. Geometry at depth w sits at most w/cos(fov/2)
		// from the eye, so a correct camera keeps this near 1-3 even at a wide field of view;
		// Rainbow Six 3 measures well under it and SOCOM's bad matrix measures ~970.
		float camera_extent_limit()
		{
			static const float value = env_float(L"PCSX2_REMIX_CAMEXTENT", 10.f);
			return value;
		}

		// Auto-classification of alpha-tested draws as cut-outs. 0 disables it.
		int cutout_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_CUTOUT", 1), 0, 1));
			return value;
		}

		int texture_stage_mode()
		{
			static live_int value(L"PCSX2_REMIX_TEXSTAGE", 1, 0, 1);
			return value.get();
		}

		int alpha_state_mode()
		{
			// Logged on first use because this knob is invisible in every counter: mode 2 is the
			// only thing that chains remixapi_InstanceInfoBlendEXT onto the instance at all
			// (instance.pNext below), so at 0 every blended draw reaches Remix as opaque and the
			// untextured material's 4x4 WHITE albedo composites as a solid white shard. There is no
			// stat that moves when this is wrong -- only the picture -- which is exactly the kind of
			// silent setting that has cost this project whole sessions. Say what it resolved to.
			static const int value = []() {
				const int v =
					static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_ALPHASTATE", 2), 0, 2));
				INFO_LOG("Remix: ALPHASTATE = {} -- {}", v,
					(v == 2) ? "InstanceInfoBlendEXT attached per draw (blend, alpha test, "
							   "vertex-colour baked-lighting flag all live)" :
							   "NO blend state sent; blended draws composite opaque and "
							   "untextured ones render as white shards");
				return v;
			}();
			return value;
		}

		// PS2 ATST -> VkCompareOp.
		//
		// NOT D3DCMPFUNC, despite the D3D9-shaped API. rtx_remix_api.cpp:946 casts this field
		// STRAIGHT to VkCompareOp with no translation, and the two enums differ by one
		// throughout (D3D NEVER=1 vs VK_COMPARE_OP_NEVER=0). Sending D3D values shifted every
		// alpha test by one comparison.
		// GS_ATST: NEVER 0, ALWAYS 1, LESS 2, LEQUAL 3, EQUAL 4, GEQUAL 5, GREATER 6, NOTEQUAL 7.
		// VkCompareOp: NEVER 0, LESS 1, EQUAL 2, LESS_OR_EQUAL 3, GREATER 4, NOT_EQUAL 5,
		//              GREATER_OR_EQUAL 6, ALWAYS 7.
		u32 to_d3d_compare(u32 atst)
		{
			switch (atst)
			{
				case 0: return 0; // NEVER
				case 1: return 7; // ALWAYS
				case 2: return 1; // LESS
				case 3: return 3; // LESS_OR_EQUAL
				case 4: return 2; // EQUAL
				case 5: return 6; // GREATER_OR_EQUAL
				case 6: return 4; // GREATER
				default: return 5; // NOT_EQUAL
			}
		}

		// The GS blend equation is (A - B) * C + D with A/B/D in {Cs, Cd, 0} and C in
		// {As, Ad, FIX}. D3D9's fixed src*srcFactor + dst*dstFactor cannot express all of it, so
		// this covers the cases that actually occur and falls back to a plain alpha blend --
		// stated plainly because it is an approximation, not a translation.
		void to_d3d_blend(const GIFRegALPHA& alpha, u32& src_factor, u32& dst_factor)
		{
			// VkBlendFactor, NOT D3DBLEND: rtx_remix_api.cpp:957-962 casts these fields straight to
			// VkBlendFactor. ZERO 0, ONE 1, SRC_ALPHA 6, ONE_MINUS_SRC_ALPHA 7, DST_ALPHA 8,
			// ONE_MINUS_DST_ALPHA 9. Sending D3D values turned every alpha blend in the game into
			// a different equation entirely (D3D SRCALPHA=5 reads as ONE_MINUS_DST_COLOR).
			const u32 c_factor = (alpha.C == 0) ? 6u : (alpha.C == 1) ? 8u : 1u; // As / Ad / FIX~ONE
			const u32 inv_c_factor = (alpha.C == 0) ? 7u : (alpha.C == 1) ? 9u : 0u;

			// A == B makes the (A - B) term identically zero, so the result is D alone and the
			// C factor is irrelevant. R6 3's projected wall shadows draw with A0 B0 C0 D1, i.e.
			// "output = destination" -- they contribute nothing and exist only to exercise the
			// GS pipeline. Falling through to the alpha-blend default below rendered them as an
			// opaque SRCALPHA lerp of an untextured (white) surface, which is the white bodies
			// standing proud of the wall. Map the degenerate case faithfully instead.
			// NARROWED (2026-08-25): only D == 1 is handled here. Mapping the other two D values
			// faithfully -- D0 to an opaque source replacement, D2 to black -- flipped a large
			// population of ordinary draws off alpha blending and made the image flicker hard.
			// D1 is the case actually diagnosed (R6 3's shadow decals, A0 B0 C0 D1 = "output is
			// the destination", a draw that contributes nothing), and it is unambiguous. The
			// other two fall through to the historical default until someone measures them.
			if (alpha.A == alpha.B && alpha.D == 1)
			{
				// (A - B) is identically zero, so the result is D alone: keep the destination.
				src_factor = 0u; // VK_BLEND_FACTOR_ZERO
				dst_factor = 1u; // VK_BLEND_FACTOR_ONE
			}
			else if (alpha.A == 0 && alpha.B == 1 && alpha.D == 1)
			{
				// Cs*C + Cd*(1-C): the standard blend.
				src_factor = c_factor;
				dst_factor = inv_c_factor;
			}
			else if (alpha.A == 0 && alpha.B == 2 && alpha.D == 1)
			{
				// Cs*C + Cd: additive.
				src_factor = c_factor;
				dst_factor = 1u; // VK_BLEND_FACTOR_ONE
			}
			else if (alpha.A == 0 && alpha.B == 2 && alpha.D == 2)
			{
				// Cs*C: modulate against nothing.
				src_factor = c_factor;
				dst_factor = 0u; // VK_BLEND_FACTOR_ZERO
			}
			else
			{
				src_factor = c_factor;
				dst_factor = inv_c_factor;
			}
		}

		// Ceiling on CreateMesh calls in a single frame; 0 (the default) is unlimited, so this
		// is inert until it is deliberately turned on. It exists because the remaining
		// device-loss reports are all "while walking", and camera translation re-hashes every
		// static mesh -- so the creation *rate*, not the live count, is the untested variable.
		u64 mesh_create_budget()
		{
			static const u64 value =
				static_cast<u64>(std::max<s64>(0, remix_ps2::read_env_int(L"PCSX2_REMIX_MESHBUDGET", 0)));
			return value;
		}

		size_t mesh_live_cap()
		{
			static const size_t value =
				static_cast<size_t>(std::max<s64>(64, remix_ps2::read_env_int(L"PCSX2_REMIX_MESHCAP", 4096)));
			return value;
		}

		void reap_idle_meshes()
		{
			const u64 idle_frames = mesh_idle_frames();
			const remixapi_Interface& api = s_remix.api();

			// Over the ceiling: evict least-recently-used until back under it, regardless of
			// age. Without this a burst of new geometry can outrun the age-based reap.
			while (s_meshes.size() > mesh_live_cap())
			{
				// Pinned entries are skipped rather than selected-then-refused: with a pool of
				// them, refusing on the single oldest would abandon the whole eviction pass and
				// let the cache grow without bound.
				auto oldest = s_meshes.end();
				for (auto it = s_meshes.begin(); it != s_meshes.end(); ++it)
				{
					if (it->second.handle == s_reuse_handle || s_pinned_hashes.count(it->first) != 0)
						continue;

					if (oldest == s_meshes.end() || it->second.last_used_frame < oldest->second.last_used_frame)
						oldest = it;
				}

				if (oldest == s_meshes.end())
					break; // everything resident is pinned

				if (oldest->second.handle)
				{
					remix_ps2::guarded_destroy_mesh(api.DestroyMesh, oldest->second.handle);
					++s_stats.meshes_destroyed;
					++s_stats.meshes_destroyed_frame;
				}

				s_meshes.erase(oldest);
			}

			if (s_frame_counter < idle_frames)
				return;

			const u64 cutoff = s_frame_counter - idle_frames;

			for (auto it = s_meshes.begin(); it != s_meshes.end();)
			{
				if (it->second.last_used_frame > cutoff)
				{
					++it;
					continue;
				}

				if (it->second.handle == s_reuse_handle || s_pinned_hashes.count(it->first) != 0)
				{
					++it;
					continue;
				}

				if (it->second.handle)
				{
					remix_ps2::guarded_destroy_mesh(api.DestroyMesh, it->second.handle);
					++s_stats.meshes_destroyed;
					++s_stats.meshes_destroyed_frame;
				}

				it = s_meshes.erase(it);
			}
		}

		// --- sky categorisation -------------------------------------------------------------
		//
		// A PS2 skybox is drawn with the depth buffer disconnected: it neither tests Z (nothing
		// can occlude it) nor writes Z (nothing it covers is nearer), and it is drawn before the
		// world. Passing categoryFlags = 0 for it -- which is what every instance did until now
		// -- makes Remix treat a box a few units across, centred on the camera, as real
		// geometry, so the player is looking at the inside of it. Tagging it SKY moves it to the
		// background at infinity.
		//
		// PCSX2_REMIX_SKY:      0 = off
		//                       1 = depth-neutral (default): the draw must neither test nor write Z
		//                       2 = draw order only: the first SKYORDER draws are sky whatever their
		//                           depth state. This is dxvk-remix's own rtx.skyDrawcallIdThreshold
		//                           rule, and it exists because the depth-neutral test is not
		//                           universal -- a title that draws its sky with Z testing on can
		//                           never be caught by mode 1, no matter what SKYORDER is set to.
		// PCSX2_REMIX_SKYORDER: how many leading gate-passing draws of the frame are eligible.
		//                       Narrows mode 1; REQUIRED by mode 2. STOPS BINDING in modes 1/3/4
		//                       once SKYMINW is armed -- see sky_min_w().
		// PCSX2_REMIX_SKYMINW:  minimum eye depth a draw must sit entirely beyond. 0 = off.
		int sky_mode()
		{
			static live_int value(L"PCSX2_REMIX_SKY", 1, 0, 4);
			return value.get();
		}

		u32 sky_order_limit()
		{
			static live_int value(L"PCSX2_REMIX_SKYORDER", 0, 0, 100000);
			return static_cast<u32>(value.get());
		}

		// PCSX2_REMIX_SKYMINW -- the far-distance requirement on sky classification, in w (eye
		// depth) units. 0 = OFF, the default, and with it off every mode behaves exactly as it did
		// before this knob existed, on every title.
		//
		// ARMING THIS CHANGES THE PICTURE, and it must: a classified draw stops being placed in
		// the world. It is un-projected with the bias-zeroed sky solver and rendered through the
		// translation-stripped sky camera instead. That IS the fix being asked for, and nothing
		// about it is cosmetic or invisible.
		//
		// WHY IT EXISTS -- the draw ORDINAL was measured to be the wrong discriminator, and the
		// measurement is what retracted the previous attempt. Combined Assault (SCUS-97545),
		// gameplay dump, frames 1992-1996, mode 4 with SKYORDER = 20: what it tagged was
		// near-field terrain -- chunks drawn as an opaque pass immediately followed by a
		// render-target overlay pass at the SAME screen rect and the SAME depth:
		//
		//   d=16,17 opaque + d=18,19 overlay     px=[476,567]x[217,332]   w=[45,103]
		//   d=20,21 opaque + d=22,23 overlay     px=[479,783]x[237,332]   w=[30,54]
		//   d=24,25 opaque + d=26,27 overlay     px=[479,783]x[328,470]   w=[23,45]
		//   d=28,29 opaque + d=30,31,32 overlay  px=[409,476]x[217,259]   w=[103,194]
		//
		// Everything mode 4 caught sat at eye depth 23..194. The same frame's genuinely far-field
		// draws reach w = 4021, 4175, 6220, and SOCOM 1's backdrop dome (SCUS-97134, d=202, 144
		// verts, 48 tris, covering over twice the framebuffer) sits at max w = 5615. So the dome
		// was never classified and the terrain overlays were -- which is WORSE than doing nothing:
		// handing a near-field overlay to the sky camera anchors it at the origin, detached from
		// the terrain it belongs to, and a bright camera-relative overlay is exactly the reported
		// "light behind me that follows me". Turning SKY on is what introduced it.
		//
		// Note the ordering, because it is the whole argument against the ordinal: the harmful
		// draws are d=16..32 and the dome is d=202. The ordinal is not merely redundant here, it
		// is ANTI-CORRELATED -- any SKYORDER large enough to admit the dome admits every terrain
		// overlay first, and any SKYORDER small enough to exclude the overlays excludes the dome.
		// No value of it separates the two. w does: 194 and 4021 are a factor of 20 apart.
		//
		// READ LIVE, and cached per presented frame rather than per call. Not `static const` -- the
		// renderer goes live ~0.2 s before the per-game .conf is applied, so a latched value is the
		// pre-conf value forever. Not live_int either -- that re-parses only when
		// paths::knob_generation() moves, and the per-game .conf never moves it (it calls
		// SetEnvironmentVariableW directly). Both are separately-diagnosed silent no-ops on this
		// project; see env_int_live. The per-frame cache exists because classify_sky IS the
		// per-draw path and a GetEnvironmentVariableW there is not free -- this is
		// refresh_user_pin() (RemixVU1Capture.cpp:108) applied to a hot-path value, refreshed from
		// OnVSync immediately after refresh_game_config() so a conf-delivered value lands on the
		// very next frame's draws.
		float s_sky_min_w = 0.f;

		void refresh_sky_min_w()
		{
			s_sky_min_w = static_cast<float>(std::max(0, env_int_live(L"PCSX2_REMIX_SKYMINW", 0)));
		}

		float sky_min_w()
		{
			return s_sky_min_w;
		}

		// Whether a draw's texture source is a render target. Same test RemixMaterials uses, kept
		// here rather than duplicated inline so the two cannot drift into disagreeing.
		bool draw_samples_render_target(const void* tex_source)
		{
			const GSTextureCache::Source* const src =
				static_cast<const GSTextureCache::Source*>(tex_source);
			return src != nullptr && (src->m_target || src->m_from_target);
		}

		// 'depth_read'/'depth_write' come from GSRendererHW's own DepthRead()/DepthWrite()
		// (GSRendererHW.h:62-79), evaluated by the caller because only OnDrawPrims is a friend
		// of GSRendererHW. Reading the registers here instead would be a second, drifting
		// interpretation of the same state.
		//
		// 'draw_min_w' is the draw's NEAREST vertex in eye depth, w = 1/Q. Passed in rather than
		// derived here because the two call sites reach it from different places: the submission
		// path has to classify BEFORE the vertex loop (the classification picks the solver that
		// loop un-projects with) and so scans for it, while build_draw_state has the loop's own
		// min_w already. Both feed the same number in, so both readings agree.
		bool classify_sky(bool depth_read, bool depth_write, u64 draw_ordinal, bool samples_target,
			float draw_min_w)
		{
			const int mode = sky_mode();
			if (mode == 0)
				return false;

			// The far-distance requirement, applied first and to EVERY mode. See sky_min_w() for
			// the measurement that made it necessary.
			//
			// MIN w, not max w and not the w RANGE, and that choice is load-bearing:
			//
			//   * min_w > limit means "every vertex of this draw is beyond the limit", i.e. the
			//     draw lies entirely in the far field. This is the same reading of the same
			//     variable the far-field submission gate already uses (`min_w > max_submitted_w()`
			//     at the eye-plane gate), so the two cannot drift into disagreeing about what
			//     "entirely beyond X" means.
			//   * max_w would admit any draw that merely REACHES the far field. A ground sheet
			//     running from under the player's feet out to the skyline has a huge max w, and
			//     detaching THAT from the world is the precise failure this knob exists to stop.
			//   * the range (max - min) describes a draw's SHAPE, not its distance. A near overlay
			//     spanning w=[23,45] and a far one spanning w=[4021,4175] have comparable ranges,
			//     so a range test cannot separate them at all.
			//
			// A dome does span depth -- its rim is nearer than its crown -- which is why this is a
			// floor to clear rather than a band to sit in. SOCOM 1's dome runs out to max w 5615
			// and Combined Assault's far field to 4021..6220, so a limit in 1000..2000 clears the
			// whole backdrop while excluding the 23..194 terrain overlays by a factor of five.
			// If a dome fails to tag, its own `w=[min,max]` is on its DRAWDUMP line: retune from
			// that, do not guess.
			const float min_w_required = sky_min_w();
			if (min_w_required > 0.f && !(draw_min_w > min_w_required))
				return false;

			// SKYORDER, and whether it still binds.
			//
			// KEPT, not deleted. Mode 2 IS the ordinal rule -- it is dxvk-remix's own
			// rtx.skyDrawcallIdThreshold and has no other test -- and confs already in the field
			// set it (SLES-51180 = 3, SLUS-20820 = 6, SCUS-97545 = 20). Removing the knob would
			// silently change those titles.
			//
			// But it must STOP BINDING in the depth modes as soon as a distance test is armed, or
			// arming SKYMINW on a title whose conf already carries SKYORDER = 20 is a guaranteed
			// no-op: the dome is d=202 and would be cut on the ordinal before the distance test
			// ever ran. A knob that reports itself applied and does nothing is the failure mode
			// this file has paid for most often. Mode 2 keeps the ordinal because mode 2 has
			// nothing else to be.
			const u32 limit = (min_w_required > 0.f && mode != 2) ? 0u : sky_order_limit();

			// Mode 3: no depth WRITE and the draw samples a render target. Depth READ is allowed,
			// which is the whole point -- mode 1 demands neither read nor write, and SOCOM's sky is
			// depth(r=1 w=0), so mode 1 can never match it and never has.
			//
			// MEASURED on Winterblade: the sky is draws 12-13 of the frame -- a 620x264 blended
			// quad, ZTST=2 ZMSK=1, ABE=1, sampling a 128x128 target=1 texture with UVs at
			// [-0.417,3.229], i.e. tiled scrolling clouds. Nothing else on the main framebuffer
			// combines "samples a render target" with "writes no depth" once PCSX2_REMIX_MINRT has
			// gated the off-screen pass that produces that target.
			//
			// Why it matters beyond tagging: the un-projection above adds the eye translation to
			// every vertex, which is right for world geometry and wrong for a backdrop the guest
			// drew with a translation-free matrix. The bias-zeroing that corrects it is gated on
			// this function, so an unclassified sky rides the camera -- the "sun/moon follows me
			// from behind" already described at the sky_solver comment.
			if (mode == 3)
				return !depth_write && samples_target && ((limit == 0) || (draw_ordinal < limit));

			// Mode 4: the backdrop band. With a measured distance floor armed, the guest submits
			// paired color/depth halves at the same screen bounds; both must use the sky solver.
			// Without that floor, retain the historical depth-write guard for other titles/configs.
			//
			// Needed because a by-hash tag CANNOT do this job. rtx.skyBoxTextures sets the Remix
			// instance CATEGORY, but the thing that actually anchors a backdrop is the sky_solver's
            // zeroed bias, chosen at RemixSubmit.cpp:4274 -- and materials::bind() does not run until
			// :4656, so no material hash exists yet at the point the solver is picked. Geometry has
			// to be classified by geometry.
			//
			// SKYORDER WAS the whole safety margin here, and it was the wrong one. REFUTED
			// 2026-08-16 on Combined Assault: mode 4 + SKYORDER = 20 tagged near-field terrain
			// overlays at w = 23..194 and never reached the backdrop at all, because the ordinal
			// puts the harmful draws (d=16..32) BEFORE the dome (d=202). The safety margin is now
			// SKYMINW, a far-distance floor, and when it is armed the ordinal above is released to
			// 0 so it cannot cut the dome off first. Full measurement at sky_min_w().
			if (mode == 4)
			{
				// The floor check above already proved every vertex is in the measured far field.
				// Highwire's textured depth-write=1 color half is immediately paired with a
				// depth-write=0 half; leaving the former world-classified splits the backdrop and
				// leaves its texture on the camera-relative path.
				if (min_w_required > 0.f)
					return true;

				return !depth_write && ((limit == 0) || (draw_ordinal < limit));
			}

			if (mode == 2)
			{
				// Order alone. Guarded on a non-zero limit because "the first 0 draws" read as "no
				// limit" in mode 1, and inheriting that here would tag the entire frame as sky.
				return (limit != 0) && (draw_ordinal < limit);
			}

			if (depth_read || depth_write)
				return false;

			return (limit == 0) || (draw_ordinal < limit);
		}

		// --- sky by texture hash, resolved BEFORE the solver is chosen -----------------------
		//
		// A rtx.skyBoxTextures tag already reaches the instance: build_draw_state ORs
		// materials::categories_for(material_hash) into the category flags. But that is all it
		// did, and it is not enough. The category is keyed on the material hash, and the material
		// hash does not exist until materials::bind() runs -- which happens AFTER the solver has
		// been picked. So a tagged backdrop still got the world solver, i.e. it still carried the
		// eye translation, which is what "I tagged it as sky and it still follows me / it
		// disappears at some angles" has meant on every title that tried it.
		//
		// classify_sky's mode 4 comment states that as a permanent constraint ("a by-hash tag
		// CANNOT do this job"). It is not one. It is a description of the ORDERING, and
		// materials::hash_only() exists precisely to compute bind()'s exact key without binding.
		// Resolve the hash here instead, and let a SKY tag force classification the same way
		// cloud_sky_draw already does.
		//
		// MEASURED, Rainbow Six 3, Alcatraz, logs\remix_draws.txt 2026-08-24, f=11090 d=0: the
		// backdrop is mat 8A5CE8DF66AF2637, 50 verts, w [0.0192, 0.489], fullscreen 3100x1875 px,
		// depth(r=1 w=1). EVERY geometric mode misses it -- mode 1 wants depth-neutral, mode 3
		// wants a render-target source, mode 4's SKYMINW is a FAR floor while this backdrop sits
		// NEARER than the world, and mode 2 (pure ordinal) also swept up the world draw d=1
		// (mat C6D48D406868B228, 34x75 px). The same hash is the backdrop on save state 7 as well,
		// so the tag list is the only rule here that both fits and survives a level change.
		//
		// COST, and why this is not on the hot path. hash_only() runs the same HashTextureLevel
		// unswizzle-and-hash as bind(), which the materials stats line measures at 1.7-2.6 us per
		// call. Two guards:
		//
		//   * the tag table must actually hold a SKY tag (tagged_category_mask(), one AND). A
		//     title with no sky tag pays a single test per draw and nothing else. SOCOM is
		//     unaffected twice over: it keeps mode 4 + SKYMINW, and its sky IS a render target,
		//     for which hash_only() returns 0 by design.
		//   * one memo entry per (source, frame), dropped when the frame or the tag generation
		//     moves. Draws re-sample the same handful of Source objects, so this collapses to
		//     roughly one hash per distinct texture per frame.
		//
		// The watchdog is the existing "hash N ms (x us/bind)" figure on the materials line --
		// compare it before and after. Note also that this path calls categories_for(), so the
		// "tags N hits M" counter now includes these probes; it was never a usable criterion for
		// this title anyway (the view-model tag matches the weapon every single frame).
		bool hash_tagged_sky(const GSTextureCache::Source* source)
		{
			const bool armed = (remix_ps2::materials::tagged_category_mask() &
								   static_cast<u32>(REMIXAPI_INSTANCE_CATEGORY_BIT_SKY)) != 0;

			// Logged on CHANGE rather than at first use, for the PERDRAWCAM reason: the first call
			// legitimately happens before the per-game conf has been applied, so a one-shot log
			// would report "no sky tags" and be wrong in the file the user reads to check the tag
			// list loaded. The tag lists are also re-read live, so this can flip mid-session.
			static int logged_armed = -1;
			if (const int now = armed ? 1 : 0; now != logged_armed)
			{
				logged_armed = now;
				INFO_LOG("Remix: sky-by-hash {} -- {}", armed ? "ARMED" : "idle",
					armed ? "a rtx.skyBoxTextures tag now forces the sky solver before the solver "
							"is picked; count it on the stats line as sky_hash" :
							"no texture carries the SKY category, so no draw is hashed for it");
			}

			if (!source || !armed)
				return false;

			static std::unordered_map<const void*, bool> cache;
			static u64 cache_frame = ~0ull;
			static u64 cache_generation = 0;

			const u64 tag_generation = remix_ps2::materials::generation();
			if (cache_frame != s_frame_counter || cache_generation != tag_generation)
			{
				cache.clear();
				cache_frame = s_frame_counter;
				cache_generation = tag_generation;
			}

			if (const auto found = cache.find(static_cast<const void*>(source)); found != cache.end())
				return found->second;

			// 0 for a render-target source by design, and categories_for(0) is 0 -- so an
			// RT-sourced draw can never be tagged this way, which is correct: a render target has
			// no stable content identity for a tag to key on.
			const u64 content_hash = remix_ps2::materials::hash_only(source);
			const bool sky = (remix_ps2::materials::categories_for(content_hash) &
								 static_cast<u32>(REMIXAPI_INSTANCE_CATEGORY_BIT_SKY)) != 0;

			cache.emplace(static_cast<const void*>(source), sky);
			return sky;
		}

		// --- per-draw state dump ------------------------------------------------------------
		//
		// PCSX2_REMIX_DRAWDUMP=N dumps every gate-passing draw for the first N frames that
		// submitted anything, to logs\remix_draws.txt. This exists so the sky rule above is
		// derived from what the title actually does rather than picked and hoped for.
		u64 drawdump_frames()
		{
			return static_cast<u64>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_DRAWDUMP", 0), 0, 16));
		}

		bool drawdump_world_only()
		{
			return remix_ps2::read_env_int(L"PCSX2_REMIX_DRAWDUMP_WORLD", 0) != 0;
		}

		// Submitting frames to let pass before the dump starts. Default 0 = start immediately,
		// which is what shipped and what the arming rule above describes.
		//
		// MEASURED 2026-08-15, the reason this exists: the first dump this title ever produced
		// landed on frames 3, 5, 6 and 8 -- the first frames that submitted with a world camera,
		// i.e. mission start. Those frames carry 11 sky draws each, while in-mission the stats line
		// settles to 2.0/frame, so the dump described a scene nobody was asking about. A dump is
		// only evidence for the scene it was taken in; "first frames that submit" is not the scene
		// the player is looking at when they report something.
		//
		// Read live, like drawdump_frames() and for the same reason -- see frametrace_frames().
		u64 drawdump_after_frames()
		{
			return static_cast<u64>(
				std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_DRAWDUMPAFTER", 0), 0, 1000000));
		}

		// EXPLODE offender lines written so far. Session-lifetime like drawdump_write's own line
		// count, not per-frame: the offenders worth reading are the first ones, and after a hundred
		// of them the stats-line counter is the number that matters.
		u32 s_explode_dumped = 0;

		// PCSX2_REMIX_FARDUMP=N dumps draws that put a vertex further than N world units from the
		// origin. 0 disables it.
		//
		// The 'explode' check asks whether a draw is large *relative to its own eye depth*, so it
		// answers "is this draw stretched" and reads 0 on SOCOM Winterblade while the screen is
		// visibly in pieces. A draw can be perfectly compact and still be placed 100k units from
		// where it belongs, and nothing measured that. This is the missing half: absolute placement,
		// not relative shape.
		//
		// Latched: this is read once per draw on a hot path, and a getenv per draw would distort
		// the very timing the dump is meant to describe.
		float far_dump_limit()
		{
			static const float value =
				static_cast<float>(remix_ps2::read_env_int(L"PCSX2_REMIX_FARDUMP", 0));
			return value;
		}

		u32 s_fardump_written = 0;

		// --- per-frame window trace -----------------------------------------------------------
		//
		// PCSX2_REMIX_FRAMETRACE=N writes one FRAME line per presented frame, for N frames, into the
		// matrix dump. It describes the WINDOW rather than the camera: how many draws were built in
		// it, how many distinct per-kick cameras those draws were built under, and whether the
		// camera the window used was resolved for it or carried over from an earlier one.
		//
		// It exists because of what the 2026-08-15 clip run REFUTED. This title presents one
		// shattered frame in every three at 60 Hz, and both obvious explanations are dead. The camera
		// is not being dropped: `accept` climbs +300 per 300-frame stats block, so resolve_world_camera
		// accepted on ~898 of 900 frames and drop_stale_camera never fired in-mission. The vertices
		// are not being flung out of the scene either: extent-reject reads 0 in every block, so the
		// damage stays inside scene scale. And the 2-on/1-off cadence in the matrix dump is not a
		// capture miss -- the absent windows carry no new matrices of ANY source, object matrices
		// included, which means the game built no frame in them: 40 fps of rendering against a 60 Hz
		// present (the run's own status bar, `FPS: 39-40  VPS: 60-61`).
		//
		// So the presented-frame unit and the game-frame unit disagree every third window, and the
		// open question is only WHICH form the disagreement takes. This measures it: draws split
		// across two ring cameras -- one window straddling two game frames -- shows up as ringN > 0,
		// while a sparse or partial window shows up as `sub` dipping with a single ring hash. Those
		// two findings want completely different fixes, and on this title guessing between them has
		// historically cost a session.
		//
		// Off by default, because the draw-side sampler calls LookupKickCamera, which walks up to
		// 2048 ring slots on a valid-gap (RemixVU1Capture.cpp:269) -- not a cost to pay per draw on
		// a run nobody is measuring.
		//
		// READ LIVE, NOT LATCHED, and that distinction is the whole reason this knob works.
		// MEASURED 2026-08-15: latched as `static const`, this shipped and produced ZERO FRAME
		// lines on a run whose own log confirmed `PCSX2_REMIX_FRAMETRACE = 600 (PCSX2 toggle)` was
		// applied. The per-game .conf is read at t=10.008 s but the renderer goes live at t=9.774 s
		// (`renderer is live`), so OnVSync had already called this and cached 0 -- permanently --
		// several frames before the conf existed. The arming check then never fired.
		//
		// This is the same trap `dump_enabled()`/`nocam_enabled()` were un-latched for
		// (RemixTransforms.cpp), and the same one `drawdump_frames()` above already avoids by
		// reading live -- which is exactly why the draw dump wrote on that run and this did not.
		// The latch bought nothing anyway: the per-draw hot path gates on `s_frametrace_left`, a
		// plain counter, and never calls this. **Any knob delivered by the per-game .conf must be
		// read live; latching one is a silent no-op that looks like a broken feature.**
		u64 frametrace_frames()
		{
			return static_cast<u64>(
				std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_FRAMETRACE", 0), 0, 100000));
		}

		// Qualifying windows to let pass before the trace arms. Default 0 = arm immediately.
		//
		// MEASURED 2026-08-15, and this is why it exists: the trace armed at the first submitting
		// window and ran 600, i.e. windows ~34-934 -- which on this title is almost entirely the
		// loading and mission-intro phase. The per-window camera census then showed the
		// multi-camera condition it was hunting **stops at window ~600 and never recurs**:
		// `multi` freezes at 126 while the window count climbs 353 -> 1952, and `draws distinct`
		// freezes at 49,418 at the same moment. So the trace characterised startup and was read as
		// though it characterised gameplay, and the fix built on it (per-draw camera placement) is
		// a no-op in the scene the player is actually looking at.
		//
		// This is the SECOND time this exact mistake was made in one session -- `DRAWDUMPAFTER`
		// above exists for the same reason, and the trace was left arming at frame one anyway.
		// **A diagnostic that arms on "the first frame that qualifies" measures startup. If the
		// symptom is in gameplay, the measurement must be delayed to gameplay.**
		//
		// Read live, like every other conf-delivered knob -- see frametrace_frames().
		u64 frametrace_after_frames()
		{
			return static_cast<u64>(
				std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_FRAMETRACEAFTER", 0), 0, 1000000));
		}

		// What the draw path accumulates for one window. GS thread only -- the sampler runs in
		// OnDrawPrims and the reader in OnVSync, and both of those are the GS thread. Nothing here
		// touches VU1 state; the ring is read through LookupKickCamera and only through it.
		struct frame_trace_state
		{
			u32 draws = 0; // draws sampled this window
			u32 misses = 0; // ring lookups that held nothing for the draw's kick
			u32 not_first = 0; // draws whose ring camera differed from the window's first
			u64 first_hash = 0; // the window's first ring camera
			u64 second_hash = 0; // the first ring camera that differed from it
		};

		frame_trace_state s_frame_trace{};
		u64 s_frametrace_left = 0;
		u64 s_frametrace_skipped = 0; // qualifying windows passed over while FRAMETRACEAFTER counts down
		bool s_frametrace_started = false;

		// The active camera's matrix hash as of the last VSync, and the frame at which it last
		// changed. `age` on the FRAME line is the difference: 1 on a window whose camera was
		// resolved for it, higher on one still drawing with an older camera.
		u64 s_frametrace_cam_hash = 0;
		u64 s_frametrace_cam_since = 0;

		// --- world-position probe -------------------------------------------------------------
		//
		// PCSX2_REMIX_WORLDPROBE=N writes one WORLDPROBE line per presented window, for N windows,
		// into the matrix dump beside the FRAME lines (same f=, so the two join).
		//
		// THE QUESTION IT ANSWERS, and it is a single yes/no: does the un-projection recover
		// STABLE world positions for static geometry as the camera turns, or do the recovered
		// positions carry the camera's rotation?
		//
		// It exists because a light was reported sitting at/behind the eye and sweeping its
		// shadows with the view, and every light that could physically do that has been ruled out:
		// light_mode()==1 builds ONE Distant light with a hard-coded world direction and no
		// position; every camera-attached light (place_debug_light at cam.position, place_sun_light
		// along camera forward) is gated on light_mode()==2, which this title does not run; the
		// runtime's own fallback is off (rtx.fallbackLightMode = 0); and the symptom survives
		// LIGHTMODE = 0, i.e. a frame with no lights created at all. What is left is the other
		// half of the same appearance: a fixed light over a WORLD THAT ROTATES. If our recovered
		// positions are camera-relative rather than world-absolute, a fixed light rakes across the
		// scene exactly as a camera-mounted one would, and the implausible `Vertical FOV: 112.6`
		// falls out of the same wrong-but-self-consistent camera solve.
		//
		// READ-ONLY. Nothing here is submitted, no transform is altered, no gate moves. It reads
		// s_scratch_vertices at the point the draw has been accepted and BEFORE the stable-identity
		// path re-expresses those positions as local + instance transform, so what it measures is
		// the world-space placement either path produces. Default 0 = off, and off costs one u64
		// compare per draw.
		//
		// READ LIVE, NEVER LATCHED -- see frametrace_frames() for the run this cost.
		u64 worldprobe_frames()
		{
			return static_cast<u64>(
				std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_WORLDPROBE", 0), 0, 100000));
		}

		// Qualifying windows to let pass before the probe arms. Default 0 = arm immediately.
		//
		// Mirrors drawdump_after_frames()/frametrace_after_frames(), and for the reason recorded
		// there: BOTH of those armed on "the first frame that qualifies", which on this title is
		// mission start, and both produced measurements of the loading phase that were then read
		// as gameplay. The symptom this probe is aimed at is something the player sees while
		// TURNING THE CAMERA in-mission, so the measurement has to be delayed to in-mission.
		//
		// Read live, like every other conf-delivered knob.
		u64 worldprobe_after_frames()
		{
			return static_cast<u64>(
				std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_WORLDPROBEAFTER", 0), 0, 1000000));
		}

		// Draws followed on the line. Four is what fits while staying readable, and it is enough
		// that one of them going ABSENT does not blind the run.
		constexpr u32 worldprobe_slots = 4;

		// Distinct content hashes considered in one window. A cap, not a target: the scan below is
		// linear in it, and beyond this the extra candidates cannot win a slot anyway without
		// being enormous.
		constexpr u32 worldprobe_candidates = 64;

		// NON-EMPTY windows surveyed before the tracked set is chosen.
		//
		// MEASURED 2026-08-15, and this is why selection is no longer taken from a single window:
		// the first capture returned 1,417 lines all reading `slots=0`, with no hashes at all. The
		// selection was one-shot, taken on the arming window, and this title under
		// PCSX2_REMIX_HOLDEMPTY = 2 submits ZERO geometry in one presented window out of three --
		// 57 of 171 windows, spacing exactly 3 in 56 of 56 gaps, which is the game's 40 fps against
		// our 60 Hz present. The arming window landed on one of those, the empty selection was
		// frozen, and the whole 345-degree capture the user performed was unusable. A 1-in-3
		// failure rate on a one-shot choice.
		//
		// So selection now RETRIES until a window actually carries qualifying draws, and while it
		// is at it, surveys several of them. Ranking on windows-seen first means a transient draw
		// -- an effect quad, a muzzle flash -- cannot take a slot from level geometry just by being
		// large in the one window that happened to be looked at. Eight non-empty windows is about
		// a fifth of a second of presented time at this title's cadence: long enough that a
		// one-frame draw loses, short enough that the view has not meaningfully turned.
		constexpr u32 worldprobe_survey_windows = 8;

		// Ceiling on the survey table. Same reasoning as worldprobe_candidates, applied across
		// windows rather than within one.
		constexpr u32 worldprobe_survey_cap = 256;

		// One followed draw's accumulation for one window. Several draws in a window may share a
		// content hash (one texture, several pieces of level), and they are merged here rather
		// than fought over: `draws` and `verts` are printed precisely so a reader can see whether
		// the merged SET changed between two lines before comparing their centroids.
		struct worldprobe_entry
		{
			u64 content_hash = 0;
			u32 draws = 0;
			u32 vertices = 0;
			u32 triangles = 0;
			double sum[3] = {0.0, 0.0, 0.0};
			scene_bounds bounds{};
		};

		// The current window's accumulation. Before the tracked set is chosen this holds every
		// world-mode non-sky content hash the window produced (capped); afterwards it is pre-seeded
		// with exactly the tracked hashes, in slot order, and nothing else is ever admitted.
		std::vector<worldprobe_entry> s_worldprobe_frame;

		// One candidate hash's standing across the survey. `windows` is how many NON-EMPTY windows
		// it appeared in and is the primary ranking key -- persistence is what distinguishes level
		// geometry from a draw that happened to be big once.
		struct worldprobe_candidate
		{
			u64 content_hash = 0;
			u32 windows = 0;
			u32 triangles = 0; // largest single-window triangle count seen
		};

		// Accumulated across survey windows and discarded the moment selection lands.
		std::vector<worldprobe_candidate> s_worldprobe_survey;
		u32 s_worldprobe_surveyed = 0; // non-empty windows folded into the survey so far

		u64 s_worldprobe_tracked[worldprobe_slots] = {};
		u32 s_worldprobe_tracked_count = 0;
		u64 s_worldprobe_left = 0;
		u64 s_worldprobe_skipped = 0; // qualifying windows passed over while WORLDPROBEAFTER counts down
		bool s_worldprobe_started = false;  // the draw-side sampler is on
		bool s_worldprobe_selected = false; // the tracked hashes have been picked and are now fixed
		bool s_worldprobe_logged_arm = false; // one-shot "armed" line

		// The camera the window that just ended un-projected its draws with. Sampled at VSync
		// BEFORE resolve_world_camera() replaces it, exactly where FRAMETRACE samples `used=`.
		struct worldprobe_camera
		{
			bool valid = false;
			u64 hash = 0;
			float position[3] = {0.f, 0.f, 0.f};
			float forward[3] = {0.f, 0.f, 0.f};
		};

		// Draw-side sampler. Called only for accepted, world-mode, non-sky draws while the probe
		// is armed; `triangles` is the post-compaction count, i.e. the same number the per-draw
		// dump prints as tris=.
		//
		// Keyed on the MATERIAL content hash and deliberately not on the mesh hash. The mesh hash
		// is the wrong key here twice over: under PCSX2_REMIX_STABLEID = 0 it is built from
		// quantized POSITIONS, so it would change every window in precisely the failure case this
		// probe exists to detect, and under STABLEID = 1 it folds in a size term that a bad solve
		// can move. The material hash comes from TEX0/TEXA/CLUT -- guest state, untouched by any
		// camera -- so it cannot be broken by the thing being measured.
		void worldprobe_sample(u64 content_hash, u32 triangles)
		{
			worldprobe_entry* entry = nullptr;

			for (worldprobe_entry& candidate : s_worldprobe_frame)
			{
				if (candidate.content_hash == content_hash)
				{
					entry = &candidate;
					break;
				}
			}

			if (!entry)
			{
				// Once the tracked set is fixed the accumulator holds exactly those hashes, so a
				// miss here is a draw we are deliberately NOT following, and dropping it is the
				// whole point: admitting it would put a DIFFERENT object in a slot and make the
				// window-to-window comparison compare two unrelated things. That is the one
				// failure mode that would render this entire measurement meaningless.
				if (s_worldprobe_selected || s_worldprobe_frame.size() >= worldprobe_candidates)
					return;

				s_worldprobe_frame.push_back(worldprobe_entry{});
				entry = &s_worldprobe_frame.back();
				entry->content_hash = content_hash;
			}

			++entry->draws;
			entry->triangles += triangles;
			entry->vertices += static_cast<u32>(s_scratch_vertices.size());

			// s_scratch_vertices still holds WORLD positions at the call site: the stable-identity
			// path does not rewrite them to local until after the batch branch. Summed in double
			// so a centroid taken over thousands of vertices at world scale does not lose the
			// digits the comparison depends on.
			for (const remixapi_HardcodedVertex& v : s_scratch_vertices)
			{
				entry->bounds.add(v.position);

				for (u32 k = 0; k < 3; ++k)
					entry->sum[k] += static_cast<double>(v.position[k]);
			}
		}

		// -----------------------------------------------------------------------------------------
		// PCSX2_REMIX_WORLDFIX audit -- does recovered world geometry hold still while the eye turns?
		// -----------------------------------------------------------------------------------------
		//
		// WHY THIS IS NOT WORLDPROBE. WORLDPROBE keys on the MATERIAL content hash and says so
		// (worldprobe_sample above). Its slots are therefore "every piece of level wearing this
		// texture", and the MEMBERSHIP of that set changes every window as the guest culls. The
		// confound is not theoretical: re-analysing the 17,412 WORLDPROBE lines already in
		// logs/remix_matrices.txt, slot C6FC960431E8CE4C ranges from v=3 to v=3414 vertices inside
		// one capture and slot D12EA192BF997FBD from v=21 to v=2098. A centroid taken over a
		// changing set moves for reasons that have nothing to do with the un-projection -- and the
		// visible subset of a large mesh sits near the screen centre by construction, which mimics
		// precisely the camera-locking the probe exists to detect. Restricted to the windows where
		// that set is provably constant, the same data gives alpha = 0.10, 1.84 and 1.94 for three
		// different tracked draws, which a single global camera error cannot produce.
		//
		// THE KEY USED HERE is (material hash, vertex count, index count). It pins one submitted
		// mesh, contains no position data -- so the thing being measured cannot move the key -- and
		// changes the instant the merged set changes, so mismatched windows simply do not pair.
		//
		// WHAT IT REPORTS. Over consecutive window pairs it least-squares fits
		//
		//     d(world bearing of the tracked centroid) = alpha * d(camera yaw)
		//
		//   alpha ~ 0  static geometry stands still in world space while the eye turns. CORRECT.
		//   alpha ~ 1  the geometry rides the camera: the elected matrix's rotation is not the one
		//              the vertices were drawn with (a stale, foreign, or per-object matrix).
		//   alpha ~ 2  the recovered world is MIRRORED about a plane fixed to the camera. Because
		//              that plane turns with the eye, the world is re-mirrored every frame and a
		//              fixed point's image counter-rotates at twice the yaw rate.
		//
		// A pair is rejected unless the yaw actually moved and the eye did not: translation
		// parallax would otherwise be read as rotation. Both rejections are counted, so an audit
		// that measured nothing says so instead of printing a confident zero.
		constexpr u32 worldfix_slots = 16;

		struct worldfix_slot
		{
			u64 key = 0;
			u64 frame = 0; // window this slot's `now` came from
			float now[3] = {0.f, 0.f, 0.f};
			u64 prev_frame = 0;
			float prev[3] = {0.f, 0.f, 0.f};
			bool prev_valid = false;
		};

		worldfix_slot s_worldfix[worldfix_slots];

		// Least-squares accumulators for d(bearing) against d(yaw), through the origin: a pair with
		// no yaw carries no information about alpha and is rejected before it gets here, so the fit
		// has no intercept to estimate.
		double s_worldfix_sxx = 0.0;
		double s_worldfix_sxy = 0.0;
		double s_worldfix_syy = 0.0;
		u64 s_worldfix_pairs = 0;
		u64 s_worldfix_reject_yaw = 0;   // the eye did not turn enough to measure anything
		u64 s_worldfix_reject_move = 0;  // the eye translated far enough to fake a rotation
		u64 s_worldfix_windows = 0;
		int s_worldfix_last_mode = -1;

		// The mode the DRAW path tests. Refreshed from the environment once per presented window by
		// worldfix_window() below -- never cached beyond that, so a .conf applied mid-session is
		// picked up on the next window. The draw path must not call worldfix_mode() itself: that is
		// a GetEnvironmentVariableW, and this title submits ~830 draws a window.
		int s_worldfix_active = 0;
		bool s_worldfix_prev_camera_valid = false;
		float s_worldfix_prev_eye[3] = {0.f, 0.f, 0.f};
		float s_worldfix_prev_forward[3] = {0.f, 0.f, 0.f};
		u64 s_worldfix_prev_frame = 0;

		// Draw-side sampler. Same call site and same gating as worldprobe_sample: accepted,
		// world-mode, non-sky draws only, with s_scratch_vertices still holding WORLD positions.
		void worldfix_sample(u64 content_hash, u32 vertex_count, u32 index_count)
		{
			const u64 key = fnv_mix(fnv_mix(content_hash, vertex_count), index_count);

			worldfix_slot* slot = nullptr;
			worldfix_slot* oldest = &s_worldfix[0];

			for (worldfix_slot& candidate : s_worldfix)
			{
				if (candidate.key == key)
				{
					slot = &candidate;
					break;
				}

				if (candidate.frame < oldest->frame)
					oldest = &candidate;
			}

			if (!slot)
			{
				slot = oldest;
				*slot = worldfix_slot{};
				slot->key = key;
			}
			else if (slot->frame == s_frame_counter)
			{
				// Two draws in one window with an identical (material, vertex, index) fingerprint
				// are two instances of the same shape, not one object seen twice. Keep the first:
				// which one is kept must not depend on submission order, or the pair below compares
				// two different objects and reports their separation as camera-induced motion.
				return;
			}

			slot->prev_valid = (slot->frame != 0);
			slot->prev_frame = slot->frame;
			for (u32 k = 0; k < 3; ++k)
				slot->prev[k] = slot->now[k];

			slot->frame = s_frame_counter;

			double sum[3] = {0.0, 0.0, 0.0};
			for (const remixapi_HardcodedVertex& v : s_scratch_vertices)
			{
				for (u32 k = 0; k < 3; ++k)
					sum[k] += static_cast<double>(v.position[k]);
			}

			const double inv = s_scratch_vertices.empty()
								   ? 0.0
								   : (1.0 / static_cast<double>(s_scratch_vertices.size()));

			for (u32 k = 0; k < 3; ++k)
				slot->now[k] = static_cast<float>(sum[k] * inv);
		}

		// Window-side half. Called at VSync with the camera the window that just ended actually
		// un-projected against -- the same instant, and for the same reason, that WORLDPROBE
		// samples its own camera: reading it after resolve_world_camera() would pair this window's
		// positions with the next window's eye.
		void worldfix_window(int mode, const worldprobe_camera& cam)
		{
			s_worldfix_active = mode;

			if (mode != s_worldfix_last_mode)
			{
				s_worldfix_last_mode = mode;
				INFO_LOG("Remix: PCSX2_REMIX_WORLDFIX = {} ({}). alpha near 0 means recovered world "
						 "geometry holds still as the eye turns (correct); near 1 means it rides the "
						 "camera; near 2 means the recovered world is mirrored about a camera-fixed "
						 "plane. Modes 2 and 3 flip the recovered handedness and cannot change the "
						 "rendered image.",
					mode,
					(mode == 0) ? "off" : ((mode == 1) ? "audit only" : ((mode == 2) ? "audit + force m00 > 0" : "audit + force m00 < 0")));
			}

			if (mode == 0)
				return;

			const bool have_pair = s_worldfix_prev_camera_valid && cam.valid &&
								   (s_frame_counter > s_worldfix_prev_frame);

			if (have_pair)
			{
				++s_worldfix_windows;

				const double yaw_now = std::atan2(static_cast<double>(cam.forward[0]),
					static_cast<double>(cam.forward[2]));
				const double yaw_prev = std::atan2(static_cast<double>(s_worldfix_prev_forward[0]),
					static_cast<double>(s_worldfix_prev_forward[2]));

				double dyaw = yaw_now - yaw_prev;
				while (dyaw > 3.14159265358979323846) dyaw -= 2.0 * 3.14159265358979323846;
				while (dyaw < -3.14159265358979323846) dyaw += 2.0 * 3.14159265358979323846;

				const double eye_move = std::sqrt(
					static_cast<double>((cam.position[0] - s_worldfix_prev_eye[0]) * (cam.position[0] - s_worldfix_prev_eye[0]) +
										(cam.position[1] - s_worldfix_prev_eye[1]) * (cam.position[1] - s_worldfix_prev_eye[1]) +
										(cam.position[2] - s_worldfix_prev_eye[2]) * (cam.position[2] - s_worldfix_prev_eye[2])));

				// 0.05 deg per window: below this the bearing change is all quantisation and the
				// ratio is a divide by noise.
				if (std::abs(dyaw) < 0.00087)
				{
					++s_worldfix_reject_yaw;
				}
				else
				{
					for (const worldfix_slot& slot : s_worldfix)
					{
						if (slot.key == 0 || !slot.prev_valid)
							continue;

						if (slot.frame != s_frame_counter || slot.prev_frame != s_worldfix_prev_frame)
							continue;

						const double nx = static_cast<double>(slot.now[0] - cam.position[0]);
						const double nz = static_cast<double>(slot.now[2] - cam.position[2]);
						const double px = static_cast<double>(slot.prev[0] - s_worldfix_prev_eye[0]);
						const double pz = static_cast<double>(slot.prev[2] - s_worldfix_prev_eye[2]);

						const double range = std::sqrt((nx * nx) + (nz * nz));
						if (!(range > 1e-3))
							continue;

						// The eye must have moved far less than the object is distant, or walking
						// past a near object produces a bearing sweep with no rotation in it at all.
						if (eye_move > (0.02 * range))
						{
							++s_worldfix_reject_move;
							continue;
						}

						double dbearing = std::atan2(nx, nz) - std::atan2(px, pz);
						while (dbearing > 3.14159265358979323846) dbearing -= 2.0 * 3.14159265358979323846;
						while (dbearing < -3.14159265358979323846) dbearing += 2.0 * 3.14159265358979323846;

						s_worldfix_sxx += dyaw * dyaw;
						s_worldfix_sxy += dyaw * dbearing;
						s_worldfix_syy += dbearing * dbearing;
						++s_worldfix_pairs;
					}
				}

				// Reported on a fixed cadence rather than at exit: ops.finalize is not guaranteed to
				// run, and a diagnostic whose only output is at shutdown is a diagnostic that
				// produces nothing on the run that mattered.
				if (s_worldfix_pairs > 0 && (s_worldfix_windows % 600) == 0)
				{
					const double alpha = (s_worldfix_sxx > 1e-12) ? (s_worldfix_sxy / s_worldfix_sxx) : 0.0;
					const double r2 = (s_worldfix_sxx > 1e-12 && s_worldfix_syy > 1e-12)
										  ? ((s_worldfix_sxy * s_worldfix_sxy) / (s_worldfix_sxx * s_worldfix_syy))
										  : 0.0;

					INFO_LOG("Remix: WORLDFIX audit -- alpha {:.3f} (R2 {:.3f}) over {} paired "
							 "samples in {} windows; rejected {} for no yaw, {} for eye translation. "
							 "alpha 0 = world holds still (correct), 1 = world rides the camera, "
							 "2 = world mirrored about a camera-fixed plane.",
						alpha, r2, s_worldfix_pairs, s_worldfix_windows,
						s_worldfix_reject_yaw, s_worldfix_reject_move);
				}
			}

			s_worldfix_prev_camera_valid = cam.valid;
			s_worldfix_prev_frame = s_frame_counter;
			for (u32 k = 0; k < 3; ++k)
			{
				s_worldfix_prev_eye[k] = cam.position[k];
				s_worldfix_prev_forward[k] = cam.forward[k];
			}
		}

		// Re-arms the accumulator for the next window. After selection it is PRE-SEEDED with the
		// tracked hashes in slot order, so slot i always means hash i and a hash that did not draw
		// this window prints ABSENT in its own slot instead of quietly ceding it to another draw.
		void worldprobe_reset_frame()
		{
			s_worldprobe_frame.clear();

			if (!s_worldprobe_selected)
				return;

			for (u32 i = 0; i < s_worldprobe_tracked_count; ++i)
			{
				worldprobe_entry seed{};
				seed.content_hash = s_worldprobe_tracked[i];
				s_worldprobe_frame.push_back(seed);
			}
		}

		// Cleared alongside worldprobe_reset_all(). s_frame_counter is zeroed on GS close, so slots
		// carrying frame indices from the previous session would sit permanently "in the future" and
		// the audit would silently pair nothing.
		void worldfix_reset_all()
		{
			for (worldfix_slot& slot : s_worldfix)
				slot = worldfix_slot{};

			s_worldfix_sxx = 0.0;
			s_worldfix_sxy = 0.0;
			s_worldfix_syy = 0.0;
			s_worldfix_pairs = 0;
			s_worldfix_reject_yaw = 0;
			s_worldfix_reject_move = 0;
			s_worldfix_windows = 0;
			s_worldfix_prev_camera_valid = false;
			s_worldfix_prev_frame = 0;
			s_worldfix_logged_projection = false;
		}

		void worldprobe_reset_all()
		{
			s_worldprobe_frame.clear();
			s_worldprobe_survey.clear();
			s_worldprobe_surveyed = 0;
			s_worldprobe_tracked_count = 0;
			s_worldprobe_left = 0;
			s_worldprobe_skipped = 0;
			s_worldprobe_started = false;
			s_worldprobe_selected = false;
			s_worldprobe_logged_arm = false;

			for (u32 i = 0; i < worldprobe_slots; ++i)
				s_worldprobe_tracked[i] = 0;
		}

		// Freezes the tracked set from the survey. Called only with a NON-EMPTY survey, so it
		// always selects at least one hash and can never be reached twice -- it sets
		// s_worldprobe_selected, and every caller is gated on that being false.
		void worldprobe_select()
		{
			// Persistence first, size second. A draw present in every surveyed window is level
			// geometry, which is the only kind of thing whose world position is supposed to hold
			// still while the eye turns; a bigger draw that appeared once is exactly what this
			// ordering is here to reject. stable_sort so a tie resolves by first-seen order and
			// the choice is reproducible from the log rather than arbitrary.
			std::stable_sort(s_worldprobe_survey.begin(), s_worldprobe_survey.end(),
				[](const worldprobe_candidate& a, const worldprobe_candidate& b) {
					if (a.windows != b.windows)
						return a.windows > b.windows;

					return a.triangles > b.triangles;
				});

			const u32 candidates = static_cast<u32>(s_worldprobe_survey.size());
			s_worldprobe_tracked_count = std::min<u32>(candidates, worldprobe_slots);

			std::string chosen;
			for (u32 i = 0; i < s_worldprobe_tracked_count; ++i)
			{
				s_worldprobe_tracked[i] = s_worldprobe_survey[i].content_hash;
				chosen += fmt::format(" [{:016X} t={} w={}]", s_worldprobe_survey[i].content_hash,
					s_worldprobe_survey[i].triangles, s_worldprobe_survey[i].windows);
			}

			s_worldprobe_selected = true;
			s_worldprobe_survey.clear();
			s_worldprobe_survey.shrink_to_fit();

			// One-shot, and it exists because the first version of this probe FAILED SILENTLY: it
			// selected from one window, that window was one of the empty ones this title presents
			// every third frame, and 1,417 lines of `slots=0` were written with nothing in the log
			// to say why. A selection that produced nothing must be visible in emulog.txt.
			INFO_LOG("Remix: WORLDPROBE selected {} draw(s) to follow at frame {} -- from {} "
					 "candidate hash(es) over {} non-empty window(s):{}",
				s_worldprobe_tracked_count, s_frame_counter, candidates, s_worldprobe_surveyed,
				chosen);
		}

		// Folds the window that just ended into the survey, and selects once enough non-empty
		// windows have been seen.
		//
		// The retry IS the fix for the empty-selection failure: an empty window contributes
		// nothing, advances nothing, and costs nothing -- the probe simply looks again next
		// window. Nothing here can spin: the only state that advances is s_worldprobe_surveyed,
		// it advances at most once per presented window and only on a window that contributed at
		// least one hash, and it is compared against a compile-time constant.
		void worldprobe_survey_window()
		{
			if (s_worldprobe_frame.empty())
				return;

			++s_worldprobe_surveyed;

			for (const worldprobe_entry& entry : s_worldprobe_frame)
			{
				worldprobe_candidate* candidate = nullptr;

				for (worldprobe_candidate& known : s_worldprobe_survey)
				{
					if (known.content_hash == entry.content_hash)
					{
						candidate = &known;
						break;
					}
				}

				if (!candidate)
				{
					if (s_worldprobe_survey.size() >= worldprobe_survey_cap)
						continue;

					s_worldprobe_survey.push_back(worldprobe_candidate{});
					candidate = &s_worldprobe_survey.back();
					candidate->content_hash = entry.content_hash;
				}

				++candidate->windows;
				candidate->triangles = std::max(candidate->triangles, entry.triangles);
			}

			if (s_worldprobe_surveyed < worldprobe_survey_windows)
				return;

			// Guaranteed non-empty: s_worldprobe_surveyed only reaches here by having been advanced
			// on windows that each contributed at least one entry, and entries are only dropped at
			// the cap, which cannot empty a non-empty table.
			if (s_worldprobe_survey.empty())
				return;

			worldprobe_select();
		}

		// Emit only. Selection has already happened -- every call site is gated on
		// s_worldprobe_selected, so this can never write a line without a tracked set behind it.
		void worldprobe_emit(const worldprobe_camera& cam)
		{
			// cv/cam/p/fwd describe the eye the geometry on this same line was un-projected
			// against, so the two are always read together.
			std::string line = fmt::format(
				"WORLDPROBE f={} cv={} cam={:016X} p=({:.2f},{:.2f},{:.2f}) fwd=({:.4f},{:.4f},{:.4f}) slots={}",
				s_frame_counter, cam.valid ? 1 : 0, cam.hash,
				cam.position[0], cam.position[1], cam.position[2],
				cam.forward[0], cam.forward[1], cam.forward[2],
				s_worldprobe_tracked_count);

			for (u32 i = 0; i < s_worldprobe_tracked_count; ++i)
			{
				const u64 tracked = s_worldprobe_tracked[i];

				// Indexed, because the accumulator is re-seeded in tracked order every window --
				// but hash-checked as well, so a future change that breaks that invariant reports
				// ABSENT rather than silently mislabelling a slot.
				const worldprobe_entry* entry = nullptr;
				if (i < s_worldprobe_frame.size() && s_worldprobe_frame[i].content_hash == tracked)
					entry = &s_worldprobe_frame[i];

				if (!entry || entry->draws == 0 || entry->vertices == 0 || !entry->bounds.valid)
				{
					line += fmt::format(" | h={:016X} ABSENT", tracked);
					continue;
				}

				const double inv = 1.0 / static_cast<double>(entry->vertices);
				const float cx = static_cast<float>(entry->sum[0] * inv);
				const float cy = static_cast<float>(entry->sum[1] * inv);
				const float cz = static_cast<float>(entry->sum[2] * inv);

				// Distance from the eye to the centroid. This is the discriminator: under a
				// camera-relative recovery a pure look-around holds d fixed while c swings,
				// because the geometry is being carried around the eye instead of standing still.
				const float dx = cx - cam.position[0];
				const float dy = cy - cam.position[1];
				const float dz = cz - cam.position[2];
				const float dist = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));

				line += fmt::format(
					" | h={:016X} n={} v={} t={} c=({:.2f},{:.2f},{:.2f}) d={:.2f} "
					"b=({:.2f},{:.2f},{:.2f})..({:.2f},{:.2f},{:.2f})",
					tracked, entry->draws, entry->vertices, entry->triangles, cx, cy, cz, dist,
					entry->bounds.min[0], entry->bounds.min[1], entry->bounds.min[2],
					entry->bounds.max[0], entry->bounds.max[1], entry->bounds.max[2]);
			}

			dump_write(line);
		}

		// Minimum render-target AREA in pixels for a target to count as on-screen; anything below it
		// is an off-screen scratch target. 0 disables the gate. Latched -- read once per draw.
		//
		// Absolute, NOT relative to the largest target seen: see the gate itself for why the
		// relative version culled the whole world.
		// Drop the depth-detached silhouette pass. Measured on R6 3 (logsemix_draws.txt, 708
		// draws): every real surface and every real character uses ZTST=2 (GEQUAL) with depth
		// writes on. Exactly 9 draws use ZTST=1 (ALWAYS) with ZMSK=1 (no depth write) -- 1.3% of
		// the draws carrying 24% of the vertices, all one ~3730-vertex character mesh at the same
		// draw index every frame. A character composited without participating in the depth buffer
		// is the game's shadow/silhouette pass, and un-projected with the player's camera it is the
		// ghost body that floats through walls. 0 disables the gate.
		int shadow_pass_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_SHADOWPASS", 0), 0, 1));
			return value;
		}

		int offscreen_rt_min_area()
		{
			static const int value =
				static_cast<int>(remix_ps2::read_env_int(L"PCSX2_REMIX_MINRT", 0));
			return value;
		}

		void drawdump_write(const std::string& line)
		{
			static constexpr u32 max_lines = 40000;
			static u32 written = 0;
			static bool tried = false;
			static std::FILE* file = nullptr;

			if (!tried)
			{
				tried = true;

				const std::string& dir = EmuFolders::Logs.empty() ? EmuFolders::AppRoot : EmuFolders::Logs;
				const std::string path = Path::Combine(dir, "remix_draws.txt");
				file = FileSystem::OpenCFile(path.c_str(), "w");

				if (file)
					INFO_LOG("Remix: writing per-draw state to '{}'", path);
				else
					ERROR_LOG("Remix: could not open '{}' for the per-draw dump", path);
			}

			if (!file || written >= max_lines)
				return;

			++written;
			std::fputs(line.c_str(), file);
			std::fputc('\n', file);
			std::fflush(file);
		}

		// --- masked multi-pass dump ---------------------------------------------------------
		//
		// Separate from drawdump_write, which only ever sees draws that PASSED the gates. The
		// masked passes are rejected, so nothing has ever been able to look at them, and they are
		// where Rainbow Six 3's lightmap lives -- 23,592 of 100,469 draws in a measured frame set.
		u64 fbmsk_dump_limit()
		{
			static const u64 value =
				static_cast<u64>(std::max<s64>(0, remix_ps2::read_env_int(L"PCSX2_REMIX_FBMSKDUMP", 0)));
			return value;
		}

		u64 s_fbmsk_dumped = 0;

		// --- lightmap discovery -------------------------------------------------------------
		//
		// PCSX2_REMIX_LIGHTMAPS (default 1) records the content hash of every texture that arrives
		// on a masked multi-pass draw, and writes the set to logs\remix_lightmaps.txt at shutdown
		// and whenever it grows.
		//
		// These hashes are the values a modder types into rtx.conf, and the keys the emissive and
		// category lists match on -- and until now they were never computed for these textures at
		// all, because the FBMSK gate rejects the draw before materials::bind() runs. On Rainbow
		// Six 3 that is the entire level lighting: the title has no runtime lights.
		//
		// Deliberately writes a REPORT and does not touch the per-game conf. The conf is a file the
		// user edits by hand, and an emulator silently rewriting it while the game runs is how edits
		// get lost. The report is meant to be reviewed and merged.
		int lightmap_discovery_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_LIGHTMAPS", 1), 0, 1));
			return value;
		}

		// PCSX2_REMIX_LIGHTMAPINJECT: submit the masked multi-pass draws instead of rejecting them,
		// as static decals so their texture can light the scene.
		//
		//   0 = reject (default, and what shipped)
		//   1 = submit as DECAL_STATIC
		//
		// Rainbow Six 3 has no runtime lights at all, so these passes are the entire level lighting
		// and rejecting them means substituting one injected fill light for the whole thing. That is
		// why the weapon blows out white while walls stay flat, and it is very likely why albedo
		// reads near-greyscale: a lightmapped title keeps its base textures desaturated because the
		// lightmap carries the colour.
		//
		// DECAL_STATIC is the load-bearing part. Submitting these as ordinary geometry is what
		// originally put four coincident surfaces on every wall and z-fought in the path tracer --
		// the reason the gate exists. A decal is exactly the thing a renderer offsets to sit on a
		// surface without fighting it, and these are static by construction.
		//
		// Increment 1 (this) reuses the existing emissive-tag path: tag the discovered lightmap
		// hashes in the per-game conf and bind() already builds them with emissiveTexture set, so no
		// new material code is needed and the z-fighting risk gets tested cheaply. Each of the three
		// channel passes then emits white x its own greyscale channel, so the LIGHT is right and the
		// COLOUR is not -- three white emitters sum to grey. Increment 2 sets emissiveColorConstant
		// per channel from the FBMSK to recover colour.
		int lightmap_inject_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_LIGHTMAPINJECT", 0), 0, 1));
			return value;
		}

		// Which colour channel a masked pass writes. FBMSK bits SET are protected, so the byte that
		// is zero is the one written. Returns a unit RGB for that channel, or all-zero if the mask
		// is not a single-channel one (which would mean the 30/30/30 structure does not hold here).
		void fbmsk_channel(u32 mask, float out_rgb[3])
		{
			const bool writes_r = ((mask >> 0) & 0xFFu) == 0;
			const bool writes_g = ((mask >> 8) & 0xFFu) == 0;
			const bool writes_b = ((mask >> 16) & 0xFFu) == 0;

			out_rgb[0] = writes_r ? 1.f : 0.f;
			out_rgb[1] = writes_g ? 1.f : 0.f;
			out_rgb[2] = writes_b ? 1.f : 0.f;
		}

		// Fold the baked lightmap into the BASE surface's vertex colours.
		//
		// Measured (LMPAIR): the masked channel passes re-draw the base pass's vertices exactly --
		// 236,001 of 236,001, no exceptions -- so the lightmap can be sampled per vertex and
		// multiplied into the colour we already submit. That needs NO extra geometry, which is the
		// whole point: submitting the passes as decals put four coincident surfaces on every wall
		// and produced the black flickering squares that got LIGHTMAPINJECT reverted.
		//
		// Per-vertex, not per-pixel: high-frequency lightmap detail inside a quad is lost. The
		// geometry is 4-5 vertex quads, so each one still gets its own corner samples.
		int lightmap_fold_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_LIGHTMAPFOLD", 0), 0, 1));
			return value;
		}
		float lightmap_fold_scale()
		{
			return env_float_signed(L"PCSX2_REMIX_LIGHTMAPSCALE", 1.f);
		}
		// Floor under the sampled lightmap: fully shadowed texels are 0, and zero albedo is black
		// under any light no matter how bright. The original never looked like that because the
		// hardware had an ambient term beneath the modulation. Remaps [0,1] to [floor,1] rather
		// than clamping, so lit areas keep their values and only the black end lifts.
		float lightmap_fold_floor()
		{
			return std::clamp(env_float_signed(L"PCSX2_REMIX_LIGHTMAPFLOOR", 0.f), 0.f, 1.f);
		}

		struct lm_image
		{
			std::vector<u8> px; // BGRA8
			u32 w = 0;
			u32 h = 0;
		};
		std::unordered_map<u64, lm_image> s_lm_images;

		struct lm_verts
		{
			std::vector<float> rgb; // 3 per vertex
			u32 n = 0;
			u8 seen = 0; // bit per channel
			u64 frame = 0;
		};
		std::unordered_map<u64, lm_verts> s_lm_verts;
		// The vertex range the last batched draw occupies, for in-frame lightmap modulation.
		size_t s_lm_span_group = 0;
		size_t s_lm_span_surface = 0;
		u32 s_lm_span_first = 0;
		u32 s_lm_span_count = 0;
		bool s_lm_span_valid = false;
		u64 s_lm_inframe = 0;
		u64 s_lm_applied = 0, s_lm_stored = 0, s_lm_decode_fail = 0, s_lm_images_made = 0;
		u64 s_lm_miss_nokey = 0, s_lm_miss_partial = 0, s_lm_miss_vcount = 0;

		struct lightmap_entry
		{
			u64 hash = 0;
			u32 tbp0 = 0;
			u32 cbp = 0;
			u32 psm = 0;
			u32 width = 0;
			u32 height = 0;
			u32 mask = 0;
			u64 draws = 0;
		};

		std::unordered_map<u64, lightmap_entry> s_lightmaps;
		bool s_lightmaps_dirty = false;

		void write_lightmap_report()
		{
			if (s_lightmaps.empty())
				return;

			const std::string path = Path::Combine(EmuFolders::Logs, "remix_lightmaps.txt");
			std::FILE* file = FileSystem::OpenCFile(path.c_str(), "w");
			if (!file)
				return;

			// Sorted, so the file is stable across runs and diffable -- an unordered_map's order is
			// not, and a report that reshuffles every session cannot be compared to the last one.
			std::vector<const lightmap_entry*> sorted;
			sorted.reserve(s_lightmaps.size());
			for (const auto& [key, entry] : s_lightmaps)
				sorted.push_back(&entry);
			std::sort(sorted.begin(), sorted.end(),
				[](const lightmap_entry* a, const lightmap_entry* b) { return a->hash < b->hash; });

			std::fprintf(file,
				"# Lightmap textures discovered on masked multi-pass draws.\n"
				"#\n"
				"# These arrive on draws the FBMSK gate rejects, so they never become Remix materials\n"
				"# and cannot be tagged in the developer menu. Rainbow Six 3 encodes a full-colour\n"
				"# lightmap as ONE index texture plus THREE CLUTs, one per colour channel, because the\n"
				"# PS2 blend equation (A-B)*C+D takes C from an alpha source and so cannot multiply by\n"
				"# per-channel colour. Each distinct cbp below is one channel of the same lightmap.\n"
				"#\n"
				"# %zu distinct textures this session.\n"
				"#\n"
				"# hash               tbp0    cbp     psm   size      mask        draws\n",
				s_lightmaps.size());

			for (const lightmap_entry* e : sorted)
			{
				std::fprintf(file, "0x%016llX   0x%04x  0x%04x  0x%02x  %4ux%-4u  0x%08x  %llu\n",
					static_cast<unsigned long long>(e->hash), e->tbp0, e->cbp, e->psm,
					e->width, e->height, e->mask, static_cast<unsigned long long>(e->draws));
			}

			// Ready to paste into the per-game conf. Not written there automatically on purpose.
			std::fprintf(file, "\n# Paste into <SERIAL>.conf to tag them, e.g.:\n# rtx.emissiveTextures = ");
			bool first = true;
			for (const lightmap_entry* e : sorted)
			{
				std::fprintf(file, "%s0x%016llX", first ? "" : ", ",
					static_cast<unsigned long long>(e->hash));
				first = false;
			}
			std::fprintf(file, "\n");

			std::fclose(file);
			INFO_LOG("Remix: wrote {} discovered lightmap textures to {}", s_lightmaps.size(), path);
		}

		void fbmsk_dump_write(const std::string& line)
		{
			static std::FILE* file = nullptr;
			static bool tried = false;

			if (!file && !tried)
			{
				tried = true;
				const std::string path = Path::Combine(EmuFolders::Logs, "remix_fbmsk.txt");
				file = FileSystem::OpenCFile(path.c_str(), "w");
				if (!file)
					return;

				INFO_LOG("Remix: dumping masked multi-pass draws to {}", path);
			}

			if (!file)
				return;

			std::fputs(line.c_str(), file);
			std::fputc('\n', file);
			std::fflush(file);
		}

		// The GS register state a draw's instance-side properties are derived from, passed by value:
		// GSRendererHW's members are private and RemixSubmit::OnDrawPrims is its only declared friend
		// (GSRendererHW.h:37), so a namespace-scope helper cannot read them itself.
		struct draw_regs
		{
			bool depth_read = false;
			bool depth_write = false;
			u32 ate = 0;
			u32 atst = 0;
			u32 aref = 0;
			u32 abe = 0;
			u32 tfx = 0;
			// Whether this draw's texture is a render target. Sky mode 3 keys on it; see
			// classify_sky.
			bool samples_target = false;
			// The draw's nearest vertex in eye depth, w = 1/Q. The far-distance requirement on sky
			// classification reads it; see sky_min_w(). Left 0 by a caller that has not computed
			// it, which reads as "at the eye" and can only ever fail the requirement -- never pass
			// it by accident.
			float min_w = 0.f;
			GIFRegALPHA alpha{};
		};

		// Everything about a draw that lives on the *instance* rather than the mesh: the blend and
		// alpha-test state Remix takes from InstanceInfoBlendEXT, and the category flags. Pulled
		// out of OnDrawPrims because frame batching has to know it before it can decide which batch
		// a draw belongs to -- draws that disagree on any of it cannot share an instance.
		struct draw_state
		{
			remixapi_InstanceInfoBlendEXT blend{};
			remixapi_InstanceCategoryFlags categories = 0;
			bool is_sky = false;
			bool is_cutout = false;
		};

		draw_state build_draw_state(const draw_regs& regs, u64 material_hash, u64 draw_ordinal,
			bool untextured, bool force_sky)
		{
			const bool depth_read = regs.depth_read;
			const bool depth_write = regs.depth_write;
			const bool is_sky = force_sky || classify_sky(depth_read, depth_write, draw_ordinal,
				regs.samples_target, regs.min_w);

			// The user's own tags, from the Remix conf layers. dxvk-remix only applies its hash
			// lists on the native D3D9 path (setupCategoriesForTexture, rtx_types.cpp:348, whose one
			// caller is d3d9_rtx.cpp:1064); an API instance's categories come solely from this
			// field (rtx_remix_api.cpp:803). So a tag made in the developer menu does nothing at all
			// unless we look it up ourselves and OR it in here.
			const remixapi_InstanceCategoryFlags tagged = remix_ps2::materials::categories_for(material_hash);

			remixapi_InstanceInfoBlendEXT blend{};
			blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
			blend.pNext = nullptr;
			blend.alphaTestEnabled = regs.ate ? 1 : 0;
			blend.alphaTestReferenceValue = static_cast<u8>(regs.aref);
			blend.alphaTestCompareOp = to_d3d_compare(regs.atst);
			blend.alphaBlendEnabled = regs.abe ? 1 : 0;
			to_d3d_blend(regs.alpha, blend.srcColorBlendFactor, blend.dstColorBlendFactor);
			blend.colorBlendOp = 0; // VK_BLEND_OP_ADD (D3DBLENDOP_ADD is 1; the runtime casts to VkBlendOp)
			blend.srcAlphaBlendFactor = blend.srcColorBlendFactor;
			blend.dstAlphaBlendFactor = blend.dstColorBlendFactor;
			blend.alphaBlendOp = 0; // VK_BLEND_OP_ADD
			// TFX: MODULATE 0, DECAL 1, HIGHLIGHT 2, HIGHLIGHT2 3. Only the first two are
			// expressible as a fixed-function stage; the highlight modes degrade to modulate.
			//
			// PCSX2_REMIX_TEXSTAGE gates this separately from the blend fields so colour can be
			// chased without giving up the alpha-state fix. 0 leaves the fields zeroed for the
			// runtime's own default, 1 derives them from TFX.
			//
			// MEASURED on Rainbow Six 3 (-state 9), identical scene, PrintWindow capture, pixels
			// with max channel > 12 counted as lit:
			//   TEXSTAGE=1 (derived): lit 2,468,536  mean sat 0.0918  coloured 12.79% of lit
			//   TEXSTAGE=0 (zeroed):  lit 1,110,107  mean sat 0.0545  coloured  3.72% of lit
			// Sending the derived stage gives 3.4x the coloured pixels and 2.2x the lit area, so
			// these fields are NOT the "colour is missing" regression -- they are carrying the
			// texture's contribution and zeroing them is what removes it. Default 1.
			if (texture_stage_mode() != 0)
			{
				const bool decal = (regs.tfx == 1);
				// RtTextureArgSource { None 0, Texture 1, VertexColor0 2, TFactor 3 } and
				// DxvkRtTextureOperation { Disable 0, SelectArg1 1, SelectArg2 2, Modulate 3,
				// Modulate2x 4, ... } -- the fork's OWN enums, cast to directly at
				// rtx_remix_api.cpp:948-954. They are NOT D3DTA_/D3DTOP_ values.
				//
				// This was the flat-albedo defect: D3DTA_TEXTURE (2) read as VertexColor0, so the
				// runtime was told to take surface colour from vertex colour and ignore the texture
				// entirely -- exactly the reported "textures bind but carry no detail, the colour
				// comes from vertex colour". D3DTOP_MODULATE (4) read as Modulate2x also doubled
				// every surface's brightness, which is the washed-out look on top of it.
				blend.textureColorArg1Source = 1; // Texture
				blend.textureColorArg2Source = 2; // VertexColor0
				blend.textureColorOperation = decal ? 1u : 3u; // SelectArg1 : Modulate
				blend.textureAlphaArg1Source = 1; // Texture
				blend.textureAlphaArg2Source = 2; // VertexColor0
				blend.textureAlphaOperation = decal ? 1u : 3u;

				// An untextured draw has no D3DTA_TEXTURE to read. MODULATE then multiplies diffuse
				// by a texture that is not there and SELECTARG1 selects it outright -- either way
				// the surface comes out BLACK. That is the pure-black albedo debug view: since
				// UNTEXZ started submitting these they are the majority of a SOCOM frame
				// (715,916 of 948,303), so most of the world was being told to take its colour
				// from nothing, and rendered black with only specular highlights on it. Which is
				// exactly the "black/white" look the user reported, and no amount of injected
				// light could have fixed it -- more light only brightens the highlights.
				//
				// SELECTARG2 takes D3DTA_DIFFUSE, the vertex colour, which is the only colour an
				// untextured draw has. Alpha likewise.
				if (untextured)
				{
					blend.textureColorOperation = 2u; // SelectArg2 -> vertex colour
					blend.textureAlphaOperation = 2u;
				}
			}
			blend.tFactor = 0xFFFFFFFFu;
			blend.isTextureFactorBlend = 0;
			blend.writeMask = 0xF; // D3DCOLORWRITEENABLE_ALL
			// See vcolor_baked_mode(): SOCOM's only baked lighting is its vertex colour, and this is
			// what tells the runtime to treat it as irradiance instead of as albedo.
			blend.isVertexColorBakedLighting = (vcolor_baked_mode() != 0) ? 1 : 0;

			// Alpha-tested draws are cut-outs -- foliage, decals, muzzle flashes -- and submitting
			// them as ordinary blended geometry makes the whole quad participate in lighting rather
			// than just the kept texels, so the billboard's triangle silhouette shows and the
			// denoiser flickers on it. ALPHA_BLEND_TO_CUTOUT is the runtime's purpose-built path
			// for exactly this (dxvk-remix rtx_instance_manager.cpp:597,
			// forceAlphaTest = categories.test(InstanceCategories::AlphaBlendToCutout)).
			//
			// Keyed on ATE because that is the guest saying "this is a cut-out" itself. Rainbow Six
			// 3's dump has ATE=0 on every draw, so this is inert there and carries no regression
			// risk for it; it is aimed at SOCOM's foliage and is NOT yet verified against a SOCOM
			// draw dump -- the counter is how that gets checked.
			const bool is_cutout =
				(cutout_mode() != 0) && regs.ate && (regs.atst != 1); // not ALWAYS

			draw_state out{};
			out.blend = blend;
			out.is_sky = is_sky;
			out.is_cutout = is_cutout;
			// A pushed sky must not carry REMIXAPI_INSTANCE_CATEGORY_BIT_SKY: the runtime deletes
			// that category outright on this path, so the bit would remove the geometry the push
			// just placed. Stripped from the tag list as well, which is where it usually arrives.
			const u32 sky_bit = static_cast<u32>(REMIXAPI_INSTANCE_CATEGORY_BIT_SKY);
			const bool pushing_sky = sky_distance() > 0.f;
			out.categories = (pushing_sky ? (tagged & ~sky_bit) : tagged) |
			                 ((is_sky && !pushing_sky) ? sky_bit : 0u) |
			                 (is_cutout ? static_cast<u32>(REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT) : 0u);
			return out;
		}

		// Appends the draw currently in the scratch buffers to its batch group, under the surface
		// for its material. Indices are rebased onto the surface's running vertex count.
		//
		// Vertices go in exactly as they were built -- world or view space, whichever the frame is
		// submitting in -- so the batch instance carries the identity transform and there is no
		// registration to get wrong. Batching and stable identity are alternatives, not partners.
		void batch_append(u64 group_key, const draw_state& ds, const remix_ps2::materials::binding& material,
			u64 draw_hash)
		{
			size_t group_index;

			if (const auto found = s_batch_group_of_key.find(group_key); found != s_batch_group_of_key.end())
			{
				group_index = found->second;
			}
			else
			{
				group_index = s_batch_groups_used++;
				if (s_batch_groups.size() < s_batch_groups_used)
					s_batch_groups.emplace_back();

				batch_group& fresh = s_batch_groups[group_index];
				fresh.blend = ds.blend;
				fresh.categories = ds.categories;
				fresh.surfaces_used = 0;
				fresh.surface_of_material.clear();
				fresh.content = fnv_seed;
				s_batch_group_of_key.emplace(group_key, group_index);
			}

			batch_group& group = s_batch_groups[group_index];
			group.content = fnv_mix(group.content, draw_hash);

			size_t surface_index;

			if (const auto found = group.surface_of_material.find(material.content_hash);
				found != group.surface_of_material.end())
			{
				surface_index = found->second;
			}
			else
			{
				surface_index = group.surfaces_used++;
				if (group.surfaces.size() < group.surfaces_used)
					group.surfaces.emplace_back();

				batch_surface& fresh = group.surfaces[surface_index];
				fresh.material_hash = material.content_hash;
				fresh.material = material.material;
				fresh.vertices.clear();
				fresh.indices.clear();
				group.surface_of_material.emplace(material.content_hash, surface_index);
			}

			batch_surface& surface = group.surfaces[surface_index];
			const u32 base = static_cast<u32>(surface.vertices.size());

			surface.vertices.insert(surface.vertices.end(), s_scratch_vertices.begin(), s_scratch_vertices.end());
			// Where this draw's vertices landed, so the masked lightmap passes that follow it in the
			// SAME frame can modulate them in place. The batch is not flushed until the end of the
			// frame, so there is no cross-frame key to go stale -- which is what made the fold hit
			// only while the camera was still.
			s_lm_span_group = group_index;
			s_lm_span_surface = surface_index;
			s_lm_span_first = base;
			s_lm_span_count = static_cast<u32>(s_scratch_vertices.size());
			s_lm_span_valid = true;
			surface.indices.reserve(surface.indices.size() + s_scratch_indices.size());

			for (const u32 index : s_scratch_indices)
				surface.indices.push_back(base + index);
		}

		// --- hold-previous-window (step 4B) -----------------------------------------------------
		//
		// PCSX2_REMIX_HOLDEMPTY makes a presented window that submits NO geometry re-present the
		// previous window instead of presenting an empty scene.
		//   0 = off, the empty window presents empty (pre-4B behaviour)
		//   1 = hold the GEOMETRY only. The window's own, newer camera is submitted.
		//   2 = hold the geometry AND the CAMERA (DEFAULT), so the held window is a true duplicate
		//       of the window it repeats.
		//   3 = SKIP THE PRESENT entirely. Nothing is submitted and Present is not called, so the
		//       previous frame stays on screen and the present rate becomes the game's own 40 Hz.
		//       Opt-in; best on a VRR display, see below.
		//
		// WHAT IS MEASURED. FRAMETRACE, armed in gameplay with FRAMETRACEAFTER = 1200 (171 windows,
		// f=2002..2172, the user's own in-mission run), says the period-3 signal this title has is
		// exactly one thing and nothing else:
		//   - sub = 0 on 57 of 171 windows (33.3%), and the spacing between those windows is
		//     EXACTLY 3 in 56 of 56 gaps. Every third presented window submits zero geometry.
		//   - sub on every other window is a flat 300-399. No dips, no sparse windows.
		//   - sol (distinct NORMALISED solvers per window) = 1 on all 114 non-empty windows, so no
		//     window straddles two cameras and branch 4A cannot help here; rings = 2 on all 114,
		//     i.e. two raw matrices that normalise to ONE camera.
		//   - miss = 0 total, fresh = 0 on zero windows, age deterministic (1 non-empty, 2 empty).
		// That is the game rendering at 40 fps against a 60 Hz present, which the run's own status
		// bar already said out loud (`FPS: 39-40  VPS: 60-61`), and it is the ONLY thing in the
		// trace that varies with period 3.
		//
		// WHAT IS INFERRED, and this branch is falsifiable on it. That the empty window is the
		// shattered frame in the user's clip is NOT proven: the trace and the clip are different
		// runs and the identity was never established frame-for-frame. It is the strongest
		// remaining candidate because nothing else in the measured path varies with period 3 --
		// not because anyone observed it. If holding the previous window does not remove the
		// shattering, the mechanism is elsewhere and the diagnosis has to start again; it does not
		// mean the hold needs tuning.
		//
		// WHY THE DETECTION IS A ZERO TEST AND NOT A THRESHOLD. The plan drafted this as "sub under
		// half the rolling median" back when a sparse window was still a live hypothesis. It is not:
		// sub is either 0 or 300-399, with nothing in between, on 171 consecutive windows. A
		// rolling-median heuristic against that distribution can only add false positives, so the
		// test is s_submitted_this_frame == 0 and nothing more.
		//
		// THE CAMERA ON A HELD WINDOW, and a REFUTATION of the reasoning this shipped with.
		//
		// Mode 1 shipped first, holding only the geometry, on the argument that the camera choice
		// was moot: `age = 2` on every empty FRAME line was read as "the active camera on an empty
		// window is already the one the held geometry was built with, so holding it or advancing it
		// is the same picture". **That is measured FALSE.** Checked directly against the user's
		// gameplay trace (FRAME lines, f=2182..2312):
		//     empty windows whose camera == the previous window's : 0
		//     empty windows whose camera CHANGED                  : 43
		// The camera advances on EVERY held window. Sample:
		//     f=2184 sub=121 used=C9D87690
		//     f=2185 sub=0   used=3A94504C   <- held geometry, NEW camera
		//     f=2186 sub=121 used=3A94504C
		// and the camera sequence over windows reads A,A,B,C,C,D,E,E -- advancing unevenly. `age`
		// counts windows since the hash last CHANGED; it never said the empty window shared the
		// previous window's camera, and reading it that way was the error.
		//
		// So under mode 1 a held window re-path-traces last game-frame's geometry FROM A MOVED EYE.
		// The image shifts while the geometry does not, which is a smear/judder artifact on turns
		// that the user reported after mode 1 shipped.
		//
		// WHY MODE 2 IS THE DEFAULT. The game is natively 40 fps -- the user confirmed it runs at
		// 40 fps with RTX off -- so the 40-on-60 cadence is not a cost this backend introduced and
		// there is no frame rate to recover. Ordinary 40-on-60 judder is the floor and is not ours
		// to fix. What IS ours is the artifact on top of it: stock PCSX2 re-blits the SAME PIXELS
		// on a duplicate vsync, a true frame repeat, while mode 1 renders a new viewpoint onto
		// stale geometry. Mode 2 re-submits the previous window's camera along with its geometry,
		// so the held window is a true duplicate and the backend matches what the game does
		// without RTX. Mode 1 stays reachable for the A/B, unchanged.
		//
		// HOW, without a second SetupCamera. The earlier objection -- that re-submitting a camera
		// means two SetupCamera calls per present, whose ordering against DrawInstance is untested
		// on this runtime -- was legitimate, so the decision moved instead of the call: OnVSync runs
		// submit_camera() before batch_flush(), and s_submitted_this_frame is already final for the
		// window at the top of OnVSync (it is cleared at the bottom). So the hold is DECIDED before
		// submit_camera() and simply changes which camera that one call submits. Still exactly one
		// SetupCamera per present, no reordering, no second call. decide_hold_window() is that
		// decision and hold_previous_window() only executes it.
		//
		// Note for whoever next reads a trace: the FRAME line's `used=` is the RESOLVED camera and
		// is unchanged by this. Under mode 2 a held window SUBMITS the previous window's camera,
		// which `used=` does not show -- read `cam-held` on the stats line for that.
		//
		// MODE 3, AND WHY IT IS NOT THE DEFAULT. Modes 1 and 2 both manufacture a 60 Hz stream out
		// of 40 fps of content, so each game frame occupies one or two refreshes unevenly and some
		// judder is unavoidable -- the floor described above. On a VRR display that floor is not
		// necessary at all: present 40 times a second, each present a unique frame, and the panel
		// refreshes to match. The user's display is an LG G3 (120 Hz OLED, HDMI 2.1 VRR /
		// G-Sync Compatible), so mode 3 skips the present on an empty window entirely. It also
		// removes ~33% of the path-tracing work per second, as a side effect rather than a goal.
		//
		// It is NOT the default because mode 2 is correct on any display while mode 3 is correct
		// only on a variable-refresh one -- on a fixed 60 Hz panel a dropped present is a stutter,
		// not a saved frame -- and because the risk below is audited but not RUN.
		//
		// WHAT WAS AUDITED FOR MODE 3, since "skip a Present" is the kind of change that hangs a
		// title with this one's history:
		//   - EMULATION PACING AND AUDIO ARE NOT OURS TO BREAK. RemixSubmit::OnVSync() is called
		//     from GSRenderer::VSync (GSRenderer.cpp:645) BEFORE the emulator's own present block
		//     (:649-720), and that block -- BeginPresentFrame / PresentRect / EndPresentFrame,
		//     PerformanceMetrics::Update, the vsync throttle -- runs unconditionally whatever Remix
		//     does. PCSX2 already skips its own presentation on duplicate frames there
		//     (`skip_frame || ShouldSkipPresentingFrame()`, :649). So the emulator's frame limiter
		//     never observes the Remix Present and cannot be paced by it.
		//   - NOTHING IS LEFT UNPRESENTED IN THE RUNTIME. A skipped window makes ZERO Remix API
		//     calls by construction: no SetupCamera, no CreateMesh (there is no geometry), no
		//     DrawInstance, no light instances, no beacon. There is no accumulated scene waiting
		//     for a Present, which is the only way a missing Present could strand runtime state.
		//   - OUR OWN PER-FRAME BOOKKEEPING IS NOT PRESENT-DRIVEN. Everything after Present in
		//     OnVSync runs unchanged on a skipped window: s_frame_counter still increments, so
		//     batch_reap() ages handles at the same rate and the held set stays valid for exactly
		//     as long; the viewport latch, the extent refutation (inert -- s_frame_bounds is empty
		//     on an empty window), the stats tick (s_frame_counter % interval), mesh and material
		//     reaping, the config poll and apply_live_knobs all still run. The beacon's empty-frame
		//     STREAK still counts the window; only its submission is suppressed.
		//   - IT CAN NEVER SKIP TWICE IN A ROW. A skip consumes the held set exactly as a hold
		//     does (s_held_used), so a skipped present is always followed by a real one. The screen
		//     cannot go stale for more than one window, and a genuine drought -- a load, a
		//     cinematic -- presents normally from its second window onward.
		// WHAT WAS NOT VERIFIED: any of this at runtime. Whether the Remix runtime and the Vulkan
		// swapchain behind it are content with a variable present cadence cannot be established
		// without launching the emulator, which this session did not do. Mode 3 is opt-in for that
		// reason, and the recovery from anything unexpected is one conf line back to 2.
		//
		// SAFETY, against the risk register's "hold detection fires on legitimate cuts -> visible
		// freeze-frames": a held set is used AT MOST ONCE. Two empty windows in a row -- a load, a
		// cinematic, a fade -- present the second one empty exactly as today, so the worst case this
		// can produce is one repeated frame at a scene boundary, not a frozen screen. A live world
		// camera is required as well, which excludes menus and loading screens outright.
		//
		// DEFAULT 2, unlike PERDRAWCAM's 0. That knob shipped off because its census could answer
		// the question without changing a pixel; this one has no such reading -- the whole claim is
		// about what the screen looks like -- and the 'empty' counter measures the population it
		// acts on whatever the mode. 1 is the shipped-and-confirmed geometry-only behaviour, 0
		// restores the pre-4B behaviour exactly.
		//
		// READ LIVE, NEVER LATCHED. Same trap as everything else the .conf delivers: the renderer
		// goes live at t=9.774 s and the per-game .conf applies at t=10.008 s, so a `static const`
		// caches the default before the value exists and never re-reads it. FRAMETRACE shipped that
		// way and produced zero output on a run whose own log confirmed it was applied -- see
		// frametrace_frames(). Logged on CHANGE rather than once, for the same reason (the first
		// get() legitimately reports the default).
		int hold_empty_mode()
		{
			static live_int value(L"PCSX2_REMIX_HOLDEMPTY", 2, 0, 3);
			static int logged = -1;

			const int mode = value.get();
			if (mode != logged)
			{
				logged = mode;
				INFO_LOG("Remix: HOLDEMPTY = {} -- {} (requires PCSX2_REMIX_BATCH = 1; read "
						 "'hold-empty' on the stats line, and treat 'offcadence' as the "
						 "misdetection tell)",
					mode,
					(mode == 3) ? "a window that submits NO geometry is NOT PRESENTED AT ALL -- the "
								  "previous frame stays on screen and the present rate becomes the "
								  "game's own. Best on a variable-refresh display; on a fixed-rate "
								  "one a dropped present is a stutter. Never two in a row" :
					(mode == 2) ? "a presented window that submits NO geometry re-presents the "
								  "previous window's geometry AND its camera -- a true duplicate "
								  "frame, at most once in a row" :
					(mode == 1) ? "a presented window that submits NO geometry re-presents the "
								  "previous window's geometry under this window's own newer camera "
								  "(the eye moves over stale geometry; smears on turns)" :
								  "an empty window presents an empty scene (pre-4B behaviour)");
			}

			return mode;
		}

		// Cadence bookkeeping, shared by every mode that acts on an empty window. A repeated frame
		// (modes 1 and 2) and a skipped present (mode 3) are the same event as far as "did this
		// fire where it was supposed to" is concerned, and `offcadence` is only a tell if every
		// mode feeds it -- mode 3 in particular, which is the one worth watching closely.
		void note_hold_cadence()
		{
			if (s_have_held_before)
			{
				const u64 gap = (s_frame_counter >= s_last_hold_window) ?
									(s_frame_counter - s_last_hold_window) : 0;
				s_stats.hold_gap_last = gap;

				if (gap == 3)
					++s_stats.hold_gap3;
				else
					++s_stats.hold_offcadence;
			}

			s_have_held_before = true;
			s_last_hold_window = s_frame_counter;
		}

		// Decides whether THIS window holds, and counts every window it looks at. Must be called
		// exactly once per window, at the top of OnVSync and BEFORE submit_camera(): mode 2 changes
		// which camera that call submits, so the decision cannot wait for flush time. Everything it
		// reads is already final for the window -- s_submitted_this_frame is cleared at the bottom
		// of OnVSync, and s_active_camera is only replaced by resolve_world_camera(), also at the
		// bottom -- so deciding early reads exactly what deciding late would have read.
		//
		// Rides the batch path because that is the only path with a retained set to re-present, and
		// it is what the deployed config uses (PCSX2_REMIX_BATCH = 1). Under BATCH = 0 nothing is
		// ever captured, so this counts the empty windows and does nothing else.
		bool decide_hold_window()
		{
			s_hold_pending = false;

			// The whole detection. Not a threshold -- see hold_empty_mode().
			if (s_submitted_this_frame != 0)
				return false;

			++s_stats.hold_empty_windows;

			// Nothing captured yet: BATCH = 0, or the first window of a scene.
			if (s_held_instances.empty() || hold_empty_mode() == 0)
				return false;

			// At most one repeat per real window. This is the freeze-frame guard, and it is also
			// what keeps a load screen looking like a load screen.
			if (s_held_used)
			{
				++s_stats.hold_skip_consecutive;
				return false;
			}

			// The handles are borrowed, not owned. batch_reap() keeps a mesh while
			// created_frame + BATCHRETAIN > s_frame_counter, so this is that same predicate: with
			// the deployed BATCHRETAIN = 16 it never fires, and with BATCHRETAIN = 1 it refuses
			// every hold rather than handing the runtime a destroyed handle.
			if ((s_held_frame + batch_retain_frames()) <= s_frame_counter)
			{
				++s_stats.hold_skip_stale;
				s_held_instances.clear();
				return false;
			}

			// A live world camera on both sides. The held vertices are in whichever space the
			// window that built them submitted in, and re-presenting world-space geometry under a
			// view-space camera (or the reverse) would place all of it wrong -- which is the very
			// failure this is meant to remove. Also excludes menus and loads, which never solve a
			// camera at all. Under mode 2 it is s_held_camera that is submitted, and the same test
			// covers it: s_held_world IS s_held_camera.valid as of the window that captured them.
			if (!s_active_camera.valid || !s_held_world)
			{
				++s_stats.hold_skip_nocam;
				return false;
			}

			s_hold_pending = true;
			return true;
		}

		// Executes the decision decide_hold_window() already made. Called from batch_flush on the
		// empty-window path, i.e. after submit_camera and before Present -- so under mode 2 the
		// camera these instances are drawn against has already been submitted as the held one.
		// Returns true if anything was drawn.
		bool hold_previous_window()
		{
			if (!s_hold_pending)
				return false;

			const remixapi_Interface& api = s_remix.api();
			const bool attach_blend = (alpha_state_mode() == 2);
			u64 drawn = 0;

			for (held_instance& held : s_held_instances)
			{
				if (!held.handle)
					continue;

				remixapi_InstanceInfo instance{};
				instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
				instance.pNext = attach_blend ? &held.blend : nullptr;
				instance.categoryFlags = held.categories;
				instance.mesh = held.handle;
				instance.transform = held.transform;
				instance.doubleSided = 1;

				if (remix_ps2::guarded_draw_instance(api.DrawInstance, &instance) !=
					REMIXAPI_ERROR_CODE_SUCCESS)
				{
					++s_stats.hold_failed;
					continue;
				}

				s_frame_instanced_keys.insert(held.mesh_hash);
				++drawn;
			}

			// Consumed even if every DrawInstance failed: a set that cannot be drawn once will not
			// draw on the next window either, and retrying it would defeat the freeze-frame guard.
			s_held_used = true;

			if (drawn == 0)
				return false;

			++s_stats.hold_windows;
			s_stats.hold_instances += drawn;
			note_hold_cadence();
			return true;
		}

		// Turns the frame's accumulated groups into meshes and instances them. Must run before
		// Present, and is the whole point of batching: one CreateMesh and one DrawInstance per
		// group, however many draws went into it.
		//
		// A window with no groups is where hold-previous-window lives: that is the empty window the
		// step-4B measurement found, and it is the last point before Present at which anything can
		// still be put on the screen.
		void batch_flush()
		{
			if (s_batch_groups_used == 0)
			{
				s_batch_group_of_key.clear();
				hold_previous_window();
				return;
			}

			const remixapi_Interface& api = s_remix.api();
			u64 vertices_this_frame = 0;
			u64 surfaces_this_frame = 0;

			// Start this window's held set. Captured unconditionally, not behind hold_empty_mode():
			// the knob is live and the .conf is re-polled about once a second, so flipping it on
			// mid-run has to find a set already standing. The cost is one push_back per GROUP, and
			// this title runs a handful of groups per frame.
			s_held_instances.clear();
			s_held_frame = s_frame_counter;
			s_held_world = s_active_camera.valid;
			s_held_used = false;
			// The camera this window's geometry was un-projected with, which is also the camera
			// submit_camera() submitted at the top of this same OnVSync -- resolve_world_camera()
			// does not run until the bottom. Mode 2 hands this straight back to submit_camera() on
			// the next window if that window turns out to be empty.
			s_held_camera = s_active_camera;

			for (size_t g = 0; g < s_batch_groups_used; ++g)
			{
				batch_group& group = s_batch_groups[g];

				s_batch_surface_scratch.clear();
				u64 hash = fnv_seed;
				hash = fnv_mix(hash, s_frame_counter);
				hash = fnv_mix(hash, g);

				for (size_t i = 0; i < group.surfaces_used; ++i)
				{
					batch_surface& surface = group.surfaces[i];

					if (surface.vertices.empty() || surface.indices.empty())
						continue;

					remixapi_MeshInfoSurfaceTriangles triangles{};
					triangles.vertices_values = surface.vertices.data();
					triangles.vertices_count = surface.vertices.size();
					triangles.indices_values = surface.indices.data();
					triangles.indices_count = surface.indices.size();
					triangles.skinning_hasvalue = 0;
					triangles.material = surface.material;
					s_batch_surface_scratch.push_back(triangles);

					vertices_this_frame += surface.vertices.size();
					++surfaces_this_frame;
				}

				if (s_batch_surface_scratch.empty())
					continue;

				remixapi_MeshInfo mesh_info{};
				mesh_info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
				mesh_info.pNext = nullptr;
				// Unique per frame per group: the geometry is camera-derived and genuinely new
				// every frame, so reusing a hash would ask the runtime to treat different
				// geometry as the same object.
				// Content key when reusing, so identical geometry keeps its handle. The old key
				// (frame counter + group index) is retained for the non-reuse path.
				const u64 mesh_key = (batch_reuse_mode() != 0) ? group.content : hash;
				mesh_info.hash = (mesh_key == 0) ? 1 : mesh_key;
				mesh_info.surfaces_values = s_batch_surface_scratch.data();
				mesh_info.surfaces_count = s_batch_surface_scratch.size();

				remixapi_MeshHandle handle = nullptr;

				// Same geometry, same handle. Refreshing created_frame keeps a visible group alive;
				// the reap below retires whatever stops being drawn.
				const bool reuse = batch_reuse_mode() != 0;
				if (reuse)
				{
					const auto cached = s_batch_mesh_cache.find(mesh_info.hash);
					if (cached != s_batch_mesh_cache.end() && cached->second.handle)
					{
						cached->second.created_frame = s_frame_counter;
						handle = cached->second.handle;
						++s_batch_reused;
					}
				}

				if (!handle)
				{
					const auto create_mesh = api.CreateMeshBatched ? api.CreateMeshBatched : api.CreateMesh;
					const u32 status = remix_ps2::guarded_create_mesh(create_mesh, &mesh_info, &handle);

					if (status != REMIXAPI_ERROR_CODE_SUCCESS || !handle)
					{
						ERROR_LOG("Remix: batch CreateMesh failed for group {} ({} surfaces): {}",
							g, s_batch_surface_scratch.size(), remix_ps2::error_name(status));
						continue;
					}

					++s_stats.meshes_created;
					++s_stats.meshes_created_frame;
					++s_stats.batch_meshes_created;
					if (reuse)
						s_batch_mesh_cache[mesh_info.hash] = batch_mesh{handle, s_frame_counter};
					else
						s_batch_meshes.push_back(batch_mesh{handle, s_frame_counter});
				}

				remixapi_InstanceInfo instance{};
				instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
				instance.pNext = (alpha_state_mode() == 2) ? &group.blend : nullptr;
				instance.categoryFlags = group.categories;
				instance.mesh = handle;
				// The geometry is already in the submitted camera's space.
				instance.transform = s_identity_transform;
				instance.doubleSided = 1;

				const u32 draw_status = remix_ps2::guarded_draw_instance(api.DrawInstance, &instance);
				s_frame_instanced_keys.insert(mesh_info.hash);

				// Retained for the next window only, and only if it drew here: an instance the
				// runtime refused once is not worth re-presenting. The blend struct is copied by
				// value because instance.pNext must point at storage that outlives this call.
				if (draw_status == REMIXAPI_ERROR_CODE_SUCCESS)
				{
					held_instance held{};
					held.handle = handle;
					held.blend = group.blend;
					held.blend.pNext = nullptr;
					held.categories = group.categories;
					held.transform = instance.transform;
					held.mesh_hash = mesh_info.hash;
					s_held_instances.push_back(held);
				}
			}

			s_stats.batch_surfaces_peak = std::max(s_stats.batch_surfaces_peak, surfaces_this_frame);
			s_stats.batch_vertices_peak = std::max(s_stats.batch_vertices_peak, vertices_this_frame);

			s_batch_group_of_key.clear();
			s_batch_groups_used = 0;
		}

		// Teardown: drop every batch mesh and every accumulated group. Used on renderer close and
		// on a save-state load, where the whole scene is replaced in one step.
		void batch_discard()
		{
			if (s_remix.ok())
			{
				const remixapi_Interface& api = s_remix.api();

				for (const batch_mesh& entry : s_batch_meshes)
				{
					if (entry.handle)
					{
						remix_ps2::guarded_destroy_mesh(api.DestroyMesh, entry.handle);
						++s_stats.meshes_destroyed;
					}
				}
				for (const auto& [k, e] : s_batch_mesh_cache)
				{
					if (e.handle)
					{
						remix_ps2::guarded_destroy_mesh(api.DestroyMesh, e.handle);
						++s_stats.meshes_destroyed;
					}
				}
			}

			s_batch_meshes.clear();
			s_batch_mesh_cache.clear();
			s_batch_group_of_key.clear();
			s_batch_groups_used = 0;

			// The held set borrows those handles, so it dies with them. Both callers -- save-state
			// load and GS close -- replace the whole scene, which is also exactly the moment a
			// repeated frame would be visible as a stutter across a cut. s_frame_counter is zeroed
			// on close, so the cadence tracking resets here too or the first hold after a restart
			// reads as a wild off-cadence gap.
			s_held_instances.clear();
			s_held_frame = 0;
			s_held_world = false;
			s_held_used = false;
			s_held_camera = world_camera{};
			s_hold_pending = false;
			s_last_hold_window = 0;
			s_have_held_before = false;
		}

		// Releases batch meshes whose frame is far enough behind that nothing in flight can still
		// reference them.
		void batch_reap()
		{
			// Both containers, not just the flat list: with BATCHREUSE on, every mesh lives in the
			// cache and the flat list is always empty, so returning on it leaked the lot.
			if (s_batch_meshes.empty() && s_batch_mesh_cache.empty())
				return;

			const u64 retain = batch_retain_frames();
			const remixapi_Interface& api = s_remix.api();
			size_t write = 0;

			for (size_t i = 0; i < s_batch_meshes.size(); ++i)
			{
				const batch_mesh& entry = s_batch_meshes[i];

				if ((entry.created_frame + retain) > s_frame_counter)
				{
					s_batch_meshes[write++] = entry;
					continue;
				}

				if (entry.handle)
				{
					remix_ps2::guarded_destroy_mesh(api.DestroyMesh, entry.handle);
					++s_stats.meshes_destroyed;
					++s_stats.meshes_destroyed_frame;
				}
			}

			s_batch_meshes.resize(write);

			for (auto it = s_batch_mesh_cache.begin(); it != s_batch_mesh_cache.end();)
			{
				if ((it->second.created_frame + retain) > s_frame_counter)
				{
					++it;
					continue;
				}
				if (it->second.handle)
				{
					remix_ps2::guarded_destroy_mesh(api.DestroyMesh, it->second.handle);
					++s_stats.meshes_destroyed;
					++s_stats.meshes_destroyed_frame;
				}
				it = s_batch_mesh_cache.erase(it);
			}
		}

		void log_stats(bool force)
		{
			if (!force && (s_frame_counter == 0 || (s_frame_counter % stats_interval_frames()) != 0))
				return;

			// Flushed here rather than only at shutdown: device loss calls exit() and the clean
			// shutdown path never runs, which is how every other end-of-session report on this
			// project has been lost.
			if (s_lightmaps_dirty)
			{
				write_lightmap_report();
				s_lightmaps_dirty = false;
			}

			INFO_LOG("Remix: frame {} | seen {} submitted {} | meshes live {} (+{} -{}) | "
					 "skip: tri {} untex {} fst {} constq {} wflat {} notarget {} empty {} large {} "
					 "nonfinite {} poisoned {} meshbudget {} fbmsk {} coincident {} multipass {} minw {} minvw {} offscreenrt {} shadowpass {} | "
					 "warn stq {} | cam world {} fallback {} held {} expired {} skycam {} vmcam {} REFUSED {} | "
					 "maxpos {:.0f}/{:.0f} | scene r {:.0f} | sky {} sky_hash {} cutout {} | degen tris {} alldegen {} | "
					 "mesh/frame peak +{} -{} | instbudget-skip {} | distinct handles/frame avg {} peak {} | "
					 "pinned pool {} | id: mode {} reuse {} create {} rebuild {} probes {} | "
				 "batch: mode {} groups/frame avg {} peak {} | surfaces peak {} verts peak {} meshes {} reused {} cached {}",
				s_frame_counter, s_stats.draws_seen, s_stats.draws_submitted, s_meshes.size(),
				s_stats.meshes_created, s_stats.meshes_destroyed,
				s_stats.skip_not_triangle, s_stats.skip_untextured, s_stats.skip_fst,
				s_stats.skip_const_q, s_stats.skip_w_flat, s_stats.skip_no_target, s_stats.skip_empty,
				s_stats.skip_too_large, s_stats.skip_nonfinite, s_stats.skip_poisoned,
				s_stats.skip_mesh_budget, s_stats.skip_fbmsk, s_stats.skip_coincident, s_stats.multipass_overlay,
				s_stats.skip_minw, s_stats.skip_min_vertex_w, s_stats.skip_offscreen_rt, s_stats.skip_shadow_pass,
				s_stats.warn_inaccurate_stq, s_stats.cam_world, s_stats.cam_fallback, s_stats.cam_held_gap, s_stats.cam_hold_expired, s_stats.cam_sky, s_stats.cam_viewmodel,
				s_stats.cam_failed,
				s_max_seen_position, max_position_magnitude(), s_last_bounds.radius(),
				s_stats.sky_tagged, s_stats.sky_hash, s_stats.cutout_tagged, s_stats.degenerate_triangles,
				s_stats.skip_all_degenerate, s_stats.meshes_created_peak,
				s_stats.meshes_destroyed_peak, s_stats.skip_inst_budget,
				(s_stats.distinct_instanced_frames > 0) ?
					(s_stats.distinct_instanced_total / s_stats.distinct_instanced_frames) : 0,
				s_stats.distinct_instanced_peak, s_pool_hashes.size(),
				stable_identity(), s_stats.id_reuse, s_stats.id_create, s_stats.id_rebuild,
				s_stats.id_probe_collisions,
				batch_mode(),
				(s_stats.batch_frames > 0) ? (s_stats.batch_groups_total / s_stats.batch_frames) : 0,
				s_stats.batch_groups_peak, s_stats.batch_surfaces_peak, s_stats.batch_vertices_peak,
				s_stats.batch_meshes_created, s_batch_reused, s_batch_mesh_cache.size());

			// The w distribution of everything submitted, which is what the min-w gate is set
			// from. A pile in the first buckets is geometry collapsing onto the eye plane.
			// 'explode' is the A-vs-B verdict for the reported vertex explosions, and it is on this
			// line rather than its own because it is only meaningful next to the w distribution the
			// ratio is measured against. A count of 0 with a peak well under the limit says no draw
			// this backend submitted was misplaced, whatever the screen looked like.
			INFO_LOG("Remix: submitted w (max per draw): <1e-3 {} <1e-2 {} <0.1 {} <1 {} <10 {} "
					 "<100 {} >=100 {} | minw gate {:g} | maxw gate {:g} skipped {} | "
					 "draw extent ratio avg {:.0f} peak {:.0f} | explode >{:g}x {} peak {:.1f}x",
				s_stats.w_histogram[0], s_stats.w_histogram[1], s_stats.w_histogram[2],
				s_stats.w_histogram[3], s_stats.w_histogram[4], s_stats.w_histogram[5],
				s_stats.w_histogram[6], min_submitted_w(), max_submitted_w(), s_stats.skip_maxw,
				(s_extent_ratio_frames > 0) ?
					(s_extent_ratio_total / static_cast<double>(s_extent_ratio_frames)) : 0.0,
				s_extent_ratio_peak,
				explode_ratio_limit(), s_stats.hyperextended, s_stats.hyperextended_peak);

			// The Z -> w calibration. This is the whole feasibility test for FST=1 recovery: if
			// neither model fits the draws that carry both Z and a real w, then Z cannot be
			// turned into depth for the draws that carry only Z, and the approach is dead
			// without writing any of it.
			{
				double a = 0.0, b = 0.0, r2 = 0.0;
				const bool ok_a = s_zfit.solve(true, a, b, r2);
				double a2 = 0.0, b2 = 0.0, r2b = 0.0;
				const bool ok_b = s_zfit.solve(false, a2, b2, r2b);

				double ua = 0.0, ub = 0.0;
				const bool live = fst_z_solution(ua, ub);

				INFO_LOG("Remix: Z->w fit over {:.0f} vertices | A (Q = a*zn+b): {} | B (w = a*zn+b): {} "
						 "| FST recovery {} (gate R2 >= {:g}), fst recovered {} skipped {} "
						 "| UNTEX recovery {}, untex recovered {} still-skipped {}",
					s_zfit.n,
					ok_a ? fmt::format("a={:.6g} b={:.6g} R2={:.5f}", a, b, r2) : std::string("insufficient"),
					ok_b ? fmt::format("a={:.6g} b={:.6g} R2={:.5f}", a2, b2, r2b) : std::string("insufficient"),
					(fst_z_mode() == 0) ? "OFF" : (live ? "ACTIVE" : "waiting/rejected"),
					fst_z_min_r2(), s_stats.fst_recovered,
					fmt::format("{} (flat-Z {})", s_stats.skip_fst, s_stats.fst_flat),
					(untex_z_mode() == 0) ? "OFF" : (live ? "ACTIVE" : "waiting/rejected"),
					s_stats.untex_recovered, s_stats.skip_untextured);
			}

			// Vertex colour. The whole vertex-colour-baking question is decided by whether these
			// vary: 128 is PS2 unity, so a title with no bake reports neutral ~100% and
			// draws const >> vary, whatever its means look like.
			INFO_LOG("Remix: vertex colour over {} verts | mean lum {:.1f} sd {:.1f} | neutral(128,128,128) {:.1f}% | "
					 "draws const {} vary {} | lum buckets {} {} {} {} {} {} {} {}",
				s_vcolor.vertices, s_vcolor.mean(), s_vcolor.stddev(),
				(s_vcolor.vertices > 0) ?
					(100.0 * static_cast<double>(s_vcolor.neutral) / static_cast<double>(s_vcolor.vertices)) : 0.0,
				s_vcolor.draws_constant, s_vcolor.draws_varying,
				s_vcolor.hist[0], s_vcolor.hist[1], s_vcolor.hist[2], s_vcolor.hist[3],
				s_vcolor.hist[4], s_vcolor.hist[5], s_vcolor.hist[6], s_vcolor.hist[7]);

			// Third line: the material bridge. Kept separate so the counter block stays
			// readable, and because the two numbers the user has to act on -- unique content
			// hashes and the per-draw hash cost -- both live here.
			INFO_LOG("{}", remix_ps2::materials::stats_line());

			// Second line: the world anchor. Every stage of the pipeline is separately
			// visible, so a null result names the stage that produced it -- no kicks, no
			// windows through the shape prefilter, no candidates, no split, or no score.
			INFO_LOG("Remix: vu kicks {} scanned {} reentrant {} windows {} shape-ok {} | "
					 "programs {}/{}{} | slice matrices {} published {} | cand {} (now {}) | "
					 "split-reject {} score-reject {} degenerate-reject {} scale-reject {} extent-reject {} accept {} (sliced {}) | camera {}{}",
				s_stats.vu_kicks, s_stats.vu_kicks_scanned, s_stats.vu_reentrant, s_stats.vu_windows,
				s_stats.vu_survivors,
				s_stats.vu_programs_used, s_stats.vu_programs_limit,
				// A non-zero refusal count means entry points exist that were never sliced, so
				// their matrices -- including any register-resident camera -- cannot reach the GS
				// side at all. The named PC is the one to raise PCSX2_REMIX_MAXPROGRAMS for.
				(s_stats.vu_programs_refused != 0)
					? fmt::format(" REFUSED {} (last pc=0x{:04x} ucode=0x{:016x}) -- entry points "
								  "never sliced; raise PCSX2_REMIX_MAXPROGRAMS",
						  s_stats.vu_programs_refused, s_stats.vu_refused_start_pc,
						  s_stats.vu_refused_ucode)
					: std::string(),
				s_stats.vu_sliced, s_stats.vu_sliced_published,
				s_stats.cam_candidates, s_stats.cam_last_candidates, s_stats.cam_reject_split,
				s_stats.cam_reject_score, s_stats.cam_reject_degenerate, s_stats.cam_reject_scale,
				s_stats.cam_reject_extent, s_stats.cam_accept, s_stats.cam_accept_sliced,
				s_active_camera.valid ? "world score " : "view-space",
				s_active_camera.valid ? fmt::format("{:.2f} near {:.5g}", s_active_camera.score, s_active_camera.near_plane) : "");

			// Per-draw camera placement, and the census that decides whether it can possibly help.
			// Its own line because it counts DRAWS and WINDOWS while the line above counts frames.
			//
			// READ 'solvers/window' FIRST. It is the whole verdict:
			//   peak 1  -> every window's draws shared ONE un-projection. Per-draw placement cannot
			//              change the picture no matter what the ring hashes say, and the shatter
			//              has another cause. Do not ship placement; re-diagnose.
			//   peak 2+ -> windows genuinely straddle cameras; per-draw placement is the fix, and
			//              'multi' says on how many windows.
			// 'rings/window' above 'solvers/window' means the title emits one camera at more than
			// one column scale (SOCOM CA does: an exact 10x on columns 0 and 1). That difference is
			// NOT normalised away here -- measured, see per_draw_solver's comment -- so on this
			// title the two counts should track each other.
			//
			// Of the draw counters: 'distinct' is the population per-draw placement acts on,
			// 'same-solver' is the population it would be a no-op for, 'match' is the draws whose
			// camera already IS the frame camera, and 'fallback' is ring misses plus refusals.
			// 'refused' climbing is the plan's named tell for 4A mis-accepting a non-camera ring
			// matrix -- watch it with maxpos and extent-reject, which read 0 today.
			INFO_LOG("Remix: per-draw camera placement {} | solvers/window last {} peak {} "
					 "(multi {} of {} windows{}) | rings/window last {} peak {} | "
					 "draws distinct {} same-solver {} match {} fallback {} | solves {} refused {}",
				(per_draw_camera_mode() != 0) ? "ON" : "OFF (frame-latched; census still measured)",
				s_stats.perdraw_solvers_last, s_stats.perdraw_solvers_peak,
				s_stats.perdraw_multi_windows, s_stats.perdraw_windows,
				(s_stats.perdraw_census_overflow != 0) ?
					fmt::format(", {} over census capacity", s_stats.perdraw_census_overflow) : std::string(),
				s_stats.perdraw_rings_last, s_stats.perdraw_rings_peak,
				s_stats.perdraw_distinct, s_stats.perdraw_same_solver, s_stats.perdraw_match,
				s_stats.perdraw_fallback, s_stats.perdraw_solved, s_stats.perdraw_refused);

			// Hold-previous-window. Its own line for the same reason as the one above: it counts
			// WINDOWS, and the number that matters is a ratio against the frame count on the first
			// line, not against anything else here.
			//
			// READ 'empty' AND 'offcadence' TOGETHER.
			//   empty ~= frames/3 -> the measured cadence is present and this is acting on it.
			//   held ~= empty     -> essentially every empty window was covered.
			//   offcadence 0      -> every hold landed exactly 3 windows after the previous one,
			//                        which is the only spacing this title's empty windows have.
			//   offcadence rising -> the detection is firing somewhere it was not designed to (a
			//                        load, a cinematic, a title that paces differently). It is the
			//                        plan's named tell and it is why the counter exists.
			// 'skip consec' is a genuine drought being correctly left alone rather than frozen.
			// A hold repeats a frame, so it can only ever restore a coherent scene -- it cannot
			// prove the shattering was the empty window. That identity is INFERRED; the user's eye
			// on a fresh clip is the test. See hold_empty_mode().
			// 'cam-held' is the mode-1-vs-2 reading, and under mode 2 it must track 'held'. Where it
			// lags, a held window rendered stale geometry from a moved eye -- which is the smear the
			// user reported on turns, and the reason mode 2 exists.
			const int hold_mode_now = hold_empty_mode();
			INFO_LOG("Remix: overlay {}x{} | screen-ui seen {} nomat {} nondc {} | "
					 "raster draws {} nopixels {} texels {} fullscreen {} | presents {} | "
					 "DrawScreenOverlay {} | uiraster {} uimode {}",
				s_overlay_w, s_overlay_h,
				s_screen_ui_seen, s_screen_ui_nomat, s_screen_ui_nondc,
				s_overlay_draws, s_overlay_nopixels, s_overlay_texels, s_overlay_fullscreen,
				s_overlay_presents,
				(s_remix.api().DrawScreenOverlay != nullptr) ? "available" : "NULL IN INTERFACE",
				ui_raster_mode(), ui_mode());

			INFO_LOG("Remix: hold-empty {} | empty windows {} held {} cam-held {} skipped-present {} "
					 "instances {} | "
					 "gap3 {} offcadence {} (last gap {}) | skip: consec {} nocam {} stale {} | "
					 "instance-fail {}",
				(hold_mode_now == 3) ? "MODE 3 (empty window NOT presented; native-rate, VRR)" :
				(hold_mode_now == 2) ? "MODE 2 (geometry + camera; true duplicate frame)" :
				(hold_mode_now == 1) ? "MODE 1 (geometry only; window's own newer camera)" :
									   "OFF (empty windows present empty; count only)",
				s_stats.hold_empty_windows, s_stats.hold_windows, s_stats.hold_cameras,
				s_stats.hold_skipped_presents, s_stats.hold_instances,
				s_stats.hold_gap3, s_stats.hold_offcadence, s_stats.hold_gap_last,
				s_stats.hold_skip_consecutive, s_stats.hold_skip_nocam, s_stats.hold_skip_stale,
				s_stats.hold_failed);

			// The split breakdown, printed only when something was rejected. One opaque count
			// could not tell "the matrix was singular" from "the recovered projection was not a
			// perspective", which are different bugs with different fixes.
			if (s_stats.cam_reject_split != 0)
			{
				std::string split_detail;
				for (u32 i = 1; i < static_cast<u32>(remix_ps2::split_stage::count); ++i)
				{
					if (s_stats.cam_split_stage[i] == 0)
						continue;

					fmt::format_to(std::back_inserter(split_detail), "{}{} {}",
						split_detail.empty() ? "" : " | ",
						remix_ps2::split_stage_name(static_cast<remix_ps2::split_stage>(i)),
						s_stats.cam_split_stage[i]);
				}

				INFO_LOG("Remix: split refusals by stage -- {}",
					split_detail.empty() ? std::string("(none recorded)") : split_detail);
			}
		}
	} // namespace

	bool g_armed = false;

	int SpikeMode()
	{
		static const int value = static_cast<int>(remix_ps2::read_env_int(L"PCSX2_REMIX_SPIKE", 0));
		return value;
	}

	void SetRendererIsRemix(bool enabled)
	{
		s_renderer_is_remix = enabled;
		g_armed = enabled || (SpikeMode() > 0);

		// Arm the VU1 matrix scan with the renderer, not with the spike: the spike has no
		// camera to feed, and an armed scan costs the EE/MTVU thread real work per XGKICK.
		RemixVU1Capture::SetArmed(enabled);
	}

	bool RendererIsRemix()
	{
		return s_renderer_is_remix;
	}

	void OnAcquireWindow(WindowInfo& wi)
	{
		if (!armed())
			return;

		if (wi.type == WindowInfo::Type::Win32 && wi.window_handle)
		{
			HWND incoming = static_cast<HWND>(wi.window_handle);

			// A different HWND after the runtime bound means PCSX2 recreated the render window
			// (fullscreen toggle is the usual cause). The old window is gone, so every later
			// Present would fault against it. Stop instead -- one clear line beats a fault storm.
			if (s_active_hwnd && incoming != s_active_hwnd && !s_window_lost)
			{
				s_window_lost = true;
				s_live = false;
				ERROR_LOG("Remix: the render window was recreated ({} -> {}); this is usually a "
						  "fullscreen toggle. The runtime cannot re-bind, so rendering has stopped -- "
						  "restart the emulator, and stay windowed in Remix mode.",
					static_cast<void*>(s_active_hwnd), static_cast<void*>(incoming));
			}

			s_hwnd = incoming;
			s_window_width = static_cast<int>(wi.surface_width);
			s_window_height = static_cast<int>(wi.surface_height);
			INFO_LOG("Remix: stashed render window {} ({}x{})", static_cast<void*>(s_hwnd), s_window_width, s_window_height);
		}
		else if (!s_hwnd)
		{
			WARNING_LOG("Remix: render window is not a Win32 surface, Remix cannot present");
			return;
		}

		// Sole-presenter model: the device must not create a swapchain on a window the Remix
		// runtime is about to present to. GSDevice11::Create skips CreateSwapChain for a
		// Surfaceless window and BeginPresent then returns FrameSkipped, which the existing
		// skip path in GSRenderer::VSync absorbs.
		const bool sole_presenter = s_renderer_is_remix || SpikeMode() >= 2;
		if (sole_presenter)
		{
			wi.type = WindowInfo::Type::Surfaceless;
			INFO_LOG("Remix: forcing a surfaceless device -- Remix is the sole presenter");
		}
	}

	namespace
	{
		// --- smooth normals -----------------------------------------------------------------
		//
		// The pass above gives every triangle a single face normal and writes it to all three of
		// its corners, so a curved surface shades as a set of flat plates and the tessellation is
		// visible -- on Rainbow Six 3 that reads as polygons on the squadmates' faces.
		//
		// Nothing in the developer menu can fix this. dxvk-remix has no smooth-normals category,
		// and even if it did, categories select behaviour for the normals we send; they cannot
		// invent normals we never sent. The PS2 has none to give us either: lighting runs on VU1
		// and is baked into the vertex colours, so the GS draw stream carries no normal at all.
		// They have to be generated here or not at all.
		//
		// PCSX2_REMIX_SMOOTHNORMALS is a crease angle in degrees. 0 (default) keeps today's flat
		// normals. A corner is smoothed only when its own face normal lies within that angle of
		// the averaged one, so a 60 degree setting rounds off a face while leaving a 90 degree
		// building corner sharp -- one threshold can serve a scene containing both.
		int smooth_normal_angle()
		{
			static live_int value(L"PCSX2_REMIX_SMOOTHNORMALS", 0, 0, 180);
			return value.get();
		}

		// --- MOONFIT: recover the game's own light direction from its baked lighting -------
		//
		// SOCOM ships NO light table -- verified against every archive in the game: the full
		// section-tag inventory (.SIZ ACTN AIMi ... SKB_ UI__ UNIV VAGS VALV WEAP ZANM ZRDR ZSND)
		// contains no LGHT/LITE/LAMP/SUN tag of any kind. But the lighting is not missing, it is
		// BAKED -- per-vertex, which is exactly what PCSX2_REMIX_VCOLOR reads. So the moon/sun the
		// artists lit each level with is still recoverable, from the shading itself.
		//
		// Model: luminance ~ a + b * dot(N, L). That is LINEAR in (a, b*Lx, b*Ly, b*Lz), so no
		// iteration is needed -- accumulate the 4x4 normal equations over the basis [1, Nx, Ny, Nz]
		// and solve once. The light direction falls out as normalize(c1, c2, c3) and `b` (the
		// vector length) is the directional contrast, which doubles as the confidence: a level lit
		// flatly returns a near-zero length and should not be trusted.
		//
		// L points TOWARD the light. Remix's fallbackLightDirection is the direction light TRAVELS,
		// so the logged vector is negated for direct paste into user.conf.
		double s_moonfit_m[4][4] = {};
		double s_moonfit_rhs[4] = {};
		u64 s_moonfit_verts = 0;
		int s_moonfit_windows = 0;

		// THE SIGN ANCHOR. The fit alone cannot tell a light above from a light below: our meshes
		// are doubleSided and smooth_scratch_normals() picks its sign from the flat normal, whose
		// winding this game never guaranteed. If every normal is consistently inverted the solved
		// direction is exactly negated -- elevation flips sign, azimuth rotates 180 -- and the fit
		// looks just as confident either way. So measure something that cannot be ambiguous:
		// compare mean baked luminance on up-facing vs down-facing surfaces. Outdoors the sky is
		// the dominant source, so up-facing MUST be brighter. If it is not, normals are inverted
		// and the recovered direction is negated before it is reported.
		double s_moonfit_up_lum = 0.0, s_moonfit_down_lum = 0.0;
		u64 s_moonfit_up_n = 0, s_moonfit_down_n = 0;

		int moonfit_window()
		{
			static live_int value(L"PCSX2_REMIX_MOONFIT", 900, 0, 100000);
			return value.get();
		}

		void solve_and_log_moon_fit()
		{
			// Below this the fit is noise; a single small mesh can bias a direction wildly.
			if (s_moonfit_verts < 20000)
				return;

			double a[4][5];
			for (int i = 0; i < 4; ++i)
			{
				for (int j = 0; j < 4; ++j)
					a[i][j] = s_moonfit_m[i][j];
				a[i][4] = s_moonfit_rhs[i];
			}

			// Gauss-Jordan with partial pivoting. Each column's pivot row is swapped into that
			// column's own row, so after elimination row i belongs to variable i.
			bool ok = true;
			for (int col = 0; col < 4 && ok; ++col)
			{
				int piv = col;
				for (int r = col + 1; r < 4; ++r)
				{
					if (std::abs(a[r][col]) > std::abs(a[piv][col]))
						piv = r;
				}

				if (std::abs(a[piv][col]) < 1e-9)
				{
					ok = false;
					break;
				}

				if (piv != col)
				{
					for (int j = 0; j <= 4; ++j)
						std::swap(a[col][j], a[piv][j]);
				}

				for (int r = 0; r < 4; ++r)
				{
					if (r == col)
						continue;

					const double f = a[r][col] / a[col][col];
					for (int j = col; j <= 4; ++j)
						a[r][j] -= f * a[col][j];
				}
			}

			if (ok)
			{
				const double c1 = a[1][4] / a[1][1];
				const double c2 = a[2][4] / a[2][2];
				const double c3 = a[3][4] / a[3][3];
				const double len = std::sqrt((c1 * c1) + (c2 * c2) + (c3 * c3));

				if (std::isfinite(len) && len > 1e-6)
				{
					double lx = c1 / len;
					double ly = c2 / len;
					double lz = c3 / len;

					const double up_mean = (s_moonfit_up_n > 0)
						? (s_moonfit_up_lum / static_cast<double>(s_moonfit_up_n)) : 0.0;
					const double down_mean = (s_moonfit_down_n > 0)
						? (s_moonfit_down_lum / static_cast<double>(s_moonfit_down_n)) : 0.0;

					// Only trust the anchor when both populations are actually present.
					const bool anchored = (s_moonfit_up_n > 500) && (s_moonfit_down_n > 500);
					const bool inverted = anchored && (up_mean < down_mean);
					if (inverted)
					{
						lx = -lx;
						ly = -ly;
						lz = -lz;
					}
					const double to_deg = 180.0 / 3.14159265358979323846;
					const double elev = std::asin(std::clamp(ly, -1.0, 1.0)) * to_deg;
					double azim = std::atan2(lx, lz) * to_deg;
					if (azim < 0.0)
						azim += 360.0;

					INFO_LOG("Remix: MOONFIT over {} verts -- the level's BAKED light comes FROM "
							 "elevation {:.1f} deg, azimuth {:.1f} deg, directional contrast {:.1f} "
							 "of 255 (under ~3 the level is lit flat and this is noise). "
							 "up-facing mean lum {:.1f} over {} verts vs down-facing {:.1f} over {} "
							 "-- anchor {}, normals {}. Paste into user.conf: "
							 "rtx.fallbackLightDirection = {:.4f}, {:.4f}, {:.4f} -- or set "
							 "PCSX2_REMIX_KEYELEV = {:.0f} and PCSX2_REMIX_KEYAZIM = {:.0f}.",
						s_moonfit_verts, elev, azim, len,
						up_mean, s_moonfit_up_n, down_mean, s_moonfit_down_n,
						anchored ? "usable" : "TOO FEW SAMPLES (direction sign unverified)",
						inverted ? "INVERTED (direction negated)" : "as-submitted",
						-lx, -ly, -lz, elev, azim);
				}
			}

			s_moonfit_verts = 0;
			s_moonfit_windows = 0;
			s_moonfit_up_lum = 0.0;
			s_moonfit_down_lum = 0.0;
			s_moonfit_up_n = 0;
			s_moonfit_down_n = 0;
			for (int i = 0; i < 4; ++i)
			{
				s_moonfit_rhs[i] = 0.0;
				for (int j = 0; j < 4; ++j)
					s_moonfit_m[i][j] = 0.0;
			}
		}

		void accumulate_moon_fit()
		{
			const int window = moonfit_window();
			if (window == 0)
				return;

			for (const remixapi_HardcodedVertex& v : s_scratch_vertices)
			{
				const double nx = v.normal[0];
				const double ny = v.normal[1];
				const double nz = v.normal[2];
				const double nlen2 = (nx * nx) + (ny * ny) + (nz * nz);

				// Only unit normals carry a meaningful dot product. Degenerate and
				// placeholder normals would otherwise drag the fit toward their own axis.
				if (!std::isfinite(nlen2) || nlen2 < 0.9 || nlen2 > 1.1)
					continue;

				// Channel order is deliberately not assumed -- a plain mean over the three
				// low bytes is order-independent, and only the DIRECTION of the fit matters.
				const u32 c = v.color;
				const double lum = (static_cast<double>(c & 0xFF) +
									static_cast<double>((c >> 8) & 0xFF) +
									static_cast<double>((c >> 16) & 0xFF)) / 3.0;

				if (ny > 0.5)
				{
					s_moonfit_up_lum += lum;
					++s_moonfit_up_n;
				}
				else if (ny < -0.5)
				{
					s_moonfit_down_lum += lum;
					++s_moonfit_down_n;
				}

				const double basis[4] = {1.0, nx, ny, nz};
				for (int i = 0; i < 4; ++i)
				{
					for (int j = 0; j < 4; ++j)
						s_moonfit_m[i][j] += basis[i] * basis[j];

					s_moonfit_rhs[i] += basis[i] * lum;
				}

				++s_moonfit_verts;
			}

			if (++s_moonfit_windows >= window)
				solve_and_log_moon_fit();
		}

		void smooth_scratch_normals(float scene_scale)
		{
			const int degrees = smooth_normal_angle();
			if (degrees <= 0)
				return;

			// Vertices are welded by position, because a PS2 draw is overwhelmingly an unindexed
			// triangle list: the two corners that ought to share a normal are usually two separate
			// entries holding the same coordinates, so averaging by index alone would do nothing.
			//
			// The tolerance is relative to the scene for the same reason the degenerate-area
			// threshold above is -- maxpos is ~2,500 on Rainbow Six 3 and ~5,300 on SOCOM, so a
			// fixed epsilon is either useless or merges the model.
			const float step = std::max(scene_scale * 1e-5f, 1e-6f);
			const float inv_step = 1.f / step;

			static std::unordered_map<u64, u32> weld;
			static std::vector<float> sums; // 3 floats per welded slot
			static std::vector<u32> slot_of;

			weld.clear();
			sums.clear();
			slot_of.assign(s_scratch_vertices.size(), 0u);

			const auto slot_for = [&](u32 index) -> u32 {
				const remixapi_HardcodedVertex& v = s_scratch_vertices[index];

				u64 key = 1469598103934665603ull;
				for (u32 axis = 0; axis < 3; ++axis)
				{
					const s64 q = static_cast<s64>(
						std::llround(static_cast<double>(v.position[axis]) * static_cast<double>(inv_step)));
					key = (key ^ static_cast<u64>(q)) * 1099511628211ull;
				}

				const auto found = weld.find(key);
				if (found != weld.end())
					return found->second;

				const u32 slot = static_cast<u32>(sums.size() / 3);
				sums.insert(sums.end(), {0.f, 0.f, 0.f});
				weld.emplace(key, slot);
				return slot;
			};

			// Accumulate the un-normalised cross product, which is twice the triangle area -- so
			// large triangles weight the average more than slivers, without a separate area term.
			for (size_t i = 0; (i + 2) < s_scratch_indices.size(); i += 3)
			{
				const u32 i0 = s_scratch_indices[i];
				const u32 i1 = s_scratch_indices[i + 1];
				const u32 i2 = s_scratch_indices[i + 2];

				const remixapi_HardcodedVertex& v0 = s_scratch_vertices[i0];
				const remixapi_HardcodedVertex& v1 = s_scratch_vertices[i1];
				const remixapi_HardcodedVertex& v2 = s_scratch_vertices[i2];

				const float e1[3] = {v1.position[0] - v0.position[0], v1.position[1] - v0.position[1],
					v1.position[2] - v0.position[2]};
				const float e2[3] = {v2.position[0] - v0.position[0], v2.position[1] - v0.position[1],
					v2.position[2] - v0.position[2]};

				const float n[3] = {
					(e1[1] * e2[2]) - (e1[2] * e2[1]),
					(e1[2] * e2[0]) - (e1[0] * e2[2]),
					(e1[0] * e2[1]) - (e1[1] * e2[0])};

				if (!std::isfinite(n[0]) || !std::isfinite(n[1]) || !std::isfinite(n[2]))
					continue;

				for (const u32 index : {i0, i1, i2})
				{
					const u32 slot = slot_for(index);
					slot_of[index] = slot;
					sums[(slot * 3) + 0] += n[0];
					sums[(slot * 3) + 1] += n[1];
					sums[(slot * 3) + 2] += n[2];
				}
			}

			const float cos_limit = std::cos(static_cast<float>(degrees) * 3.14159265358979f / 180.f);

			for (size_t i = 0; (i + 2) < s_scratch_indices.size(); i += 3)
			{
				for (u32 k = 0; k < 3; ++k)
				{
					const u32 index = s_scratch_indices[i + k];
					remixapi_HardcodedVertex& target = s_scratch_vertices[index];
					const u32 slot = slot_of[index];

					const float s[3] = {sums[(slot * 3) + 0], sums[(slot * 3) + 1], sums[(slot * 3) + 2]};
					const float len = std::sqrt((s[0] * s[0]) + (s[1] * s[1]) + (s[2] * s[2]));

					// Opposing faces can cancel to nothing. Keep the flat normal rather than
					// normalising a zero vector into NaNs and handing them to the BVH build.
					if (!std::isfinite(len) || len <= 0.f)
						continue;

					const float smoothed[3] = {s[0] / len, s[1] / len, s[2] / len};

					// target.normal still holds this vertex's flat normal from the pass above.
					const float dot = (smoothed[0] * target.normal[0]) + (smoothed[1] * target.normal[1]) +
					                  (smoothed[2] * target.normal[2]);

					// Past the crease angle the corner belongs to an edge, not a curve. doubleSided
					// is set on our meshes, so a normal that ended up on the far side of the surface
					// is a sign flip rather than a wrong direction -- compare on magnitude.
					if (std::abs(dot) < cos_limit)
						continue;

					const float sign = (dot < 0.f) ? -1.f : 1.f;
					target.normal[0] = smoothed[0] * sign;
					target.normal[1] = smoothed[1] * sign;
					target.normal[2] = smoothed[2] * sign;
				}
			}
		}
	} // namespace

	void OnDrawPrims(const GSRendererHW& r, int rt_unscaled_width, int rt_unscaled_height, const void* tex_source)
	{
		if (!armed())
			return;

		ensure_initialized();

		if (!s_live)
			return;

		++s_stats.draws_seen;

		if (s_frame_counter < submit_delay_frames())
		{
			++s_stats.skip_submit_delay;
			return;
		}

		// --- classification gates -----------------------------------------------------------
		// Order matters only for the counters: the first gate that refuses is the one blamed.

		// PS2 text is drawn as SPRITES, not triangles, and this gate is the first one in the
		// function -- so every glyph in the game was discarded ~900 lines before the screen-UI
		// classifier could see it. That is why menu panels, icons and the button glyphs
		// composited correctly while no text ever appeared: those are triangle-class, the text
		// is not. Sprites are still never world geometry, so they are admitted ONLY as overlay
		// candidates and are refused again below if the raster path does not consume them.
		const bool sprite_class = (r.m_vt.m_primclass == GS_SPRITE_CLASS);
		const bool sprite_ui_probe = sprite_class && r.m_process_texture &&
			ui_mode() != 0 && ui_raster_mode() != 0;

		if (r.m_vt.m_primclass != GS_TRIANGLE_CLASS && !sprite_ui_probe)
		{
			++s_stats.skip_not_triangle;
			return;
		}

		// Q only means "the perspective divisor" when the draw is textured with ST/Q. With
		// TME=0 nothing ever wrote a meaningful Q, and with FST=1 the guest fed UVs instead.
		//
		// Neither is fatal any more: both classes can take their depth from Z instead, on the
		// calibration fitted continuously from the textured FST=0 draws. What they cannot do is
		// take it from Q, so they share one flag from here on.
		double fst_z_a = 0.0;
		double fst_z_b = 0.0;
		const bool untex_draw = !r.m_process_texture;
		const bool fst_draw = !untex_draw && (r.PRIM->FST != 0);
		const bool z_depth = untex_draw || fst_draw;
		// TWO DIFFERENT QUESTIONS, and widening one of them broke the other.
		//
		// fallback_screen_ui answers "there is no camera, so treat this flat draw as a screen
		// overlay": it forces w = 1 and the VIEW-SPACE tier. That must stay tied to
		// !s_active_camera.valid. When UIMODE widened it, every distant flat quad -- a moon
		// sprite, a billboard -- was pushed through the view-space tier at w = 1, i.e. placed at
		// the eye, and vanished. That is why the moon disappeared.
		//
		// ui_candidate answers only "is this draw eligible for the 2D overlay rasteriser". It may
		// be wide, because the raster path uses NDC and UVs and never touches w, and a draw it
		// consumes returns before geometry submission anyway.
		const bool flat_2d = (z_depth ? (r.m_vt.m_eq.z && (!fst_draw || !remix_ps2::nocam_enabled()))
									  : r.m_vt.m_eq.q);
		const bool fallback_screen_ui = !untex_draw && !s_active_camera.valid && flat_2d;
		const bool ui_candidate = !untex_draw && ui_mode() != 0 && flat_2d;

		// Counted at the point of CLASSIFICATION, so "the HUD is not being recognised as 2D" and
		// "it is recognised but the rasteriser rejects it" stop looking identical in the log.
		if (fallback_screen_ui)
			++s_screen_ui_seen;

		// Untextured: no texture means no Q was ever written, and also no material -- these come
		// back from materials::bind() with the null binding and shade like Rainbow Six 3's white
		// geometry. Untextured-and-placed beats absent, which is what dropping them amounted to.
		if (untex_draw && !fallback_screen_ui && (untex_z_mode() == 0 || !fst_z_solution(fst_z_a, fst_z_b)))
		{
			++s_stats.skip_untextured;
			return;
		}

		// FST=1: the guest fed direct UV texels, so Q is not the perspective divisor. Recoverable
		// only if this title's Z has been shown to be a usable depth (see fst_z_solution), which
		// is measured continuously from the FST=0 draws rather than assumed.
		if (fst_draw && !fallback_screen_ui && !ui_candidate && !fst_z_solution(fst_z_a, fst_z_b))
		{
			++s_stats.skip_fst;
			return;
		}

		// One Q across the whole draw means no perspective, i.e. 2D even when textured.
		//
		// For an FST draw Q is meaningless and almost always constant, so this test would reject
		// every one of them. The analogue that still discriminates is Z: a HUD element is
		// screen-aligned and has constant Z, whereas world geometry under a perspective
		// projection does not. It is the same trade this gate already makes -- flat geometry
		// viewed exactly head-on is lost -- applied to the only varying quantity FST draws have.
		// Applies to every Z-depth draw, textured or not: for an untextured draw Q is not merely
		// the wrong divisor, it was never written, so testing it would pass everything through.
		if (!fallback_screen_ui && !ui_candidate && (z_depth ? (r.m_vt.m_eq.z && fst_flat_mode() == 0) : r.m_vt.m_eq.q))
		{
			// Counted separately: "how many FST draws have no depth variation at all" is the
			// number that decides whether depth-from-Z can recover geometry for this title or
			// only ever produces camera-facing flats. Untextured flats go to the const-depth
			// bucket, which is the same finding this gate always reported.
			if (fst_draw)
				++s_stats.fst_flat;
			else
				++s_stats.skip_const_q;

			return;
		}

		// A partial *colour* write mask means the draw is one channel of a multi-pass
		// modulation, not a surface. The GS has a single texture unit, so a PS2 title
		// multitextures by drawing the same geometry again with FBMSK set to leave two of the
		// three colour channels alone. Measured in Rainbow Six 3: of 231 logged draws, 90 carry
		// a colour mask, split exactly 30/30/30 across 0xff00ffff / 0xffff00ff / 0xffffff00 --
		// every masked draw arrives as a set of three, one per channel, over geometry that was
		// already drawn unmasked.
		//
		// Submitting them was producing four coincident surfaces per wall: z-fighting in the
		// path tracer, which is what flickers, and a single colour channel of a modulation term
		// is not a valid albedo in any case. Alpha-only masks (0xFF000000) are ordinary and are
		// deliberately not caught here.
		//
		// The texture bound in one of these passes is the title's lightmap. Folding it into the
		// base material is the next phase; the per-draw mask test is what will identify it
		// there too, so nothing here needs a per-title texture-page list.
		// Set by the masked-pass gate below when injection is on, and consumed at instance build to
		// mark the draw a static decal. Declared here so the gate can fall through instead of
		// returning.
		bool lightmap_pass = false;
		float lightmap_channel[3] = {0.f, 0.f, 0.f};

		// LMPAIR. Whether a masked lightmap pass re-draws the SAME vertices as the unmasked base
		// pass it modulates decides how the lightmap can be folded in. Same positions -> the
		// lightmap's own UVs can be sampled per vertex and multiplied into the base surface's
		// vertex colour, needing no extra geometry and so no coincident surfaces. Different
		// positions -> it has to be separate geometry, which is what flickered before.
		u64 lm_pos_hash = 0;
		{
			const u32 lm_vn = r.m_vertex->next;
			const GSVertex* const lm_v = r.m_vertex->buff;
			u64 h = fnv_seed;
			for (u32 i = 0; i < lm_vn; ++i)
			{
				// Vertex COUNT and the index stream, not screen positions: XYZ here is post-transform,
				// so a key built from it changes the instant the camera moves and the fold only ever
				// hit while standing still. With LIGHTMAPEMISSIVE on, a miss means no lightmap
				// darkening at all, so walls emitted at full albedo whenever the player moved.
				h = fnv_mix(h, static_cast<u32>(lm_v[i].XYZ.X));
				h = fnv_mix(h, static_cast<u32>(lm_v[i].XYZ.Y));
				h = fnv_mix(h, lm_v[i].XYZ.Z);
			}
			lm_pos_hash = h;
			const bool lm_masked = (r.m_cached_ctx.FRAME.FBMSK & 0x00FFFFFFu) != 0;
			static u64 prev_hash = 0;
			static u32 prev_vn = 0;
			static u64 lm_match = 0, lm_miss = 0, lm_n = 0;
			if (lm_masked)
			{
				const bool same = (lm_vn == prev_vn) && (h == prev_hash);
				if (same) ++lm_match; else ++lm_miss;
				if ((lm_n++ % 4000) == 0)
				{
					INFO_LOG("Remix: LMPAIR masked draws {} | same-vertices-as-previous {} ({:.1f}%) "
							 "different {} | this masked verts {} prev unmasked verts {} | mask 0x{:08x}",
						lm_match + lm_miss, lm_match,
						(100.0 * static_cast<double>(lm_match)) / static_cast<double>(std::max<u64>(1, lm_match + lm_miss)),
						lm_miss, lm_vn, prev_vn, static_cast<u32>(r.m_cached_ctx.FRAME.FBMSK));
				}
			}
			else
			{
				prev_hash = h;
				prev_vn = lm_vn;
			}
		}

		if ((r.m_cached_ctx.FRAME.FBMSK & 0x00FFFFFFu) != 0)
		{
			++s_stats.skip_fbmsk;

			// Record the lightmap this pass carries. The source is available here -- it is bind()
			// that is skipped, not the source -- so the hash can be computed without uploading
			// anything. See lightmap_discovery_mode().
			if (lightmap_discovery_mode() != 0)
			{
				const GSTextureCache::Source* const masked_source =
					static_cast<const GSTextureCache::Source*>(tex_source);
				const u64 lm_hash = remix_ps2::materials::hash_only(masked_source);

				if (lm_hash != 0)
				{
					auto [it, inserted] = s_lightmaps.try_emplace(lm_hash);
					lightmap_entry& e = it->second;
					if (inserted)
					{
						const GIFRegTEX0& lt = r.m_cached_ctx.TEX0;
						e.hash = lm_hash;
						e.tbp0 = static_cast<u32>(lt.TBP0);
						e.cbp = static_cast<u32>(lt.CBP);
						e.psm = static_cast<u32>(lt.PSM);
						e.width = 1u << static_cast<u32>(lt.TW);
						e.height = 1u << static_cast<u32>(lt.TH);
						e.mask = static_cast<u32>(r.m_cached_ctx.FRAME.FBMSK);
						s_lightmaps_dirty = true;
					}
					++e.draws;
				}
			}

			// Sample this channel of the lightmap at each vertex and stash it against the base
			// pass's vertex identity, for the base draw to pick up. One frame of latency, which is
			// exact for a baked lightmap: it does not change between frames.
			if (lightmap_fold_mode() != 0 && lm_pos_hash != 0)
			{
				const GSTextureCache::Source* const lm_src =
					static_cast<const GSTextureCache::Source*>(tex_source);
				const u64 lm_key = remix_ps2::materials::hash_only(lm_src);
				float chan[3] = {0.f, 0.f, 0.f};
				fbmsk_channel(static_cast<u32>(r.m_cached_ctx.FRAME.FBMSK), chan);
				const u32 ci = (chan[0] > 0.f) ? 0u : ((chan[1] > 0.f) ? 1u : 2u);
				const bool one_channel = (chan[0] + chan[1] + chan[2]) == 1.f;

				if (lm_key != 0 && one_channel)
				{
					auto img_it = s_lm_images.find(lm_key);
					if (img_it == s_lm_images.end())
					{
						lm_image made{};
						if (remix_ps2::materials::decode_source(lm_src, made.px, made.w, made.h) &&
							made.w > 0 && made.h > 0)
						{
							{
								double mb = 0, mg = 0, mr = 0, ma = 0;
								const size_t n = made.px.size() / 4;
								for (size_t q = 0; q < n; ++q)
								{
									mb += made.px[(q * 4) + 0]; mg += made.px[(q * 4) + 1];
									mr += made.px[(q * 4) + 2]; ma += made.px[(q * 4) + 3];
								}
								const double inv = (n > 0) ? (1.0 / static_cast<double>(n)) : 0.0;
								INFO_LOG("Remix: LMDECODE {:016X} {}x{} mask 0x{:08x} channel {} | mean "
										 "B {:.1f} G {:.1f} R {:.1f} A {:.1f}",
									lm_key, made.w, made.h, static_cast<u32>(r.m_cached_ctx.FRAME.FBMSK),
									ci, mb * inv, mg * inv, mr * inv, ma * inv);
							}
							img_it = s_lm_images.emplace(lm_key, std::move(made)).first;
							++s_lm_images_made;
						}
						else
							++s_lm_decode_fail;
					}

					if (img_it != s_lm_images.end())
					{
						const lm_image& img = img_it->second;
						const u32 vn = r.m_vertex->next;
						const GSVertex* const vv = r.m_vertex->buff;
						const bool fst = r.PRIM->FST != 0;
						const float inv_tw = 1.f / static_cast<float>(1u << r.m_cached_ctx.TEX0.TW);
						const float inv_th = 1.f / static_cast<float>(1u << r.m_cached_ctx.TEX0.TH);

						// Modulate the base draw's vertices directly, in this frame's batch buffer.
						const bool inframe = batch_mode() != 0 && s_lm_span_valid &&
							s_lm_span_count == vn && s_lm_span_group < s_batch_groups.size() &&
							s_lm_span_surface < s_batch_groups[s_lm_span_group].surfaces.size();
						std::vector<remixapi_HardcodedVertex>* target = nullptr;
						if (inframe)
						{
							auto& tv = s_batch_groups[s_lm_span_group].surfaces[s_lm_span_surface].vertices;
							if ((static_cast<size_t>(s_lm_span_first) + vn) <= tv.size())
								target = &tv;
						}

						lm_verts& lv = s_lm_verts[lm_pos_hash];
						if (lv.n != vn)
						{
							lv.n = vn;
							lv.rgb.assign(static_cast<size_t>(vn) * 3, 0.f);
							lv.seen = 0;
						}
						for (u32 i = 0; i < vn; ++i)
						{
							float u, v;
							if (fst)
							{
								u = (static_cast<float>(vv[i].U) * (1.f / 16.f)) * inv_tw;
								v = (static_cast<float>(vv[i].V) * (1.f / 16.f)) * inv_th;
							}
							else
							{
								const float q = vv[i].RGBAQ.Q;
								if (!std::isfinite(q) || q == 0.f)
									continue;
								u = vv[i].ST.S / q;
								v = vv[i].ST.T / q;
							}
							const int tx = std::clamp(static_cast<int>(u * static_cast<float>(img.w)), 0, static_cast<int>(img.w) - 1);
							const int ty = std::clamp(static_cast<int>(v * static_cast<float>(img.h)), 0, static_cast<int>(img.h) - 1);
							const size_t off = ((static_cast<size_t>(ty) * img.w) + static_cast<size_t>(tx)) * 4;
							if ((off + 3) >= img.px.size())
								continue;
							// ALPHA, not colour. The PS2 blend equation (A-B)*C+D takes C from an alpha
							// source, which is the entire reason this title moves each lightmap channel
							// into alpha via its own CLUT. Sampling the colour components instead gave a
							// heavy orange cast, since they carry the palette entry rather than the value.
							lv.rgb[(static_cast<size_t>(i) * 3) + ci] =
								static_cast<float>(img.px[off + 3]) * (1.f / 255.f);
							if (target != nullptr)
							{
								remixapi_HardcodedVertex& tvx = (*target)[s_lm_span_first + i];
								const float lmv = lightmap_fold_floor() +
									((1.f - lightmap_fold_floor()) * (static_cast<float>(img.px[off + 3]) * (1.f / 255.f)));
								const u32 shift = (ci == 0) ? 16u : ((ci == 1) ? 8u : 0u);
								const u32 chan = (tvx.color >> shift) & 0xFFu;
								const u32 lit = static_cast<u32>(std::clamp(
									static_cast<float>(chan) * lmv * lightmap_fold_scale(), 0.f, 255.f));
								tvx.color = (tvx.color & ~(0xFFu << shift)) | (lit << shift);
								++s_lm_inframe;
							}
						}
						lv.seen |= static_cast<u8>(1u << ci);
						lv.frame = s_frame_counter;
						++s_lm_stored;
					}
				}
			}

			// Submit it instead of rejecting, as a static decal. See lightmap_inject_mode().
			if (lightmap_inject_mode() != 0)
			{
				lightmap_pass = true;
				fbmsk_channel(static_cast<u32>(r.m_cached_ctx.FRAME.FBMSK), lightmap_channel);
			}

			// PCSX2_REMIX_FBMSKDUMP=N: dump the first N masked draws to logs\remix_fbmsk.txt.
			//
			// These never reach materials::bind(), so the lightmap they carry is never created as a
			// Remix material and cannot even be tagged emissive in the developer menu. Identifying
			// the texture is therefore the prerequisite for folding it in, and TEX0's page fields
			// are enough for that -- the measured pattern is one texture page across all of them.
			if (s_fbmsk_dumped < fbmsk_dump_limit())
			{
				++s_fbmsk_dumped;

				const GIFRegTEX0& t0 = r.m_cached_ctx.TEX0;
				const GIFRegALPHA& al = r.m_context->ALPHA;

				// Why hash_only() refused, if it did. A masked pass whose texture is a render TARGET
				// has no stable content identity and can never be tagged -- that would mean the
				// lightmap is generated into the framebuffer rather than being an asset.
				const GSTextureCache::Source* const dbg_src =
					static_cast<const GSTextureCache::Source*>(tex_source);
				const char* src_state = !dbg_src ? "null" :
					(dbg_src->m_target ? "target" : (dbg_src->m_from_target ? "from_target" : "ok"));
				const u64 dbg_hash = remix_ps2::materials::hash_only(dbg_src);

				// CLUT and ALPHA are the fields that decide the whole design. All three passes bind
				// the SAME texture page, and three identical samples masked to different channels
				// would only ever write greyscale -- so something must differ per pass. The PS2
				// blend equation is (A-B)*C+D where C is an ALPHA source, so it cannot multiply by
				// per-channel colour in one pass; the way to modulate RGB by a colour texture is to
				// move one channel into alpha via the CLUT and mask the other two off, three times.
				// If CBP/CSA vary across the triple, that is confirmed and the three passes
				// reconstruct a full-colour lightmap.
				fbmsk_dump_write(fmt::format(
					"f={} mask=0x{:08x} verts={} tris={} | tex tbp0=0x{:x} tbw={} psm=0x{:02x} "
					"tw={} th={} tcc={} tfx={} | clut cbp=0x{:x} cpsm=0x{:02x} csm={} csa={} cld={} | "
					"alpha A={} B={} C={} D={} FIX={} | ate={} abe={} depth(r={} w={}) | "
					"px=[{:.0f},{:.0f}]x[{:.0f},{:.0f}] rt={}x{} | src={} hash=0x{:016x}",
					s_frame_counter, static_cast<u32>(r.m_cached_ctx.FRAME.FBMSK),
					r.m_vertex->next, r.m_index->tail / 3,
					static_cast<u32>(t0.TBP0), static_cast<u32>(t0.TBW), static_cast<u32>(t0.PSM),
					static_cast<u32>(t0.TW), static_cast<u32>(t0.TH), static_cast<u32>(t0.TCC),
					static_cast<u32>(t0.TFX),
					static_cast<u32>(t0.CBP), static_cast<u32>(t0.CPSM), static_cast<u32>(t0.CSM),
					static_cast<u32>(t0.CSA), static_cast<u32>(t0.CLD),
					static_cast<u32>(al.A), static_cast<u32>(al.B), static_cast<u32>(al.C),
					static_cast<u32>(al.D), static_cast<u32>(al.FIX),
					static_cast<u32>(r.m_cached_ctx.TEST.ATE), static_cast<u32>(r.PRIM->ABE),
					r.m_cached_ctx.DepthRead() ? 1 : 0, r.m_cached_ctx.DepthWrite() ? 1 : 0,
					r.m_vt.m_min.p.x, r.m_vt.m_max.p.x, r.m_vt.m_min.p.y, r.m_vt.m_max.p.y,
					rt_unscaled_width, rt_unscaled_height, src_state, dbg_hash));
			}

			// Fall through only when injecting; otherwise this draw is done.
			if (!lightmap_pass)
				return;
		}

		if (r.m_vt.m_accurate_stq)
			++s_stats.warn_inaccurate_stq;

		if (rt_unscaled_width <= 0 || rt_unscaled_height <= 0)
		{
			++s_stats.skip_no_target;
			return;
		}

		// Draws that render into a small OFF-SCREEN target are not world geometry.
		//
		// MEASURED on SOCOM Winterblade (slot 1): the frame's main target is 640x448 and carries
		// 1,859 of 1,867 dumped draws. The other 8 go to a 128x128 target, and every single one of
		// the 200 draws caught by PCSX2_REMIX_FARDUMP=25000 was one of them -- unanimous on six
		// independent fields: rt=128x128, sky=1, fbmsk=0x00ffffff (alpha only), the untextured
		// material, ZTST=1 (depth test always), tex psm=0x00.
		//
		// They are a render-to-texture pass: the game is baking an alpha mask into a small texture
		// it will sample later. Un-projecting them with the *main* camera is meaningless, and the
		// result is spectacular -- a draw covering 62x75 pixels comes out with a world extent of
		// 115,012 units and a w range spanning 0.0156 to 72,812 within one draw. Two or three of
		// those per frame put gigantic geometry through the path tracer.
		//
		// The threshold is an ABSOLUTE pixel AREA, and the first version of this gate got that
		// wrong in a way worth recording.
		//
		// It originally compared each target's smaller side against a percentage of the largest
		// side seen so far. That is fragile by construction: the reference only ever grows, so a
		// single larger target anywhere in the session moves it permanently. At MINRT=50 a 1024-wide
		// target makes the cutoff 512, which is above the main framebuffer's 448 height -- so the
		// ENTIRE 640x448 world gets culled from that point on, and the level appears to lose its
		// geometry. Reported as "geometry exploding/disappearing again".
		//
		// An absolute area has none of that ordering dependence: 128x128 = 16,384 is rejected at any
		// threshold that keeps 640x448 = 286,720, and a title with an unusual framebuffer (512x224 =
		// 114,688) still clears a 65,536 setting comfortably. 0 disables the gate.
		// RTSIZE census: what render-target shapes this title actually draws into, so MINRT can be
		// set from measurement. A shadow pass rendered from a light's viewpoint is a small target;
		// the main framebuffer is the large one that must survive.
		{
			static std::map<u32, u64> census;
			static u32 rt_n = 0;
			const u32 key = (static_cast<u32>(rt_unscaled_width) << 16) | static_cast<u32>(rt_unscaled_height & 0xFFFF);
			++census[key];
			if ((rt_n++ % 20000) == 0 && !census.empty())
			{
				std::string line;
				for (const auto& [k, n] : census)
					line += fmt::format(" {}x{}={} (area {})", k >> 16, k & 0xFFFF, n, (k >> 16) * (k & 0xFFFF));
				INFO_LOG("Remix: RTSIZE census --{}", line);
			}
		}

		if (shadow_pass_mode() != 0 && r.m_cached_ctx.TEST.ZTE != 0 &&
			r.m_cached_ctx.TEST.ZTST == 1 && r.m_cached_ctx.ZBUF.ZMSK != 0)
		{
			++s_stats.skip_shadow_pass;
			return;
		}

		if (const int min_rt_area = offscreen_rt_min_area(); min_rt_area > 0)
		{
			if ((rt_unscaled_width * rt_unscaled_height) < min_rt_area)
			{
				++s_stats.skip_offscreen_rt;
				return;
			}
		}

		const u32 vertex_count = r.m_vertex->next;
		const u32 index_count = r.m_index->tail;

		if (vertex_count < 3 || index_count < 3)
		{
			++s_stats.skip_empty;
			return;
		}

		if (vertex_count > s_max_vertices_per_mesh || index_count > s_max_indices_per_mesh)
		{
			++s_stats.skip_too_large;
			return;
		}

		// --- un-projection ------------------------------------------------------------------
		// The exact inverse of what PCSX2 itself applies: the native-alignment branch of
		// DetermineVSConfig (GSRendererHW.cpp:6176-6209) composed with tfx.fx:1654-1669.
		// Raw XY are the u16 GSVertex.XYZ.X/Y in 12.4 fixed point; the -0.05 is the shader's
		// own rasterisation rounding fudge and is reproduced so the two agree exactly.
		const float ox = static_cast<float>(static_cast<int>(r.m_context->XYOFFSET.OFX));
		const float oy = static_cast<float>(static_cast<int>(r.m_context->XYOFFSET.OFY));
		const float sx = 2.0f / static_cast<float>(rt_unscaled_width << 4);
		const float sy = 2.0f / static_cast<float>(rt_unscaled_height << 4);
		const float ox2 = -1.0f / static_cast<float>(rt_unscaled_width);
		const float oy2 = -1.0f / static_cast<float>(rt_unscaled_height);
		const float offset_x = (ox * sx) + ox2 + 1.0f;
		const float offset_y = (oy * sy) + oy2 + 1.0f;

		// Hand these constants to the world tier: the screen-clip normaliser has to invert
		// exactly the map used here, and the biggest 3D draw is the best proxy for "the
		// frame's main 3D target". Ties keep the first, which is stable frame to frame.
		if (vertex_count > s_frame_viewport.weight)
		{
			s_frame_viewport.valid = true;
			s_frame_viewport.ofx = ox;
			s_frame_viewport.ofy = oy;
			s_frame_viewport.width = rt_unscaled_width;
			s_frame_viewport.height = rt_unscaled_height;
			s_frame_viewport.weight = vertex_count;
		}

		// The synthetic projection the recovered geometry is expressed against. Only its two
		// scale terms are needed: a point at eye depth w projects to ndc = (vx*a/w, vy*b/w).
		const remix_ps2::mat4 projection = remix_ps2::make_perspective(
			s_debug_fov_y_degrees, window_aspect(), 0.1f, remix_ps2::hardcoded_far_plane());
		const float inv_a = 1.0f / projection.m[0][0];
		const float inv_b = 1.0f / projection.m[1][1];

		// World tier: positions are solved against the frame's fused matrix instead of being
		// expressed in the synthetic camera's space. Both paths consume the same NDC, so the
		// only difference is the three lines that turn (ndc, w) into a position.
		// A screen-UI draw must NOT be un-projected: its w is synthetic (1), so the world tier
		// would place it at the eye. The view-space tier plus a camera-relative instance
		// transform is what makes it an overlay.
		const bool world_mode = s_active_camera.valid && !fallback_screen_ui;
		const float position_limit = max_position_magnitude();

		// --- which camera un-projects THIS draw (step 4A) ------------------------------------
		//
		// The ring is walked at most once here (memoised per kick sequence) and the result serves
		// both consumers below: the per-draw camera census and election, and the FRAMETRACE sampler
		// further down, which used to do its own lookup -- two ring walks per draw on exactly the
		// run the trace is measuring.
		//
		// Only in world mode: with no frame camera the vertex loop takes the view-space branch and
		// no solver is read at all, so a lookup there would be pure cost.
		//
		// The CENSUS runs whatever PCSX2_REMIX_PERDRAWCAM says -- see per_draw_solver() above for
		// why, for the measurement that made this necessary, for the three hypotheses it refutes
		// (camera refusal, camera dropout, the ring failing to record), and for the fourth it
		// nearly fell to (the same camera at two column scales).
		float draw_kick_m[16] = {};
		u64 draw_kick_hash = 0;

		if (world_mode || s_frametrace_left > 0)
			draw_kick_hash = lookup_draw_kick_camera(draw_kick_m);

		// nullptr = "this draw's camera is the frame camera, or the ring/pipeline could not give a
		// trustworthy one, or placement is off" -- all of which mean: un-project with the frame
		// solver, unchanged.
		const remix_ps2::clip_solver* const draw_camera =
			world_mode ? per_draw_solver(draw_kick_hash, draw_kick_m) : nullptr;
		const remix_ps2::clip_solver& base_solver = draw_camera ? *draw_camera : s_active_camera.solver;

		// Sky geometry has to be solved in a space with no eye in it.
		//
		// solve_world_position computes (clip - bias) * B^-1, and for an accepted type-1
		// perspective the solver's bias IS the eye-translation term: it is the fused matrix's
		// row 3 restricted to columns {0,1,3}, and classify_perspective has already pinned the
		// other row-3 entries to ~0. So subtracting it adds +eye to every vertex. For world
		// geometry that is correct and is the whole point. For a backdrop it is wrong: the guest
		// drew the sky with its own translation-free matrix, so the eye gets added to something
		// that never had one, and the dome and the sun ride the camera -- which is exactly the
		// reported "sun/moon follows me from behind".
		//
		// Zeroing bias leaves the 3x3 orientation inverse untouched, so this is precisely "world
		// rotation, zero eye" -- the same space the sky camera is submitted in by submit_camera().
		// Both halves have to agree or the sky is displaced by the full eye position, which on
		// save state 7 is ~20,000 units and puts the backdrop somewhere off in the distance
		// instead of behind the level.
		//
		// Derived from base_solver, not from s_active_camera.solver, so a sky draw placed with its
		// own kick camera gets the bias-zeroed variant of THAT camera. The two halves have to be
		// the same camera or the dome is displaced by the difference between them.
		remix_ps2::clip_solver sky_solver = base_solver;
		sky_solver.bias[0] = 0.f;
		sky_solver.bias[1] = 0.f;
		sky_solver.bias[2] = 0.f;

		const GSVertex* const verts = r.m_vertex->buff;

		// The baked lightmap for this surface, sampled from the masked channel passes that
		// re-drew these same vertices. Complete only once all three channels have been seen.
		const lm_verts* lm_fold = nullptr;
		float lm_scale = 1.f;
		float lm_floor = 0.f;
		// Batched draws are modulated in place by the masked passes that follow them in the same
		// frame, so applying the cross-frame cache here as well would square the lightmap.
		if (lightmap_fold_mode() != 0 && lm_pos_hash != 0 && batch_mode() == 0)
		{
			const auto lm_it = s_lm_verts.find(lm_pos_hash);
			if (lm_it == s_lm_verts.end())
				++s_lm_miss_nokey;
			else if (lm_it->second.seen != 0x7)
				++s_lm_miss_partial;
			else if (lm_it->second.n != r.m_vertex->next)
				++s_lm_miss_vcount;
			else
			{
				lm_fold = &lm_it->second;
				lm_scale = lightmap_fold_scale();
				lm_floor = lightmap_fold_floor();
				++s_lm_applied;
			}
		}

		// Z -> w calibration scale for this draw. Hoisted above the sky classification because the
		// far-distance scan below needs it too, and computing it twice would be two expressions
		// that have to be kept in step by hand. Its other reader is draw_zfit further down.
		const double zfit_scale = 1.0 / static_cast<double>(
			0xFFFFFFFFu >> (GSLocalMemory::m_psm[r.m_cached_ctx.ZBUF.PSM].fmt * 8));

		// The draw's nearest vertex in eye depth, for the far-distance requirement on sky
		// classification (PCSX2_REMIX_SKYMINW). It has to be known HERE, before the vertex loop,
		// because the classification chooses the solver that loop un-projects with -- so it cannot
		// wait for the loop's own min_w.
		//
		// Walked only when the requirement is armed. With SKYMINW = 0 (the default) this is one
		// float compare per draw and no vertex traffic at all.
		//
		// Computed with the SAME q/w expression the loop uses, deliberately. A second
		// interpretation of the same state is how the two would drift into classifying a draw one
		// way here and reporting it the other way on the dump line, and the loop's min_w likewise
		// covers every vertex (a vertex that fails its finite/limit check drops the whole draw),
		// so the two are the same number by construction.
		const GSTextureCache::Source* const source =
			static_cast<const GSTextureCache::Source*>(tex_source);
		const bool cloud_rt_candidate = sky_mode() == 4 && sky_min_w() > 0.f && source &&
			draw_samples_render_target(source) && !r.m_cached_ctx.DepthWrite() &&
			source->m_TEX0.TW == 7 && source->m_TEX0.TH == 7 && index_count <= 12;
		float sky_gate_min_w = 0.f;
		float cloud_gate_max_w = -std::numeric_limits<float>::max();
		float cloud_gate_min_px = std::numeric_limits<float>::max();
		float cloud_gate_max_px = -std::numeric_limits<float>::max();
		float cloud_gate_min_py = std::numeric_limits<float>::max();
		float cloud_gate_max_py = -std::numeric_limits<float>::max();
		float cloud_gate_min_u = std::numeric_limits<float>::max();
		float cloud_gate_max_u = -std::numeric_limits<float>::max();
		if (sky_min_w() > 0.f)
		{
			float scan_min_w = std::numeric_limits<float>::max();
			const float scan_fst_inv_w =
				1.0f / static_cast<float>(1u << r.m_cached_ctx.TEX0.TW) / 16.0f;
			for (u32 i = 0; i < vertex_count; ++i)
			{
				const GSVertex& v = verts[i];
				const float q = fallback_screen_ui ? 1.f : (z_depth ?
					static_cast<float>((static_cast<double>(v.XYZ.Z) * zfit_scale * fst_z_a) + fst_z_b) :
					v.RGBAQ.Q);
				const float w = 1.0f / q;
				scan_min_w = std::min(scan_min_w, w);

				if (cloud_rt_candidate)
				{
					const float px = (static_cast<float>(v.XYZ.X) - ox) * (1.0f / 16.0f);
					const float py = (static_cast<float>(v.XYZ.Y) - oy) * (1.0f / 16.0f);
					const float u = fst_draw ? (static_cast<float>(v.U) * scan_fst_inv_w) : (v.ST.S * w);
					cloud_gate_max_w = std::max(cloud_gate_max_w, w);
					cloud_gate_min_px = std::min(cloud_gate_min_px, px);
					cloud_gate_max_px = std::max(cloud_gate_max_px, px);
					cloud_gate_min_py = std::min(cloud_gate_min_py, py);
					cloud_gate_max_py = std::max(cloud_gate_max_py, py);
					cloud_gate_min_u = std::min(cloud_gate_min_u, u);
					cloud_gate_max_u = std::max(cloud_gate_max_u, u);
				}
			}

			sky_gate_min_w = scan_min_w;
		}
		// Highwire's four cloud layers are near, blended 128x128 render-target quads. Their
		// 620x264 coverage and U span of 3.35..3.65 distinguish them from the smaller target
		// consumers later in the frame. They need the sky solver as well as a material snapshot;
		// otherwise the textured quads remain camera-relative world panels.
		const bool cloud_sky_draw = cloud_rt_candidate && cloud_gate_max_w > 0.f &&
			cloud_gate_max_w < sky_min_w() && (cloud_gate_max_u - cloud_gate_min_u) >= 3.f &&
			(cloud_gate_max_px - cloud_gate_min_px) >= 500.f &&
			(cloud_gate_max_px - cloud_gate_min_px) <= 700.f &&
			(cloud_gate_max_py - cloud_gate_min_py) >= 200.f &&
			(cloud_gate_max_py - cloud_gate_min_py) <= 320.f;

		// A rtx.skyBoxTextures tag, resolved here rather than left to the category merge, so that
		// it selects the bias-zeroed sky solver below instead of only setting the instance's SKY
		// bit after the geometry has already been un-projected with the eye translation in it.
		// See hash_tagged_sky() for the measurement and the cost guards.
		const bool hash_sky_draw = hash_tagged_sky(source);
		if (hash_sky_draw)
			++s_stats.sky_hash;

		// classify_sky is evaluated again by build_draw_state below; it reads only the cached
		// depth bits, s_submitted_this_frame and the draw's min w, none of which moves between
		// here and there -- the min w passed there is the vertex loop's own, which the scan above
		// reproduces exactly -- so the two agree by construction. The two forced paths
		// (cloud_sky_draw, hash_sky_draw) are handed to it as force_sky for the same reason.
		const bool sky_draw = sky_camera_enabled() &&
			(cloud_sky_draw || hash_sky_draw ||
				classify_sky(r.m_cached_ctx.DepthRead(), r.m_cached_ctx.DepthWrite(),
					s_submitted_this_frame, draw_samples_render_target(tex_source), sky_gate_min_w));

		// Independent of SKYCAM: that knob only chooses the solver. The push replaces the guest's
		// depth outright, so the base solver plants the draw on a sphere centred on the eye --
		// which is what a skybox is -- rather than 5 feet in front of the player.
		const bool sky_push = (sky_distance() > 0.f) && (cloud_sky_draw || hash_sky_draw);

		const remix_ps2::clip_solver& solver = sky_draw ? sky_solver : base_solver;

		s_scratch_vertices.clear();
		s_scratch_vertices.resize(vertex_count);

		// Accumulated locally and only merged once the draw is known to be accepted, so a draw
		// that bails out halfway cannot poison the frame's measured extent.
		scene_bounds draw_bounds{};
		float min_w = std::numeric_limits<float>::max();
		float max_w = -std::numeric_limits<float>::max();
		// Largest |component| this draw put into out.position. The session-wide s_max_seen_position
		// on the stats line is a running max over every vertex ever submitted, so it names a number
		// and nothing else -- it cannot say whether one draw went far away or the whole world did.
		// This is the per-draw version, and it is what the FARDUMP trigger below reads.
		float draw_max_pos = 0.f;
		u32 min_z = 0xFFFFFFFFu;
		u32 max_z = 0;
		// Screen-space extent in pixels, (XY - XYOFFSET)/16 per GSState::GetXYWindow
		// (GSState.cpp:5093). Without this there is no way to tell a full-screen quad from a
		// small one in the dump, which is the single most diagnostic fact about a draw that is
		// covering the viewport.
		float min_px = std::numeric_limits<float>::max();
		float max_px = -std::numeric_limits<float>::max();
		float min_py = std::numeric_limits<float>::max();
		float max_py = -std::numeric_limits<float>::max();
		// Recovered texture coordinates, S/Q and T/Q. A draw whose UV range has collapsed samples
		// one texel and renders as a flat untextured surface whatever the texture contains, which
		// by eye is indistinguishable from a broken palette decode.
		float min_u = std::numeric_limits<float>::max();
		float max_u = -std::numeric_limits<float>::max();
		float min_v = std::numeric_limits<float>::max();
		float max_v = -std::numeric_limits<float>::max();

		// Non-zero once the hyperextension check below has flagged this draw, carried all the way
		// down to the dump site rather than dumped where it is computed: the offender line is only
		// worth reading with the draw's texture source, material hash and sky classification on it,
		// and none of those three exist yet at the point the ratio is known.
		float explode_ratio = 0.f;

		// Z -> w calibration samples for this draw, merged only if the draw is accepted. Only
		// FST=0 draws contribute: they are the ones carrying both Z and a trustworthy w, and
		// feeding recovered values back into the fit would make it self-confirming.
		z_fit draw_zfit{};

		vcolor_stats draw_vcolor{};
		u32 first_vcolor = 0;
		bool vcolor_varies = false;

		// FST draws carry UV in 12.4 fixed-point texels (GSVertex.h:22), so the normalised
		// texture coordinate is (U/16)/width. TEX0.TW/TH are log2 sizes.
		const float fst_inv_w = 1.0f / static_cast<float>(1u << r.m_cached_ctx.TEX0.TW) / 16.0f;
		const float fst_inv_h = 1.0f / static_cast<float>(1u << r.m_cached_ctx.TEX0.TH) / 16.0f;

		// Hoisted: constant for the whole draw, and the vertex loop is the hottest code here.
		const int rot_mode = world_mode ? world_rot_mode() : 0;
		float cam_pos[3] = {0.f, 0.f, 0.f};
		if (rot_mode != 0)
		{
			cam_pos[0] = s_active_camera.position[0];
			cam_pos[1] = s_active_camera.position[1];
			cam_pos[2] = s_active_camera.position[2];
		}

		// Tracked across the draw so a wholly transparent one can be dropped after the loop.
		u32 max_vertex_alpha = 0;

		// The overlay rasteriser needs screen-space positions, which only exist inside this loop.
		if (ui_raster_mode() != 0)
			s_scratch_ndc.resize((size_t)vertex_count * 2);
		else
			s_scratch_ndc.clear();

		for (u32 i = 0; i < vertex_count; ++i)
		{
			const GSVertex& v = verts[i];

			const float ndc_x = ((static_cast<float>(v.XYZ.X) - 0.05f) * sx) - offset_x;
			const float ndc_y = -(((static_cast<float>(v.XYZ.Y) - 0.05f) * sy) - offset_y);

			// w is the eye-space depth the guest divided by. For an FST=0 draw it is 1/Q
			// directly; the two Q guards (GSState.cpp:1399/:1403) can still leave FLT_MIN or
			// GSVector4::m_max here, which the finite check below rejects.
			//
			// For an FST draw Q is not a divisor at all, so w comes from the calibrated
			// Q = a*zn + b fitted on this title's FST=0 draws. Non-positive or non-finite
			// results fall out at the same finite check -- Z below the far plane inverts to a
			// negative w, and rejecting it is correct.
			const float q = fallback_screen_ui ? 1.f : (z_depth ?
				static_cast<float>((static_cast<double>(v.XYZ.Z) * zfit_scale * fst_z_a) + fst_z_b) :
				v.RGBAQ.Q);
			const float w = 1.0f / q;

			if (!s_scratch_ndc.empty())
			{
				s_scratch_ndc[(size_t)i * 2] = ndc_x;
				s_scratch_ndc[((size_t)i * 2) + 1] = ndc_y;
			}

			remixapi_HardcodedVertex& out = s_scratch_vertices[i];

			if (world_mode)
			{
				// clip = (ndc_x*w, ndc_y*w, ., w). The z equation is deliberately not used:
				// a PS2 vertex's GS Z is a raw integer in a per-title convention, and w
				// already carries the absolute depth, so x/y/w is exactly determined.
				float world[3];
				// Scale the whole clip vector, so NDC is preserved and only depth changes scale.
				const float wk = sky_push ? sky_distance() :
					((ee_cam_mode() == 2) ? (w * ee_cam_depth_scale()) : w);
				remix_ps2::solve_world_position(solver, ndc_x * wk, ndc_y * wk, wk, world);

				// A constant wk puts every sky vertex at the same VIEW DEPTH, which is a plane normal
				// to the view -- camera-locked geometry, measured as the sky mesh normal's Z tracking
				// the camera forward's Z exactly. Re-project onto a sphere of that radius about the
				// eye instead, which is what a backdrop actually is.
				if (sky_push)
				{
					const float ox = world[0] - s_active_camera.position[0];
					const float oy = world[1] - s_active_camera.position[1];
					const float oz = world[2] - s_active_camera.position[2];
					const float olen = std::sqrt((ox * ox) + (oy * oy) + (oz * oz));
					if (std::isfinite(olen) && olen > 1e-6f)
					{
						const float scale_to_sphere = wk / olen;
						world[0] = s_active_camera.position[0] + (ox * scale_to_sphere);
						world[1] = s_active_camera.position[1] + (oy * scale_to_sphere);
						world[2] = s_active_camera.position[2] + (oz * scale_to_sphere);
					}
				}

				if (std::isfinite(w))
				{
					s_dd_w_min = std::min(s_dd_w_min, w);
					s_dd_w_max = std::max(s_dd_w_max, w);
					for (u32 k = 0; k < 3; ++k)
					{
						if (!std::isfinite(world[k])) continue;
						s_dd_p_min[k] = std::min(s_dd_p_min[k], world[k]);
						s_dd_p_max[k] = std::max(s_dd_p_max[k], world[k]);
					}
					++s_dd_verts;
				}

				remix_ps2::apply_world_basis_rotation(
					s_active_camera.view, cam_pos, rot_mode, world, out.position);
			}
			else
			{
				out.position[0] = ndc_x * w * inv_a;
				out.position[1] = ndc_y * w * inv_b;
				out.position[2] = w;
			}

			out.normal[0] = 0.f;
			out.normal[1] = 0.f;
			out.normal[2] = -1.f;
			// An untextured draw has no texture to coordinate against, and TEX0.TW/TH are stale,
			// so fst_inv_w/h would be meaningless here. Zero, and let the null material shade it.
			out.texcoord[0] = untex_draw ? 0.f : (fst_draw ? (static_cast<float>(v.U) * fst_inv_w) : (v.ST.S * w));
			out.texcoord[1] = untex_draw ? 0.f : (fst_draw ? (static_cast<float>(v.V) * fst_inv_h) : (v.ST.T * w));
			// PS2 alpha is 0..128 (0x80 == 1.0); scale into 0..255 for a D3DCOLOR-style ARGB.
			// Alpha is kept either way -- it is real transparency, not baked lighting.
			const u32 alpha = std::min<u32>(255u, static_cast<u32>(v.RGBAQ.A) * 2u);
			max_vertex_alpha = std::max(max_vertex_alpha, alpha);
			if (lm_fold != nullptr)
			{
				const float lr = (lm_floor + ((1.f - lm_floor) * lm_fold->rgb[(static_cast<size_t>(i) * 3) + 0])) * lm_scale;
				const float lg = (lm_floor + ((1.f - lm_floor) * lm_fold->rgb[(static_cast<size_t>(i) * 3) + 1])) * lm_scale;
				const float lb = (lm_floor + ((1.f - lm_floor) * lm_fold->rgb[(static_cast<size_t>(i) * 3) + 2])) * lm_scale;
				const u32 mr = static_cast<u32>(std::clamp(static_cast<float>(v.RGBAQ.R) * lr, 0.f, 255.f));
				const u32 mg = static_cast<u32>(std::clamp(static_cast<float>(v.RGBAQ.G) * lg, 0.f, 255.f));
				const u32 mb = static_cast<u32>(std::clamp(static_cast<float>(v.RGBAQ.B) * lb, 0.f, 255.f));
				out.color = (alpha << 24) | (mr << 16) | (mg << 8) | mb;
			}
			else
			out.color = (vcolor_mode() != 0) ?
				((alpha << 24) | (static_cast<u32>(v.RGBAQ.R) << 16) |
					(static_cast<u32>(v.RGBAQ.G) << 8) | static_cast<u32>(v.RGBAQ.B)) :
				((alpha << 24) | 0x00FFFFFFu);

			draw_vcolor.add(v.RGBAQ.R, v.RGBAQ.G, v.RGBAQ.B);
			if (i == 0)
				first_vcolor = out.color & 0x00FFFFFFu;
			else if ((out.color & 0x00FFFFFFu) != first_vcolor)
				vcolor_varies = true;

			if (!std::isfinite(out.position[0]) || !std::isfinite(out.position[1]) || !std::isfinite(out.position[2]) ||
				std::abs(out.position[0]) > position_limit ||
				std::abs(out.position[1]) > position_limit ||
				std::abs(out.position[2]) > position_limit ||
				!(w > 0.0f))
			{
				++s_stats.skip_nonfinite;
				// NFWHY. One bucket cannot say whether these draws are arithmetic casualties or
				// simply behind the eye, and those need opposite fixes.
				{
					static u64 nf_nan = 0, nf_big = 0, nf_w = 0, nf_n = 0;
					const bool nan_hit = !std::isfinite(out.position[0]) || !std::isfinite(out.position[1]) ||
						!std::isfinite(out.position[2]);
					if (nan_hit) ++nf_nan;
					else if (!(w > 0.0f)) ++nf_w;
					else ++nf_big;
					if ((nf_n++ % 20000) == 0)
					{
						// Whether the draw is salvageable: if every vertex but this one yields a
						// finite positive w, discarding the whole draw throws away real geometry.
						u32 good = 0;
						for (u32 j = 0; j < vertex_count; ++j)
						{
							const float qj = fallback_screen_ui ? 1.f : (z_depth ?
								static_cast<float>((static_cast<double>(verts[j].XYZ.Z) * zfit_scale * fst_z_a) + fst_z_b) :
								verts[j].RGBAQ.Q);
							const float wj = 1.0f / qj;
							if (std::isfinite(wj) && wj > 0.f)
								++good;
						}
						INFO_LOG("Remix: NFWHY nan {} w<=0 {} overlimit {} | this draw: good {}/{} vert {} "
								 "w {:.6g} pos {:.1f} {:.1f} {:.1f} tex {} prim {}",
							nf_nan, nf_w, nf_big, good, vertex_count, i, w,
							out.position[0], out.position[1], out.position[2],
							r.m_process_texture ? 1 : 0, static_cast<int>(r.m_vt.m_primclass));
					}
				}
				return;
			}

			if (s_lightfit_armed && world_mode)
			{
				const float lx = out.position[0] - s_lightfit_light[0];
				const float ly = out.position[1] - s_lightfit_light[1];
				const float lz = out.position[2] - s_lightfit_light[2];
				const float ld2 = (lx * lx) + (ly * ly) + (lz * lz);
				if (ld2 < s_lightfit_best_d2)
				{
					s_lightfit_best_d2 = ld2;
					s_lightfit_vert[0] = out.position[0];
					s_lightfit_vert[1] = out.position[1];
					s_lightfit_vert[2] = out.position[2];
					s_lightfit_best_light[0] = s_lightfit_light[0];
					s_lightfit_best_light[1] = s_lightfit_light[1];
					s_lightfit_best_light[2] = s_lightfit_light[2];
				}
			}

			const float vert_max_pos = std::max({std::abs(out.position[0]),
				std::abs(out.position[1]), std::abs(out.position[2])});
			s_max_seen_position = std::max(s_max_seen_position, vert_max_pos);
			draw_max_pos = std::max(draw_max_pos, vert_max_pos);

			draw_bounds.add(out.position);
			min_w = std::min(min_w, w);
			max_w = std::max(max_w, w);

			const float px = (static_cast<float>(v.XYZ.X) - ox) * (1.0f / 16.0f);
			const float py = (static_cast<float>(v.XYZ.Y) - oy) * (1.0f / 16.0f);
			min_px = std::min(min_px, px);
			max_px = std::max(max_px, px);
			min_py = std::min(min_py, py);
			max_py = std::max(max_py, py);
			min_u = std::min(min_u, out.texcoord[0]);
			max_u = std::max(max_u, out.texcoord[0]);
			min_v = std::min(min_v, out.texcoord[1]);
			max_v = std::max(max_v, out.texcoord[1]);
			min_z = std::min(min_z, static_cast<u32>(v.XYZ.Z));
			max_z = std::max(max_z, static_cast<u32>(v.XYZ.Z));

			// Only draws whose w came from Q may feed the Q-from-Z fit. Feeding a Z-derived w back
			// in would be circular -- the fit would be regressing its own output and would report a
			// perfect R^2 no matter how wrong it was. z_depth, not !fst_draw, for that reason.
			if (!z_depth)
				draw_zfit.add(static_cast<double>(v.XYZ.Z) * zfit_scale, static_cast<double>(w));
		}

		// The eye-plane gate. w = 1/Q is the depth the guest divided by; the per-vertex check
		// above only rejects w <= 0, so a draw whose vertices all sit at w = 1e-4 still passes
		// and un-projects to something degenerate at or inside the camera. It is invisible
		// (zero projected area) but it is still a first hit for the runtime's object picking,
		// which is what stops the user selecting anything else in the developer menu.
		//
		// The threshold is deliberately far below the first-person weapon -- culling that by
		// accident is the obvious failure mode here -- and the histogram in the stats line is
		// what it was set from. 0 disables the gate.
		{
			// 2D that the exact const-Q test missed. Placed here because min_w/max_w only exist
			// once the vertex loop has run; see w_flat_limit() for the menu measurement behind it.
			const float flat_limit = w_flat_limit();
			if (!fallback_screen_ui && !ui_candidate && flat_limit > 0.f && max_w > 0.f && ((max_w - min_w) / max_w) < flat_limit)
			{
				++s_stats.skip_w_flat;
				return;
			}

			const float min_w_limit = min_submitted_w();
			if (min_w_limit > 0.f && max_w < min_w_limit)
			{
				++s_stats.skip_minw;
				return;
			}

			// The near-vertex twin of the gate above. That one drops a draw lying ENTIRELY inside
			// the eye; this one drops a draw that merely REACHES it, which the max-w test cannot
			// see.
			//
			// Measured on Rainbow Six 3 slot 9: exactly two draws per frame carry vertices at
			// w ~ 0.007 while their max w is comfortable, so nothing currently rejects them.
			// d=14 spans w [0.0074, 3.3343] -- a 448x ratio across six triangles covering 36.6% of
			// the screen -- and both draws' w values repeat to four decimal places every frame,
			// i.e. they do not move relative to the camera. Reconstruction from clip x/y/w has no
			// precision left at that w, and with this title's z = w + 2715.9 there is no
			// independent depth to stabilise it.
			//
			// Default 0 (off). This rejects whole draws, and the first-person weapon legitimately
			// lives near the eye -- on R6 3 the other near draw is ~430 triangles over half the
			// screen -- so a value that removes the artefact may remove the weapon with it. Set it
			// from the w range in the per-draw dump, not by guessing.
			const float min_vertex_w_limit = min_vertex_w();
			if (min_vertex_w_limit > 0.f && min_w < min_vertex_w_limit)
			{
				++s_stats.skip_min_vertex_w;
				return;
			}

			// The far-field gate. See max_submitted_w() for why this is the arm that matters.
			const float max_w_limit = max_submitted_w();
			if (max_w_limit > 0.f && min_w > max_w_limit)
			{
				++s_stats.skip_maxw;
				return;
			}

			const u32 bucket = (max_w < 1e-3f) ? 0 : (max_w < 1e-2f) ? 1 :
			                   (max_w < 1e-1f) ? 2 : (max_w < 1.f)   ? 3 :
			                   (max_w < 10.f)  ? 4 : (max_w < 100.f) ? 5 : 6;
			++s_stats.w_histogram[bucket];
			s_frame_max_w = std::max(s_frame_max_w, max_w);

			// Per-draw AABB diagonal, i.e. the size of the box this draw will occupy in the TLAS.
			// Accumulated only for draws that clear every gate above, so the ratio describes what
			// the path tracer is actually given rather than what the tee saw.
			const float extent = draw_bounds.diagonal();
			if (extent > 0.f)
			{
				s_frame_max_extent = std::max(s_frame_max_extent, extent);
				s_frame_min_extent = std::min(s_frame_min_extent, extent);

				// The hyperextension check. extent is this draw's size in world units and max_w is
				// the depth its furthest vertex sits at, so extent/max_w is how many eye-depths
				// across the draw is. Being a ratio it is scale-free, which is what makes it
				// comparable between titles whose world units differ by 2x (maxpos ~2,500 on
				// Rainbow Six 3, ~5,300 on SOCOM) and between world and view space, where the
				// positions are computed by two different code paths.
				//
				// max_w > 0 is guaranteed by the per-vertex !(w > 0) rejection above; the guard is
				// kept so a future change there cannot turn this into a division by zero silently.
				if (max_w > 0.f)
				{
					const float ratio = extent / max_w;
					if (std::isfinite(ratio))
					{
						s_stats.hyperextended_peak = std::max(s_stats.hyperextended_peak, ratio);

						const float explode_limit = explode_ratio_limit();
						if (explode_limit > 0.f && ratio > explode_limit)
						{
							++s_stats.hyperextended;
							explode_ratio = ratio;
						}
					}
				}
			}

			s_zfit.merge(draw_zfit);

			if (vcolor_varies)
				++draw_vcolor.draws_varying;
			else
				++draw_vcolor.draws_constant;

			s_vcolor.merge(draw_vcolor);

			if (fst_draw)
				++s_stats.fst_recovered;
			else if (untex_draw)
				++s_stats.untex_recovered;
		}

		// Indices are already a triangle list for GS_TRIANGLE_CLASS (indices_per_prim == 3,
		// GSRendererHW.cpp:5557-5562). Widen u16 -> u32 and bounds-check as we go.
		const u16* const src_indices = r.m_index->buff;
		const u32 triangle_indices = index_count - (index_count % 3);

		s_scratch_indices.clear();
		s_scratch_indices.resize(triangle_indices);

		for (u32 i = 0; i < triangle_indices; ++i)
		{
			const u32 index = src_indices[i];
			if (index >= vertex_count)
			{
				++s_stats.skip_empty;
				return;
			}

			s_scratch_indices[i] = index;
		}

		// A GS sprite is TWO vertices naming opposite corners of a screen-aligned quad, so the
		// buffer above (which assumes indices_per_prim == 3) yields nothing usable. Rebuild it
		// as a triangle list, synthesising the two off-diagonal corners per sprite and taking
		// the flat colour from the second vertex, which is what the GS shades a sprite with.
		if (sprite_ui_probe)
		{
			s_scratch_indices.clear();
			const u32 sprite_indices = index_count - (index_count % 2);
			for (u32 i = 0; i + 1 < sprite_indices; i += 2)
			{
				const u32 a = src_indices[i];
				const u32 b = src_indices[i + 1];
				if (a >= vertex_count || b >= vertex_count || s_scratch_ndc.size() < (size_t)vertex_count * 2)
					continue;

				const float xa = s_scratch_ndc[(size_t)a * 2], ya = s_scratch_ndc[((size_t)a * 2) + 1];
				const float xb = s_scratch_ndc[(size_t)b * 2], yb = s_scratch_ndc[((size_t)b * 2) + 1];
				// A sprite spanning essentially the whole target is a background/framebuffer blit,
				// not UI. Admitting those made the overlay repaint itself once per frame (measured
				// 1.04x overdraw) and paint over the very glyphs this path exists to composite.
				// NDC spans -1..1, so 1.8 is 90% of an axis; real HUD/text elements are far smaller.
				if (std::abs(xb - xa) >= 1.8f && std::abs(yb - ya) >= 1.8f)
				{
					++s_overlay_fullscreen;
					continue;
				}

				const remixapi_HardcodedVertex& va = s_scratch_vertices[a];
				const remixapi_HardcodedVertex& vb = s_scratch_vertices[b];

				const u32 c1 = (u32)s_scratch_vertices.size();
				remixapi_HardcodedVertex corner = vb;
				corner.texcoord[0] = vb.texcoord[0]; corner.texcoord[1] = va.texcoord[1];
				s_scratch_vertices.push_back(corner);
				s_scratch_ndc.push_back(xb); s_scratch_ndc.push_back(ya);

				const u32 c2 = (u32)s_scratch_vertices.size();
				corner = vb;
				corner.texcoord[0] = va.texcoord[0]; corner.texcoord[1] = vb.texcoord[1];
				s_scratch_vertices.push_back(corner);
				s_scratch_ndc.push_back(xa); s_scratch_ndc.push_back(yb);

				s_scratch_indices.push_back(a);  s_scratch_indices.push_back(c1); s_scratch_indices.push_back(b);
				s_scratch_indices.push_back(a);  s_scratch_indices.push_back(b);  s_scratch_indices.push_back(c2);
			}
		}

		if (s_scratch_indices.empty())
		{
			++s_stats.skip_empty;
			return;
		}

		// Flat per-triangle normals from the un-projected edges, AND -- load-bearing -- the
		// point at which zero-area triangles are dropped instead of submitted. The smoothing
		// pass below runs after it and only rewrites the normals it produced.
		//
		// This code used to detect a degenerate triangle, decline to give it a normal, and hand
		// it to CreateMesh anyway. A zero-area or slivered triangle is exactly what produces
		// pathological BVH nodes, and a build that runs long enough trips the driver watchdog:
		// VK_ERROR_DEVICE_LOST, which dxvk-remix turns into exit(0x60D0DEAD). Measured on SOCOM
		// slot 2, 20 launches per arm: submitting no game geometry at all survived 20/20 against
		// a 1/20 control, so the fault is in the content of what we submit -- not its volume
		// (throttling creation to 50/frame gave 4/20) and not its timing (withholding it for the
		// first 600 frames and then submitting still gave 1/20).
		//
		// The threshold is relative to the scene, not absolute: |cross| is twice the triangle
		// area and therefore scales as length squared, and a world unit means something
		// different per title (maxpos ~2,500 on Rainbow Six 3, ~5,300 on SOCOM). A fixed 1e-12
		// is meaningless in both.
		const float degenerate_scale = std::max(s_last_bounds.radius(), 1.f);
		const float degenerate_edge = degenerate_area_epsilon() * degenerate_scale;
		const float degenerate_cross = degenerate_edge * degenerate_edge;

		size_t write = 0;

		for (size_t i = 0; (i + 2) < s_scratch_indices.size(); i += 3)
		{
			const remixapi_HardcodedVertex& v0 = s_scratch_vertices[s_scratch_indices[i]];
			const remixapi_HardcodedVertex& v1 = s_scratch_vertices[s_scratch_indices[i + 1]];
			const remixapi_HardcodedVertex& v2 = s_scratch_vertices[s_scratch_indices[i + 2]];

			const float e1[3] = {v1.position[0] - v0.position[0], v1.position[1] - v0.position[1], v1.position[2] - v0.position[2]};
			const float e2[3] = {v2.position[0] - v0.position[0], v2.position[1] - v0.position[1], v2.position[2] - v0.position[2]};

			float n[3] = {
				(e1[1] * e2[2]) - (e1[2] * e2[1]),
				(e1[2] * e2[0]) - (e1[0] * e2[2]),
				(e1[0] * e2[1]) - (e1[1] * e2[0])};

			const float len = std::sqrt((n[0] * n[0]) + (n[1] * n[1]) + (n[2] * n[2]));

			if (!std::isfinite(len) || len < degenerate_cross)
			{
				// Drop the whole index triple. Skipping only the normal is what shipped and it
				// left the triangle in the acceleration structure.
				++s_stats.degenerate_triangles;
				continue;
			}

			n[0] /= len;
			n[1] /= len;
			n[2] /= len;

			// Orient it toward the eye. The cross product's sign follows the triangle's winding,
			// and PS2 strip-to-list conversion does not keep winding consistent -- measured as two
			// adjacent meshes carrying exactly opposite normals in one frame. doubleSided then
			// resolves each ray toward the viewer, which makes the shading normal view-dependent
			// and sweeps light across surfaces as the camera turns. The guest rasterised this
			// triangle, so its front face is the one pointing at the eye.
			{
				const float cx = (v0.position[0] + v1.position[0] + v2.position[0]) * (1.f / 3.f);
				const float cy = (v0.position[1] + v1.position[1] + v2.position[1]) * (1.f / 3.f);
				const float cz = (v0.position[2] + v1.position[2] + v2.position[2]) * (1.f / 3.f);
				const float ex = s_active_camera.position[0] - cx;
				const float ey = s_active_camera.position[1] - cy;
				const float ez = s_active_camera.position[2] - cz;
				if (((n[0] * ex) + (n[1] * ey) + (n[2] * ez)) < 0.f)
				{
					n[0] = -n[0];
					n[1] = -n[1];
					n[2] = -n[2];
				}
			}

			// The whole triangle shares it: doubleSided = 1 makes the winding irrelevant.
			for (u32 k = 0; k < 3; ++k)
			{
				const u32 index = s_scratch_indices[i + k];
				remixapi_HardcodedVertex& target = s_scratch_vertices[index];
				target.normal[0] = n[0];
				target.normal[1] = n[1];
				target.normal[2] = n[2];
				s_scratch_indices[write + k] = index;
			}

			write += 3;
		}

		s_scratch_indices.resize(write);

		if (s_scratch_indices.empty())
		{
			// Every triangle in the draw was degenerate.
			++s_stats.skip_all_degenerate;
			return;
		}

		smooth_scratch_normals(degenerate_scale);

		// Normals are final here, and every vertex still carries its baked colour, which is the
		// only place this game's authored lighting survives. Called from the caller rather than
		// from inside smooth_scratch_normals() so the fit still runs when smoothing is disabled.
		accumulate_moon_fit();

		// --- material ------------------------------------------------------------------------
		// Recomputed here rather than read off the Source: only HashCacheEntry* is stored
		// there (GSTextureCache.h:306) and it is null on most draws and always null for a
		// render-target source, so the key has to be rebuilt from TEX0/TEXA/CLUT/region.
		const bool allow_render_target_snapshot = sky_draw;
		// An untextured draw has no source to key a material on, so bind() would hand back null and
		// the surface would shade colourless -- and since UNTEXZ these are the majority of a SOCOM
		// frame. Give them the shared white material instead, so their per-vertex colour lands.
		const remix_ps2::materials::binding material = untex_draw ?
			remix_ps2::materials::bind_untextured(s_remix) :
			remix_ps2::materials::bind(s_remix, source, s_frame_counter, allow_render_target_snapshot);

		// Fully transparent in the guest: it drew nothing, so neither should we.
		if (drop_clear_mode() != 0 && max_vertex_alpha == 0 && vcolor_mode() != 0)
		{
			++s_stats.skip_clear_alpha;
			return;
		}

		// A 2D draw is a HUD element, not a surface. Rasterise it into the screen overlay and
		// stop -- submitting it as geometry is what put the menus out in the world.
		// A world billboard is not UI, however flat its depth is.
		const float ui_w_limit = ui_w_max();
		const bool ui_depth_ok = (ui_w_limit <= 0.f) || (max_w > 0.f && max_w <= ui_w_limit);
		if (s_drawdump_frames_left > 0 && flat_2d && !r.m_cached_ctx.DepthWrite() &&
			max_w > 0.f && (ui_w_limit <= 0.f || max_w > ui_w_limit))
		{
			INFO_LOG("Remix: MOONCAND d={} w=[{:.6g},{:.6g}] target={} alpha={} mat={:016X} present={} sky={}",
				s_submitted_this_frame, min_w, max_w,
				(source && (source->m_target || source->m_from_target)) ? 1 : 0,
				max_vertex_alpha, material.content_hash, material.material ? 1 : 0, sky_draw ? 1 : 0);
		}

		if ((fallback_screen_ui || ui_candidate) && ui_raster_mode() != 0 && ui_depth_ok)
		{
			if (material.content_hash == 0)
				++s_screen_ui_nomat;
			else if (s_scratch_ndc.empty())
				++s_screen_ui_nondc;
		}

		if ((fallback_screen_ui || ui_candidate) && ui_raster_mode() != 0 && ui_depth_ok &&
			material.content_hash != 0 && !s_scratch_ndc.empty())
		{
			// Size the buffer to the guest's own target the first time a UI draw appears, so the
			// overlay is authored at native resolution and Remix scales it once, at the end.
			if (s_overlay_w != (u32)rt_unscaled_width || s_overlay_h != (u32)rt_unscaled_height)
			{
				overlay_reset((u32)rt_unscaled_width, (u32)rt_unscaled_height);
				s_overlay_frame = ~0ull;
			}

			// First UI draw of this frame: wipe last frame's HUD and rebuild.
			if (s_overlay_frame != s_frame_counter)
			{
				s_overlay_frame = s_frame_counter;
				std::fill(s_overlay.begin(), s_overlay.end(), (u8)0);
				s_overlay_used = false;
			}

			overlay_raster(material.content_hash);
			return;
		}

		// A sprite was admitted past the primclass gate ONLY as an overlay candidate. If the
		// raster path above did not consume it, refuse it here exactly as that gate would have:
		// it must never reach mesh identity or geometry submission as world geometry.
		if (sprite_ui_probe)
		{
			++s_stats.skip_not_triangle;
			return;
		}

		// --- mesh identity ------------------------------------------------------------------
		u64 hash = fnv_seed;
		const bool stable_id = (stable_identity() != 0);

		// The draw's own frame of reference: centroid and radius of gyration. Both are averages
		// over every vertex, so per-vertex un-projection jitter cancels in them to O(1/sqrt(N))
		// -- which is exactly why they can carry identity where a per-vertex quantization cannot.
		float centroid[3] = {0.f, 0.f, 0.f};
		float draw_rms = 0.f;

		if (stable_id)
		{
			double sum[3] = {0.0, 0.0, 0.0};

			for (const remixapi_HardcodedVertex& v : s_scratch_vertices)
			{
				for (u32 k = 0; k < 3; ++k)
					sum[k] += static_cast<double>(v.position[k]);
			}

			const double inv_count = 1.0 / static_cast<double>(s_scratch_vertices.size());
			for (u32 k = 0; k < 3; ++k)
				centroid[k] = static_cast<float>(sum[k] * inv_count);

			double radius_sq = 0.0;
			s_scratch_local.clear();
			s_scratch_local.resize(s_scratch_vertices.size() * 3);

			for (size_t i = 0; i < s_scratch_vertices.size(); ++i)
			{
				for (u32 k = 0; k < 3; ++k)
				{
					const float local = s_scratch_vertices[i].position[k] - centroid[k];
					s_scratch_local[(i * 3) + k] = local;
					radius_sq += static_cast<double>(local) * static_cast<double>(local);
				}
			}

			draw_rms = static_cast<float>(std::sqrt(radius_sq * inv_count));

			if (!std::isfinite(draw_rms) || !std::isfinite(centroid[0]) ||
				!std::isfinite(centroid[1]) || !std::isfinite(centroid[2]))
			{
				++s_stats.skip_nonfinite;
				return;
			}
		}

		if (stable_id)
		{
			// Everything here is either an exact integer out of the guest's own vertex buffer or
			// an average over the whole draw. Nothing derived from the recovered camera per
			// vertex takes part, which is what makes the key survive camera motion.
			hash = fnv_mix(hash, s_scratch_vertices.size());
			hash = fnv_mix(hash, s_scratch_indices.size());

			for (const u32 index : s_scratch_indices)
				hash = fnv_mix(hash, index);

			if (identity_use_color())
			{
				for (const remixapi_HardcodedVertex& v : s_scratch_vertices)
					hash = fnv_mix(hash, v.color);
			}

			// Size, log-quantized at eight steps per octave (~9% resolution). A plain linear
			// quantization would have the same boundary-flip problem as position hashing; a log
			// scale keeps the relative resolution constant across a decal and a wall, and the
			// quantity itself is an average, so it barely moves.
			const float scale = (draw_rms > 1e-6f) ? (std::log2(draw_rms) * 8.f) : -1024.f;
			hash = fnv_mix(hash, static_cast<u64>(static_cast<s64>(std::lround(scale))));

			// Remix binds the material into the mesh at CreateMesh time, so two draws with
			// identical geometry but different textures must not share a handle -- and a tag-list
			// change has to re-key so the new material reaches live geometry.
			hash = fnv_mix(hash, material.content_hash);
			hash = fnv_mix(hash, remix_ps2::materials::generation());
		}
		else
		{
			// Positions are quantized *for hashing only* -- the submitted vertex data stays at
			// full precision.
			//
			// Why: the world position of a vertex is solved through the recovered camera, so
			// sub-unit jitter in the camera solution shifts every position slightly every
			// frame. Hashing the raw bytes therefore mints a new mesh for static geometry on a
			// static camera, and Remix destroys and recreates the geometry each frame -- which
			// is visible as flicker and makes the runtime's texture-categorisation UI
			// unusable, because there is nothing on screen long enough to hover.
			//
			// Normals are deliberately excluded: they are derived from the positions, so they
			// carry exactly the same jitter. Texcoords and colour come straight out of the
			// guest's GSVertex and are bit-exact frame to frame, so they are hashed as-is and
			// still distinguish genuinely different draws.
			const float quantum = mesh_hash_quantum();
			const float inv_quantum = (quantum > 0.f) ? (1.0f / quantum) : 0.f;

			for (const remixapi_HardcodedVertex& v : s_scratch_vertices)
			{
				for (u32 k = 0; k < 3; ++k)
				{
					if (inv_quantum > 0.f)
					{
						hash = fnv_mix(hash, static_cast<u64>(static_cast<s64>(std::lround(v.position[k] * inv_quantum))));
					}
					else
					{
						u32 bits;
						std::memcpy(&bits, &v.position[k], sizeof(bits));
						hash = fnv_mix(hash, bits);
					}
				}

				u32 st[2];
				std::memcpy(&st[0], &v.texcoord[0], sizeof(u32));
				std::memcpy(&st[1], &v.texcoord[1], sizeof(u32));
				hash = fnv_mix(hash, (static_cast<u64>(st[1]) << 32) | st[0]);
				hash = fnv_mix(hash, v.color);
			}

			for (const u32 index : s_scratch_indices)
				hash = fnv_mix(hash, index);

			// Load-bearing: Remix binds the material into the mesh at CreateMesh time, so two
			// draws with identical geometry but different textures must not share a handle.
			// Zero when no material resolved, which also means a draw that missed the create
			// budget this frame gets its own (untextured) handle and picks up the real one on a
			// later frame instead of being stuck with the default material forever.
			hash = fnv_mix(hash, material.content_hash);

			// The tag-list generation. Remix binds the material into the mesh at CreateMesh
			// time, so a mesh cached before the user tagged a texture would keep the pre-tag
			// material -- an emissive tag would appear to do nothing until the mesh aged out.
			hash = fnv_mix(hash, remix_ps2::materials::generation());
		}

		if (hash == 0)
		{
			// Remix treats a zero hash as unset.
			hash = 1;
		}

		if (s_poisoned.count(hash) != 0)
		{
			++s_stats.skip_poisoned;
			return;
		}

		// Latch one big mesh and follow it. Big, because a large draw is world geometry rather
		// than a HUD quad or a particle, and its centroid is averaged over enough vertices that
		// per-vertex un-projection noise cancels.
		if (stable_id)
		{
			// Latching ONE hash failed: the chosen mesh was submitted once and never again
			// (seen 1), so its centroid froze and measured nothing. Report the LARGEST mesh of
			// each window instead, with its hash. Whenever consecutive log lines share a hash,
			// their centroids are directly comparable -- and a world-anchored mesh's centroid
			// cannot move while the player only turns.
			// EXCLUDE THE PLAYER. The biggest mesh on screen is always the animated character:
			// ~1000 verts, a new geometry hash every frame, and a centroid ~10 units from the
			// eye. It follows the camera by definition, so it can never answer "is the world
			// anchored?". Require the mesh to be far enough away to be scenery.
			const float dx = centroid[0] - s_active_camera.position[0];
			const float dy = centroid[1] - s_active_camera.position[1];
			const float dz = centroid[2] - s_active_camera.position[2];
			const float cam_dist = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));

			const u32 verts = static_cast<u32>(s_scratch_vertices.size());
			// 3000 was set when the un-projection scattered the scene; the EE camera resolves a
			// room ~580 units across, so nothing was ever far enough and this never fired. The
			// bound only has to clear the player (~10 units) and the weapon.
			if (verts >= 64 && cam_dist > 250.f && verts > s_meshtrack_verts)
			{
				s_meshtrack_hash = hash;
				s_meshtrack_verts = verts;
				s_meshtrack_centroid[0] = centroid[0];
				s_meshtrack_centroid[1] = centroid[1];
				s_meshtrack_centroid[2] = centroid[2];
				s_meshtrack_dist = cam_dist;
				{
					const remixapi_HardcodedVertex& nv = s_scratch_vertices[s_scratch_indices[0]];
					s_meshtrack_normal[0] = nv.normal[0];
					s_meshtrack_normal[1] = nv.normal[1];
					s_meshtrack_normal[2] = nv.normal[2];
					const float inv = (cam_dist > 0.f) ? (1.f / cam_dist) : 0.f;
					s_meshtrack_facing = ((nv.normal[0] * -dx) + (nv.normal[1] * -dy) + (nv.normal[2] * -dz)) * inv;
				}
				++s_meshtrack_seen;
			}
		}

		// The registration tolerance, and the quantum the dedupe key measures position in. One
		// number so that "close enough to be the same geometry" and "close enough to be the same
		// placement" cannot disagree.
		const float fit_tolerance =
			std::max(identity_tolerance() * std::max(draw_rms, 1e-3f), 1e-5f);

		// Coincident-geometry dedupe. A second draw with the same identity AND the same placement
		// in the same frame is literally the same triangles in the same place -- overdraw the
		// guest needed for its own blending, and z-fighting once a path tracer gets hold of it.
		// 81% of the logged draws in Rainbow Six 3 were coincident with an earlier one.
		//
		// The placement has to be part of this key under stable identity: two copies of one
		// object share a mesh handle by design, and dropping the second would delete it from the
		// scene. The legacy path hashed positions, so its key already carried the placement.
		u64 dedupe_key = hash;

		if (stable_id)
		{
			const float inv_quantum = 1.0f / fit_tolerance;
			for (u32 k = 0; k < 3; ++k)
			{
				dedupe_key = fnv_mix(dedupe_key,
					static_cast<u64>(static_cast<s64>(std::lround(centroid[k] * inv_quantum))));
			}
		}

		if (!s_frame_submitted_hashes.insert(dedupe_key).second)
		{
			++s_stats.skip_coincident;
			return;
		}

		// Same geometry, different material: a multitexture pass, not a duplicate. Keyed on the
		// dedupe key with the material contribution removed, so it is exactly "this draw's
		// triangles in this place, whatever is bound to them". Diagnostic only -- the draw is
		// submitted either way. See multipass_overlay for what this is for.
		{
			const u64 geometry_key = fnv_mix(dedupe_key ^ material.content_hash, 0x9E3779B97F4A7C15ull);
			if (!s_frame_geometry_hashes.insert(geometry_key).second)
				++s_stats.multipass_overlay;
		}

		const remixapi_Interface& api = s_remix.api();

		// The instance-side state, needed here rather than further down because it is what
		// decides which batch this draw can join. Non-const because its blend struct is chained
		// into remixapi_InstanceInfo::pNext, which is a plain void*.
		draw_regs regs{};
		regs.depth_read = r.m_cached_ctx.DepthRead();
		regs.depth_write = r.m_cached_ctx.DepthWrite();
		regs.ate = r.m_cached_ctx.TEST.ATE;
		regs.atst = r.m_cached_ctx.TEST.ATST;
		regs.aref = r.m_cached_ctx.TEST.AREF;
		regs.abe = r.PRIM->ABE;
		regs.tfx = r.m_cached_ctx.TEX0.TFX;
		regs.samples_target = draw_samples_render_target(tex_source);
		// The vertex loop's own value, which is the same number the pre-loop scan above fed to the
		// first classify_sky call. Both readings must agree or the dump's sky= field would describe
		// a different classification than the one the solver was picked on.
		regs.min_w = min_w;
		regs.alpha = r.m_context->ALPHA;

		draw_state ds = build_draw_state(regs, material.content_hash, s_submitted_this_frame,
			untex_draw, cloud_sky_draw || hash_sky_draw);

		// A lightmap pass sits exactly on the surface it modulates, so it has to be a decal or it
		// z-fights -- that is what made the FBMSK gate necessary in the first place. Static, because
		// baked lighting does not move. IGNORE_BAKED_LIGHTING keeps the runtime from also treating
		// the pass's vertex colour as baked light on top of it.
		if (lightmap_pass)
		{
			ds.categories |= REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
			ds.categories |= REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING;
		}

		// The batch key: everything that lives on the instance. Two draws may share a mesh only
		// if they agree on all of it. Materials are per surface and deliberately absent.
		u64 group_key = fnv_seed;
		{
			const u8* const blend_bytes = reinterpret_cast<const u8*>(&ds.blend);
			// From categoryFlags onward -- sType and pNext are fixed and pNext is a pointer, so
			// hashing the whole struct would key on an address.
			for (size_t i = offsetof(remixapi_InstanceInfoBlendEXT, alphaTestEnabled);
				 i < sizeof(remixapi_InstanceInfoBlendEXT); ++i)
			{
				group_key = fnv_mix(group_key, blend_bytes[i]);
			}

			group_key = fnv_mix(group_key, ds.categories);
			group_key = fnv_mix(group_key, (alpha_state_mode() == 2) ? 1u : 0u);
		}

		s_frame_group_keys.insert(group_key);

		// PCSX2_REMIX_FRAMETRACE: one sample per draw, folded into the window's totals and read out
		// at VSync. See frametrace_frames() for what it is measuring and why the obvious readings
		// were refuted first.
		//
		// Placed here, in the SHARED section above the batch return, deliberately -- see the dump
		// block below for what living on the far side of that return costs a diagnostic.
		//
		// The ring lookup itself now happens once, up at the per-draw camera election, and this
		// only folds its result in -- reading it twice would double the trace's own cost on the
		// exact run it is measuring. The accumulation deliberately stays HERE rather than moving up
		// with the lookup: it must keep counting only the draws that clear every gate and are
		// actually submitted, which is what the baseline 600-window measurement counted.
		if (s_frametrace_left > 0)
		{
			++s_frame_trace.draws;

			if (draw_kick_hash != 0)
			{
				if (s_frame_trace.first_hash == 0)
				{
					s_frame_trace.first_hash = draw_kick_hash;
				}
				else if (draw_kick_hash != s_frame_trace.first_hash)
				{
					++s_frame_trace.not_first;

					if (s_frame_trace.second_hash == 0)
						s_frame_trace.second_hash = draw_kick_hash;
				}
			}
			else
			{
				// Counted and nothing else. The ring's contract is that a miss means fall back to
				// the frame camera and never guess (RemixVU1Capture.h:127-129); this sampler places
				// nothing at all, so all it can honestly report is that the ring was empty here.
				++s_frame_trace.misses;
			}
		}

		// PCSX2_REMIX_WORLDPROBE: accumulate this draw's world-space placement under its material
		// content hash. See worldprobe_frames() for the question this answers.
		//
		// Placed here, beside the FRAMETRACE sampler, for the same two reasons: this is the SHARED
		// section above the batch early return -- a diagnostic on the far side of that return is
		// unreachable under PCSX2_REMIX_BATCH = 1, which is this title's deployed setting and which
		// silently produced an empty per-draw dump once already -- and it counts only draws that
		// cleared every gate and are actually being submitted.
		//
		// world_mode only: with no world camera the vertex loop wrote VIEW-space positions, which
		// are camera-relative by construction and would answer the probe's question "yes" for a
		// reason that has nothing to do with the defect. Sky is excluded because a sky draw is
		// deliberately solved with the bias-zeroed (eye-free) solver, so it is not evidence about
		// world geometry either way.
		//
		// s_scratch_vertices still holds world positions at this point: the stable-identity path
		// rewrites them to local coordinates further down, past the batch branch.
		if (s_worldprobe_left > 0 && world_mode && !ds.is_sky)
			worldprobe_sample(material.content_hash, static_cast<u32>(s_scratch_indices.size() / 3));

		// PCSX2_REMIX_WORLDFIX rides the same gate for the same three reasons -- shared section
		// above the batch early return, world positions still in s_scratch_vertices, and only
		// draws that cleared every gate -- but keys on (material, vertex count, index count)
		// rather than the material alone, so a window whose merged set changed does not pair.
		if (s_worldfix_active > 0 && world_mode && !ds.is_sky)
			worldfix_sample(material.content_hash, static_cast<u32>(s_scratch_vertices.size()),
				static_cast<u32>(s_scratch_indices.size()));

		// PCSX2_REMIX_CAMTEST rides the same gate for the same three reasons, and adds the clipping
		// defence its whole result depends on: the draw's screen box must be strictly inside the
		// viewport. It stores CLIP samples rather than the world positions beside them -- clip is the
		// guest's own output and carries no camera, so the same samples can be un-projected with
		// every candidate. The constants below are the ones the vertex loop above used, handed over
		// rather than recomputed so the two cannot disagree.
		if (s_camtest_active > 0 && world_mode && !ds.is_sky)
		{
			camtest_clip_map map{};
			map.scale_x = sx;
			map.scale_y = sy;
			map.offset_x = offset_x;
			map.offset_y = offset_y;
			map.z_scale = zfit_scale;
			map.z_a = fst_z_a;
			map.z_b = fst_z_b;
			map.screen_ui = fallback_screen_ui;
			map.z_depth = z_depth;

			camtest_screen_box box{};
			box.min_x = min_px;
			box.max_x = max_px;
			box.min_y = min_py;
			box.max_y = max_py;
			box.width = rt_unscaled_width;
			box.height = rt_unscaled_height;

			camtest_sample(material.content_hash, vertex_count,
				static_cast<u32>(s_scratch_indices.size()), verts, map, box);
		}

		// Built once and used twice. PCSX2_REMIX_DRAWDUMP's per-frame dump wants it, and so do the
		// hyperextension offenders: bucketing an explosion by class needs the same fields the sky
		// rule was derived from -- FST, world or view space, whether the texture is a render target,
		// and the w range -- so duplicating a shortened field set for them would be strictly worse.
		//
		// MOVED HERE 2026-08-15, from after DrawInstance -- i.e. from the far side of the batch
		// early return below -- because that made it UNREACHABLE under PCSX2_REMIX_BATCH = 1, which
		// is this title's deployed setting (SCUS-97545.conf). MEASURED on the clip run: the log
		// carries `PCSX2_REMIX_DRAWDUMP = 4 (PCSX2 toggle)`, drawdump_write's own "writing per-draw
		// state to ..." line never appears, and logs\remix_draws.txt is two days older than the run.
		// The knob was accepted, reported applied, and wrote nothing -- which is the worst failure
		// mode a diagnostic has, and it blocked the sky work that needs the dome's draw ordinal.
		//
		// The one behavioural difference on the non-batch path, stated so it is not re-diagnosed as
		// a bug: a draw is now dumped when it is accepted for submission rather than after
		// DrawInstance returned, so draws later dropped by the instance budget or by a poisoned
		// handle appear in the dump too. Both counters read 0 on this title (instbudget-skip 0,
		// poisoned 0), and "what the frame asked to submit" is the more useful set for a dump whose
		// whole job is to describe the frame. None of the printed fields change between here and
		// there; they are all final by this point.
		const float far_limit = far_dump_limit();
		const bool far_hit = (far_limit > 0.f) && (draw_max_pos > far_limit);

		if (s_drawdump_frames_left > 0 || explode_ratio > 0.f || far_hit)
		{
			// The camera this draw was actually built under, from the per-kick ring, identified by
			// content hash. Diagnostic for now -- nothing is placed with it yet. If cm varies
			// across the draws of one frame, the title renders with more than one view matrix and
			// the single frame camera is provably wrong for some of them; if it is constant, the
			// door welded to the screen has some other cause.
			float kick_m[16];
			u32 kick_offset = 0;
			u64 kick_hash = 0;

			if (RemixVU1Capture::LookupKickCamera(RemixVU1Capture::GSKickSeq(), kick_m, kick_offset))
			{
				kick_hash = 0xCBF29CE484222325ULL;
				for (u32 i = 0; i < 16; ++i)
				{
					u32 bits = 0;
					std::memcpy(&bits, &kick_m[i], sizeof(bits));
					kick_hash = (kick_hash ^ bits) * 0x100000001B3ULL;
				}
			}
			else
			{
				kick_offset = 0xFFFFFFFFu;
			}

			// One line per distinct camera the ring reports, the first time it is seen, as the 16
			// floats sit in VU1 memory. Uninterpreted on purpose: row- versus column-major is the
			// GS side's guess, and the point of this dump is to read the structure off the raw
			// values rather than through that guess. A backdrop/sky transform carries no
			// translation; a shadow transform is anchored at the light rather than the eye.
			if (kick_hash != 0 &&
				s_logged_kick_cam_count < (sizeof(s_logged_kick_cams) / sizeof(s_logged_kick_cams[0])))
			{
				bool seen = false;
				for (u32 i = 0; i < s_logged_kick_cam_count; ++i)
					seen = seen || (s_logged_kick_cams[i] == kick_hash);

				if (!seen)
				{
					s_logged_kick_cams[s_logged_kick_cam_count++] = kick_hash;

					drawdump_write(fmt::format(
						"CAMERA {:016X} off=0x{:04x} f={} d={} | "
						"[{: .6g} {: .6g} {: .6g} {: .6g}] [{: .6g} {: .6g} {: .6g} {: .6g}] "
						"[{: .6g} {: .6g} {: .6g} {: .6g}] [{: .6g} {: .6g} {: .6g} {: .6g}]",
						kick_hash, kick_offset, s_frame_counter, s_submitted_this_frame,
						kick_m[0], kick_m[1], kick_m[2], kick_m[3],
						kick_m[4], kick_m[5], kick_m[6], kick_m[7],
						kick_m[8], kick_m[9], kick_m[10], kick_m[11],
						kick_m[12], kick_m[13], kick_m[14], kick_m[15]));
				}
			}

			const std::string draw_state_line = fmt::format(
				// pdc: 1 = this draw was un-projected with its OWN kick camera (cm), 0 = with the
				// frame-latched one. It is the per-draw witness for the stats line's placed/match/
				// fallback totals, and the only place the two can be checked against each other.
				"f={} d={} k={} kf={} kt={} cm={:016X} co=0x{:08x} pdc={} verts={} tris={} fst={} world={} sky={} | ZTE={} ZTST={} ZMSK={} zpsm=0x{:02x} depth(r={} w={}) | "
				"ATE={} ATST={} AREF={} AFAIL={} ABE={} ALPHA(A={} B={} C={} D={} FIX={}) | fpsm=0x{:02x} fbmsk=0x{:08x} | "
				"tex tbp0=0x{:04x} tbw={} psm=0x{:02x} tw={} th={} tcc={} tfx={} target={} | "
				// w to six significant figures, not one decimal. At one decimal every draw whose w
				// spread is under 0.1 reads as perfectly flat, and the minw gate sits at 0.01 --
				// so the printed value could not resolve the very range the gate acts on, nor tell
				// a w of exactly 0 (where the un-projection is undefined) from a small one.
				"w=[{:.6g},{:.6g}] z=[{},{}] | px=[{:.0f},{:.0f}]x[{:.0f},{:.0f}] rt={}x{} | "
				"uv=[{:.3f},{:.3f}]x[{:.3f},{:.3f}] | mat={:016X}",
				s_frame_counter, s_submitted_this_frame,
				RemixVU1Capture::KickSeq(), s_latched_kick_seq, RemixVU1Capture::GSKickSeq(),
				kick_hash, kick_offset, (draw_camera != nullptr) ? 1 : 0,
				vertex_count, s_scratch_indices.size() / 3,
				fst_draw ? 1 : 0, world_mode ? 1 : 0,
				ds.is_sky ? 1 : 0,
				static_cast<u32>(r.m_cached_ctx.TEST.ZTE), static_cast<u32>(r.m_cached_ctx.TEST.ZTST),
				static_cast<u32>(r.m_cached_ctx.ZBUF.ZMSK), static_cast<u32>(r.m_cached_ctx.ZBUF.PSM),
				r.m_cached_ctx.DepthRead() ? 1 : 0, r.m_cached_ctx.DepthWrite() ? 1 : 0,
				static_cast<u32>(r.m_cached_ctx.TEST.ATE), static_cast<u32>(r.m_cached_ctx.TEST.ATST),
				static_cast<u32>(r.m_cached_ctx.TEST.AREF), static_cast<u32>(r.m_cached_ctx.TEST.AFAIL),
				static_cast<u32>(r.PRIM->ABE),
				// The GS blend equation (A-B)*C+D, verbatim. ABE=1 is set on 100% of this title's
				// draws, so it says nothing about whether a draw actually blends -- the equation
				// does. Without these fields there is no way to tell an opaque-equivalent setting
				// from a real blend, and ALPHASTATE=2 therefore turned the whole world translucent.
				static_cast<u32>(r.m_context->ALPHA.A), static_cast<u32>(r.m_context->ALPHA.B),
				static_cast<u32>(r.m_context->ALPHA.C), static_cast<u32>(r.m_context->ALPHA.D),
				static_cast<u32>(r.m_context->ALPHA.FIX),
				static_cast<u32>(r.m_cached_ctx.FRAME.PSM), static_cast<u32>(r.m_cached_ctx.FRAME.FBMSK),
				static_cast<u32>(r.m_cached_ctx.TEX0.TBP0), static_cast<u32>(r.m_cached_ctx.TEX0.TBW),
				static_cast<u32>(r.m_cached_ctx.TEX0.PSM), static_cast<u32>(r.m_cached_ctx.TEX0.TW),
				static_cast<u32>(r.m_cached_ctx.TEX0.TH), static_cast<u32>(r.m_cached_ctx.TEX0.TCC),
				static_cast<u32>(r.m_cached_ctx.TEX0.TFX),
				(source && (source->m_target || source->m_from_target)) ? 1 : 0,
				min_w, max_w, min_z, max_z, min_px, max_px, min_py, max_py,
				rt_unscaled_width, rt_unscaled_height, min_u, max_u, min_v, max_v,
				material.content_hash);

			if (s_drawdump_frames_left > 0)
				drawdump_write(draw_state_line);

			// Offender detail behind the 'explode' counter on the stats line. Capped, because a
			// title that misplaces every draw would otherwise fill the 40k-line dump with the same
			// finding: a hundred lines is plenty to name the class, and the counter stays exact
			// regardless of the cap. Deliberately NOT gated on PCSX2_REMIX_DRAWDUMP -- the counter is
			// meant to be readable from a default run, and drawdump_write only creates the file on
			// its first call, so a session with no offenders still writes nothing at all.
			if (explode_ratio > 0.f && s_explode_dumped < 100)
			{
				++s_explode_dumped;
				drawdump_write(fmt::format("EXPLODE ratio={:.1f}x limit={:g}x extent={:.2f} | {}",
					explode_ratio, explode_ratio_limit(), draw_bounds.diagonal(), draw_state_line));
			}

			// Absolute-placement offenders. Same cap and the same reasoning as EXPLODE above: the
			// point is to name the class the far draws belong to, not to log every one. The extent
			// is printed alongside the distance on purpose -- a compact draw sitting 100k units out
			// is a placement bug, a draw whose own extent is 100k is a stretched one, and the two
			// want completely different fixes.
			if (far_hit && s_fardump_written < 200)
			{
				++s_fardump_written;
				drawdump_write(fmt::format("FARDRAW maxpos={:.0f} limit={:g} extent={:.2f} "
										   "w=[{:.2f}..{:.2f}] | {}",
					draw_max_pos, far_limit, draw_bounds.diagonal(), min_w, max_w, draw_state_line));
			}
		}

		if (batch_mode() != 0)
		{
			// A GEOMETRY key, not an identity key. `hash` under STABLEID deliberately excludes
			// positions so an object keeps its identity as the camera moves -- correct for "is this
			// the same object", wrong for "is the vertex data unchanged". The view model has the
			// same identity every frame but new vertices every frame, so keying mesh REUSE on the
			// identity hash handed it a stale mesh and froze the weapon in place. Quantised
			// positions absorb camera jitter on static geometry while still moving when the
			// geometry genuinely does.
			// Per-vertex quantisation cannot work here: with ~1.4 units of camera jitter and a
			// 4-unit bucket, some vertex in a mesh always crosses a boundary and the key changes
			// anyway (reused fell 3709 -> 552 when it was tried). The CENTROID is an average, so
			// the jitter cancels, while anything that genuinely moves -- the view model above all
			// -- shifts it far more than one bucket and correctly re-creates its mesh.
			u64 geom_hash = hash;
			if (!s_scratch_vertices.empty())
			{
				double cx = 0.0, cy = 0.0, cz = 0.0;
				for (const remixapi_HardcodedVertex& gv : s_scratch_vertices)
				{
					cx += gv.position[0]; cy += gv.position[1]; cz += gv.position[2];
				}
				const double inv_n = 1.0 / static_cast<double>(s_scratch_vertices.size());
				const float ccx = static_cast<float>(cx * inv_n);
				const float ccy = static_cast<float>(cy * inv_n);
				const float ccz = static_cast<float>(cz * inv_n);
				// The view model has a stable identity and new vertices every frame, so no key built
				// from identity can tell it changed. It is the only thing that sits at the eye, which
				// is the same test MESHTRACK uses to exclude it. Mixing the frame in forces a rebuild.
				const float edx = ccx - s_active_camera.position[0];
				const float edy = ccy - s_active_camera.position[1];
				const float edz = ccz - s_active_camera.position[2];
				if (((edx * edx) + (edy * edy) + (edz * edz)) < (250.f * 250.f))
					geom_hash = fnv_mix(geom_hash, s_frame_counter);

				const float cq = std::max(1.f, mesh_hash_quantum() * 4.f);
				geom_hash = fnv_mix(geom_hash, static_cast<u64>(static_cast<s64>(std::llround(ccx / cq))));
				geom_hash = fnv_mix(geom_hash, static_cast<u64>(static_cast<s64>(std::llround(ccy / cq))));
				geom_hash = fnv_mix(geom_hash, static_cast<u64>(static_cast<s64>(std::llround(ccz / cq))));
			}
			batch_append(group_key, ds, material, geom_hash);

			if (!ds.is_sky && draw_bounds.valid)
			{
				s_frame_bounds.add(draw_bounds.min);
				s_frame_bounds.add(draw_bounds.max);
				s_frame_bounds_world = world_mode;
			}

			if (ds.is_sky)
				++s_stats.sky_tagged;

			if (ds.is_cutout)
				++s_stats.cutout_tagged;

			++s_stats.draws_submitted;
			++s_submitted_this_frame;
			return;
		}

		// --- claim a slot ---------------------------------------------------------------------
		// Open addressing over identity_slots() keys. The first slot whose stored geometry
		// rigidly registers onto this draw wins; the first empty slot takes a new mesh; and if
		// every slot is occupied by geometry that does not fit, the best of them is used for this
		// frame and flagged to be rebuilt at the frame boundary. Never destroyed here: an
		// instance submitted earlier in this frame may already reference the handle.
		auto it = s_meshes.end();
		remixapi_Transform placement = s_identity_transform;

		if (stable_id)
		{
			const u64 slots = identity_slots();
			u64 probe = hash;
			auto fallback = s_meshes.end();
			u64 fallback_key = hash;
			float fallback_residual = std::numeric_limits<float>::max();
			float fallback_rotation[3][3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
			bool matched = false;
			bool empty_slot = false;

			for (u64 slot = 0; slot < slots; ++slot)
			{
				auto candidate = s_meshes.find(probe);

				if (candidate == s_meshes.end())
				{
					hash = probe;
					empty_slot = true;
					break;
				}

				float rotation[3][3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
				float residual = std::numeric_limits<float>::max();

				if (candidate->second.local.size() == s_scratch_local.size() &&
					!candidate->second.rebuild_pending)
				{
					remix_ps2::kabsch_rotation(candidate->second.local.data(), s_scratch_local.data(),
						s_scratch_vertices.size(), rotation);
					residual = remix_ps2::rigid_residual(candidate->second.local.data(),
						s_scratch_local.data(), s_scratch_vertices.size(), rotation);
				}

				if (residual <= fit_tolerance)
				{
					hash = probe;
					it = candidate;
					matched = true;
					s_stats.id_probe_collisions += slot;
					for (u32 i = 0; i < 3; ++i)
					{
						for (u32 j = 0; j < 3; ++j)
							placement.matrix[i][j] = rotation[i][j];
					}

					break;
				}

				if (residual < fallback_residual)
				{
					fallback_residual = residual;
					fallback = candidate;
					fallback_key = probe;
					std::memcpy(fallback_rotation, rotation, sizeof(rotation));
				}

				// Next slot. A different constant from fnv_mix's own so a slot chain cannot walk
				// onto another key's first slot for a trivial reason.
				probe = fnv_mix(probe, 0x9E3779B97F4A7C15ULL);
			}

			if (matched)
			{
				++s_stats.id_reuse;
			}
			else if (!empty_slot && fallback != s_meshes.end())
			{
				// Every slot is occupied by geometry that does not fit: a real key collision, or
				// an object that is deforming rather than moving rigidly. Use the closest one for
				// this frame and rebuild it at the frame boundary.
				it = fallback;
				hash = fallback_key;
				it->second.rebuild_pending = true;
				++s_stats.id_rebuild;

				for (u32 i = 0; i < 3; ++i)
				{
					for (u32 j = 0; j < 3; ++j)
						placement.matrix[i][j] = fallback_rotation[i][j];
				}
			}

			placement.matrix[0][3] = centroid[0];
			placement.matrix[1][3] = centroid[1];
			placement.matrix[2][3] = centroid[2];
		}
		else
		{
			it = s_meshes.find(hash);
		}

		if (it == s_meshes.end())
		{
			// Mesh creation is the leading suspect for the device-loss exit, which is reported
			// to happen while walking -- i.e. when new geometry streams in and the camera moves,
			// which re-hashes every static mesh. 0 = unlimited (the default, so this changes
			// nothing until it is turned on deliberately).
			const u64 budget = mesh_create_budget();
			if (budget != 0 && s_stats.meshes_created_frame >= budget)
			{
				++s_stats.skip_mesh_budget;
				return;
			}

			// Under stable identity the mesh is uploaded in its own local frame -- positions
			// relative to the draw's centroid -- and placed by the instance transform. That is
			// what lets the same handle serve the object again next frame from a moved camera:
			// the BLAS holds a shape, not a location.
			if (stable_id)
			{
				for (size_t i = 0; i < s_scratch_vertices.size(); ++i)
				{
					for (u32 k = 0; k < 3; ++k)
						s_scratch_vertices[i].position[k] = s_scratch_local[(i * 3) + k];
				}
			}

			remixapi_MeshInfoSurfaceTriangles surface{};
			surface.vertices_values = s_scratch_vertices.data();
			surface.vertices_count = s_scratch_vertices.size();
			surface.indices_values = s_scratch_indices.data();
			surface.indices_count = s_scratch_indices.size();
			surface.skinning_hasvalue = 0;
			// Null here means Remix substitutes its own default material -- still the case for
			// render-target sources, untextured draws and anything the budget deferred.
			surface.material = material.material;

			remixapi_MeshInfo mesh_info{};
			mesh_info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			mesh_info.pNext = nullptr;
			mesh_info.hash = hash;
			mesh_info.surfaces_values = &surface;
			mesh_info.surfaces_count = 1;

			remixapi_MeshHandle handle = nullptr;
			const u32 status = remix_ps2::guarded_create_mesh(api.CreateMesh, &mesh_info, &handle);

			if (status != REMIXAPI_ERROR_CODE_SUCCESS || !handle)
			{
				s_poisoned.insert(hash);
				++s_stats.skip_poisoned;
				return;
			}

			++s_stats.meshes_created;
			++s_stats.meshes_created_frame;
			it = s_meshes.emplace(hash, mesh_entry{handle, s_frame_counter}).first;

			if (stable_id)
			{
				++s_stats.id_create;
				it->second.local = s_scratch_local;
				// The mesh was uploaded in local space, so the identity rotation is exactly
				// right for the frame that created it.
				placement.matrix[0][3] = centroid[0];
				placement.matrix[1][3] = centroid[1];
				placement.matrix[2][3] = centroid[2];
			}

			if (const u64 pool = reuse_pool_size(); pool != 0 && s_pool_hashes.size() < pool)
			{
				s_pool_hashes.push_back(hash);
				s_pinned_hashes.insert(hash);
			}
		}

		it->second.last_used_frame = s_frame_counter;

		remixapi_InstanceInfo instance{};
		instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		instance.pNext = (alpha_state_mode() == 2) ? &ds.blend : nullptr;
		instance.categoryFlags = ds.categories;
		if (reuse_one_handle())
		{
			// First game mesh of the session, held for its lifetime: real geometry rather than
			// the debug triangle, so the BLAS being instanced is representative.
			if (!s_reuse_handle)
				s_reuse_handle = it->second.handle;

			instance.mesh = s_reuse_handle;
		}
		else if (!s_pool_hashes.empty())
		{
			// Round-robin over the pinned pool, so a frame that draws N times references
			// min(N, pool) distinct long-lived handles instead of N fresh ones.
			const u64 index = s_submitted_this_frame % s_pool_hashes.size();
			const auto pooled = s_meshes.find(s_pool_hashes[index]);
			instance.mesh = (pooled != s_meshes.end()) ? pooled->second.handle : it->second.handle;
		}
		else
		{
			instance.mesh = it->second.handle;
		}
		// Under stable identity this is the rigid placement recovered by registering the stored
		// mesh onto this draw; on the legacy path the positions are already in the submitted
		// camera's space and the transform is the identity.
		instance.transform = stable_id ? placement : s_identity_transform;

		// View-space overlay geometry needs the eye's own frame to sit in, otherwise Remix
		// interprets it as world coordinates and the HUD ends up somewhere in the level.
		if (fallback_screen_ui && s_active_camera.valid)
		{
			remix_ps2::mat4 view_to_world{};
			if (remix_ps2::mat4_invert(s_active_camera.view, view_to_world))
				instance.transform = remix_ps2::to_remix_transform(view_to_world);
		}
		instance.doubleSided = 1;

		const u64 inst_budget = instance_budget();
		if (inst_budget != 0 && s_submitted_this_frame >= inst_budget)
		{
			++s_stats.skip_inst_budget;
			return;
		}

		if (no_draw_instance())
		{
			// The mesh was created and is resident; nothing is instanced. Counted as submitted
			// so the rest of the accounting still lines up with a normal run.
			++s_stats.draws_submitted;
			++s_submitted_this_frame;
			return;
		}

		const u32 status = remix_ps2::guarded_draw_instance(api.DrawInstance, &instance);
		if (status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			s_poisoned.insert(hash);
			++s_stats.skip_poisoned;
			return;
		}

		s_frame_instanced_keys.insert(hash);

		if (ds.is_sky)
			++s_stats.sky_tagged;

		if (ds.is_cutout)
			++s_stats.cutout_tagged;

		// Only now that the draw is committed does its extent count towards the frame's, and
		// sky geometry is deliberately excluded: a skybox is huge by construction and would
		// dominate the scene radius the debug light is scaled from.
		if (!ds.is_sky && draw_bounds.valid)
		{
			s_frame_bounds.add(draw_bounds.min);
			s_frame_bounds.add(draw_bounds.max);
			s_frame_bounds_world = world_mode;
		}

		++s_stats.draws_submitted;
		++s_submitted_this_frame;
	}

	void OnVSync()
	{
		if (!armed())
			return;

		ensure_initialized();

		if (!s_live)
			return;

		// FIRST, before anything in this window has referenced a light handle. Rebuilds the fill
		// lights if a knob behind them has moved since the ones that exist were built -- which is
		// the only way a per-game .conf value can ever reach them, the conf being applied 234 ms
		// after create_debug_scene() built the originals. Costs one struct compare per window when
		// nothing changed. See refresh_fill_lights() for why this point in the frame is the safe
		// one to free a handle at.
		refresh_fill_lights();

		// Whether this window holds is decided HERE, ahead of submit_camera(), because under
		// HOLDEMPTY = 2 it changes which camera that call submits. Everything it reads is already
		// final for the window; see decide_hold_window().
		const bool hold_now = decide_hold_window();
		const int hold_mode = hold_empty_mode();
		const bool hold_camera = hold_now && (hold_mode == 2) && s_held_camera.valid;

		// Mode 3: this window is not presented at all. Every Remix API call below is suppressed --
		// camera, geometry, beacon, lights, Present -- so nothing is left accumulated in the runtime
		// awaiting a Present that never comes. Everything AFTER Present still runs, because none of
		// it is present-driven; see hold_empty_mode() for the full audit.
		const bool skip_present = hold_now && (hold_mode == 3);

		if (skip_present)
		{
			++s_stats.hold_skipped_presents;
			note_hold_cadence();

			// Consumed exactly as a hold consumes it, and for the more important reason: this is
			// what makes it impossible to skip two presents in a row. A real drought presents
			// normally from its second window on, so the screen can never go stale.
			s_held_used = true;
			s_hold_pending = false;
		}

		// Frame order mirrors RPCS3's flip(): camera -> Present -> latch -> reap -> stats.
		// The camera submitted here was resolved at the previous VSync, which is the same one
		// this frame's draws un-projected against -- geometry and camera always reference one
		// matrix, at the cost of a bounded one-frame lag under motion.
		//
		// ...except on a held window under mode 2, which submits the PREVIOUS window's camera so
		// that the geometry batch_flush is about to repeat is repeated from the eye it was built
		// for. One SetupCamera per present either way -- the camera changes, the call does not.
		// s_active_camera itself is untouched, so resolve, the extent refutation and the FRAME
		// trace all still see the real resolved camera.
		refresh_window_size();

		// EE camera. Mode 1 logs only; mode 2 REPLACES the resolved camera outright -- view,
		// projection and the clip solver together, so the geometry un-projection and the
		// submitted viewpoint agree. Assigned here, ahead of submit_camera(), which is the
		// same one-frame-lag contract the resolved camera already runs under.
		s_ee_depth_scale = env_float_signed(L"PCSX2_REMIX_EECAMDEPTH", 1.f);
		s_ee_fov = env_float_signed(L"PCSX2_REMIX_EECAMFOV", 48.1f);
		s_ee_aspect = env_float_signed(L"PCSX2_REMIX_EECAMASPECT", 1.429f);

		// LIGHTFIT: pick the emitting lamp nearest the eye, report last frame's residual against it,
		// then arm the next frame's search.
		if (level_lights_mode() != 0 && !s_level_lights.empty())
		{
			static u32 lf_n = 0;
			if (s_lightfit_armed && s_lightfit_best_d2 < 1e29f && (lf_n++ % 120) == 0)
			{
				const float dx = s_lightfit_vert[0] - s_lightfit_best_light[0];
				const float dy = s_lightfit_vert[1] - s_lightfit_best_light[1];
				const float dz = s_lightfit_vert[2] - s_lightfit_best_light[2];
				const float ex = s_lightfit_best_light[0] - s_active_camera.position[0];
				const float ey = s_lightfit_best_light[1] - s_active_camera.position[1];
				const float ez = s_lightfit_best_light[2] - s_active_camera.position[2];
				const float elen = std::sqrt((ex * ex) + (ey * ey) + (ez * ez));
				const float along = (elen > 0.f) ? (((dx * ex) + (dy * ey) + (dz * ez)) / elen) : 0.f;
				const float total = std::sqrt(s_lightfit_best_d2);
				const float perp = std::sqrt(std::max(0.f, (total * total) - (along * along)));
				INFO_LOG("Remix: LIGHTFIT lamp {:.0f} {:.0f} {:.0f} at {:.0f} from eye | nearest vertex "
						 "{:.0f} {:.0f} {:.0f} | residual {:.1f} (along view {:+.1f}, perpendicular {:.1f}) "
						 "| fov {:.1f} aspect {:.3f} depth {:.0f}",
					s_lightfit_best_light[0], s_lightfit_best_light[1], s_lightfit_best_light[2], elen,
					s_lightfit_vert[0], s_lightfit_vert[1], s_lightfit_vert[2],
					total, along, perp, ee_cam_fov(), ee_cam_aspect(), ee_cam_depth_scale());
				s_lightfit_best_d2 = 1e30f;
			}

			float best = 1e30f;
			for (const level_light& ll : s_level_lights)
			{
				if (ll.distant)
					continue;
				const float dx = ll.position[0] - s_active_camera.position[0];
				const float dy = ll.position[1] - s_active_camera.position[1];
				const float dz = ll.position[2] - s_active_camera.position[2];
				const float d2 = (dx * dx) + (dy * dy) + (dz * dz);
				if (d2 < best)
				{
					best = d2;
					s_lightfit_light[0] = ll.position[0];
					s_lightfit_light[1] = ll.position[1];
					s_lightfit_light[2] = ll.position[2];
				}
			}
			s_lightfit_armed = (best < 1e29f);
		}

		if (lightmap_fold_mode() != 0)
		{
			// The per-surface cache is keyed by vertex identity and would otherwise grow for the
			// life of the session -- 78,165 entries and climbing when this was first measured.
			// Anything not re-drawn in 600 frames is out of the room and can be re-sampled if the
			// player returns; a baked lightmap costs one frame to rebuild.
			if ((s_frame_counter % 600) == 0 && s_frame_counter > 600)
			{
				const u64 cutoff = s_frame_counter - 600;
				for (auto it = s_lm_verts.begin(); it != s_lm_verts.end();)
					it = (it->second.frame < cutoff) ? s_lm_verts.erase(it) : std::next(it);
			}

			static u32 lmf_n = 0;
			if ((lmf_n++ % 300) == 0)
			{
				INFO_LOG("Remix: LIGHTMAPFOLD applied {} miss(no-key {} partial {} vcount {}) | stored {} | cached {} "
						 "| lightmap images {} decode-fail {} | in-frame {} | scale {:.2f}",
					s_lm_applied, s_lm_miss_nokey, s_lm_miss_partial, s_lm_miss_vcount,
					s_lm_stored, s_lm_verts.size(), s_lm_images_made, s_lm_decode_fail, s_lm_inframe,
					lightmap_fold_scale());
			}
		}

		if (ee_cam_mode() != 0)
		{
			float eepos[3], eeang[3];
			remix_ps2::mat4 eeview{};
			static u32 s_eecam_n = 0;
			static u64 s_eecam_used = 0;
			if (read_ee_camera(eepos, eeview, eeang))
			{
				// STAGED, not installed here. submit_camera() runs a few lines below and must
				// send the camera THIS frame's geometry was already un-projected against -- the
				// backend's own one-matrix contract. Installing a fresh read before that sent
				// Remix a camera one frame ahead of the geometry, which is the view model
				// lagging and the world sliding under motion. It looked fine with the dev menu
				// open only because the emulator slows and the mismatch shrinks to nothing.
				if (ee_cam_mode() == 2 && build_ee_world_camera(s_ee_pending_camera))
					s_ee_pending_valid = true;
				if ((s_eecam_n++ % 120) == 0)
				{
					if (s_dd_verts > 0)
					{
						INFO_LOG("Remix: DEPTHDIAG verts {} | clip w [{:.6g},{:.6g}] | world x[{:.1f},{:.1f}] "
								 "y[{:.1f},{:.1f}] z[{:.1f},{:.1f}] | extent {:.1f} | eye {:.1f} {:.1f} {:.1f}",
							s_dd_verts, s_dd_w_min, s_dd_w_max,
							s_dd_p_min[0], s_dd_p_max[0], s_dd_p_min[1], s_dd_p_max[1],
							s_dd_p_min[2], s_dd_p_max[2],
							std::max(std::max(s_dd_p_max[0]-s_dd_p_min[0], s_dd_p_max[1]-s_dd_p_min[1]),
								s_dd_p_max[2]-s_dd_p_min[2]),
							eepos[0], eepos[1], eepos[2]);
					}
					s_dd_w_min = 1e30f; s_dd_w_max = -1e30f; s_dd_verts = 0;
					for (u32 k = 0; k < 3; ++k) { s_dd_p_min[k] = 1e30f; s_dd_p_max[k] = -1e30f; }
				}
				if (false)
					INFO_LOG("Remix: EECAM mode {} pos {:.1f} {:.1f} {:.1f} | pitch {:+.1f} yaw {:+.1f} roll {:+.1f} | used {}",
						ee_cam_mode(), eepos[0], eepos[1], eepos[2], eeang[0], eeang[1], eeang[2], s_ee_installed);
			}
			else if ((s_eecam_n++ % 600) == 0)
				INFO_LOG("Remix: EECAM read FAILED (eeMem {} loc {:#x})",
					eeMem ? "ok" : "null", ee_cam_loc_addr());
		}

		if (!skip_present)
		{
			submit_camera(hold_camera ? s_held_camera : s_active_camera);

			if (hold_camera)
				++s_stats.hold_cameras;
		}

		// Now install the staged EE camera: it governs the draws of the NEXT window, and is
		// submitted at the NEXT VSync -- one matrix for geometry and camera, as designed.
		if (s_ee_pending_valid)
		{
			s_active_camera = s_ee_pending_camera;
			s_camera_last_accept_frame = s_frame_counter;
			s_ee_pending_valid = false;
			++s_ee_installed;
		}

		// Before Present, and before the beacon's empty-frame test, because a batched frame's
		// geometry has not been instanced until this runs.
		//
		// This is also where an EMPTY window re-presents the previous one (PCSX2_REMIX_HOLDEMPTY,
		// step 4B): it is the last point before Present at which anything can still reach the
		// screen. Whether it holds was decided above, before submit_camera(), and under mode 2 that
		// call has already submitted the held camera -- so these instances are drawn against the
		// eye they were built for and the window is a true duplicate. See hold_empty_mode() for the
		// measurement, for the age=2 reasoning it refutes, and for what it still does not prove.
		// The beacon's streak below is intentionally left counting raw empty windows: a held
		// window still submitted nothing of its own, and the streak resets on the next real window
		// long before the beacon threshold, so a sustained drought still reports as one.
		batch_flush();

		// The beacon only appears when nothing of the guest's own geometry survived the gates,
		// so it never clutters a working scene but still says "the runtime is alive".
		// Gating this on a single empty frame makes the beacon strobe on and off through normal
		// play, because plenty of individual frames legitimately submit nothing (loads, fades,
		// all-2D frames). Require a sustained drought instead: it still answers "is the runtime
		// alive with no geometry?" but stays out of the way of a scene that is working.
		if (s_submitted_this_frame == 0)
			++s_empty_frame_streak;
		else
			s_empty_frame_streak = 0;

		// The streak itself keeps counting on a skipped window -- it IS an empty window, and the
		// drought accounting has to stay honest -- but nothing is submitted for it.
		if (!skip_present)
		{
			if (s_empty_frame_streak >= s_beacon_after_empty_frames)
				submit_debug_triangle();

			// The return code used to be discarded here, and that was the single remaining blind
			// spot behind "the backend builds a key and a dome, CreateLight reports success, and
			// the developer menu still reads Total Lights: 0". A light that is created but never
			// accepted by the runtime is indistinguishable from one that was never created at all
			// unless this status is read. Logged sparsely -- first failure, then every 300th --
			// because a failure here repeats every frame and would otherwise bury the log.
			//
			// The success side is logged EXACTLY ONCE per handle-set, because "we asked for three
			// lights and the runtime took three" is the fact that separates a submission problem
			// from a shading/exposure one, and it is worth one line to never have to guess again.
			{
				static u64 s_light_draw_failures = 0;
				static u32 s_light_draw_last_ok = 0;
				u32 accepted = 0;
				u32 attempted = 0;

				const auto draw_light = [&](remixapi_LightHandle handle, const char* which) {
					if (!handle)
						return;

					++attempted;
					const u32 status =
						remix_ps2::guarded_draw_light_instance(s_remix.api().DrawLightInstance, handle);
					if (status == REMIXAPI_ERROR_CODE_SUCCESS)
					{
						++accepted;
						return;
					}

					if ((s_light_draw_failures++ % 300) == 0)
					{
						ERROR_LOG("Remix: DrawLightInstance REFUSED the {} light ({}) -- {} refusals so far. "
								  "The light exists (CreateLight succeeded) but the runtime is not taking it, "
								  "which is why LIGHT STATISTICS reads 0.",
							which, remix_ps2::error_name(status), s_light_draw_failures);
					}
				};

				draw_light((light_mode() == 2) ? s_debug_light : nullptr, "camera-attached sphere");
				draw_light(s_sun_light, "key/distant");
				draw_light(s_dome_light, "dome");

				// Authored level lights, if loaded. Drawn every presented frame like the others.
				build_level_lights();
				const float light_range = level_light_range();
				u32 culled = 0;
				for (const level_light& ll : s_level_lights)
				{
					if (light_range > 0.f && !ll.distant)
					{
						const float dx = ll.position[0] - s_active_camera.position[0];
						const float dy = ll.position[1] - s_active_camera.position[1];
						const float dz = ll.position[2] - s_active_camera.position[2];
						if (((dx * dx) + (dy * dy) + (dz * dz)) > (light_range * light_range))
						{
							++culled;
							continue;
						}
					}
					draw_light(ll.handle, "level");
				}

				if (accepted != s_light_draw_last_ok)
				{
					s_light_draw_last_ok = accepted;
					INFO_LOG("Remix: DrawLightInstance accepted {} of {} lights this window "
							 "(sphere {}, key {}, dome {}), {} level lights out of range",
						accepted, attempted, s_debug_light ? "yes" : "no", s_sun_light ? "yes" : "no",
						s_dome_light ? "yes" : "no", culled);
				}
			}

			// The HUD, composited over the traced image. Submitted before Present so the runtime
			// has it for this frame; the buffer is cleared at the top of the next one.
			if (s_overlay_used && s_remix.api().DrawScreenOverlay != nullptr)
			{
				s_remix.api().DrawScreenOverlay(s_overlay.data(), s_overlay_w, s_overlay_h,
					REMIXAPI_FORMAT_B8G8R8A8_UNORM, 1.0f);
				++s_overlay_presents;
			}

			remixapi_PresentInfo present_info{};
			present_info.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
			present_info.pNext = nullptr;
			present_info.hwndOverride = s_hwnd;

			const u32 status = remix_ps2::guarded_present(s_remix.api().Present, &present_info);
			if (status != REMIXAPI_ERROR_CODE_SUCCESS)
				ERROR_LOG("Remix: Present failed ({})", remix_ps2::error_name(status));
		}

		// The frame that just presented is the one whose un-projection constants the world
		// tier has to normalise against.

		if (s_frame_viewport.valid)
			s_last_viewport = s_frame_viewport;

		s_frame_viewport = viewport_constants{};

		// Same one-frame latch for the measured extent: the light placed at the top of the next
		// frame is scaled from the frame that just presented.
		if (s_frame_bounds.valid)
		{
			// The rigid-transform check. A frame submitted in world space must describe the same
			// scene, at the same size, as the view-space frames before it -- the map between the
			// two is rigid. When it does not, the recovered matrix is some other perspective-ish
			// matrix out of VU1 memory rather than the camera, and it will be self-consistent
			// while placing every vertex somewhere wrong. That is the failure the internal
			// consistency checks cannot see: SOCOM's accepted camera measures depth scale 0.9999
			// and anisotropy 2.6x, both perfect, and still explodes the scene 1,450x.
			// EECAM 2 is exempt: its world units come from the game's own actors and relate to the
			// guest's clip w only through PCSX2_REMIX_EECAMDEPTH, so the ratio this tests is
			// non-unity by construction and no camera read that way can ever pass it.
			if (s_frame_bounds_world && s_active_camera.valid && s_frame_max_w > 0.f && ee_cam_mode() != 2)
			{
				const float world_radius = s_frame_bounds.radius();
				const float ratio = world_radius / s_frame_max_w;

				if (!(ratio <= camera_extent_limit()))
				{
					++s_stats.cam_reject_extent;
					s_refuted_matrices.insert(s_active_camera.matrix_hash);

					if (s_stats.cam_reject_extent == 1)
					{
						WARNING_LOG("Remix: refusing a world camera that scatters the scene -- the frame's "
									"deepest vertex sits at w={:.4g}, so the visible geometry cannot span more "
									"than a few times that, and this camera un-projects it to radius {:.4g} "
									"({:.0f}x, limit {:g}x). Its depth scale and anisotropy are both fine, so "
									"this is the wrong matrix rather than a mis-scaled one. Staying in "
									"view-space; raise PCSX2_REMIX_CAMEXTENT to override.",
							s_frame_max_w, world_radius, ratio, camera_extent_limit());
					}

					s_active_camera = world_camera{};
					s_camera_last_accept_frame = 0;
					s_logged_world_camera = false;
				}
			}

			s_last_bounds = s_frame_bounds;
			s_last_bounds_world = s_frame_bounds_world;
		}

		// Fold this frame's largest/smallest submitted extent into the session ratio before the
		// per-frame values are cleared. Frames that submitted fewer than two draws have no ratio
		// to speak of and are not counted, so an empty menu frame cannot dilute the average.
		if (s_frame_max_extent > 0.f && s_frame_min_extent < std::numeric_limits<float>::max() &&
			s_frame_min_extent > 0.f)
		{
			const float ratio = s_frame_max_extent / s_frame_min_extent;
			if (std::isfinite(ratio))
			{
				s_extent_ratio_peak = std::max(s_extent_ratio_peak, ratio);
				s_extent_ratio_total += static_cast<double>(ratio);
				++s_extent_ratio_frames;
			}
		}
		s_frame_max_extent = 0.f;
		s_frame_min_extent = std::numeric_limits<float>::max();

		// Ages the Z->w calibration so it tracks the current projection rather than the session.
		s_zfit.decay(fst_z_decay());

		s_frame_bounds = scene_bounds{};
		s_frame_bounds_world = false;
		s_frame_max_w = 0.f;

		// FRAMETRACE reads the contract this function's own comment states -- "the camera resolved
		// here is the one both the next frame's draws and the next frame's SetupCamera use" -- so
		// the camera the window that just ended actually drew with has to be sampled BEFORE resolve
		// replaces it. Its age is how many presented frames it has been the active one.
		// The per-window camera census, read out before resolve_world_camera replaces the camera it
		// was accumulated against. `solvers` is the decisive number: 1 means every draw of the
		// window that just ended shared one un-projection and per-draw placement cannot change the
		// picture; 2+ means the window straddled cameras and it can. `rings` is the same count over
		// raw ring hashes -- rings > solvers is the "one camera, two column scales" case.
		if (s_window_cameras.solver_count > 0)
		{
			++s_stats.perdraw_windows;
			s_stats.perdraw_solvers_last = s_window_cameras.solver_count;
			s_stats.perdraw_solvers_peak = std::max(s_stats.perdraw_solvers_peak, s_window_cameras.solver_count);
			s_stats.perdraw_rings_last = s_window_cameras.ring_count;
			s_stats.perdraw_rings_peak = std::max(s_stats.perdraw_rings_peak, s_window_cameras.ring_count);

			if (s_window_cameras.solver_count > 1)
				++s_stats.perdraw_multi_windows;

			if (s_window_cameras.overflowed)
				++s_stats.perdraw_census_overflow;
		}

		const u32 trace_census_solvers = s_window_cameras.solver_count;
		const u32 trace_census_rings = s_window_cameras.ring_count;
		s_window_cameras = window_camera_census{};

		const u64 trace_used_hash = s_active_camera.matrix_hash;

		// The same sample for the WORLDPROBE line, taken here for the same reason and at the same
		// instant: this is the eye the window that just ended un-projected its geometry against.
		// Reading it after resolve_world_camera() below would pair this window's recovered world
		// positions with the NEXT window's camera, and a probe whose whole job is to say whether
		// geometry moves with the eye cannot afford to be one frame out on which eye it means.
		worldprobe_camera probe_camera{};
		probe_camera.valid = s_active_camera.valid;
		probe_camera.hash = s_active_camera.matrix_hash;
		probe_camera.position[0] = s_active_camera.position[0];
		probe_camera.position[1] = s_active_camera.position[1];
		probe_camera.position[2] = s_active_camera.position[2];
		// Camera forward in world space, exactly as submit_camera() computes it for
		// place_sun_light: p_view = p_world * V is row-vector, so the gradient of view z with
		// respect to world position is V's third column.
		probe_camera.forward[0] = s_active_camera.view.m[0][2];
		probe_camera.forward[1] = s_active_camera.view.m[1][2];
		probe_camera.forward[2] = s_active_camera.view.m[2][2];

		// PCSX2_REMIX_WORLDFIX consumes the SAME sample, and must be called here rather than after
		// resolve_world_camera() for the identical reason: the eye it pairs this window's recovered
		// positions with has to be the eye those positions were un-projected against. This is also
		// the one place the knob is read from the environment each window; the draw path reads the
		// cached s_worldfix_active instead.
		worldfix_window(worldfix_mode(), probe_camera);

		// Guarded rather than subtracted blind: OnGSClose zeroes s_frame_counter, and an unsigned
		// wrap would print an age of 18 quintillion in the one file this trace exists to be
		// trusted in.
		const u64 trace_used_age = (s_frame_counter >= s_frametrace_cam_since)
									   ? (s_frame_counter - s_frametrace_cam_since)
									   : 0;

		resolve_world_camera();

		if (s_frametrace_cam_hash != s_active_camera.matrix_hash)
		{
			s_frametrace_cam_hash = s_active_camera.matrix_hash;
			s_frametrace_cam_since = s_frame_counter;
		}

		// One line per presented window. Armed on the same condition the per-draw dump uses -- a
		// frame that submitted something with a world camera live -- so the budget is spent
		// in-mission rather than on the deploy menu, which never solves a camera at all.
		//
		// The frame that arms it emits nothing: the draw-side sampler was off while its draws were
		// being built, so its counters would read as an empty window and look exactly like the
		// defect being hunted.
		if (!s_frametrace_started)
		{
			if (frametrace_frames() > 0 && s_submitted_this_frame > 0 && s_active_camera.valid)
			{
				// ...but "the first window that qualifies" is still startup, not gameplay. Count
				// those qualifying windows off first so FRAMETRACEAFTER can put the measurement
				// where the symptom is -- see frametrace_after_frames() for the session this cost.
				if (s_frametrace_skipped < frametrace_after_frames())
				{
					++s_frametrace_skipped;
				}
				else
				{
					s_frametrace_started = true;
					s_frametrace_left = frametrace_frames();
				}
			}
		}
		else if (s_frametrace_left > 0)
		{
			--s_frametrace_left;

			// Rides dump_write so the FRAME lines land in remix_matrices.txt beside the per-frame
			// candidate lines the analysis scripts already parse, and can be joined to them on f=.
			// (dump_write opens that file on its first call whatever PCSX2_REMIX_DUMP says, so a
			// trace-only run still produces one -- with FRAME lines and nothing else.)
			//
			// fresh=1 means resolve_world_camera ACCEPTED a camera for the window that follows,
			// rather than leaving the previous one standing. It is the direct test of the
			// stale-hold story: on the 2026-08-15 run accept climbed by the full frame count, so
			// this is expected to read 1 on essentially every line -- and a period-3 pattern in it
			// would overturn that, which is the only reason it is printed.
			// sol= and rings= are the census: DISTINCT NORMALISED SOLVERS and distinct raw ring
			// hashes among this window's world-mode draws. sol=1 with rings=2 is one camera emitted
			// at two column scales; sol>=2 is a genuine multi-camera window. ringN above counts
			// draws, not cameras, and cannot tell those apart -- which is why these two exist.
			dump_write(fmt::format(
				"FRAME f={} sub={} used={:016X} age={} ring1={:016X} ringN={} ring2={:016X} "
				"miss={} draws={} sol={} rings={} cand={} next={:016X} fresh={}",
				s_frame_counter, s_submitted_this_frame, trace_used_hash, trace_used_age,
				s_frame_trace.first_hash, s_frame_trace.not_first, s_frame_trace.second_hash,
				s_frame_trace.misses, s_frame_trace.draws, trace_census_solvers, trace_census_rings,
				s_stats.cam_last_candidates, s_active_camera.matrix_hash,
				(s_camera_last_accept_frame == s_frame_counter && s_active_camera.valid) ? 1 : 0));
		}

		s_frame_trace = frame_trace_state{};

		// PCSX2_REMIX_WORLDPROBE, armed on exactly the shape the window trace above uses: a window
		// that submitted something with a world camera live, then WORLDPROBEAFTER of those counted
		// off first so the measurement lands in gameplay rather than on mission start. See
		// worldprobe_after_frames() for the two diagnostics this session that armed at frame one
		// and measured the loading phase.
		//
		// The arming window itself emits nothing -- the draw-side sampler was off while its draws
		// were built, so it has no samples to report. The windows after it SURVEY: they pick the
		// hashes to follow and are deliberately not emitted, both because the survey spans several
		// windows (so it has no single window's centroid to report) and because the tracked slots
		// do not exist yet.
		if (!s_worldprobe_started)
		{
			if (worldprobe_frames() > 0 && s_submitted_this_frame > 0 && probe_camera.valid)
			{
				if (s_worldprobe_skipped < worldprobe_after_frames())
				{
					++s_worldprobe_skipped;
				}
				else
				{
					s_worldprobe_started = true;
					s_worldprobe_left = worldprobe_frames();

					// The complement of the selection line: if the survey never finds a window
					// with world geometry in it, THIS is the only evidence that the knob was ever
					// applied, and its absence says the arming condition itself never fired.
					if (!s_worldprobe_logged_arm)
					{
						s_worldprobe_logged_arm = true;
						INFO_LOG("Remix: WORLDPROBE armed at frame {} for {} window(s) after "
								 "skipping {} -- surveying up to {} non-empty windows to choose "
								 "the draws to follow; a 'WORLDPROBE selected' line must follow "
								 "this one or nothing was measured",
							s_frame_counter, s_worldprobe_left, s_worldprobe_skipped,
							worldprobe_survey_windows);
					}
				}
			}
		}
		else if (s_worldprobe_left > 0)
		{
			if (!s_worldprobe_selected)
			{
				// SELECTION RETRIES, and the budget is deliberately untouched until it lands.
				//
				// MEASURED 2026-08-15: selecting once, on the arming window, produced 1,417 lines
				// of `slots=0` and a wasted capture. This title under PCSX2_REMIX_HOLDEMPTY = 2
				// submits nothing at all in one presented window out of three (57 of 171, gap
				// exactly 3 in 56 of 56), so a one-shot choice had a 1-in-3 chance of freezing an
				// empty set for the whole run -- and it fired. An empty window now simply costs
				// another look.
				//
				// Not spending the budget here matters for the same reason: a survey that takes a
				// dozen windows must not eat a dozen lines out of the capture the user is standing
				// there performing.
				worldprobe_survey_window();
			}
			else
			{
				--s_worldprobe_left;
				worldprobe_emit(probe_camera);
			}
		}

		worldprobe_reset_frame();

		// The per-draw dump follows the frames that actually submitted something, so it never
		// burns its budget on the loading screens before the mission starts.
		if (s_submitted_this_frame > 0 &&
			(!drawdump_world_only() || (s_drawdump_world_armed && s_active_camera.valid)))
		{
			// ...but "first frames that submit" is still mission start, not gameplay. Count those
			// qualifying frames off first so DRAWDUMPAFTER can put the dump where the player
			// actually is -- see drawdump_after_frames() for what that cost us once already.
			if (!s_drawdump_started && s_drawdump_skipped < drawdump_after_frames())
			{
				++s_drawdump_skipped;
			}
			else if (!s_drawdump_started)
			{
				s_drawdump_started = true;
				s_drawdump_frames_left = drawdump_frames();
			}
			else if (s_drawdump_frames_left > 0)
			{
				--s_drawdump_frames_left;
			}
		}

		s_stats.meshes_created_peak = std::max(s_stats.meshes_created_peak, s_stats.meshes_created_frame);
		s_stats.meshes_created_frame = 0;
		s_stats.meshes_destroyed_peak = std::max(s_stats.meshes_destroyed_peak, s_stats.meshes_destroyed_frame);
		s_stats.meshes_destroyed_frame = 0;

		if (!s_frame_instanced_keys.empty())
		{
			const u64 distinct = s_frame_instanced_keys.size();
			s_stats.distinct_instanced_peak = std::max(s_stats.distinct_instanced_peak, distinct);
			s_stats.distinct_instanced_total += distinct;
			++s_stats.distinct_instanced_frames;
		}

		++s_frame_counter;
		s_submitted_this_frame = 0;
		s_frame_submitted_hashes.clear();
		s_frame_geometry_hashes.clear();
		s_frame_instanced_keys.clear();
		// Counted whether or not batching is enabled: this is the number that decides whether
		// batching can reach the operating point the dose-response calls safe, and it has to be
		// answerable without turning the feature on.
		if (!s_frame_group_keys.empty())
		{
			const u64 groups = s_frame_group_keys.size();
			s_stats.batch_groups_peak = std::max(s_stats.batch_groups_peak, groups);
			s_stats.batch_groups_total += groups;
			++s_stats.batch_frames;
		}

		s_frame_group_keys.clear();

		batch_reap();

		// Entries a draw claimed but could not fit. Dropped here, at the frame boundary, so the
		// handle is only destroyed once nothing in flight references it; the next frame's draw
		// finds the slot empty and rebuilds it from current geometry.
		for (auto it = s_meshes.begin(); it != s_meshes.end();)
		{
			if (!it->second.rebuild_pending)
			{
				++it;
				continue;
			}

			if (it->second.handle)
			{
				remix_ps2::guarded_destroy_mesh(s_remix.api().DestroyMesh, it->second.handle);
				++s_stats.meshes_destroyed;
				++s_stats.meshes_destroyed_frame;
			}

			it = s_meshes.erase(it);
		}

		// Meshes first, then materials: a live mesh holds the material handle Remix bound into
		// it at CreateMesh time, so releasing a material before the meshes referencing it would
		// leave the runtime holding a dead handle.
		reap_idle_meshes();
		remix_ps2::materials::reap(s_remix, s_frame_counter);
		remix_ps2::materials::begin_frame();

		// Picks up tags the user saved from the developer menu without an emulator restart.
		remix_ps2::materials::refresh_categories();

		// Per-game settings. Ordered after refresh_categories only for log readability -- both
		// poll independently, and this one also re-applies when the running game changes.
		remix_ps2::materials::refresh_game_config(s_remix);

		// Settings-page knobs the backend re-reads. Ordered AFTER refresh_game_config on purpose:
		// that one applies the per-game .conf, and a title's .conf should outrank the GUI's
		// global values the same way it outranks everything else.
		remix_ps2::paths::apply_live_knobs();

		// The sky far-distance requirement. Re-read here, once per presented frame, rather than in
		// classify_sky itself: that one is the per-draw path and cannot afford a
		// GetEnvironmentVariableW, while a value latched at first call would be the pre-conf value
		// forever (the renderer goes live ~0.2 s before the per-game .conf is applied). Placed
		// after both config layers above so a value either of them just delivered is in force for
		// the next frame's draws. See sky_min_w().
		refresh_sky_min_w();

		// A knob that changes the GEOMETRY has to invalidate the mesh cache, or it only takes
		// effect on meshes created after it -- which is what turning the crease angle on looked
		// like in practice: the arm stayed faceted until enough turning evicted and rebuilt it.
		//
		// Deliberately keyed on the generation rather than on which knob moved. The cache holds
		// vertex data, so every knob that feeds vertex data is in scope, and enumerating them
		// here is a list that would silently fall out of date. Knob changes are user-driven and
		// rare, so a full flush costs one frame of rebuilds and cannot accumulate.
		if (const u64 generation = remix_ps2::paths::knob_generation(); generation != s_knob_generation_seen)
		{
			const bool first = (s_knob_generation_seen == ~0ull);
			s_knob_generation_seen = generation;

			// Not on the first observation: at that point the cache is empty and the generation
			// is simply catching up with the writes apply_before_runtime_load already made.
			if (!first)
			{
				const auto& api = s_remix.api();
				for (auto& [key, entry] : s_meshes)
				{
					if (entry.handle)
					{
						remix_ps2::guarded_destroy_mesh(api.DestroyMesh, entry.handle);
						++s_stats.meshes_destroyed;
						++s_stats.meshes_destroyed_frame;
					}
				}

				if (!s_meshes.empty())
					INFO_LOG("Remix: settings changed, rebuilding {} cached meshes", s_meshes.size());

				s_meshes.clear();
			}
		}

		log_stats(false);
	}

	void OnGSStateLoaded()
	{
		if (!armed())
			return;

		// The VU1 candidates first, so nothing can latch a pre-load matrix in between.
		RemixVU1Capture::DropPublished();
		RemixVU1Capture::SetPinnedOffset(0xFFFFFFFFu);

		// A state load can switch title, and the per-game config poll would otherwise leave up
		// to a second of frames running the previous game's settings.
		remix_ps2::materials::invalidate_game_config();

		// Back to the fallback camera until the back-slice republishes. Holding the old one
		// for even the three frames s_camera_hold_frames allows would submit the new scene
		// through the old world transform.
		s_active_camera = world_camera{};
		s_camera_last_accept_frame = 0;
		s_logged_world_camera = false;

		// Same reason as the camera above, and the same failure if it is skipped: a cached
		// per-draw solver describes a world the load just replaced, and the cache is keyed on
		// matrix CONTENT, so a hash that recurs after the load would place the new scene's draws
		// through the old scene's transform.
		for (per_draw_camera_entry& entry : s_per_draw_cameras)
			entry = per_draw_camera_entry{};
		s_kick_camera_memo = kick_camera_memo{};
		s_window_cameras = window_camera_census{};

		s_frame_viewport = viewport_constants{};
		s_last_viewport = viewport_constants{};
		s_frame_bounds = scene_bounds{};
		s_last_bounds = scene_bounds{};
		s_frame_bounds_world = false;
		s_last_bounds_world = false;
		s_frame_max_w = 0.f;
		s_frame_max_extent = 0.f;
		s_frame_min_extent = std::numeric_limits<float>::max();
		s_refuted_matrices.clear();
		s_light_placed = false;
		s_sun_placed = false;
		s_drawdump_started = false;
		s_drawdump_frames_left = 0;
		s_drawdump_skipped = 0;
		s_drawdump_world_armed = false;

		// Same reasoning as the per-draw dump above: the window trace's budget has to be spent on
		// the world the user just loaded into, not on the one the state replaced.
		s_frametrace_started = false;
		s_frametrace_left = 0;
		s_frametrace_skipped = 0;
		s_frame_trace = frame_trace_state{};
		s_frametrace_cam_hash = 0;
		s_frametrace_cam_since = s_frame_counter;

		// And the world probe, for a reason stronger than budget: its four tracked hashes name
		// geometry in the world the load just replaced. Following them across a state load would
		// either report ABSENT forever or, worse, re-acquire the same texture on different level
		// pieces and compare two unrelated objects.
		worldprobe_reset_all();
		worldfix_reset_all();
		camtest_reset_all();

		// The beacon must not fire just because the first frames after a load submit nothing.
		s_empty_frame_streak = 0;

		if (!s_live || !s_remix.ok())
			return;

		// Drop the mesh cache: every hash in it describes geometry from a world that no longer
		// exists, so keeping it only delays the eviction and holds acceleration structures the
		// scene will never reference again. The runtime itself is deliberately left up -- a
		// second Startup after Shutdown is unproven in dxvk-remix and the window-loss guard
		// depends on the singleton staying alive.
		const remixapi_Interface& api = s_remix.api();
		if (s_debug_light)
		{
			remix_ps2::guarded_destroy_light(api.DestroyLight, s_debug_light);
			s_debug_light = nullptr;
		}

		if (s_sun_light)
		{
			remix_ps2::guarded_destroy_light(api.DestroyLight, s_sun_light);
			s_sun_light = nullptr;
		}

		if (s_dome_light)
		{
			remix_ps2::guarded_destroy_light(api.DestroyLight, s_dome_light);
			s_dome_light = nullptr;
		}

		// The fill's rebuild check compares against the parameters the EXISTING lights were built
		// from, so tearing the lights down without clearing it would leave the next resolve reading
		// "nothing changed" and rebuilding nothing -- an unlit scene with nothing in the log to
		// explain it.
		s_fill_params = fill_light_params{};
		s_fill_params_resolved = false;

		const size_t dropped = s_meshes.size();

		for (const auto& [hash, entry] : s_meshes)
		{
			if (entry.handle)
			{
				remix_ps2::guarded_destroy_mesh(api.DestroyMesh, entry.handle);
				++s_stats.meshes_destroyed;
			}
		}

		s_meshes.clear();
		s_poisoned.clear();
		s_frame_submitted_hashes.clear();
		s_frame_geometry_hashes.clear();
		s_frame_instanced_keys.clear();
		s_frame_group_keys.clear();
		batch_discard();
		s_reuse_handle = nullptr;
		s_pool_hashes.clear();
		s_pinned_hashes.clear();

		// Keep ordinary materials, but discard render-target snapshots: their pixels belong to
		// the state that was replaced. The material cache suppresses readback for the first post-load
		// frame so GS local memory has time to settle.
		remix_ps2::materials::begin_frame();
		remix_ps2::materials::on_state_loaded(s_remix);

		INFO_LOG("Remix: save state loaded -- dropped {} meshes, camera back to fallback until "
				 "the back-slice republishes",
			dropped);
	}

	void OnGSClose()
	{
		// Unconditional, and before the runtime check: the scan must stop costing the VU
		// thread work even when the runtime never came up.
		RemixVU1Capture::SetArmed(false);

		s_active_camera = world_camera{};
		s_frame_viewport = viewport_constants{};
		s_last_viewport = viewport_constants{};
		s_frame_bounds = scene_bounds{};
		s_last_bounds = scene_bounds{};
		s_frame_bounds_world = false;
		s_last_bounds_world = false;
		s_frame_max_w = 0.f;
		s_frame_max_extent = 0.f;
		s_frame_min_extent = std::numeric_limits<float>::max();
		s_refuted_matrices.clear();
		for (per_draw_camera_entry& entry : s_per_draw_cameras)
			entry = per_draw_camera_entry{};
		s_kick_camera_memo = kick_camera_memo{};
		s_window_cameras = window_camera_census{};
		s_camera_last_accept_frame = 0;
		s_logged_world_camera = false;
		s_logged_sky_camera = false;
		s_logged_view_model_camera = false;
		s_light_placed = false;
		s_sun_placed = false;
		s_drawdump_started = false;
		s_drawdump_frames_left = 0;
		s_drawdump_skipped = 0;
		s_drawdump_world_armed = false;
		s_frametrace_started = false;
		s_frametrace_left = 0;
		s_frametrace_skipped = 0;
		s_frame_trace = frame_trace_state{};
		s_frametrace_cam_hash = 0;
		// Paired with the s_frame_counter = 0 at the end of this function.
		s_frametrace_cam_since = 0;
		worldprobe_reset_all();
		worldfix_reset_all();
		camtest_reset_all();
		s_logged_kick_cam_count = 0;
		s_stable_frames = 0;
		s_last_startup_rect = RECT{};

		if (!s_remix.ok())
			return;

		const remixapi_Interface& api = s_remix.api();

		for (const auto& [hash, entry] : s_meshes)
		{
			if (entry.handle)
				remix_ps2::guarded_destroy_mesh(api.DestroyMesh, entry.handle);
		}

		s_meshes.clear();
		s_poisoned.clear();
		s_frame_submitted_hashes.clear();
		s_frame_geometry_hashes.clear();
		s_frame_instanced_keys.clear();
		s_frame_group_keys.clear();
		batch_discard();
		s_reuse_handle = nullptr;
		s_pool_hashes.clear();
		s_pinned_hashes.clear();

		if (s_debug_mesh)
		{
			remix_ps2::guarded_destroy_mesh(api.DestroyMesh, s_debug_mesh);
			s_debug_mesh = nullptr;
		}

		if (s_debug_light)
		{
			remix_ps2::guarded_destroy_light(api.DestroyLight, s_debug_light);
			s_debug_light = nullptr;
		}

		if (s_sun_light)
		{
			remix_ps2::guarded_destroy_light(api.DestroyLight, s_sun_light);
			s_sun_light = nullptr;
		}

		if (s_dome_light)
		{
			remix_ps2::guarded_destroy_light(api.DestroyLight, s_dome_light);
			s_dome_light = nullptr;
		}

		// Same reason as the close path: the rebuild check must not describe lights that no longer
		// exist, or the next session's first resolve reads "nothing changed" and builds nothing.
		s_fill_params = fill_light_params{};
		s_fill_params_resolved = false;

		// One last counter block, so a short session still reports what it saw. Emitted before
		// the material cache is torn down, because its counters are half the answer.
		log_stats(true);

		// After the meshes above: they hold the material handles.
		remix_ps2::materials::destroy_all(s_remix);

		// The runtime itself is deliberately NOT shut down: it is a process-lifetime singleton
		// because a second Startup after Shutdown is unproven in dxvk-remix. Clearing
		// s_init_attempted makes the next open rebuild only the scene -- runtime::initialize()
		// short-circuits to true once it is already started.
		s_live = false;
		s_init_attempted = false;
		s_frame_counter = 0;
		s_submitted_this_frame = 0;
		s_stats = {};
	}
} // namespace RemixSubmit
