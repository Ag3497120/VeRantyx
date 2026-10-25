// VeRantyx's Mac side (MacEncoder/HardwareEncoder.swift) to ALVR's streaming core: a small C interface. The encoder
// sends its HEVC access units here, stamped with the pose time they were rendered for; what the ALVR client sends
// (head, hands or controllers, buttons, the eyes' field of view) comes back as one snapshot in the shape of the
// shared map's SharedHands (VRDriver/src/driver_verantyx.cpp), which the encoder copies there, so the SteamVR driver
// works as it does for the Vision Pro app.
//
// Times: ALVR stamps each tracking sample (a Duration on its clock); that sample's time, in seconds, is what goes
// into SharedHands.visionTimestamp, travels with the frame rendered for it, and comes back in vx_alvr_send, where
// it is turned back into the same Duration (exact to the nanosecond for some 100 days of uptime).
use alvr_common::{
    glam::{EulerRot, Quat, Vec3},
    DeviceMotion, Fov, Pose, HAND_LEFT_ID, HAND_RIGHT_ID, HEAD_ID, LEFT_MENU_CLICK_ID, LEFT_SQUEEZE_VALUE_ID,
    LEFT_THUMBSTICK_CLICK_ID, LEFT_THUMBSTICK_TOUCH_ID, LEFT_THUMBSTICK_X_ID, LEFT_THUMBSTICK_Y_ID, LEFT_TRIGGER_VALUE_ID,
    LEFT_X_CLICK_ID, LEFT_Y_CLICK_ID, RIGHT_A_CLICK_ID, RIGHT_B_CLICK_ID, RIGHT_SQUEEZE_VALUE_ID,
    RIGHT_SYSTEM_CLICK_ID, RIGHT_THUMBSTICK_CLICK_ID, RIGHT_THUMBSTICK_TOUCH_ID, RIGHT_THUMBSTICK_X_ID,
    RIGHT_THUMBSTICK_Y_ID, RIGHT_TRIGGER_VALUE_ID,
};
use alvr_packets::{ButtonValue, Haptics};
use alvr_server_core::{HandType, ServerCoreContext, ServerCoreEvent};
use alvr_session::{CodecType, SessionConfig};
use std::{
    collections::HashMap,
    env,
    ffi::{c_char, CStr},
    fs,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering},
        Arc, Mutex, OnceLock, RwLock,
    },
    thread,
    time::Duration,
};

pub fn layout(config_dir: Option<PathBuf>) -> alvr_filesystem::Layout {
    let dir = config_dir.unwrap_or_else(|| PathBuf::from(env::var("HOME").unwrap()).join("Library/Application Support/VeRantyx/ALVR"));
    // (executables_dir: where ALVR puts the adb it downloads for a wired headset when there is none on PATH)
    alvr_filesystem::Layout { config_dir: dir.clone(), log_dir: dir.clone(), executables_dir: dir, ..Default::default() }
}

// The session for the Mac, written before ALVR reads it (it reads it once): clients trusted as they come (there is no
// dashboard), game audio only from the device the encoder made for it (macOS cannot record an output device), HEVC
// (VideoToolbox), no server-side foveation (VeRantyx's frames are full resolution), TCP (the visionOS client wants it).
pub fn prepare_session(layout: &alvr_filesystem::Layout) {
    let path = layout.session();
    fs::create_dir_all(&layout.config_dir).ok();
    let mut s: serde_json::Value = fs::read_to_string(&path).ok().and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_else(|| serde_json::to_value(SessionConfig::default()).unwrap());
    let ss = &mut s["session_settings"];
    ss["connection"]["client_discovery"]["content"]["auto_trust_clients"] = true.into();
    // (VERANTYX_ALVR_PROTOCOL=udp: ALVR's own default, for a headset whose client takes it)
    let udp = env::var("VERANTYX_ALVR_PROTOCOL").is_ok_and(|v| v.eq_ignore_ascii_case("udp"));
    ss["connection"]["stream_protocol"]["variant"] = (if udp { "Udp" } else { "Tcp" }).into();
    // (game audio: the device the encoder made, VERANTYX_ALVR_AUDIO_DEVICE, when it made one; macOS has no loopback)
    match env::var("VERANTYX_ALVR_AUDIO_DEVICE") {
        Ok(name) if !name.is_empty() => {
            ss["audio"]["game_audio"]["enabled"] = true.into();
            let device = &mut ss["audio"]["game_audio"]["content"]["device"];
            device["set"] = true.into();
            device["content"]["variant"] = "NameSubstring".into();
            device["content"]["NameSubstring"] = name.into();
            // (the headset's buffer: ALVR's 50 ms ran dry, heard as breaks, in Half-Life: Alyx with the Vision Pro's
            //  ALVR app, where the sound shares the TCP stream with 40 Mbps of video; 150 ms still broke, 250 did not.
            //  VERANTYX_ALVR_AUDIO_BUFFER_MS)
            let ms = env::var("VERANTYX_ALVR_AUDIO_BUFFER_MS").ok().and_then(|v| v.parse::<u64>().ok()).unwrap_or(250);
            ss["audio"]["game_audio"]["content"]["buffering"]["average_buffering_ms"] = ms.into();
        }
        _ => ss["audio"]["game_audio"]["enabled"] = false.into(),
    }
    // (a headset on a USB cable, VERANTYX_ALVR_WIRED=1: the entry ALVR's dashboard adds with its Wired Connection
    //  switch. ALVR then forwards its two ports with adb to the headset's ALVR app, starts the app, and streams over
    //  the cable as TCP to localhost; adb from PATH, else ALVR downloads Google's platform-tools once.)
    let wired = "client.wired"; // (alvr_sockets::WIRED_CLIENT_HOSTNAME)
    if env::var("VERANTYX_ALVR_WIRED").is_ok_and(|v| v == "1") {
        if s["client_connections"].get(wired).is_none() {
            s["client_connections"][wired] = serde_json::json!({
                "display_name": "Wired", "current_ip": null, "manual_ips": [], "trusted": true,
                "connection_state": "Disconnected" });
        }
    } else if let Some(clients) = s["client_connections"].as_object_mut() {
        clients.remove(wired);
    }
    let ss = &mut s["session_settings"];
    // (socket buffers: ALVR's default, "Maximum", fails on macOS ("Error setting socket send buffer: Invalid
    //  argument") and leaves them at 128 KB; a Quest over Wi-Fi then saw frames held 270-800 ms (issue #4, and #2's
    //  session log). A fixed 4 MB is taken (macOS allows 8) and ended those spikes there.)
    for key in ["server_send_buffer_bytes", "server_recv_buffer_bytes"] {
        ss["connection"][key]["variant"] = "Custom".into();
        ss["connection"][key]["Custom"] = 4_000_000.into();
    }
    ss["video"]["preferred_codec"]["variant"] = "Hevc".into();
    ss["video"]["foveated_encoding"]["enabled"] = false.into();
    fs::write(&path, serde_json::to_string_pretty(&s).unwrap()).unwrap();
}

/// What the client sent last, in SharedHands' terms (driver_verantyx.cpp): column-major 4x4 matrices in SteamVR's
/// space, tracked flags 0 / 1, triggers and grips 0-255, button bits (right 0-4, left 8-12: A/X, B/Y, stick click,
/// stick touch, menu), sticks -1..1, eye tangents (left, right, bottom edge, top edge).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct VxAlvrInput {
    pub timestamp_s: f64,
    pub head: [f32; 16],
    pub left: [f32; 16],
    pub right: [f32; 16],
    pub left_velocity: [f32; 3],
    pub right_velocity: [f32; 3],
    pub left_angular_velocity: [f32; 3],
    pub right_angular_velocity: [f32; 3],
    pub left_stick: [f32; 2],
    pub right_stick: [f32; 2],
    pub left_tangents: [f32; 4],
    pub right_tangents: [f32; 4],
    pub right_buttons: u32,
    pub left_buttons: u32,
    pub head_tracked: u8,
    pub left_tracked: u8,
    pub right_tracked: u8,
    pub left_trigger: u8,
    pub right_trigger: u8,
    pub left_grip: u8,
    pub right_grip: u8,
    pub tangents_valid: u8,
}

// (HardwareEncoder.swift reads it at these offsets)
const _: () = {
    use std::mem::{offset_of, size_of};
    assert!(size_of::<VxAlvrInput>() == 312);
    assert!(offset_of!(VxAlvrInput, head) == 8 && offset_of!(VxAlvrInput, left) == 72 && offset_of!(VxAlvrInput, right) == 136);
    assert!(offset_of!(VxAlvrInput, left_velocity) == 200 && offset_of!(VxAlvrInput, right_velocity) == 212);
    assert!(offset_of!(VxAlvrInput, left_angular_velocity) == 224 && offset_of!(VxAlvrInput, right_angular_velocity) == 236);
    assert!(offset_of!(VxAlvrInput, left_stick) == 248 && offset_of!(VxAlvrInput, right_stick) == 256);
    assert!(offset_of!(VxAlvrInput, left_tangents) == 264 && offset_of!(VxAlvrInput, right_tangents) == 280);
    assert!(offset_of!(VxAlvrInput, right_buttons) == 296 && offset_of!(VxAlvrInput, left_buttons) == 300);
    assert!(offset_of!(VxAlvrInput, head_tracked) == 304 && offset_of!(VxAlvrInput, tangents_valid) == 311);
};

// The tracking samples' times and how the frames' stamps compare: logged every 5 s (session_log.txt). A stamp that is
// not a sample's time, or one far older than the newest, is a frame the client cannot place (it reprojects from it).
#[derive(Default)]
struct SampleLog {
    recent: std::collections::VecDeque<u64>,                           // (the last 2000 samples' times, ns)
    newest: u64,
    frames: u64, exact: u64, age_ns_sum: u64, age_ns_max: u64,
    since: Option<std::time::Instant>,
}
impl SampleLog {
    fn sample(&mut self, t: u64) {
        if self.recent.len() == 2000 { self.recent.pop_front(); }
        self.recent.push_back(t); self.newest = self.newest.max(t);
    }
    fn frame(&mut self, t: u64) {
        self.frames += 1;
        if self.recent.iter().rev().take(2000).any(|&x| x == t) { self.exact += 1; }
        let age = self.newest.saturating_sub(t);
        self.age_ns_sum += age; self.age_ns_max = self.age_ns_max.max(age);
        let since = *self.since.get_or_insert_with(std::time::Instant::now);
        if since.elapsed() > Duration::from_secs(5) && self.frames > 0 {
            alvr_common::info!("VeRantyx: {} frames in {:.1} s, {} stamped with a sample's exact time; stamp age (newest sample minus the stamp) mean {:.1} ms, max {:.1} ms",
                self.frames, since.elapsed().as_secs_f32(), self.exact, self.age_ns_sum as f64 / self.frames as f64 / 1e6, self.age_ns_max as f64 / 1e6);
            let line = HEADSET.lock().unwrap().line(since.elapsed().as_secs_f64());
            alvr_common::info!("VeRantyx: headset: {line}");
            if let Some((sent, poses, lead, from)) = HAND_SPEEDS.lock().unwrap().take() {
                alvr_common::info!("VeRantyx: controllers: fastest {sent:.1} m/s as the headset sent it, {poses:.1} m/s from the poses moved back {:.0} ms (the game gets {})",
                    lead * 1000.0, if from == VelocitySource::Poses { "the poses'" } else { "the headset's" });
            }
            *self = SampleLog { recent: std::mem::take(&mut self.recent), newest: self.newest, since: Some(std::time::Instant::now()), ..Default::default() };
        }
    }
}

// What reaches the headset. The ALVR client reports each frame it shows, once, when it shows it (ClientStatistics:
// its time on the client and the whole latency from the tracking sample to the display); ALVR's statistics turn
// each into a GraphStatistics event, which its logger hands only to its dashboard (session_log.txt gets an empty
// "[GRAPH]" line). There is no dashboard here: the bridge's logger (init_logging) counts them, and every 5 s the
// line above says how many frames the headset showed a second, against the Mac's frames sent. Lost video is not in
// ALVR 20's packet count (only an old client's VideoErrorReport counts there): the client asks for an IDR instead
// (and the core asks for one when it cannot push a frame to the network), so those are counted.
#[derive(Default)]
struct HeadsetLog {
    frames: u64, total_s: f64, mac_s: f64, network_s: f64, decode_s: f64, headset_s: f64,
    idr_requests: u64,
    bytes: u64, sent: u64,
    reported: bool,                                                    // (the client has sent statistics since it connected)
}
static HEADSET: Mutex<HeadsetLog> = Mutex::new(HeadsetLog {
    frames: 0, total_s: 0.0, mac_s: 0.0, network_s: 0.0, decode_s: 0.0, headset_s: 0.0, idr_requests: 0, bytes: 0, sent: 0, reported: false });
// (the same, since the encoder last asked: vx_alvr_link_trouble)
static LINK: Mutex<HeadsetLog> = Mutex::new(HeadsetLog {
    frames: 0, total_s: 0.0, mac_s: 0.0, network_s: 0.0, decode_s: 0.0, headset_s: 0.0, idr_requests: 0, bytes: 0, sent: 0, reported: false });
fn headset_logs(f: impl Fn(&mut HeadsetLog)) { f(&mut HEADSET.lock().unwrap()); f(&mut LINK.lock().unwrap()); }
impl HeadsetLog {
    fn line(&mut self, seconds: f64) -> String {
        let n = self.frames.max(1) as f64;
        let shown = if self.frames > 0 {
            format!("{:.1} fps shown, latency {:.0} ms (Mac {:.0} ms, network {:.0} ms, decode {:.0} ms, headset {:.0} ms)",
                self.frames as f64 / seconds, self.total_s / n * 1e3, self.mac_s / n * 1e3, self.network_s / n * 1e3,
                self.decode_s / n * 1e3, self.headset_s / n * 1e3)
        } else if self.reported || self.sent >= 30 {
            // (every ALVR 20 client reports what it shows; a frame later than ALVR's 256 tracking samples back, a second
            //  or so, is no longer matched: a link far slower than the bitrate leaves none reported)
            "0.0 fps shown".into()
        } else { "no statistics from the client yet".into() };
        let line = format!("{shown}, {} video losses (IDRs asked for), {:.1} Mbps sent", self.idr_requests, self.bytes as f64 * 8.0 / seconds / 1e6);
        *self = HeadsetLog { reported: self.reported, ..Default::default() };
        line
    }
}

// ALVR's logger (alvr_server_core::init_logging), line for line the same in session_log.txt and crash_log.txt, with
// the statistics kept (HeadsetLog) and their empty lines left out (one a frame).
struct Logger { session: Mutex<fs::File>, crash: Option<Mutex<fs::File>> }
impl alvr_common::log::Log for Logger {
    fn enabled(&self, m: &alvr_common::log::Metadata) -> bool {
        m.level() <= alvr_common::log::Level::Info && !m.target().starts_with("mdns_sd")
    }
    fn log(&self, record: &alvr_common::log::Record) {
        use alvr_events::{Event, EventType};
        use std::io::Write;
        if !self.enabled(record.metadata()) { return; }
        let text = format!("{}", record.args());
        let event_type = (text.starts_with('{') && text.ends_with('}')).then(|| serde_json::from_str::<EventType>(&text).ok()).flatten()
            .unwrap_or_else(|| EventType::Log(alvr_common::LogEntry { severity: alvr_common::LogSeverity::from_log_level(record.level()), content: text }));
        match &event_type {
            EventType::GraphStatistics(g) => return { note_latency(g.total_pipeline_latency_s); headset_logs(|h| {
                h.frames += 1; h.reported = true;
                // (Mac: from the tracking sample's arrival to the frame's encoding, which is all "encoder" to ALVR here:
                //  the bridge does not report present or composed; headset: waiting to be shown, its compositor, vsync)
                h.total_s += g.total_pipeline_latency_s as f64; h.network_s += g.network_s as f64; h.decode_s += g.decoder_s as f64;
                h.mac_s += (g.game_time_s + g.server_compositor_s + g.encoder_s) as f64;
                h.headset_s += (g.decoder_queue_s + g.client_compositor_s + g.vsync_queue_s) as f64;
            }) },
            EventType::StatisticsSummary(_) => return,
            _ => {}
        }
        let event = Event { timestamp: chrono::Local::now().format("%H:%M:%S.%3f").to_string(), event_type };
        let line = format!("{} [{}] {}\n", event.timestamp, event.event_type_string(), event.message());
        self.session.lock().unwrap().write_all(line.as_bytes()).ok();
        if let (Some(crash), alvr_common::log::Level::Error) = (&self.crash, record.level()) { crash.lock().unwrap().write_all(line.as_bytes()).ok(); }
    }
    fn flush(&self) {}
}

fn init_logging(session_log: PathBuf, crash_log: PathBuf) {
    let Ok(session) = fs::File::create(&session_log) else { return alvr_server_core::init_logging(Some(session_log), Some(crash_log)) };
    let crash = fs::OpenOptions::new().create(true).append(true).open(crash_log).ok().map(Mutex::new);
    if alvr_common::log::set_boxed_logger(Box::new(Logger { session: Mutex::new(session), crash })).is_ok() {
        alvr_common::log::set_max_level(alvr_common::log::LevelFilter::Info);
    }
    alvr_common::set_panic_hook();
}

struct Bridge {
    ctx: RwLock<Option<ServerCoreContext>>,
    input: Mutex<VxAlvrInput>,
    seq: AtomicU64,
    connected: AtomicBool,
    idr_requested: AtomicBool,
    config: Mutex<Vec<u8>>,
    samples: Mutex<SampleLog>,                                         // (what frames are stamped with, checked)
    layout: alvr_filesystem::Layout,
    started_with: serde_json::Value,                                   // (the display settings the core read)
    connecting_since: Mutex<Option<std::time::Instant>>,               // (a headset's handshake, as the session shows it)
}
static BRIDGE: OnceLock<Arc<Bridge>> = OnceLock::new();
static LAST_VIEWS: Mutex<String> = Mutex::new(String::new());
static IPD_BITS: AtomicU32 = AtomicU32::new(0);                        // (the eyes' distance in metres, f32; 0: none)

fn matrix(m: Option<DeviceMotion>) -> Option<([f32; 16], [f32; 3], [f32; 3])> {
    let m = m?;
    let r = alvr_common::glam::Mat4::from_rotation_translation(m.pose.orientation, m.pose.position);
    Some((r.to_cols_array(), m.linear_velocity.to_array(), m.angular_velocity.to_array()))
}
// A hand the client tracks as a hand (its skeleton) rather than as a controller: ALVR then drops that hand's
// controller motion, so the hand vanished in the game whenever the Vision Pro's ALVR app had gone over to hand
// tracking (the Joy-Con untouched for a while). Its pose is then the palm moved by ALVR's default hand-tracking offset,
// as ALVR's own SteamVR driver places a tracked hand (server_openvr tracking.rs, to_openvr_ffi_hand_skeleton), so it
// sits where the controller would.
fn hand_motion(ctx: &ServerCoreContext, hand: HandType, t: Duration) -> Option<DeviceMotion> {
    let (offset, turn) = ([0.04f32, -0.02, -0.13], [0.0f32, -45.0, -90.0]);   // (ALVR's left hand; the right mirrored)
    let (sign, d) = (if matches!(hand, HandType::Left) { 1.0 } else { -1.0 }, std::f32::consts::PI / 180.0);
    let palm = ctx.get_hand_skeleton(hand, t)?[0];
    let o = Quat::from_euler(EulerRot::XYZ, turn[0] * d, sign * turn[1] * d, sign * turn[2] * d);
    let at = Vec3::new(sign * offset[0], offset[1], offset[2]);
    Some(DeviceMotion {
        pose: Pose { orientation: palm.orientation * o, position: palm.position + palm.orientation * o * at },
        linear_velocity: Vec3::ZERO, angular_velocity: Vec3::ZERO,
    })
}
fn tangents(f: &Fov) -> [f32; 4] { [f.left.tan(), f.right.tan(), f.down.tan(), f.up.tan()] }

// The controllers' velocities, which SteamVR passes to the game (Half-Life: Alyx throws with them). ALVR sends one
// with each controller pose, but on a Pico, VIVE or YVR headset it is not the runtime's: ALVR's client (client_openxr
// interaction.rs, get_hand_data) works around those runtimes' velocities by asking for the controller's pose a whole
// pipeline latency ahead (its average, from the tracking sample to the display) and dividing how far that is from the
// pose now by that time. A PC with ALVR's SteamVR driver has some 50-80 ms of it; VeRantyx on a Mac has 150-250 ms
// (the session log's "latency"), and a pose predicted that far ahead of a hand that is speeding up or turning in a
// throw is far off: Half-Life: Alyx threw things as from a cannon (a Pico 4 Ultra with its controllers, 0.2.2). For
// those headsets the velocities come from the poses themselves instead: each sample's pose against the one before,
// over the time between their samples (the client's clock, so samples that arrive together still count right).
// The poses are not quite the measured ones either: ALVR's client moves each controller pose ahead by its velocity
// (client_core lib.rs, send_tracking: the average latency less SteamVR's pipeline, steamvr_pipeline_frames frames,
// at most max_prediction_ms, 100 ms), so their differences would carry that velocity's errors on, times that lead.
// The same lead is worked out here (client_lead, from the latencies the client reports) and each pose moved back by
// the velocity it came with (unpredict) before the difference is taken. VERANTYX_ALVR_VELOCITY=headset or =poses
// chooses for any headset.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum VelocitySource { Headset, Poses }

/// Which velocities the game gets, for the connected headsets' names (ALVR's display names, which are the platform:
/// "Pico 4 Ultra", "Quest 3", "Apple Headset") and VERANTYX_ALVR_VELOCITY.
pub fn velocity_source(headsets: &[String], setting: Option<&str>) -> VelocitySource {
    match setting.map(|s| s.trim().to_ascii_lowercase()).as_deref() {
        Some("headset") => return VelocitySource::Headset,
        Some("poses") => return VelocitySource::Poses,
        _ => {}
    }
    // (the platforms for which ALVR's client predicts instead: Platform::is_pico, is_vive, is_yvr in alvr_system_info)
    let extrapolated = |n: &str| ["Pico", "VIVE", "HTC VIVE", "YVR", "Play For Dream"].iter().any(|p| n.starts_with(p));
    if headsets.iter().any(|n| extrapolated(n)) { VelocitySource::Poses } else { VelocitySource::Headset }
}

/// How far ahead ALVR's client moved the controllers' poses, in seconds: its average latency from the tracking sample
/// to the display less SteamVR's pipeline (steamvr_pipeline_frames at the display rate), at most max_prediction_ms
/// (client_core statistics.rs, tracker_prediction_offset; lib.rs, send_tracking).
pub fn client_lead(average_latency_s: f32, pipeline_frames: f32, refresh_hz: f32, max_prediction_ms: u64) -> f32 {
    let pipeline = if refresh_hz > 0.0 { pipeline_frames / refresh_hz } else { 0.0 };
    (average_latency_s - pipeline).clamp(0.0, max_prediction_ms as f32 / 1000.0)
}

/// A controller's pose as ALVR's core hands it on, moved back by the lead the client moved it ahead with (its linear
/// velocity in the tracking space, its angular one in the controller's frame, as the core turns it).
pub fn unpredict(m: &DeviceMotion, lead_s: f32) -> (Vec3, Quat) {
    let turn = m.pose.orientation * m.angular_velocity;                // (in the tracking space)
    (m.pose.position - m.linear_velocity * lead_s, (Quat::from_scaled_axis(-turn * lead_s) * m.pose.orientation).normalize())
}

// The latencies the client reports (seconds, one per frame shown: the logger's GraphStatistics), the last
// statistics_history_size of them as the client averages them, and what the lead is worked out with.
static LATENCIES: Mutex<std::collections::VecDeque<f32>> = Mutex::new(std::collections::VecDeque::new());
static LATENCY_WINDOW: AtomicU64 = AtomicU64::new(256);              // (statistics_history_size, set on connecting:
fn note_latency(seconds: f32) {                                        //  not read here, the logger may run under
    let window = LATENCY_WINDOW.load(Ordering::Relaxed).max(1) as usize; //  the session's lock)
    let mut l = LATENCIES.lock().unwrap();
    l.push_back(seconds);
    while l.len() > window { l.pop_front(); }
}
fn average_latency() -> f32 {
    let l = LATENCIES.lock().unwrap();
    if l.is_empty() { 0.0 } else { l.iter().sum::<f32>() / l.len() as f32 }
}

/// A hand's velocity from its successive poses (at their sample times in seconds), both in the tracking space.
#[derive(Default)]
pub struct PoseVelocity {
    last: Option<(f64, Vec3, Quat)>,
    linear: Vec3,
    angular: Vec3,                                                     // (in the tracking space)
}
impl PoseVelocity {
    const SMOOTHING: f32 = 0.6;                                        // (of each new difference; the poses are smooth)
    const MIN_DT: f64 = 0.004;                                         // (closer samples: measured against an older one)
    const MAX_DT: f64 = 0.1;                                           // (a longer gap: the hand was lost, no motion)
    const MAX_SPEED: f32 = 20.0;                                       // (m/s: faster is a tracking jump, not a hand)
    const MAX_TURN: f32 = 60.0;                                        // (rad/s)

    pub fn reset(&mut self) { *self = Self::default(); }

    pub fn update(&mut self, t: f64, position: Vec3, orientation: Quat) -> (Vec3, Vec3) {
        match self.last {
            Some((t0, ..)) if t == t0 || (t > t0 && t - t0 < Self::MIN_DT) => return self.output(),
            Some((t0, p0, q0)) if t > t0 && t - t0 <= Self::MAX_DT => {
                let dt = (t - t0) as f32;
                let linear = (position - p0) / dt;
                if linear.is_finite() && linear.length() < Self::MAX_SPEED { self.linear += (linear - self.linear) * Self::SMOOTHING; }
                let mut turn = (orientation * q0.inverse()).normalize();
                if turn.w < 0.0 { turn = -turn; }                       // (the shorter way round)
                let angular = turn.to_scaled_axis() / dt;
                if angular.is_finite() && angular.length() < Self::MAX_TURN { self.angular += (angular - self.angular) * Self::SMOOTHING; }
            }
            _ => { self.linear = Vec3::ZERO; self.angular = Vec3::ZERO; }   // (the first, after a gap, or an older clock)
        }
        self.last = Some((t, position, orientation));
        self.output()
    }

    fn output(&self) -> (Vec3, Vec3) { (self.linear, self.angular) }
}

// The hands' fastest speeds since the last 5 s report (SampleLog), as the headset sent them and from the poses, m/s,
// and the client's lead (s).
static HAND_SPEEDS: Mutex<Option<(f32, f32, f32, VelocitySource)>> = Mutex::new(None);

fn connected_headsets(layout: &alvr_filesystem::Layout) -> Vec<String> {
    fs::read_to_string(layout.session()).ok().and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .and_then(|s| s["client_connections"].as_object().map(|c| c.values()
            .filter(|v| v["connection_state"] == "Streaming")
            .filter_map(|v| v["display_name"].as_str().map(String::from)).collect()))
        .unwrap_or_default()
}

fn apply_buttons(input: &mut VxAlvrInput, values: &HashMap<u64, f32>) {
    let v = |id: u64| values.get(&id).copied().unwrap_or(0.0);
    let bit = |id: u64, b: u32| if v(id) > 0.5 { 1u32 << b } else { 0 };
    input.right_buttons = bit(*RIGHT_A_CLICK_ID, 0) | bit(*RIGHT_B_CLICK_ID, 1) | bit(*RIGHT_THUMBSTICK_CLICK_ID, 2)
        | bit(*RIGHT_THUMBSTICK_TOUCH_ID, 3) | bit(*RIGHT_SYSTEM_CLICK_ID, 4);
    input.left_buttons = (bit(*LEFT_X_CLICK_ID, 0) | bit(*LEFT_Y_CLICK_ID, 1) | bit(*LEFT_THUMBSTICK_CLICK_ID, 2)
        | bit(*LEFT_THUMBSTICK_TOUCH_ID, 3) | bit(*LEFT_MENU_CLICK_ID, 4)) << 8;
    let u8v = |id: u64| (v(id).clamp(0.0, 1.0) * 255.0).round() as u8;
    input.right_trigger = u8v(*RIGHT_TRIGGER_VALUE_ID); input.left_trigger = u8v(*LEFT_TRIGGER_VALUE_ID);
    input.right_grip = u8v(*RIGHT_SQUEEZE_VALUE_ID); input.left_grip = u8v(*LEFT_SQUEEZE_VALUE_ID);
    input.right_stick = [v(*RIGHT_THUMBSTICK_X_ID), v(*RIGHT_THUMBSTICK_Y_ID)];
    input.left_stick = [v(*LEFT_THUMBSTICK_X_ID), v(*LEFT_THUMBSTICK_Y_ID)];
}

fn openvr_config(layout: &alvr_filesystem::Layout) -> serde_json::Value {
    fs::read_to_string(layout.session()).ok().and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .map(|s| s["openvr_config"].clone()).unwrap_or_default()
}

// A headset whose display (view size, rate, codec) differs from the last one makes ALVR store the new settings and
// ask for its host (SteamVR, for ALVR) to be restarted, which ALVR's dashboard does; meanwhile it turns the headset
// away. Here the host is the encoder: vx_alvr_restart_needed says so, the encoder exits with 75 and
// scripts/launch_vxrt_steamvr.sh starts it again, which reads the stored settings and connects. (Making the core anew
// in the process does not do: dropping it writes default settings back over the headset's.)
fn run(bridge: Arc<Bridge>) {
    let (ctx, receiver) = ServerCoreContext::new();
    let config = bridge.config.lock().unwrap().clone();                // (parameter sets given before the core was up)
    if !config.is_empty() { ctx.set_video_config_nals(config, CodecType::Hevc); }
    ctx.start_connection();
    *bridge.ctx.write().unwrap() = Some(ctx);
    events(&bridge, receiver);
}

fn events(bridge: &Bridge, receiver: std::sync::mpsc::Receiver<ServerCoreEvent>) {
    let mut buttons: HashMap<u64, f32> = HashMap::new();
    let mut velocity_from = VelocitySource::Headset;
    let mut pose_velocity = [PoseVelocity::default(), PoseVelocity::default()];
    let mut lead_settings = (2.1f32, 90.0f32, 100u64);                 // (steamvr_pipeline_frames, display rate, max_prediction_ms)
    while let Ok(e) = receiver.recv() {
        match e {
            ServerCoreEvent::ClientConnected => {
                bridge.connected.store(true, Ordering::SeqCst); bridge.idr_requested.store(true, Ordering::SeqCst);
                headset_logs(|h| *h = HeadsetLog::default());
                let headsets = connected_headsets(&bridge.layout);
                velocity_from = velocity_source(&headsets, env::var("VERANTYX_ALVR_VELOCITY").ok().as_deref());
                pose_velocity.iter_mut().for_each(PoseVelocity::reset);
                let settings = alvr_server_core::settings();
                lead_settings = (settings.headset.controllers.as_option().map_or(0.0, |c| c.steamvr_pipeline_frames),
                    openvr_config(&bridge.layout)["refresh_rate"].as_f64().unwrap_or(90.0) as f32, settings.headset.max_prediction_ms);
                LATENCY_WINDOW.store(settings.connection.statistics_history_size as u64, Ordering::Relaxed);
                LATENCIES.lock().unwrap().clear();
                alvr_common::info!("VeRantyx: {:?} connected; the controllers' velocities come from {}", headsets,
                    if velocity_from == VelocitySource::Poses { "their poses (sample to sample, moved back by the client's lead)" } else { "the headset" });
            }
            ServerCoreEvent::ClientDisconnected => bridge.connected.store(false, Ordering::SeqCst),
            ServerCoreEvent::RequestIDR => { bridge.idr_requested.store(true, Ordering::SeqCst); headset_logs(|h| h.idr_requests += 1); }
            ServerCoreEvent::ViewsConfig(v) => {
                // (the visionOS client sends an infinite field of view for its first seconds: kept out, as the driver
                //  would latch a fallback frustum if the map held one it rejects)
                let sane = |f: &Fov| [f.left, f.right, f.up, f.down].iter().all(|a| a.is_finite() && a.abs() < 1.5)
                    && f.left < 0.0 && f.right > 0.0 && f.up > 0.0 && f.down < 0.0;
                {
                    let t = &v.local_view_transforms;
                    let line = format!("eyes: left at ({:.4}, {:.4}, {:.4}) turned {:?}, right at ({:.4}, {:.4}, {:.4}) turned {:?}; fov {:?} / {:?}",
                        t[0].position.x, t[0].position.y, t[0].position.z, t[0].orientation, t[1].position.x, t[1].position.y, t[1].position.z, t[1].orientation, v.fov[0], v.fov[1]);
                    let mut last = LAST_VIEWS.lock().unwrap();
                    if *last != line { alvr_common::info!("VeRantyx: the client's {line}"); *last = line; }
                    let ipd = (t[1].position - t[0].position).length();
                    if (0.04..0.09).contains(&ipd) { IPD_BITS.store(ipd.to_bits(), Ordering::Relaxed); }   // (the mock sends 0)
                }
                if sane(&v.fov[0]) && sane(&v.fov[1]) {
                    let mut i = bridge.input.lock().unwrap();
                    i.left_tangents = tangents(&v.fov[0]); i.right_tangents = tangents(&v.fov[1]); i.tangents_valid = 1;
                }
            }
            ServerCoreEvent::Buttons(entries) => {
                for b in entries {
                    buttons.insert(b.path_id, match b.value { ButtonValue::Binary(x) => x as u8 as f32, ButtonValue::Scalar(x) => x });
                }
                apply_buttons(&mut bridge.input.lock().unwrap(), &buttons);
                bridge.seq.fetch_add(1, Ordering::SeqCst);
            }
            ServerCoreEvent::Tracking { sample_timestamp } => {
                let (head, mut hands) = {
                    let guard = bridge.ctx.read().unwrap();
                    let Some(ctx) = guard.as_ref() else { continue };
                    let t = sample_timestamp;
                    let hand = |id: u64, h: HandType| ctx.get_device_motion(id, t).map(|m| (m, true)).or_else(|| hand_motion(ctx, h, t).map(|m| (m, false)));
                    (matrix(ctx.get_device_motion(*HEAD_ID, t)), [hand(*HAND_LEFT_ID, HandType::Left), hand(*HAND_RIGHT_ID, HandType::Right)])
                };
                // (a controller's velocities, from its poses when the headset's are a long prediction: velocity_source)
                {
                    let lead = client_lead(average_latency(), lead_settings.0, lead_settings.1, lead_settings.2);
                    let mut speeds = HAND_SPEEDS.lock().unwrap();
                    for (i, h) in hands.iter_mut().enumerate() {
                        let Some((m, true)) = h.as_mut() else { pose_velocity[i].reset(); continue };
                        let (position, orientation) = unpredict(m, lead);
                        let (linear, turn) = pose_velocity[i].update(sample_timestamp.as_secs_f64(), position, orientation);
                        let s = speeds.get_or_insert((0.0, 0.0, 0.0, velocity_from));
                        *s = (s.0.max(m.linear_velocity.length()), s.1.max(linear.length()), lead, velocity_from);
                        // (angular in the controller's frame, of the pose SteamVR gets: as ALVR's core hands it on)
                        if velocity_from == VelocitySource::Poses { m.linear_velocity = linear; m.angular_velocity = m.pose.orientation.conjugate() * turn; }
                    }
                }
                let [left, right] = hands.map(|h| matrix(h.map(|(m, _)| m)));
                bridge.samples.lock().unwrap().sample(sample_timestamp.as_nanos() as u64);
                let mut i = bridge.input.lock().unwrap();
                i.timestamp_s = sample_timestamp.as_secs_f64();
                if let Some((m, _, _)) = head { i.head = m; }
                i.head_tracked = head.is_some() as u8;
                if let Some((m, lv, av)) = left { i.left = m; i.left_velocity = lv; i.left_angular_velocity = av; }
                if let Some((m, lv, av)) = right { i.right = m; i.right_velocity = lv; i.right_angular_velocity = av; }
                i.left_tracked = left.is_some() as u8; i.right_tracked = right.is_some() as u8;
                drop(i);
                bridge.seq.fetch_add(1, Ordering::SeqCst);
            }
            _ => {}
        }
    }
}

/// Starts ALVR's core (its session in config_dir, or ~/Library/Application Support/VeRantyx/ALVR when null) and its
/// event thread. Once per process.
#[no_mangle]
pub unsafe extern "C" fn vx_alvr_start(config_dir: *const c_char) -> bool {
    if BRIDGE.get().is_some() { return true; }
    let dir = (!config_dir.is_null()).then(|| PathBuf::from(CStr::from_ptr(config_dir).to_string_lossy().into_owned()));
    let l = layout(dir);
    prepare_session(&l);
    alvr_server_core::initialize_environment(l.clone());
    init_logging(l.session_log(), l.crash_log());
    let identity = alvr_common::glam::Mat4::from_rotation_translation(Quat::IDENTITY, Vec3::new(0.0, 1.6, 0.0)).to_cols_array();
    let bridge = Arc::new(Bridge {
        ctx: RwLock::new(None), input: Mutex::new(VxAlvrInput { head: identity, left: identity, right: identity, ..Default::default() }),
        seq: AtomicU64::new(0), connected: AtomicBool::new(false), idr_requested: AtomicBool::new(false), config: Mutex::new(vec![]), samples: Mutex::new(SampleLog::default()),
        started_with: openvr_config(&l), layout: l, connecting_since: Mutex::new(None),
    });
    BRIDGE.set(Arc::clone(&bridge)).ok();
    thread::spawn(move || run(bridge));
    true
}

/// The newest input, if newer than last_seq: copies it to out and returns its sequence number (else last_seq).
#[no_mangle]
pub unsafe extern "C" fn vx_alvr_input(out: *mut VxAlvrInput, last_seq: u64) -> u64 {
    let Some(b) = BRIDGE.get() else { return last_seq };
    let seq = b.seq.load(Ordering::SeqCst);
    if seq == last_seq || out.is_null() { return last_seq; }
    *out = *b.input.lock().unwrap();
    seq
}

#[no_mangle]
pub extern "C" fn vx_alvr_connected() -> bool { BRIDGE.get().is_some_and(|b| b.connected.load(Ordering::SeqCst)) }

/// True once per IDR the client asked for (or on connecting).
#[no_mangle]
pub extern "C" fn vx_alvr_take_idr_request() -> bool { BRIDGE.get().is_some_and(|b| b.idr_requested.swap(false, Ordering::SeqCst)) }

// A headset that connects again can find ALVR's core stuck for good in its handshake (issue #6: the headset says
// "Connection error: Try again (os error 11)", or 35 on a Mac, and ALVR logs nothing more). ALVR's game audio
// callback reads the session lock on CoreAudio's thread; cpal 0.15.3 never stops a stream on a named device (its
// disconnect listener keeps the stream alive), so the last connection's recording goes on, and the next handshake holds
// that lock for writing while it asks CoreAudio about the same device, which waits for that callback. A handshake takes
// well under a second and ALVR gives up on each of its steps after 2 s, so a client still "Connecting" after
// HANDSHAKE_HUNG means the core is stuck: the encoder starts again, as after new display settings, and the headset
// connects to the new one. (Whether a client still counts as connected does not matter: when the last connection's
// recording was being stopped as the lock was taken, its end hangs too and ALVR never says it ended.)
const HANDSHAKE_HUNG: Duration = Duration::from_secs(6);

fn handshake_hung(b: &Bridge) -> bool {
    let connecting = fs::read_to_string(b.layout.session()).ok().and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .and_then(|s| s["client_connections"].as_object().map(|c| c.values().any(|v| v["connection_state"] == "Connecting")))
        .unwrap_or(false);
    let mut since = b.connecting_since.lock().unwrap();
    if !connecting { *since = None; return false; }
    let hung = since.get_or_insert_with(std::time::Instant::now).elapsed() > HANDSHAKE_HUNG;
    // (to the encoder's log: the core started again writes session_log.txt anew)
    if hung { eprintln!("ALVR: a headset's handshake has not ended in {} s (ALVR's core is stuck)", HANDSHAKE_HUNG.as_secs()); }
    hung
}

/// True when the host has to start again: a headset brought display settings other than those the core started with
/// (see run), or a handshake hangs (handshake_hung).
#[no_mangle]
pub extern "C" fn vx_alvr_restart_needed() -> bool {
    BRIDGE.get().is_some_and(|b| (!b.connected.load(Ordering::SeqCst) && openvr_config(&b.layout) != b.started_with) || handshake_hung(b))
}

/// The link's trouble since the last call, as loss events for the encoder's adaptive bitrate (HardwareEncoder.swift,
/// AdaptiveBitrate, which lowers the bitrate at two in a period): the video lost (IDRs asked for), and two more when the
/// frames spent over 80 ms on the network (some 20-40 ms over a home Wi-Fi), or the headset showed none of them. A link
/// slower than the bitrate loses nothing over TCP: the frames queue (ALVR keeps up to 1024) and the stream falls behind,
/// until they are too late to be counted at all. (Not the share of the frames shown: a client drops some of its own when
/// they come in bursts, and that is no reason for a lower bitrate.)
#[no_mangle]
pub extern "C" fn vx_alvr_link_trouble() -> u32 {
    let mut l = LINK.lock().unwrap();
    let reported = l.reported;
    let t = std::mem::replace(&mut *l, HeadsetLog { reported, ..Default::default() });
    let mut events = t.idr_requests as u32;
    if t.sent >= 30 {
        let network = if t.frames > 0 { t.network_s / t.frames as f64 } else { 1.0 };
        if network > 0.08 { events += 2; }
    }
    events
}

/// The client's display rate (ALVR's session, as negotiated at the last connection), 0 before any.
#[no_mangle]
pub extern "C" fn vx_alvr_refresh_hz() -> f32 {
    let path = layout(None).session();
    fs::read_to_string(path).ok().and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .and_then(|s| s["openvr_config"]["refresh_rate"].as_f64()).unwrap_or(0.0) as f32
}

// Vibrations go to ALVR's core on a thread of their own. ALVR's send_haptics reads the session's lock, which a hung
// handshake holds for writing for good (issue #6, HANDSHAKE_HUNG): called on the encoder's ALVR thread, the first
// vibration the game asked for while a headset reconnected stopped that thread, which is also the one that asks
// vx_alvr_restart_needed, so the encoder never started again and the headset stayed at "Try again (os error 11)"
// (a Pico 4 Ultra tester, 0.3.0, leaving and reopening ALVR in the middle of Half-Life: Alyx). Now they queue (64
// at most; more are dropped while the core cannot take them) and the caller never waits.
pub struct Queue<T: Send + 'static> { sender: std::sync::mpsc::SyncSender<T>, dropped: AtomicU64 }
impl<T: Send + 'static> Queue<T> {
    pub fn new(capacity: usize, mut take: impl FnMut(T) + Send + 'static) -> Self {
        let (sender, receiver) = std::sync::mpsc::sync_channel::<T>(capacity);
        thread::spawn(move || { while let Ok(item) = receiver.recv() { take(item) } });
        Queue { sender, dropped: AtomicU64::new(0) }
    }
    /// Never waits; false (and counted) when the queue is full.
    pub fn push(&self, item: T) -> bool {
        let ok = self.sender.try_send(item).is_ok();
        if !ok { self.dropped.fetch_add(1, Ordering::Relaxed); }
        ok
    }
    pub fn dropped(&self) -> u64 { self.dropped.load(Ordering::Relaxed) }
}

fn send_haptics(hand: u32, duration_s: f32, frequency: f32, amplitude: f32) {
    static PULSES: AtomicU64 = AtomicU64::new(0);
    let Some(b) = BRIDGE.get() else { return };
    let guard = b.ctx.read().unwrap();
    let Some(ctx) = guard.as_ref() else { return };
    let device_id = if hand == 0 { *HAND_LEFT_ID } else { *HAND_RIGHT_ID };
    ctx.send_haptics(Haptics { device_id, duration: Duration::from_secs_f32(duration_s.clamp(0.0, 5.0)), frequency, amplitude });
    let n = PULSES.fetch_add(1, Ordering::Relaxed) + 1;
    if n <= 3 || n % 500 == 0 {
        alvr_common::info!("VeRantyx: vibration #{n} to the {} hand: {duration_s:.3} s, {frequency:.0} Hz, amplitude {amplitude:.2}", if hand == 0 { "left" } else { "right" });
    }
}

/// A vibration the game asked for (hand 0 left, 1 right), to the headset's controller through ALVR, which scales it
/// by its haptics settings (on by default). Never waits (Queue).
#[no_mangle]
pub extern "C" fn vx_alvr_haptics(hand: u32, duration_s: f32, frequency: f32, amplitude: f32) {
    static HAPTICS: OnceLock<Queue<(u32, f32, f32, f32)>> = OnceLock::new();
    let queue = HAPTICS.get_or_init(|| Queue::new(64, |(h, d, f, a)| send_haptics(h, d, f, a)));
    if !queue.push((hand, duration_s, frequency, amplitude)) && queue.dropped() % 100 == 1 {
        eprintln!("ALVR: {} vibrations dropped: ALVR's core is not taking them (a headset's handshake may hang)", queue.dropped());
    }
}

/// The distance between the headset's eyes, in metres, as its client reports it (0 before it has).
#[no_mangle]
pub extern "C" fn vx_alvr_ipd() -> f32 { f32::from_bits(IPD_BITS.load(Ordering::Relaxed)) }

/// The HEVC parameter sets (VPS SPS PPS, Annex-B), before the first frame and whenever they change.
#[no_mangle]
pub unsafe extern "C" fn vx_alvr_set_config(nals: *const u8, len: usize) {
    if let (Some(b), false) = (BRIDGE.get(), nals.is_null()) {
        let config = std::slice::from_raw_parts(nals, len).to_vec();
        *b.config.lock().unwrap() = config.clone();                  // (a core made anew gets them too)
        if let Some(ctx) = b.ctx.read().unwrap().as_ref() { ctx.set_video_config_nals(config, CodecType::Hevc); }
    }
}

/// One access unit (Annex-B), rendered for the pose of the tracking sample at timestamp_s (VxAlvrInput.timestamp_s).
#[no_mangle]
pub unsafe extern "C" fn vx_alvr_send(timestamp_s: f64, au: *const u8, len: usize, is_idr: bool) {
    if let (Some(b), false) = (BRIDGE.get(), au.is_null()) {
        let t = Duration::from_nanos((timestamp_s * 1e9).round() as u64);
        headset_logs(|h| { h.bytes += len as u64; h.sent += 1; });
        b.samples.lock().unwrap().frame(t.as_nanos() as u64);
        if let Some(ctx) = b.ctx.read().unwrap().as_ref() { ctx.send_video_nal(t, std::slice::from_raw_parts(au, len).to_vec(), is_idr); }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const HZ72: f64 = 1.0 / 72.0;

    fn close(a: Vec3, b: Vec3, tolerance: f32) -> bool { (a - b).length() <= tolerance }

    #[test]
    fn velocities_from_poses_for_the_headsets_alvr_extrapolates() {
        let names = |n: &[&str]| n.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(velocity_source(&names(&["Pico 4 Ultra"]), None), VelocitySource::Poses);
        assert_eq!(velocity_source(&names(&["Pico (unknown)"]), None), VelocitySource::Poses);
        assert_eq!(velocity_source(&names(&["VIVE Focus Vision"]), None), VelocitySource::Poses);
        assert_eq!(velocity_source(&names(&["YVR"]), None), VelocitySource::Poses);
        assert_eq!(velocity_source(&names(&["Quest 3"]), None), VelocitySource::Headset);
        assert_eq!(velocity_source(&names(&["Apple Headset"]), None), VelocitySource::Headset);
        assert_eq!(velocity_source(&[], None), VelocitySource::Headset);
        assert_eq!(velocity_source(&names(&["Pico 4 Ultra"]), Some("headset")), VelocitySource::Headset);
        assert_eq!(velocity_source(&names(&["Quest 3"]), Some(" Poses ")), VelocitySource::Poses);
        assert_eq!(velocity_source(&names(&["Pico 4"]), Some("auto")), VelocitySource::Poses);
    }

    #[test]
    fn the_streaming_headsets_name_from_the_session() {
        let dir = std::env::temp_dir().join(format!("vx-alvr-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let l = layout(Some(dir.clone()));
        fs::write(l.session(), r#"{"client_connections": {
            "a.client.alvr": {"display_name": "Quest 3", "connection_state": "Disconnected"},
            "b.client.alvr": {"display_name": "Pico 4 Ultra", "connection_state": "Streaming"}}}"#).unwrap();
        let headsets = connected_headsets(&l);
        fs::remove_dir_all(&dir).ok();
        assert_eq!(headsets, vec!["Pico 4 Ultra".to_string()]);
        assert_eq!(velocity_source(&headsets, None), VelocitySource::Poses);
    }

    #[test]
    fn the_clients_lead() {
        // 190 ms of latency, 2.1 frames of SteamVR pipeline at 72 Hz (29 ms): 161 ms, but at most 100 ms
        assert!((client_lead(0.190, 2.1, 72.0, 100) - 0.100).abs() < 1e-6);
        assert!((client_lead(0.080, 2.1, 72.0, 100) - (0.080 - 2.1 / 72.0)).abs() < 1e-6);
        assert_eq!(client_lead(0.010, 2.1, 72.0, 100), 0.0);
        assert_eq!(client_lead(0.0, 2.1, 0.0, 100), 0.0);
    }

    // What ALVR does to a controller's motion on its way: the client moves the pose ahead by the velocity it sends
    // (DeviceMotion::predict, send_tracking), the core turns the angular velocity into the controller's frame.
    fn as_alvr_hands_it_on(position: Vec3, orientation: Quat, linear: Vec3, turn: Vec3, lead: f32) -> DeviceMotion {
        let sent = DeviceMotion { pose: Pose { orientation, position }, linear_velocity: linear, angular_velocity: turn }
            .predict(Duration::from_secs(10), Duration::from_secs(10) + Duration::from_secs_f32(lead));
        DeviceMotion { angular_velocity: sent.pose.orientation.conjugate() * sent.angular_velocity, ..sent }
    }

    #[test]
    fn a_pose_moved_back_by_the_clients_lead() {
        let (position, orientation) = (Vec3::new(0.2, 1.1, -0.4), Quat::from_euler(EulerRot::YXZ, 0.7, -0.3, 0.2));
        let m = as_alvr_hands_it_on(position, orientation, Vec3::new(3.0, 1.0, -4.0), Vec3::new(2.0, -6.0, 1.0), 0.1);
        let (p, q) = unpredict(&m, 0.1);
        assert!(close(p, position, 1e-4), "{p:?}");
        assert!(q.angle_between(orientation) < 1e-3, "{q:?}");
    }

    #[test]
    fn a_throw_with_wrong_velocities_from_the_headset() {
        // the hand speeds up at 40 m/s² for 0.15 s and stops; the headset's velocity runs 0.1 s ahead of the hand and
        // twice too fast (as a prediction over the whole latency might), and the client moved each pose ahead by it
        let (a, release, lead) = (40.0f32, 0.15f64, 0.1f32);
        let hand = |t: f64| { let tt = t.clamp(0.0, release) as f32; (Vec3::new(0.5 * a * tt * tt, 1.2, -0.3), Vec3::new(a * tt * (t <= release) as u8 as f32, 0.0, 0.0)) };
        let mut moved_back = PoseVelocity::default();
        let mut as_sent = PoseVelocity::default();
        let (mut fastest, mut fastest_as_sent, mut fastest_headset) = (0.0f32, 0.0f32, 0.0f32);
        for i in 0..40 {
            let t = i as f64 * HZ72;
            let (position, _) = hand(t);
            let wrong = hand(t + lead as f64).1 * 2.0;
            let m = as_alvr_hands_it_on(position, Quat::IDENTITY, wrong, Vec3::ZERO, lead);
            let (p, q) = unpredict(&m, lead);
            fastest = fastest.max(moved_back.update(t, p, q).0.length());
            fastest_as_sent = fastest_as_sent.max(as_sent.update(t, m.pose.position, m.pose.orientation).0.length());
            fastest_headset = fastest_headset.max(m.linear_velocity.length());
        }
        assert!(fastest_headset > 11.0, "{fastest_headset}");
        assert!(fastest_as_sent > 8.0, "the poses as sent carry the wrong velocity on: {fastest_as_sent}");
        assert!(fastest <= 6.05 && fastest > 4.5, "moved back, they move as the hand did (6 m/s): {fastest}");
    }

    #[test]
    fn a_queue_that_never_waits() {
        // the taker stuck (as ALVR's send_haptics is behind a hung handshake's lock): pushing still returns at once
        let (stuck_tx, stuck_rx) = std::sync::mpsc::channel::<()>();
        let stuck = Mutex::new(stuck_rx);
        let (took_tx, took_rx) = std::sync::mpsc::channel::<u32>();
        let q = Queue::new(4, move |n: u32| { took_tx.send(n).ok(); stuck.lock().unwrap().recv().ok(); });
        let started = std::time::Instant::now();
        let pushed = (0..100).filter(|&n| q.push(n)).count();
        assert!(started.elapsed() < Duration::from_millis(100), "{:?}", started.elapsed());
        assert!((4..=5).contains(&pushed), "{pushed} (the taker holds one, the queue four)");
        assert_eq!(q.dropped(), 100 - pushed as u64);
        assert_eq!(took_rx.recv_timeout(Duration::from_secs(1)).unwrap(), 0);
        for _ in 0..pushed { stuck_tx.send(()).unwrap(); }            // (unstuck: the queued ones go on, in order)
        let rest: Vec<u32> = (1..pushed).map(|_| took_rx.recv_timeout(Duration::from_secs(1)).unwrap()).collect();
        assert_eq!(rest, (1..pushed as u32).collect::<Vec<_>>());
    }

    #[test]
    fn steady_motion_at_uneven_sample_times() {
        // 2 m/s forward (-z) and 0.5 m/s up; the samples' times jitter by a few ms, as a headset's do
        let v = Vec3::new(0.0, 0.5, -2.0);
        let jitter = [0.0, 0.004, -0.003, 0.002, -0.004, 0.003];
        let mut p = PoseVelocity::default();
        let mut out = Vec3::ZERO;
        for i in 0..40 {
            let t = 100.0 + i as f64 * HZ72 + jitter[i % jitter.len()];
            out = p.update(t, Vec3::new(0.2, 1.2, -0.3) + v * (t - 100.0) as f32, Quat::IDENTITY).0;
        }
        assert!(close(out, v, 0.01), "{out:?}");
    }

    #[test]
    fn repeated_and_close_samples() {
        // ALVR can hand over the same sample again, or one that came only just after the last (a bunched stream)
        let v = Vec3::new(1.5, 0.0, 0.0);
        let mut p = PoseVelocity::default();
        let mut out = Vec3::ZERO;
        for i in 0..40 {
            let t = i as f64 * HZ72;
            out = p.update(t, v * t as f32, Quat::IDENTITY).0;
        }
        assert!(close(out, v, 0.01), "{out:?}");
        // the same sample again changes nothing; one 2 ms later is measured against the older one
        let t = 40.0 * HZ72;
        let again = p.update(39.0 * HZ72, v * (39.0 * HZ72) as f32, Quat::IDENTITY).0;
        assert!(close(again, v, 0.01));
        let soon = p.update(39.0 * HZ72 + 0.002, v * (39.0 * HZ72 + 0.002) as f32 + Vec3::new(0.05, 0.0, 0.0), Quat::IDENTITY).0;
        assert!(close(soon, v, 0.01), "a sample 2 ms after the last is not divided by 2 ms: {soon:?}");
        let next = p.update(t, v * t as f32, Quat::IDENTITY).0;
        assert!(close(next, v, 0.01), "{next:?}");
    }

    #[test]
    fn a_throw_reads_as_the_hand_moved() {
        // the hand speeds up at 40 m/s² for 0.15 s (to 6 m/s) and stops: the estimate follows within a sample or two
        let mut p = PoseVelocity::default();
        let (a, release) = (40.0f32, 0.15f64);
        let mut fastest = 0.0f32;
        let mut at_release = 0.0f32;
        for i in 0..30 {
            let t = i as f64 * HZ72;
            let tt = t.min(release) as f32;
            let x = 0.5 * a * tt * tt;
            let v = p.update(t, Vec3::new(x, 1.2, -0.3), Quat::IDENTITY).0.length();
            fastest = fastest.max(v);
            if t <= release { at_release = v; }
        }
        assert!(fastest <= 6.0 + 0.05, "never faster than the hand was: {fastest}");
        assert!(at_release > 4.5, "close to the hand's speed at the release: {at_release}");
    }

    #[test]
    fn turning_in_the_hands_frame() {
        // the hand, rolled a quarter turn about its forward axis, turns about the world's up axis at 5 rad/s
        let rolled = Quat::from_rotation_z(std::f32::consts::FRAC_PI_2);
        let w = Vec3::new(0.0, 5.0, 0.0);
        let mut p = PoseVelocity::default();
        let mut out = (Vec3::ZERO, Vec3::ZERO);
        let mut q = rolled;
        for i in 0..40 {
            let t = i as f64 * HZ72;
            q = Quat::from_scaled_axis(w * t as f32) * rolled;
            out = p.update(t, Vec3::new(0.0, 1.2, -0.3), q);
        }
        assert!(close(out.1, w, 0.01), "the turn is about up: {:?}", out.1);
        assert!(close(out.0, Vec3::ZERO, 1e-4));
    }

    #[test]
    fn a_fast_wrist_flick_is_not_wrapped() {
        // 20 rad/s (a sharp flick), and a quaternion that flips sign between samples (the same rotation)
        let w = Vec3::new(20.0, 0.0, 0.0);
        let mut p = PoseVelocity::default();
        let mut out = Vec3::ZERO;
        let mut q = Quat::IDENTITY;
        for i in 0..20 {
            let t = i as f64 * HZ72;
            q = Quat::from_scaled_axis(w * t as f32);
            if i % 2 == 1 { q = -q; }
            out = p.update(t, Vec3::ZERO, q).1;
        }
        assert!(close(out, w, 0.05), "{out:?}");
    }

    #[test]
    fn tracking_jumps_and_gaps() {
        let mut p = PoseVelocity::default();
        for i in 0..20 { p.update(i as f64 * HZ72, Vec3::new(1.0 * (i as f64 * HZ72) as f32, 0.0, 0.0), Quat::IDENTITY); }
        // a 0.5 m jump in one sample (36 m/s) is not motion
        let t = 20.0 * HZ72;
        let jumped = p.update(t, Vec3::new(t as f32 + 0.5, 0.0, 0.0), Quat::IDENTITY).0;
        assert!(close(jumped, Vec3::new(1.0, 0.0, 0.0), 0.01), "{jumped:?}");
        // after 0.3 s without samples, no motion until two new samples
        assert_eq!(p.update(t + 0.3, Vec3::new(5.0, 0.0, 0.0), Quat::IDENTITY).0, Vec3::ZERO);
        let after = p.update(t + 0.3 + HZ72, Vec3::new(5.0, 0.0, 0.0), Quat::IDENTITY).0;
        assert_eq!(after, Vec3::ZERO);
        // an older time (a new connection's clock) starts over
        assert_eq!(p.update(1.0, Vec3::new(9.0, 0.0, 0.0), Quat::IDENTITY).0, Vec3::ZERO);
    }
}
