// Zoomed far out on a big map, the editor drew every tile of every layer as its own quad, twice, each frame:
// millions of quads. Then a tile layer is drawn as one picture made of it instead (each tile a few pixels in
// the average colors of its picture), built again only when the map changes.
#ifndef GAME_CLIENT_COMPONENTS_XCLIENT_EDITOR_XEDITOR_LOD_H
#define GAME_CLIENT_COMPONENTS_XCLIENT_EDITOR_XEDITOR_LOD_H

#include <base/color.h>

class CEditor;
class CEditorMap;
class CLayerTiles;

namespace XcEditorLod
{
// draws the layer as its picture when that is worth it, false: draw it the normal way
bool Render(CEditor *pEditor, const CLayerTiles *pLayer, const CEditorMap *pMap, ColorRGBA Color);
} // namespace XcEditorLod

#endif
