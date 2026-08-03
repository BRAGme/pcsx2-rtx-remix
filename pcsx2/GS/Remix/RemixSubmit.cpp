// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixSubmit.h"
#include "GS/Remix/RemixMaterials.h"
#include "GS/Remix/RemixRuntime.h"
#include "GS/Remix/RemixTransforms.h"
#include "GS/Remix/RemixVU1Capture.h"

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
			u64 skip_const_q = 0; // m_vt.m_eq.q: one Q for the whole draw => 2D/HUD
			u64 warn_inaccurate_stq = 0; // m_vt.m_accurate_stq: Q precision already suspect
			u64 skip_no_target = 0; // no colour target, so no viewport to un-project against
			u64 skip_empty = 0;
			u64 skip_too_large = 0;
			u64 skip_nonfinite = 0; // Q guard values poisoned the un-projected positions
			u64 skip_poisoned = 0; // hash quarantined by an earlier faulting runtime call
			u64 skip_mesh_budget = 0; // over the per-frame CreateMesh budget
			u64 skip_fbmsk = 0; // partial colour write mask: a multi-pass modulation term
			u64 skip_coincident = 0; // identical geometry already submitted this frame
			u64 skip_minw = 0; // draw sits at or inside the eye
			u64 skip_maxw = 0; // draw sits entirely beyond the far-field cap (PCSX2_REMIX_MAXW)
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
			u64 degenerate_triangles = 0; // zero-area triangles dropped before CreateMesh
			u64 skip_all_degenerate = 0; // draws where every triangle was degenerate
			u64 cam_world = 0;
			u64 cam_fallback = 0;
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
			u64 cam_accept_sliced = 0; // accepted cameras that came from the back-slice
			u64 cam_candidates = 0; // offered to the GS side, summed over frames
			u64 cam_reject_split = 0; // split_view_projection_direct refused
			u64 cam_reject_score = 0; // split worked, score_perspective refused
			u64 cam_accept = 0;
			u64 cam_reject_degenerate = 0; // split and scored, but refuted by the geometry
			u64 cam_reject_scale = 0; // the un-projection's unit disagrees with the guest's w
			u64 cam_reject_extent = 0; // world space changed the scene's size, so it is not rigid
			u32 cam_last_candidates = 0;
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
		u64 s_submitted_this_frame = 0;

		// Consecutive frames that submitted nothing, and how many of them the beacon waits for.
		// ~2 seconds at 60Hz: long enough that a load or an all-2D stretch never triggers it.
		u64 s_empty_frame_streak = 0;
		constexpr u64 s_beacon_after_empty_frames = 120;
		stat_counters s_stats{};

		std::unordered_map<u64, mesh_entry> s_meshes;
		std::unordered_set<u64> s_poisoned;

		// Mesh hashes already submitted this frame, cleared at each VSync. See the dedupe gate.
		std::unordered_set<u64> s_frame_submitted_hashes;

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
		std::vector<remixapi_MeshInfoSurfaceTriangles> s_batch_surface_scratch;

		// Distinct (blend state, category) groups seen this frame, counted even when batching is
		// off so the feasibility question can be answered without turning it on.
		std::unordered_set<u64> s_frame_group_keys;

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

		//   0 = no lights at all
		//   1 = distance-independent fill: dome ambient + distant key. The default.
		//   2 = the legacy camera-attached sphere light, kept for comparison
		int light_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_LIGHTMODE", 1), 0, 2));
			return value;
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
			static const float value = env_float(L"PCSX2_REMIX_AMBIENT", 0.f);
			return value;
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
			static const float value = env_float(L"PCSX2_REMIX_KEY", 100.f);
			return value;
		}

		float key_angle_degrees()
		{
			static const float value = env_float(L"PCSX2_REMIX_KEYANGLE", 8.f);
			return value;
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

		// Whether s_last_bounds was captured while world-anchored. Without this the drift guard
		// cannot tell world-space bounds from view-space ones, and comparing a world-space eye
		// against view-space bounds is a units error -- the bug that locked Rainbow Six 3 out of
		// world mode and, once "fixed" by restricting when the guard runs, let SOCOM's bad camera
		// through unchallenged.
		bool s_frame_bounds_world = false;
		bool s_last_bounds_world = false;

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
			light_info.hash = 0x3;
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
			light_info.hash = 0x4;
			light_info.radiance = {radiance, radiance, radiance};

			const u32 status = remix_ps2::guarded_create_light(api.CreateLight, &light_info, &s_sun_light);
			if (status != REMIXAPI_ERROR_CODE_SUCCESS || !s_sun_light)
			{
				ERROR_LOG("Remix: CreateLight failed for the distant light ({})", remix_ps2::error_name(status));
				s_sun_light = nullptr;
			}
		}

		// The distance-independent fill. Created once, never moved, never rescaled: a dome and a
		// distant light have no position and no falloff, so there is nothing about them that
		// depends on the scene's size or on where the camera is.
		void place_fill_lights()
		{
			if (no_debug_scene() || light_mode() != 1)
				return;

			const remixapi_Interface& api = s_remix.api();

			if (!s_dome_light && ambient_radiance() > 0.f)
			{
				remixapi_LightInfoDomeEXT dome{};
				dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
				dome.pNext = nullptr;
				dome.transform = s_identity_transform;
				dome.colorTexture = nullptr; // flat radiance rather than an environment map

				const float radiance = ambient_radiance();
				remixapi_LightInfo light_info{};
				light_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
				light_info.pNext = &dome;
				light_info.hash = 0x5;
				light_info.radiance = {radiance, radiance, radiance};

				const u32 status = remix_ps2::guarded_create_light(api.CreateLight, &light_info, &s_dome_light);
				if (status != REMIXAPI_ERROR_CODE_SUCCESS || !s_dome_light)
				{
					ERROR_LOG("Remix: CreateLight failed for the dome light ({})", remix_ps2::error_name(status));
					s_dome_light = nullptr;
				}
			}

			if (!s_sun_light && key_radiance() > 0.f)
			{
				// Fixed direction, deliberately off-axis on all three so no wall, floor or
				// ceiling is lit flat-on and surface orientation stays readable.
				const float direction[3] = {0.35f, -0.86f, 0.37f};

				remixapi_LightInfoDistantEXT distant{};
				distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
				distant.pNext = nullptr;
				distant.direction = {direction[0], direction[1], direction[2]};
				distant.angularDiameterDegrees = key_angle_degrees();
				distant.volumetricRadianceScale = 1.f;

				const float radiance = key_radiance();
				remixapi_LightInfo light_info{};
				light_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
				light_info.pNext = &distant;
				light_info.hash = 0x6;
				light_info.radiance = {radiance, radiance, radiance};

				const u32 status = remix_ps2::guarded_create_light(api.CreateLight, &light_info, &s_sun_light);
				if (status != REMIXAPI_ERROR_CODE_SUCCESS || !s_sun_light)
				{
					ERROR_LOG("Remix: CreateLight failed for the key light ({})", remix_ps2::error_name(status));
					s_sun_light = nullptr;
				}
				else
				{
					s_sun_placed = true; // stop place_sun_light from replacing it
				}
			}

			INFO_LOG("Remix: scene lighting -- distance-independent fill (key {:g} at {:g} deg, dome {:g}). "
					 "No 1/d^2 falloff, so it is scale-free: the same numbers work at scene radius 4 and "
					 "11,785. PCSX2_REMIX_KEY / KEYANGLE / AMBIENT tune it, LIGHTMODE=2 restores the "
					 "camera-attached sphere.",
				key_radiance(), key_angle_degrees(), ambient_radiance());
		}

		bool create_debug_scene()
		{
			const remixapi_Interface& api = s_remix.api();

			// Scene lighting. The default is the fill above; mode 2 is the old camera-attached
			// sphere light, straight from the official remixapi_example_c.c, whose scene radius
			// is a placeholder until the first frame measures one.
			place_fill_lights();

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
			const bool lights_requested = !no_debug_scene() && light_mode() != 0;
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

			remix_ps2::guarded_setup_camera(api.SetupCamera, &camera_info);
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

		void submit_camera()
		{
			// The extent the previous frame actually submitted, in whichever space is in use.
			// Both tiers get the same treatment: view-space positions are in guest eye-depth
			// units, which are just as far from "near 0.1" as world-space ones are.
			const float scene_radius = s_last_bounds.radius();

			if (!s_active_camera.valid)
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
			remix_ps2::to_camera_matrix(s_active_camera.view, camera_info.view);
			remix_ps2::to_camera_matrix(s_active_camera.projection, camera_info.projection);

			remix_ps2::guarded_setup_camera(s_remix.api().SetupCamera, &camera_info);
			++s_stats.cam_world;

			if (light_mode() == 2)
			{
				// The sphere light rides the camera, as in RPCS3: a world-space scene lit from
				// wherever the origin happens to be is usually a black scene. This is also what
				// blows out the first-person weapon, which is why it is no longer the default.
				place_debug_light(s_active_camera.position, scene_radius);

				// Camera forward in world space. p_view = p_world * V (row-vector), so the
				// gradient of view z with respect to world position is V's third column.
				const float forward[3] = {
					s_active_camera.view.m[0][2], s_active_camera.view.m[1][2], s_active_camera.view.m[2][2]};
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

		// Latches the VU thread's candidates and turns the best one into a world camera.
		// Runs after Present, so the camera resolved here is the one both the next frame's
		// draws and the next frame's SetupCamera use -- geometry and camera always reference
		// the same matrix (RPCS3's one-frame latch).
		void resolve_world_camera()
		{
			RemixVU1Capture::Frame frame{};
			const bool have = RemixVU1Capture::Latch(frame);

			// The producer is on another thread behind a seqlock; never let its count index
			// this side's loop without a bound of our own.
			frame.count = std::min(frame.count, RemixVU1Capture::max_candidates);

			s_stats.vu_kicks += frame.kicks_seen;
			s_stats.vu_kicks_scanned += frame.kicks_scanned;
			s_stats.vu_windows += frame.windows_examined;
			s_stats.vu_survivors += frame.windows_survived;
			s_stats.vu_reentrant += frame.kicks_reentrant;
			s_stats.vu_sliced += frame.sliced_matrices;
			s_stats.vu_sliced_published += frame.sliced_published;
			s_stats.cam_candidates += frame.count;
			s_stats.cam_last_candidates = frame.count;

			const viewport_constants vp = s_last_viewport;

			if (remix_ps2::nocam_enabled() || !have || frame.count == 0 || !vp.valid)
			{
				drop_stale_camera();
				return;
			}

			const float width = static_cast<float>(vp.width);
			const float height = static_cast<float>(vp.height);
			const float reference_aspect = (height > 0.f) ? (width / height) : (4.f / 3.f);

			// The guest's post-divide output space is not knowable in advance: the fused
			// matrix may already carry the full 12.4 viewport fold, or emit pixels, or emit
			// plain NDC and leave the viewport to a post-divide multiply-add in the VU. Try
			// each, both majorness ways round, and let score_perspective decide. That is a
			// handful of 4x4 inversions per frame.
			struct hypothesis
			{
				const char* name;
				float scale_x;
				float offset_x;
				float scale_y;
				float offset_y;
			};

			const hypothesis hypotheses[] = {
				// GS 12.4 subpixels: the exact inverse of the per-vertex un-projection.
				{"gs", width * 8.f, vp.ofx + (width * 8.f) - 8.f + 0.05f,
					-(height * 8.f), vp.ofy + (height * 8.f) - 8.f + 0.05f},
				// Pixels, origin top-left. XYOFFSET cancels: it is added after this stage.
				{"px", width * 0.5f, (width * 0.5f) - 0.5f, -(height * 0.5f), (height * 0.5f) - 0.5f},
				// Already NDC, +Y up like Remix.
				{"ndc", 1.f, 0.f, 1.f, 0.f},
				// Already NDC, +Y down like the GS.
				{"ndcY", 1.f, 0.f, -1.f, 0.f},
			};

			float best_score = 0.f;
			remix_ps2::vp_split best_split{};
			remix_ps2::mat4 best_normalized = remix_ps2::mat4_identity();
			u64 best_hash = 0;
			u32 best_offset = 0;
			u8 best_source = 0;
			const char* best_name = "";
			bool best_transposed = false;

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
					hypothesis derived[2] = {};
					u32 derived_count = 0;
					{
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

								derived[0] = {"auto", ox, ox, -sy_mag, oy};
								derived[1] = {"autoY", ox, ox, sy_mag, oy};
								derived_count = 2;
							}
						}
					}

					for (u32 h = 0; h < (std::size(hypotheses) + derived_count); ++h)
					{
						const hypothesis& hyp = (h < std::size(hypotheses)) ?
						                            hypotheses[h] :
						                            derived[h - std::size(hypotheses)];

						const remix_ps2::mat4 normalized = remix_ps2::normalize_screen_clip(
							oriented, hyp.scale_x, hyp.offset_x, hyp.scale_y, hyp.offset_y);

						remix_ps2::vp_split split{};
						if (!remix_ps2::split_view_projection_direct(normalized, split))
						{
							++s_stats.cam_reject_split;
							if (dump)
								fmt::format_to(std::back_inserter(detail), " {}/{}=-", hyp.name, major ? 'C' : 'R');

							continue;
						}

						float score = remix_ps2::score_perspective(split.projection, reference_aspect);
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
						if (candidate.source != 0)
							score += 10.f;

						if (score > candidate_best)
						{
							candidate_best = score;
							candidate_name = hyp.name;
							remix_ps2::describe_projection(split.projection, candidate_params);
						}

						if (score > best_score)
						{
							best_score = score;
							best_split = split;
							best_normalized = normalized;
							best_hash = content;
							best_offset = candidate.mem_offset;
							best_source = candidate.source;
							best_name = hyp.name;
							best_transposed = (major != 0);
						}
					}
				}

				if (dump)
				{
					dump_write(fmt::format(
						"f={} src={} off=0x{:04x} pc=0x{:04x} ucode=0x{:016x} shape={:.2f} res=[{} ] best={} "
						"score={:.2f} fovY={:.2f} aspect={:.3f} near={:.5g} M={}",
						s_frame_counter, (candidate.source == 0) ? "scan" : ((candidate.source == 1) ? "slice" : ((candidate.source == 2) ? "slice-tops" : "pinned")),
						candidate.mem_offset, candidate.start_pc, candidate.ucode_hash,
						candidate.score, detail, candidate_name, candidate_best,
						candidate_params.fov_y_degrees, candidate_params.aspect, candidate_params.near_plane,
						remix_ps2::format_matrix(raw)));
				}
			}

			if (!(best_score > 0.f))
			{
				drop_stale_camera();
				return;
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

			s_active_camera = camera;
			s_camera_last_accept_frame = s_frame_counter;
			++s_stats.cam_accept;
			if (best_source != 0)
				++s_stats.cam_accept_sliced;

			// Pin the address the winner came from. A camera actually recovered from an
			// offset is stronger evidence than any shape score, and without it the true
			// matrix has to out-rank thousands of shape-plausible windows every frame.
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
							((best_source == 2) ? "ucode back-slice (TOPS)" : "pinned back-slice address")),
					best_name, best_transposed ? "column-major" : "row-major", best_score,
					params.fov_y_degrees, camera.near_plane, camera.far_plane,
					camera.position[0], camera.position[1], camera.position[2],
					camera.depth_scale, camera.depth_anisotropy,
					described ? params.near_plane : 0.f);
			}
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
		float camera_scale_limit()
		{
			static const float value = env_float(L"PCSX2_REMIX_CAMSCALE", 8.f);
			return value;
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
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_TEXSTAGE", 1), 0, 1));
			return value;
		}

		int alpha_state_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(remix_ps2::read_env_int(L"PCSX2_REMIX_ALPHASTATE", 2), 0, 2));
			return value;
		}

		// PS2 ATST -> D3D9 D3DCMPFUNC, which is the vocabulary the D3D9-shaped Remix API uses.
		// GS_ATST: NEVER 0, ALWAYS 1, LESS 2, LEQUAL 3, EQUAL 4, GEQUAL 5, GREATER 6, NOTEQUAL 7.
		u32 to_d3d_compare(u32 atst)
		{
			switch (atst)
			{
				case 0: return 1; // NEVER
				case 1: return 8; // ALWAYS
				case 2: return 2; // LESS
				case 3: return 4; // LESSEQUAL
				case 4: return 3; // EQUAL
				case 5: return 7; // GREATEREQUAL
				case 6: return 5; // GREATER
				default: return 6; // NOTEQUAL
			}
		}

		// The GS blend equation is (A - B) * C + D with A/B/D in {Cs, Cd, 0} and C in
		// {As, Ad, FIX}. D3D9's fixed src*srcFactor + dst*dstFactor cannot express all of it, so
		// this covers the cases that actually occur and falls back to a plain alpha blend --
		// stated plainly because it is an approximation, not a translation.
		void to_d3d_blend(const GIFRegALPHA& alpha, u32& src_factor, u32& dst_factor)
		{
			// D3DBLEND: ZERO 1, ONE 2, SRCALPHA 5, INVSRCALPHA 6, DESTALPHA 7, INVDESTALPHA 8.
			const u32 c_factor = (alpha.C == 0) ? 5u : (alpha.C == 1) ? 7u : 2u; // As / Ad / FIX~ONE
			const u32 inv_c_factor = (alpha.C == 0) ? 6u : (alpha.C == 1) ? 8u : 1u;

			if (alpha.A == 0 && alpha.B == 1 && alpha.D == 1)
			{
				// Cs*C + Cd*(1-C): the standard blend.
				src_factor = c_factor;
				dst_factor = inv_c_factor;
			}
			else if (alpha.A == 0 && alpha.B == 2 && alpha.D == 1)
			{
				// Cs*C + Cd: additive.
				src_factor = c_factor;
				dst_factor = 2u; // ONE
			}
			else if (alpha.A == 0 && alpha.B == 2 && alpha.D == 2)
			{
				// Cs*C: modulate against nothing.
				src_factor = c_factor;
				dst_factor = 1u; // ZERO
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
		// PCSX2_REMIX_SKY:      0 = off, 1 = depth-neutral (default).
		// PCSX2_REMIX_SKYORDER: when > 0, additionally require the draw to be among the first N
		//                       gate-passing draws of the frame.
		int sky_mode()
		{
			static const int value = static_cast<int>(remix_ps2::read_env_int(L"PCSX2_REMIX_SKY", 1));
			return value;
		}

		u32 sky_order_limit()
		{
			static const u32 value =
				static_cast<u32>(std::max<s64>(0, remix_ps2::read_env_int(L"PCSX2_REMIX_SKYORDER", 0)));
			return value;
		}

		// 'depth_read'/'depth_write' come from GSRendererHW's own DepthRead()/DepthWrite()
		// (GSRendererHW.h:62-79), evaluated by the caller because only OnDrawPrims is a friend
		// of GSRendererHW. Reading the registers here instead would be a second, drifting
		// interpretation of the same state.
		bool classify_sky(bool depth_read, bool depth_write, u64 draw_ordinal)
		{
			if (sky_mode() == 0)
				return false;

			if (depth_read || depth_write)
				return false;

			const u32 limit = sky_order_limit();
			return (limit == 0) || (draw_ordinal < limit);
		}

		// --- per-draw state dump ------------------------------------------------------------
		//
		// PCSX2_REMIX_DRAWDUMP=N dumps every gate-passing draw for the first N frames that
		// submitted anything, to logs\remix_draws.txt. This exists so the sky rule above is
		// derived from what the title actually does rather than picked and hoped for.
		u64 drawdump_frames()
		{
			static const u64 value =
				static_cast<u64>(std::max<s64>(0, remix_ps2::read_env_int(L"PCSX2_REMIX_DRAWDUMP", 0)));
			return value;
		}

		u64 s_drawdump_frames_left = 0;
		bool s_drawdump_started = false;

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

		draw_state build_draw_state(const draw_regs& regs, u64 material_hash, u64 draw_ordinal)
		{
			const bool depth_read = regs.depth_read;
			const bool depth_write = regs.depth_write;
			const bool is_sky = classify_sky(depth_read, depth_write, draw_ordinal);

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
			blend.colorBlendOp = 1; // D3DBLENDOP_ADD
			blend.srcAlphaBlendFactor = blend.srcColorBlendFactor;
			blend.dstAlphaBlendFactor = blend.dstColorBlendFactor;
			blend.alphaBlendOp = 1;
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
				blend.textureColorArg1Source = 2; // D3DTA_TEXTURE
				blend.textureColorArg2Source = 0; // D3DTA_DIFFUSE
				blend.textureColorOperation = decal ? 2u : 4u; // SELECTARG1 : MODULATE
				blend.textureAlphaArg1Source = 2;
				blend.textureAlphaArg2Source = 0;
				blend.textureAlphaOperation = decal ? 2u : 4u;
			}
			blend.tFactor = 0xFFFFFFFFu;
			blend.isTextureFactorBlend = 0;
			blend.writeMask = 0xF; // D3DCOLORWRITEENABLE_ALL
			blend.isVertexColorBakedLighting = 0;

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
			out.categories = tagged |
			                 (is_sky ? static_cast<u32>(REMIXAPI_INSTANCE_CATEGORY_BIT_SKY) : 0u) |
			                 (is_cutout ? static_cast<u32>(REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT) : 0u);
			return out;
		}

		// Appends the draw currently in the scratch buffers to its batch group, under the surface
		// for its material. Indices are rebased onto the surface's running vertex count.
		//
		// Vertices go in exactly as they were built -- world or view space, whichever the frame is
		// submitting in -- so the batch instance carries the identity transform and there is no
		// registration to get wrong. Batching and stable identity are alternatives, not partners.
		void batch_append(u64 group_key, const draw_state& ds, const remix_ps2::materials::binding& material)
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
				s_batch_group_of_key.emplace(group_key, group_index);
			}

			batch_group& group = s_batch_groups[group_index];

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
			surface.indices.reserve(surface.indices.size() + s_scratch_indices.size());

			for (const u32 index : s_scratch_indices)
				surface.indices.push_back(base + index);
		}

		// Turns the frame's accumulated groups into meshes and instances them. Must run before
		// Present, and is the whole point of batching: one CreateMesh and one DrawInstance per
		// group, however many draws went into it.
		void batch_flush()
		{
			if (s_batch_groups_used == 0)
			{
				s_batch_group_of_key.clear();
				return;
			}

			const remixapi_Interface& api = s_remix.api();
			u64 vertices_this_frame = 0;
			u64 surfaces_this_frame = 0;

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
				mesh_info.hash = (hash == 0) ? 1 : hash;
				mesh_info.surfaces_values = s_batch_surface_scratch.data();
				mesh_info.surfaces_count = s_batch_surface_scratch.size();

				remixapi_MeshHandle handle = nullptr;
				const u32 status = remix_ps2::guarded_create_mesh(api.CreateMesh, &mesh_info, &handle);

				if (status != REMIXAPI_ERROR_CODE_SUCCESS || !handle)
				{
					ERROR_LOG("Remix: batch CreateMesh failed for group {} ({} surfaces): {}",
						g, s_batch_surface_scratch.size(), remix_ps2::error_name(status));
					continue;
				}

				++s_stats.meshes_created;
				++s_stats.meshes_created_frame;
				++s_stats.batch_meshes_created;
				s_batch_meshes.push_back(batch_mesh{handle, s_frame_counter});

				remixapi_InstanceInfo instance{};
				instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
				instance.pNext = (alpha_state_mode() == 2) ? &group.blend : nullptr;
				instance.categoryFlags = group.categories;
				instance.mesh = handle;
				// The geometry is already in the submitted camera's space.
				instance.transform = s_identity_transform;
				instance.doubleSided = 1;

				remix_ps2::guarded_draw_instance(api.DrawInstance, &instance);
				s_frame_instanced_keys.insert(mesh_info.hash);
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
			}

			s_batch_meshes.clear();
			s_batch_group_of_key.clear();
			s_batch_groups_used = 0;
		}

		// Releases batch meshes whose frame is far enough behind that nothing in flight can still
		// reference them.
		void batch_reap()
		{
			if (s_batch_meshes.empty())
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
		}

		void log_stats(bool force)
		{
			if (!force && (s_frame_counter == 0 || (s_frame_counter % stats_interval_frames()) != 0))
				return;

			INFO_LOG("Remix: frame {} | seen {} submitted {} | meshes live {} (+{} -{}) | "
					 "skip: tri {} untex {} fst {} constq {} notarget {} empty {} large {} "
					 "nonfinite {} poisoned {} meshbudget {} fbmsk {} coincident {} minw {} | "
					 "warn stq {} | cam world {} fallback {} | "
					 "maxpos {:.0f}/{:.0f} | scene r {:.0f} | sky {} cutout {} | degen tris {} alldegen {} | "
					 "mesh/frame peak +{} -{} | instbudget-skip {} | distinct handles/frame avg {} peak {} | "
					 "pinned pool {} | id: mode {} reuse {} create {} rebuild {} probes {} | "
				 "batch: mode {} groups/frame avg {} peak {} | surfaces peak {} verts peak {} meshes {}",
				s_frame_counter, s_stats.draws_seen, s_stats.draws_submitted, s_meshes.size(),
				s_stats.meshes_created, s_stats.meshes_destroyed,
				s_stats.skip_not_triangle, s_stats.skip_untextured, s_stats.skip_fst,
				s_stats.skip_const_q, s_stats.skip_no_target, s_stats.skip_empty,
				s_stats.skip_too_large, s_stats.skip_nonfinite, s_stats.skip_poisoned,
				s_stats.skip_mesh_budget, s_stats.skip_fbmsk, s_stats.skip_coincident,
				s_stats.skip_minw,
				s_stats.warn_inaccurate_stq, s_stats.cam_world, s_stats.cam_fallback,
				s_max_seen_position, max_position_magnitude(), s_last_bounds.radius(),
				s_stats.sky_tagged, s_stats.cutout_tagged, s_stats.degenerate_triangles,
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
				s_stats.batch_meshes_created);

			// The w distribution of everything submitted, which is what the min-w gate is set
			// from. A pile in the first buckets is geometry collapsing onto the eye plane.
			INFO_LOG("Remix: submitted w (max per draw): <1e-3 {} <1e-2 {} <0.1 {} <1 {} <10 {} "
					 "<100 {} >=100 {} | minw gate {:g} | maxw gate {:g} skipped {} | "
					 "draw extent ratio avg {:.0f} peak {:.0f}",
				s_stats.w_histogram[0], s_stats.w_histogram[1], s_stats.w_histogram[2],
				s_stats.w_histogram[3], s_stats.w_histogram[4], s_stats.w_histogram[5],
				s_stats.w_histogram[6], min_submitted_w(), max_submitted_w(), s_stats.skip_maxw,
				(s_extent_ratio_frames > 0) ?
					(s_extent_ratio_total / static_cast<double>(s_extent_ratio_frames)) : 0.0,
				s_extent_ratio_peak);

			// Third line: the material bridge. Kept separate so the counter block stays
			// readable, and because the two numbers the user has to act on -- unique content
			// hashes and the per-draw hash cost -- both live here.
			INFO_LOG("{}", remix_ps2::materials::stats_line());

			// Second line: the world anchor. Every stage of the pipeline is separately
			// visible, so a null result names the stage that produced it -- no kicks, no
			// windows through the shape prefilter, no candidates, no split, or no score.
			INFO_LOG("Remix: vu kicks {} scanned {} reentrant {} windows {} shape-ok {} | "
					 "slice matrices {} published {} | cand {} (now {}) | "
					 "split-reject {} score-reject {} degenerate-reject {} scale-reject {} extent-reject {} accept {} (sliced {}) | camera {}{}",
				s_stats.vu_kicks, s_stats.vu_kicks_scanned, s_stats.vu_reentrant, s_stats.vu_windows,
				s_stats.vu_survivors, s_stats.vu_sliced, s_stats.vu_sliced_published,
				s_stats.cam_candidates, s_stats.cam_last_candidates, s_stats.cam_reject_split,
				s_stats.cam_reject_score, s_stats.cam_reject_degenerate, s_stats.cam_reject_scale,
				s_stats.cam_reject_extent, s_stats.cam_accept, s_stats.cam_accept_sliced,
				s_active_camera.valid ? "world score " : "view-space",
				s_active_camera.valid ? fmt::format("{:.2f} near {:.5g}", s_active_camera.score, s_active_camera.near_plane) : "");
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

		if (r.m_vt.m_primclass != GS_TRIANGLE_CLASS)
		{
			++s_stats.skip_not_triangle;
			return;
		}

		// Q only means "the perspective divisor" when the draw is textured with ST/Q. With
		// TME=0 nothing ever wrote a meaningful Q, and with FST=1 the guest fed UVs instead.
		if (!r.m_process_texture)
		{
			++s_stats.skip_untextured;
			return;
		}

		if (r.PRIM->FST)
		{
			++s_stats.skip_fst;
			return;
		}

		// One Q across the whole draw means no perspective, i.e. 2D even when textured.
		if (r.m_vt.m_eq.q)
		{
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
		if ((r.m_cached_ctx.FRAME.FBMSK & 0x00FFFFFFu) != 0)
		{
			++s_stats.skip_fbmsk;
			return;
		}

		if (r.m_vt.m_accurate_stq)
			++s_stats.warn_inaccurate_stq;

		if (rt_unscaled_width <= 0 || rt_unscaled_height <= 0)
		{
			++s_stats.skip_no_target;
			return;
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
		const bool world_mode = s_active_camera.valid;
		const remix_ps2::clip_solver& solver = s_active_camera.solver;
		const float position_limit = max_position_magnitude();

		const GSVertex* const verts = r.m_vertex->buff;

		s_scratch_vertices.clear();
		s_scratch_vertices.resize(vertex_count);

		// Accumulated locally and only merged once the draw is known to be accepted, so a draw
		// that bails out halfway cannot poison the frame's measured extent.
		scene_bounds draw_bounds{};
		float min_w = std::numeric_limits<float>::max();
		float max_w = -std::numeric_limits<float>::max();
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

		for (u32 i = 0; i < vertex_count; ++i)
		{
			const GSVertex& v = verts[i];

			const float ndc_x = ((static_cast<float>(v.XYZ.X) - 0.05f) * sx) - offset_x;
			const float ndc_y = -(((static_cast<float>(v.XYZ.Y) - 0.05f) * sy) - offset_y);

			// w is the eye-space depth the guest divided by. Trusted only because of the
			// TME=1 && FST=0 gate above; the two Q guards (GSState.cpp:1399/:1403) can still
			// leave FLT_MIN or GSVector4::m_max here, which the finite check below rejects.
			const float q = v.RGBAQ.Q;
			const float w = 1.0f / q;

			remixapi_HardcodedVertex& out = s_scratch_vertices[i];

			if (world_mode)
			{
				// clip = (ndc_x*w, ndc_y*w, ., w). The z equation is deliberately not used:
				// a PS2 vertex's GS Z is a raw integer in a per-title convention, and w
				// already carries the absolute depth, so x/y/w is exactly determined.
				float world[3];
				remix_ps2::solve_world_position(solver, ndc_x * w, ndc_y * w, w, world);
				out.position[0] = world[0];
				out.position[1] = world[1];
				out.position[2] = world[2];
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
			out.texcoord[0] = v.ST.S * w;
			out.texcoord[1] = v.ST.T * w;
			// PS2 alpha is 0..128 (0x80 == 1.0); scale into 0..255 for a D3DCOLOR-style ARGB.
			const u32 alpha = std::min<u32>(255u, static_cast<u32>(v.RGBAQ.A) * 2u);
			out.color = (alpha << 24) | (static_cast<u32>(v.RGBAQ.R) << 16) |
			            (static_cast<u32>(v.RGBAQ.G) << 8) | static_cast<u32>(v.RGBAQ.B);

			if (!std::isfinite(out.position[0]) || !std::isfinite(out.position[1]) || !std::isfinite(out.position[2]) ||
				std::abs(out.position[0]) > position_limit ||
				std::abs(out.position[1]) > position_limit ||
				std::abs(out.position[2]) > position_limit ||
				!(w > 0.0f))
			{
				++s_stats.skip_nonfinite;
				return;
			}

			s_max_seen_position = std::max({s_max_seen_position, std::abs(out.position[0]),
				std::abs(out.position[1]), std::abs(out.position[2])});

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
			const float min_w_limit = min_submitted_w();
			if (min_w_limit > 0.f && max_w < min_w_limit)
			{
				++s_stats.skip_minw;
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
			}
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

		if (s_scratch_indices.empty())
		{
			++s_stats.skip_empty;
			return;
		}

		// Flat per-triangle normals from the un-projected edges, AND -- load-bearing -- the
		// point at which zero-area triangles are dropped instead of submitted.
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

		// --- material ------------------------------------------------------------------------
		// Recomputed here rather than read off the Source: only HashCacheEntry* is stored
		// there (GSTextureCache.h:306) and it is null on most draws and always null for a
		// render-target source, so the key has to be rebuilt from TEX0/TEXA/CLUT/region.
		const GSTextureCache::Source* const source = static_cast<const GSTextureCache::Source*>(tex_source);
		const remix_ps2::materials::binding material = remix_ps2::materials::bind(s_remix, source, s_frame_counter);

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
		regs.alpha = r.m_context->ALPHA;

		draw_state ds = build_draw_state(regs, material.content_hash, s_submitted_this_frame);

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

		if (batch_mode() != 0)
		{
			batch_append(group_key, ds, material);

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

		if (s_drawdump_frames_left > 0)
		{
			drawdump_write(fmt::format(
				"f={} d={} verts={} tris={} sky={} | ZTE={} ZTST={} ZMSK={} zpsm=0x{:02x} depth(r={} w={}) | "
				"ATE={} ATST={} AREF={} AFAIL={} ABE={} | fpsm=0x{:02x} fbmsk=0x{:08x} | "
				"tex tbp0=0x{:04x} tbw={} psm=0x{:02x} tw={} th={} tcc={} tfx={} target={} | "
				"w=[{:.1f},{:.1f}] z=[{},{}] | px=[{:.0f},{:.0f}]x[{:.0f},{:.0f}] rt={}x{} | "
				"uv=[{:.3f},{:.3f}]x[{:.3f},{:.3f}] | mat={:016X}",
				s_frame_counter, s_submitted_this_frame, vertex_count, s_scratch_indices.size() / 3,
				ds.is_sky ? 1 : 0,
				static_cast<u32>(r.m_cached_ctx.TEST.ZTE), static_cast<u32>(r.m_cached_ctx.TEST.ZTST),
				static_cast<u32>(r.m_cached_ctx.ZBUF.ZMSK), static_cast<u32>(r.m_cached_ctx.ZBUF.PSM),
				r.m_cached_ctx.DepthRead() ? 1 : 0, r.m_cached_ctx.DepthWrite() ? 1 : 0,
				static_cast<u32>(r.m_cached_ctx.TEST.ATE), static_cast<u32>(r.m_cached_ctx.TEST.ATST),
				static_cast<u32>(r.m_cached_ctx.TEST.AREF), static_cast<u32>(r.m_cached_ctx.TEST.AFAIL),
				static_cast<u32>(r.PRIM->ABE),
				static_cast<u32>(r.m_cached_ctx.FRAME.PSM), static_cast<u32>(r.m_cached_ctx.FRAME.FBMSK),
				static_cast<u32>(r.m_cached_ctx.TEX0.TBP0), static_cast<u32>(r.m_cached_ctx.TEX0.TBW),
				static_cast<u32>(r.m_cached_ctx.TEX0.PSM), static_cast<u32>(r.m_cached_ctx.TEX0.TW),
				static_cast<u32>(r.m_cached_ctx.TEX0.TH), static_cast<u32>(r.m_cached_ctx.TEX0.TCC),
				static_cast<u32>(r.m_cached_ctx.TEX0.TFX),
				(source && (source->m_target || source->m_from_target)) ? 1 : 0,
				min_w, max_w, min_z, max_z, min_px, max_px, min_py, max_py,
				rt_unscaled_width, rt_unscaled_height, min_u, max_u, min_v, max_v,
				material.content_hash));
		}

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

		// Frame order mirrors RPCS3's flip(): camera -> Present -> latch -> reap -> stats.
		// The camera submitted here was resolved at the previous VSync, which is the same one
		// this frame's draws un-projected against -- geometry and camera always reference one
		// matrix, at the cost of a bounded one-frame lag under motion.
		refresh_window_size();
		submit_camera();

		// Before Present, and before the beacon's empty-frame test, because a batched frame's
		// geometry has not been instanced until this runs.
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

		if (s_empty_frame_streak >= s_beacon_after_empty_frames)
			submit_debug_triangle();

		if (s_debug_light)
			remix_ps2::guarded_draw_light_instance(s_remix.api().DrawLightInstance, s_debug_light);

		if (s_sun_light)
			remix_ps2::guarded_draw_light_instance(s_remix.api().DrawLightInstance, s_sun_light);

		if (s_dome_light)
			remix_ps2::guarded_draw_light_instance(s_remix.api().DrawLightInstance, s_dome_light);

		const u32 status = remix_ps2::guarded_present(s_remix.api().Present, nullptr);
		if (status != REMIXAPI_ERROR_CODE_SUCCESS)
			ERROR_LOG("Remix: Present failed ({})", remix_ps2::error_name(status));

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
			if (s_frame_bounds_world && s_active_camera.valid && s_frame_max_w > 0.f)
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

		s_frame_bounds = scene_bounds{};
		s_frame_bounds_world = false;
		s_frame_max_w = 0.f;

		resolve_world_camera();

		// The per-draw dump follows the frames that actually submitted something, so it never
		// burns its budget on the loading screens before the mission starts.
		if (s_submitted_this_frame > 0)
		{
			if (!s_drawdump_started)
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
		s_frame_instanced_keys.clear();
		s_frame_group_keys.clear();
		batch_discard();
		s_reuse_handle = nullptr;
		s_pool_hashes.clear();
		s_pinned_hashes.clear();

		// Materials are content-keyed, so they survive a load by construction -- the same
		// texture bytes produce the same hash. Only the per-frame budget is reset, which is
		// what ramps the rebuild instead of doing it all in the first frame back.
		remix_ps2::materials::begin_frame();

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
		s_camera_last_accept_frame = 0;
		s_logged_world_camera = false;
		s_light_placed = false;
		s_sun_placed = false;
		s_drawdump_started = false;
		s_drawdump_frames_left = 0;
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
