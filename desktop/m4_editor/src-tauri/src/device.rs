use crate::compiler::crc32;
use serde::Serialize;
use thiserror::Error;
use std::{thread, time::Duration};

const MAGIC: u32 = 0x3343_4453;
const VERSION: u8 = 1;
const HELLO: u8 = 1;
const BEGIN: u8 = 2;
const CHUNK: u8 = 3;
const COMMIT: u8 = 4;
const STATUS: u8 = 6;
const CHUNK_BYTES: usize = 112;
const DEVICE_RESPONSE_SETTLE_MS: u64 = 50;

#[derive(Debug, Error)]
pub enum DeviceError {
    #[error("Screendeck sync interface was not found. Connect the USB-OTG port and confirm that Windows shows ‘Sync channel’ under USB devices.")]
    NotFound,
    #[error("Windows could not open the Screendeck WinUSB interface (error {0}). Close other Screendeck tools and reconnect the device.")]
    Windows(u32),
    #[error("device returned a malformed SDC3 response: {0}")]
    Protocol(String),
    #[error("device rejected {operation}: {status} ({detail})")]
    Rejected { operation: &'static str, status: u32, detail: &'static str },
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeviceStatus { pub connected: bool, pub generation: u32, pub capabilities: u32, pub detail: String }

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SyncResult { pub generation: u32, pub bytes_sent: usize, pub resumed_at: u32, pub fingerprint: String }

fn status_detail(status: u32) -> &'static str {
    match status { 1 => "bad frame or checksum", 2 => "transfer is not open", 3 => "microSD I/O failure", 4 => "bundle validation failed", 5 => "device is busy", _ => "unknown device error" }
}

fn frame(opcode: u8, sequence: u32, payload: &[u8]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(20 + payload.len());
    bytes.extend_from_slice(&MAGIC.to_le_bytes()); bytes.push(VERSION); bytes.push(opcode);
    bytes.extend_from_slice(&0u16.to_le_bytes()); bytes.extend_from_slice(&sequence.to_le_bytes());
    bytes.extend_from_slice(&(payload.len() as u32).to_le_bytes()); bytes.extend_from_slice(&crc32(payload).to_le_bytes());
    bytes.extend_from_slice(payload); bytes
}

fn parse_response(bytes: &[u8], opcode: u8, sequence: u32) -> Result<(u32, u32), DeviceError> {
    if bytes.len() != 28 || u32::from_le_bytes(bytes[0..4].try_into().unwrap()) != MAGIC || bytes[4] != VERSION || bytes[5] != opcode | 0x80 || u32::from_le_bytes(bytes[8..12].try_into().unwrap()) != sequence || u32::from_le_bytes(bytes[12..16].try_into().unwrap()) != 8 {
        return Err(DeviceError::Protocol(format!("unexpected header ({} bytes)", bytes.len())));
    }
    if u32::from_le_bytes(bytes[16..20].try_into().unwrap()) != crc32(&bytes[20..28]) { return Err(DeviceError::Protocol("response checksum mismatch".into())); }
    Ok((u32::from_le_bytes(bytes[20..24].try_into().unwrap()), u32::from_le_bytes(bytes[24..28].try_into().unwrap())))
}

fn checked_exchange(opcode: u8, sequence: u32, payload: &[u8], operation: &'static str) -> Result<u32, DeviceError> {
    let response = transport::exchange(&frame(opcode, sequence, payload))?;
    let (status, value) = parse_response(&response, opcode, sequence)?;
    if status != 0 { return Err(DeviceError::Rejected { operation, status, detail: status_detail(status) }); }
    // M3 sends replies non-blockingly. Give TinyUSB time to complete the IN
    // transfer before the next OUT frame so its response buffer is available.
    thread::sleep(Duration::from_millis(DEVICE_RESPONSE_SETTLE_MS));
    Ok(value)
}

pub fn status() -> DeviceStatus {
    let result = (|| {
        let capabilities = checked_exchange(HELLO, 1, &[], "capability query")?;
        if capabilities & 0x1f != 0x1f { return Err(DeviceError::Protocol(format!("unsupported capability word 0x{capabilities:08X}"))); }
        let generation = checked_exchange(STATUS, 2, &[], "status query")?;
        Ok((capabilities, generation))
    })();
    match result {
        Ok((capabilities, generation)) => DeviceStatus { connected: true, generation, capabilities, detail: "SDC3 v1 · resume · checksums · atomic activation".into() },
        Err(error) => DeviceStatus { connected: false, generation: 0, capabilities: 0, detail: error.to_string() },
    }
}

pub fn sync(bundle: &[u8], fingerprint: String) -> Result<SyncResult, DeviceError> {
    let capabilities = checked_exchange(HELLO, 1, &[], "capability query")?;
    if capabilities & 0x1f != 0x1f { return Err(DeviceError::Protocol(format!("device capabilities 0x{capabilities:08X} do not satisfy M4"))); }
    let initial_generation = checked_exchange(STATUS, 2, &[], "status query")?;
    let payload_crc = u32::from_le_bytes(bundle[12..16].try_into().unwrap());
    let mut begin = Vec::with_capacity(8);
    begin.extend_from_slice(&(bundle.len() as u32).to_le_bytes()); begin.extend_from_slice(&payload_crc.to_le_bytes());
    let resumed_at = checked_exchange(BEGIN, 3, &begin, "begin upload")?;
    if resumed_at as usize > bundle.len() { return Err(DeviceError::Protocol(format!("resume offset {resumed_at} exceeds bundle size {}", bundle.len()))); }
    let mut offset = resumed_at as usize;
    let mut sequence = 4u32;
    while offset < bundle.len() {
        let end = (offset + CHUNK_BYTES).min(bundle.len());
        let chunk = &bundle[offset..end];
        let mut payload = Vec::with_capacity(chunk.len() + 8);
        payload.extend_from_slice(&(offset as u32).to_le_bytes()); payload.extend_from_slice(&crc32(chunk).to_le_bytes()); payload.extend_from_slice(chunk);
        let acknowledged = match checked_exchange(CHUNK, sequence, &payload, "upload chunk") {
            Ok(value) => value,
            Err(DeviceError::Protocol(message)) if message.contains("timed out") => {
                thread::sleep(Duration::from_millis(250));
                sequence += 1;
                let safe_offset = checked_exchange(BEGIN, sequence, &begin, "recover upload acknowledgement")?;
                if safe_offset < offset as u32 || safe_offset as usize > bundle.len() { return Err(DeviceError::Protocol(format!("invalid recovered offset {safe_offset}"))); }
                offset = safe_offset as usize;
                sequence += 1;
                continue;
            }
            Err(error) => return Err(error),
        };
        if acknowledged as usize != end { return Err(DeviceError::Protocol(format!("device acknowledged byte {acknowledged}; expected {end}"))); }
        offset = end; sequence += 1;
    }
    let generation = match checked_exchange(COMMIT, sequence, &[], "commit bundle") {
        Ok(value) => value,
        Err(DeviceError::Protocol(message)) if message.contains("timed out") => {
            thread::sleep(Duration::from_millis(250));
            let observed = checked_exchange(STATUS, sequence + 1, &[], "verify commit")?;
            if observed <= initial_generation { return Err(DeviceError::Protocol("commit acknowledgement was lost and the active generation did not advance".into())); }
            observed
        }
        Err(error) => return Err(error),
    };
    Ok(SyncResult { generation, bytes_sent: bundle.len() - resumed_at as usize, resumed_at, fingerprint })
}

#[cfg(windows)]
mod transport {
    use super::DeviceError;
    use std::{ffi::c_void, mem::{size_of, zeroed}, ptr::{null, null_mut}};

    type Handle = *mut c_void;
    const INVALID_HANDLE: Handle = -1isize as Handle;
    const DIGCF_PRESENT: u32 = 0x2;
    const DIGCF_DEVICEINTERFACE: u32 = 0x10;
    const GENERIC_READ_WRITE: u32 = 0xC000_0000;
    const OPEN_EXISTING: u32 = 3;
    const FILE_FLAG_OVERLAPPED: u32 = 0x4000_0000;
    const PIPE_TRANSFER_TIMEOUT: u32 = 3;
    const ERROR_IO_PENDING: u32 = 997;
    const WAIT_OBJECT_0: u32 = 0;
    const IO_TIMEOUT_MS: u32 = 5_000;

    #[repr(C)] #[derive(Clone, Copy)] struct Guid { data1: u32, data2: u16, data3: u16, data4: [u8; 8] }
    const INTERFACE_GUID: Guid = Guid { data1: 0xF38C253C, data2: 0x7E95, data3: 0x4F15, data4: [0xA9,0xFD,0x7B,0xBC,0x31,0xE4,0xF0,0xC4] };
    #[repr(C)] struct InterfaceData { cb_size: u32, guid: Guid, flags: u32, reserved: usize }
    #[repr(C)] struct InterfaceDescriptor { length: u8, descriptor_type: u8, interface_number: u8, alternate_setting: u8, num_endpoints: u8, interface_class: u8, interface_sub_class: u8, interface_protocol: u8, interface_index: u8 }
    #[repr(C)] struct PipeInfo { pipe_type: u32, pipe_id: u8, padding: [u8; 7] }
    #[repr(C)] struct Overlapped { internal: usize, internal_high: usize, offset: u32, offset_high: u32, event: Handle }

    #[link(name="setupapi")] extern "system" {
        fn SetupDiGetClassDevsW(guid: *const Guid, enumerator: *const u16, hwnd: Handle, flags: u32) -> Handle;
        fn SetupDiEnumDeviceInterfaces(set: Handle, device: *mut c_void, guid: *const Guid, index: u32, data: *mut InterfaceData) -> i32;
        fn SetupDiGetDeviceInterfaceDetailW(set: Handle, data: *mut InterfaceData, detail: *mut u8, size: u32, required: *mut u32, device: *mut c_void) -> i32;
        fn SetupDiDestroyDeviceInfoList(set: Handle) -> i32;
    }
    #[link(name="kernel32")] extern "system" {
        fn CreateFileW(name: *const u16, access: u32, share: u32, security: *mut c_void, creation: u32, flags: u32, template: Handle) -> Handle;
        fn CloseHandle(handle: Handle) -> i32;
        fn GetLastError() -> u32;
        fn CreateEventW(attributes: *mut c_void, manual_reset: i32, initial_state: i32, name: *const u16) -> Handle;
        fn WaitForSingleObject(handle: Handle, milliseconds: u32) -> u32;
        fn GetOverlappedResult(file: Handle, overlapped: *mut Overlapped, transferred: *mut u32, wait: i32) -> i32;
        fn CancelIoEx(file: Handle, overlapped: *mut Overlapped) -> i32;
    }
    #[link(name="winusb")] extern "system" {
        fn WinUsb_Initialize(file: Handle, interface: *mut Handle) -> i32;
        fn WinUsb_Free(interface: Handle) -> i32;
        fn WinUsb_QueryInterfaceSettings(interface: Handle, alternate: u8, descriptor: *mut InterfaceDescriptor) -> i32;
        fn WinUsb_QueryPipe(interface: Handle, alternate: u8, index: u8, pipe: *mut PipeInfo) -> i32;
        fn WinUsb_SetPipePolicy(interface: Handle, pipe: u8, policy: u32, length: u32, value: *mut u32) -> i32;
        fn WinUsb_WritePipe(interface: Handle, pipe: u8, buffer: *const u8, length: u32, written: *mut u32, overlapped: *mut c_void) -> i32;
        fn WinUsb_ReadPipe(interface: Handle, pipe: u8, buffer: *mut u8, length: u32, read: *mut u32, overlapped: *mut c_void) -> i32;
        fn WinUsb_AbortPipe(interface: Handle, pipe: u8) -> i32;
        fn WinUsb_ResetPipe(interface: Handle, pipe: u8) -> i32;
    }

    struct DeviceSet(Handle); impl Drop for DeviceSet { fn drop(&mut self) { unsafe { SetupDiDestroyDeviceInfoList(self.0); } } }
    struct File(Handle); impl Drop for File { fn drop(&mut self) { unsafe { CloseHandle(self.0); } } }
    struct WinUsb(Handle); impl Drop for WinUsb { fn drop(&mut self) { unsafe { WinUsb_Free(self.0); } } }
    struct Event(Handle); impl Drop for Event { fn drop(&mut self) { unsafe { CloseHandle(self.0); } } }
    fn last_error() -> DeviceError { DeviceError::Windows(unsafe { GetLastError() }) }

    unsafe fn await_io(file: Handle, started: i32, overlapped: &mut Overlapped, transferred: &mut u32) -> Result<(), DeviceError> {
        if started == 0 && GetLastError() != ERROR_IO_PENDING { return Err(last_error()); }
        if started == 0 {
            if WaitForSingleObject(overlapped.event, IO_TIMEOUT_MS) != WAIT_OBJECT_0 {
                CancelIoEx(file, overlapped);
                return Err(DeviceError::Protocol("USB transfer timed out after 5 seconds".into()));
            }
            if GetOverlappedResult(file, overlapped, transferred, 0) == 0 { return Err(last_error()); }
        }
        Ok(())
    }

    unsafe fn device_path() -> Result<Vec<u16>, DeviceError> {
        let set = DeviceSet(SetupDiGetClassDevsW(&INTERFACE_GUID, null(), null_mut(), DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
        if set.0 == INVALID_HANDLE { return Err(DeviceError::NotFound); }
        let mut data: InterfaceData = zeroed(); data.cb_size = size_of::<InterfaceData>() as u32;
        if SetupDiEnumDeviceInterfaces(set.0, null_mut(), &INTERFACE_GUID, 0, &mut data) == 0 { return Err(DeviceError::NotFound); }
        let mut required = 0u32;
        SetupDiGetDeviceInterfaceDetailW(set.0, &mut data, null_mut(), 0, &mut required, null_mut());
        if required < 8 { return Err(last_error()); }
        let mut detail = vec![0u8; required as usize];
        *(detail.as_mut_ptr() as *mut u32) = if size_of::<usize>() == 8 { 8 } else { 6 };
        if SetupDiGetDeviceInterfaceDetailW(set.0, &mut data, detail.as_mut_ptr(), required, &mut required, null_mut()) == 0 { return Err(last_error()); }
        let start = detail.as_ptr().add(4) as *const u16;
        let length = (0..).take_while(|&i| *start.add(i) != 0).count();
        let mut path = std::slice::from_raw_parts(start, length).to_vec(); path.push(0); Ok(path)
    }

    struct Connection { file: File, usb: WinUsb, input: u8, output: u8 }
    unsafe impl Send for Connection {}

    impl Connection {
        unsafe fn open() -> Result<Self, DeviceError> {
            let path = device_path()?;
            let file = File(CreateFileW(path.as_ptr(), GENERIC_READ_WRITE, 3, null_mut(), OPEN_EXISTING, FILE_FLAG_OVERLAPPED, null_mut()));
            if file.0 == INVALID_HANDLE { return Err(last_error()); }
            let mut usb_handle = null_mut();
            if WinUsb_Initialize(file.0, &mut usb_handle) == 0 { return Err(last_error()); }
            let usb = WinUsb(usb_handle);
            let mut descriptor: InterfaceDescriptor = zeroed();
            if WinUsb_QueryInterfaceSettings(usb.0, 0, &mut descriptor) == 0 { return Err(last_error()); }
            let (mut input, mut output) = (0u8, 0u8);
            for index in 0..descriptor.num_endpoints {
                let mut pipe: PipeInfo = zeroed();
                if WinUsb_QueryPipe(usb.0, 0, index, &mut pipe) == 0 { return Err(last_error()); }
                if pipe.pipe_id & 0x80 != 0 { input = pipe.pipe_id; } else { output = pipe.pipe_id; }
            }
            if input == 0 || output == 0 { return Err(DeviceError::Protocol("bulk endpoints were not found".into())); }
            let mut timeout = IO_TIMEOUT_MS;
            if WinUsb_SetPipePolicy(usb.0, input, PIPE_TRANSFER_TIMEOUT, 4, &mut timeout) == 0 { return Err(last_error()); }
            Ok(Self { file, usb, input, output })
        }

        unsafe fn exchange(&mut self, frame: &[u8]) -> Result<Vec<u8>, DeviceError> {
            let file = self.file.0;
            let usb = self.usb.0;
            let input = self.input;
            let output = self.output;
            let write_event = Event(CreateEventW(null_mut(), 0, 0, null()));
            if write_event.0.is_null() { return Err(last_error()); }
            let mut write_overlapped = Overlapped { internal: 0, internal_high: 0, offset: 0, offset_high: 0, event: write_event.0 };
            let mut written = 0u32;
            let write_started = WinUsb_WritePipe(usb, output, frame.as_ptr(), frame.len() as u32, &mut written, &mut write_overlapped as *mut _ as *mut c_void);
            await_io(file, write_started, &mut write_overlapped, &mut written)?;
            if written as usize != frame.len() { return Err(DeviceError::Protocol(format!("short USB write: {written}/{}", frame.len()))); }
            let mut response = Vec::with_capacity(28);
            while response.len() < 28 {
                let mut packet = [0u8; 512]; let mut read = 0u32;
                let read_event = Event(CreateEventW(null_mut(), 0, 0, null()));
                if read_event.0.is_null() { return Err(last_error()); }
                let mut read_overlapped = Overlapped { internal: 0, internal_high: 0, offset: 0, offset_high: 0, event: read_event.0 };
                let read_started = WinUsb_ReadPipe(usb, input, packet.as_mut_ptr(), packet.len() as u32, &mut read, &mut read_overlapped as *mut _ as *mut c_void);
                if let Err(error) = await_io(file, read_started, &mut read_overlapped, &mut read) {
                    WinUsb_AbortPipe(usb, input);
                    WinUsb_ResetPipe(usb, input);
                    return Err(error);
                }
                if read == 0 { return Err(DeviceError::Protocol("empty USB response".into())); }
                response.extend_from_slice(&packet[..read as usize]);
            }
            response.truncate(28); Ok(response)
        }
    }

    pub fn exchange(frame: &[u8]) -> Result<Vec<u8>, DeviceError> {
        unsafe {
            let mut connection = Connection::open()?;
            connection.exchange(frame)
        }
    }
}

#[cfg(not(windows))]
mod transport { use super::DeviceError; pub fn exchange(_: &[u8]) -> Result<Vec<u8>, DeviceError> { Err(DeviceError::NotFound) } }
