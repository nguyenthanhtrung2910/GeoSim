# Footprint buildings

`AFootprintBuilding` lives in `Source/BuildingGeneration/` and owns a
`FootprintSpline` and a `PCGComponent`. It is a target
for Landscape Combinator's Spline Importer **Custom Actor** mode.

## Local Overpass OSM files

Place **OSM Building Importer** (`AOsmBuildingImporter`) from the BuildingGeneration
module to import raw `.osm` or `.xml` files exported by Overpass Turbo. This subclasses
Spline Importer and defaults to **Local File**, **Custom Actor**, and **FootprintBuilding**.
Set **Local File**, select the terrain under **Actors Or Landscapes To Place Splines
Selection**, and choose your building Blueprint as **Actor To Spawn**. Click **Import**.
Enable **Delete Old Splines When Creating Splines** when replacing earlier output.

The loader resolves closed ways tagged `building` and their referenced nodes directly,
including exports with the XML declaration and `<osm>` on the same line. Export the
referenced nodes too: `way["building"]({{bbox}}); (._;>;); out body;` with `[out:xml];`.
Open outlines and `building=no` are skipped; relation-only multipolygons and courtyard
holes are not supported by this loader. OSM tags, including `height` and `building:levels`,
are attached to each spline as OSM user data. These tags do not automatically configure
the building's PCG graph. Imported footprint edges are linear.

Other file formats and Overpass sources retain the parent importer's loading behavior.
The only shared plugin change is making its dataset-loading method virtual so this
project-specific loader can override it. No downloaded files are rewritten.

## Building generation

1. Create a Blueprint subclass of `FootprintBuilding`.
2. Select its PCG component and assign a wrapper graph that reads spline data
   from **Self**, supplies City Sample data, and calls
   `Assign_CitySampleBuildings`. Connect any subsequent generation/spawning
   stages required by that graph. The C++ actor does not supply City Sample data
   or create this wrapper graph.
3. In Spline Importer, disable **Use Landscape Splines**, set **Spline Owner
   Kind** to **Custom Actor**, and select the Blueprint as **Actor To Spawn**.
4. Import the footprints. Each imported spline receives its own building actor.
5. Select the resulting actors and click **Generate Building**. Use **Clear
   Building** to remove their PCG output while retaining their footprints.

Generation requires a closed spline with at least three points and an assigned
graph. It runs on demand: spawning, loading, and editing the actor do not
automatically generate buildings with the default component settings. This
avoids generating before the importer has filled the spline.

**Generate Building** calls `PreprocessFootprint()` before invalidating PCG's
cached input and generating. By default, **Flatten Footprint** moves all corners
to the highest corner's world-space Z, preserving their world-space X/Y and the
closed loop. Custom spline tangents are flattened too. Each building uses its
own elevation; this does not sample terrain inside the footprint or create
foundation geometry. Disable **Flatten Footprint** to keep the imported heights.
Preprocessing changes the source spline; reimport to restore its original heights.

Use the actor's **Generate Building** action: the PCG component's own **Generate**
button bypasses preprocessing. The actor does not derive height, floor count, or
style from OSM metadata.

The automation tests are `GeoSim.FootprintBuilding.ImportTarget`,
`GeoSim.FootprintBuilding.Preprocess`, and
`GeoSim.BuildingGeneration.ClassRedirects`. A class redirect preserves saved
references to the former `/Script/GeoSim.FootprintBuilding` path.
