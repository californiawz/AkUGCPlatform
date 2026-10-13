"""Validate native viewport object attribution; never accept HUD/sky brightness alone."""
import argparse
import json
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw


REQUIRED = {"official.environment.ground", "official.gameplay.base", "official.tower.basic"}
EXPECTED_MESH = {"official.environment.ground": "/Engine/BasicShapes/Plane.Plane",
                 "official.gameplay.base": "/Engine/BasicShapes/Cube.Cube",
                 "official.tower.basic": "/Engine/BasicShapes/Cylinder.Cylinder",
                 "official.unit.basic_enemy": "/Engine/BasicShapes/Sphere.Sphere"}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def validate_camera(scene):
    """Current fixed Phase0 solo/network/room view, not custom-work camera support.

    Validate final runtime GetViewTarget/PlayerCameraManager evidence only.
    """
    camera = scene.get("camera", {})
    require(camera.get("manager_valid") and camera.get("local_controller") and camera.get("view_target"),
            "Missing actual local PlayerCameraManager/GetViewTarget evidence")
    for field in ("location", "rotation"):
        require(len(camera.get(field, [])) == 3 and all(math.isfinite(v) for v in camera[field]),
                f"Invalid final camera {field}")
    require(1 < camera.get("fov", 0) < 179, "Invalid final camera FOV")
    actor_name = camera["view_target"].rsplit(".", 1)[-1]
    require(actor_name == "CameraActor" or actor_name.startswith("CameraActor_"),
            "Fixed Phase0 view target must be the runtime CameraActor, not DefaultPawn")
    for field, expected in (("location", (750, 0, 1800)), ("rotation", (-90, 0, 0))):
        require(all(math.isclose(value, target, rel_tol=0, abs_tol=0.1) for value, target in zip(camera[field], expected)),
                f"Fixed Phase0 final camera {field} differs from {expected}")
    require(math.isclose(camera["fov"], 90, rel_tol=0, abs_tol=0.01), "Fixed Phase0 final camera FOV must be 90")
    require(len(scene.get("viewport", [])) == 2 and min(scene["viewport"]) > 0, "Invalid viewport extent")
    return camera


def validate_mesh(mesh):
    kind = mesh.get("prefab")
    require(kind in EXPECTED_MESH and mesh.get("mesh") == EXPECTED_MESH[kind], "Not the official mesh")
    require(all(mesh.get(key) for key in ("registered", "visible", "projected", "official_mesh")),
            f"Official target not registered/visible/projected: {kind}")
    require(mesh.get("entity_id") and len(mesh.get("roi", [])) == 4, "Missing entity identity/projected bounds")
    require(all(math.isfinite(v) for v in mesh["roi"]), "Nonfinite target ROI")


def validate_roi(mesh, scene):
    """An ROI extension must be the finite light-ray projection onto official ground."""
    roi = mesh.get("roi", [])
    body = mesh.get("mesh_roi", roi)
    width, height = scene["viewport"]
    def rectangle(value):
        require(len(value) == 4 and all(math.isfinite(v) for v in value), "Invalid projected rectangle")
        require(0 <= value[0] <= value[2] <= width and 0 <= value[1] <= value[3] <= height,
                "Projected rectangle escapes viewport")
    rectangle(roi)
    rectangle(body)
    union = list(body)
    shadows = mesh.get("shadows", [])
    require(mesh.get("prefab") != "official.environment.ground" or not shadows, "Ground ROI cannot be expanded by shadows")
    for shadow in shadows:
        require(shadow.get("light_registered") and shadow.get("light_visible")
                and shadow.get("light_cast_shadows") and shadow.get("mesh_cast_shadow")
                and shadow.get("light_class") == "DirectionalLightComponent" and shadow.get("light_intensity", 0) > 0,
                "Shadow lacks an active directional light/casting mesh")
        lights = [light for light in scene.get("lights", []) if light.get("component") == shadow.get("light_component")]
        require(len(lights) == 1 and lights[0].get("registered") and lights[0].get("visible")
                and lights[0].get("class") == "DirectionalLightComponent"
                and abs(lights[0].get("intensity", 0) - shadow["light_intensity"]) < 0.001,
                "Shadow light does not match captured scene")
        direction = shadow.get("ray_direction", [])
        require(len(direction) == 3 and all(math.isfinite(v) for v in direction)
                and abs(sum(v * v for v in direction) - 1) < 0.001 and direction[2] <= -0.25,
                "Invalid/near-horizontal shadow ray")
        grounds = [ground for ground in scene.get("meshes", []) if ground.get("entity_id") == shadow.get("ground_entity_id")]
        require(len(grounds) == 1 and grounds[0].get("prefab") == "official.environment.ground"
                and grounds[0].get("mesh") == EXPECTED_MESH["official.environment.ground"]
                and grounds[0].get("official_mesh") and grounds[0].get("registered") and grounds[0].get("visible"),
                "Shadow receiver is not visible registered official ground")
        ground = grounds[0]
        up = ground.get("up_vector", [])
        require(len(up) == 3 and all(math.isfinite(v) for v in up) and up[2] >= 0.999,
                "Shadow receiver is not an upward horizontal plane")
        lo, hi = shadow["ground_bounds_min"], shadow["ground_bounds_max"]
        require(len(lo) == len(hi) == 3 and all(math.isfinite(v) for v in lo + hi)
                and 0 <= hi[2] - lo[2] <= 1 and all(a <= b for a, b in zip(lo, hi)), "Invalid horizontal ground bounds")
        for field in ("bounds_min", "bounds_max"):
            recorded = shadow["ground_" + field]
            require(max(abs(a - b) for a, b in zip(recorded, ground[field])) <= 0.01, "Ground bounds changed")
        z = shadow["ground_z"]
        require(math.isfinite(z) and abs(z - (lo[2] + hi[2]) / 2) <= 0.01, "Incorrect ground plane")
        lower, upper = mesh["bounds_min"], mesh["bounds_max"]
        require(len(lower) == len(upper) == 3 and lower[2] >= z - 0.01 and upper[2] > z, "Mesh is below shadow receiver")
        world_points, screen_points = shadow["world_points"], shadow["screen_points"]
        require(len(world_points) == len(screen_points) == 8 and shadow.get("max_ray_distance") == 2000,
                "Incomplete or unbounded shadow projection")
        for corner, hit in enumerate(world_points):
            point = [upper[axis] if corner & (1 << axis) else lower[axis] for axis in range(3)]
            distance = (z - point[2]) / direction[2]
            require(len(hit) == 3 and all(math.isfinite(v) for v in hit) and -0.01 <= distance <= 2000,
                    "Invalid ray-ground intersection")
            require(max(abs(hit[axis] - point[axis] - direction[axis] * distance) for axis in range(3)) <= 0.01
                    and lo[0] <= hit[0] <= hi[0] and lo[1] <= hit[1] <= hi[1], "Shadow ray misses finite ground")
        require(all(len(p) == 2 and all(math.isfinite(v) for v in p) for p in screen_points), "Invalid projected shadow vertices")
        shadow_roi = [max(0, min(p[0] for p in screen_points)), max(0, min(p[1] for p in screen_points)),
                      min(width, max(p[0] for p in screen_points)), min(height, max(p[1] for p in screen_points))]
        rectangle(shadow_roi)
        require(max(abs(a - b) for a, b in zip(shadow_roi, shadow["roi"])) <= 0.01, "Shadow ROI is not its projected bounds")
        union = [min(union[0], shadow_roi[0]), min(union[1], shadow_roi[1]),
                 max(union[2], shadow_roi[2]), max(union[3], shadow_roi[3])]
    require(max(abs(a - b) for a, b in zip(union, roi)) <= 0.01, "ROI contains arbitrary padding outside mesh/shadow bounds")


def channel_max(image):
    r, g, b = image.convert("RGB").split()
    return ImageChops.lighter(ImageChops.lighter(r, g), b)


def threshold(image, limit):
    return image.point(lambda value: 255 if value >= limit else 0)


def pixel_count(mask):
    return mask.histogram()[255]


def eligible_mask(size, roi, scenes, target_id=None):
    mask = Image.new("L", size, 0)
    draw = ImageDraw.Draw(mask)
    x0, y0, x1, y1 = roi
    box = (max(0, math.ceil(x0) + 2), max(0, math.ceil(y0) + 2),
           min(size[0] - 1, math.floor(x1) - 2), min(size[1] - 1, math.floor(y1) - 2))
    if box[2] > box[0] and box[3] > box[1]:
        draw.rectangle(box, fill=255)
    for scene in scenes:
        for rectangle in scene.get("hud_exclusions", []):
            draw.rectangle(tuple(rectangle), fill=0)
    # Exclude other bodies AND their bounded cast shadows. For moving enemies,
    # use the union over all capture states to cover motion between screenshots.
    others = {}
    for scene in scenes:
        for mesh in scene.get("meshes", []):
            if not mesh.get("projected") or not mesh.get("mesh") or mesh.get("prefab") == "official.environment.ground":
                continue
            if target_id is not None and mesh.get("entity_id") == target_id:
                continue
            if target_id is None and mesh.get("prefab") != "official.unit.basic_enemy":
                continue
            if mesh.get("shadows"):
                validate_roi(mesh, scene)
            a, b, c, d = mesh["roi"]
            key = mesh.get("entity_id", "enemy")
            old = others.get(key, [a, b, c, d])
            others[key] = [min(a, old[0]), min(b, old[1]), max(c, old[2]), max(d, old[3])]
    for a, b, c, d in others.values():
        draw.rectangle((a - 6, b - 6, c + 6, d + 6), fill=0)
    return mask


def validate_pixels(visible, hidden, restored, roi, scenes, target_id=None):
    require(visible.size == hidden.size == restored.size == tuple(scenes[0]["viewport"]),
            "Screenshot/viewport dimensions disagree; refusing desktop/DPI coordinate guessing")
    mask = eligible_mask(visible.size, roi, scenes, target_id)
    eligible = pixel_count(mask)
    require(eligible >= 32, "Target has too few HUD-free projected pixels")
    a = channel_max(ImageChops.difference(visible, hidden))
    b = channel_max(ImageChops.difference(restored, hidden))
    recovery = channel_max(ImageChops.difference(visible, restored))
    # A reversible contribution may be a bright body OR its dark cast shadow.
    # At least one state is lit; both hidden deltas and restoration remain strict.
    changed = ImageChops.multiply(threshold(a, 18), threshold(b, 18))
    lit = ImageChops.lighter(ImageChops.lighter(channel_max(visible), channel_max(hidden)), channel_max(restored))
    changed = ImageChops.multiply(changed, threshold(lit, 24))
    changed = ImageChops.multiply(changed, recovery.point(lambda v: 255 if v <= 8 else 0))
    changed = ImageChops.multiply(changed, mask)
    # Reject screen-wide exposure/sky variation: target change must exceed median
    # change outside its projected bounds (HUD and moving silhouettes excluded).
    outside = eligible_mask(visible.size, (0, 0, *visible.size), scenes, target_id)
    ImageDraw.Draw(outside).rectangle(tuple(roi), fill=0)
    outside_count = pixel_count(outside)
    require(outside_count >= 32, "No outside-ROI control pixels")
    histogram = ImageChops.multiply(a, outside).histogram()
    histogram[0] -= visible.width * visible.height - outside_count
    total = 0
    median = 0
    for value, frequency in enumerate(histogram):
        total += frequency
        if total >= outside_count / 2:
            median = value
            break
    changed = ImageChops.multiply(changed, threshold(a, median + 18))
    count = pixel_count(changed)
    require(count >= 24 and count / eligible >= 0.04,
            f"No reversible official-object ROI contribution: {count}/{eligible}, outside median={median}")
    return {"eligible_pixels": eligible, "attributed_pixels": count,
            "attributed_fraction": round(count / eligible, 5), "outside_delta_median": median,
            "pixel_delta_threshold": 18, "restore_tolerance": 8}


def validate_report(path, require_enemy=True):
    path = Path(path).resolve()
    report = json.loads(path.read_text(encoding="utf-8-sig"))
    require(report.get("schema") == 1 and report.get("method") == "native_viewport_visible_hidden_restored",
            "Unsupported visual evidence report")
    require(report.get("capture_complete") and report.get("visibility_restored") and not report.get("error"),
            "Incomplete/unrestored scene capture")
    validate_camera(report["initial"])
    validate_camera(report["final"])
    results = []
    kinds = set()
    for sample in report.get("samples", []):
        target = sample["target"]
        validate_mesh(target)
        kind = target["prefab"]
        require(kind in REQUIRED and not target.get("component_replicated"), "Only local static VisualMesh may be toggled")
        scenes = [sample[f"{phase}_state"] for phase in ("visible", "hidden", "restored")]
        cameras = [validate_camera(scene) for scene in scenes]
        require(scenes[0]["frame"] < scenes[1]["frame"] < scenes[2]["frame"], "Captures did not cross real frames")
        require(all(scene["viewport"] == scenes[0]["viewport"] for scene in scenes), "Viewport resized during attribution")
        for camera in cameras[1:]:
            require(camera["view_target"] == cameras[0]["view_target"] and abs(camera["fov"] - cameras[0]["fov"]) <= 0.01,
                    "View target/FOV changed during attribution")
            for field in ("location", "rotation"):
                require(max(abs(a - b) for a, b in zip(camera[field], cameras[0][field])) <= 0.1,
                        "Final camera moved during attribution")
        for index, scene in enumerate(scenes):
            matching = [mesh for mesh in scene["meshes"] if mesh["entity_id"] == target["entity_id"]]
            require(len(matching) == 1, "Target identity disappeared/duplicated")
            mesh = matching[0]
            require(mesh["registered"] and mesh["official_mesh"] and mesh["projected"]
                    and mesh["mesh"] == target["mesh"] and mesh["prefab"] == kind
                    and not mesh.get("component_replicated"), "Target invalid during capture")
            require(mesh["visible"] == (index != 1), "Visibility transition not evidenced")
            validate_roi(mesh, scene)
            validate_roi(target, scene)
            require(mesh.get("shadows", []) == target.get("shadows", [])
                    and max(abs(a - b) for a, b in zip(mesh["roi"], target["roi"])) <= 0.01,
                    "Shadow projection/light changed during capture")
            for field in ("bounds_min", "bounds_max", "location"):
                require(max(abs(a - b) for a, b in zip(mesh[field], target[field])) < 0.01, "Target moved during capture")
        images = []
        for phase in ("visible", "hidden", "restored"):
            filename = path.parent / sample[phase]
            require(filename.resolve().parent == path.parent, "Capture escapes evidence directory")
            with Image.open(filename) as image:
                images.append(image.convert("RGB"))
        metrics = validate_pixels(*images, target["roi"], scenes, target["entity_id"])
        results.append({"entity_id": target["entity_id"], "prefab": kind, "roi": target["roi"],
                        "mesh_roi": target.get("mesh_roi", target["roi"]), "shadows": target.get("shadows", []), **metrics})
        kinds.add(kind)
    require(kinds == REQUIRED, "Ground/base/tower did not all pass attributed pixels")
    enemy = None
    if require_enemy:
        enemy_path = path.parent / "enemy.json"
        enemy = json.loads(enemy_path.read_text(encoding="utf-8-sig"))
        validate_camera(enemy)
        enemies = [m for m in enemy["meshes"] if m.get("prefab") == "official.unit.basic_enemy"]
        require(enemies, "No official enemy observation")
        for mesh in enemies:
            validate_mesh(mesh)
    return {"scene_visual_validated": True, "report": str(path), "targets": results,
            "final_camera": report["final"]["camera"], "lights": report["final"]["lights"],
            "enemy_projected_mesh_evidence": enemy is not None,
            "scope": "Static ground/body and physically bounded cast-shadow reversible pixels; other bodies/enemy shadows excluded; enemy projection/registration only"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    parser.add_argument("--no-enemy", action="store_true", help="Room checkpoints record enemy data separately")
    options = parser.parse_args()
    print(json.dumps(validate_report(options.report, not options.no_enemy), indent=2))


if __name__ == "__main__":
    main()
