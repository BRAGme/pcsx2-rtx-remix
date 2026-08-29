// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixMaterials.h"
#include "GS/Remix/RemixPaths.h"

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "common/StringUtil.h"
#include "GS/Renderers/Common/GSRenderer.h"

#include "Config.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace remix_ps2::materials
{
	namespace
	{
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

		// How many textures may be created in one frame. RPCS3 uses 8; a PS2 frame touches an
		// order of magnitude more distinct textures than an RSX one, so the warm-up would take
		// minutes at 8. The budget exists to stop a level transition from stalling on hundreds
		// of CreateTexture calls in a single frame, not to be a steady-state limit.
		// 0 disables the bridge outright, which is the runtime bisection handle: it takes
		// materials out of the picture without a rebuild.
		u32 texture_budget()
		{
			static const u32 value =
				static_cast<u32>(std::max<s64>(0, read_env_int(L"PCSX2_REMIX_TEXBUDGET", 32)));
			return value;
		}

		// Frames a material may go unreferenced before release.
		//
		// LOAD-BEARING INVARIANT: this must be >= the mesh LRU window (PCSX2_REMIX_MESHIDLE,
		// default 120), because Remix binds the material into the mesh at CreateMesh time and a
		// live mesh holds that handle. bind() stamps the material on every draw and the mesh is
		// stamped in the same draw, so material age <= mesh age always; keeping the material
		// window wider means a material can never be destroyed out from under a live mesh.
		u64 texture_idle_frames()
		{
			static const u64 value =
				static_cast<u64>(std::max<s64>(1, read_env_int(L"PCSX2_REMIX_TEXIDLE", 600)));
			return value;
		}

		// Safety valve on resident materials. Only entries already older than the mesh window
		// are eligible, so the cap can never dangle a live mesh's material either.
		size_t texture_live_cap()
		{
			static const size_t value =
				static_cast<size_t>(std::max<s64>(64, read_env_int(L"PCSX2_REMIX_TEXCAP", 4096)));
			return value;
		}

		// The mesh LRU window, read from the same env var RemixSubmit uses, so the invariant
		// above can be checked rather than assumed.
		u64 mesh_idle_frames()
		{
			static const u64 value =
				static_cast<u64>(std::max<s64>(1, read_env_int(L"PCSX2_REMIX_MESHIDLE", 120)));
			return value;
		}

		// Bisection handle for the bridge itself, because "materials on" is four separable
		// runtime interactions and a GPU device loss names none of them:
		//   1 = key + decode only, no Remix call at all
		//   2 = CreateTexture only, mesh still gets a null material
		//   3 = CreateTexture + CreateMaterial, but with no albedo texture named
		//   4 = full, including the pseudo-path that binds the two (default)
		int material_stage()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(read_env_int(L"PCSX2_REMIX_MATSTAGE", 4), 1, 4));
			return value;
		}

		// Perceptual roughness for every legacy (non-replaced) material. 0.7 matches the runtime's
		// own default. PS2 titles ship no roughness map, so this single constant decides how glossy
		// the whole world is, and a low value gives every bright light a broad specular lobe that
		// slides across surfaces as the view moves.
		float legacy_roughness()
		{
			static const float value = [] {
				const std::wstring raw = read_env(L"PCSX2_REMIX_ROUGHNESS");
				if (raw.empty())
					return 0.7f;
				return std::clamp(static_cast<float>(std::wcstod(raw.c_str(), nullptr)), 0.f, 1.f);
			}();
			return value;
		}

		// Mirrors RemixSubmit's PCSX2_REMIX_ALPHASTATE so the material flag and the instance
		// struct can never disagree.
		int alpha_state_mode()
		{
			static const int value =
				static_cast<int>(std::clamp<s64>(read_env_int(L"PCSX2_REMIX_ALPHASTATE", 2), 0, 2));
			return value;
		}

		// PCSX2_REMIX_ALBEDOPROBE=1 sets every material's albedoConstant to magenta while leaving its
		// albedoTexture alone.
		//
		// The discriminator for "why are there no textures". If surfaces render with texture detail,
		// the pseudo-path albedo resolves and the flatness is elsewhere. If they render MAGENTA, the
		// texture is not resolving and Remix is falling back to the constant.
		//
		// This exists because the earlier conclusion that the pseudo-path resolves was CONFOUNDED: the
		// untextured material was given a white 4x4 texture AND a white albedoConstant, so albedo
		// going from 0 (nullptr) to ~580,000 is equally consistent with the texture resolving or with
		// the constant being used once a path is merely named. Same colour either way, no information.

		// Re-upload a material's texture whenever the material is rebuilt. Default 1: addTexture is a
		// per-frame registration, so without this a rebuilt material resolves its albedo against a
		// table that no longer contains our texture.
		bool texture_reupload_on_rebuild()
		{
			static const bool value = read_env_int(L"PCSX2_REMIX_TEXREUPLOAD", 1) != 0;
			return value;
		}

		bool replacement_albedo()
		{
			// DEFAULT 0. Pointing albedoTexture at a .dds on disk is the one thing this path does
			// that the last build with working textures (036a3529b) does not do at all -- there,
			// albedoTexture is always the API pseudo-path. With a replacement pack installed the
			// file branch wins for most textures (measured: 27 of 34 unique on Rainbow Six 3), so
			// almost nothing goes through the pseudo-path and the API-uploaded textures are never
			// referenced by any material. The user's Remix developer menu then lists NO textures
			// at all, which is what put the fault here rather than in shading: it is a supply
			// problem, not an albedo problem.
			//
			// On by default this was never a measured improvement, only an untested hypothesis
			// about the pseudo-path being broken. Off restores the known-good behaviour; turn it
			// back on only with a before/after that shows replacement textures actually resolving.
			static const bool value = read_env_int(L"PCSX2_REMIX_REPLACEALBEDO", 0) != 0;
			return value;
		}

		bool albedo_probe()
		{
			static const bool value = read_env_int(L"PCSX2_REMIX_ALBEDOPROBE", 0) != 0;
			return value;
		}

		// Frames to wait after a material is created before rebuilding it once. 0 = never.
		// See the probe at the bind() cache-hit path for what this is testing.
		u64 material_late_rebuild()
		{
			// Live, not latched: this is a refresh interval, and finding the right one means
			// trying several against a scene that is visibly losing its textures.
			static u64 value = 0;
			static u64 generation = ~0ull;

			if (const u64 now = remix_ps2::paths::knob_generation(); now != generation)
			{
				generation = now;
				value = static_cast<u64>(std::max<s64>(0, read_env_int(L"PCSX2_REMIX_MATREBUILD", 0)));
			}

			return value;
		}

		bool textures_linear()
		{
			static const bool value = read_env_int(L"PCSX2_REMIX_TEXLINEAR", 0) != 0;
			return value;
		}

		// Whether to give render-to-texture sources a material by snapshotting them.
		//
		// Off by default: it costs a GPU->CPU download per distinct render target and it takes a
		// ONE-OFF snapshot, so anything animated freezes at whatever the first frame held. Worth it
		// on SOCOM, where the sky IS a render target and rejecting it left the scene with no sky
		// and nothing to light it from.
		u32 rt_texture_interval()
		{
			static const u32 value =
				static_cast<u32>(std::max<s64>(0, read_env_int(L"PCSX2_REMIX_RTTEX", 0)));
			return value;
		}

		// One line per unique content hash, to the emulator log and to logs\remix_textures.txt.
		// This is the modder-facing artefact: the hash printed here is exactly the value that
		// goes into rtx.conf (rtx.skyBoxTextures, rtx.ignoreTextures, ...) and that the runtime
		// UI shows when the texture is selected.
		bool dump_enabled()
		{
			static const bool value = read_env_int(L"PCSX2_REMIX_TEXDUMP", 1) != 0;
			return value;
		}

		// PS2 stores alpha with 0x80 == 1.0, so a straight copy would make every surface
		// half-transparent. Doubling with a clamp is the standard expansion and matches what
		// the PS2 hardware does on output.
		bool expand_alpha()
		{
			static const bool value = read_env_int(L"PCSX2_REMIX_TEXALPHA", 1) != 0;
			return value;
		}

		struct material_entry
		{
			remixapi_TextureHandle texture = nullptr;
			remixapi_MaterialHandle material = nullptr;
			// The decoded BGRA8 payload, kept resident for the life of the texture. This is not
			// belt-and-braces: the runtime does not take a copy at CreateTexture time, so a
			// shared scratch buffer that the next texture overwrites is a use-after-free from
			// the GPU's point of view. It cost a reproducible VK_ERROR_DEVICE_LOST about two
			// seconds into every session before it was made per-entry. RPCS3 keeps its decoded
			// BGRA8 resident for the same reason (RemixTextures.cpp, "kept resident").
			std::vector<u8> pixels;
			u64 last_used_frame = 0;
			u64 created_frame = 0; // frame CreateMaterial ran, for the late-rebuild probe
			// Resolved replacement .dds path, empty when there is none. Held here because
			// rebuild_material only receives content_hash and cannot redo the HashCacheKey lookup.
			std::wstring albedo_file;
			// Frame of the most recent albedo re-resolve. Not a one-shot flag: Remix's texture table
			// is a per-frame registration, so a material that resolved once can lose its texture at
			// any later point and has to be able to resolve again.
			u64 last_rebuild_frame = 0;
			u64 generation = 0; // tag-list generation this material was built for
			u32 width = 0;
			u32 height = 0;
			bool failed = false; // decode or a runtime call refused: never retried
			bool is_render_target = false;
		};

		struct counters
		{
			u64 binds = 0;
			u64 hits = 0;
			u64 misses = 0;
			u64 created = 0;
			u64 destroyed = 0;
			u64 deferred = 0; // over the per-frame budget
			u64 skip_no_source = 0;
			u64 texture_reuploads = 0;    // re-registrations to keep the texture in this frame''s table
			u64 replacement_albedo = 0;   // materials bound to a real replacement .dds
			u64 replacement_not_dds = 0;  // replacement existed but was not .dds, so unusable
			// Times the replacement lookup was actually ATTEMPTED, i.e. REPLACEALBEDO was on at a
			// create(). MEASURED 2026-08-06: SOCOM with PCSX2_REMIX_REPLACEALBEDO=1 in the launch
			// environment reported `replacement dds 0 skipped-nondds 0` over 153 unique textures,
			// and those two counters cannot tell "the knob never applied" apart from "the knob
			// applied and PCSX2's replacement map has no entry for any of this title's hashes".
			// Without this probe the arm has no positive check that the work happened at all,
			// which is the standing rule this project has broken three times.
			u64 replacement_probe = 0;
			u64 skip_target = 0; // render-target source: no stable content identity
			u64 rt_snapshots = 0; // GPU->local-memory downloads taken for a render-target source
			u64 skip_unsupported = 0; // no rtx entry point, or absurd dimensions
			u64 skip_failed = 0; // quarantined after a decode / runtime failure
			u64 failures = 0;

			// Cost measurement (research 6.6 / 7.5 both flag this as unmeasured).
			u64 hash_ticks = 0;
			u64 decode_ticks = 0;

			// CLUT-animation measurement: an animated palette mints a new HashCacheKey every
			// frame, because HashCacheKey folds CLUTHash in. If that is happening, unique
			// content hashes climb without bound while unique TEX0 hashes do not.
			u64 unique_content = 0;
			u64 unique_tex0 = 0;
		};

		std::unordered_map<u64, material_entry> s_entries;
		std::unordered_set<u64> s_seen_content;
		std::unordered_set<u64> s_seen_tex0;
		std::unordered_set<u64> s_dumped;

		// The unswizzle target MUST be vector-aligned: GSLocalMemory's readTexture functions
		// store with aligned SIMD writes, which is why PCSX2's own scratch buffer is an
		// _aligned_malloc (GSTextureCache.cpp:55) and DumpTexture aligns its per-texture buffer
		// to 32 (GSTextureReplacements.cpp:836). std::vector<u8> only guarantees 16 on MSVC x64,
		// so it is the wrong container for this and cannot be used here.
		u8* s_decode_buffer = nullptr;
		size_t s_decode_capacity = 0;

		counters s_stats{};
		u32 s_budget_left = 0;
		u32 s_rt_readback_block_frames = 0;

		bool reserve_decode_buffer(size_t bytes)
		{
			if (s_decode_capacity >= bytes)
				return true;

			if (s_decode_buffer)
			{
				_aligned_free(s_decode_buffer);
				s_decode_buffer = nullptr;
				s_decode_capacity = 0;
			}

			s_decode_buffer = static_cast<u8*>(_aligned_malloc(bytes, VECTOR_ALIGNMENT));
			if (!s_decode_buffer)
				return false;

			s_decode_capacity = bytes;
			return true;
		}

		// Bounded so a title that really does animate every palette cannot turn the
		// measurement sets into a leak.
		constexpr size_t s_max_measurement_entries = 200000;

		// --- category tags from the runtime's own conf layers ---------------------------------
		//
		// Names and bits are paired against dxvk-remix's setupCategoriesForTexture()
		// (rtx_types.cpp:348) and remix_c.h's remixapi_InstanceCategoryBit. The conf text format
		// is HashSetLayer's (util_hash_set_layer.h:129-152, :157-183): a comma-separated list of
		// "0x%016X", where a leading '-' negates an entry inherited from an earlier layer.
		struct category_option
		{
			const char* key;
			remixapi_InstanceCategoryFlags bit;
		};

		constexpr category_option s_category_options[] = {
			{"rtx.worldSpaceUiTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI},
			{"rtx.worldSpaceUiBackgroundTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_MATTE},
			{"rtx.skyBoxTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_SKY},
			{"rtx.ignoreTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE},
			{"rtx.ignoreLights", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS},
			{"rtx.antiCullingTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ANTI_CULLING},
			{"rtx.motionBlurMaskOutTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_MOTION_BLUR},
			{"rtx.opacityMicromapIgnoreTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP},
			{"rtx.hideInstanceTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN},
			{"rtx.particleTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE},
			{"rtx.beamTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_BEAM},
			{"rtx.decalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC},
			{"rtx.dynamicDecalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC},
			{"rtx.singleOffsetDecalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET},
			{"rtx.nonOffsetDecalTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET},
			{"rtx.terrainTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN},
			{"rtx.animatedWaterTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_ANIMATED_WATER},
			{"rtx.playerModelTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL},
			{"rtx.playerModelBodyTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY},
			{"rtx.ignoreBakedLightingTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING},
			{"rtx.ignoreAlphaOnTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL},
			{"rtx.ignoreTransparencyLayerTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_TRANSPARENCY_LAYER},
			{"rtx.particleEmitterTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE_EMITTER},
			// Not a dxvk-remix option name -- setupCategoriesForTexture has no entry for
			// AlphaBlendToCutout, so the developer menu cannot write it and there is nothing to
			// stay compatible with. Ours, so the user still has a by-hash lever for foliage and
			// decals that the ATE auto-classifier misses.
			{"rtx.pcsx2AlphaCutoutTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT},
			// Routes an instance to the VIEW_MODEL camera rather than the world one. This is the
			// only way to reach that camera from the API: categoryToCameraType checks this bit
			// first and returns CameraType::ViewModel, and the result becomes
			// prototype.cameraType (rtx_remix_api.cpp, remix-plus-1.5.1). There is no such
			// texture-list option in the runtime, so the name is ours -- but the BIT is the
			// runtime's, verified present at 1 << 26 in the deployed tag's public header. The
			// April source checkout has neither the bit nor the ViewModel branch, so read the
			// deployed tag, not the clone.
			//
			// Also needs rtx.viewModel.enable, which defaults false.
			//
			// Spelled pcsx2.*, not rtx.*, for the same reason as pcsx2.emissiveTextures: the
			// runtime has no such option, so an rtx.* spelling makes the loader push it through
			// SetConfigVariable and Remix rejects it -- measured as
			// "rtx.viewModelTextures = ... FAILED (GENERAL_FAILURE)" in the per-game config lines,
			// even though our own parser had already applied the flag correctly. The tag worked
			// and the log said it failed, which is the worst of both.
			{"pcsx2.viewModelTextures", REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL},
		};

		std::unordered_map<u64, remixapi_InstanceCategoryFlags> s_categories;
		u64 s_category_tags = 0; // entries in s_categories, for the stats line
		u64 s_category_hits = 0; // instances that picked a flag up
		// The OR of every flag in s_categories, rebuilt with the table.
		//
		// Exists so a caller can ask "is ANY texture tagged with this category?" without a map
		// lookup, and therefore without computing a content hash first. The sky classifier reads
		// it as an early-out: hashing a source costs a full HashTextureLevel unswizzle+hash
		// (GSTextureCache.cpp, HashCacheKey::Create) -- the same work that makes bind() 1.7-2.6 us
		// -- so a title with no sky tags must not pay it on every textured draw.
		remixapi_InstanceCategoryFlags s_category_mask = 0;

		// Textures the user has asked to emit light. There is no emissive texture list in
		// RtxOptions -- dxvk-remix only has global scales (rtx_options.h:339, :894) -- so there
		// is no existing option name the developer menu would write for us. Two sources:
		//
		//  * our own conf key, in the same layers and the same format as the category lists, so
		//    it lives next to everything else the user edits; and
		//  * an existing Remix category, so a tag made in the developer menu turns a fixture
		//    into a light with no file editing at all. The default is worldSpaceUiTextures
		//    because that is precisely what the user had already found and used to make bulbs
		//    bright -- WORLD_UI makes them *render* bright but emits nothing, which is the gap
		//    this closes.
		std::unordered_set<u64> s_emissive;
		u64 s_generation = 1;

		// Bit 31 is unused by remixapi_InstanceCategoryBit (which stops at 1 << 26), so the
		// emissive marker can ride through the same parse/negate machinery as a real category
		// and then be stripped before anything reaches the runtime.
		constexpr remixapi_InstanceCategoryFlags s_emissive_marker_bit = 1u << 31;

		// Materials replaced because the tag lists changed. They are not destroyed immediately:
		// meshes built earlier still hold the handle Remix bound into them at CreateMesh time,
		// so they are retired on the same age rule the meshes are.
		struct retired_material
		{
			remixapi_MaterialHandle handle;
			u64 frame;
			bool is_render_target;
		};

		std::vector<retired_material> s_retired;

		float emissive_intensity()
		{
			static const float value = []() -> float {
				if (const std::wstring env = read_env(L"PCSX2_REMIX_EMISSIVEINTENSITY"); !env.empty())
				{
					const float parsed = static_cast<float>(::_wtof(env.c_str()));
					if (std::isfinite(parsed) && parsed >= 0.f)
						return parsed;
				}

				// Deliberately strong: the levels measured here contain no lights at all, so a
				// timid default would look like the feature had not worked. The user is the one
				// who can see the result, so this is the first knob they should reach for.
				return 20.f;
			}();

			return value;
		}

		// Which existing Remix category also means "emissive". Empty disables the bridge and
		// leaves only the explicit list.
		remixapi_InstanceCategoryFlags emissive_from_category()
		{
			static const remixapi_InstanceCategoryFlags value = []() -> remixapi_InstanceCategoryFlags {
				const std::wstring env = read_env(L"PCSX2_REMIX_EMISSIVEFROM");
				if (env.empty())
					return REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI;

				if (env == L"0" || env == L"none")
					return 0;

				return static_cast<remixapi_InstanceCategoryFlags>(::wcstoull(env.c_str(), nullptr, 0));
			}();

			return value;
		}

		// The per-game Remix conf layers, most specific last.
		//
		// PCSX2 is one executable for hundreds of games, and Remix keys its user settings by exe
		// identity -- so without this every PS2 title shares one pot, and SOCOM's texture tags,
		// sky categorisation and light calibration land on top of Rainbow Six 3's.
		//
		// The naming mirrors PCSX2's own per-game settings (VMManager::GetGameSettingsPath,
		// VMManager.cpp:812) rather than inventing a scheme: a user who knows where
		// gamesettings\SCUS-97545_D7CFDCCF.ini lives can guess SCUS-97545_D7CFDCCF.conf. The
		// serial-only name is accepted too and is the one to reach for by hand, since it
		// survives a different disc revision of the same game.
		//
		// GetDiscSerial/GetDiscCRC take s_info_mutex (VMManager.cpp:333, :358), so calling them
		// from the GS thread is safe.
		std::vector<std::string> game_conf_paths()
		{
			std::vector<std::string> paths;

			const std::string serial = VMManager::GetDiscSerial();
			const u32 crc = VMManager::GetDiscCRC();
			if (serial.empty() && crc == 0)
				return paths;

			// The per-game folder layer (Remix\<SERIAL>\rtx.conf, then user.conf) comes first, so
			// the flat <exe dir>\<SERIAL>.conf files below still outrank it. That ordering is
			// deliberate: those flat files predate this layout and are what the existing A/B
			// harness and the hand-written hash lists live in, so adopting the folder must not
			// silently change what a title already renders like.
			if (const std::string game_dir = remix_ps2::paths::game_dir(); !game_dir.empty())
			{
				paths.push_back(Path::Combine(game_dir, "rtx.conf"));
				paths.push_back(Path::Combine(game_dir, "user.conf"));
			}

			const std::string dir(Path::GetDirectory(FileSystem::GetProgramPath()));
			const std::string sanitized(Path::SanitizeFileName(serial));

			if (!sanitized.empty())
				paths.push_back(Path::Combine(dir, fmt::format("{}.conf", sanitized)));

			if (crc != 0)
			{
				paths.push_back(Path::Combine(dir, sanitized.empty() ?
					fmt::format("{:08X}.conf", crc) :
					fmt::format("{}_{:08X}.conf", sanitized, crc)));
			}

			return paths;
		}

		std::vector<std::string> conf_paths()
		{
			std::vector<std::string> paths;

			// Layer order matters: rtx.conf is the mod layer, user.conf the user layer, and the
			// developer menu writes to both. Later files win, and a '-' entry in a later file
			// removes one an earlier file added.
			const std::string dir(Path::GetDirectory(FileSystem::GetProgramPath()));
			paths.push_back(Path::Combine(dir, "rtx.conf"));
			paths.push_back(Path::Combine(dir, "user.conf"));

			// Extra layers, semicolon-separated, for a mod kept outside the emulator directory.
			if (const std::wstring extra = read_env(L"PCSX2_REMIX_CONF"); !extra.empty())
			{
				const std::string narrow(extra.begin(), extra.end());
				size_t start = 0;
				while (start <= narrow.size())
				{
					const size_t end = narrow.find(';', start);
					std::string one = narrow.substr(start, (end == std::string::npos) ? std::string::npos : (end - start));
					if (!one.empty())
						paths.push_back(std::move(one));

					if (end == std::string::npos)
						break;

					start = end + 1;
				}
			}

			// Per-game last, so a title's own tags outrank the shared layers. This is also what
			// makes the digit-group-comma parser (see parse_hash_list) apply to per-game hash
			// lists for free -- they go through exactly the same reader.
			for (std::string& one : game_conf_paths())
				paths.push_back(std::move(one));

			return paths;
		}

		void parse_hash_list(const std::string& value, remixapi_InstanceCategoryFlags bit)
		{
			// The list is comma-separated -- but the runtime's own writer emits each hash with
			// digit-group commas as well. HashSetLayer::toString() (util_hash_set_layer.h:157)
			// streams the value through std::hex on a stringstream that has picked up a
			// grouping locale, so a single hash lands on disk as
			//   rtx.ignoreTextures = 0x2,267,8AA,9EB,AF3,737
			// (observed verbatim after the developer menu saved a tag we had planted).
			// Splitting naively on ',' turns one hash into six garbage ones.
			//
			// The disambiguator is the "0x": the runtime always writes it, and only at the
			// start of an entry. So a token beginning with 0x (or -0x) opens a new entry and
			// anything else is a continuation of the digits of the current one.
			std::vector<std::string> entries;

			size_t start = 0;
			while (start <= value.size())
			{
				const size_t end = value.find(',', start);
				std::string token = value.substr(start, (end == std::string::npos) ? std::string::npos : (end - start));

				const size_t first = token.find_first_not_of(" \t\r\n");
				const size_t last = token.find_last_not_of(" \t\r\n");

				if (first != std::string::npos)
				{
					token = token.substr(first, last - first + 1);

					const bool opens =
						(token.size() > 1 && token[0] == '-') ?
							(token.size() > 3 && token[1] == '0' && (token[2] == 'x' || token[2] == 'X')) :
							(token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'));

					if (opens || entries.empty())
						entries.push_back(std::move(token));
					else
						entries.back() += token;
				}

				if (end == std::string::npos)
					break;

				start = end + 1;
			}

			for (std::string& entry : entries)
			{
				const bool negate = (!entry.empty() && entry[0] == '-');
				if (negate)
					entry.erase(0, 1);

				// std::stoull with base 16 accepts the "0x" prefix the runtime writes.
				u64 hash = 0;
				try
				{
					size_t consumed = 0;
					hash = std::stoull(entry, &consumed, 16);
					if (consumed == 0)
						hash = 0;
				}
				catch (...)
				{
					hash = 0;
				}

				if (hash == 0)
					continue;

				if (negate)
					s_categories[hash] &= ~bit;
				else
					s_categories[hash] |= bit;
			}
		}

		void load_categories()
		{
			s_categories.clear();

			for (const std::string& path : conf_paths())
			{
				std::FILE* file = FileSystem::OpenCFile(path.c_str(), "r");
				if (!file)
					continue;

				char line[8192];
				while (std::fgets(line, sizeof(line), file))
				{
					std::string text(line);

					// Strip a trailing comment and the newline; the runtime writes '#' comments.
					if (const size_t hash_pos = text.find('#'); hash_pos != std::string::npos)
						text.erase(hash_pos);

					const size_t eq = text.find('=');
					if (eq == std::string::npos)
						continue;

					std::string key = text.substr(0, eq);
					const size_t kfirst = key.find_first_not_of(" \t");
					const size_t klast = key.find_last_not_of(" \t\r\n");
					if (kfirst == std::string::npos)
						continue;

					key = key.substr(kfirst, klast - kfirst + 1);

					// Our own key, in the same layers and the same textual format. Both
					// spellings are accepted because the runtime's config reader tolerates
					// either and a user is as likely to write one as the other.
					if (key == "rtx.pcsx2EmissiveTextures" || key == "pcsx2.emissiveTextures")
					{
						parse_hash_list(text.substr(eq + 1), s_emissive_marker_bit);
						continue;
					}

					for (const category_option& option : s_category_options)
					{
						if (key == option.key)
						{
							parse_hash_list(text.substr(eq + 1), option.bit);
							break;
						}
					}
				}

				std::fclose(file);
			}

			// Drop entries every layer cancelled out, so the count means what it says.
			for (auto it = s_categories.begin(); it != s_categories.end();)
				it = (it->second == 0) ? s_categories.erase(it) : std::next(it);

			// Split the emissive marker back out: it is carried through the same map only so
			// that '-0x...' negation works on it exactly as it does on a real category, and it
			// must never reach the runtime as a category flag.
			s_emissive.clear();

			const remixapi_InstanceCategoryFlags from_category = emissive_from_category();

			for (auto it = s_categories.begin(); it != s_categories.end();)
			{
				const bool marked = (it->second & s_emissive_marker_bit) != 0;
				it->second &= ~s_emissive_marker_bit;

				if (marked || (from_category != 0 && (it->second & from_category) != 0))
					s_emissive.insert(it->first);

				it = (it->second == 0) ? s_categories.erase(it) : std::next(it);
			}

			// PCSX2_REMIX_EMISSIVE, same format, for iterating without touching a file.
			if (const std::wstring env = read_env(L"PCSX2_REMIX_EMISSIVE"); !env.empty())
			{
				const std::string narrow(env.begin(), env.end());
				const size_t before = s_categories.size();
				parse_hash_list(narrow, s_emissive_marker_bit);

				for (auto it = s_categories.begin(); it != s_categories.end();)
				{
					if ((it->second & s_emissive_marker_bit) != 0)
					{
						s_emissive.insert(it->first);
						it->second &= ~s_emissive_marker_bit;
					}

					it = (it->second == 0) ? s_categories.erase(it) : std::next(it);
				}

				(void)before;
			}

			s_category_tags = s_categories.size();

			s_category_mask = 0;
			for (const auto& [hash, flags] : s_categories)
				s_category_mask |= flags;
		}

		void dump_write(const std::string& line)
		{
			static constexpr u32 max_lines = 8192;
			static u32 written = 0;
			static bool tried = false;
			static std::FILE* file = nullptr;

			if (!tried)
			{
				tried = true;

				const std::string& dir = EmuFolders::Logs.empty() ? EmuFolders::AppRoot : EmuFolders::Logs;
				const std::string path = Path::Combine(dir, "remix_textures.txt");
				file = FileSystem::OpenCFile(path.c_str(), "w");

				if (file)
					INFO_LOG("Remix: writing the modder-facing texture hash list to '{}'", path);
				else
					ERROR_LOG("Remix: could not open '{}' for the texture hash list", path);
			}

			if (!file || written >= max_lines)
				return;

			++written;
			std::fputs(line.c_str(), file);
			std::fputc('\n', file);
			std::fflush(file);
		}

		// Decodes the source's texels out of GS local memory into tightly packed BGRA8.
		// Same route as GSTextureReplacements::DumpTexture (GSTextureReplacements.cpp:801-844),
		// so what Remix gets is byte-for-byte what a PCSX2 texture dump would contain, modulo
		// the alpha expansion and the channel order Remix wants.
		bool decode(const GSTextureCache::Source& source, std::vector<u8>& out_pixels, u32& out_width, u32& out_height)
		{
			const GIFRegTEX0& TEX0 = source.m_TEX0;
			const GIFRegTEXA& TEXA = source.m_TEXA;
			const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];

			if (!psm.rtx)
				return false;

			const GSTextureCache::SourceRegion region = source.m_region;
			const int tw = region.HasX() ? region.GetWidth() : (1 << TEX0.TW);
			const int th = region.HasY() ? region.GetHeight() : (1 << TEX0.TH);

			if (tw <= 0 || th <= 0 || tw > 2048 || th > 2048)
				return false;

			const GSVector4i rect(region.GetRect(tw, th));
			const GSVector4i block_rect(rect.ralign<Align_Outside>(psm.bs));
			const int read_width = block_rect.width();
			const int read_height = block_rect.height();

			if (read_width <= 0 || read_height <= 0)
				return false;

			// Block-aligned, so read_width is a multiple of the format's block width and the
			// pitch is a multiple of 32 for every PSM -- the same assumption DumpTexture makes.
			const u32 pitch = static_cast<u32>(read_width) * sizeof(u32);
			const size_t needed = static_cast<size_t>(pitch) * static_cast<size_t>(read_height);

			if (!reserve_decode_buffer(needed))
				return false;

			GSLocalMemory& mem = g_gs_renderer->m_mem;
			psm.rtx(mem, mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM), block_rect,
				s_decode_buffer, static_cast<int>(pitch), TEXA);

			// rtx() writes the whole block-aligned rect; the texture itself is the sub-rect.
			const u32 offset = (static_cast<u32>(rect.top - block_rect.top) * pitch) +
			                   (static_cast<u32>(rect.left - block_rect.left) * sizeof(u32));

			out_pixels.resize(static_cast<size_t>(tw) * static_cast<size_t>(th) * 4);

			const bool alpha_expand = expand_alpha();
			u8* dst = out_pixels.data();

			for (int y = 0; y < th; ++y)
			{
				const u8* src = s_decode_buffer + offset + (static_cast<size_t>(y) * pitch);

				for (int x = 0; x < tw; ++x, src += 4, dst += 4)
				{
					// rtx() emits RGBA8 (GSTexture::Format::Color). Remix's proven upload format
					// is BGRA8, so swap R and B here rather than trusting the RGBA enum path.
					dst[0] = src[2];
					dst[1] = src[1];
					dst[2] = src[0];
					dst[3] = alpha_expand ? static_cast<u8>(std::min<u32>(255u, static_cast<u32>(src[3]) * 2u)) : src[3];
				}
			}

			out_width = static_cast<u32>(tw);
			out_height = static_cast<u32>(th);
			return true;
		}

		bool create(const runtime& rt, u64 content_hash, const GSTextureCache::Source& source,
			const GSTextureCache::HashCacheKey& key, material_entry& entry)
		{
			// One-shot on the first create, because MATSTAGE decides whether this function hands
			// Remix anything at all and NOTHING else in the log says what it is.
			//
			// MEASURED 2026-08-06, and it cost most of a session: SOCOM was rendering untextured
			// with `replacement dds 0 probe 0`, which reads as "the replacement route is broken".
			// It was not. `gamesettings\SCUS-97545_D7CFDCCF.ini` -- PCSX2's per-game settings
			// overlay, a config layer distinct from `<SERIAL>.conf` -- carried a stale
			// `[Remix] MATSTAGE = 1` from an old bisection, so every create returned at the
			// `material_stage() < 2` gate below: no CreateTexture, no CreateMaterial, every
			// surface shaded by a null material. Clearing it took the same scene from
			// `replacement dds 0` to `replacement dds 1 probe 1` with no other change.
			//
			// Note the precedence trap this exposes: the overlay reaches the backend through
			// apply_knob() -> the process environment -> apply_before_runtime_load(), which runs
			// BEFORE refresh_game_config() reads `<SERIAL>.conf`. The conf then skips those keys
			// as "already set in the environment", so the settings overlay silently OUTRANKS the
			// per-game conf -- the reverse of the documented order.
			static bool logged_stage = false;
			if (!logged_stage)
			{
				logged_stage = true;
				INFO_LOG("Remix: first material create -- MATSTAGE {} (env '{}')", material_stage(),
					StringUtil::WideStringToUTF8String(read_env(L"PCSX2_REMIX_MATSTAGE")));
			}

			const Common::Timer::Value decode_start = Common::Timer::GetCurrentValue();
			const bool decoded = decode(source, entry.pixels, entry.width, entry.height);
			s_stats.decode_ticks += Common::Timer::GetCurrentValue() - decode_start;

			if (!decoded)
			{
				++s_stats.skip_unsupported;
				return false;
			}

			if (material_stage() < 2)
			{
				++s_stats.created;
				return true; // hashed and decoded, but nothing handed to the runtime
			}

			const remixapi_Interface& api = rt.api();

			remixapi_TextureInfo info{};
			info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
			info.pNext = nullptr;
			info.hash = content_hash;
			info.width = entry.width;
			info.height = entry.height;
			info.depth = 1;
			info.mipLevels = 1;
			info.format = textures_linear() ? REMIXAPI_FORMAT_B8G8R8A8_UNORM : REMIXAPI_FORMAT_B8G8R8A8_SRGB;
			info.data = entry.pixels.data();
			info.dataSize = entry.pixels.size();

			// Under the probe, paint EVERY uploaded texture magenta. The untextured-material variant
			// only covers draws with no texture at all, which Rainbow Six 3 barely has -- and R6 3 is
			// the only title reliable enough to test on (50% launch survival, 30 s sessions) since
			// SOCOM''s crash rate beat 12 straight attempts. Any magenta on screen means an
			// API-uploaded texture reached a surface.
			if (albedo_probe())
			{
				for (size_t px = 0; px + 3 < entry.pixels.size(); px += 4)
				{
					entry.pixels[px + 0] = 0xFF; // B
					entry.pixels[px + 1] = 0x00; // G
					entry.pixels[px + 2] = 0xFF; // R
				}
			}

			const u32 tex_status = guarded_create_texture(api.CreateTexture, &info, &entry.texture);
			if (tex_status != REMIXAPI_ERROR_CODE_SUCCESS || !entry.texture)
			{
				ERROR_LOG("Remix: CreateTexture failed for {}x{} hash {:016x} ({})",
					entry.width, entry.height, content_hash, error_name(tex_status));
				entry.texture = nullptr;
				++s_stats.failures;
				return false;
			}

			if (material_stage() < 3)
			{
				++s_stats.created;
				return true; // texture uploaded; the mesh still goes out with a null material
			}

			// The pseudo-path trick (RPCS3 RemixTextures.cpp:643-648). The fork resolves this
			// string against the texture manager's hash table that remixapi_CreateTexture just
			// populated, which is what puts an API texture in the same hash namespace as a
			// native D3D9 one. Stock Remix hashes material textures by path string only, so
			// this resolves to nothing there and no modding surface exists -- hence the
			// fork_features() gate at the call site.
			wchar_t albedo_path[32]{};
			::swprintf_s(albedo_path, L"0x%016llX", static_cast<unsigned long long>(content_hash));

			// ...except it does NOT resolve in the deployed runtime, so on its own no texture ever
			// reaches a surface. MEASURED: with the shared untextured material's 4x4 filled MAGENTA
			// and albedoConstant left white, SOCOM -- where ~76% of draws use it -- renders
			// mean_sat 0.038 with 0.11% coloured pixels. No magenta. A late material rebuild
			// (PCSX2_REMIX_MATREBUILD=120) changes nothing, so it is not an ordering or lifetime race.
			//
			// So prefer a REAL FILE PATH when the user has a replacement pack. Remix resolves
			// albedoTexture through its asset loader, which handles .dds, and PCSX2's replacement map
			// is keyed on the very HashCacheKey we just computed -- an exact lookup, not a guess. It
			// also renders the user's UPSCALED texture instead of the 8-bit original.
			//
			// .dds only: findAsset rejects anything else outright ("use the RTX-Remix toolkit and
			// ingest the following asset"), so a PNG replacement would silently bind nothing and be
			// worse than leaving the pseudo-path in place.
			std::wstring replacement_path;
			if (replacement_albedo())
			{
				++s_stats.replacement_probe;

				if (const std::string* path = GSTextureReplacements::GetReplacementTexturePath(key))
				{
					if (StringUtil::EndsWithNoCase(*path, ".dds"))
					{
						replacement_path = StringUtil::UTF8StringToWideString(*path);
						entry.albedo_file = replacement_path;
						++s_stats.replacement_albedo;
					}
					else
					{
						++s_stats.replacement_not_dds;
					}
				}
			}

			remixapi_MaterialInfoOpaqueEXT opaque{};
			opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
			opaque.pNext = nullptr;
			opaque.roughnessTexture = nullptr;
			opaque.metallicTexture = nullptr;
			opaque.anisotropy = 0.f;
			opaque.albedoConstant = {1.f, 1.f, 1.f};
			opaque.opacityConstant = 1.f;
			opaque.roughnessConstant = legacy_roughness();
			opaque.metallicConstant = 0.f;
			opaque.thinFilmThickness_hasvalue = 0;
			opaque.thinFilmThickness_value = 0.f;
			opaque.alphaIsThinFilmThickness = 0;
			opaque.heightTexture = nullptr;
			opaque.displaceIn = 0.f;
			// 0 when the caller is not chaining an InstanceInfoBlendEXT: claiming to supply
			// draw-call alpha state and then not supplying it is what made every ABE=1 surface
			// blend against nothing. See alpha_state_mode() in RemixSubmit.cpp.
			opaque.useDrawCallAlphaState = (alpha_state_mode() != 0) ? 1 : 0;
			opaque.blendType_hasvalue = 0;
			opaque.blendType_value = 0;
			opaque.invertedBlend = 0;
			opaque.alphaTestType = 7; // always
			opaque.alphaReferenceValue = 0;
			opaque.displaceOut = 0.f;

			// Emissive. The texture is pointed at the *same* pseudo-path as the albedo, so the
			// bulb's own texels shape what it radiates instead of the whole quad glowing flat.
			// This is what turns a tagged fixture into an actual light source: WORLD_UI alone
			// makes it render bright but contributes nothing to the path tracer, which is why
			// the level still needed the follow-cam debug light.
			const bool is_emissive = (material_stage() >= 4) && (s_emissive.count(content_hash) != 0);
			const float intensity = is_emissive ? emissive_intensity() : 0.f;

			remixapi_MaterialInfo material{};
			material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
			material.pNext = &opaque;
			material.hash = content_hash;
			material.albedoTexture = (material_stage() >= 4)
				? (replacement_path.empty() ? albedo_path : replacement_path.c_str())
				: nullptr;
			material.normalTexture = nullptr;
			material.tangentTexture = nullptr;
			material.emissiveTexture = is_emissive ? albedo_path : nullptr;
			material.emissiveIntensity = intensity;
			material.emissiveColorConstant = is_emissive ? remixapi_Float3D{1.f, 1.f, 1.f} : remixapi_Float3D{0.f, 0.f, 0.f};
			material.spriteSheetRow = 1;
			material.spriteSheetCol = 1;
			material.spriteSheetFps = 0;
			material.filterMode = 1; // linear
			// PS2 CLAMP is per-draw state and the source already baked the region for the
			// clamped case; repeat is the right default for everything that got this far.
			material.wrapModeU = 1;
			material.wrapModeV = 1;

			const u32 mat_status = guarded_create_material(api.CreateMaterial, &material, &entry.material);
			if (mat_status != REMIXAPI_ERROR_CODE_SUCCESS || !entry.material)
			{
				ERROR_LOG("Remix: CreateMaterial failed for hash {:016x} ({})", content_hash, error_name(mat_status));
				guarded_destroy_texture(api.DestroyTexture, entry.texture);
				entry.texture = nullptr;
				entry.material = nullptr;
				++s_stats.failures;
				return false;
			}

			entry.generation = s_generation;

			if (is_emissive)
				INFO_LOG("Remix: material {:016X} is emissive (intensity {:g})", content_hash, intensity);

			++s_stats.created;
			return true;
		}

		// Rebuilds only the material of an existing entry, keeping its texture. Used when the
		// tag lists change: the old handle is retired rather than destroyed, because meshes
		// created earlier still hold the handle Remix bound into them at CreateMesh time.
		void rebuild_material(const runtime& rt, u64 content_hash, material_entry& entry, u64 frame)
		{
			if (!entry.texture || entry.failed || material_stage() < 3)
			{
				entry.generation = s_generation;
				return;
			}

			const remixapi_Interface& api = rt.api();

			// Re-upload the texture immediately before rebuilding the material.
			//
			// RtxTextureManager::addTexture is a PER-FRAME registration -- its own header calls the
			// preserve path "re-run addTexture for the bindless slot so THIS FRAME's texture table
			// matches the dynamic path" -- and getTextureTable() returns that frame's table. The
			// fork's createTexture calls addTexture exactly ONCE, so an API texture is present in the
			// table only for the frame it was uploaded on, while the material's albedo pseudo-path is
			// resolved against that table once, at finalization.
			//
			// That is the mechanism behind the user seeing colour "live a frame or two": a material
			// whose finalization landed in the same frame as its texture upload resolved and looked
			// right, and every other one bound nothing. It is also why a late rebuild alone
			// (MATREBUILD=120) changed nothing -- by then the table had been rebuilt many times
			// without our texture in it. Re-uploading first is what gives the rebuild a table to
			// find; the decoded pixels are still resident on the entry for exactly this purpose.
			if (texture_reupload_on_rebuild())
			{
				remixapi_TextureInfo rinfo{};
				rinfo.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
				rinfo.pNext = nullptr;
				rinfo.hash = content_hash;
				rinfo.width = entry.width;
				rinfo.height = entry.height;
				rinfo.depth = 1;
				rinfo.mipLevels = 1;
				rinfo.format = textures_linear() ? REMIXAPI_FORMAT_B8G8R8A8_UNORM : REMIXAPI_FORMAT_B8G8R8A8_SRGB;
				rinfo.data = entry.pixels.data();
				rinfo.dataSize = entry.pixels.size();

				remixapi_TextureHandle refreshed = nullptr;
				if (guarded_create_texture(api.CreateTexture, &rinfo, &refreshed) == REMIXAPI_ERROR_CODE_SUCCESS && refreshed)
				{
					entry.texture = refreshed;
					++s_stats.texture_reuploads;
				}
			}

			wchar_t albedo_path[32]{};
			::swprintf_s(albedo_path, L"0x%016llX", static_cast<unsigned long long>(content_hash));

			const bool is_emissive = (material_stage() >= 4) && (s_emissive.count(content_hash) != 0);
			const float intensity = is_emissive ? emissive_intensity() : 0.f;

			remixapi_MaterialInfoOpaqueEXT opaque{};
			opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
			opaque.albedoConstant = {1.f, 1.f, 1.f};
			opaque.opacityConstant = 1.f;
			opaque.roughnessConstant = legacy_roughness();
			opaque.metallicConstant = 0.f;
			opaque.useDrawCallAlphaState = (alpha_state_mode() != 0) ? 1 : 0;
			opaque.alphaTestType = 7;

			remixapi_MaterialInfo material{};
			material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
			material.pNext = &opaque;
			material.hash = content_hash;
			material.albedoTexture = (material_stage() >= 4)
				? (entry.albedo_file.empty() ? albedo_path : entry.albedo_file.c_str())
				: nullptr;
			material.emissiveTexture = is_emissive ? albedo_path : nullptr;
			material.emissiveIntensity = intensity;
			material.emissiveColorConstant = is_emissive ? remixapi_Float3D{1.f, 1.f, 1.f} : remixapi_Float3D{0.f, 0.f, 0.f};
			material.spriteSheetRow = 1;
			material.spriteSheetCol = 1;
			material.filterMode = 1;
			material.wrapModeU = 1;
			material.wrapModeV = 1;

			remixapi_MaterialHandle replacement = nullptr;
			const u32 status = guarded_create_material(api.CreateMaterial, &material, &replacement);

			entry.generation = s_generation;

			if (status != REMIXAPI_ERROR_CODE_SUCCESS || !replacement)
			{
				ERROR_LOG("Remix: CreateMaterial failed rebuilding {:016X} ({})", content_hash, error_name(status));
				return;
			}

			if (entry.material)
				s_retired.push_back({entry.material, frame, entry.is_render_target});

			entry.material = replacement;

			if (is_emissive)
				INFO_LOG("Remix: material {:016X} is now emissive (intensity {:g})", content_hash, intensity);
		}

		void destroy(const runtime& rt, material_entry& entry)
		{
			const remixapi_Interface& api = rt.api();

			// Material first: it references the texture through the pseudo-path.
			if (entry.material)
			{
				guarded_destroy_material(api.DestroyMaterial, entry.material);
				entry.material = nullptr;
			}

			if (entry.texture)
			{
				guarded_destroy_texture(api.DestroyTexture, entry.texture);
				entry.texture = nullptr;
				++s_stats.destroyed;
			}
		}
	} // namespace

	// Set by invalidate_game_config() so the next refresh re-applies immediately instead of
	// waiting out the poll interval. GS thread only, like everything else in this file.
	bool s_game_config_dirty = true;

	struct ca_runtime_setting
	{
		const char* key;
		const char* backend_default;
	};

	static constexpr ca_runtime_setting s_ca_runtime_settings[] = {
		{"rtx.fallbackLightMode", "1"},
		{"rtx.antiCulling.object.enable", "False"},
		{"rtx.antiCulling.object.enableHighPrecisionAntiCulling", "True"},
		{"rtx.antiCulling.object.enableInfinityFarFrustum", "False"},
		{"rtx.antiCulling.object.farPlaneScale", "10"},
	};

	std::unordered_map<std::string, std::string> capture_ca_runtime_baseline()
	{
		std::unordered_map<std::string, std::string> result;
		for (const ca_runtime_setting& setting : s_ca_runtime_settings)
			result.emplace(setting.key, setting.backend_default);

		// SetConfigVariable writes the runtime's user layer and has no getter or unset operation.
		// Preserve any value from the rtx.conf layer that was actually loaded at startup, then
		// write it back when CA is left. This avoids replacing a user's global/per-game tuning
		// with our compiled guesses merely because SCUS-97545 ran once in this process.
		std::string config_paths = StringUtil::WideStringToUTF8String(read_env(L"DXVK_RTX_CONFIG_FILE"));
		if (config_paths.empty())
			config_paths = "rtx.conf";

		std::wstring module_path(32768, L'\0');
		const DWORD module_length = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
		module_path.resize((module_length > 0 && module_length < module_path.size()) ? module_length : 0);
		const std::string module_path_utf8 = StringUtil::WideStringToUTF8String(module_path);
		const std::string executable_name(Path::GetFileName(module_path_utf8));

		size_t begin = 0;
		while (begin <= config_paths.size())
		{
			const size_t comma = config_paths.find(',', begin);
			const std::string path = config_paths.substr(begin,
				(comma == std::string::npos) ? std::string::npos : comma - begin);
			begin = (comma == std::string::npos) ? config_paths.size() + 1 : comma + 1;

			std::FILE* file = path.empty() ? nullptr : FileSystem::OpenCFile(path.c_str(), "r");
			if (!file)
				continue;

			bool active_section = true;
			char line[8192];
			while (std::fgets(line, sizeof(line), file))
			{
				std::string text(line);
				if (const size_t hash_pos = text.find('#'); hash_pos != std::string::npos)
					text.erase(hash_pos);

				const size_t first_non_space = text.find_first_not_of(" \t\r\n");
				if (first_non_space != std::string::npos && text[first_non_space] == '[')
				{
					const size_t close = text.find_last_of(']');
					active_section = close > first_non_space + 1 &&
						text.substr(first_non_space + 1, close - first_non_space - 1) == executable_name;
					continue;
				}
				if (!active_section)
					continue;

				const size_t eq = text.find('=');
				if (eq == std::string::npos)
					continue;

				const auto trim = [](std::string s) {
					const size_t first = s.find_first_not_of(" \t\r\n");
					const size_t last = s.find_last_not_of(" \t\r\n");
					return (first == std::string::npos) ? std::string() : s.substr(first, last - first + 1);
				};
				const std::string key = trim(text.substr(0, eq));
				if (auto it = result.find(key); it != result.end())
				{
					std::string value = trim(text.substr(eq + 1));
					value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
					if (!value.empty())
						it->second = value;
				}
			}
			std::fclose(file);
		}

		return result;
	}

	bool restore_ca_runtime_baseline(const runtime& rt,
		const std::unordered_map<std::string, std::string>& baseline)
	{
		bool success = true;
		for (const ca_runtime_setting& setting : s_ca_runtime_settings)
		{
			const auto it = baseline.find(setting.key);
			const std::string value = (it == baseline.end()) ? setting.backend_default : it->second;
			const u32 code = guarded_set_config_variable(rt.api().SetConfigVariable, setting.key, value.c_str());
			if (code == REMIXAPI_ERROR_CODE_SUCCESS)
				INFO_LOG("Remix:   {} = {} (restored pre-CA runtime value)", setting.key, value);
			else
			{
				success = false;
				WARNING_LOG("Remix:   {} = {} RESTORE FAILED ({})", setting.key, value, error_name(code));
			}
		}
		return success;
	}

	bool cpu_pixels(u64 content_hash, const u8*& out_pixels, u32& out_width, u32& out_height)
	{
		const auto it = s_entries.find(content_hash);
		if (it == s_entries.end() || it->second.pixels.empty())
			return false;

		out_pixels = it->second.pixels.data();
		out_width = it->second.width;
		out_height = it->second.height;
		return (out_width != 0) && (out_height != 0);
	}

	void begin_frame()
	{
		s_budget_left = texture_budget();
		if (s_rt_readback_block_frames > 0)
			--s_rt_readback_block_frames;
	}

	void invalidate_game_config()
	{
		s_game_config_dirty = true;
	}

	void on_state_loaded(const runtime& rt)
	{
		for (auto it = s_entries.begin(); it != s_entries.end(); )
		{
			if (!it->second.is_render_target)
			{
				++it;
				continue;
			}

			destroy(rt, it->second);
			it = s_entries.erase(it);
		}

		if (rt.ok())
		{
			s_retired.erase(std::remove_if(s_retired.begin(), s_retired.end(),
				[&](const retired_material& entry) {
					if (!entry.is_render_target)
						return false;
					guarded_destroy_material(rt.api().DestroyMaterial, entry.handle);
					return true;
				}), s_retired.end());
		}

		// The first draw window after a load still observes the rebuilding GS texture cache. The
		// following begin_frame() releases the gate, so the second window may take the snapshot.
		s_rt_readback_block_frames = 1;
	}

	void refresh_game_config(const runtime& rt)
	{
		// Same cadence and the same identity-plus-mtime signature as refresh_categories, so a
		// user can tune a per-game conf while the game runs, and so a game change re-applies.
		static Common::Timer::Value last_check = 0;
		static u64 last_signature = 0;
		static bool first = true;
		static std::string configured_game;
		static bool configured_ca_profile = false;
		static std::unordered_map<std::string, std::string> ca_runtime_baseline;

		const Common::Timer::Value now = Common::Timer::GetCurrentValue();

		// A save-state load can switch to a different game, and the once-a-second poll would
		// leave up to a second of frames running the previous title's settings. The hook makes
		// it immediate.
		if (s_game_config_dirty)
		{
			s_game_config_dirty = false;
			first = true;
		}

		if (!first && Common::Timer::ConvertValueToMilliseconds(now - last_check) < 1000.0)
			return;

		last_check = now;

		const std::string current_game_id = remix_ps2::paths::game_id();
		const bool next_ca_profile = current_game_id == "SCUS-97545";
		const std::vector<std::string> paths = game_conf_paths();

		u64 signature = fnv_seed;
		for (const char c : current_game_id)
			signature = fnv_mix(signature, static_cast<u64>(static_cast<unsigned char>(c)));
		for (const std::string& path : paths)
		{
			// The identity itself is part of the signature: two games can both have no conf
			// file, and switching between them must still be seen as a change.
			for (const char c : path)
				signature = fnv_mix(signature, static_cast<u64>(static_cast<unsigned char>(c)));

			FILESYSTEM_STAT_DATA sd{};
			if (FileSystem::StatFile(path.c_str(), &sd))
			{
				signature = fnv_mix(signature, static_cast<u64>(sd.ModificationTime));
				signature = fnv_mix(signature, static_cast<u64>(sd.Size));
			}
			else
			{
				signature = fnv_mix(signature, 0);
			}
		}

		if (!first && signature == last_signature)
			return;

		first = false;
		last_signature = signature;
		if (!remix_ps2::paths::clear_title_env())
		{
			// A transient Win32 environment failure must not turn into a permanent stale
			// per-title override merely because the config-file signature did not change.
			s_game_config_dirty = true;
		}

		// Runtime settings land in Remix's user layer, which has no public unset operation. CA is
		// the only profile that promises clean removal: restore the rtx.conf values captured
		// before its first override, including when the next state has no serial/config paths.
		if (configured_ca_profile && !next_ca_profile)
		{
			if (!restore_ca_runtime_baseline(rt, ca_runtime_baseline))
			{
				// Keep ownership and retry on the next refresh instead of forgetting a partial
				// restore and silently leaving CA values active for another title.
				s_game_config_dirty = true;
				return;
			}
			configured_ca_profile = false;
			ca_runtime_baseline.clear();
		}

		if (paths.empty())
		{
			configured_game.clear();
			return;
		}

		const std::string game = paths.back();
		if (!configured_game.empty() && configured_game != game)
		{
			WARNING_LOG("Remix: per-game config changing from '{}' to '{}' in one process. "
						"Combined Assault runtime keys are restored; other titles' runtime keys still "
						"require an emulator restart because Remix exposes no unset operation.",
				Path::GetFileName(configured_game), Path::GetFileName(game));
		}

		if (!configured_ca_profile && next_ca_profile)
			ca_runtime_baseline = capture_ca_runtime_baseline();

		configured_game = game;
		configured_ca_profile = next_ca_profile;

		u32 found = 0;
		u32 applied = 0;
		u32 failed = 0;
		u32 env_applied = 0;
		u32 env_skipped = 0;

		for (const std::string& path : paths)
		{
			std::FILE* file = FileSystem::OpenCFile(path.c_str(), "r");
			if (!file)
				continue;

			++found;
			INFO_LOG("Remix: per-game config '{}'", path);

			char line[8192];
			while (std::fgets(line, sizeof(line), file))
			{
				std::string text(line);

				if (const size_t hash_pos = text.find('#'); hash_pos != std::string::npos)
					text.erase(hash_pos);

				const size_t eq = text.find('=');
				if (eq == std::string::npos)
					continue;

				const auto trim = [](std::string s) {
					const size_t f = s.find_first_not_of(" \t\r\n");
					const size_t l = s.find_last_not_of(" \t\r\n");
					return (f == std::string::npos) ? std::string() : s.substr(f, l - f + 1);
				};

				const std::string key = trim(text.substr(0, eq));
				const std::string value = trim(text.substr(eq + 1));
				if (key.empty())
					continue;

				// Our own knobs, spelled exactly as the environment variables they already are,
				// because that is the spelling every note and toggle table in this project uses.
				// This is where per-title light intensity, far plane and emissive settings
				// belong -- it retires "constants calibrated for Rainbow Six 3 are meaningless
				// on SOCOM" properly instead of by deriving them.
				if (key.rfind("PCSX2_REMIX_", 0) == 0)
				{
					// A real environment variable always wins. Every A/B arm this project runs
					// sets these from the harness, and a per-game conf silently overriding one
					// mid-arm would invalidate the measurement without saying so.
					//
					if (remix_ps2::paths::is_external_env(key))
					{
						++env_skipped;
						INFO_LOG("Remix:   {} = {} SKIPPED (external process environment)", key, value);
						continue;
					}

					if (remix_ps2::paths::apply_title_env(key, value))
					{
						++env_applied;
					}
					else
					{
						++failed;
						WARNING_LOG("Remix:   {} = {} FAILED to set", key, value);
					}

					continue;
				}

				// Category and emissive hash lists are deliberately NOT pushed here: they are
				// ours, not the runtime's, and conf_paths() already feeds these same files to
				// load_categories(), which has the digit-group-comma parser. Pushing them would
				// double-apply and, worse, hand the runtime a key it does not know.
				if (key == "rtx.pcsx2EmissiveTextures" || key == "pcsx2.emissiveTextures")
					continue;

				const u32 code = guarded_set_config_variable(
					rt.api().SetConfigVariable, key.c_str(), value.c_str());

				if (code == REMIXAPI_ERROR_CODE_SUCCESS)
				{
					++applied;
					INFO_LOG("Remix:   {} = {} (per-game conf)", key, value);
				}
				else
				{
					++failed;
					WARNING_LOG("Remix:   {} = {} FAILED ({})", key, value, error_name(code));
				}
			}

			std::fclose(file);
		}

		if (found == 0)
		{
			// Named explicitly, because "no per-game config" and "per-game config with a typo in
			// the filename" look identical otherwise, and that class of silence has cost this
			// project two rounds already.
			INFO_LOG("Remix: no per-game config found. Create one of these to add per-title "
					 "settings, tags and toggles:");
			for (const std::string& path : paths)
				INFO_LOG("Remix:   {}", path);

			return;
		}

		INFO_LOG("Remix: per-game config applied -- {} runtime keys, {} PCSX2 toggles, "
				 "{} skipped (environment wins), {} failed",
			applied, env_applied, env_skipped, failed);
		const auto effective_env = [](std::string_view name, const char* fallback) {
			std::string value = remix_ps2::paths::env_value(name);
			return value.empty() ? std::string(fallback) : value;
		};
		INFO_LOG("Remix: effective LIGHTMODE={} [{}] WORLDROT={} [{}] SKY={} [{}] SKYMINW={} [{}] "
				 "SKYCAM={} [{}] RTTEX={} [{}] MATSTAGE={} [{}] UIMODE={} [{}] UIRASTER={} [{}] "
				 "UIWMAX={} [{}]",
			effective_env("PCSX2_REMIX_LIGHTMODE", "1"),
			remix_ps2::paths::env_source("PCSX2_REMIX_LIGHTMODE"),
			effective_env("PCSX2_REMIX_WORLDROT", "0"),
			remix_ps2::paths::env_source("PCSX2_REMIX_WORLDROT"),
			effective_env("PCSX2_REMIX_SKY", "1"),
			remix_ps2::paths::env_source("PCSX2_REMIX_SKY"),
			effective_env("PCSX2_REMIX_SKYMINW", "0"),
			remix_ps2::paths::env_source("PCSX2_REMIX_SKYMINW"),
			effective_env("PCSX2_REMIX_SKYCAM", "1"),
			remix_ps2::paths::env_source("PCSX2_REMIX_SKYCAM"),
			effective_env("PCSX2_REMIX_RTTEX", "0"),
			remix_ps2::paths::env_source("PCSX2_REMIX_RTTEX"),
			effective_env("PCSX2_REMIX_MATSTAGE", "4"),
			remix_ps2::paths::env_source("PCSX2_REMIX_MATSTAGE"),
			effective_env("PCSX2_REMIX_UIMODE", "0"),
			remix_ps2::paths::env_source("PCSX2_REMIX_UIMODE"),
			effective_env("PCSX2_REMIX_UIRASTER", "0"),
			remix_ps2::paths::env_source("PCSX2_REMIX_UIRASTER"),
			effective_env("PCSX2_REMIX_UIWMAX", "50"),
			remix_ps2::paths::env_source("PCSX2_REMIX_UIWMAX"));
	}

	void refresh_categories()
	{
		// The developer menu writes the conf files live, so polling is what makes a tag take
		// effect without restarting the emulator. One stat per layer per second is nothing.
		static Common::Timer::Value last_check = 0;
		static u64 last_signature = 0;
		static bool first = true;

		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (!first && Common::Timer::ConvertValueToMilliseconds(now - last_check) < 1000.0)
			return;

		last_check = now;

		u64 signature = fnv_seed;
		for (const std::string& path : conf_paths())
		{
			FILESYSTEM_STAT_DATA sd{};
			if (FileSystem::StatFile(path.c_str(), &sd))
			{
				signature = fnv_mix(signature, static_cast<u64>(sd.ModificationTime));
				signature = fnv_mix(signature, static_cast<u64>(sd.Size));
			}
			else
			{
				signature = fnv_mix(signature, 0);
			}
		}

		if (!first && signature == last_signature)
			return;

		first = false;
		last_signature = signature;

		load_categories();
		++s_generation;

		INFO_LOG("Remix: {} texture category tags and {} emissive tags loaded from the Remix conf layers",
			s_category_tags, s_emissive.size());

		// Printed so a tag that silently failed to parse is visible as a hash that does not
		// match anything in remix_textures.txt, rather than as "nothing happened".
		for (const auto& [hash, flags] : s_categories)
			INFO_LOG("Remix:   tag {:016X} -> categoryFlags 0x{:X}", hash, flags);

		for (const u64 hash : s_emissive)
			INFO_LOG("Remix:   tag {:016X} -> emissive", hash);
	}

	u64 generation()
	{
		return s_generation;
	}

	remixapi_InstanceCategoryFlags tagged_category_mask()
	{
		return s_category_mask;
	}

	remixapi_InstanceCategoryFlags categories_for(u64 content_hash)
	{
		if (s_categories.empty() || content_hash == 0)
			return 0;

		const auto it = s_categories.find(content_hash);
		if (it == s_categories.end())
			return 0;

		++s_category_hits;
		return it->second;
	}

	u64 hash_only(const GSTextureCache::Source* source)
	{
		if (!source || source->m_target || source->m_from_target)
			return 0;

		const GIFRegTEX0& TEX0 = source->m_TEX0;
		const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];

		// Identical to the computation in bind(), deliberately -- a hash that did not match would be
		// useless for tagging, since the emissive and category lists key on bind()'s value.
		const u32* clut = (psm.pal > 0) ? static_cast<const u32*>(g_gs_renderer->m_mem.m_clut) : nullptr;
		const GSVector2i* lod =
			(GSConfig.HWMipmap || GSConfig.TriFilter == TriFiltering::Forced) ? &source->m_lod : nullptr;

		const GSTextureCache::HashCacheKey key =
			GSTextureCache::HashCacheKey::Create(TEX0, source->m_TEXA, clut, lod, source->m_region);

		u64 content_hash = fnv_seed;
		{
			const u64* words = reinterpret_cast<const u64*>(&key);
			for (u32 i = 0; i < (sizeof(GSTextureCache::HashCacheKey) / sizeof(u64)); ++i)
				content_hash = fnv_mix(content_hash, words[i]);
		}

		return (content_hash == 0) ? 1 : content_hash;
	}

	binding bind_untextured(const runtime& rt)
	{
		// A fixed, arbitrary hash. It is not a content hash -- there is no content -- but it must
		// be stable and distinct, because the caller folds it into the mesh identity and Remix
		// binds the material at CreateMesh time. "UNTEXTRD" in ASCII, so it is recognisable in a
		// log and cannot collide with a real GS texture hash.
		static constexpr u64 untextured_hash = 0x554E544558545244ull;

		static remixapi_MaterialHandle s_untextured = nullptr;
		static remixapi_TextureHandle s_untextured_tex = nullptr;
		static bool s_tried = false;

		binding out{};

		// fork_features() IS required: the albedo below is bound through the fork's pseudo-path
		// resolution, exactly as the textured path does. No texture *budget* gate though -- this is
		// one 4x4 upload for the whole session, not per-draw churn.
		if (!rt.ok() || !rt.fork_features())
			return out;

		if (!s_untextured && !s_tried)
		{
			s_tried = true;

			const remixapi_Interface& api = rt.api();

			// A material with albedoTexture = nullptr renders BLACK -- albedoConstant alone is not
			// enough. MEASURED: with no albedo texture named, the albedo debug view
			// (DXVK_RTX_DEBUG_VIEW_INDEX=23) reads lit_px 0 across an entire SOCOM mission, while
			// Rainbow Six 3 in the same view on the same build reads lit_px 2,421,139 at mean
			// luminance 111 -- so the view works and the surfaces genuinely had zero albedo. Since
			// UNTEXZ these are ~76% of a SOCOM frame, so most of the world was black with only
			// specular highlights on it. That is the "black/white" look, and no injected light
			// could have fixed it.
			//
			// This is what material_stage() == 3 exists to bisect: "CreateMaterial, but with no
			// albedo texture named". So name one: 4x4 opaque white, uploaded once per session, and
			// let the vertex diffuse and the path tracer supply the actual colour and lighting.
			u8 white[4 * 4 * 4];
			std::memset(white, 0xFF, sizeof(white));

			// ALBEDOPROBE puts the distinctive colour in the TEXTURE rather than the constant, which
			// is the only way round that actually discriminates. Magenta constant + white texture
			// cannot tell "texture resolved" from "lookup failed and defaulted to white" -- both
			// render white. Magenta TEXTURE + white constant can: magenta means the pseudo-path
			// resolved, white means it did not. B,G,R,A order.
			if (albedo_probe())
			{
				for (u32 px = 0; px < 4 * 4; ++px)
				{
					white[px * 4 + 0] = 0xFF; // B
					white[px * 4 + 1] = 0x00; // G
					white[px * 4 + 2] = 0xFF; // R
					white[px * 4 + 3] = 0xFF; // A
				}
			}

			remixapi_TextureInfo tex{};
			tex.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
			tex.pNext = nullptr;
			tex.hash = untextured_hash;
			tex.width = 4;
			tex.height = 4;
			tex.depth = 1;
			tex.mipLevels = 1;
			tex.format = textures_linear() ? REMIXAPI_FORMAT_B8G8R8A8_UNORM : REMIXAPI_FORMAT_B8G8R8A8_SRGB;
			tex.data = white;
			tex.dataSize = sizeof(white);

			const u32 tex_status = guarded_create_texture(api.CreateTexture, &tex, &s_untextured_tex);
			if (tex_status != REMIXAPI_ERROR_CODE_SUCCESS || !s_untextured_tex)
			{
				ERROR_LOG("Remix: CreateTexture failed for the untextured white albedo ({})",
					error_name(tex_status));
				s_untextured_tex = nullptr;
				++s_stats.failures;
				return out;
			}

			remixapi_MaterialInfoOpaqueEXT opaque{};
			opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
			opaque.pNext = nullptr;
			opaque.roughnessTexture = nullptr;
			opaque.metallicTexture = nullptr;
			opaque.anisotropy = 0.f;
			// White, so the per-vertex RGBAQ colour is what decides the surface colour.
			opaque.albedoConstant = {1.f, 1.f, 1.f};
			opaque.opacityConstant = 1.f;
			opaque.roughnessConstant = legacy_roughness();
			opaque.metallicConstant = 0.f;
			opaque.thinFilmThickness_hasvalue = 0;
			opaque.thinFilmThickness_value = 0.f;
			opaque.alphaIsThinFilmThickness = 0;
			opaque.heightTexture = nullptr;
			opaque.displaceIn = 0.f;
			// Must agree with what RemixSubmit chains on the instance, exactly as in the textured
			// path -- claiming draw-call alpha state and not supplying it is what once suppressed
			// every blended surface.
			opaque.useDrawCallAlphaState = (alpha_state_mode() != 0) ? 1 : 0;
			opaque.blendType_hasvalue = 0;
			opaque.blendType_value = 0;
			opaque.invertedBlend = 0;
			opaque.alphaTestType = 7; // always
			opaque.alphaReferenceValue = 0;
			opaque.displaceOut = 0.f;

			// The pseudo-path trick, same as the textured path: the fork resolves this string
			// against the texture manager's hash table that CreateTexture just populated.
			wchar_t albedo_path[32]{};
			::swprintf_s(albedo_path, L"0x%016llX", static_cast<unsigned long long>(untextured_hash));

			remixapi_MaterialInfo material{};
			material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
			material.pNext = &opaque;
			material.hash = untextured_hash;
			material.albedoTexture = albedo_path; // 4x4 white; albedoConstant alone renders black
			material.normalTexture = nullptr;
			material.tangentTexture = nullptr;
			material.emissiveTexture = nullptr;
			material.emissiveIntensity = 0.f;
			material.emissiveColorConstant = remixapi_Float3D{0.f, 0.f, 0.f};
			material.spriteSheetRow = 1;
			material.spriteSheetCol = 1;
			material.spriteSheetFps = 0;
			material.filterMode = 1; // linear
			material.wrapModeU = 1;
			material.wrapModeV = 1;

			const u32 status = guarded_create_material(api.CreateMaterial, &material, &s_untextured);

			if (status != REMIXAPI_ERROR_CODE_SUCCESS || !s_untextured)
			{
				ERROR_LOG("Remix: CreateMaterial failed for the shared untextured material ({})",
					error_name(status));
				guarded_destroy_texture(api.DestroyTexture, s_untextured_tex);
				s_untextured_tex = nullptr;
				s_untextured = nullptr;
				++s_stats.failures;
				return out;
			}

			INFO_LOG("Remix: created the shared untextured material (4x4 white albedo at {}, "
					 "vertex diffuse carries the colour)", "0x554E544558545244");
		}

		if (!s_untextured)
			return out;

		out.material = s_untextured;
		out.content_hash = untextured_hash;
		return out;
	}

	binding bind(const runtime& rt, const GSTextureCache::Source* source, u64 frame,
		bool allow_render_target_snapshot)
	{
		binding out{};

		if (!rt.ok() || !rt.fork_features() || texture_budget() == 0)
			return out;

		++s_stats.binds;

		if (!source)
		{
			++s_stats.skip_no_source;
			return out;
		}

		// A render-to-texture has no stable content identity by construction, so there is
		// nothing a modder could key a replacement on. Counted, not silently dropped.
		//
		// That is right for REPLACEMENT and wrong as a blanket rule, and it cost SOCOM its sky.
		// Winterblade draws the sky as draws 12-13: a 620x264 blended quad sampling a 128x128
		// target with UVs at [-0.417,3.229] -- tiled scrolling clouds. Rejecting it here gave the
		// quad no material at all, so the sky rendered as an untextured null surface and Remix had
		// no environment to light the scene from. 79,084 draws a session hit this path.
		//
		// PCSX2_REMIX_RTTEX = frames between refreshes, 0 = off (the old behaviour).
		const bool is_render_target = (source->m_target || source->m_from_target);
		const u32 rt_interval = rt_texture_interval();

		if (is_render_target && !allow_render_target_snapshot)
		{
			++s_stats.skip_target;
			return out;
		}

		if (is_render_target && (rt_interval == 0 || !source->m_texture || !g_texture_cache))
		{
			++s_stats.skip_target;
			return out;
		}

		if (is_render_target && s_rt_readback_block_frames > 0)
		{
			++s_stats.skip_target;
			return out;
		}

		const GIFRegTEX0& TEX0 = source->m_TEX0;
		const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];

		// Mirrors CreateSource (GSTextureCache.cpp:6608-6609) and the LookupSource call in
		// GSRendererHW.cpp:3545 exactly, so the key we compute is the key PCSX2 itself would
		// have computed for this source -- which is what makes it match a PCSX2 texture dump.
		const u32* clut = (psm.pal > 0) ? static_cast<const u32*>(g_gs_renderer->m_mem.m_clut) : nullptr;
		const GSVector2i* lod =
			(GSConfig.HWMipmap || GSConfig.TriFilter == TriFiltering::Forced) ? &source->m_lod : nullptr;

		const Common::Timer::Value hash_start = Common::Timer::GetCurrentValue();
		const GSTextureCache::HashCacheKey key =
			GSTextureCache::HashCacheKey::Create(TEX0, source->m_TEXA, clut, lod, source->m_region);
		s_stats.hash_ticks += Common::Timer::GetCurrentValue() - hash_start;

		// HashCacheKey is 40 bytes with no padding (static_assert at GSTextureCache.h:121), so
		// folding it whole is well defined.
		u64 content_hash = fnv_seed;
		{
			const u64* words = reinterpret_cast<const u64*>(&key);
			for (u32 i = 0; i < (sizeof(GSTextureCache::HashCacheKey) / sizeof(u64)); ++i)
				content_hash = fnv_mix(content_hash, words[i]);
		}

		// A render target's pixels change every frame -- scrolling clouds, by definition -- so a
		// CONTENT hash would mint a fresh texture and material every single frame. That is exactly
		// the per-frame churn that correlates monotonically with the device-lost hang, so it must
		// not be reintroduced here. Key on the target's IDENTITY instead (where it lives, how big
		// it is) and refresh the pixels of that one entry on an interval.
		if (is_render_target)
		{
			content_hash = fnv_mix(fnv_seed, 0x52545845'52545845ull); // "RTXERTXE" domain separator
			content_hash = fnv_mix(content_hash, TEX0.TBP0);
			content_hash = fnv_mix(content_hash, TEX0.TBW);
			content_hash = fnv_mix(content_hash, TEX0.PSM);
			content_hash = fnv_mix(content_hash, TEX0.TW);
			content_hash = fnv_mix(content_hash, TEX0.TH);
		}

		if (content_hash == 0)
			content_hash = 1; // Remix treats a zero hash as unset

		if (s_seen_content.size() < s_max_measurement_entries && s_seen_content.insert(content_hash).second)
			++s_stats.unique_content;

		if (s_seen_tex0.size() < s_max_measurement_entries && s_seen_tex0.insert(key.TEX0Hash).second)
			++s_stats.unique_tex0;

		auto it = s_entries.find(content_hash);

		if (it == s_entries.end())
		{
			++s_stats.misses;

			if (s_budget_left == 0)
			{
				// Over budget: the draw goes out with no material this frame and picks one up
				// once the budget refills. The caller folds content_hash=0 into its mesh hash,
				// so the untextured mesh is a distinct handle and ages out on its own.
				++s_stats.deferred;
				return out;
			}

			// Inserted before the runtime call, not after: entry.pixels is the buffer Remix is
			// handed and it must live at a stable address for the life of the texture.
			// unordered_map never invalidates references to existing elements, so the map node
			// is that stable address; a local built here and copied in afterwards would leave
			// the runtime holding a pointer into a dead stack frame.
			it = s_entries.emplace(content_hash, material_entry{}).first;
			it->second.created_frame = frame;
			material_entry& entry = it->second;
			entry.last_used_frame = frame;
			entry.is_render_target = is_render_target;

			// Pull the render target's pixels down into GS local memory so the ordinary decode()
			// inside create() can see them. Under the hardware renderer the content lives in a GPU
			// texture and local memory is stale -- that is the real reason this path was closed.
			// GSTextureCache::Read(Source*, r) downloads and WritePixel32s it back at the source's
			// own TBP0, so nothing downstream needs to know a readback happened.
			//
			// ONLY on the miss. A GPU download is expensive and there are ~190 render-target draws
			// a frame on SOCOM; doing it per bind would pay for all of them every frame.
			//
			// Consequence, and it is deliberate: the sky is a ONE-OFF SNAPSHOT, so the clouds do
			// not scroll. Refreshing would mean rebuilding the material, and rebuild_material swaps
			// the material handle WITHOUT bumping generation() -- with stable mesh identity on, the
			// mesh keeps the old handle, reap() destroys it, and the surface goes white on a timer
			// (see the long comment on material_late_rebuild below, which is defaulted off for
			// exactly this reason). A static sky that lights the scene beats an animated one that
			// disappears.
			if (is_render_target)
			{
				const int rt_w = 1 << TEX0.TW;
				const int rt_h = 1 << TEX0.TH;

				if (rt_w > 0 && rt_h > 0 && rt_w <= 2048 && rt_h <= 2048)
				{
					// Read() takes a non-const Source* because it is the general readback entry
					// point, but it only reads m_texture and m_TEX0 off the source -- what it
					// mutates is GS local memory, not the Source. bind() takes its source const by
					// this backend's convention, so cast rather than widen the signature through
					// every caller.
					g_texture_cache->Read(
						const_cast<GSTextureCache::Source*>(source), GSVector4i(0, 0, rt_w, rt_h));
					++s_stats.rt_snapshots;
				}
			}

			if (!create(rt, content_hash, *source, key, entry))
			{
				entry.failed = true;
				entry.texture = nullptr;
				entry.material = nullptr;
				entry.pixels.clear();
				entry.pixels.shrink_to_fit();
			}
			else
			{
				--s_budget_left;
			}

			if (!entry.failed && dump_enabled() && s_dumped.insert(content_hash).second)
			{
				// Mean colour of what we actually handed the runtime. This is the one
				// measurement that separates "the decode produced grey" from "the colour was
				// lost downstream" -- a question that has now cost two wrong diagnoses.
				u32 mean_r = 0, mean_g = 0, mean_b = 0;
				double sat_sum = 0.0;
				const size_t texels = entry.pixels.size() / 4;

				for (size_t i = 0; i < texels; ++i)
				{
					// Packed BGRA.
					const u32 b = entry.pixels[(i * 4) + 0];
					const u32 g = entry.pixels[(i * 4) + 1];
					const u32 r = entry.pixels[(i * 4) + 2];
					mean_b += b;
					mean_g += g;
					mean_r += r;

					const u32 mx = std::max(r, std::max(g, b));
					const u32 mn = std::min(r, std::min(g, b));
					if (mx > 8)
						sat_sum += static_cast<double>(mx - mn) / static_cast<double>(mx);
				}

				if (texels > 0)
				{
					mean_r = static_cast<u32>(mean_r / texels);
					mean_g = static_cast<u32>(mean_g / texels);
					mean_b = static_cast<u32>(mean_b / texels);
				}

				const double mean_sat = (texels > 0) ? (sat_sum / static_cast<double>(texels)) : 0.0;

				// Spatial detail, which the mean and the saturation cannot see: a flat white
				// block and a photograph of concrete can have identical means. 'std' is the RMS
				// deviation of luminance from the mean over the whole image, 'uniq' the number of
				// distinct texel values (capped). A palette-decode failure shows as std ~0 and
				// uniq 1; a correctly decoded 4-bit texture cannot have more than 16 distinct
				// values, so uniq also says whether the CLUT width was honoured.
				double var_sum = 0.0;
				const double mean_luma =
					(0.299 * mean_r) + (0.587 * mean_g) + (0.114 * mean_b);
				std::unordered_set<u32> distinct;

				for (size_t i = 0; i < texels; ++i)
				{
					const u32 b = entry.pixels[(i * 4) + 0];
					const u32 g = entry.pixels[(i * 4) + 1];
					const u32 r = entry.pixels[(i * 4) + 2];
					const double luma = (0.299 * r) + (0.587 * g) + (0.114 * b);
					var_sum += (luma - mean_luma) * (luma - mean_luma);

					if (distinct.size() < 4096)
					{
						u32 packed;
						std::memcpy(&packed, &entry.pixels[i * 4], sizeof(packed));
						distinct.insert(packed);
					}
				}

				const double std_dev = (texels > 0) ? std::sqrt(var_sum / static_cast<double>(texels)) : 0.0;

				const std::string line = fmt::format(
					"Remix tex={:016X} {}x{} psm=0x{:02x} tw={} th={} tbp0=0x{:04x} tbw={} pal={} "
					"tex0hash={:016x} cluthash={:016x} region={}x{} meanRGB=({},{},{}) sat={:.3f} "
					"std={:.1f} uniq={}",
					content_hash, entry.width, entry.height, static_cast<u32>(TEX0.PSM),
					static_cast<u32>(TEX0.TW), static_cast<u32>(TEX0.TH), static_cast<u32>(TEX0.TBP0),
					static_cast<u32>(TEX0.TBW), static_cast<u32>(psm.pal), key.TEX0Hash, key.CLUTHash,
					key.region_width, key.region_height, mean_r, mean_g, mean_b, mean_sat,
					std_dev, distinct.size());

				INFO_LOG("{}", line);
				dump_write(line);
			}
		}
		else
		{
			++s_stats.hits;

			// The tag lists changed since this material was built, so its emissive state may be
			// stale. Rebuilding is cheap (the texture is kept) and the caller folds
			// generation() into its mesh hash, so meshes pick the new handle up too.
			if (it->second.generation != s_generation)
			{
				rebuild_material(rt, content_hash, it->second, frame);
			}
			else if (const u64 interval = material_late_rebuild();
					 interval > 0 && frame >= std::max(it->second.created_frame, it->second.last_rebuild_frame) + interval)
			{
				// Re-resolve the albedo on an interval. Was a ONE-SHOT probe; periodic is strictly
				// more general, and 0 (the default) still disables it entirely.
				//
				// DEFAULTED OFF, and NOT a fix for the white-out. Two reasons, both learned the
				// hard way. First, the measurement at the top of this file already recorded that
				// MATREBUILD=120 changes nothing, so the albedo failure is not an ordering or
				// lifetime race and re-resolving cannot address it. Second, and worse: a rebuild
				// swaps the material handle WITHOUT bumping generation(), and generation() is what
				// the caller folds into its mesh hash (RemixSubmit.cpp:4404, :4461) to make cached
				// meshes pick a new handle up. With stable mesh identity on, a mesh built before
				// the rebuild keeps the OLD handle, which reap() then destroys once it ages past
				// mesh_idle_frames() -- so turning this on makes surfaces go white on a timer.
				// Anything that re-enables it must bump generation() in the same breath.
				//
				// A material resolves its texture ONCE at finalization and caches an empty
				// TextureRef forever if the lookup misses. Remix's texture table is a PER-FRAME
				// registration and we register at CreateTexture only, so once the runtime drops
				// the entry the material holds a dead reference and the surface renders white --
				// permanently, because bind() still finds the material and reports a cache hit, so
				// nothing ever rebuilds it. MEASURED on Rainbow Six 3: squadmates render textured,
				// then turn white a few seconds later while "mat live 29 | bind 1049346 hit
				// 1049271 miss 75 created 75" shows binds still hitting and no re-creation at all.
				//
				// Rebuilding re-runs CreateTexture (TEXREUPLOAD, on by default), which re-registers
				// the texture, and re-resolves the pseudo-path against the table that registration
				// just landed in.
				it->second.last_rebuild_frame = frame;
				rebuild_material(rt, content_hash, it->second, frame);
			}
		}

		it->second.last_used_frame = frame;

		if (it->second.failed)
		{
			++s_stats.skip_failed;
			return out;
		}

		out.material = it->second.material;
		out.content_hash = content_hash;
		return out;
	}

	void reap(const runtime& rt, u64 frame)
	{
		// Retired materials first: same age rule as the meshes, because a mesh built before a
		// tag change still holds the handle Remix bound into it.
		if (!s_retired.empty() && rt.ok())
		{
			const u64 window = mesh_idle_frames() + 1;
			const remixapi_Interface& api = rt.api();

			s_retired.erase(std::remove_if(s_retired.begin(), s_retired.end(),
								[&](const retired_material& r) {
									if (frame < r.frame + window)
										return false;

									guarded_destroy_material(api.DestroyMaterial, r.handle);
									return true;
								}),
				s_retired.end());
		}

		if (s_entries.empty())
			return;

		// Never touch anything that could still be referenced by a live mesh: the mesh LRU
		// window is the floor on how stale an entry has to be before it is safe to release.
		const u64 safe_age = std::max(texture_idle_frames(), mesh_idle_frames() + 1);
		if (frame < safe_age)
			return;

		const u64 cutoff = frame - safe_age;
		const bool over_cap = s_entries.size() > texture_live_cap();

		for (auto it = s_entries.begin(); it != s_entries.end();)
		{
			if (it->second.last_used_frame > cutoff)
			{
				++it;
				continue;
			}

			destroy(rt, it->second);
			it = s_entries.erase(it);
		}

		if (over_cap && s_entries.size() > texture_live_cap())
		{
			DEV_LOG("Remix: {} materials resident, over the {} cap -- raise PCSX2_REMIX_TEXIDLE "
					"or lower PCSX2_REMIX_TEXCAP if this grows",
				s_entries.size(), texture_live_cap());
		}
	}

	void destroy_all(const runtime& rt)
	{
		if (rt.ok())
		{
			for (auto& [hash, entry] : s_entries)
				destroy(rt, entry);

			for (const retired_material& r : s_retired)
				guarded_destroy_material(rt.api().DestroyMaterial, r.handle);
		}

		s_retired.clear();

		s_entries.clear();
		s_seen_content.clear();
		s_seen_tex0.clear();
		s_dumped.clear();

		if (s_decode_buffer)
		{
			_aligned_free(s_decode_buffer);
			s_decode_buffer = nullptr;
			s_decode_capacity = 0;
		}

		s_stats = {};
		s_budget_left = 0;
		s_category_hits = 0;
	}

	std::string stats_line()
	{
		const double hash_ms = Common::Timer::ConvertValueToMilliseconds(s_stats.hash_ticks);
		const double decode_ms = Common::Timer::ConvertValueToMilliseconds(s_stats.decode_ticks);
		const double hash_us_per_bind =
			(s_stats.binds > 0) ? ((hash_ms * 1000.0) / static_cast<double>(s_stats.binds)) : 0.0;

		return fmt::format(
			"Remix: mat live {} | bind {} hit {} miss {} created {} destroyed {} deferred {} | "
			"skip: nosrc {} target {} unsup {} failed {} | fail {} | rt snapshots {} | "
			"unique content {} tex0 {} (clut variants {}) | replacement dds {} skipped-nondds {} probe {} "
			"packmap {} | tags {} hits {} | "
			"hash {:.0f} ms ({:.2f} us/bind) decode {:.0f} ms",
			s_entries.size(), s_stats.binds, s_stats.hits, s_stats.misses, s_stats.created,
			s_stats.destroyed, s_stats.deferred, s_stats.skip_no_source, s_stats.skip_target,
			s_stats.skip_unsupported, s_stats.skip_failed, s_stats.failures, s_stats.rt_snapshots,
			s_stats.unique_content, s_stats.unique_tex0,
			(s_stats.unique_content > s_stats.unique_tex0) ? (s_stats.unique_content - s_stats.unique_tex0) : 0,
			s_stats.replacement_albedo, s_stats.replacement_not_dds, s_stats.replacement_probe,
			// `packmap` is PCSX2's own replacement map, not ours: 0 means ReloadReplacementMap
			// found nothing, which separates a missing/mis-placed pack (or LoadTextureReplacements
			// off) from a pack that is loaded but whose hashes do not match this title's.
			GSTextureReplacements::HasAnyReplacementTextures() ? 1 : 0,
			s_category_tags, s_category_hits,
			hash_ms, hash_us_per_bind, decode_ms);
	}

	u64 unique_hashes()
	{
		return s_stats.unique_content;
	}
} // namespace remix_ps2::materials
