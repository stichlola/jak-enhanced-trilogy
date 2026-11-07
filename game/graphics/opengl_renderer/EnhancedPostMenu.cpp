// The Enhanced menu: the start menu, the photo hint, the in-game menu, its settings file and
// presets (split from EnhancedPost.cpp; shared helpers in EnhancedPostInternal.h)

#include "game/graphics/opengl_renderer/EnhancedPostInternal.h"

using namespace enh_post;

void EnhancedPost::draw_start_menu() {
  if (!g_enhanced_menu.start_menu) {
    return;
  }
  static const char* const kTitles[3] = {"Jak and Daxter", "Jak II", "Jak 3"};
  const int current = std::clamp((int)g_game_version - 1, 0, 2);
  if (!m_start_loaded) {
    // the portraits (the old game wheel's), the lettering (start_menu_text.py) and which games are
    // set up (their disc extracted)
    m_start_loaded = true;
    m_start_opened_at = ImGui::GetTime();
    const auto proj = file_util::get_jak_project_dir();
    auto load = [&](const std::string& name, GLuint* tex, float* w_out, float* h_out) {
      const auto path = proj / "game/assets/enhanced/wheel" / name;
      int w = 0, h = 0, n = 0;
      unsigned char* px = stbi_load(path.string().c_str(), &w, &h, &n, 4);
      if (!px) {
        lg::warn("[enhanced] start menu: no {}", path.string());
        return;
      }
      glGenTextures(1, tex);
      glBindTexture(GL_TEXTURE_2D, *tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glGenerateMipmap(GL_TEXTURE_2D);
      glBindTexture(GL_TEXTURE_2D, 0);
      stbi_image_free(px);
      *w_out = (float)w;
      *h_out = (float)h;
    };
    float unused_w, unused_h;
    for (int i = 0; i < 3; i++) {
      m_start_ready[i] =
          i == current || fs::exists(proj / "out" / fmt::format("jak{}", i + 1) / "iso");
      load(fmt::format("menu{}.jpg", i + 1), &m_start_tex[i], &unused_w, &unused_h);
    }
    for (int i = 0; i < 4; i++) {
      load(i == 0 ? std::string("title.png") : fmt::format("name{}.png", i), &m_start_text[i],
           &m_start_text_w[i], &m_start_text_h[i]);
    }
    m_start_pick = current;
  }

  // what the player picked: the game running now just closes the menu, another one (or the one
  // running, from its newest save) boots in this same process
  auto choose = [&](int game, bool from_save) {
    if (!m_start_ready[game]) {
      return;
    }
    g_enhanced_menu.start_menu = false;
    if (game == current && !from_save) {
      return;
    }
    if (MasterExit == RuntimeExitStatus::RUNNING) {
      g_switch_game_to = (GameVersion)(game + 1);
      g_enhanced_switch_restore = from_save;
      MasterExit = RuntimeExitStatus::SWITCH_GAME;
    }
  };
  auto pressed = [](std::initializer_list<ImGuiKey> keys) {
    for (auto k : keys) {
      if (ImGui::IsKeyPressed(k, true)) {
        return true;
      }
    }
    return false;
  };
  int dir = 0;
  if (pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft,
               ImGuiKey_GamepadLStickLeft})) {
    dir = -1;
  } else if (pressed({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight,
                      ImGuiKey_GamepadLStickRight})) {
    dir = 1;
  }
  for (int pick = m_start_pick + dir; dir && pick >= 0 && pick <= 2; pick += dir) {
    // past the games not set up
    if (m_start_ready[pick]) {
      m_start_pick = pick;
      break;
    }
  }

  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float fade = std::min(1.f, (float)(ImGui::GetTime() - m_start_opened_at) * 2.5f);
  auto col = [&](float r, float g, float b, float a) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a * fade));
  };
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(size);
  ImGui::SetNextWindowBgAlpha(0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
  ImGui::Begin("##start-menu", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  const float unit = size.y / 1080.f;
  auto text_centered = [&](float px, float x, float y, ImU32 c, const char* text) {
    const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.f, text);
    dl->AddText(font, px, ImVec2(x - ts.x * 0.5f, y), c, text);
  };
  // opaque (the game waits to boot: nothing is drawn behind), deep blue at the top to black
  const ImU32 top = ImGui::ColorConvertFloat4ToU32(ImVec4(0.03f, 0.05f, 0.10f, 1.f));
  const ImU32 bottom = ImGui::ColorConvertFloat4ToU32(ImVec4(0.f, 0.f, 0.f, 1.f));
  dl->AddRectFilledMultiColor(ImVec2(0, 0), size, top, top, bottom, bottom);
  // the lettering pictures at a given height, centered on x (text when a picture is missing)
  auto lettering = [&](int which, float height, float x, float y, float shade, const char* text) {
    if (!m_start_text[which]) {
      text_centered(height * 0.8f, x, y, col(0.96f * shade, 0.62f * shade, 0.2f * shade, 1.f), text);
      return;
    }
    const float w = height * m_start_text_w[which] / m_start_text_h[which];
    dl->AddImage((ImTextureID)(intptr_t)m_start_text[which], ImVec2(x - w * 0.5f, y),
                 ImVec2(x + w * 0.5f, y + height), ImVec2(0, 0), ImVec2(1, 1),
                 col(shade, shade, shade, 1.f));
  };
  lettering(0, 96.f * unit, size.x * 0.5f, size.y * 0.07f, 1.f, "Jak Enhanced Trilogy");
  text_centered(26.f * unit, size.x * 0.5f, size.y * 0.07f + 112.f * unit,
                col(0.75f, 0.78f, 0.85f, 1.f), "Choose a game");

  // the three cards, the one picked larger, lit and framed in gold
  const float gap = size.x * 0.035f;
  const float card = std::min(size.y * 0.42f, (size.x * 0.9f - 2.f * gap) / 3.f);
  const float x0 = size.x * 0.5f - (3.f * card + 2.f * gap) * 0.5f;
  const float y0 = size.y * 0.27f;
  const ImGuiIO& io = ImGui::GetIO();
  int clicked = -1;
  for (int i = 0; i < 3; i++) {
    const bool picked = i == m_start_pick;
    const float grow = picked ? card * 0.05f : 0.f;
    const ImVec2 a(x0 + i * (card + gap) - grow, y0 - grow);
    const ImVec2 b(a.x + card + 2.f * grow, a.y + card + 2.f * grow);
    // the mouse picks too, a click plays (not in the first moment: the cursor appearing where it
    // was reads as a move)
    if (m_start_ready[i] && ImGui::GetTime() - m_start_opened_at > 0.5 && io.MousePos.x >= a.x &&
        io.MousePos.x < b.x &&
        io.MousePos.y >= a.y && io.MousePos.y < b.y) {
      if (io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f) {
        m_start_pick = i;
      }
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        clicked = i;
      }
    }
    const float lit = !m_start_ready[i] ? 0.22f : picked ? 1.f : 0.5f;
    if (picked) {
      dl->AddRectFilled(ImVec2(a.x - 14.f * unit, a.y - 14.f * unit),
                        ImVec2(b.x + 14.f * unit, b.y + 14.f * unit),
                        col(0.96f, 0.72f, 0.2f, 0.18f), 18.f * unit);
    }
    if (m_start_tex[i]) {
      dl->AddImageRounded((ImTextureID)(intptr_t)m_start_tex[i], a, b, ImVec2(0, 0), ImVec2(1, 1),
                          col(lit, lit, lit, 1.f), 10.f * unit);
    } else {
      dl->AddRectFilled(a, b, col(0.15f * lit, 0.18f * lit, 0.25f * lit, 1.f), 10.f * unit);
    }
    dl->AddRect(a, b, picked ? col(0.98f, 0.8f, 0.32f, 1.f) : col(0.45f, 0.48f, 0.55f, 0.6f),
                10.f * unit, 0, picked ? 4.f * unit : 1.5f * unit);
    const float cx = (a.x + b.x) * 0.5f;
    const float shade = picked ? 1.f : 0.62f;
    lettering(i + 1, 54.f * unit, cx, b.y + 22.f * unit, shade, kTitles[i]);
    if (!m_start_ready[i]) {
      text_centered(24.f * unit, cx, (a.y + b.y) * 0.5f - 12.f * unit,
                    col(0.9f, 0.9f, 0.9f, 0.9f), "Not set up: its disc goes in the ISO folder");
    }
  }
  text_centered(24.f * unit, size.x * 0.5f, size.y - 70.f * unit, col(0.7f, 0.73f, 0.8f, 1.f),
                "Left / Right: choose      Enter or A: play      C or Y: continue from the last "
                "save      Esc or B: close");
  ImGui::End();

  if (clicked >= 0) {
    choose(clicked, false);
  } else if (pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space,
                      ImGuiKey_GamepadFaceDown})) {
    choose(m_start_pick, false);
  } else if (pressed({ImGuiKey_C, ImGuiKey_GamepadFaceUp})) {
    choose(m_start_pick, true);
  } else if (pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight})) {
    g_enhanced_menu.start_menu = false;
  }
}

void EnhancedPost::draw_photo_hint() {
  if (!g_enhanced_menu.photo_active || g_enhanced_menu.editor) {
    return;
  }
  const double now =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const float age = (float)(now - g_enhanced_menu.photo_entered_at.load());
  if (age > 6.f) {
    return;
  }
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y - 40.f), ImGuiCond_Always, ImVec2(0.5f, 1.f));
  ImGui::SetNextWindowBgAlpha(0.6f * std::min(1.f, (6.f - age) * 2.f));
  ImGui::Begin("##photo-hint", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoNav);
  // (the menu's and photo mode's keys as bound in the menu's Controls)
  const std::string hint = fmt::format(
      "Photo mode   left stick: move   right stick: look   L1 / R1: down / up   R2: slower   "
      "F2: picture   {}: filters   {} or Start: back",
      SDL_GetKeyName((SDL_Keycode)g_enhanced_menu.keys[0].load()),
      SDL_GetKeyName((SDL_Keycode)g_enhanced_menu.keys[1].load()));
  ImGui::TextUnformatted(hint.c_str());
  ImGui::End();
}

void EnhancedPost::draw_menu() {
  ImGui::SetNextWindowPos(ImVec2(30, 60), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.88f);
  // (focused as it opens, so a pad can move through it right away)
  static int drawn_frame = -10;
  if (ImGui::GetFrameCount() - drawn_frame > 1) {
    ImGui::SetNextWindowFocus();
    // (one line each time it opens: test runs check it)
    lg::info("[enhanced] menu open");
  }
  drawn_frame = ImGui::GetFrameCount();
  // the menu's own look, popped on every way out (see MenuStyle)
  const MenuStyle style;
  // no close button: \ or F1 closes it (and gives the mouse back to the camera); Select + R3 on a pad.
  // Its own title bar is drawn inside (menu_title); the name stays the window's id, so it keeps
  // where it was put
  if (!ImGui::Begin("Jak Enhanced Trilogy by stichlola  ( \\, F1 or Select+R3 closes )", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
    ImGui::End();
    return;
  }
  menu_glow();
  menu_title("\\, F1 or Select+R3 closes");
  auto& s = m_settings;
  // photo mode: in and out, and while it's on its filter
  if (!enhanced_light() &&
      ImGui::Button(g_enhanced_menu.photo ? "Leave photo mode  (F3, Start)" : "Photo mode  (F3)")) {
    if (g_enhanced_menu.editor) {
      enhanced_editor_toggle();
    } else {
      g_enhanced_menu.photo = !g_enhanced_menu.photo;
    }
    if (g_enhanced_menu.photo) {
      g_enhanced_menu.open = false;
    }
  }
  // the in-game editor: photo mode with tools to paint grass, place things, light them
  if (!enhanced_light()) {
    ImGui::SameLine();
    if (ImGui::Button(g_enhanced_menu.editor ? "Leave the editor  (F4)" : "Editor  (F4)")) {
      enhanced_editor_toggle();
      g_enhanced_menu.open = false;
    }
  }
  if (g_enhanced_menu.photo_active) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    if (ImGui::BeginCombo("Filter", kPhotoFilters[std::clamp(s.photo_filter, 0, kPhotoFilterCount - 1)].name)) {
      for (int i = 0; i < kPhotoFilterCount; i++) {
        if (ImGui::Selectable(kPhotoFilters[i].name, s.photo_filter == i)) {
          s.photo_filter = i;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    menu_slider_float("Vignette##photo", &s.photo_vignette, 0.f, 1.f, "%.2f");
    ImGui::TextDisabled("Close this menu (F1), then F2 takes the picture (in your OpenGOAL screenshots folder).");
  }
  // the game's nap window (and the beds, the report: not in Enhanced Light)
  if (!enhanced_light() && ImGui::Button("Nap...  (N, Select)")) {
    g_enhanced_menu.ask(12, "");
    g_enhanced_menu.open = false;
  }
  // a bed for the nap: Jak standing on it (saved in game/assets/enhanced/beds.json)
  if (!enhanced_light()) {
    auto& beds = g_beds[game_index()];
    const auto& pl = g_enhanced_player;
    ImGui::SameLine();
    if (ImGui::Button("Mark a bed here") && pl.valid) {
      beds.push_back({pl.pos[0], pl.pos[1], pl.pos[2]});
      save_beds();
    }
    ImGui::SameLine();
    if (ImGui::Button("Unmark the nearest bed") && pl.valid && !beds.empty()) {
      size_t best = 0;
      float best_d2 = 1e30f;
      for (size_t i = 0; i < beds.size(); i++) {
        float d2 = 0.f;
        for (int k = 0; k < 3; k++) {
          d2 += (beds[i][k] - pl.pos[k]) * (beds[i][k] - pl.pos[k]);
        }
        if (d2 < best_d2) {
          best_d2 = d2;
          best = i;
        }
      }
      beds.erase(beds.begin() + best);
      save_beds();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Napping within 3 m of a marked bed, Jak sleeps on it (%d in this game)",
                        (int)beds.size());
    }
  }
  // the report window (scripts\enhanced\report.ps1, also opened by the launcher after a crash),
  // read as text like update.ps1 so the execution policy doesn't matter
  if (!enhanced_light()) {
    const auto proj = file_util::get_jak_project_dir();
    const fs::path ps1 = proj / "scripts" / "enhanced" / "report.ps1";
    ImGui::SameLine();
    if (ImGui::Button("Send feedback") && fs::exists(ps1)) {
      std::system(fmt::format("start \"\" powershell -NoProfile -WindowStyle Hidden -Command \"& "
                              "([scriptblock]::Create([IO.File]::ReadAllText('{}'))) -Proj '{}'\"",
                              ps1.string(), proj.string())
                      .c_str());
      g_enhanced_menu.open = false;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "A problem, an idea? Write it and send it with one click: your newest logs, PC details\n"
          "and settings go with it (a window opens over the game: Alt+Tab if you don't see it)");
    }
  }
  // the preset last picked, lit in its row until a setting changes (only the look: nothing reads it)
  static int preset_lit = -1;
  static std::string preset_lit_settings;
  if (menu_section("Graphics", ImGuiTreeNodeFlags_DefaultOpen)) {
    // (in the menu's order: Performance sits between Classic and Medium)
    static const char* presets[] = {"Classic", "Performance", "Medium", "Maximum"};
    static const int preset_ids[] = {0, 3, 1, 2};
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset:");
    for (int i = 0; i < 4; i++) {
      ImGui::SameLine(0, i == 0 ? -1.f : 0.f);
      if (menu_segment(presets[i], preset_lit == i, 110, i == 0 ? 0 : i == 3 ? 2 : 1)) {
        apply_preset(preset_ids[i]);
        preset_lit = i;
        preset_lit_settings.clear();
      }
    }
    menu_toggle("Enhanced graphics (all)", &s.enabled);
    ImGui::SameLine(260);
    menu_toggle("Auto quality (keeps 60 fps)", &s.auto_quality);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Where the graphics card can't keep 60 fps (Haven at night...), the mist volume, the\n"
          "indirect light, the god rays and the soft shadows are set aside one at a time, then the\n"
          "resolution is lowered, and they come back when there is room again. Now: %s",
          m_auto_level == 0 ? "everything on"
          : m_auto_level == 1 ? "mist volume off"
          : m_auto_level == 2 ? "mist volume and indirect light off"
          : m_auto_level == 3 ? "mist volume, indirect light, god rays and soft shadows off"
          : m_auto_level == 4 ? "those off, and 85% resolution"
                              : "those off, and 72% resolution");
    }
    ImGui::BeginDisabled(!s.enabled);
    // the switches in groups (closed until opened: ImGui remembers which), two columns in each
    int n = 0;
    auto check = [&](const char* label, bool* value) {
      if (n++ % 2) {
        ImGui::SameLine(260);
      }
      return menu_toggle(label, value);
    };
    // a switch for a setting kept as a strength: on is `on`, off is 0
    auto check_amount = [&](const char* label, float* value, float on) {
      bool b = *value > 0.f;
      if (check(label, &b)) {
        *value = b ? on : 0.f;
      }
    };
    auto group = [&](const char* label) {
      n = 0;
      return ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth);
    };
    if (group("Image and camera")) {
      check("Antialiasing (TAA)", &s.taa);
      check("Bloom", &s.bloom);
      check("God rays", &s.shafts);
      check_amount("Lens flare", &s.lens_flare, 0.6f);
      check("Depth of field", &s.dof);
      check("Cutscene depth of field", &s.cine_dof);
      check_amount("Motion blur", &s.motion_blur, 0.3f);
      check("Color grade per area", &s.area_grading);
      ImGui::SetNextItemWidth(200);
      menu_slider_float("Vivid colors", &s.vivid, 0.f, 1.f, "%.2f");
      ImGui::TreePop();
    }
    if (group("Light and shadows")) {
      check("Sun shadows", &s.shadow_map);
      check("Character and tree shadows", &s.shadow_casters);
      check("Contact shadows", &s.sun_shadows);
      check("Soft shadows", &s.soft_shadows);
      check("Ambient occlusion (SSAO)", &s.ssao);
      check("Ray-traced ambient occlusion", &s.rt_ao);
      check("Indirect light (SSGI)", &s.gi);
      check("Character lighting", &g_enhanced_merc.enabled);
      check("Dynamic lights", &s.lights);
      check_amount("Light shadows", &s.light_shadow, 0.85f);
      ImGui::BeginDisabled(!s.lights);
      ImGui::SetNextItemWidth(200);
      menu_slider_float("Lamp strength", &s.lamp_strength, 0.f, 3.f, "%.2f");
      if (ImGui::TreeNode("Place a light")) {
        // a light of your own where Jak stands, 2.5 m up (saved in game/assets/enhanced/lights.json)
        static float color[3] = {1.f, 0.62f, 0.32f};
        static float radius = 8.f;
        static bool flicker = false;
        ImGui::SetNextItemWidth(200);
        ImGui::ColorEdit3("Color##ml", color);
        ImGui::SetNextItemWidth(200);
        menu_slider_float("Radius (m)##ml", &radius, 2.f, 40.f, "%.0f");
        menu_toggle("Flickers like a fire##ml", &flicker);
        auto& list = g_manual_lights[game_index()];
        const auto& pl = g_enhanced_player;
        if (ImGui::Button("Add a light here") && pl.valid) {
          ManualLight l;
          l.note = g_enhanced_scene.level;
          l.pos[0] = pl.pos[0];
          l.pos[1] = pl.pos[1] + 2.5f * 4096.f;
          l.pos[2] = pl.pos[2];
          l.radius = radius * 4096.f;
          std::memcpy(l.color, color, sizeof(color));
          l.flicker = flicker;
          list.push_back(l);
          save_manual_lights();
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove the nearest") && pl.valid && !list.empty()) {
          size_t best = 0;
          float best_d2 = 1e30f;
          for (size_t i = 0; i < list.size(); i++) {
            float d2 = 0.f;
            for (int k = 0; k < 3; k++) {
              d2 += (list[i].pos[k] - pl.pos[k]) * (list[i].pos[k] - pl.pos[k]);
            }
            if (d2 < best_d2) {
              best_d2 = d2;
              best = i;
            }
          }
          list.erase(list.begin() + best);
          save_manual_lights();
        }
        ImGui::Text("%d lights of your own in this game", (int)list.size());
        ImGui::TreePop();
      }
      ImGui::EndDisabled();
      ImGui::TreePop();
    }
    if (group("Night")) {
      check_amount("Dark nights", &s.dark_nights, 1.f);
      check_amount("Light spill at night", &s.light_spill, 1.f);
      check("Jak's lantern in the desert", &s.desert_lamp);
      ImGui::TreePop();
    }
    if (group("Sky and weather")) {
      check("Clouds", &s.clouds);
      check("Cloud shadows", &s.cloud_shadows);
      if (!kFogOff) {
        check("Volumetric mist", &s.volume);
      }
      ImGui::SetNextItemWidth(200);
      menu_slider_float("Snow on the ground", &s.snow_cover, 0.f, 1.f, "%.2f");
      weather_menu(s, "Weather");
      ImGui::TreePop();
    }
    if (group("Surfaces and materials")) {
      check_amount("Surface relief", &s.relief, 1.f);
      check("Geometry normals", &s.geometry_normals);
      check_amount("Reflective metal and glass", &s.materials, 1.f);
      bool traced = s.ray_traced > 0;
      if (check("Ray-traced reflections", &traced)) {
        s.ray_traced = traced ? 1 : 0;
      }
      check_amount("Footprints in the sand", &s.prints, 1.f);
      bool smooth = g_enhanced_smooth_chars > 0;
      if (check("Smoother characters (after a restart)", &smooth)) {
        g_enhanced_smooth_chars = smooth ? 1 : 0;
      }
      ImGui::TreePop();
    }
    if (group("World and vegetation")) {
      check("Extended draw distance", &s.far_view);
      check("Wind on the grass", &g_enhanced_wind.enabled);
      check("Grass bends away", &g_enhanced_wind.push);
      ImGui::NewLine();
      ImGui::SetNextItemWidth(200);
      ImGui::Combo("Vegetation", &g_enhanced_vegetation, "Classic\0Moderate\0Lush\0");
      ImGui::SetNextItemWidth(200);
      menu_slider_float("Detailed scenery", &g_enhanced_scenery_dist, 0.f, 120.f, "%.0f m");
      ImGui::TreePop();
    }
    if (group("Effects")) {
      check("Fires (heat, embers)", &s.fire);
      check_amount("Lit smoke", &s.lit_smoke, 0.8f);
      check_amount("Explosion sparks", &s.blast_sparks, 1.f);
      check_amount("Fireflies and pollen", &s.motes, 1.f);
      ImGui::TreePop();
    }
    if (group("Water")) {
      check("Reflections", &s.water_reflections);
      check_amount("Swell", &s.water_swell, 1.f);
      check_amount("Refraction", &s.water_refraction, 0.012f);
      check_amount("Caustics", &s.water_caustics, 0.8f);
      ImGui::TreePop();
    }
    ImGui::EndDisabled();
    if (group("Sound and subtitles")) {
      ImGui::SetNextItemWidth(200);
      menu_slider_float("Ambient sounds", &s.ambience, 0.f, 1.f, "%.2f");
      // the subtitles' size (accessibility): the default is smaller than the PS2's
      static const int kSizes[] = {100, 120, 138, 170};
      int pick = 0;
      for (int i = 0; i < 4; i++) {
        if (g_enhanced_menu.subtitle_size >= kSizes[i]) {
          pick = i;
        }
      }
      static const char* const kSizeNames[] = {"Small", "Medium", "Large (original)", "Extra large"};
      ImGui::SetNextItemWidth(200);
      if (ImGui::Combo("Subtitle size", &pick, kSizeNames, 4)) {
        g_enhanced_menu.subtitle_size = kSizes[pick];
      }
      ImGui::TreePop();
    }
  }
  if (menu_section("City", ImGuiTreeNodeFlags_DefaultOpen)) {
    // passers-by, traffic, guards and Metal Heads (the game scales its wanted counts, see
    // enhanced-population-count); the game reads the same variable at boot
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo("Population", &m_population, "Low\0Normal\0High\0Extreme\0")) {
      g_enhanced_menu.ask(6, std::to_string(m_population));
    }
    ImGui::SetNextItemWidth(200);
    int fov = g_enhanced_menu.fov;
    if (menu_slider_int("Field of view %", &fov, 80, 140)) {
      g_enhanced_menu.fov = fov;
    }
    bool map = g_enhanced_menu.minimap;
    if (menu_toggle("Minimap", &map)) {
      g_enhanced_menu.minimap = map;
    }
    ImGui::BeginDisabled(!map);
    ImGui::SetNextItemWidth(200);
    int map_size = g_enhanced_menu.minimap_size;
    if (menu_slider_int("Minimap size %", &map_size, 50, 100)) {
      g_enhanced_menu.minimap_size = map_size;
    }
    ImGui::EndDisabled();
    if (g_game_version == GameVersion::Jak3) {
      // (off: none parked, and one left out goes once Jak is out of it; enhanced-hellcat-update)
      bool hellcat = g_enhanced_menu.desert_hellcat;
      if (menu_toggle("Hellcat in the desert", &hellcat)) {
        g_enhanced_menu.desert_hellcat = hellcat;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The War Factory's Hellcat, parked by the Spargus wall: triangle to get in");
      }
      int character = g_enhanced_menu.player;
      if (ImGui::Combo(
              "Character", &character,
              [](void*, int i) { return kEnhancedCharacterTable[i].label; }, nullptr,
              kEnhancedCharacterCount)) {
        g_enhanced_menu.player = character;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Swap character: Ashelin or Sig in Jak's place (moves, jumps, vehicles), switched live.\n"
            "Ashelin: D-pad up takes out her pistol\n"
            "Sig: D-pad right takes out his Peacemaker");
      }
    }
    // (not in Enhanced Light: the game plays as it was, see pc_enhanced_storm_drag and the rest)
    bool play_weather = g_enhanced_play_weather.enabled;
    if (!enhanced_light()) {
      if (menu_toggle("Weather and night affect play", &play_weather)) {
        g_enhanced_play_weather.enabled = play_weather;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Sandstorms slow vehicles on sand; thick fog and sandstorms hide the minimap;\n"
            "more marauders roam the desert at night (Jak 3)");
      }
      bool balance = g_enhanced_balance;
      if (menu_toggle("Enhanced balance (Jak 3)", &balance)) {
        g_enhanced_balance = balance;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Guns for range, Dark Jak for close combat and crowds, Light Jak for defense:\n"
            "the Beam Reflexor bounces fewer times, the Needle Lazer, Arc Wielder and Gyro Burster\n"
            "hit softer, the Wave Concussor and Mass Inverter harder, the Super Nova costs less;\n"
            "Dark Jak hits much harder, shrugs off hits, and his blows and kills keep him going\n"
            "and heal him; Light Jak's shield and freeze cost less, his kills give light eco back");
      }
    }
  }
  save_menu_settings();
  if (preset_lit >= 0) {
    if (preset_lit_settings.empty()) {
      preset_lit_settings = m_saved_settings;
    } else if (preset_lit_settings != m_saved_settings) {
      preset_lit = -1;
    }
  }
  if (menu_section("Time of day", ImGuiTreeNodeFlags_DefaultOpen)) {
    struct Hour {
      const char* name;
      int hour;
    };
    static const Hour hours[] = {{"Dawn", 6},    {"Morning", 9},  {"Noon", 12},
                                 {"Sunset", 18}, {"Evening", 20}, {"Night", 23}};
    // the one the clock is at (within an hour and a half) lit in the row
    float now = 12.f;
    {
      std::lock_guard<std::mutex> lock(g_enhanced_scene.mutex);
      now = g_enhanced_scene.hour;
    }
    for (int i = 0; i < 6; i++) {
      if (i % 3) {
        ImGui::SameLine(0, 0);
      }
      const float away = std::fabs(std::fmod(now - hours[i].hour + 36.f, 24.f) - 12.f);  // round the clock
      if (menu_segment(hours[i].name, away < 1.5f, 162, i % 3)) {
        g_enhanced_menu.ask(1, std::to_string(hours[i].hour));
      }
    }
    static int hour = 12;
    ImGui::SetNextItemWidth(260);
    menu_slider_int("##hour", &hour, 0, 23, "%d:00");
    ImGui::SameLine();
    if (ImGui::Button("Set")) {
      g_enhanced_menu.ask(1, std::to_string(hour));
    }
    static bool frozen = false;
    if (menu_toggle("Freeze time", &frozen)) {
      g_enhanced_menu.ask(4, frozen ? "0" : "1");
    }
    // this evening's sky (each evening picks its own: this sets it by hand)
    static const char* nights[] = {"Starry", "Few stars", "Full moon", "Nearly full moon",
                                   "Red eclipse"};
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("Tonight", nights[m_night_kind])) {
      for (int i = 0; i < kSkyNights; i++) {
        if (ImGui::Selectable(nights[i], m_night_kind == i)) {
          m_sky_force_night = i;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    bool red = m_red_sunset;
    if (menu_toggle("Red sunset", &red)) {
      m_sky_force_red = red ? 1 : 0;
    }
  }
  if (menu_section("Controls")) {
    // the Enhanced shortcuts on the keyboard (the game's own buttons are in its Options menu)
    static const char* names[EnhancedMenu::kShortcuts] = {"This menu (\\ too)", "Photo mode",
                                                          "Nap", "Next game of the trilogy"};
    const int listening = g_enhanced_menu.listening;
    for (int i = 0; i < (enhanced_light() ? 1 : EnhancedMenu::kShortcuts); i++) {
      ImGui::PushID(i);
      ImGui::TextUnformatted(names[i]);
      ImGui::SameLine(260);
      const std::string label =
          listening == i ? std::string("press a key (Esc: keep)")
                         : std::string(SDL_GetKeyName((SDL_Keycode)g_enhanced_menu.keys[i].load()));
      if (ImGui::Button(label.c_str(), ImVec2(200, 0))) {
        g_enhanced_menu.listening = i;
      }
      ImGui::SameLine();
      if (ImGui::Button("Default")) {
        g_enhanced_menu.keys[i] = EnhancedMenu::kDefaultKeys[i];
      }
      ImGui::PopID();
    }
    ImGui::TextDisabled("Pad: Select opens the nap window, Select + R3 this menu.");
    ImGui::TextDisabled("The game's own buttons: its Options menu.");
  }
  if (menu_section("My Levels", ImGuiTreeNodeFlags_DefaultOpen)) {
    // (read again now and then: the editor or an import may have changed them)
    static std::vector<EnhancedLevelInfo> levels;
    static double levels_read = -100.0;
    if (m_time - levels_read > 2.0) {
      levels_read = m_time;
      levels = enhanced_levels();
    }
    const std::string active = enhanced_level_active();
    if (levels.empty()) {
      ImGui::TextDisabled("None yet: in the editor (F4), File > New Level.");
    }
    for (const auto& l : levels) {
      ImGui::PushID(l.id.c_str());
      ImGui::BeginDisabled(l.id == active);
      if (ImGui::Button(l.id == active ? "Playing" : "Play", ImVec2(90, 0))) {
        enhanced_level_play(l.id);
        g_enhanced_menu.open = false;
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::Text("%s", l.name.c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("%d objects", l.objects);
      ImGui::PopID();
    }
    if (!active.empty() && ImGui::Button("Back to the main world", ImVec2(240, 0))) {
      enhanced_level_leave();
    }
  }
  if (menu_section("Teleport", ImGuiTreeNodeFlags_DefaultOpen)) {
    int shown = 0;
    for (const auto& p : enhanced_places()) {
      if (shown++ % 2) {
        ImGui::SameLine();
      }
      if (ImGui::Button(p.name, ImVec2(240, 0))) {
        g_enhanced_menu.ask(2, p.continue_point);
      }
    }
    const auto& cam = g_enhanced_camera;
    if (cam.valid) {
      ImGui::TextDisabled("camera %.0f %.0f %.0f m", cam.trans[0] / kMeter, cam.trans[1] / kMeter,
                          cam.trans[2] / kMeter);
    }
  }
  ImGui::End();
}

std::string EnhancedPost::menu_settings_json() const {
  const auto& s = m_settings;
  nlohmann::json j;
  j["enabled"] = s.enabled;
  j["taa"] = s.taa;
  j["volume"] = s.volume;
  j["shadow_map"] = s.shadow_map;
  j["shadow_casters"] = s.shadow_casters;
  j["sun_shadows"] = s.sun_shadows;
  j["ssao"] = s.ssao;
  j["gi"] = s.gi;
  j["characters"] = g_enhanced_merc.enabled;
  j["bloom"] = s.bloom;
  j["shafts"] = s.shafts;
  j["dof"] = s.dof;
  j["motion_blur"] = s.motion_blur;
  j["lights"] = s.lights;
  j["lamp_strength"] = s.lamp_strength;
  j["fire"] = s.fire;
  j["light_shadow"] = s.light_shadow;
  j["geometry_normals"] = s.geometry_normals;
  j["prints"] = s.prints;
  j["dark_nights"] = s.dark_nights;
  j["cloud_shadows"] = s.cloud_shadows;
  j["lens_flare"] = s.lens_flare;
  j["desert_lamp"] = s.desert_lamp;
  j["lantern_intro_done"] = s.lantern_intro_done;
  j["light_spill"] = s.light_spill;
  j["lit_smoke"] = s.lit_smoke;
  j["far_view"] = s.far_view;
  j["materials"] = s.materials;
  j["ray_traced"] = s.ray_traced;
  j["rt_ao"] = s.rt_ao;
  j["relief"] = s.relief;
  j["wind"] = g_enhanced_wind.enabled;
  j["grass_push"] = g_enhanced_wind.push;
  j["vegetation"] = g_enhanced_vegetation;
  j["scenery_dist"] = g_enhanced_scenery_dist;
  j["smooth_chars"] = g_enhanced_smooth_chars;
  j["water_reflections"] = s.water_reflections;
  j["water_swell"] = s.water_swell;
  j["water_refraction"] = s.water_refraction;
  j["water_caustics"] = s.water_caustics;
  j["area_grading"] = s.area_grading;
  j["soft_shadows"] = s.soft_shadows;
  j["auto_quality"] = s.auto_quality;
  j["clouds"] = s.clouds;
  j["cine_dof"] = s.cine_dof;
  j["blast_sparks"] = s.blast_sparks;
  j["motes"] = s.motes;
  j["ambience"] = s.ambience;
  j["snow_cover"] = s.snow_cover;
  j["weather"] = s.weather;
  j["weather_mix"] = s.weather_mix;
  j["season"] = s.season;
  j["photo_filter"] = s.photo_filter;
  j["photo_vignette"] = s.photo_vignette;
  j["eye_adapt"] = s.eye_adapt;
  j["char_detail"] = s.char_detail;
  j["parallax"] = s.parallax;
  j["wall_notile"] = s.wall_notile;
  j["fx_detail"] = s.fx_detail;
  j["hit_chroma"] = s.hit_chroma;
  j["dawn_fog"] = s.dawn_fog;
  j["cloud_rays"] = s.cloud_rays;
  j["lens_drops"] = s.lens_drops;
  j["vivid"] = s.vivid;
  j["population"] = m_population;
  j["fov"] = g_enhanced_menu.fov.load();
  j["subtitle_size"] = g_enhanced_menu.subtitle_size.load();
  j["show_minimap"] = g_enhanced_menu.minimap.load();
  j["minimap_size"] = g_enhanced_menu.minimap_size.load();
  j["desert_hellcat"] = g_enhanced_menu.desert_hellcat.load();
  {
    int player = g_enhanced_menu.player.load();
    j["player"] = kEnhancedCharacterTable[(player >= 0 && player < kEnhancedCharacterCount) ? player : 0].name;
  }
  j["weather_affects_play"] = g_enhanced_play_weather.enabled.load();
  j["enhanced_balance"] = g_enhanced_balance.load();
  auto keys = nlohmann::json::array();
  for (const auto& k : g_enhanced_menu.keys) {
    keys.push_back(k.load());
  }
  j["shortcut_keys"] = keys;
  j["hints_shown"] = g_enhanced_menu.hints_shown.load();
  return j.dump(2);
}

void EnhancedPost::apply_menu_settings(const std::string& text) {
  auto j = parse_commented_json(text, "enhanced-settings.json");
  auto get = [&](const char* key, auto& value) {
    if (j.contains(key)) {
      value = j.at(key).get<std::decay_t<decltype(value)>>();
    }
  };
  auto& s = m_settings;
  get("enabled", s.enabled);
  get("taa", s.taa);
  get("volume", s.volume);
  get("shadow_map", s.shadow_map);
  get("shadow_casters", s.shadow_casters);
  get("sun_shadows", s.sun_shadows);
  get("ssao", s.ssao);
  get("gi", s.gi);
  get("characters", g_enhanced_merc.enabled);
  get("bloom", s.bloom);
  get("shafts", s.shafts);
  get("dof", s.dof);
  get("motion_blur", s.motion_blur);
  get("lights", s.lights);
  get("lamp_strength", s.lamp_strength);
  get("fire", s.fire);
  get("light_shadow", s.light_shadow);
  get("geometry_normals", s.geometry_normals);
  get("prints", s.prints);
  get("dark_nights", s.dark_nights);
  get("cloud_shadows", s.cloud_shadows);
  get("lens_flare", s.lens_flare);
  get("desert_lamp", s.desert_lamp);
  get("lantern_intro_done", s.lantern_intro_done);
  get("light_spill", s.light_spill);
  get("lit_smoke", s.lit_smoke);
  get("far_view", s.far_view);
  get("materials", s.materials);
  get("ray_traced", s.ray_traced);
  get("rt_ao", s.rt_ao);
  get("relief", s.relief);
  get("wind", g_enhanced_wind.enabled);
  get("grass_push", g_enhanced_wind.push);
  if (!std::getenv("JAK_ENHANCED_VEGETATION")) {
    get("vegetation", g_enhanced_vegetation);
    g_enhanced_vegetation = std::clamp(g_enhanced_vegetation, 0, 2);
  }
  if (!std::getenv("JAK_ENHANCED_SCENERY")) {
    get("scenery_dist", g_enhanced_scenery_dist);
    g_enhanced_scenery_dist = std::clamp(g_enhanced_scenery_dist, 0.f, 200.f);
  }
  if (!std::getenv("JAK_ENHANCED_SMOOTH_CHARS")) {
    get("smooth_chars", g_enhanced_smooth_chars);
  }
  get("water_reflections", s.water_reflections);
  get("water_swell", s.water_swell);
  get("water_refraction", s.water_refraction);
  get("water_caustics", s.water_caustics);
  get("area_grading", s.area_grading);
  get("soft_shadows", s.soft_shadows);
  get("auto_quality", s.auto_quality);
  get("clouds", s.clouds);
  get("cine_dof", s.cine_dof);
  get("blast_sparks", s.blast_sparks);
  get("motes", s.motes);
  get("ambience", s.ambience);
  get("snow_cover", s.snow_cover);
  get("weather", s.weather);
  s.weather = std::clamp(s.weather, 0, kWeatherCount - 1);
  get("weather_mix", s.weather_mix);
  get("season", s.season);
  get("photo_filter", s.photo_filter);
  get("photo_vignette", s.photo_vignette);
  get("eye_adapt", s.eye_adapt);
  get("char_detail", s.char_detail);
  get("parallax", s.parallax);
  get("wall_notile", s.wall_notile);
  get("fx_detail", s.fx_detail);
  get("hit_chroma", s.hit_chroma);
  get("dawn_fog", s.dawn_fog);
  get("cloud_rays", s.cloud_rays);
  get("lens_drops", s.lens_drops);
  s.season = std::clamp(s.season, 0, 4);
  get("vivid", s.vivid);
  get("population", m_population);
  {
    int fov = g_enhanced_menu.fov;
    get("fov", fov);
    g_enhanced_menu.fov = fov;
    int subtitle_size = g_enhanced_menu.subtitle_size;
    get("subtitle_size", subtitle_size);
    g_enhanced_menu.subtitle_size = subtitle_size;
  }
  m_population = std::clamp(m_population, 0, 3);
  bool map = true;  // ("show_minimap": the old "minimap" key saved it off by default)
  get("show_minimap", map);
  g_enhanced_menu.minimap = map;
  int map_size = g_enhanced_menu.minimap_size;
  get("minimap_size", map_size);
  bool hellcat = true;
  get("desert_hellcat", hellcat);
  g_enhanced_menu.desert_hellcat = hellcat;
  std::string player_name = "jak";
  get("player", player_name);
  g_enhanced_menu.player = std::max(0, enhanced_character_index(player_name.c_str()));
  bool play_weather = true;
  get("weather_affects_play", play_weather);
  g_enhanced_play_weather.enabled = play_weather;
  bool balance = false;
  get("enhanced_balance", balance);
  g_enhanced_balance = balance;
  g_enhanced_menu.minimap_size = map_size;
  int hints = 0;
  get("hints_shown", hints);
  g_enhanced_menu.hints_shown = hints;
  std::vector<u32> keys;
  get("shortcut_keys", keys);
  for (size_t i = 0; i < keys.size() && i < (size_t)EnhancedMenu::kShortcuts; i++) {
    g_enhanced_menu.keys[i] = keys[i];
  }
}

namespace {
bool all_on(int preset) {
  return preset == 2;
}
}  // namespace

// Classico: the game as it was (the enhanced pass off), Medio: the costly passes off (mist, indirect
// light, depth of field, the lights' shadows...), Massimo: everything on. Performance (3): Medium
// without the passes that cost the most on a weak graphics card (the sun's shadow map, the
// occlusion, the traced reflections, the far view, the extra plants), the look kept (grade, lights,
// weather, nights)
void EnhancedPost::apply_preset(int preset) {
  const bool perf = preset == 3;
  if (perf) {
    apply_preset(1);
    auto& s = m_settings;
    s.shadow_map = false;
    s.ssao = false;
    s.sun_shadows = false;
    s.water_reflections = false;
    s.far_view = false;
    s.light_spill = 0.f;
    s.lit_smoke = 0.f;
    s.cloud_shadows = false;
    s.relief = 0.f;
    s.parallax = 0.f;
    s.wall_notile = false;
    g_enhanced_vegetation = 0;
    return;
  }
  auto& s = m_settings;
  const Settings d;
  s.enabled = preset > 0;
  s.taa = d.taa;
  s.shadow_map = d.shadow_map;
  s.sun_shadows = d.sun_shadows;
  s.ssao = d.ssao;
  s.bloom = d.bloom;
  s.lights = d.lights;
  s.lamp_strength = d.lamp_strength;
  s.fire = d.fire;
  s.geometry_normals = d.geometry_normals;
  s.prints = d.prints;
  s.dark_nights = d.dark_nights;
  s.cloud_shadows = d.cloud_shadows;
  s.lens_flare = d.lens_flare;
  s.desert_lamp = d.desert_lamp;
  s.light_spill = d.light_spill;
  s.lit_smoke = d.lit_smoke;
  s.far_view = d.far_view;
  s.materials = d.materials;
  s.relief = d.relief;
  s.parallax = d.parallax;
  s.wall_notile = d.wall_notile;
  s.ray_traced = all_on(preset) ? d.ray_traced : 0;
  s.rt_ao = all_on(preset) && d.rt_ao;
  s.water_reflections = d.water_reflections;
  s.water_swell = d.water_swell;
  bool all = preset == 2;
  s.volume = all && d.volume;
  s.shadow_casters = all && d.shadow_casters;
  s.gi = all && d.gi;
  s.shafts = all && d.shafts;
  s.dof = all && d.dof;
  s.motion_blur = all ? d.motion_blur : 0.f;
  s.light_shadow = all ? d.light_shadow : 0.f;
  s.water_refraction = all ? d.water_refraction : 0.f;
  s.water_caustics = all ? d.water_caustics : 0.f;
  s.area_grading = d.area_grading;
  s.clouds = d.clouds;
  s.blast_sparks = d.blast_sparks;
  s.fx_detail = d.fx_detail;
  s.motes = d.motes;
  s.weather = d.weather;
  s.weather_mix = d.weather_mix;
  s.vivid = d.vivid;
  s.soft_shadows = all && d.soft_shadows;
  s.cine_dof = all && d.cine_dof;
  g_enhanced_merc.enabled = preset > 0;
  g_enhanced_wind.enabled = preset > 0;
  g_enhanced_wind.push = preset > 0;
  g_enhanced_vegetation = preset;
}

// written only when something changed (the menu calls it every frame it is open)
void EnhancedPost::save_menu_settings() {
  auto text = menu_settings_json();
  if (text == m_saved_settings) {
    return;
  }
  m_saved_settings = text;
  try {
    auto path = file_util::get_user_settings_dir(g_game_version) / "enhanced-settings.json";
    file_util::create_dir_if_needed_for_file(path);
    file_util::write_text_file(path, text);
  } catch (std::exception& e) {
    lg::error("[enhanced] could not save the settings: {}", e.what());
  }
}
