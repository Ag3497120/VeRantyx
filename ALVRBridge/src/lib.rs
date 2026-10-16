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
    glam::{Quat, Vec3},
    DeviceMotion, Fov, HAND_LEFT_ID, HAND_RIGHT_ID, HEAD_ID, LEFT_MENU_CLICK_ID, LEFT_SQUEEZE_VALUE_ID,
    LEFT_THUMBSTICK_CLICK_ID, LEFT_THUMBSTICK_TOUCH_ID, LEFT_THUMBSTICK_X_ID, LEFT_THUMBSTICK_Y_ID, LEFT_TRIGGER_VALUE_ID,
    LEFT_X_CLICK_ID, LEFT_Y_CLICK_ID, RIGHT_A_CLICK_ID, RIGHT_B_CLICK_ID, RIGHT_SQUEEZE_VALUE_ID,
    RIGHT_SYSTEM_CLICK_ID, RIGHT_THUMBSTICK_CLICK_ID, RIGHT_THUMBSTICK_TOUCH_ID, RIGHT_THUMBSTICK_X_ID,
    RIGHT_THUMBSTICK_Y_ID, RIGHT_TRIGGER_VALUE_ID,
};
use alvr_packets::ButtonValue;
use alvr_server_core::{ServerCoreContext, ServerCoreEvent};
use alvr_session::{CodecType, SessionConfig};
use std::{
    collections::HashMap,
    env,
    ffi::{c_char, CStr},
    fs,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc, Mutex, OnceLock, RwLock,
    },
    thread,
    time::Duration,
};

pub fn layout(config_dir: Option<PathBuf>) -> alvr_filesystem::Layout {
    let dir = config_dir.unwrap_or_else(|| PathBuf::from(env::var("HOME").unwrap()).join("Library/Application Support/VeRantyx/ALVR"));
    alvr_filesystem::Layout { config_dir: dir.clone(), log_dir: dir, ..Default::default() }
}

// The session for the Mac, written before ALVR reads it (it reads it once): clients trusted as they come (there is no
// dashboard), no game audio (macOS cannot record an output device; VeRantyx's audio comes later), HEVC
// (VideoToolbox), no server-side foveation (VeRantyx's frames are full resolution), TCP (the visionOS client wants it).
pub fn prepare_session(layout: &alvr_filesystem::Layout) {
    let path = layout.session();
    fs::create_dir_all(&layout.config_dir).ok();
    let mut s: serde_json::Value = fs::read_to_string(&path).ok().and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_else(|| serde_json::to_value(SessionConfig::default()).unwrap());
    let ss = &mut s["session_settings"];
    ss["connection"]["client_discovery"]["content"]["auto_trust_clients"] = true.into();
    ss["connection"]["stream_protocol"]["variant"] = "Tcp".into();
    ss["audio"]["game_audio"]["enabled"] = false.into();
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

struct Bridge {
    ctx: RwLock<Option<ServerCoreContext>>,
    input: Mutex<VxAlvrInput>,
    seq: AtomicU64,
    connected: AtomicBool,
    idr_requested: AtomicBool,
    config: Mutex<Vec<u8>>,
    layout: alvr_filesystem::Layout,
    started_with: serde_json::Value,                                   // (the display settings the core read)
}
static BRIDGE: OnceLock<Arc<Bridge>> = OnceLock::new();

fn matrix(m: Option<DeviceMotion>) -> Option<([f32; 16], [f32; 3], [f32; 3])> {
    let m = m?;
    let r = alvr_common::glam::Mat4::from_rotation_translation(m.pose.orientation, m.pose.position);
    Some((r.to_cols_array(), m.linear_velocity.to_array(), m.angular_velocity.to_array()))
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
                    (matrix(ctx.get_device_motion(*HEAD_ID, sample_timestamp)), matrix(ctx.get_device_motion(*HAND_LEFT_ID, sample_timestamp)),
                     matrix(ctx.get_device_motion(*HAND_RIGHT_ID, sample_timestamp)))
                };
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
        seq: AtomicU64::new(0), connected: AtomicBool::new(false), idr_requested: AtomicBool::new(false), config: Mutex::new(vec![]),
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
        if let Some(ctx) = b.ctx.read().unwrap().as_ref() { ctx.send_video_nal(t, std::slice::from_raw_parts(au, len).to_vec(), is_idr); }
    }
}
