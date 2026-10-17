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
    ss["connection"]["stream_protocol"]["variant"] = "Tcp".into();
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
            *self = SampleLog { recent: std::mem::take(&mut self.recent), newest: self.newest, since: Some(std::time::Instant::now()), ..Default::default() };
        }
    }
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
    while let Ok(e) = receiver.recv() {
        match e {
            ServerCoreEvent::ClientConnected => { bridge.connected.store(true, Ordering::SeqCst); bridge.idr_requested.store(true, Ordering::SeqCst); }
            ServerCoreEvent::ClientDisconnected => bridge.connected.store(false, Ordering::SeqCst),
            ServerCoreEvent::RequestIDR => bridge.idr_requested.store(true, Ordering::SeqCst),
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
                let (head, left, right) = {
                    let guard = bridge.ctx.read().unwrap();
                    let Some(ctx) = guard.as_ref() else { continue };
                    let t = sample_timestamp;
                    (matrix(ctx.get_device_motion(*HEAD_ID, t)),
                     matrix(ctx.get_device_motion(*HAND_LEFT_ID, t).or_else(|| hand_motion(ctx, HandType::Left, t))),
                     matrix(ctx.get_device_motion(*HAND_RIGHT_ID, t).or_else(|| hand_motion(ctx, HandType::Right, t))))
                };
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
    alvr_server_core::init_logging(Some(l.session_log()), Some(l.crash_log()));
    let identity = alvr_common::glam::Mat4::from_rotation_translation(Quat::IDENTITY, Vec3::new(0.0, 1.6, 0.0)).to_cols_array();
    let bridge = Arc::new(Bridge {
        ctx: RwLock::new(None), input: Mutex::new(VxAlvrInput { head: identity, left: identity, right: identity, ..Default::default() }),
        seq: AtomicU64::new(0), connected: AtomicBool::new(false), idr_requested: AtomicBool::new(false), config: Mutex::new(vec![]), samples: Mutex::new(SampleLog::default()),
        started_with: openvr_config(&l), layout: l,
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

/// True when a headset brought display settings other than those the core started with (see run): the host has to
/// start again.
#[no_mangle]
pub extern "C" fn vx_alvr_restart_needed() -> bool {
    BRIDGE.get().is_some_and(|b| !b.connected.load(Ordering::SeqCst) && openvr_config(&b.layout) != b.started_with)
}

/// The client's display rate (ALVR's session, as negotiated at the last connection), 0 before any.
#[no_mangle]
pub extern "C" fn vx_alvr_refresh_hz() -> f32 {
    let path = layout(None).session();
    fs::read_to_string(path).ok().and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .and_then(|s| s["openvr_config"]["refresh_rate"].as_f64()).unwrap_or(0.0) as f32
}

/// A vibration the game asked for (hand 0 left, 1 right), to the headset's controller through ALVR, which scales it
/// by its haptics settings (on by default).
#[no_mangle]
pub extern "C" fn vx_alvr_haptics(hand: u32, duration_s: f32, frequency: f32, amplitude: f32) {
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
        b.samples.lock().unwrap().frame(t.as_nanos() as u64);
        if let Some(ctx) = b.ctx.read().unwrap().as_ref() { ctx.send_video_nal(t, std::slice::from_raw_parts(au, len).to_vec(), is_idr); }
    }
}
