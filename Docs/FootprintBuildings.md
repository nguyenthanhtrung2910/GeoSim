# Footprint buildings

`AFootprintBuilding` lives in `Source/BuildingGeneration/` and owns a
`FootprintSpline` and a `PCGComponent`. It is a target
for Landscape Combinator's Spline Importer **Custom Actor** mode.

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
