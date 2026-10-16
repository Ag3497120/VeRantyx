// alvr_probe: ALVR's streaming core on the Mac, alone (the step before VeRantyx's frames go to it). It sets the
// session up for the Mac (below), waits for an ALVR client, and logs what arrives; given --hevc <file> (Annex-B
// HEVC, tools/hevc_testpattern.swift), it streams that file's frames, each at the newest head pose's time.
//
//     alvr_probe [--hevc <file>] [--seconds N]
use alvr_common::{glam::Vec3, HAND_LEFT_ID, HAND_RIGHT_ID, HEAD_ID};
use alvr_server_core::{ServerCoreContext, ServerCoreEvent};
use alvr_session::CodecType;
use std::{
    env, fs,
    time::{Duration, Instant},
};

fn layout() -> alvr_filesystem::Layout { verantyx_alvr_bridge::layout(None) }

// Annex-B NAL units: (type, bytes with their start code)
fn nal_units(data: &[u8]) -> Vec<(u8, &[u8])> {
    let mut starts = vec![];
    let mut i = 0;
    while i + 3 < data.len() {
        if data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1 { starts.push(i); i += 4; } else { i += 1; }
    }
    starts.iter().enumerate().map(|(k, &a)| {
        let b = if k + 1 < starts.len() { starts[k + 1] } else { data.len() };
        ((data[a + 4] >> 1) & 0x3F, &data[a..b])
    }).collect()
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let arg = |name: &str| args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).cloned();
    let seconds: u64 = arg("--seconds").and_then(|s| s.parse().ok()).unwrap_or(60);
    let session = layout().session();
    verantyx_alvr_bridge::prepare_session(&layout());
    alvr_server_core::initialize_environment(layout());
    alvr_server_core::init_logging(Some(layout().session_log()), Some(layout().crash_log()));   // (both: else ALVR's log
                                                                                              //  goes to stdout too)

    // The test stream: its parameter sets, then its frames (an access unit per IDR or other slice)
    let stream = arg("--hevc").map(|p| fs::read(p).unwrap());
    let (mut config, mut frames): (Vec<u8>, Vec<(bool, Vec<u8>)>) = (vec![], vec![]);
    if let Some(data) = &stream {
        for (t, bytes) in nal_units(data) {
            match t {
                32 | 33 | 34 => { if frames.is_empty() { config.extend_from_slice(bytes) } }   // VPS SPS PPS
                16..=21 => frames.push((true, bytes.to_vec())),                                 // IDR / CRA
                0..=9 => frames.push((false, bytes.to_vec())),
                _ => {}
            }
        }
        println!("probe: {} bytes of parameter sets, {} frames", config.len(), frames.len());
    }

    let (ctx, events) = ServerCoreContext::new();
    if !config.is_empty() { ctx.set_video_config_nals(config.clone(), CodecType::Hevc); }   // (the client's decoder needs them first)
    ctx.start_connection();
    println!("probe: waiting for an ALVR client (session {})", session.display());
    let t0 = Instant::now();
    let (mut connected, mut tracking, mut last_sample, mut sent, mut idr_wanted, mut idr_requests, mut delivered) = (false, 0u64, Duration::ZERO, 0usize, true, 0u32, 0usize);
    let mut next_frame = Instant::now();
    let mut last_views = String::new();
    let mut frame_time = Duration::from_micros(13_889);                     // (the client's rate, once it says it)
    while t0.elapsed() < Duration::from_secs(seconds) {
        while let Ok(e) = events.recv_timeout(Duration::from_millis(1)) {
            match e {
                ServerCoreEvent::ClientConnected => {
                    connected = true; idr_wanted = true;
                    let o = &serde_json::from_str::<serde_json::Value>(&fs::read_to_string(&session).unwrap()).unwrap()["openvr_config"];
                    let hz = o["refresh_rate"].as_f64().unwrap_or(72.0).max(1.0);
                    frame_time = Duration::from_secs_f64(1.0 / hz);
                    println!("probe: {:.1} s: client connected: {}x{} per eye at {hz} Hz", t0.elapsed().as_secs_f32(), o["eye_resolution_width"], o["eye_resolution_height"]);
                    if !config.is_empty() { ctx.set_video_config_nals(config.clone(), CodecType::Hevc); }
                }
                ServerCoreEvent::ClientDisconnected => { connected = false; println!("probe: {:.1} s: client disconnected", t0.elapsed().as_secs_f32()); }
                ServerCoreEvent::ViewsConfig(v) => {
                    let f = format!("{:?} / {:?}", v.fov[0], v.fov[1]);
                    if f != last_views { println!("probe: views: fov {f}"); last_views = f; }   // (the client repeats them)
                }
                ServerCoreEvent::Tracking { sample_timestamp } => {
                    tracking += 1; last_sample = sample_timestamp;
                    if tracking % 300 == 1 {
                        let head = ctx.get_device_motion(*HEAD_ID, sample_timestamp);
                        let left = ctx.get_device_motion(*HAND_LEFT_ID, sample_timestamp).is_some();
                        let right = ctx.get_device_motion(*HAND_RIGHT_ID, sample_timestamp).is_some();
                        let at = head.map(|m| m.pose.position).unwrap_or(Vec3::ZERO);
                        println!("probe: tracking #{tracking}: head at ({:.2}, {:.2}, {:.2}), hands {left}/{right}", at.x, at.y, at.z);
                    }
                }
                ServerCoreEvent::Buttons(b) => println!("probe: {} buttons", b.len()),
                ServerCoreEvent::RequestIDR => { if !idr_wanted { idr_requests += 1; } idr_wanted = true; }
                ServerCoreEvent::Battery(b) => println!("probe: battery of device {:#x}: {} (plugged in: {})", b.device_id, b.gauge_value, b.is_plugged),
                ServerCoreEvent::PlayspaceSync(_) | ServerCoreEvent::CaptureFrame | ServerCoreEvent::GameRenderLatencyFeedback(_)
                | ServerCoreEvent::SetOpenvrProperty { .. } => {}
                ServerCoreEvent::ShutdownPending | ServerCoreEvent::RestartPending => println!("probe: restart / shutdown asked"),
            }
        }
        // The client's rate, each frame stamped with the newest tracking sample (what it would have been rendered for)
        if connected && !frames.is_empty() && tracking > 0 && Instant::now() >= next_frame {
            next_frame = (next_frame + frame_time).max(Instant::now());       // (no catching up)
            if idr_wanted {                                                   // (on from the stream's next IDR)
                while !frames[sent % frames.len()].0 { sent += 1; }
                idr_wanted = false;
            }
            let (idr, f) = &frames[sent % frames.len()];
            ctx.send_video_nal(last_sample, f.clone(), *idr);
            sent += 1; delivered += 1;
            if delivered % 720 == 0 { println!("probe: {delivered} frames sent ({idr_requests} IDRs asked for)"); }
        }
    }
    println!("probe: done: {tracking} tracking samples, {delivered} frames sent, {idr_requests} IDRs asked for");
}
