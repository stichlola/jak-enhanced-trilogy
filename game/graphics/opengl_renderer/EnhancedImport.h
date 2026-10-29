#pragma once

#include <string>
#include <vector>

// Importing a user's model into the detailed scenery (EnhancedScenery), for the in-game editor, all
// in the game (no Blender): a .glb or .gltf is read, its meshes joined into one (the first material's
// texture for all), stood on the origin (the middle of its bounds, its lowest point at 0), kept at
// its size (meters) and, past max_faces faces, reduced to them. Written as the scenery's own models:
//   out_dir/NAME.glb        the model
//   out_dir/NAME_lod1.glb   its far version (a third of the faces)
//   out_dir/NAME_imp.png    its impostor: pictures from 8x8 directions over the upper hemisphere
//   out_dir/NAME_nrm.png    the impostor's normals
// (scripts/enhanced/scenery_import.py makes the same with Blender: OBJ and FBX still go there)
struct EnhancedImportResult {
  bool ok = false;
  std::string error;
  int faces_in = 0, faces = 0, lod_faces = 0, materials = 0;
  float size[3] = {0, 0, 0};  // m: x, height, z
};

EnhancedImportResult enhanced_import_model(const std::string& src,
                                           const std::string& out_dir,
                                           const std::string& name,
                                           int max_faces,
                                           float scale = 1.f);

