# GeoSim

GeoSim is an Unreal Engine 5.8 C++ project for building georeferenced environments
from real-world elevation and OpenStreetMap (OSM) data. The project organizes environment generation into three layers: Landscape,
Building, and Road.

## Landscape layer

The `LandscapeGeneration` module creates the terrain that supports the rest of the
environment. It downloads elevation data, prepares heightmaps, and generates Unreal
Landscapes using Landscape Combinator. Supported sources include ArcGIS World
Elevation 3D, Mapbox, MapTiler, NextZen, and generic XYZ tiles.

Geographic bounds and coordinate conversion keep the terrain aligned with imported
features. `LandscapeBoundsPCGVolume` fits a PCG volume to a landscape's bounds for
procedural generation across the terrain.

See [Landscape generation](Docs/LandscapeGeneration.md) and
[Landscape Bounds PCG Volume](Docs/LandscapeBoundsPCGVolume.md).

## Building layer

The `BuildingGeneration` module imports OSM building footprints and tags, places
them on the selected terrain, and generates buildings with Vitruvio rule packages.
Building heights can use OSM height and floor-count tags, with configurable defaults
when that information is missing.

Import and generation are separate steps. Generated buildings can be edited and
baked into reusable assets, with an Unbake workflow for further changes.

See [Building import and generation](Docs/FootprintBuildings.md).

## Road layer

The `RoadGeneration` module imports OSM highway data and generates road meshes
through the Road Constructor plugin. Road styles control lanes, sidewalks,
materials, and props, with styles selected by OSM highway type.

Road generation uses a separate importer cache. Generated roads can also create
Landscape splines to deform the terrain, and provide corridors for vegetation
exclusion.

See [Road import and generation](Docs/RoadGeneration.md).

## Working with the layers

Start by generating the Landscape and configuring the level coordinates. Import
Building and Road data against that terrain, then generate their output. The
building and road importer caches are kept in memory, so reimport after reopening
the level.

Open `GeoSim.uproject` with Unreal Engine 5.8. The generation workflows rely on
Landscape Combinator, Vitruvio, and Road Constructor; initialize the repository's
submodules and retrieve Git LFS assets before building.
