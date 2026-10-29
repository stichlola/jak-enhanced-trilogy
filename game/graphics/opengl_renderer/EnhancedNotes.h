#pragma once

#include "game/graphics/opengl_renderer/EnhancedPost.h"
#include "game/graphics/pipelines/opengl.h"

/*!
 * Jak Enhanced: notes drawn on the world, for whoever builds the mod. The in-game editor's Note tool
 * (EnhancedEditor: the game held still) the mouse or a drawing tablet's pen paints strokes on whatever is under
 * the cursor, with a few colored brushes; they stay put in the world as the camera moves, and are
 * saved (local/notes/notes.json, meters in the game's world) to be read back later: "the red is
 * where the dense grass goes".
 *   left button / pen: paint   1-6: brush color   [ ]: brush size   E: eraser
 *   Backspace: undo the last stroke   H: hide or show the notes
 */

// in the post pass, with the depth of this frame: reads the depth under the cursor (a frame late,
// no stall) and adds the painted point
void enhanced_notes_frame(int w,
                          int h,
                          GLuint depth_fbo,
                          const EnhancedCamera& cam,
                          const float proj[4],
                          bool active);

// in the ImGui pass: the input, the strokes over the picture, the brush panel
void enhanced_notes_ui(bool active);
