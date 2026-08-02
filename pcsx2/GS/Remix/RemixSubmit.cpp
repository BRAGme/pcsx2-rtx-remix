// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixSubmit.h"
#include "GS/Remix/RemixRuntime.h"
#include "GS/Remix/RemixTransforms.h"

#include "GS/Renderers/HW/GSRendererHW.h"

#include "common/Console.h"
#include "common/WindowInfo.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RemixSubmit
{
	namespace
	{
		// Frames a mesh may go unreferenced before its handle is released. Matches RPCS3.
		constexpr u64 s_mesh_idle_frames = 300;

		// How often the counter block is emitted, in frames.
		constexpr u64 s_stats_interval_frames = 300;

		// Sanity ceilings on a single submitted mesh, from RPCS3. Guards against a malformed
		// draw turning into a multi-gigabyte allocation.
		constexpr u32 s_max_vertices_per_mesh = 0x40000;
		constexpr u32 s_max_indices_per_mesh = 0x100000;

		// Positions further out than this are the FLT_MIN/m_max Q guards leaking through
		// (GSState.cpp:1399/:1403 replace a zero Q with FLT_MIN, whose reciprocal is ~8.5e37).
		constexpr float s_max_position_magnitude = 1.0e9f;

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
			u64 meshes_created = 0;
			u64 meshes_destroyed = 0;
			u64 cam_world = 0;
			u64 cam_fallback = 0;
		};

		struct mesh_entry
		{
			remixapi_MeshHandle handle = nullptr;
			u64 last_used_frame = 0;
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

		remixapi_MeshHandle s_debug_mesh = nullptr;
		remixapi_LightHandle s_debug_light = nullptr;

		u64 s_frame_counter = 0;
		u64 s_submitted_this_frame = 0;
		stat_counters s_stats{};

		std::unordered_map<u64, mesh_entry> s_meshes;
		std::unordered_set<u64> s_poisoned;

		// Reused across draws to keep the hot path allocation free.
		std::vector<remixapi_HardcodedVertex> s_scratch_vertices;
		std::vector<u32> s_scratch_indices;

		float debug_light_radius()
		{
			static const float value = []() -> float {
				if (const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_LIGHTRADIUS"); !env.empty())
				{
					const float parsed = static_cast<float>(::_wtof(env.c_str()));
					if (std::isfinite(parsed) && parsed > 0.f)
						return parsed;
				}

				return 0.1f;
			}();

			return value;
		}

		float debug_light_radiance()
		{
			static const float value = []() -> float {
				if (const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_LIGHTRADIANCE"); !env.empty())
				{
					const float parsed = static_cast<float>(::_wtof(env.c_str()));
					if (std::isfinite(parsed) && parsed > 0.f)
						return parsed;
				}

				return 100.f;
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

		void place_debug_light(const float (&position)[3])
		{
			const remixapi_Interface& api = s_remix.api();

			if (s_debug_light)
			{
				remix_ps2::guarded_destroy_light(api.DestroyLight, s_debug_light);
				s_debug_light = nullptr;
			}

			remixapi_LightInfoSphereEXT sphere_light{};
			sphere_light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
			sphere_light.pNext = nullptr;
			sphere_light.position = {position[0], position[1], position[2]};
			sphere_light.radius = debug_light_radius();
			sphere_light.shaping_hasvalue = 0;
			// Zero-init leaves this at 0, but the runtime's own default is 1.0
			// (rtx_lights.h kVolumetricRadianceScaleDefaultValue). At 0 the light contributes
			// nothing volumetrically.
			sphere_light.volumetricRadianceScale = 1.f;

			const float radiance = debug_light_radiance();

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

		bool create_debug_scene()
		{
			const remixapi_Interface& api = s_remix.api();

			// Sphere light, straight from the official remixapi_example_c.c.
			const float origin_light[3] = {0.f, -1.f, 0.f};
			place_debug_light(origin_light);

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
		void ensure_initialized()
		{
			if (s_init_attempted)
				return;

			s_init_attempted = true;

			if (!s_hwnd)
			{
				ERROR_LOG("Remix: no game window was stashed, rendering is disabled");
				return;
			}

			if (!s_remix.initialize(s_hwnd))
			{
				ERROR_LOG("Remix: runtime unavailable, degrading to a no-op");
				return;
			}

			if (!create_debug_scene())
			{
				ERROR_LOG("Remix: debug scene setup failed, degrading to a no-op");
				return;
			}

			s_live = true;
			INFO_LOG("Remix: renderer is live (far plane {})", remix_ps2::hardcoded_far_plane());
		}

		bool armed()
		{
			return g_armed;
		}

		void reap_idle_meshes()
		{
			if (s_frame_counter < s_mesh_idle_frames)
				return;

			const u64 cutoff = s_frame_counter - s_mesh_idle_frames;
			const remixapi_Interface& api = s_remix.api();

			for (auto it = s_meshes.begin(); it != s_meshes.end();)
			{
				if (it->second.last_used_frame > cutoff)
				{
					++it;
					continue;
				}

				if (it->second.handle)
				{
					remix_ps2::guarded_destroy_mesh(api.DestroyMesh, it->second.handle);
					++s_stats.meshes_destroyed;
				}

				it = s_meshes.erase(it);
			}
		}

		void log_stats(bool force)
		{
			if (!force && (s_frame_counter == 0 || (s_frame_counter % s_stats_interval_frames) != 0))
				return;

			INFO_LOG("Remix: frame {} | seen {} submitted {} | meshes live {} (+{} -{}) | "
					 "skip: tri {} untex {} fst {} constq {} notarget {} empty {} large {} "
					 "nonfinite {} poisoned {} | warn stq {} | cam world {} fallback {}",
				s_frame_counter, s_stats.draws_seen, s_stats.draws_submitted, s_meshes.size(),
				s_stats.meshes_created, s_stats.meshes_destroyed,
				s_stats.skip_not_triangle, s_stats.skip_untextured, s_stats.skip_fst,
				s_stats.skip_const_q, s_stats.skip_no_target, s_stats.skip_empty,
				s_stats.skip_too_large, s_stats.skip_nonfinite, s_stats.skip_poisoned,
				s_stats.warn_inaccurate_stq, s_stats.cam_world, s_stats.cam_fallback);
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
			s_hwnd = static_cast<HWND>(wi.window_handle);
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

	void OnDrawPrims(const GSRendererHW& r, int rt_unscaled_width, int rt_unscaled_height)
	{
		if (!armed())
			return;

		ensure_initialized();

		if (!s_live)
			return;

		++s_stats.draws_seen;

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

		// The synthetic projection the recovered geometry is expressed against. Only its two
		// scale terms are needed: a point at eye depth w projects to ndc = (vx*a/w, vy*b/w).
		const remix_ps2::mat4 projection = remix_ps2::make_perspective(
			s_debug_fov_y_degrees, window_aspect(), 0.1f, remix_ps2::hardcoded_far_plane());
		const float inv_a = 1.0f / projection.m[0][0];
		const float inv_b = 1.0f / projection.m[1][1];

		const GSVertex* const verts = r.m_vertex->buff;

		s_scratch_vertices.clear();
		s_scratch_vertices.resize(vertex_count);

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
			out.position[0] = ndc_x * w * inv_a;
			out.position[1] = ndc_y * w * inv_b;
			out.position[2] = w;
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
				std::abs(out.position[0]) > s_max_position_magnitude ||
				std::abs(out.position[1]) > s_max_position_magnitude ||
				std::abs(out.position[2]) > s_max_position_magnitude ||
				!(w > 0.0f))
			{
				++s_stats.skip_nonfinite;
				return;
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

		// Flat per-triangle normals from the un-projected edges. Ten lines that remove an
		// all-black-shading failure mode; a smooth normal has nothing to be derived from here.
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
			if (!std::isfinite(len) || len < 1e-12f)
			{
				// Degenerate triangle: leave the -Z default rather than emit a NaN normal.
				continue;
			}

			n[0] /= len;
			n[1] /= len;
			n[2] /= len;

			// The whole triangle shares it: doubleSided = 1 makes the winding irrelevant.
			for (u32 k = 0; k < 3; ++k)
			{
				remixapi_HardcodedVertex& target = s_scratch_vertices[s_scratch_indices[i + k]];
				target.normal[0] = n[0];
				target.normal[1] = n[1];
				target.normal[2] = n[2];
			}
		}

		// --- mesh identity ------------------------------------------------------------------
		u64 hash = fnv_seed;

		{
			const u64* words = reinterpret_cast<const u64*>(s_scratch_vertices.data());
			const size_t word_count = (s_scratch_vertices.size() * sizeof(remixapi_HardcodedVertex)) / sizeof(u64);
			for (size_t i = 0; i < word_count; ++i)
				hash = fnv_mix(hash, words[i]);

			for (const u32 index : s_scratch_indices)
				hash = fnv_mix(hash, index);
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

		const remixapi_Interface& api = s_remix.api();

		auto it = s_meshes.find(hash);

		if (it == s_meshes.end())
		{
			remixapi_MeshInfoSurfaceTriangles surface{};
			surface.vertices_values = s_scratch_vertices.data();
			surface.vertices_count = s_scratch_vertices.size();
			surface.indices_values = s_scratch_indices.data();
			surface.indices_count = s_scratch_indices.size();
			surface.skinning_hasvalue = 0;
			// Null material: Remix substitutes its own default. Materials are a later phase.
			surface.material = nullptr;

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
			it = s_meshes.emplace(hash, mesh_entry{handle, s_frame_counter}).first;
		}

		it->second.last_used_frame = s_frame_counter;

		remixapi_InstanceInfo instance{};
		instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		instance.pNext = nullptr;
		instance.categoryFlags = 0;
		instance.mesh = it->second.handle;
		// Identity: the positions are already in the submitted camera's space. Per-draw world
		// transforms are phase 2.
		instance.transform = s_identity_transform;
		instance.doubleSided = 1;

		const u32 status = remix_ps2::guarded_draw_instance(api.DrawInstance, &instance);
		if (status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			s_poisoned.insert(hash);
			++s_stats.skip_poisoned;
			return;
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
		// The world-anchored camera is a later step; phase 1 always runs the fallback.
		refresh_window_size();
		submit_fallback_camera();
		++s_stats.cam_fallback;

		// The beacon only appears when nothing of the guest's own geometry survived the gates,
		// so it never clutters a working scene but still says "the runtime is alive".
		if (s_submitted_this_frame == 0)
			submit_debug_triangle();

		if (s_debug_light)
			remix_ps2::guarded_draw_light_instance(s_remix.api().DrawLightInstance, s_debug_light);

		const u32 status = remix_ps2::guarded_present(s_remix.api().Present, nullptr);
		if (status != REMIXAPI_ERROR_CODE_SUCCESS)
			ERROR_LOG("Remix: Present failed ({})", remix_ps2::error_name(status));

		++s_frame_counter;
		s_submitted_this_frame = 0;

		reap_idle_meshes();
		log_stats(false);
	}

	void OnGSClose()
	{
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

		// One last counter block, so a short session still reports what it saw.
		log_stats(true);

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
