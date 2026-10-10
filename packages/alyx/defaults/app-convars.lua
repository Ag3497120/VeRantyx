-- app-convars.lua: the convars file bundled in the app (tools/alyx-test/build-tf.sh). It sets NOTHING: the demo's
-- render settings reach the game as defaults through the files the app writes when missing (hlvr/cfg/deckard/video2.txt,
-- the game's own saved video settings, and the fixed fidelity table), so whatever a player then changes in Options is the
-- game's to keep. On a fresh install those give exactly FIX-demo-convars.lua's values at spawn (Mac, a1_intro_world,
-- 2026-10-03), which test scripts still push to force them. This only reads them back for a pulled log: KLCV, KLTAB,
-- KLSTREAM at each spawn.
ListenToGameEvent("player_activate", function()
  local w = Entities:First()
  if w then w:SetThink(function()
    local out = {}
    for _, k in ipairs({ "r_texturefilteringquality", "r_texture_stream_max_resolution", "volume_fog_width", "vr_aoproxy_enable",
        "r_high_quality_shadows", "vr_fidelity_level", "vr_render_target_scale" }) do out[#out + 1] = k .. "=" .. tostring(Convars:GetStr(k)) end
    Msg("KLCV " .. table.concat(out, " ") .. "\n")
    local tab = {}
    for _, k in ipairs({ "vr_msaa", "r_ssao", "r_post_bloom", "r_high_quality_shadows", "sc_disable_spotlight_shadows",
        "vr_single_light_fast_path_sun_cascade_0_texels_per_world", "vr_single_light_fast_path_sun_cascade_1_texels_per_world",
        "vr_aoproxy_enable", "vr_character_level_placeholder", "cl_enable_eye_occlusion" }) do tab[#tab + 1] = k .. "=" .. tostring(Convars:GetStr(k)) end
    Msg("KLTAB " .. table.concat(tab, " ") .. "\n")
    return nil end, "klhi", 2.0) end
end, nil)

-- Print the texture-streaming convars' current values (defaults under our video settings) at the first spawn.
ListenToGameEvent("player_activate", function()
  local w = Entities:First(); if not w then return end
  w:SetThink(function()
    for _, k in ipairs({ "r_texture_stream_max_resolution", "r_texture_stream_mip_bias", "r_texture_stream_resolution_bias",
        "r_texture_stream_resolution_bias_decrease_rate", "r_texture_stream_resolution_bias_increase_rate",
        "r_texture_stream_resolution_bias_min", "r_texture_stream_resolution_bias_update_period",
        "r_texture_stream_throttle_amount", "r_texture_stream_throttle_count", "gpu_mem_level", "mem_level" }) do
      Msg("KLSTREAM " .. k .. " = " .. tostring(Convars:GetStr(k)) .. "\n")
    end
    return nil end, "klstream", 2.0)
end, nil)
