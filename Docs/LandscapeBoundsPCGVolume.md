# Landscape Bounds PCG Volume

Place **Landscape Bounds PCG Volume**, assign a graph on its inherited **PCG Component**,
and select **Target Landscape** under **Landscape Bounds**. Press **Match Landscape
Bounds** to rebuild the Brush Component as a world-aligned box covering the landscape.
Selecting a streaming proxy fits its complete landscape, using Unreal's complete
landscape bounds, including known unloaded regions.

**Bounds Padding** adds space on each side in centimeters. The default is zero in
XY and 100 cm in Z so flat terrain is inside the volume. Zero Z padding still gives
a perfectly flat landscape a minimum volume height of 1 cm. Matching resets the
volume's rotation and scale and supports the editor's Undo transaction.

Changing the target or padding does not fit the brush automatically. Press the
button again after changing them or extending the landscape. The PCG Component's
native generation controls remain available; the fit operation marks its data dirty.

To map an image over this volume, use **Get Actor Data** with **Actor Filter = Original**
and **Mode = Get Single Point**, followed by **Get Bounds**. Use its `BoundsMin` and
`BoundsMax` attributes to calculate world-space XY mask coordinates consistently
across PCG partitions.
