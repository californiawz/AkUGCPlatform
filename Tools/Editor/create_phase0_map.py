import unreal

ASSET_PATH = "/Game/Maps/Phase0"

level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if unreal.EditorAssetLibrary.does_asset_exist(ASSET_PATH):
    unreal.log(f"Phase 0 map already exists: {ASSET_PATH}")
elif not level_editor.new_level(ASSET_PATH, False):
    raise RuntimeError(f"Failed to create Phase 0 map: {ASSET_PATH}")
else:
    unreal.log(f"Created Phase 0 map: {ASSET_PATH}")
