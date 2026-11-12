#pragma once

#include <string>

#include "common/custom_data/Tfrag3Data.h"

/*!
 * Jak Enhanced: wind for the static TIE trees. The game sways only the protos it flagged for wind
 * (Tie3::render_tree_wind); every other tree, palm and bush drawn by TIE stands still. When a level
 * loads, each static TIE draw whose texture is foliage (a cutout with leafy name) or a tree's trunk
 * gets a sway weight per vertex in PreloadedVertex::enh_sway. A trunk vertex: its height above
 * the plant's foot (bits 0-14, 0..kEnhTieSwayMaxM meters). A leaf (bit 15): its height in
 * decimeters (bits 8-14) and its reach (bits 0-7, kEnhTieReachStepM steps): how far it is from
 * the plant's trunk (the nearest trunk vertex, else its own height: a bush), so a palm's frond
 * swings from where it grows out of the crown, its tip most, and the trunk barely leans.
 * The foot is a smooth field over the world (the lowest such vertex around, blended between 2 m
 * cells), so a trunk and its leaves lean as one. tfrag3.vert does the sway (tune_tie_wind,
 * tune_tie_trunk, tune_tie_frond).
 * The game's wind ties get the same gusts as a shear from their foot (Tie3::render_tree_wind): in
 * Jak 3 the game's own wind never moved them (forest, 697 trees, still at tie.wind_gain 20).
 * JAK_ENHANCED_TIE_WIND_LOG=1 logs each level's swaying, cutout, wind-tie and shrub textures.
 */
void enhanced_tie_wind_weights(tfrag3::Level& level, const std::string& name);

// whether a texture is leaves to the wind (a cutout with a plant's name): the wind ties' leaves
// flutter (tie_wind.vert)
bool enhanced_tie_wind_is_leaf(const tfrag3::Texture& tex);

// the trees' wind clock, seconds (Tie3.cpp): TIE and tfrag sway in step
float enh_tie_wind_seconds();

constexpr float kEnhTieSwayMaxM = 12.f;
constexpr float kEnhTieReachStepM = 0.025f;  // a leaf's reach: 0..6.375 m in 255 steps
