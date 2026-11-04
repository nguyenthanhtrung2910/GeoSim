# Building import and generation

The workflow is `OsmBuidingImporter -> cached building records -> BuildingGenerator -> output actors`.

## Import

Place **OSM Building Importer**, choose the source, configure **Terrain Selection**,
and click **Import**. Local files must contain raw OSM XML with closed building ways
and their referenced nodes. Overpass queries must return XML. Relation multipolygons,
courtyard holes, and Overpass JSON are not supported.

The importer creates no actors, splines, or meshes. It caches OSM IDs, raw tags, and
complete terrain-following footprint rings in world centimeters. Coordinate conversion
uses Landscape Combinator utilities. Entire footprints with missing terrain hits are
skipped, with their IDs and failed corners logged.

The cache is memory-only: reimport after reopening or changing source, coordinates,
placement, or bounds. Failed imports invalidate the cache. **Clear** clears source
data and invalidates pending results; it does not delete generated buildings.

## Generate

Place **Building Generator**, assign **Importer** and **Rule Package**. Click
**Generate Buildings**. Generation only reads a valid cache; it never downloads.
The generator owns its output actors and **Clear Buildings** action. By default it
replaces previous output. **Flatten Footprints** flattens copies to their highest
corner while preserving the original cache.

### Heights

Configure **Heights > Min Floor Height Meters**, **Max Floor Height Meters**, and
**Seed**. One floor height is sampled per OSM ID, reproducibly for the same seed
and range. Building height follows RoadForge's positive-value priority:

1. OSM `height` in meters (including a trailing ` m`).
2. Otherwise `building:levels * sampled floor height`.
3. Otherwise **Default Building Height Meters**, initially 12 m.

The optional floor-count override is `max(1, floor(building height / floor height))`.
Explicit height takes priority when it disagrees with the levels tag. No interior
floor slabs are implied by this count.

### Vitruvio rule attributes

Assign **Rule Package**, then enter the numeric keys exposed by that package:

- **Floor Height Attribute** receives the sampled floor height.
- **Building Height Attribute** receives the calculated building height.
- **Floor Count Attribute** optionally receives the derived floor count.

Full style/import keys are accepted, as are unique names displayed in Vitruvio's
Details panel (for example `Floor_Height`). Ambiguous displayed names require the
full key. Error messages list available numeric keys. Empty names retain package
defaults. Names must resolve to distinct attributes. **Rule Units Per Meter**
is 1 for rules using meters, or 100 for centimeters; this affects heights, not count.

The generator evaluates the package's attributes without generating geometry, waits
for completion, validates the configured keys and numeric types, sets per-actor
user overrides, then reevaluates and generates. Missing or wrongly typed keys are
reported instead of silently generating defaults. The shared rule-package asset
is not modified.

Auto Generate is temporarily disabled while package defaults load. It is enabled
after the overrides are successfully applied, and the first model is generated
automatically. There is no need to enable it by hand; toggling it later retains
the user-set height overrides.

Output actors preserve raw tags as UOSMUserData on their root component.

The configured floor height, building height, and floor count attributes are
overridden. Enable **High Detail** under **Buildings > Vitruvio** on the generator
to also set supported `Level_of_Detail` dropdown attributes to `high`. The option
is off by default, leaving the package's detail default unchanged. Packages without
a supported high option keep their defaults and log a warning. This affects newly
generated buildings; the chosen value is saved through Bake and Unbake. All other
rule parameters retain their defaults.

## Bake and Unbake

For a manually placed Bakeable Building, press **Unbake / Edit** to create its
Vitruvio component and a closed 10 m square footprint. Select the Vitruvio
component, assign a rule package, and edit the footprint as needed. Pressing
Unbake again keeps the active editing session. The Details component tree
refreshes after Bake and Unbake without needing to reselect the actor.

BuildingGenerator now creates **Bakeable Building** actors. New actors start an
editable Vitruvio session with the imported footprint and configured height/floor
overrides, plus the optional High Detail override. Other rule parameters retain
their package defaults.

- **Bake** saves the current live-generated result without requesting regeneration.
  Wait for any live update to finish before pressing Bake. It creates mesh, material,
  and texture assets under **Bake Directory** (default `/Game/Generated/Buildings`),
  then switches to Baked mode and removes the procedural component and preview.
- **Unbake / Edit** regenerates from saved footprint, rule package, explicit user
  overrides, replacement settings, and seed. The last bake remains visible until
  generation succeeds. In Editable mode, footprint and rule edits regenerate
  automatically. There is no separate Generate or Rebake button on this actor.

Only pressing Bake converts resources to assets. Failed conversions retain the
editable output and inputs. Instanced props, transforms, and custom data are
preserved. Newly created baked meshes have Nanite enabled; existing referenced
mesh assets retain their settings. Each successful bake creates a new asset folder;
Unbake keeps previous assets on disk and never deletes shared assets.

Baked displays prestream their textures for five seconds when first shown, then
return to normal distance-based mip streaming. No additional UV-density rebuild,
material streaming-data build, or explicit asset-compilation wait is performed by
the baker.

Save the level/actors after baking to retain the mode and asset references. On
reload and in PIE, Baked mode displays the baked assets without creating a procedural
component. Editable mode automatically regenerates from the saved inputs on the next
actor tick. This also restores buildings that have never been baked. The last bake,
when present, stays visible until generation succeeds; afterward the editable result
replaces it. PIE generates its own procedural content without changing the editor actor.
Generation is asynchronous, so a never-baked building appears when its first result
is ready. Bake completed buildings to avoid procedural startup work in the game.

Inactive baked display components are removed after Unbake succeeds; only soft
asset references and placement data remain. Removing the procedural preview after
Bake releases its actor-owned references, though Vitruvio's shared caches and
Unreal's asset/undo systems may retain some resources. There are no custom texture,
cache, or resource-ownership hooks. The project Vitruvio plugin now supports
optional Nanite generation for the editable main building mesh, enabled by default
by Bakeable Building. Change **Enable Nanite** on its Vitruvio component to opt out;
the preference survives Bake and Unbake. This editor build takes longer than the
normal preview build. Shared instanced props and batch generation keep their
existing behavior. Main meshes with translucent/unsupported blend modes retain
the normal rendering path. See [project plugin changes](VitruvioNanite.md).

## PCG exclusion

Assign actor tags directly in the building's standard **Actor > Tags** property,
for example `BuildingExclusion`. There are no generator tag-management settings
or exclusion buttons. Multiple buildings can be selected to edit their tags together.

The saved footprint spline is attached to the building root and shared with
Vitruvio authoring. Bake keeps this same component; Unbake reuses it. Save/reload
preserves the points, transform and tags. Existing baked actors can restore their
spline from saved footprint data. Previously created exclusion boxes are removed
on load. Existing actor/component tags remain as saved. Save changed building
actors/level after editing tags.

In the target PCG graph, use **Get Actor Data** to select actors by your actor tag
and collect spline components. Use the spline in the graph's footprint/interior
exclusion branch before spawning meshes. If needed, assign a component tag
manually to filter specifically to the footprint spline. Regenerate the target
PCG output after changing tags.

## Migration and validation

The persistent building actor, persistent component, and shared resource library
have been removed. Saved actor/component class references redirect to the standard
Vitruvio types. Regenerate existing output to replace previously persistent buildings.

The importer class redirect is retained. Previously imported actors remain in the
level; remove old output when switching to this workflow. Output replacement,
folders, and generation now belong to BuildingGenerator. The legacy
VitruvioBuildingsFromSplines class has been removed; saved references redirect to
BuildingGenerator. Assign an importer to migrated actors and import source data
before generating. BuildingGenerator now generates Bakeable Building actors. Regenerate standard Vitruvio actors to obtain the Bake/Unbake controls.

Tests cover GeoSim.BuildingGeneration.* and GeoSim.FootprintBuilding.*. Verify
actual Vitruvio appearance using the chosen installed rule package.
