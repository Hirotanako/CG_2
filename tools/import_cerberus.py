"""Convert the bundled Cerberus FBX into the OBJ format used by the demo.

Run from the repository root with Blender 4/5:
    blender --background --python tools/import_cerberus.py
"""

from pathlib import Path

import bpy


root = Path.cwd()
source = root / "Cerberus_by_Andrew_Maximov" / "Cerberus_LP.FBX"
target = root / "Cerberus_by_Andrew_Maximov" / "Cerberus_LP.obj"

if not source.exists():
    raise FileNotFoundError(source)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=str(source), use_anim=False)

meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
if not meshes:
    raise RuntimeError(f"No mesh objects found in {source}")

bpy.ops.object.select_all(action="DESELECT")
for obj in meshes:
    obj.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]

# Bake FBX object transforms, triangulate, and write normals/UVs. The runtime
# loader deliberately stays tiny and only needs the resulting static OBJ.
bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
bpy.ops.wm.obj_export(
    filepath=str(target),
    export_selected_objects=True,
    export_materials=False,
    export_triangulated_mesh=True,
    export_normals=True,
    export_uv=True,
    forward_axis="NEGATIVE_Z",
    up_axis="Y",
)

print(f"Exported {len(meshes)} mesh object(s) to {target}")
