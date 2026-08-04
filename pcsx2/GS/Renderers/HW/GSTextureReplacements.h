// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/HW/GSTextureCache.h"

#include <utility>

namespace GSTextureReplacements
{
	struct ReplacementTexture
	{
		u32 width;
		u32 height;
		GSTexture::Format format;
		std::pair<u8, u8> alpha_minmax;

		u32 pitch;
		std::vector<u8> data;

		struct MipData
		{
			u32 width;
			u32 height;
			u32 pitch;
			std::vector<u8> data;
		};
		std::vector<MipData> mips;
	};

	void Initialize();
	void GameChanged();
	void ReloadReplacementMap();
	void UpdateConfig(Pcsx2Config::GSOptions& old_config);
	void Shutdown();

	u32 CalcMipmapLevelsForReplacement(u32 width, u32 height);

	/// On-disk path of the replacement texture for this hash, or nullptr if there is none.
	///
	/// For the RTX Remix backend. A Remix material's albedoTexture is a real file path resolved
	/// through its asset loader, and the API's "0x<hash>" pseudo-path -- which is meant to reference
	/// a texture uploaded via remixapi_CreateTexture -- does not resolve in the deployed runtime, so
	/// no API-uploaded texture ever reaches a surface. Handing Remix a real .dds path is the route
	/// that works, and it has the side benefit of feeding it the user's upscaled pack rather than the
	/// 8-bit original. Requires GSConfig.LoadTextureReplacements, since that is what populates the
	/// map (see ReloadReplacementMap).
	const std::string* GetReplacementTexturePath(const GSTextureCache::HashCacheKey& hash);

	bool HasAnyReplacementTextures();
	bool HasReplacementTextureWithOtherPalette(const GSTextureCache::HashCacheKey& hash);
	GSTexture* LookupReplacementTexture(const GSTextureCache::HashCacheKey& hash, bool mipmap, bool* pending, std::pair<u8, u8>* alpha_minmax);
	GSTexture* CreateReplacementTexture(const ReplacementTexture& rtex, bool mipmap);
	void ProcessAsyncLoadedTextures();

	void DumpTexture(const GSTextureCache::HashCacheKey& hash, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA,
		GSTextureCache::SourceRegion region, GSLocalMemory& mem, u32 level);
	void ClearDumpedTextureList();

	/// Get the number of textures that have been dumped.
	u32 GetDumpedTextureCount();

	/// Get the number of replacement textures that have been loaded/cached.
	u32 GetLoadedTextureCount();

	/// Loader will take a filename and interpret the format (e.g. DDS, PNG, etc).
	using ReplacementTextureLoader = bool (*)(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image);
	ReplacementTextureLoader GetLoader(const std::string_view filename);

	/// Saves an image buffer to a PNG file (for dumping).
	bool SavePNGImage(const std::string& filename, u32 width, u32 height, const u8* buffer, u32 pitch);
} // namespace GSTextureReplacements
