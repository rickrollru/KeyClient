#include "xeditor_lod.h"

#include <base/math.h>
#include <base/mem.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/image.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <game/editor/editor.h>
#include <game/editor/mapitems/image.h>
#include <game/editor/mapitems/layer_tiles.h>
#include <game/editor/mapitems/map.h>
#include <game/mapitems.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
// colors of the 256 tiles of a picture, each tile as Size x Size pixels
struct STileColors
{
	int m_Size = 0;
	std::vector<uint8_t> m_vColors; // [index][y][x][rgba]
};

// a 16 x 16 tile picture: the editor's own ones are read from disk once
struct SSource
{
	const uint8_t *m_pData = nullptr;
	int m_Width = 0;
	int m_Height = 0;
	int m_PixelSize = 4;
};

struct SLod
{
	IGraphics::CTextureHandle m_Texture;
	const CTile *m_pTiles = nullptr;
	int m_Width = 0;
	int m_Height = 0;
	int m_Size = 0;
	const void *m_pSource = nullptr;
	float m_Modified = -1.0f;
	uint64_t m_Hash = 0;
	float m_LastCheck = 0.0f;
	float m_LastUsed = 0.0f;
};

std::map<std::string, std::unique_ptr<CImageInfo>> s_EditorPictures;
std::map<std::pair<const void *, int>, STileColors> s_TileColors;
std::map<const CLayerTiles *, SLod> s_Lods;
float s_LastSweep = 0.0f;

uint64_t HashTiles(const CTile *pTiles, int Num)
{
	// FNV over the raw tiles: only to notice changes the map did not report
	uint64_t Hash = 1469598103934665603ull;
	const uint8_t *pBytes = reinterpret_cast<const uint8_t *>(pTiles);
	const size_t Size = (size_t)Num * sizeof(CTile);
	size_t i = 0;
	for(; i + 8 <= Size; i += 8)
	{
		uint64_t Word;
		mem_copy(&Word, pBytes + i, 8);
		Hash = (Hash ^ Word) * 1099511628211ull;
	}
	for(; i < Size; i++)
		Hash = (Hash ^ pBytes[i]) * 1099511628211ull;
	return Hash;
}

bool EditorPicture(IGraphics *pGraphics, const char *pPath, SSource *pSource)
{
	auto It = s_EditorPictures.find(pPath);
	if(It == s_EditorPictures.end())
	{
		auto pImage = std::make_unique<CImageInfo>();
		if(!pGraphics->LoadPng(*pImage, pPath, IStorage::TYPE_ALL))
			pImage.reset();
		It = s_EditorPictures.emplace(pPath, std::move(pImage)).first;
	}
	if(!It->second || It->second->m_Format != CImageInfo::FORMAT_RGBA)
		return false;
	pSource->m_pData = It->second->m_pData;
	pSource->m_Width = (int)It->second->m_Width;
	pSource->m_Height = (int)It->second->m_Height;
	return true;
}

const STileColors &TileColors(const SSource &Source, int Size)
{
	STileColors &Colors = s_TileColors[{Source.m_pData, Size}];
	if(Colors.m_Size == Size)
		return Colors;
	Colors.m_Size = Size;
	Colors.m_vColors.assign((size_t)256 * Size * Size * 4, 0);
	const int TileW = Source.m_Width / 16, TileH = Source.m_Height / 16;
	for(int Index = 1; Index < 256; Index++)
	{
		const int X0 = (Index % 16) * TileW, Y0 = (Index / 16) * TileH;
		for(int sy = 0; sy < Size; sy++)
			for(int sx = 0; sx < Size; sx++)
			{
				// the average of this part of the tile, weighted by alpha
				const int PartX0 = X0 + sx * TileW / Size, PartX1 = X0 + (sx + 1) * TileW / Size;
				const int PartY0 = Y0 + sy * TileH / Size, PartY1 = Y0 + (sy + 1) * TileH / Size;
				const int Step = std::max(1, (PartX1 - PartX0) / 8);
				double r = 0, g = 0, b = 0, a = 0;
				int Count = 0;
				for(int y = PartY0; y < PartY1; y += Step)
					for(int x = PartX0; x < PartX1; x += Step)
					{
						const uint8_t *p = Source.m_pData + ((size_t)y * Source.m_Width + x) * 4;
						const double Alpha = p[3] / 255.0;
						r += p[0] * Alpha;
						g += p[1] * Alpha;
						b += p[2] * Alpha;
						a += Alpha;
						Count++;
					}
				uint8_t *pOut = &Colors.m_vColors[(((size_t)Index * Size + sy) * Size + sx) * 4];
				if(a > 0.0)
				{
					pOut[0] = (uint8_t)std::clamp(r / a, 0.0, 255.0);
					pOut[1] = (uint8_t)std::clamp(g / a, 0.0, 255.0);
					pOut[2] = (uint8_t)std::clamp(b / a, 0.0, 255.0);
				}
				pOut[3] = (uint8_t)std::clamp(a / std::max(Count, 1) * 255.0, 0.0, 255.0);
			}
	}
	return Colors;
}

void Build(IGraphics *pGraphics, SLod &Lod, const CLayerTiles *pLayer, const STileColors &Colors)
{
	if(Lod.m_Texture.IsValid())
		pGraphics->UnloadTexture(&Lod.m_Texture);
	const int Size = Colors.m_Size;
	CImageInfo Image;
	Image.m_Width = (size_t)pLayer->m_Width * Size;
	Image.m_Height = (size_t)pLayer->m_Height * Size;
	Image.m_Format = CImageInfo::FORMAT_RGBA;
	Image.m_pData = static_cast<uint8_t *>(malloc(Image.DataSize()));
	mem_zero(Image.m_pData, Image.DataSize());
	for(int y = 0; y < pLayer->m_Height; y++)
		for(int x = 0; x < pLayer->m_Width; x++)
		{
			const CTile &Tile = pLayer->m_pTiles[y * pLayer->m_Width + x];
			if(!Tile.m_Index)
				continue;
			const uint8_t *pTile = &Colors.m_vColors[(size_t)Tile.m_Index * Size * Size * 4];
			for(int py = 0; py < Size; py++)
				for(int px = 0; px < Size; px++)
				{
					// the same turn and flips as the tile itself
					int u = px, v = py;
					if(Tile.m_Flags & TILEFLAG_ROTATE)
					{
						const int Old = u;
						u = v;
						v = Size - 1 - Old;
					}
					if(Tile.m_Flags & TILEFLAG_XFLIP)
						u = Size - 1 - u;
					if(Tile.m_Flags & TILEFLAG_YFLIP)
						v = Size - 1 - v;
					mem_copy(Image.m_pData + (((size_t)y * Size + py) * Image.m_Width + (size_t)x * Size + px) * 4, pTile + ((size_t)v * Size + u) * 4, 4);
				}
		}
	Lod.m_Texture = pGraphics->LoadTextureRawMove(Image, 0, "xc_editor_lod");
	Lod.m_Size = Size;
}
} // namespace

bool XcEditorLod::Render(CEditor *pEditor, const CLayerTiles *pLayer, const CEditorMap *pMap, ColorRGBA Color)
{
	if(!g_Config.m_XcEditorLod || pLayer->m_Width <= 0 || pLayer->m_Height <= 0 || !pLayer->m_pTiles)
		return false;
	IGraphics *pGraphics = pEditor->Graphics();
	const float Now = pEditor->Client()->GlobalTime();

	// forget the pictures of layers that were not drawn for a while (closed maps, deleted layers)
	if(Now - s_LastSweep > 5.0f)
	{
		s_LastSweep = Now;
		for(auto It = s_Lods.begin(); It != s_Lods.end();)
		{
			if(Now - It->second.m_LastUsed > 10.0f || Now < It->second.m_LastUsed)
			{
				if(It->second.m_Texture.IsValid())
					pGraphics->UnloadTexture(&It->second.m_Texture);
				It = s_Lods.erase(It);
			}
			else
				++It;
		}
	}

	// only when a tile is a few pixels and many of them are on the screen
	const CScreenRect Screen = pGraphics->GetScreen();
	const float TilePixels = 32.0f / std::max(Screen.Width(), 1.0f) * pGraphics->ScreenWidth();
	const float VisibleW = std::min((float)pLayer->m_Width, Screen.Width() / 32.0f + 2.0f);
	const float VisibleH = std::min((float)pLayer->m_Height, Screen.Height() / 32.0f + 2.0f);
	if(TilePixels >= 6.0f || VisibleW * VisibleH < 30000.0f)
		return false;

	// where the colors of the tiles come from
	SSource Source;
	const void *pSourceKey = nullptr;
	const char *pEditorPicture = pLayer->m_HasGame ? "editor/entities/DDNet.png" :
				     pLayer->m_HasFront   ? "editor/front.png" :
				     pLayer->m_HasTele    ? "editor/tele.png" :
				     pLayer->m_HasSpeedup ? "editor/speedup.png" :
				     pLayer->m_HasSwitch  ? "editor/switch.png" :
				     pLayer->m_HasTune    ? "editor/tune.png" :
							    nullptr;
	if(pEditorPicture)
	{
		if(!EditorPicture(pGraphics, pEditorPicture, &Source))
			return false;
		pSourceKey = Source.m_pData;
	}
	else
	{
		if(pLayer->m_Image < 0 || (size_t)pLayer->m_Image >= pMap->m_vpImages.size())
			return true; // no picture: the normal drawing shows nothing either
		const auto &pImage = pMap->m_vpImages[pLayer->m_Image];
		if(!pImage->m_pData || pImage->m_Format != CImageInfo::FORMAT_RGBA || pImage->m_Width % 16 != 0 || pImage->m_Height % 16 != 0)
			return false;
		Source.m_pData = pImage->m_pData;
		Source.m_Width = (int)pImage->m_Width;
		Source.m_Height = (int)pImage->m_Height;
		pSourceKey = pImage->m_pData;
	}

	// pixels per tile: up to 4, the picture stays at most 4096 wide
	const int Size = std::clamp(4096 / std::max(pLayer->m_Width, pLayer->m_Height), 1, 4);
	SLod &Lod = s_Lods[pLayer];
	Lod.m_LastUsed = Now;
	bool Rebuild = !Lod.m_Texture.IsValid() || Lod.m_pTiles != pLayer->m_pTiles || Lod.m_Width != pLayer->m_Width ||
		       Lod.m_Height != pLayer->m_Height || Lod.m_Size != Size || Lod.m_pSource != pSourceKey || Lod.m_Modified != pMap->m_LastModifiedTime;
	// a change the map did not report (co-op, undo...) is found by a check once a second
	if(!Rebuild && Now - Lod.m_LastCheck > 1.0f)
	{
		Lod.m_LastCheck = Now;
		Rebuild = HashTiles(pLayer->m_pTiles, pLayer->m_Width * pLayer->m_Height) != Lod.m_Hash;
	}
	if(Rebuild)
	{
		Build(pGraphics, Lod, pLayer, TileColors(Source, Size));
		Lod.m_pTiles = pLayer->m_pTiles;
		Lod.m_Width = pLayer->m_Width;
		Lod.m_Height = pLayer->m_Height;
		Lod.m_pSource = pSourceKey;
		Lod.m_Modified = pMap->m_LastModifiedTime;
		Lod.m_Hash = HashTiles(pLayer->m_pTiles, pLayer->m_Width * pLayer->m_Height);
		Lod.m_LastCheck = Now;
	}
	if(!Lod.m_Texture.IsValid())
		return false;

	pGraphics->BlendNormal();
	pGraphics->TextureSet(Lod.m_Texture);
	pGraphics->QuadsBegin();
	pGraphics->SetColor(Color);
	pGraphics->QuadsSetSubset(0.0f, 0.0f, 1.0f, 1.0f);
	const IGraphics::CQuadItem Quad(0.0f, 0.0f, pLayer->m_Width * 32.0f, pLayer->m_Height * 32.0f);
	pGraphics->QuadsDrawTL(&Quad, 1);
	pGraphics->QuadsEnd();
	return true;
}
