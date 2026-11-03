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

When the package exposes a `Level_of_Detail` dropdown with a `high` option, the
generator sets that option before the first model generation. It uses the exact
spelling supplied by the package and preserves it as a user override. Packages
without a compatible dropdown keep their own default.

## Generated actors

BuildingGenerator spawns standard **Vitruvio Actors** with the imported footprint,
configured height overrides, and high detail when supported by the rule package.
It does not capture persistent geometry or build Nanite data. Existing generated
actors are replaced only when you run generation with replacement enabled.

## Migration and validation

The removed persistent building actor and component redirect to standard Vitruvio
types. Their automatic geometry capture and Nanite build support have been
removed. Regenerate old output to return to the standard workflow.

The importer class redirect is retained. Previously imported actors remain in the
level; remove old output when switching to this workflow. Output replacement,
folders, and generation now belong to BuildingGenerator. The legacy
VitruvioBuildingsFromSplines class has been removed; saved references redirect to
BuildingGenerator. Assign an importer to migrated actors and import source data
before generating. BuildingGenerator now generates Vitruvio actors only.

Tests cover GeoSim.BuildingGeneration.* and GeoSim.FootprintBuilding.*. Verify
actual Vitruvio appearance using the chosen installed rule package.
