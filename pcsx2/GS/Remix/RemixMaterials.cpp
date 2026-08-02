// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixMaterials.h"

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSRenderer.h"

#include "Config.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
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
		u32 texture_budget()
		{
			static const u32 value =
				static_cast<u32>(std::max<s64>(1, read_env_int(L"PCSX2_REMIX_TEXBUDGET", 32)));
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

		bool textures_linear()
		{
			static const bool value = read_env_int(L"PCSX2_REMIX_TEXLINEAR", 0) != 0;
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
			u64 last_used_frame = 0;
			u32 width = 0;
			u32 height = 0;
			bool failed = false; // decode or a runtime call refused: never retried
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
			u64 skip_target = 0; // render-target source: no stable content identity
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
		std::vector<u8> s_decode_buffer;
		std::vector<u8> s_pixel_buffer;
		counters s_stats{};
		u32 s_budget_left = 0;

		// Bounded so a title that really does animate every palette cannot turn the
		// measurement sets into a leak.
		constexpr size_t s_max_measurement_entries = 200000;

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
		bool decode(const GSTextureCache::Source& source, u32& out_width, u32& out_height)
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

			const u32 pitch = static_cast<u32>(read_width) * sizeof(u32);
			const size_t needed = static_cast<size_t>(pitch) * static_cast<size_t>(read_height);

			s_decode_buffer.resize(needed);

			GSLocalMemory& mem = g_gs_renderer->m_mem;
			psm.rtx(mem, mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM), block_rect,
				s_decode_buffer.data(), static_cast<int>(pitch), TEXA);

			// rtx() writes the whole block-aligned rect; the texture itself is the sub-rect.
			const u32 offset = (static_cast<u32>(rect.top - block_rect.top) * pitch) +
			                   (static_cast<u32>(rect.left - block_rect.left) * sizeof(u32));

			s_pixel_buffer.resize(static_cast<size_t>(tw) * static_cast<size_t>(th) * 4);

			const bool alpha_expand = expand_alpha();
			u8* dst = s_pixel_buffer.data();

			for (int y = 0; y < th; ++y)
			{
				const u8* src = s_decode_buffer.data() + offset + (static_cast<size_t>(y) * pitch);

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

		bool create(const runtime& rt, u64 content_hash, const GSTextureCache::Source& source, material_entry& entry)
		{
			const Common::Timer::Value decode_start = Common::Timer::GetCurrentValue();
			const bool decoded = decode(source, entry.width, entry.height);
			s_stats.decode_ticks += Common::Timer::GetCurrentValue() - decode_start;

			if (!decoded)
			{
				++s_stats.skip_unsupported;
				return false;
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
			info.data = s_pixel_buffer.data();
			info.dataSize = s_pixel_buffer.size();

			const u32 tex_status = guarded_create_texture(api.CreateTexture, &info, &entry.texture);
			if (tex_status != REMIXAPI_ERROR_CODE_SUCCESS || !entry.texture)
			{
				ERROR_LOG("Remix: CreateTexture failed for {}x{} hash {:016x} ({})",
					entry.width, entry.height, content_hash, error_name(tex_status));
				entry.texture = nullptr;
				++s_stats.failures;
				return false;
			}

			// The pseudo-path trick (RPCS3 RemixTextures.cpp:643-648). The fork resolves this
			// string against the texture manager's hash table that remixapi_CreateTexture just
			// populated, which is what puts an API texture in the same hash namespace as a
			// native D3D9 one. Stock Remix hashes material textures by path string only, so
			// this resolves to nothing there and no modding surface exists -- hence the
			// fork_features() gate at the call site.
			wchar_t albedo_path[32]{};
			::swprintf_s(albedo_path, L"0x%016llX", static_cast<unsigned long long>(content_hash));

			remixapi_MaterialInfoOpaqueEXT opaque{};
			opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
			opaque.pNext = nullptr;
			opaque.roughnessTexture = nullptr;
			opaque.metallicTexture = nullptr;
			opaque.anisotropy = 0.f;
			opaque.albedoConstant = {1.f, 1.f, 1.f};
			opaque.opacityConstant = 1.f;
			opaque.roughnessConstant = 0.7f;
			opaque.metallicConstant = 0.f;
			opaque.thinFilmThickness_hasvalue = 0;
			opaque.thinFilmThickness_value = 0.f;
			opaque.alphaIsThinFilmThickness = 0;
			opaque.heightTexture = nullptr;
			opaque.displaceIn = 0.f;
			opaque.useDrawCallAlphaState = 1;
			opaque.blendType_hasvalue = 0;
			opaque.blendType_value = 0;
			opaque.invertedBlend = 0;
			opaque.alphaTestType = 7; // always
			opaque.alphaReferenceValue = 0;
			opaque.displaceOut = 0.f;

			remixapi_MaterialInfo material{};
			material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
			material.pNext = &opaque;
			material.hash = content_hash;
			material.albedoTexture = albedo_path;
			material.normalTexture = nullptr;
			material.tangentTexture = nullptr;
			material.emissiveTexture = nullptr;
			material.emissiveIntensity = 0.f;
			material.emissiveColorConstant = {0.f, 0.f, 0.f};
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

			++s_stats.created;
			return true;
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

	void begin_frame()
	{
		s_budget_left = texture_budget();
	}

	binding bind(const runtime& rt, const GSTextureCache::Source* source, u64 frame)
	{
		binding out{};

		if (!rt.ok() || !rt.fork_features())
			return out;

		++s_stats.binds;

		if (!source)
		{
			++s_stats.skip_no_source;
			return out;
		}

		// A render-to-texture has no stable content identity by construction, so there is
		// nothing a modder could key a replacement on. Counted, not silently dropped.
		if (source->m_target || source->m_from_target)
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

			material_entry entry{};
			if (!create(rt, content_hash, *source, entry))
			{
				entry.failed = true;
				entry.texture = nullptr;
				entry.material = nullptr;
			}
			else
			{
				--s_budget_left;
			}

			entry.last_used_frame = frame;
			it = s_entries.emplace(content_hash, entry).first;

			if (!entry.failed && dump_enabled() && s_dumped.insert(content_hash).second)
			{
				const std::string line = fmt::format(
					"Remix tex={:016X} {}x{} psm=0x{:02x} tw={} th={} tbp0=0x{:04x} tbw={} pal={} "
					"tex0hash={:016x} cluthash={:016x} region={}x{}",
					content_hash, entry.width, entry.height, static_cast<u32>(TEX0.PSM),
					static_cast<u32>(TEX0.TW), static_cast<u32>(TEX0.TH), static_cast<u32>(TEX0.TBP0),
					static_cast<u32>(TEX0.TBW), static_cast<u32>(psm.pal), key.TEX0Hash, key.CLUTHash,
					key.region_width, key.region_height);

				INFO_LOG("{}", line);
				dump_write(line);
			}
		}
		else
		{
			++s_stats.hits;
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
		}

		s_entries.clear();
		s_seen_content.clear();
		s_seen_tex0.clear();
		s_dumped.clear();
		s_decode_buffer.clear();
		s_decode_buffer.shrink_to_fit();
		s_pixel_buffer.clear();
		s_pixel_buffer.shrink_to_fit();
		s_stats = {};
		s_budget_left = 0;
	}

	std::string stats_line()
	{
		const double hash_ms = Common::Timer::ConvertValueToMilliseconds(s_stats.hash_ticks);
		const double decode_ms = Common::Timer::ConvertValueToMilliseconds(s_stats.decode_ticks);
		const double hash_us_per_bind =
			(s_stats.binds > 0) ? ((hash_ms * 1000.0) / static_cast<double>(s_stats.binds)) : 0.0;

		return fmt::format(
			"Remix: mat live {} | bind {} hit {} miss {} created {} destroyed {} deferred {} | "
			"skip: nosrc {} target {} unsup {} failed {} | fail {} | "
			"unique content {} tex0 {} (clut variants {}) | "
			"hash {:.0f} ms ({:.2f} us/bind) decode {:.0f} ms",
			s_entries.size(), s_stats.binds, s_stats.hits, s_stats.misses, s_stats.created,
			s_stats.destroyed, s_stats.deferred, s_stats.skip_no_source, s_stats.skip_target,
			s_stats.skip_unsupported, s_stats.skip_failed, s_stats.failures,
			s_stats.unique_content, s_stats.unique_tex0,
			(s_stats.unique_content > s_stats.unique_tex0) ? (s_stats.unique_content - s_stats.unique_tex0) : 0,
			hash_ms, hash_us_per_bind, decode_ms);
	}

	u64 unique_hashes()
	{
		return s_stats.unique_content;
	}
} // namespace remix_ps2::materials
