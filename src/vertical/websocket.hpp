#pragma once

class VerticalCanvas;
class RelayOutput;

// obs-websocket vendor "multistream-vertical" (CallVendorRequest), so the
// Multistream app, Stream Deck and Streamer.bot can drive the vertical
// canvas: scenes, the sources in them, its sound and its stream.
//
// Requests: state, get_scenes, switch_scene {scene}, create_scene {scene},
// rename_scene {scene, to}, duplicate_scene {scene, to}, remove_scene
// {scene}, move_scene {scene, delta}, link_scene {main, scene}, set_follow
// {on},
// add_source {scene, source}, get_audio, set_audio {source, mode, volume,
// muted}, set_output {enabled, server, key, bitrate}, start_output,
// stop_output. Each answers {ok: true, ...} or {ok: false, error}.
namespace vertical_ws {

// From obs_module_post_load (obs-websocket must have loaded first).
void registerVendor(VerticalCanvas *canvas, RelayOutput *output);
// At exit: later requests answer an error instead of touching OBS.
void shutdown();

} // namespace vertical_ws
