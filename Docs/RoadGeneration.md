# OSM data import and road generation

`OSMRoadImporter` is a data-only actor in the `RoadGeneration` module. It reads local OSM
XML/Overpass JSON or requests OSM data, converts coordinates using Level Coordinates,
samples selected terrain collision and caches all highway ways in memory. It creates
no actors, splines or meshes and has no Spawned Actors Path or spline-generation settings.
Generate refreshes the cache; Clear discards it. The cache is not saved with the level.

`RoadGenerator` is the project's road mesh generator.
It reads the OSMRoadImporter cache and delegates mesh generation to the
installed Road Constructor plugin. It does not request OSM data. Road Constructor's
Terrain Deform command creates the Landscape splines from the generated road actors.

The adapter lives in the `RoadGeneration` runtime module under `Source/RoadGeneration`.
Generated roads use `AOsmRoadActor`, and their owning scene uses `AOsmRoadScene`.
Class redirects preserve references saved under their previous ImportedRoadConstructor names.
Its public actor headers are in `Public`; implementations and automation tests are in
`Private`. Class redirects preserve saved references from the former `Landscapes` module.
Existing OSMDataImporter actors, source/bounds enums and RoadGenerator importer references
redirect to OSMRoadImporter in RoadGeneration. The importer cache still needs Generate
after reopening the level.

## Use

1. Place an **OSM Road Importer**, choose its source, configure bounds where applicable
   and **Terrain Selection**, and click **Generate** to populate the cache.
   Coordinate conversion uses the level's **Level Coordinates** actor.
2. Place a **Road Generator** and assign its **OSM Road Importer** reference.
3. Choose **Default Road Style**. The initial default is Road Constructor's
   `RS_Street_2Lane`, including its sidewalk profiles. Use **Highway Styles** to select
   different styles for OSM `highway` values such as `residential`, `primary`, or `service`.
4. Click **Generate Roads**. **Clear Roads** deletes this generator's scene and children;
   the importer cache is retained. A failed replacement keeps the previous scene.

For existing levels, the former RoadConstructorFromSplineImporter actor redirects to
RoadGenerator. Create and assign an OSMRoadImporter with the old source, bounds and
terrain selection settings. SplineImporter now exclusively uses its original spline
Import/Delete workflow, including buildings; its Road Data Mode is removed.

Styles determine lane count/width, sidewalks, materials and props. This first integration
does not translate OSM `width`, `lanes`, or `oneway` into custom styles automatically.
**Surface Offset Cm** adds to the road's imported elevation and reduces Road Constructor's
default spline roadbed height offset by the same amount.

## Landscape deformation and debugging

Assign **Target Landscape**, generate roads, then click **Generate Landscape Splines**.
This calls Road Constructor's **Create / Update Landscape Spline Deform** for every generated
road. The Landscape owns the resulting controls and segments; inspect them in
**Landscape > Manage > Splines**. They follow Road Constructor's processed road curves,
including merged straight spans and cleaned junction approaches. Road Constructor samples
and simplifies these curves for deformation, so the Landscape controls are not a one-to-one
copy of road controls.

**Deform Landscape** enables the vendor Terrain Deform command. **Create Spline Layer If Missing**
creates a reserved Landscape Splines edit layer when needed; otherwise provide that layer yourself.
The generated road actors expose Road Constructor's width, falloff, extension and other Terrain
Deform settings for subsequent manual adjustment.

Road Constructor derives the deformation width and falloff from each road style and its
Terrain Deform settings. **Landscape Height Offset Cm** shifts imported road elevations.
Road controls use that shift plus **Surface Offset Cm**; the vendor spline roadbed offset
is reduced by the same amount. Road Constructor computes its roadbed from surface samples,
so the exact road-to-terrain gap can vary. Road Constructor may add sampled controls,
extensions and end falloff.

These splines come from source roads, not junction asphalt patches; wide junctions may need
further terrain shaping. Missing terrain samples break the source road instead of connecting
across a gap when **Require Terrain Samples** is enabled.

Enable **Generate Landscape Splines With Roads** to run Terrain Deform with **Generate Roads**.
**Clear Landscape Splines** removes the generated roads' Landscape spline objects and requests a
spline-layer update.
**Clear Roads** removes both generated outputs. Neither clears the importer's cache.

## Connectivity and output

Shared positive OSM node IDs determine connectivity. Ways are split at shared nodes,
so their junction endpoints remain fixed while Road Constructor rounds interior controls.
Positions alone never create a junction. Bridges, tunnels, nonzero layers, pedestrian
paths and areas are excluded from this ground-road generator. Missing terrain samples
split a way unless **Require Terrain Samples** is disabled.

**Road Curve Radius Cm** defaults to 5,000,000 cm as an upper bound, allowing Road
Constructor to use the available distance around interior OSM points for broad bends.
The neighboring span lengths limit the actual bend. A road with only two points has
no interior point to infer a turn and remains straight; add a source shape point or
author tangents if such a road must curve. Regenerate roads after changing this setting.

**Junction Merge Distance Cm** contracts short corridors between branching junctions.
**Override Junction Radius** defaults to disabled, leaving link initialization and corner
radius fitting to Road Constructor. Enable it to seed links with **Junction Radius Cm**;
the plugin still solves the final corner radii and cut distances. Regenerate roads after
changing this option.
Separately, **Merge Straight Roads** joins compatible spans before Road Constructor actors
are created. **Straight Connection Tolerance Degrees** defaults to 10 degrees from a
straight continuation (outward XY directions at 180 degrees). A merge requires exactly
two arms at a shared OSM node, the same resolved RoadStyle asset, and endpoints within
1 cm in 3D. A style asset also fixes widths; equal widths alone do not qualify different
assets for merging. Reversed point orders are supported only when left/right lane
definitions are identical, so their lane widths, profiles, markings and props are not swapped.
Closed loops stay split for the native solver. The shared sample is kept once and all source
OSM way IDs remain in the generated actor's tags. Branching nodes and 90-degree corners
remain junctions. Incompatible spans retain the existing junction handling; no transition
generator is added. The log reports the number of merged connections.

**Clean Junction Approaches** defaults to enabled. The adapter first fits the roads and solves
junctions without building their meshes. It uses the resulting cut-distance intervals along
each road to remove ordinary interior controls covered by a junction, then refits the affected
roads and solves all junctions again. Road endpoints are always retained to preserve OSM
connectivity. Points outside the first-pass cut ranges keep their original positions/elevations.
The second solve recalculates gate distances for the new road lengths; both road and junction
meshes use those final cuts. This is one correction pass, not repeated simplification.
ImportedPositions and the OSMRoadImporter cache retain the original prepared data.
Each regeneration starts from that source, so cleanup does not accumulate.
Disable the option to compare against the initial fit; the log reports removed control counts.

**Remove Corners Without Junction Surface** defaults to enabled. After Road Constructor
builds a junction, the adapter checks whether its central asphalt mesh exists. If native
triangulation left it empty, the adapter deletes that junction's generated corner roads,
including their sidewalk and curb meshes and props. Other junctions and the source-road
approaches remain. Regeneration restores corners if the asphalt builds successfully later.

Road Constructor then solves corner radii, cutbacks, overlapping junction cuts, central
asphalt triangulation, sidewalk profiles and UVs. Its native approach-gate guard is 800 cm;
very short terminal arms may be omitted from a junction by that guard.

The output scene and source road controls are saved with the level. Connector roads and
junction meshes are derived output and are regenerated on loading and entering play,
without accessing the importer cache or making network requests. The project subclasses
own this lifecycle because the vendor scene's full refresh discovers XY crossings.
No installed plugin source or global plugin settings are modified.

Regenerate through this generator after changing source data or styles. Generated controls
are importer-owned: direct road-point edits are replaced from the saved imported positions.
Do not use Road Constructor's generic **Refresh All** or topology editing tools on this
managed scene: those explicitly invoke its geometric connectivity rebuild. This initial
adapter is for generating a managed network, rather than round-tripping manual edits.

The previous custom road, bridge, sidewalk and lamp mesh generators have been removed.
RoadForge is disabled and is no longer a project module dependency. Existing saved
visual output is not automatically deleted from levels. This importer currently excludes
bridges, tunnels and nonzero layers; there is no separate bridge generator.
The shared road cache now belongs to OSMRoadImporter. SplineImporter and the building
import workflow remain independent of road generation.

## Verification

Editor automation tests: `Landscapes.RoadConstructor`.
They check shared-node splitting, coincident unconnected nodes, bridge exclusion,
terrain gaps, and native road/junction mesh generation and regeneration.
