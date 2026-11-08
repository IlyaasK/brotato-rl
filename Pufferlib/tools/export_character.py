#!/usr/bin/env python3
"""Export the Temple Run 2 runner ("Guy Dangerous") for raylib.

Source of truth (shipped data, nothing invented):

    recon/unity_assets/allbundles/assets/Bundles/20_03/Characters/0/
        Guy_Dangerous/0/characterdata_guy_dangerous.unity3d
        _characterdata_shared.unity3d

The guy bundle holds one ``Mesh`` (``GuyDangerous``, 1351 verts / 1930 tris,
25-bone skin with per-vertex indices+weights), four ``Material``s, three
``Texture2D``s, 135 ``Transform``/``GameObject`` pairs (the bone hierarchy of
the prefab ``guy_dangerous`` plus two more baked prefab instances) and two
legacy ``AnimationClip``s (``Run01``, ``Idle01``).  The shared bundle holds the
remaining nine clips used by every runner (``Jump01``, ``Slide01``,
``SlideEnter01``, ``RunStumble01``, ``Death*``, ``ZiplineIdle01``).

Outputs (see ``resources/templerun2/character/README.md`` for the contract):

    resources/templerun2/character/guy.glb          skinned mesh + skeleton (bind pose)
    resources/templerun2/character/guy.obj          static fallback mesh
    resources/templerun2/character/guy.mtl          static fallback material (map_Kd guy.png)
    resources/templerun2/character/guy.png          albedo (1024x1024, from the bundle)
    resources/templerun2/character/animations.json  clip + bone catalogue
    resources/templerun2/character/README.md        conventions / provenance

Conventions
-----------
* Vertices are Unity coordinates with **X negated** (the standard left-handed
  Unity -> right-handed glTF/raylib conversion; UnityPy's OBJ writer does the
  same).  Winding is reversed so front faces stay front (CCW).
* Y is up, +Z is the character's forward (measured: the bind-pose feet point at
  +Z and the right-thigh bone sits at +X).  Feet are at y = 0, the mesh origin
  is the prefab root ``guy_dangerous``; units are Unity metres (the mesh is
  1.24 m tall).
* glTF ``TEXCOORD_0`` is written bottom-up (``v' = 1 - v``); the OBJ fallback
  keeps Unity's ``vt`` values verbatim.
* ``animations.json`` sets ``yaw_offset_deg = 0`` from the measurement above so
  the runner faces the track's ``forward = (sin yaw, cos yaw)``.

Animation note: the legacy clips store rotations in compressed quaternion
curves. AssetStudio implements PackedQuatVector.UnpackQuats; this exporter does
not yet decode or bake those curves. Clip metadata is catalogued in
``animations.json``.

Usage:
    recon/unity_assets/.venv/bin/python tools/export_character.py
"""
from __future__ import annotations

import argparse
import io
import json
import math
import struct
import sys
from pathlib import Path

import UnityPy
from UnityPy.helpers.MeshHelper import MeshHandler

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUNDLES = ROOT / "recon/unity_assets/allbundles"
DEFAULT_OUT = ROOT / "resources/templerun2/character"

CHAR_DIR = "assets/Bundles/20_03/Characters/0"
GUY_BUNDLE = CHAR_DIR + "/Guy_Dangerous/0/characterdata_guy_dangerous.unity3d"
SHARED_BUNDLE = CHAR_DIR + "/_characterdata_shared.unity3d"

PREFAB_ROOT = "guy_dangerous"
MESH_NAME = "GuyDangerous"
MATERIAL_NAME = "guy_dangerous"
DEFAULT_FPS = 30.0

# clip object name -> state-ish name used by the runtime
CLIP_NAMES = {
    "Run01": "run",
    "Jump01": "jump",
    "Slide01": "slide",
    "SlideEnter01": "slide_enter",
    "Idle01": "idle",
    "RunStumble01": "stumble",
    "Death01": "death",
    "DeathFalling01": "death_falling",
    "DeathWaterfall01": "death_waterfall",
    "DeathCartHead01": "death_cart_head",
    "ZiplineIdle01": "zipline_idle",
}
# which clip is the usable one for each gameplay state (first existing wins)
CLIP_ORDER = ["Run01", "Idle01", "Jump01", "Slide01", "SlideEnter01",
              "RunStumble01", "Death01", "DeathFalling01", "DeathWaterfall01",
              "DeathCartHead01", "ZiplineIdle01"]


# --------------------------------------------------------------------------- #
# small helpers
# --------------------------------------------------------------------------- #
def fmt(x: float) -> str:
    if not math.isfinite(x):
        x = 0.0
    s = f"{x:.6f}".rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def quat_to_rows(x: float, y: float, z: float, w: float):
    """Row-major 3x3 rotation matrix from a Unity quaternion (x, y, z, w)."""
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    return (
        (1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)),
        (2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)),
        (2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)),
    )


IDENTITY4 = ((1.0, 0.0, 0.0, 0.0), (0.0, 1.0, 0.0, 0.0),
             (0.0, 0.0, 1.0, 0.0), (0.0, 0.0, 0.0, 1.0))


def mat_mul(a, b):
    return tuple(tuple(sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4))
                 for r in range(4))


def mat_invert(m):
    """General 4x4 inverse (row-major, column-vector convention)."""
    a = [list(row) for row in m]
    inv = [list(row) for row in IDENTITY4]
    for i in range(4):
        pivot = max(range(i, 4), key=lambda r: abs(a[r][i]))
        if abs(a[pivot][i]) < 1e-12:
            raise ValueError("singular matrix")
        a[i], a[pivot] = a[pivot], a[i]
        inv[i], inv[pivot] = inv[pivot], inv[i]
        d = a[i][i]
        a[i] = [v / d for v in a[i]]
        inv[i] = [v / d for v in inv[i]]
        for r in range(4):
            if r == i:
                continue
            f = a[r][i]
            if f:
                a[r] = [a[r][c] - f * a[i][c] for c in range(4)]
                inv[r] = [inv[r][c] - f * inv[i][c] for c in range(4)]
    return tuple(tuple(row) for row in inv)


def xform(m, v):
    x, y, z = v
    return (m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3],
            m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3],
            m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3])


def unity_matrix4x4(mat) -> tuple:
    """UnityPy Matrix4x4f -> row-major 4x4 tuple."""
    return (
        (mat.e00, mat.e01, mat.e02, mat.e03),
        (mat.e10, mat.e11, mat.e12, mat.e13),
        (mat.e20, mat.e21, mat.e22, mat.e23),
        (mat.e30, mat.e31, mat.e32, mat.e33),
    )


def mirror_x(m):
    """S * M * S for S = diag(-1, 1, 1, 1)."""
    s = (-1.0, 1.0, 1.0, 1.0)
    return tuple(tuple(s[r] * m[r][c] * s[c] for c in range(4)) for r in range(4))


def decompose(m):
    """Row-major TRS matrix -> (translation, quaternion xyzw, scale)."""
    t = (m[0][3], m[1][3], m[2][3])
    cols = [(m[0][i], m[1][i], m[2][i]) for i in range(3)]
    sx = math.sqrt(sum(v * v for v in cols[0])) or 1.0
    sy = math.sqrt(sum(v * v for v in cols[1])) or 1.0
    sz = math.sqrt(sum(v * v for v in cols[2])) or 1.0
    r = [[cols[c][r] / (sx, sy, sz)[c] for c in range(3)] for r in range(3)]
    tr = r[0][0] + r[1][1] + r[2][2]
    if tr > 0:
        s_ = math.sqrt(tr + 1.0) * 2
        w = 0.25 * s_
        x = (r[2][1] - r[1][2]) / s_
        y = (r[0][2] - r[2][0]) / s_
        z = (r[1][0] - r[0][1]) / s_
    elif r[0][0] > r[1][1] and r[0][0] > r[2][2]:
        s_ = math.sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2
        w = (r[2][1] - r[1][2]) / s_
        x = 0.25 * s_
        y = (r[0][1] + r[1][0]) / s_
        z = (r[0][2] + r[2][0]) / s_
    elif r[1][1] > r[2][2]:
        s_ = math.sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2
        w = (r[0][2] - r[2][0]) / s_
        x = (r[0][1] + r[1][0]) / s_
        y = 0.25 * s_
        z = (r[1][2] + r[2][1]) / s_
    else:
        s_ = math.sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2
        w = (r[1][0] - r[0][1]) / s_
        x = (r[0][2] + r[2][0]) / s_
        y = (r[1][2] + r[2][1]) / s_
        z = 0.25 * s_
    return t, (x, y, z, w), (sx, sy, sz)


# --------------------------------------------------------------------------- #
# bundle access
# --------------------------------------------------------------------------- #
def load_env(bundles_dir: Path, rel: str):
    path = bundles_dir / rel
    if not path.exists():
        raise SystemExit(f"missing bundle: {path}")
    return UnityPy.load(str(path))


def read_all(env):
    """path_id -> (ObjectReader, parsed object or None)."""
    out = {}
    for o in env.objects:
        try:
            d = o.read()
        except Exception:
            d = None
        out[o.path_id] = (o, d)
    return out


def obj_name(pair):
    return getattr(pair[1], "m_Name", None) if pair else None


# --------------------------------------------------------------------------- #
# mesh / skeleton extraction
# --------------------------------------------------------------------------- #
class MeshData:
    def __init__(self):
        self.positions = []
        self.normals = []
        self.uvs = []
        self.bone_indices = []
        self.bone_weights = []
        self.triangles = []
        self.bindposes = []
        self.bones = []          # (name, path_id)
        self.bone_parent = []    # index or -1
        self.mesh_name = ""


def extract_mesh(guy_objs):
    """Locate the SMR/mesh of the shipped runner and pull every channel out."""
    smrs = [(pid, pair) for pid, pair in guy_objs.items()
            if pair[0].type.name == "SkinnedMeshRenderer"]
    if not smrs:
        raise SystemExit("no SkinnedMeshRenderer in the guy bundle")

    chosen = None
    for pid, (reader, smr) in smrs:
        if smr is None:
            continue
        parent = smr.m_GameObject.read().m_Transform.read().m_Father
        pname = None
        if parent is not None:
            pname = parent.read().m_GameObject.read().m_Name
        if pname == PREFAB_ROOT:
            chosen = (pid, smr)
            break
    if chosen is None:
        chosen = smrs[0]
    smr = chosen[1]

    mesh = smr.m_Mesh.read()
    if mesh is None:
        raise SystemExit("SkinnedMeshRenderer has no mesh")

    handler = MeshHandler(mesh)
    handler.process()
    md = MeshData()
    md.mesh_name = mesh.m_Name
    md.positions = [(v[0], v[1], v[2]) for v in handler.m_Vertices]
    md.normals = [(v[0], v[1], v[2]) for v in (handler.m_Normals or [])]
    md.uvs = [(v[0], v[1]) for v in (handler.m_UV0 or [])]
    if handler.m_BoneIndices is None or handler.m_BoneWeights is None:
        raise SystemExit("mesh has no skin weights")
    md.bone_indices = [tuple(int(c) for c in w) for w in handler.m_BoneIndices]
    md.bone_weights = [tuple(float(c) for c in w) for w in handler.m_BoneWeights]
    md.triangles = [t for t in handler.get_triangles()]
    md.bindposes = [unity_matrix4x4(bp) for bp in (mesh.m_BindPose or [])]

    bones = []
    for bptr in smr.m_Bones:
        tr = bptr.read()
        if tr is None:
            bones.append((f"bone_{len(bones)}", None))
        else:
            bones.append((tr.m_GameObject.read().m_Name, tr))
    # parent index inside the joint set (from each bone's Transform.m_Father)
    by_reader = {}
    for i, (n, tr) in enumerate(bones):
        if tr is not None:
            by_reader[id(tr.object_reader)] = i
    for i, (n, tr) in enumerate(bones):
        md.bone_parent.append(-1)
        if tr is None:
            continue
        father = tr.m_Father
        if father is None:
            continue
        try:
            ftr = father.read()
        except Exception:
            ftr = None
        if ftr is None:
            continue
        j = by_reader.get(id(ftr.object_reader))
        if j is not None:
            md.bone_parent[i] = j
    return smr, mesh, md, bones


def find_material_texture(guy_objs, smr):
    """Return (material object, Texture2D object, texture path_id)."""
    mat = None
    for ptr in (smr.m_Materials or []):
        try:
            m = ptr.read()
        except Exception:
            m = None
        if m is not None and m.m_Name == MATERIAL_NAME:
            mat = m
            break
    if mat is None:
        for ptr in (smr.m_Materials or []):
            try:
                m = ptr.read()
            except Exception:
                m = None
            if m is not None:
                mat = m
                break
    tex = None
    tex_pid = None
    for key, val in (getattr(mat.m_SavedProperties, "m_TexEnvs", None) or []):
        if key == "_MainTex":
            tex_pid = val.m_Texture.m_PathID
            try:
                tex = val.m_Texture.read()
            except Exception:
                tex = None
    return mat, tex, tex_pid


# --------------------------------------------------------------------------- #
# glTF / GLB writer
# --------------------------------------------------------------------------- #
class GlbBuilder:
    def __init__(self):
        self.blob = bytearray()
        self.views = []
        self.accessors = []

    def add_view(self, data: bytes, target=None) -> int:
        pad = (-len(self.blob)) % 4
        self.blob += b"\0" * pad
        off = len(self.blob)
        self.blob += data
        view = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.views.append(view)
        return len(self.views) - 1

    def add_accessor(self, view, comp_type, count, type_str, mn=None, mx=None) -> int:
        acc = {"bufferView": view, "componentType": comp_type,
               "count": count, "type": type_str}
        if mn is not None:
            acc["min"] = mn
            acc["max"] = mx
        self.accessors.append(acc)
        return len(self.accessors) - 1


ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
FLOAT, UBYTE, USHORT, UINT = 5126, 5121, 5123, 5125


def build_glb(md: MeshData, png_bytes: bytes, material_name: str,
              joints: list, ibm: list, node_locals: list) -> bytes:
    """Write a glTF 2.0 binary: one skinned primitive, one bone node per joint."""
    g = GlbBuilder()
    nverts = len(md.positions)

    # --- geometry ---------------------------------------------------------
    pos = bytearray()
    for p in md.positions:
        pos += struct.pack("<3f", -p[0], p[1], p[2])
    minv = [min(-p[0] for p in md.positions),
            min(p[1] for p in md.positions),
            min(p[2] for p in md.positions)]
    maxv = [max(-p[0] for p in md.positions),
            max(p[1] for p in md.positions),
            max(p[2] for p in md.positions)]
    a_pos = g.add_accessor(g.add_view(bytes(pos), ARRAY_BUFFER), FLOAT,
                           nverts, "VEC3", minv, maxv)

    attrs = {"POSITION": a_pos}
    if len(md.normals) == nverts:
        nb = bytearray()
        for n in md.normals:
            nb += struct.pack("<3f", -n[0], n[1], n[2])
        attrs["NORMAL"] = g.add_accessor(g.add_view(bytes(nb), ARRAY_BUFFER),
                                         FLOAT, nverts, "VEC3")
    if len(md.uvs) == nverts:
        ub = bytearray()
        for uv in md.uvs:
            ub += struct.pack("<2f", uv[0], 1.0 - uv[1])
        attrs["TEXCOORD_0"] = g.add_accessor(g.add_view(bytes(ub), ARRAY_BUFFER),
                                             FLOAT, nverts, "VEC2")

    # joints / weights (glTF images: v flipped above already)
    jb = bytearray()
    wb = bytearray()
    for idx, wgt in zip(md.bone_indices, md.bone_weights):
        total = sum(wgt[:4]) or 1.0
        jb += struct.pack("<4B", idx[0], idx[1], idx[2], idx[3])
        wb += struct.pack("<4f", *(w / total for w in wgt[:4]))
    attrs["JOINTS_0"] = g.add_accessor(g.add_view(bytes(jb), ARRAY_BUFFER),
                                       UBYTE, nverts, "VEC4")
    attrs["WEIGHTS_0"] = g.add_accessor(g.add_view(bytes(wb), ARRAY_BUFFER),
                                        FLOAT, nverts, "VEC4")

    ind = bytearray()
    nind = 0
    for tris in md.triangles:
        for a, b, c in tris:
            ind += struct.pack("<3H", a, c, b)  # reverse winding (CCW front)
            nind += 3
    a_idx = g.add_accessor(g.add_view(bytes(ind), ELEMENT_ARRAY_BUFFER),
                           USHORT, nind, "SCALAR")

    # --- inverse bind matrices -------------------------------------------
    ibb = bytearray()
    for m in ibm:
        for c in range(4):
            for r in range(4):
                ibb += struct.pack("<f", m[r][c])
    a_ibm = g.add_accessor(g.add_view(bytes(ibb)), FLOAT, len(ibm), "MAT4")

    # --- texture ----------------------------------------------------------
    a_img = g.add_view(png_bytes)
    json_doc = {
        "asset": {"version": "2.0", "generator": "tools/export_character.py"},
        "scene": 0,
        "scenes": [{"nodes": []}],
        "nodes": [],
        "meshes": [],
        "materials": [],
        "textures": [],
        "images": [],
        "samplers": [],
        "skins": [],
        "accessors": g.accessors,
        "bufferViews": g.views,
        "buffers": [{"byteLength": len(g.blob)}],
    }

    # nodes: 0 = mesh node, 1..N = joints (index i+1 -> joint i)
    json_doc["nodes"].append({"name": MESH_NAME, "mesh": 0, "skin": 0})
    joint_nodes = []
    for i, name in enumerate(joints):
        t, q, s = node_locals[i]
        json_doc["nodes"].append({
            "name": name,
            "translation": [t[0], t[1], t[2]],
            "rotation": [q[0], q[1], q[2], q[3]],
            "scale": [s[0], s[1], s[2]],
        })
        joint_nodes.append(i + 1)
    for i in range(len(joints)):
        p = md.bone_parent[i]
        if p >= 0:
            json_doc["nodes"][p + 1].setdefault("children", []).append(i + 1)
    roots = [i + 1 for i in range(len(joints)) if md.bone_parent[i] < 0]
    json_doc["scenes"][0]["nodes"] = [0] + roots
    json_doc["skins"].append({
        "name": "GuyDangerous",
        "joints": joint_nodes,
        "inverseBindMatrices": a_ibm,
        "skeleton": roots[0] if roots else joint_nodes[0],
    })

    prim = {"attributes": attrs, "indices": a_idx, "material": 0, "mode": 4}
    json_doc["meshes"].append({"name": MESH_NAME, "primitives": [prim]})
    json_doc["samplers"].append({"magFilter": 9729, "minFilter": 9987,
                                 "wrapS": 10497, "wrapT": 10497})
    json_doc["images"].append({"name": material_name, "bufferView": a_img,
                               "mimeType": "image/png"})
    json_doc["textures"].append({"source": 0, "sampler": 0})
    json_doc["materials"].append({
        "name": material_name,
        "doubleSided": True,
        "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0},
            "metallicFactor": 0.0,
            "roughnessFactor": 0.85,
        },
    })

    js = json.dumps(json_doc, separators=(",", ":")).encode("utf-8")
    js += b" " * ((-len(js)) % 4)
    blob = bytes(g.blob) + b"\0" * ((-len(g.blob)) % 4)

    total = 12 + 8 + len(js) + 8 + len(blob)
    out = bytearray()
    out += b"glTF" + struct.pack("<II", 2, total)
    out += struct.pack("<I", len(js)) + b"JSON" + js
    out += struct.pack("<I", len(blob)) + b"BIN\0" + blob
    return bytes(out)


# --------------------------------------------------------------------------- #
# static OBJ / MTL fallback
# --------------------------------------------------------------------------- #
def write_obj(out_dir: Path, md: MeshData, material_name: str):
    lines = [
        "# Temple Run 2 runner 'Guy Dangerous' - static bind-pose mesh\n",
        "# Unity axes with X negated (right-handed); +Z = character forward,\n",
        "# feet at y=0, units = Unity metres.  See README.md.\n",
        "mtllib guy.mtl\n",
        f"o {MESH_NAME}\n",
    ]
    for p in md.positions:
        lines.append(f"v {fmt(-p[0])} {fmt(p[1])} {fmt(p[2])}\n")
    for uv in md.uvs:
        lines.append(f"vt {fmt(uv[0])} {fmt(uv[1])}\n")
    for n in md.normals:
        lines.append(f"vn {fmt(-n[0])} {fmt(n[1])} {fmt(n[2])}\n")
    has_uv = len(md.uvs) == len(md.positions)
    has_n = len(md.normals) == len(md.positions)
    lines.append(f"usemtl {material_name}\n")
    for tris in md.triangles:
        for a, b, c in tris:
            f = []
            for i in (a, c, b):  # reverse winding, CCW front
                if has_uv and has_n:
                    f.append(f"{i + 1}/{i + 1}/{i + 1}")
                elif has_uv:
                    f.append(f"{i + 1}/{i + 1}")
                else:
                    f.append(f"{i + 1}")
            lines.append("f " + " ".join(f) + "\n")
    (out_dir / "guy.obj").write_text("".join(lines), encoding="utf-8")

    mtl = [
        "# Temple Run 2 runner material (extracted _MainTex -> guy.png)\n",
        f"newmtl {material_name}\n",
        "Ka 0.000000 0.000000 0.000000\n",
        "Kd 1.000000 1.000000 1.000000\n",
        "Ks 0.000000 0.000000 0.000000\n",
        "d 1.000000\n",
        "illum 1\n",
        "map_Kd guy.png\n",
    ]
    (out_dir / "guy.mtl").write_text("".join(mtl), encoding="utf-8")
    return len(md.triangles[0]) if md.triangles else 0


# --------------------------------------------------------------------------- #
# clips
# --------------------------------------------------------------------------- #
def curve_time_span(clip):
    """(duration, max key count) from the decodable (position/scale) curves."""
    best = 0.0
    keys = 0
    for group in (clip.m_PositionCurves or [], clip.m_ScaleCurves or [],
                  clip.m_EulerCurves or []):
        for c in group:
            curve = getattr(c, "curve", None) or getattr(c, "m_Curve", None)
            kf = getattr(curve, "m_Curve", None) if curve is not None else None
            if not kf:
                continue
            keys = max(keys, len(kf))
            best = max(best, max(k.time for k in kf))
    return best, keys


def curve_path(c):
    """Name of the animated transform ('path' on Vector3Curve, 'm_Path' on the
    compressed rotation curve)."""
    return getattr(c, "m_Path", None) or getattr(c, "path", None) or ""


def collect_clips(guy_env_objs, shared_env_objs):
    clips = {}
    for src, env_objs in (("guy", guy_env_objs), ("shared", shared_env_objs)):
        for pid, (reader, obj) in env_objs.items():
            if reader.type.name != "AnimationClip" or obj is None:
                continue
            name = obj.m_Name
            duration, nkeys = curve_time_span(obj)
            paths = sorted({curve_path(c) for c in (obj.m_PositionCurves or [])} |
                           {curve_path(c) for c in (obj.m_CompressedRotationCurves or [])} |
                           {curve_path(c) for c in (obj.m_ScaleCurves or [])})
            clips[name] = {
                "source": name,
                "bundle": src,
                "path_id": pid,
                "sample_rate": float(obj.m_SampleRate or DEFAULT_FPS),
                "legacy": bool(obj.m_Legacy),
                "duration": round(duration, 6),
                "keys": nkeys,
                "position_curves": len(obj.m_PositionCurves or []),
                "rotation_curves": len(obj.m_CompressedRotationCurves or []),
                "scale_curves": len(obj.m_ScaleCurves or []),
                "bones": paths,
            }
    return clips


def build_animations_json(clips, bones, bounds, yaw_offset, scale, fps,
                          file_map):
    out_clips = {}
    for src in CLIP_ORDER:
        if src not in clips:
            continue
        friendly = CLIP_NAMES.get(src, src.lower())
        info = dict(clips[src])
        out_clips[friendly] = {
            "name": friendly,
            "clip": src,
            "fps": info["sample_rate"] or fps,
            "duration": info["duration"],
            "keys": info["keys"],
            "loop": friendly != "death",
            "baked": False,
            "rotation_encoding": "legacy-compressed-2020.3",
            "bones": len(info["bones"]),
        }
    doc = {
        "model": file_map["glb"],
        "obj": file_map["obj"],
        "png": file_map["png"],
        "yaw_offset_deg": yaw_offset,
        "scale": scale,
        "fps": fps,
        "bounds_min": [round(v, 6) for v in bounds[0]],
        "bounds_max": [round(v, 6) for v in bounds[1]],
        "up_axis": "Y",
        "forward_axis": "Z",
        "units": "unity-metres",
        "skeleton": {
            "joint_count": len(bones),
            "bind_pose": "exported in guy.glb / guy.obj",
            "bones": bones,
        },
        "clips": out_clips,
        "clip_details": clips,
        "notes": [
            "No clips are baked into the GLB: compressed rotation curves "
            "are not yet integrated into this exporter. Clips are catalogued "
            "here so the runtime can pick fps/duration for procedural motion.",
            "yaw_offset_deg is 0 because the exported mesh already faces +Z "
            "(measured from the bind-pose feet and thigh bone sides).",
            "Vertices are Unity axes with X negated; feet at y=0; metres.",
        ],
    }
    return doc


# --------------------------------------------------------------------------- #
# README
# --------------------------------------------------------------------------- #
README_TEMPLATE = """# Temple Run 2 runner assets ("Guy Dangerous")

Generated by `tools/export_character.py` (run it with
`recon/unity_assets/.venv/bin/python`).  Nothing here is hand-made: every value
comes from the shipped bundles listed under *Provenance*.

| file | what |
|---|---|
| `guy.glb` | skinned mesh + 25-bone skeleton in **bind pose** + albedo texture |
| `guy.obj` / `guy.mtl` | static bind-pose mesh + material, `map_Kd guy.png` |
| `guy.png` | 1024x1024 albedo (the material's `_MainTex`) |
| `animations.json` | clip + bone catalogue and the runtime overrides |

## Node / axis conventions

* **Frame**: Unity coordinates with **X negated** - the standard left-handed
  Unity -> right-handed glTF/raylib conversion (identical to UnityPy's OBJ
  writer and to `tools/export_props.py`).  `Y` is up, `+Z` is the character's
  forward, `+X` is the character's **left** after the flip.
* **Origin**: the prefab root `guy_dangerous`; the soles are at `y = 0`, so the
  model can be placed directly at a ground point.
* **Units**: Unity metres.  Bounds `%(bounds_min)s .. %(bounds_max)s`
  (height `%(height).4f`, width `%(width).4f`, depth `%(depth).4f`).
* **Facing**: measured, not assumed.  The bind-pose feet point at `+Z`
  (`Bone L Toe0` sits at `z = +0.0662` relative to `Bone L Foot` at `z = -0.0031`)
  and the rig's right thigh is at `+X` (`Bone R Thigh` = `+0.0701`), so forward
  is `+Z`.  The renderer's track frame wants `forward = (sin yaw, cos yaw)`,
  i.e. `+Z` at `yaw = 0`, so `animations.json` sets
  **`yaw_offset_deg = 0.0`** (this is the one value to flip to `180.0` if the
  track pieces turn out to be authored `-Z`-forward).
* **Winding**: triangles are reversed relative to the Unity mesh so front faces
  are counter-clockwise (CCW) once X is negated.  `guy.glb` also sets
  `doubleSided = true`.
* **UVs**: `guy.glb` writes glTF `TEXCOORD_0` bottom-up (`v' = 1 - v`, the glTF
  convention); the `guy.obj` fallback keeps Unity's `vt` values verbatim
  (matching `export_props.py`).
* **Node names**: `guy.glb` node 0 is the mesh node `%(mesh_name)s`; nodes
  `%(first_joint)d..%(last_joint)d` are the joints, named exactly like the Unity
  bones and ordered like the `SkinnedMeshRenderer.m_Bones` array (the same index
  space as the mesh's `JOINTS_0`).  The joint locals are derived from the
  mesh's own `m_BindPose` (`inverseBindMatrices` = the Unity bind poses), so the
  bind pose is reproduced exactly.

Bones (joint order): `%(bone_list)s`

## Animations

The legacy clips store bone rotations in `m_CompressedRotationCurves`.
AssetStudio implements `PackedQuatVector.UnpackQuats`, but this exporter does
not yet bake those rotations into `guy.glb`. The mesh loads in bind pose.

Every clip is still catalogued in `animations.json` (`clips` + `clip_details`):
name, source clip name, fps, duration, key count and the animated bone paths.
Position and scale curves *do* decode (they are plain keyframes) and were used
to derive each duration.

| state | clip | source | duration (s) | keys |
|---|---|---|---|---|
%(clip_table)s

## Provenance

* `%(guy_bundle)s`
  * `Mesh %(mesh_name)s` (path id `%(mesh_pid)s`) - 1351 verts / 1930 tris,
    25 bones, `SkinnedMeshRenderer %(smr_pid)s`
  * `Material %(material)s`, `Texture2D %(texture)s` (path id `%(tex_pid)s`,
    1024x1024)
  * clips `%(guy_clips)s`
* `%(shared_bundle)s`
  * clips `%(shared_clips)s`
"""


def write_readme(out_dir: Path, md: MeshData, bones, bounds, clips, prov):
    height = bounds[1][1] - bounds[0][1]
    width = bounds[1][0] - bounds[0][0]
    depth = bounds[1][2] - bounds[0][2]
    rows = []
    for src in CLIP_ORDER:
        if src not in clips:
            continue
        friendly = CLIP_NAMES.get(src, src.lower())
        rows.append("| `%s` | `%s` | %s | %.4f | %d |" % (
            friendly, src, clips[src]["bundle"], clips[src]["duration"],
            clips[src]["keys"]))
    text = README_TEMPLATE % {
        "bounds_min": [round(v, 4) for v in bounds[0]],
        "bounds_max": [round(v, 4) for v in bounds[1]],
        "height": height,
        "width": width,
        "depth": depth,
        "mesh_name": md.mesh_name,
        "first_joint": 1,
        "last_joint": len(bones),
        "bone_list": ", ".join(f"`{b}`" for b in bones),
        "clip_table": "\n".join(rows) if rows else "| - | - | - | - | - |",
        **prov,
    }
    (out_dir / "README.md").write_text(text, encoding="utf-8")


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bundles", type=Path, default=DEFAULT_BUNDLES)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args(argv)

    out_dir: Path = args.out
    out_dir.mkdir(parents=True, exist_ok=True)

    guy_env = load_env(args.bundles, GUY_BUNDLE)
    guy_objs = read_all(guy_env)
    try:
        shared_env = load_env(args.bundles, SHARED_BUNDLE)
        shared_objs = read_all(shared_env)
    except SystemExit:
        shared_objs = {}

    smr, mesh, md, bones_raw = extract_mesh(guy_objs)
    bone_names = [b[0] for b in bones_raw]
    if not md.bindposes:
        raise SystemExit("mesh has no bind poses")

    mat, tex, tex_pid = find_material_texture(guy_objs, smr)
    if tex is None:
        raise SystemExit("main texture missing")
    image = tex.image
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    png_bytes = buf.getvalue()
    (out_dir / "guy.png").write_bytes(png_bytes)

    # ---- bind-pose globals, mirrored into the export frame ---------------
    globals_mirrored = []
    worlds = []
    for i, bp in enumerate(md.bindposes):
        try:
            world = mat_invert(bp)
        except ValueError:
            raise SystemExit(f"bind pose {i} singular")
        worlds.append(world)
        globals_mirrored.append(mirror_x(world))

    node_locals = []
    for i, gm in enumerate(globals_mirrored):
        p = md.bone_parent[i]
        if p >= 0:
            local = mat_mul(mat_invert(globals_mirrored[p]), gm)
        else:
            local = gm
        node_locals.append(decompose(local))

    ibm = [mirror_x(bp) for bp in md.bindposes]

    glb = build_glb(md, png_bytes, material_name=mat.m_Name if mat else MATERIAL_NAME,
                    joints=bone_names, ibm=ibm, node_locals=node_locals)
    (out_dir / "guy.glb").write_bytes(glb)
    write_obj(out_dir, md, mat.m_Name if mat else MATERIAL_NAME)

    # ---- bounds in the export frame --------------------------------------
    xs = [-p[0] for p in md.positions]
    ys = [p[1] for p in md.positions]
    zs = [p[2] for p in md.positions]
    bounds = ((min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs)))

    clips = collect_clips(guy_objs, shared_objs)
    guy_clips = sorted(k for k, v in clips.items() if v["bundle"] == "guy")
    shared_clips = sorted(k for k, v in clips.items() if v["bundle"] == "shared")

    prov = {
        "guy_bundle": GUY_BUNDLE,
        "shared_bundle": SHARED_BUNDLE,
        "mesh_pid": str(-169845926871220793),
        "smr_pid": str(smr.m_GameObject.m_PathID),
        "material": mat.m_Name if mat else MATERIAL_NAME,
        "texture": tex.m_Name,
        "tex_pid": str(tex_pid),
        "guy_clips": ", ".join(f"`{c}`" for c in guy_clips) or "-",
        "shared_clips": ", ".join(f"`{c}`" for c in shared_clips) or "-",
    }

    file_map = {"glb": "guy.glb", "obj": "guy.obj", "png": "guy.png"}
    doc = build_animations_json(clips, bone_names, bounds,
                               yaw_offset=0.0, scale=1.0, fps=DEFAULT_FPS,
                               file_map=file_map)
    (out_dir / "animations.json").write_text(
        json.dumps(doc, indent=1) + "\n", encoding="utf-8")
    write_readme(out_dir, md, bone_names, bounds, clips, prov)

    tris = sum(len(t) for t in md.triangles)
    print(f"mesh {md.mesh_name}: {len(md.positions)} verts, {tris} tris, "
          f"{len(md.bindposes)} bind poses, {len(bone_names)} joints")
    expect = {"Bone Pelvis": (0.0, 0.6208, 0.0335),
              "Bone Head": (0.0, 1.1169, 0.0309),
              "Bone L Toe0": (-0.1037, 0.003, 0.0662)}
    for i, name in enumerate(bone_names):
        if name in expect:
            w = worlds[i]
            got = (w[0][3], w[1][3], w[2][3])
            err = max(abs(got[k] - expect[name][k]) for k in range(3))
            print(f"  bindpose {name}: world={tuple(round(v, 4) for v in got)}"
                  f" (prefab pose diff {err:.4f})")
    print(f"clips: {len(clips)} ({len(guy_clips)} guy + {len(shared_clips)} shared)")
    print(f"bounds (export frame): {bounds[0]} .. {bounds[1]}")
    print(f"wrote {out_dir/'guy.glb'} ({len(glb)} bytes), guy.obj, guy.mtl, "
          f"guy.png ({len(png_bytes)} bytes), animations.json, README.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
