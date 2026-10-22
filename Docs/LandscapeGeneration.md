# LandscapeGeneration

Adds ArcGIS World Elevation 3D to the project-owned image downloader and landscape
spawner while preserving the other image sources.

## Fetchers

- `HMWorldElevation3D : HMFetcher` handles only Terrain3D: download LERC,
  decode to Float32 TIFF, then assign EPSG:3857 georeferencing.
- MapTiler, Mapbox, NextZen, and Generic XYZ use the plugin's original `HMXYZ`.
- Existing plugin transformers handle merging, cropping, reprojection, and PNG
  conversion. Their public classes, including HMXYZ, are exported with
  `IMAGEDOWNLOADER_API` for normal DLL linking; no source bridges are needed.

## Use

1. Place **Landscape Generator** (`ALandscapeGenerator`) and select its
   HeightmapDownloader (`UMapDowloader`).
2. Select **ArcGIS World Elevation 3D (LERC Tiles)** in Image Source Kind, or choose
   any existing source for its normal behavior.
3. For ArcGIS, choose XYZ Zoom from 0 through 16 and set the area through
   coordinates, an EPSG:4326 box, a bounding actor, or manual XYZ tile indices.
   Bounding actors require Level Coordinates.
4. Enable Merge Images for one image. Enable Crop Coordinates for a geographic
   crop. With manual tile indices, disable cropping or supply a Cropping Actor.
5. Click Spawn Landscape.

ArcGIS uses `/tile/{z}/{y}/{x}?blankTile=false`; no token is required. Its 257
samples span 256 cells. Half-sample raster margins align shared edge samples,
so two adjacent tiles merge to 513 samples across.

Raw `.lerc` and `-decoded.tif` files are saved in ImageDownloader/Download under
LandscapeCombinator's configured temporary folder. Georeferenced tiles use the
spawner's `-XYZ` folder; later stages retain their existing output directories.

Decoding preserves valid zero heights and represents invalid samples with a
separate NoData value. Downstream NoData behavior remains the installed plugin's
behavior, including its use of zero as merged/reprojected output NoData.
