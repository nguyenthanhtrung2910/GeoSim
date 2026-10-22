# ArcGIS Terrain3D heightmaps

Select **ArcGIS Terrain3D (LERC Tiles)** in the Landscape Spawner's image source.
Choose **XYZ Zoom** from 0 through 16 and select an area using the existing
coordinate or bounding actor controls. Manual XYZ tile coordinates also work.
No MapTiler or Mapbox token is needed.

Enable **Merge Images** and **Crop Coordinates** for a single cropped heightmap.
Landscape generation uses the existing conversion to a 16-bit PNG and altitude
scaling. XYZ parallel download and Allow Invalid Tiles work with this source.
If every requested tile is missing or blank, generation fails.

## Data flow

1. Download `/tile/{z}/{y}/{x}?blankTile=false` from Terrain3D.
2. Decode with the bundled LERC library into a single-band Float32 TIFF.
3. Assign EPSG:3857 georeferencing without resampling.
4. Run the existing merge, optional crop, and PNG conversion steps.

Raw `.lerc` files and `-decoded.tif` files are saved in `ImageDownloader/Download`.
Georeferenced tiles are saved in the spawner's `-XYZ` directory; subsequent
stages use their existing output directories.

Decoded TIFF pixels contain elevations in meters. Valid zero heights are
preserved. Invalid samples are marked as NoData, rather than converted to zero.
The 257 samples span 256 cells; adjacent rasters share one boundary sample.
Their georeferencing includes a half-sample margin so GDAL places that shared
sample at the same location when merging.

This source uses cached elevation tiles, not `exportImage`. A high zoom level
does not guarantee equally fine source elevation detail everywhere.
