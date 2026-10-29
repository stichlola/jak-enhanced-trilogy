#pragma once

#include "game/graphics/opengl_renderer/EnhancedPost.h"
#include "game/graphics/pipelines/opengl.h"

/*!
 * Jak Enhanced: the in-game editor (F4, the Enhanced menu). Photo mode (the game held still) with the
 * mouse or a drawing tablet's pen working as in Blender, or the pad, changes seen at once and kept
 * with Save (Ctrl+S; leaving with changes asks). A simple UI (big buttons, the essentials) or an
 * advanced one (numbers, the scene's list, every option):
 *  - the view: middle button (the pen's lower button) turns around the point under the cursor,
 *    with Shift pans, with Ctrl zooms; the wheel zooms; the pad's free camera still works;
 *  - tools (1-5): select and move what was placed, paint the tall grass, place models (grass tufts,
 *    stones: by kind, searchable, one at a click or strewn with the brush), lights, notes;
 *  - Blender's keys: G move, R turn, S scale (left button or Enter keeps, right button or Esc
 *    drops), X / Delete delete, Shift+D duplicate, A all, Alt+A none, Shift+A add, Ctrl+Z undo,
 *    Ctrl+Shift+Z redo, F or [ ] brush size, Ctrl inverts a brush (erases), . frames the selection,
 *    N the side panel, T the tools, Ctrl+S save, End lets the selection fall;
 *  - gravity: placed with it on, things fall from above onto the game's own collision (probed on
 *    the game's thread), with the game's gravity, and stop dead where they land;
 *  - the pad: the free camera's sticks (L1 / R1 down and up, R2 slower), a reticle in the middle;
 *    Cross the tool (held: paint, strew), Square erase / delete, Circle deselect / cancel, Triangle
 *    the next tool, the D-pad the model / the brush's size / turning and scaling the selection, L2
 *    held carries the selection, L3 gravity / let fall, Select undo, R3 redo, Start leave.
 * The placed objects are in game/assets/enhanced/placed.json, the lights in lights.json, the
 * painted grass in grass_paint/.
 */

// in the post pass, with this frame's depth: reads the depth under the cursor and at the middle of
// the view (a frame late, no stall)
void enhanced_editor_frame(int w, int h, GLuint depth_fbo, const EnhancedCamera& cam, const float proj[4]);

// the levels loaded this frame (the Textures tool looks through them)
struct LevelData;
void enhanced_editor_levels(const std::vector<LevelData*>& levels);

// in the ImGui pass, every frame (loads the placed objects the first time): the editor's input,
// its marks over the picture, its panels
void enhanced_editor_ui();
