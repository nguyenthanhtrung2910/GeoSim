# ArcGIS Mini Map HUD

After building the project manually, place **ArcGIS Mini Map HUD** in the level and start Play.

- Use a **LevelCoordinates** actor configured with the same origin, CRS, and scale used to place the landscape. The HUD can use a single loaded actor even when it is Hidden in Game. Assign **Level Coordinates Actor** explicitly to choose among multiple coordinate actors.
- Keep the coordinate actor loaded during play. For World Partition, disable **Is Spatially Loaded** on it and keep its level/data layer enabled at runtime.
- Assign **Target Landscape** to fit a fixed, north-up map to the landscape bounds. Loaded streaming proxies are included; the editor can also use the complete World Partition bounds.
- Leave Target Landscape unset to center the initial map on the player. **Fallback Area Width Meters** controls its width in EPSG:3857 units.
- The green marker follows the local player's possessed pawn. Assign **Tracked Actor** to follow another actor instead. The marker hides outside the map.
- **Map Size** and **Screen Margin** control the HUD at the top-right of the screen. **Image Resolution** controls download quality.
- Call **Refresh Map** from Blueprint during play to change the fixed map area or retry a failed download.

The default service is the public ArcGIS World Street Map MapServer. You can replace **Map Service URL** with another MapServer supporting `/export`, supply an optional **Service Token**, and update **Map Attribution** for that service.

Each refresh requests export metadata and then downloads its PNG image. The returned extent positions the marker accurately. Marker updates use the cached image and a cached GDAL coordinate transformation; they make no HTTP requests. Pending downloads are canceled when the HUD ends play.

The actor uses Unreal HTTP and UMG directly. It does not require an ArcGIS map actor, Cesium, a second scene capture, or a Widget Blueprint.

The export response includes the actual image bounds, which can differ from the requested bounds: [ArcGIS Export Map reference](https://developers.arcgis.com/rest/services-reference/enterprise/export-map/).
