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
4. Set **Spawned Actors Path** to a World Outliner folder such as `Generated/Roads`,
   or leave it empty to use the generator's folder. Click **Generate Roads**. **Clear Roads** deletes this generator's scene and children;
   the importer cache and Landscape splines are retained. A failed replacement keeps the previous scene.
5. To deform terrain, assign **Target Landscape** and click **Generate Landscape Splines**.
   **Clear Landscape Spline Points** removes only this generator's splines and deformation.

For existing levels, the former RoadConstructorFromSplineImporter actor redirects to
RoadGenerator. Create and assign an OSMRoadImporter with the old source, bounds and
terrain selection settings. SplineImporter now exclusively uses its original spline
Import/Delete workflow, including buildings; its Road Data Mode is removed.

Styles determine lane widths, sidewalks, materials and props. A positive integer OSM
`lanes` tag overrides only the number of Driving lanes: half go on each side, with
the extra lane on the left for odd totals (for example, 3 means 2 left and 1 right).
Existing driving lanes retain their attributes; extra lanes copy the outermost driving
lane on that side, borrowing from the opposite side if needed. Non-driving lanes and
all other style settings are retained. Generated roads receive copies of their selected
Highway Style, falling back to Default Road Style for missing or empty highway entries.
Spans split from one surviving way share that copy; ways merged at two-arm nodes share
the surviving style and retain all source IDs in tags. Unrelated ways remain independent.
Generated styles are embedded in the output scene; source assets are
unchanged and no separate style asset is created. Missing, invalid,
zero, or excessive counts (over 64) retain the highway style's original layout, as do
styles with no Driving lane to copy. OSM `width`, `oneway`, and directional lane tags
are not translated into style overrides.

The scene serializes its per-way style collection so World Partition road packages
can resolve their style references after reopening the map. For roads generated before
this persistence fix, refresh the importer cache, click **Generate Roads**, then **Save All**
to replace missing style references. Rebuilding code alone cannot restore unsaved styles.

**Surface Offset Cm** controls road mesh elevation relative to the cached imported points.
**Roadbed Height Offset** independently controls Landscape spline elevation relative to
those same points. Both accept negative values. For imported Z = 1,000 cm, Surface Offset =
20 cm and Landscape Height Offset = -10 cm produce road controls at 1,020 cm and spline
controls at 990 cm. Apply mesh changes with **Generate Roads** and terrain changes with
**Generate Landscape Splines**. Details edits wait for the corresponding generation button.
Neither operation moves the other output.

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
The generator's **Terrain Deform** category applies settings to every generated road.
**Generate Landscape Splines** applies deformation; **Clear Landscape Spline Points**
removes it. There is no Enable checkbox. **Spline Edit Layer** accepts only **Splines**,
which must be the reserved
spline layer type. An ordinary sculpt layer named Splines is rejected without being renamed
or overwritten. Rename conflicting layers yourself before generating.

The deformation corridor includes **all lane types** on both sides, including sidewalks,
verges, and medians. **Road Width Scale** follows RoadActor's convention: the default 0.5
stores half the total lane width in each control point's **Half-Width**.
**Side Falloff Scale** uses Road Constructor's native behavior: it multiplies the plugin's
default base distance (700 cm), with scale limited to 0.05–3 and final distance clamped to
25–3,500 cm. The default scale of 1 produces 700 cm. There is no separate distance control.
**Start Falloff** and **End Falloff** set separate endpoint blend distances (default 2,000 cm).
**Start Extension** and **End Extension** extend the full-width corridor before those blends
(default 1,000 cm). Distances accept zero. The reserved layer
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
the current roads. **Clear Landscape Spline Points** removes them and rebuilds the reserved
layer, preserving original sculpt layers and other generators' splines. Spline ownership
is saved independently, so clearing or regenerating roads does not prevent later spline
cleanup. Clearing either output does not clear the importer's cache.

Deformation uses a layer whose class is `ULandscapeEditLayerSplines`, rather than relying on
its display name. Earlier output stamped into an ordinary sculpt layer cannot be separated
from that layer's original heights automatically; restore that layer from Undo or a backup.

**Spawned Actors Path** applies to every generated scene, road, junction, connector,
transition, and attached prop actor. It is a World Outliner folder, not a filesystem path.
An explicit generation/pipeline folder override takes priority. After editing only this
path, click **Generate Roads** to relocate existing output while retaining its actors and meshes.

## RoadScene ownership and editing

**Road Scene Size Cm** assigns junction ownership to a spatial grid (default 100,000 cm,
or 1 km). All road ends at one OSM junction stay in the same scene. A span joining nodes
in different cells is split halfway along its imported polyline, with both halves meeting
at the same position. This avoids splitting a junction across scene ownership boundaries.
It is an ownership grid, not a hard limit on road count or scene bounds: long roads can
extend beyond their owning cell. Cuts can affect curve fitting and style-prop spacing.

The generator tracks every scene under **Generated Scenes**. Generate Roads replaces
previous output only after all candidate scenes build successfully. Clear Roads removes
all owned scenes; Landscape spline generation and clearing cover all of them. Previously
saved single-scene output remains recognized and is replaced on the next generation.

Imported roads now retain native property-edit, movement, and undo/redo behavior.
Connected-road undo may refresh its entire owning scene, while other scenes remain
unchanged by that callback. Loading/BeginPlay still preserve saved imported output.
Native shared-style live updates can touch multiple scenes, and native Props asset
callbacks can rebuild a scene independently of the generator.

## Generation change checks

**Generate Roads** compares only selected importer data and relevant parameters in
RoadGenerator's Details panel. It does not hash generated curves, junctions, surface
meshes, or props. Unchanged inputs retain the existing scene and its output, including
manual edits and deletions. Imported roads use native editor callbacks for property
edits, movement, and undo/redo. Connected-road undo can refresh the owning RoadScene;
load/play callbacks continue to preserve saved output.

Road inputs include selected cached ways, active road settings, effective highway style
references and actor label. Output folder changes relocate actors without rebuilding geometry. The contents of referenced RoadStyle, Props,
and profile assets are not hashed. Selecting a different effective style triggers road
preparation; editing the contents of the same asset does not. Unused highway styles,
terrain settings, and parameters for disabled features do not invalidate road generation.

The stages still run in dependency order:

1. **Prepare road curves:** importer data, style selection, or road shape/placement
   parameter changes create source roads and curves. Failed replacement preserves the
   previous scene.
2. **Solve junctions:** new source roads or changed junction parameters determine shared-node
   gates and cuts. Junction-only changes retain source roads.
3. **Build surface meshes:** new junction solutions or changed corner-removal policy rebuild
   road, connector, and junction surfaces. Native Road Constructor mesh construction also
   generates props from the applied style, including base-curve and lane Props settings.
   There is no separate prop-generation stage or custom prop-output tracking.

Input baselines are recorded only after every requested stage succeeds. They last for the
current editor session; the first generation after loading establishes a baseline.
Use **Clear Roads** followed by **Generate Roads** to force a rebuild after manual output
changes or edits inside referenced assets. The installed plugin's asset editors can still
invoke their own native rebuild callbacks independently of this adapter.

Landscape spline generation retains its separate input/output validation. Terrain settings
apply only through **Generate Landscape Splines**; editing **Terrain Deform** does not regenerate output.

## Connectivity and output

Shared positive OSM node IDs determine connectivity. Ways are split at shared nodes,
so their junction endpoints remain fixed while Road Constructor rounds interior controls.
Only nodes with at least three connecting road arms create junctions; positions alone never create a junction. **Highway Filter** is the only tag-based
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
Two-arm shared nodes are handled before actor creation. Compatible ways using the same
selected source style and effective lane layout merge regardless of angle; the shared point
remains a curve control and all source way IDs remain in the actor's tags. Merge comparisons
use source assets rather than per-way copies. Reversing asymmetric lane layouts remains
protected, and intentional split loops remain split. Scene ownership boundaries are retained.

Nodes with three or more connecting road arms create junctions. Count road arms, not distinct
OSM way IDs: a through-way plus one branch has three arms and remains a T-junction.
Two-arm style or lane-count changes receive a separate native Road Transition segment.
**Road Transition Length Cm** controls its desired combined length (default 6,000 cm);
each approach contributes at most a quarter of its remaining length. Source and target styles
are oriented to the transition direction and saved with the scene. The native lane-width
taper handles reductions and expansions using an outer-edge-aligned union of lane stacks.
It does not blend materials or lane profiles. No junction actor is added for a transition.

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

The output scene, source road controls, junction actors, connector roads and their
surface meshes are saved with the level and its external actor packages. Imported
junctions save durable copies of the plugin's transient gate/connector references;
loading restores those references without regenerating roads or replacing meshes.
Connector styles are exported with their owning connector actor. Both source and
connector roads use the imported actor's load-safe lifecycle.

Output created before junction persistence was added must be generated once with the
new code and saved with **Save All**. Reopening the editor or entering play does not
solve junctions, rebuild meshes, or automatically recreate missing legacy output.
Imported scenes bypass the vendor scene's load/play callbacks, and imported roads
bypass its load/undo callbacks that request automatic connectivity discovery. They keep
the normal Actor lifecycle; property edits, moves, and undo do not queue regeneration. Geometric crossings
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
