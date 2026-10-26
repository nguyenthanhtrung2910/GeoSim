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
   the importer cache and Landscape splines are retained. A failed replacement keeps the previous scene.
5. To deform terrain, assign **Target Landscape** and click **Generate Landscape Splines**.
   **Clear Landscape Splines** removes only this generator's splines and deformation.

For existing levels, the former RoadConstructorFromSplineImporter actor redirects to
RoadGenerator. Create and assign an OSMRoadImporter with the old source, bounds and
terrain selection settings. SplineImporter now exclusively uses its original spline
Import/Delete workflow, including buildings; its Road Data Mode is removed.

Styles determine lane widths, sidewalks, materials and props. A positive integer OSM
`lanes` tag overrides only the number of Driving lanes: half go on each side, with
the extra lane on the left for odd totals (for example, 3 means 2 left and 1 right).
Existing driving lanes retain their attributes; extra lanes copy the outermost driving
lane on that side, borrowing from the opposite side if needed. Non-driving lanes and
all other style settings are retained. Every OSM way gets its own copy of its selected
Highway Style, falling back to Default Road Style for missing or empty highway entries.
Spans split from one way share that copy; different ways remain independent even with
identical tags. Generated styles are embedded in the output scene; source assets are
unchanged and no separate style asset is created. Missing, invalid,
zero, or excessive counts (over 64) retain the highway style's original layout, as do
styles with no Driving lane to copy. OSM `width`, `oneway`, and directional lane tags
are not translated into style overrides.

The scene serializes its per-way style collection so World Partition road packages
can resolve their style references after reopening the map. For roads generated before
this persistence fix, refresh the importer cache, click **Generate Roads**, then **Save All**
to replace missing style references. Rebuilding code alone cannot restore unsaved styles.

**Surface Offset Cm** controls road mesh elevation relative to the cached imported points.
**Landscape Height Offset Cm** independently controls Landscape spline elevation relative to
those same points. Both accept negative values. For imported Z = 1,000 cm, Surface Offset =
20 cm and Landscape Height Offset = -10 cm produce road controls at 1,020 cm and spline
controls at 990 cm. Apply mesh changes with **Generate Roads** and terrain changes with
**Generate Landscape Splines**. Neither operation moves the other output.

## Landscape deformation and debugging

Generate roads first, assign **Target Landscape**, then click **Generate Landscape Splines**.
This calls Road Constructor's **Create / Update Landscape Spline Deform** for every generated
road. The Landscape owns the resulting controls and segments; inspect them in
**Landscape > Manage > Splines**. They follow Road Constructor's processed road curves,
including merged straight spans and cleaned junction approaches. Road Constructor samples
and simplifies these curves for deformation, so the Landscape controls are not a one-to-one
copy of road controls.

**Create Spline Layer If Missing** creates a reserved Landscape Splines edit layer when
needed; otherwise provide that layer yourself.
The generated road actors expose Road Constructor's width, falloff, extension and other Terrain
Deform settings for subsequent manual adjustment.

The deformation corridor's full width is the sum of the generated style's **Driving**
and **Shoulder** lane widths on both sides. Sidewalks, verges, medians, and other roles
are excluded. Unreal stores half this total in each control point's **Half-Width**.
**Landscape Spline Side Falloff Cm** sets the blend distance on each side (default 700 cm).
**Landscape Spline End Falloff Cm** sets the blend distance at open endpoints (default
2,000 cm). Both accept zero. Click **Generate Landscape Splines** to apply changed values. The reserved layer
is rebuilt from the final spline dimensions so the plugin's preliminary wider stamps
do not remain in the terrain.
Spline control heights are taken from the processed imported centreline plus Landscape Height
Offset, independently of Surface Offset. The adapter replaces the plugin's preliminary roadbed
heights and rebuilds the reserved layer, so its clearance and vertical lowering add no hidden
offset. Extension points use the corresponding endpoint baseline. Mesh cross-sections, curve
sampling and Landscape vertex resolution can still affect the visible fit between controls.

Each spline-generation run uses the currently generated road geometry and its saved mesh
offset to recover imported elevations. Changing Surface Offset without regenerating roads
does not change this baseline. Previous deformation controls are detached and the Landscape update is completed before
creating the replacement splines. The final layer update is completed before generation
returns. If replacement fails, the previous spline controls are restored.

These splines come from source roads, not junction asphalt patches; wide junctions may need
further terrain shaping. Missing terrain samples break the source road instead of connecting
across a gap when **Require Terrain Samples** is enabled.

**Generate Roads** and **Clear Roads** only modify road output. They do not create,
replace, delete, or reapply Landscape splines or deformation, and do not require a Target
Landscape. The former **Generate Landscape Splines With Roads** toggle is removed.
**Generate Landscape Splines** replaces this generator's previously tracked splines using
the current roads. **Clear Landscape Splines** removes them and rebuilds the reserved
layer, preserving original sculpt layers and other generators' splines. Spline ownership
is saved independently, so clearing or regenerating roads does not prevent later spline
cleanup. Clearing either output does not clear the importer's cache.

Deformation uses a layer whose class is `ULandscapeEditLayerSplines`, rather than relying on
its display name. Earlier output stamped into an ordinary sculpt layer cannot be separated
from that layer's original heights automatically; restore that layer from Undo or a backup.

## Generation change checks

Both generation buttons compare input and output fingerprints before rebuilding.
Unchanged road requests reuse the same scene, roads, and junctions; unchanged Landscape
spline requests retain their existing control points, segments, and deformation.
Deferred editor callbacks use the same output checks, so actor-label changes and queued
notifications do not cause a second junction solve or invalidate the generator cache.

Road inputs include selected cached ways, relevant settings, effective highway styles,
and authored data in referenced Road Constructor props/profile assets. Unused highway
styles, terrain settings, and parameters for disabled road features do not invalidate
road output. Source coordinates and in-place style or prop edits do invalidate it.
Missing or edited output also prevents reuse. A failed generation retains the previous
successful baseline. Changing road inputs rebuilds the road stage, including junctions;
terrain changes are applied only by the separate Landscape spline button.

Fingerprints are held for the current editor session. The first generation after loading
establishes a baseline; Clear discards the corresponding baseline. Mesh and material
asset resources remain managed by Unreal, while changes to their style references are
included in the comparisons. To force a complete rebuild, clear that output and generate again.

## Connectivity and output

Shared positive OSM node IDs determine connectivity. Ways are split at shared nodes,
so their junction endpoints remain fixed while Road Constructor rounds interior controls.
Positions alone never create a junction. **Highway Filter** is the only tag-based
selection rule: listed `highway` values are included, and an empty list includes all
highway values. There are no built-in exclusions for road classes, bridges, tunnels,
nonzero layers, or areas. Assign base styles in **Highway Styles**. Layer values still
distinguish shared-node connectivity. Missing terrain samples
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
two arms at a shared OSM node, the same generated style instance and lane-count override,
and endpoints within 1 cm in 3D. Different OSM ways have independent styles and remain
separate, preserving their ability to receive different tag overrides. Reversed point
orders are supported only when left/right lane
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
Imported scenes bypass the vendor scene's load/play callbacks, and imported roads
bypass its load/undo callbacks that request automatic connectivity discovery. They keep
the normal Actor lifecycle and queue only the shared-node rebuild. Geometric crossings
without shared node keys therefore stay disconnected after reload, play, and undo.
No installed plugin source or global plugin settings are modified.

Regenerate through this generator after changing source data or styles. Generated controls
are importer-owned: direct road-point edits are replaced from the saved imported positions.
Do not use Road Constructor's generic **Refresh All** or topology editing tools on this
managed scene: those explicitly invoke its geometric connectivity rebuild. This initial
adapter is for generating a managed network, rather than round-tripping manual edits.

The previous custom road, bridge, sidewalk and lamp mesh generators have been removed.
RoadForge is disabled and is no longer a project module dependency. Existing saved
visual output is not automatically deleted from levels. There is no separate bridge
generator; ways are selected solely by **Highway Filter**.
The shared road cache now belongs to OSMRoadImporter. SplineImporter and the building
import workflow remain independent of road generation.

## Verification

Editor automation tests: `Landscapes.RoadConstructor`.
Test implementations live in `Source/RoadGeneration/Private/Tests`.
They check shared-node splitting, coincident unconnected nodes, highway-only filtering,
terrain gaps, and native road/junction mesh generation and regeneration.
