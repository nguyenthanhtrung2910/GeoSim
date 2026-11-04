# Editable building Nanite support

The project plugin under `Plugins/Vitruvio` has six source files changed:

- `Source/Vitruvio/Public/VitruvioComponent.h`: adds opt-in `bEnableNanite` and an
  optional generation-build argument. Ordinary Vitruvio components default to off.
- `Source/Vitruvio/Private/VitruvioComponent.cpp`: forwards the option to the main
  generated mesh and regenerates when the option is edited. It tracks individual
  generation until the current result is applied and discards queued older results,
  allowing Bake to reject pending live updates without requesting a new generation.
- `Source/Vitruvio/Public/VitruvioMesh.h`: adds the optional mesh-build argument.
- `Source/Vitruvio/Private/VitruvioMesh.cpp`: enables Nanite, checks material usage,
  uses the full editor mesh build, waits for this mesh's compilation and updates
  its streaming UV densities before it is displayed.
- `Source/VitruvioEditor/Public/VitruvioEditorModule.h` and
  `Source/VitruvioEditor/Private/VitruvioEditorModule.cpp`: defer generation-complete
  notifications to the core ticker. Game-thread tasks pumped during mesh builds
  must not create notification windows while Slate iterates its windows. Pending
  notifications are coalesced and the ticker is removed on module shutdown.

Bakeable Building enables the setting for new editable sessions and saves the
component's current preference for Unbake. No second mesh copy is made by the
building actor to enable Nanite. Materials, textures, shared caches and resource
ownership retain the plugin's existing behavior. Collision still uses Vitruvio's
original collision provider and BlockAll setup.

Only the unique main generated mesh uses this option. Shared instanced props,
replacement meshes and batch generation keep their original settings. A main mesh
containing translucent or other unsupported material blend modes skips Nanite and
logs a warning. Full Nanite generation runs in editor builds; runtime procedural
generation retains the fast non-Nanite path. Bake buildings to assets for game use.

The repository currently ignores `Plugins/Vitruvio`. These local plugin changes
must be preserved separately when replacing or updating the plugin. The pre-change
copies are under `Saved/Backups/VitruvioGeneratedNanite/`.

Validation includes a rendered Bake/Unbake round trip, actual Nanite data before
baking, footprint regeneration, toggling the component setting off and on, and
the normal building regression suite.
