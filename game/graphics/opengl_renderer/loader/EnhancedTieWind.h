#pragma once

#include <string>

#include "common/custom_data/Tfrag3Data.h"

/*!
 * Jak Enhanced: wind for the static TIE trees. The game sways only the protos it flagged for wind
 * (Tie3::render_tree_wind); every other tree, palm and bush drawn by TIE stands still. When a level
 * loads, each static TIE draw whose texture is foliage (a cutout with leafy name) or a tree's trunk
 * gets a sway weight per vertex in PreloadedVertex::enh_sway: its height above the plant's foot
 * (bits 0-14, 0..kEnhTieSwayMaxM meters) and whether it is a leaf (bit 15, a flutter on top).
 * The foot is a smooth field over the world (the lowest such vertex around, blended between 2 m
 * cells), so a trunk and its leaves bend as one. tfrag3.vert does the sway (tune_tie_wind).
 * The game's wind ties get the same gusts as a shear from their foot (Tie3::render_tree_wind): in
 * Jak 3 the game's own wind never moved them (forest, 697 trees, still at tie.wind_gain 20).
 * JAK_ENHANCED_TIE_WIND_LOG=1 logs each level's swaying, cutout, wind-tie and shrub textures.
 */
void enhanced_tie_wind_weights(tfrag3::Level& level, const std::string& name);

constexpr float kEnhTieSwayMaxM = 12.f;
