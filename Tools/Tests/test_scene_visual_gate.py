import copy
import json
import unittest
from pathlib import Path
from unittest.mock import patch

from PIL import Image, ImageDraw
from scene_visual_gate import EXPECTED_MESH, REQUIRED, validate_pixels, validate_report, validate_roi


class SceneVisualGateTests(unittest.TestCase):
    def setUp(self):
        self.size = (160, 120)
        self.roi = [50, 40, 80, 70]
        self.visible = Image.new("RGB", self.size, (20, 20, 20))
        ImageDraw.Draw(self.visible).rectangle((54, 44, 76, 66), fill=(130, 100, 80))
        self.hidden = Image.new("RGB", self.size, (20, 20, 20))
        self.scenes = [{"viewport": list(self.size), "hud_exclusions": [[0, 0, 45, 30]], "meshes": []}] * 3

    def test_reversible_object_contribution_passes(self):
        result = validate_pixels(self.visible, self.hidden, self.visible.copy(), self.roi, self.scenes)
        self.assertGreaterEqual(result["attributed_pixels"], 24)

    def test_black_hud_only_and_bright_sky_fail(self):
        for color in ((0, 0, 0), (120, 170, 230)):
            image = Image.new("RGB", self.size, color)
            ImageDraw.Draw(image).rectangle((0, 0, 40, 28), fill="white")
            with self.assertRaises(RuntimeError):
                validate_pixels(image, image.copy(), image.copy(), self.roi, self.scenes)

    def test_hud_changes_cannot_be_attributed(self):
        image = self.hidden.copy()
        ImageDraw.Draw(image).rectangle((0, 0, 40, 28), fill="white")
        with self.assertRaises(RuntimeError):
            validate_pixels(image, self.hidden, image.copy(), [0, 0, 40, 28], self.scenes)

    def test_screenwide_exposure_change_fails(self):
        bright = Image.new("RGB", self.size, (90, 90, 90))
        with self.assertRaises(RuntimeError):
            validate_pixels(bright, self.hidden, bright.copy(), self.roi, self.scenes)

    def test_unrestored_target_and_wrong_extent_fail(self):
        with self.assertRaises(RuntimeError):
            validate_pixels(self.visible, self.hidden, self.hidden.copy(), self.roi, self.scenes)
        with self.assertRaises(RuntimeError):
            validate_pixels(self.visible.resize((80, 60)), self.hidden, self.visible, self.roi, self.scenes)

    def shadow_fixture(self):
        ground = {"entity_id": "ground", "prefab": "official.environment.ground", "mesh": EXPECTED_MESH["official.environment.ground"],
                  "official_mesh": True, "registered": True, "visible": True, "up_vector": [0, 0, 1],
                  "bounds_min": [0, 0, 0], "bounds_max": [160, 120, 0]}
        light = {"component": "sun", "class": "DirectionalLightComponent", "intensity": 3, "registered": True, "visible": True}
        target = {"entity_id": "tower", "prefab": "official.tower.basic", "mesh": EXPECTED_MESH["official.tower.basic"],
                  "registered": True, "visible": True, "projected": True, "official_mesh": True,
                  "bounds_min": [50, 40, 0], "bounds_max": [70, 60, 20], "mesh_roi": [50, 40, 70, 60], "roi": [50, 40, 90, 60]}
        direction = [2 ** -0.5, 0, -2 ** -0.5]
        points = [[70 if i & 1 else 50, 60 if i & 2 else 40, 20 if i & 4 else 0] for i in range(8)]
        hits = [[p[0] + p[2], p[1], 0] for p in points]
        shadow = {"light_component": "sun", "light_class": "DirectionalLightComponent", "light_intensity": 3,
                  "light_registered": True, "light_visible": True, "light_cast_shadows": True, "mesh_cast_shadow": True,
                  "ray_direction": direction, "ground_entity_id": "ground", "ground_bounds_min": ground["bounds_min"],
                  "ground_bounds_max": ground["bounds_max"], "ground_z": 0, "max_ray_distance": 2000,
                  "world_points": hits, "screen_points": [p[:2] for p in hits], "roi": [50, 40, 90, 60]}
        target["shadows"] = [shadow]
        scene = {"viewport": list(self.size), "hud_exclusions": [], "lights": [light], "meshes": [ground, target]}
        return target, scene

    def test_dark_cast_shadow_with_lit_hidden_state_passes(self):
        target, scene = self.shadow_fixture()
        validate_roi(target, scene)
        hidden = Image.new("RGB", self.size, (100, 100, 100))
        visible = hidden.copy()
        ImageDraw.Draw(visible).rectangle((72, 43, 86, 57), fill=(0, 0, 0))
        metrics = validate_pixels(visible, hidden, visible.copy(), target["roi"], [scene] * 3, "tower")
        self.assertGreater(metrics["attributed_fraction"], 0.04)

    def test_other_shadow_and_enemy_swept_shadow_are_excluded(self):
        target, scene = self.shadow_fixture()
        hidden = Image.new("RGB", self.size, (100, 100, 100))
        visible = hidden.copy()
        ImageDraw.Draw(visible).rectangle((72, 43, 86, 57), fill=(0, 0, 0))
        for kind in ("official.tower.basic", "official.unit.basic_enemy"):
            other = copy.deepcopy(target)
            other["entity_id"] = "other"
            other["prefab"] = kind
            other["mesh"] = EXPECTED_MESH[kind]
            scenes = [copy.deepcopy(scene) for _ in range(3)]
            # The last capture contains the moving enemy's shadow envelope.
            scenes[-1]["meshes"].append(other)
            with self.subTest(kind=kind), self.assertRaises(RuntimeError):
                validate_pixels(visible, hidden, visible.copy(), target["roi"], scenes, "tower")

    def test_unrelated_shadow_outside_geometry_and_dark_global_change_fail(self):
        target, scene = self.shadow_fixture()
        hidden = Image.new("RGB", self.size, (100, 100, 100))
        visible = hidden.copy()
        ImageDraw.Draw(visible).rectangle((105, 43, 125, 57), fill=(0, 0, 0))
        with self.assertRaises(RuntimeError):
            validate_pixels(visible, hidden, visible.copy(), target["roi"], [scene] * 3, "tower")
        black = Image.new("RGB", self.size, (0, 0, 0))
        with self.assertRaises(RuntimeError):
            validate_pixels(black, hidden, black.copy(), target["roi"], [scene] * 3, "tower")

    def test_shadow_geometry_rejects_missing_light_horizontal_ray_and_padding(self):
        target, scene = self.shadow_fixture()
        for mutation in ("light", "horizontal", "padding", "ground", "points", "distance", "tilted"):
            mesh, state = copy.deepcopy(target), copy.deepcopy(scene)
            shadow = mesh["shadows"][0]
            if mutation == "light":
                state["lights"][0]["visible"] = False
            elif mutation == "horizontal":
                shadow["ray_direction"] = [1, 0, 0]
            elif mutation == "padding":
                mesh["roi"][2] = 150
            elif mutation == "ground":
                shadow["ground_z"] = 50
            elif mutation == "points":
                shadow["world_points"][0][0] += 30
            elif mutation == "tilted":
                state["meshes"][0]["up_vector"] = [0, 1, 0]
            else:
                shadow["max_ray_distance"] = 100000
            with self.subTest(mutation=mutation), self.assertRaises(RuntimeError):
                validate_roi(mesh, state)

    def fixture(self):
        camera = {"local_controller": True, "manager_valid": True,
                  "view_target": "/Game/Maps/Phase0.Phase0:PersistentLevel.CameraActor_2147482366",
                  "location": [750, 0, 1800], "rotation": [-90, 0, 0], "fov": 90}
        scene = {"viewport": list(self.size), "camera": camera, "lights": [], "meshes": [],
                 "hud_exclusions": [[0, 0, 45, 30]], "frame": 1}
        samples = []
        for index, kind in enumerate(sorted(REQUIRED)):
            target = {"entity_id": f"entity_{index}", "prefab": kind, "mesh": EXPECTED_MESH[kind],
                      "registered": True, "visible": True, "official_mesh": True, "projected": True,
                      "component_replicated": False, "roi": self.roi,
                      "location": [0, 0, 0], "bounds_min": [-1, -1, -1], "bounds_max": [1, 1, 1]}
            sample = {"target": target}
            for phase, frame in (("visible", 2), ("hidden", 6), ("restored", 10)):
                state = copy.deepcopy(scene)
                state["frame"] = frame
                mesh = copy.deepcopy(target)
                mesh["visible"] = phase != "hidden"
                state["meshes"] = [mesh]
                sample[phase + "_state"] = state
                sample[phase] = phase + ".bmp"
            samples.append(sample)
        return {"schema": 1, "method": "native_viewport_visible_hidden_restored", "capture_complete": True,
                "visibility_restored": True, "initial": scene, "final": scene, "samples": samples}

    def run_report(self, report):
        def image(path):
            return (self.hidden if Path(path).name == "hidden.bmp" else self.visible).copy()
        with patch.object(Path, "read_text", return_value=json.dumps(report)), patch("scene_visual_gate.Image.open", side_effect=image):
            return validate_report(Path(__file__).parent / "synthetic/report.json", require_enemy=False)

    def test_full_report_and_fail_closed_contracts(self):
        report = self.fixture()
        self.assertTrue(self.run_report(report)["scene_visual_validated"])
        for mutation in ("camera", "mesh", "frame", "restore", "missing", "transition",
                         "default_pawn", "wrong_position", "wrong_rotation", "wrong_fov"):
            value = copy.deepcopy(report)
            if mutation == "camera":
                value["samples"][0]["hidden_state"]["camera"]["location"][0] += 10
            elif mutation == "mesh":
                value["samples"][0]["target"]["mesh"] = "/Game/NotOfficial.NotOfficial"
            elif mutation == "frame":
                value["samples"][0]["hidden_state"]["frame"] = 2
            elif mutation == "restore":
                value["visibility_restored"] = False
            elif mutation == "transition":
                value["samples"][0]["hidden_state"]["meshes"][0]["mesh"] = "/Game/NotOfficial.NotOfficial"
            elif mutation == "default_pawn":
                value["initial"]["camera"]["view_target"] = "/Game/Maps/Phase0.Phase0:PersistentLevel.DefaultPawn_0"
            elif mutation == "wrong_position":
                value["final"]["camera"]["location"] = [0, 0, 100]
            elif mutation == "wrong_rotation":
                value["final"]["camera"]["rotation"] = [-89.8, 0, 0]
            elif mutation == "wrong_fov":
                value["final"]["camera"]["fov"] = 90.02
            else:
                value["samples"].pop()
            with self.subTest(mutation=mutation), self.assertRaises(RuntimeError):
                self.run_report(value)


if __name__ == "__main__":
    unittest.main()
