import bpy, os, sys
import xml.etree.ElementTree as ET

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(argv) < 3:
    sys.exit(1)

dae_path = os.path.abspath(argv[0])
tex_dir = os.path.abspath(argv[1])
out_glb = os.path.abspath(argv[2])

# 1. Clear objects cleanly without destroying the scene context
for obj in list(bpy.data.objects):
    bpy.data.objects.remove(obj, do_unlink=True)
for mat in list(bpy.data.materials):
    bpy.data.materials.remove(mat, do_unlink=True)

# 2. Import Collada with full directory & file arguments
dir_name = os.path.dirname(dae_path)
file_name = os.path.basename(dae_path)

bpy.ops.import_scene.blender_collada_full(
    filepath=dae_path,
    directory=dir_name,
    files=[{"name": file_name}]
)

# 3. Parse XML mapping
tree = ET.parse(dae_path)
root = tree.getroot()
ns = {'c': root.tag.split('}')[0].strip('{')} if '}' in root.tag else {'c': ''}

img_map = {}
for img in root.findall('.//c:image', ns):
    img_id = img.attrib.get('id')
    ref = img.find('.//c:ref', ns)
    if ref is not None and ref.text:
        img_map[img_id] = os.path.basename(ref.text.strip())
    else:
        init_from = img.find('.//c:init_from', ns)
        if init_from is not None and init_from.text:
            img_map[img_id] = os.path.basename(init_from.text.strip())

effect_to_img = {}
for effect in root.findall('.//c:effect', ns):
    eff_id = effect.attrib.get('id')
    inst_img = effect.find('.//c:instance_image', ns)
    if inst_img is not None:
        target = inst_img.attrib.get('url', '').lstrip('#')
        if target in img_map:
            effect_to_img[eff_id] = img_map[target]
            continue
    surf_ref = effect.find('.//c:surface//c:ref', ns)
    if surf_ref is not None and surf_ref.text:
        target = surf_ref.text.strip().lstrip('#')
        if target in img_map:
            effect_to_img[eff_id] = img_map[target]

mat_to_img = {}
for mat in root.findall('.//c:material', ns):
    mat_id = mat.attrib.get('id')
    inst = mat.find('.//c:instance_effect', ns)
    if inst is not None:
        eff_url = inst.attrib.get('url', '').lstrip('#')
        if eff_url in effect_to_img:
            mat_to_img[mat_id] = effect_to_img[eff_url]

# 4. Attach textures
for mat in bpy.data.materials:
    base_name = mat.name.split('.')[0]
    img_name = mat_to_img.get(mat.name) or mat_to_img.get(base_name)
    if not img_name:
        continue

    img_path = os.path.join(tex_dir, img_name)
    if not os.path.exists(img_path) and not img_path.lower().endswith('.png'):
        img_path += '.png'
    
    if os.path.exists(img_path):
        mat.use_nodes = True
        bsdf = mat.node_tree.nodes.get("Principled BSDF")
        if not bsdf: continue
        
        img = bpy.data.images.load(img_path)
        tex_node = mat.node_tree.nodes.new('ShaderNodeTexImage')
        tex_node.image = img
        mat.node_tree.links.new(tex_node.outputs['Color'], bsdf.inputs['Base Color'])

# 5. Export GLB
os.makedirs(os.path.dirname(out_glb), exist_ok=True)
if hasattr(bpy.ops.export_scene, "gltf"):
    bpy.ops.export_scene.gltf(filepath=out_glb, export_format='GLB')
elif hasattr(bpy.ops.wm, "gltf_export"):
    bpy.ops.wm.gltf_export(filepath=out_glb, export_format='GLB')

print("GLB export finished successfully:", out_glb)
