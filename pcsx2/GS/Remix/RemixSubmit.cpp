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
			u32 cam_last_candidates = 0;
		};

		struct mesh_entry
		{
			remixapi_MeshHandle handle = nullptr;
			u64 last_used_frame = 0;
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
		};

		scene_bounds s_frame_bounds{};
		scene_bounds s_last_bounds{};

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

		bool create_debug_scene()
		{
			const remixapi_Interface& api = s_remix.api();

			// Sphere light, straight from the official remixapi_example_c.c. The scene radius
			// is a placeholder until the first frame measures one.
			const float origin_light[3] = {0.f, -1.f, 0.f};
			place_debug_light(origin_light, 2.f);

			if (!s_debug_light)
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

				// The fallback camera sits at the origin looking down +Z, so the light rides
				// with it and the distant light points the same way.
				const float origin_light[3] = {0.f, 0.f, 0.f};
				place_debug_light(origin_light, scene_radius);

				const float forward[3] = {0.f, 0.f, 1.f};
				place_sun_light(forward);
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

			// The debug light rides the camera, as in RPCS3: a world-space scene lit from
			// wherever the origin happens to be is usually a black scene.
			place_debug_light(s_active_camera.position, scene_radius);

			// Camera forward in world space. p_view = p_world * V (row-vector), so the gradient
			// of view z with respect to world position is V's third column.
			const float forward[3] = {
				s_active_camera.view.m[0][2], s_active_camera.view.m[1][2], s_active_camera.view.m[2][2]};
			place_sun_light(forward);
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
						s_frame_counter, (candidate.source == 0) ? "scan" : ((candidate.source == 1) ? "slice" : "slice-tops"),
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

			if (s_last_bounds.valid)
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
						 "fovY {:.1f} deg, near {:.5g}, far {:.5g}, eye ({:.3f}, {:.3f}, {:.3f}) "
						 "[matrix-implied near {:.5g}, not used]",
					(best_source == 0) ? "window scan" : ((best_source == 1) ? "ucode back-slice" : "ucode back-slice (TOPS)"),
					best_name, best_transposed ? "column-major" : "row-major", best_score,
					params.fov_y_degrees, camera.near_plane, camera.far_plane,
					camera.position[0], camera.position[1], camera.position[2],
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

		// Quantum, in world units, that positions are snapped to before they are hashed into a
		// mesh identity. 0 disables quantization and restores exact-bit hashing, which is the
		// A/B handle for the measurement. The default is deliberately coarse relative to the
		// camera's solve jitter but fine relative to real geometry: this title's submitted
		// scene spans ~6-8 world units, so 0.01 is ~0.15% of the scene.
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
				auto oldest = s_meshes.begin();
				for (auto it = s_meshes.begin(); it != s_meshes.end(); ++it)
				{
					if (it->second.last_used_frame < oldest->second.last_used_frame)
						oldest = it;
				}

				if (oldest->second.handle == s_reuse_handle)
				{
					// Would dangle the handle every instance this frame points at.
					oldest->second.last_used_frame = s_frame_counter;
					break;
				}

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

				if (it->second.handle == s_reuse_handle)
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

		void log_stats(bool force)
		{
			if (!force && (s_frame_counter == 0 || (s_frame_counter % stats_interval_frames()) != 0))
				return;

			INFO_LOG("Remix: frame {} | seen {} submitted {} | meshes live {} (+{} -{}) | "
					 "skip: tri {} untex {} fst {} constq {} notarget {} empty {} large {} "
					 "nonfinite {} poisoned {} meshbudget {} fbmsk {} coincident {} minw {} | "
					 "warn stq {} | cam world {} fallback {} | "
					 "maxpos {:.0f}/{:.0f} | scene r {:.0f} | sky {} cutout {} | degen tris {} alldegen {} | "
					 "mesh/frame peak +{} -{} | instbudget-skip {}",
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
				s_stats.meshes_destroyed_peak, s_stats.skip_inst_budget);

			// The w distribution of everything submitted, which is what the min-w gate is set
			// from. A pile in the first buckets is geometry collapsing onto the eye plane.
			INFO_LOG("Remix: submitted w (max per draw): <1e-3 {} <1e-2 {} <0.1 {} <1 {} <10 {} "
					 "<100 {} >=100 {} | minw gate {:g}",
				s_stats.w_histogram[0], s_stats.w_histogram[1], s_stats.w_histogram[2],
				s_stats.w_histogram[3], s_stats.w_histogram[4], s_stats.w_histogram[5],
				s_stats.w_histogram[6], min_submitted_w());

			// Third line: the material bridge. Kept separate so the counter block stays
			// readable, and because the two numbers the user has to act on -- unique content
			// hashes and the per-draw hash cost -- both live here.
			INFO_LOG("{}", remix_ps2::materials::stats_line());

			// Second line: the world anchor. Every stage of the pipeline is separately
			// visible, so a null result names the stage that produced it -- no kicks, no
			// windows through the shape prefilter, no candidates, no split, or no score.
			INFO_LOG("Remix: vu kicks {} scanned {} reentrant {} windows {} shape-ok {} | "
					 "slice matrices {} published {} | cand {} (now {}) | "
					 "split-reject {} score-reject {} degenerate-reject {} accept {} (sliced {}) | camera {}{}",
				s_stats.vu_kicks, s_stats.vu_kicks_scanned, s_stats.vu_reentrant, s_stats.vu_windows,
				s_stats.vu_survivors, s_stats.vu_sliced, s_stats.vu_sliced_published,
				s_stats.cam_candidates, s_stats.cam_last_candidates, s_stats.cam_reject_split,
				s_stats.cam_reject_score, s_stats.cam_reject_degenerate, s_stats.cam_accept,
				s_stats.cam_accept_sliced,
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

			const u32 bucket = (max_w < 1e-3f) ? 0 : (max_w < 1e-2f) ? 1 :
			                   (max_w < 1e-1f) ? 2 : (max_w < 1.f)   ? 3 :
			                   (max_w < 10.f)  ? 4 : (max_w < 100.f) ? 5 : 6;
			++s_stats.w_histogram[bucket];
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

		// Coincident-geometry dedupe. Every instance goes out with the identity transform, so a
		// second instance of the same mesh hash in the same frame is literally the same
		// triangles in the same place -- overdraw the guest needed for its own blending, and
		// z-fighting once a path tracer gets hold of it. 81% of the logged draws in Rainbow Six
		// 3 were coincident with an earlier one.
		if (!s_frame_submitted_hashes.insert(hash).second)
		{
			++s_stats.skip_coincident;
			return;
		}

		const remixapi_Interface& api = s_remix.api();

		auto it = s_meshes.find(hash);

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
		}

		it->second.last_used_frame = s_frame_counter;

		const bool depth_read = r.m_cached_ctx.DepthRead();
		const bool depth_write = r.m_cached_ctx.DepthWrite();
		const bool is_sky = classify_sky(depth_read, depth_write, s_submitted_this_frame);

		// The user's own tags, from the Remix conf layers. dxvk-remix only applies its hash
		// lists on the native D3D9 path (setupCategoriesForTexture, rtx_types.cpp:348, whose one
		// caller is d3d9_rtx.cpp:1064); an API instance's categories come solely from this
		// field (rtx_remix_api.cpp:803). So a tag made in the developer menu does nothing at all
		// unless we look it up ourselves and OR it in here.
		const remixapi_InstanceCategoryFlags tagged = remix_ps2::materials::categories_for(material.content_hash);

		remixapi_InstanceInfoBlendEXT blend{};
		blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
		blend.pNext = nullptr;
		blend.alphaTestEnabled = r.m_cached_ctx.TEST.ATE ? 1 : 0;
		blend.alphaTestReferenceValue = static_cast<u8>(r.m_cached_ctx.TEST.AREF);
		blend.alphaTestCompareOp = to_d3d_compare(r.m_cached_ctx.TEST.ATST);
		blend.alphaBlendEnabled = r.PRIM->ABE ? 1 : 0;
		to_d3d_blend(r.m_context->ALPHA, blend.srcColorBlendFactor, blend.dstColorBlendFactor);
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
			const bool decal = (r.m_cached_ctx.TEX0.TFX == 1);
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
			(cutout_mode() != 0) && r.m_cached_ctx.TEST.ATE && (r.m_cached_ctx.TEST.ATST != 1); // not ALWAYS

		remixapi_InstanceInfo instance{};
		instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		instance.pNext = (alpha_state_mode() == 2) ? &blend : nullptr;
		instance.categoryFlags = tagged |
		                         (is_sky ? static_cast<u32>(REMIXAPI_INSTANCE_CATEGORY_BIT_SKY) : 0u) |
		                         (is_cutout ? static_cast<u32>(REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT) : 0u);
		if (reuse_one_handle())
		{
			// First game mesh of the session, held for its lifetime: real geometry rather than
			// the debug triangle, so the BLAS being instanced is representative.
			if (!s_reuse_handle)
				s_reuse_handle = it->second.handle;

			instance.mesh = s_reuse_handle;
		}
		else
		{
			instance.mesh = it->second.handle;
		}
		// Identity: the positions are already in the submitted camera's space. Per-draw world
		// transforms are phase 2.
		instance.transform = s_identity_transform;
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

		if (is_sky)
			++s_stats.sky_tagged;

		if (is_cutout)
			++s_stats.cutout_tagged;

		if (s_drawdump_frames_left > 0)
		{
			drawdump_write(fmt::format(
				"f={} d={} verts={} tris={} sky={} | ZTE={} ZTST={} ZMSK={} zpsm=0x{:02x} depth(r={} w={}) | "
				"ATE={} ATST={} AREF={} AFAIL={} ABE={} | fpsm=0x{:02x} fbmsk=0x{:08x} | "
				"tex tbp0=0x{:04x} tbw={} psm=0x{:02x} tw={} th={} tcc={} tfx={} target={} | "
				"w=[{:.1f},{:.1f}] z=[{},{}] | px=[{:.0f},{:.0f}]x[{:.0f},{:.0f}] rt={}x{} | mat={:016X}",
				s_frame_counter, s_submitted_this_frame, vertex_count, s_scratch_indices.size() / 3,
				is_sky ? 1 : 0,
				static_cast<u32>(r.m_cached_ctx.TEST.ZTE), static_cast<u32>(r.m_cached_ctx.TEST.ZTST),
				static_cast<u32>(r.m_cached_ctx.ZBUF.ZMSK), static_cast<u32>(r.m_cached_ctx.ZBUF.PSM),
				depth_read ? 1 : 0, depth_write ? 1 : 0,
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
				rt_unscaled_width, rt_unscaled_height, material.content_hash));
		}

		// Only now that the draw is committed does its extent count towards the frame's, and
		// sky geometry is deliberately excluded: a skybox is huge by construction and would
		// dominate the scene radius the debug light is scaled from.
		if (!is_sky && draw_bounds.valid)
		{
			s_frame_bounds.add(draw_bounds.min);
			s_frame_bounds.add(draw_bounds.max);
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
			s_last_bounds = s_frame_bounds;

		s_frame_bounds = scene_bounds{};

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

		++s_frame_counter;
		s_submitted_this_frame = 0;
		s_frame_submitted_hashes.clear();

		// Meshes first, then materials: a live mesh holds the material handle Remix bound into
		// it at CreateMesh time, so releasing a material before the meshes referencing it would
		// leave the runtime holding a dead handle.
		reap_idle_meshes();
		remix_ps2::materials::reap(s_remix, s_frame_counter);
		remix_ps2::materials::begin_frame();

		// Picks up tags the user saved from the developer menu without an emulator restart.
		remix_ps2::materials::refresh_categories();

		log_stats(false);
	}

	void OnGSStateLoaded()
	{
		if (!armed())
			return;

		// The VU1 candidates first, so nothing can latch a pre-load matrix in between.
		RemixVU1Capture::DropPublished();
		RemixVU1Capture::SetPinnedOffset(0xFFFFFFFFu);

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
		s_reuse_handle = nullptr;

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
