"""Skinned, animated glTF export of object models (Goombas, cows, windmills, trees, ...).

The static exporter bakes an object's bind pose into plain meshes. Models that come with skeletal
animations (AnmChr / CHR0 clips) are exported here instead: every bone becomes glTF nodes, every
vertex is skinned to its bone (or blend of up to four bones for weighted vertices), and every clip
that drives the model's bones becomes a glTF animation sampled at 60 frames per second.

Bones and scale. NW4R bones inherit scale in one of two ways, chosen per bone (flag 0x40):
  * classic:  world = parent world * local (T R S), scale is inherited by everything below;
  * "Maya" (segment scale compensation): the bone's own scale stretches only its own segment and
    moves its children (their translation is scaled by it) but is not passed on to their shape.
glTF only has the classic kind, so each bone is two nodes: '<bone>' carries translation + rotation
and '<bone>_scale' below it carries the bone's own scale (the skin joint). Classic children hang
off the parent's scale node; Maya children hang off its frame node with their translation
multiplied by the parent's scale (in the bind pose and in every animation frame).
The construction is checked against the bone poses stored in the game file on every export.
"""

import math

import numpy as np

import brres_to_glb as b

ANIM_FPS = 60
MAYA_SCALE_OFF = 0x40


def rest_world_matrices(m):
    """World matrix of each bone's skin joint as this file's node hierarchy produces it; used to
    check the hierarchy against the file's own bone poses."""
    frame, world = {}, {}
    for bone in m.bones:      # bones are ordered parents-first
        t = np.array(bone.trans, dtype=float)
        if bone.parent is None:
            attach = np.eye(4)
        elif bone.flags & MAYA_SCALE_OFF:
            attach = frame[bone.parent.index]
            t = t * np.array(bone.parent.scale)
        else:
            attach = world[bone.parent.index]
        frame[bone.index] = attach @ b.srt_matrix((1, 1, 1), bone.rot, t)
        world[bone.index] = frame[bone.index] @ np.diag([*bone.scale, 1.0])
    return world


def add_rigged_model(g, m, tex, r, clips, srt_map=None):
    """Add one skinned model (with all matching clips) to glTF writer g.

    clips: {clip name: CHR0 offset}. Returns (clip names exported, max bind-pose error vs file)."""
    nodes = g.j['nodes']
    root = len(nodes)
    nodes.append({'name': m.name, 'children': []})
    nb = len(m.bones)
    frame_base = len(nodes)
    for bone in m.bones:
        t = np.array(bone.trans, dtype=float)
        if bone.parent is not None and bone.flags & MAYA_SCALE_OFF:
            t = t * np.array(bone.parent.scale)
        nodes.append({'name': bone.name, 'translation': [float(x) for x in t],
                      'rotation': b._quat(bone.rot).tolist(), 'children': [frame_base + nb + bone.index]})
    scale_base = len(nodes)
    for bone in m.bones:
        nodes.append({'name': bone.name + '_scale', 'scale': list(bone.scale)})
    for bone in m.bones:
        if bone.parent is None:
            nodes[root]['children'].append(frame_base + bone.index)
        elif bone.flags & MAYA_SCALE_OFF:
            nodes[frame_base + bone.parent.index]['children'].append(frame_base + bone.index)
        else:
            nodes[scale_base + bone.parent.index].setdefault('children', []).append(frame_base + bone.index)

    err = max(float(np.abs(rest_world_matrices(m)[bone.index] - bone.world).max()) for bone in m.bones)

    # Skinned mesh in the bind pose
    mix = b.node_mix(m)
    skin = []
    prims = b.model_primitives(g, 'obj', m, tex, track=True, srt_map=srt_map, skin=skin, mix=mix)
    inv = np.array([np.linalg.inv(bone.world).T.reshape(-1) for bone in m.bones])
    g.j.setdefault('skins', []).append({
        'joints': [scale_base + bone.index for bone in m.bones], 'skeleton': root,
        'inverseBindMatrices': g._accessor(inv, 'MAT4', target=None)})
    nodes.append({'name': m.name + '_mesh', 'mesh': g.mesh(m.name, prims, skin), 'skin': len(g.j['skins']) - 1})
    nodes[root]['children'].append(len(nodes) - 1)
    g.j['scenes'][0]['nodes'].append(root)

    # Animations
    bone_names = {bone.name for bone in m.bones}
    bind = {bone.name: {'scale': np.array(bone.scale), 'rot': b._quat(bone.rot), 'trans': np.array(bone.trans)}
            for bone in m.bones}
    exported = []
    for clip_name, off in clips.items():
        tracks = {n for n, _ in b.Reader.dict(r, off + r.s32(off + 0x10))}
        if not tracks & bone_names:
            continue
        n_frames = b.chr0_frame_count(r, off)
        frames = [b.chr0_pose(r, off, f) for f in range(n_frames)]
        times = g._accessor(np.arange(n_frames, dtype=np.float32) / ANIM_FPS, 'SCALAR', target=None)

        def track(name, kind):
            out = []
            for fr in frames:
                v = fr.get(name, {}).get(kind)
                out.append(bind[name][kind] if v is None else (b._quat(v) if kind == 'rot' else np.array(v)))
            return np.array(out)

        channels, samplers = [], []
        for bone in m.bones:
            for kind, path in (('trans', 'translation'), ('rot', 'rotation'), ('scale', 'scale')):
                vals = track(bone.name, kind)
                ref = bind[bone.name][kind]
                if kind == 'trans' and bone.parent is not None and bone.flags & MAYA_SCALE_OFF:
                    vals = vals * track(bone.parent.name, 'scale')
                    ref = ref * bind[bone.parent.name]['scale']
                if kind == 'rot':      # keep neighbouring quaternions on the same hemisphere
                    for i in range(1, len(vals)):
                        if np.dot(vals[i], vals[i - 1]) < 0:
                            vals[i] = -vals[i]
                if np.allclose(vals, ref, atol=1e-4) or (kind == 'rot' and np.allclose(vals, -ref, atol=1e-4)):
                    continue
                node = scale_base + bone.index if kind == 'scale' else frame_base + bone.index
                samplers.append({'input': times, 'interpolation': 'LINEAR',
                                 'output': g._accessor(vals, 'VEC4' if kind == 'rot' else 'VEC3', target=None)})
                channels.append({'sampler': len(samplers) - 1, 'target': {'node': node, 'path': path}})
        g.j.setdefault('animations', []).append({'name': '%s::%s' % (m.name, clip_name),
                                                 'channels': channels, 'samplers': samplers})
        exported.append(clip_name)
    return exported, err
