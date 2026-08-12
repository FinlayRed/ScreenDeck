# SPDX-License-Identifier: GPL-3.0-or-later

[CmdletBinding()]
param(
    [switch] $CommitTestBundle,
    [switch] $ResumeTest,
    [switch] $AllowDestructiveBundle
)

$ErrorActionPreference = 'Stop'

# A dependency-free Windows smoke test for the M3 WinUSB function. The future
# Tauri client will use the same stable interface GUID and SDC3 wire format.
if (-not ('Screendeck.M3WinUsb' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Screendeck {
  public static class M3WinUsb {
    static readonly Guid InterfaceGuid = new Guid("F38C253C-7E95-4F15-A9FD-7BBC31E4F0C4");
    const uint Present = 0x2, DeviceInterface = 0x10, ReadWrite = 0xC0000000, OpenExisting = 3;
    const byte PipeTransferTimeout = 3;
    [StructLayout(LayoutKind.Sequential)] struct IfaceData { public int cbSize; public Guid guid; public int flags; public IntPtr reserved; }
    [StructLayout(LayoutKind.Sequential, Pack=1)] struct IfaceDesc { public byte length, type, number, alternate, pipes, klass, subclass, protocol, index; }
    // WINUSB_PIPE_INFORMATION is not a raw USB endpoint descriptor: PipeType is
    // a 32-bit enum followed by the endpoint address (PipeId).
    [StructLayout(LayoutKind.Sequential, Pack=1)] struct PipeInfo { public uint type; public byte address; public ushort maximumPacketSize; public byte interval; }
    [DllImport("setupapi.dll", SetLastError=true)] static extern IntPtr SetupDiGetClassDevs(ref Guid guid, IntPtr enumerator, IntPtr hwnd, uint flags);
    [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr device, ref Guid guid, uint index, ref IfaceData data);
    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr set, ref IfaceData data, IntPtr detail, uint size, out uint required, IntPtr device);
    [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint create, uint flags, IntPtr template);
    [DllImport("winusb.dll", SetLastError=true)] static extern bool WinUsb_Initialize(SafeFileHandle file, out IntPtr handle);
    [DllImport("winusb.dll", SetLastError=true)] static extern bool WinUsb_Free(IntPtr handle);
    [DllImport("winusb.dll", SetLastError=true)] static extern bool WinUsb_QueryInterfaceSettings(IntPtr handle, byte alternate, out IfaceDesc descriptor);
    [DllImport("winusb.dll", SetLastError=true)] static extern bool WinUsb_QueryPipe(IntPtr handle, byte alternate, byte index, out PipeInfo pipe);
    [DllImport("winusb.dll", SetLastError=true)] static extern bool WinUsb_SetPipePolicy(IntPtr handle, byte pipe, uint policy, uint length, ref uint value);
    [DllImport("winusb.dll", SetLastError=true)] static extern bool WinUsb_WritePipe(IntPtr handle, byte pipe, byte[] buffer, uint length, out uint written, IntPtr overlapped);
    [DllImport("winusb.dll", SetLastError=true)] static extern bool WinUsb_ReadPipe(IntPtr handle, byte pipe, byte[] buffer, uint length, out uint read, IntPtr overlapped);
    static void Check(bool value) { if (!value) throw new Win32Exception(Marshal.GetLastWin32Error()); }
    static string Path() {
      Guid interfaceGuid = InterfaceGuid;
      IntPtr set = SetupDiGetClassDevs(ref interfaceGuid, IntPtr.Zero, IntPtr.Zero, Present | DeviceInterface);
      if (set == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error(), "M3 WinUSB interface not present");
      try {
        var data = new IfaceData { cbSize = Marshal.SizeOf(typeof(IfaceData)) };
        Check(SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref interfaceGuid, 0, ref data));
        uint required; SetupDiGetDeviceInterfaceDetail(set, ref data, IntPtr.Zero, 0, out required, IntPtr.Zero);
        IntPtr detail = Marshal.AllocHGlobal((int)required);
        try {
          Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
          Check(SetupDiGetDeviceInterfaceDetail(set, ref data, detail, required, out required, IntPtr.Zero));
          return Marshal.PtrToStringUni(IntPtr.Add(detail, 4));
        } finally { Marshal.FreeHGlobal(detail); }
      } finally { SetupDiDestroyDeviceInfoList(set); }
    }
    public static uint Crc32(byte[] data) {
      // esp_rom_crc32_le() complements its public seed internally. M3 passes
      // UINT32_MAX, which corresponds to a raw reflected CRC accumulator of 0.
      uint crc = 0;
      foreach (byte value in data) { crc ^= value; for (int bit=0; bit<8; bit++) crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320u : 0u); }
      return ~crc;
    }
    public static string DevicePath() { return Path(); }
    static uint U32(byte[] value, int offset) { return BitConverter.ToUInt32(value, offset); }
    static void Put(byte[] value, int offset, uint number) { Array.Copy(BitConverter.GetBytes(number), 0, value, offset, 4); }
    public static uint[] Exchange(byte opcode, uint sequence, byte[] payload) {
      payload = payload ?? Array.Empty<byte>();
      var frame = new byte[20 + payload.Length];
      Put(frame, 0, 0x33434453); frame[4] = 1; frame[5] = opcode; Put(frame, 8, sequence); Put(frame, 12, (uint)payload.Length); Put(frame, 16, Crc32(payload));
      Array.Copy(payload, 0, frame, 20, payload.Length);
      // WinUsb_Initialize requires FILE_FLAG_OVERLAPPED (Microsoft WinUSB API).
      using (var file = CreateFile(Path(), ReadWrite, 3, IntPtr.Zero, OpenExisting, 0x40000000, IntPtr.Zero)) {
        if (file.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
        IntPtr usb;
        if (!WinUsb_Initialize(file, out usb)) throw new Win32Exception(Marshal.GetLastWin32Error(), "WinUsb_Initialize");
        try {
          IfaceDesc descriptor; Check(WinUsb_QueryInterfaceSettings(usb, 0, out descriptor));
          byte input=0, output=0;
          for (byte i=0; i<descriptor.pipes; i++) { PipeInfo pipe; Check(WinUsb_QueryPipe(usb, 0, i, out pipe)); if ((pipe.address & 0x80) != 0) input=pipe.address; else output=pipe.address; }
          if (input == 0 || output == 0) throw new InvalidOperationException("M3 bulk endpoints were not found (interface=" + descriptor.number + ", pipes=" + descriptor.pipes + ").");
          uint timeout = 3000; Check(WinUsb_SetPipePolicy(usb, input, PipeTransferTimeout, 4, ref timeout));
          uint written; Check(WinUsb_WritePipe(usb, output, frame, (uint)frame.Length, out written, IntPtr.Zero));
          if (written != frame.Length) throw new InvalidOperationException("Short M3 write.");
          var response = new List<byte>();
          while (response.Count < 20 || response.Count < 20 + (int)U32(response.ToArray(), 12)) {
            var packet = new byte[512]; uint read; Check(WinUsb_ReadPipe(usb, input, packet, (uint)packet.Length, out read, IntPtr.Zero));
            for (int i=0; i<read; i++) response.Add(packet[i]);
          }
          var bytes = response.ToArray();
          if (U32(bytes, 0) != 0x33434453 || bytes[4] != 1 || bytes[5] != (byte)(opcode | 0x80) || U32(bytes, 8) != sequence || U32(bytes, 12) != 8) throw new InvalidOperationException("Malformed M3 response.");
          var body = new byte[8]; Array.Copy(bytes, 20, body, 0, 8);
          if (U32(bytes, 16) != Crc32(body)) throw new InvalidOperationException("M3 response CRC mismatch (device=0x" + U32(bytes, 16).ToString("X8") + ", host=0x" + Crc32(body).ToString("X8") + ").");
          return new uint[] { U32(body, 0), U32(body, 4) };
        } finally { WinUsb_Free(usb); }
      }
    }
    public static byte[] Bundle(byte[] payload) {
      var bundle = new byte[16 + payload.Length];
      Put(bundle, 0, 0x33424453); Array.Copy(BitConverter.GetBytes((ushort)1), 0, bundle, 4, 2); Array.Copy(BitConverter.GetBytes((ushort)16), 0, bundle, 6, 2);
      Put(bundle, 8, (uint)bundle.Length); Put(bundle, 12, Crc32(payload)); Array.Copy(payload, 0, bundle, 16, payload.Length); return bundle;
    }
  }
}
'@
}

function Invoke-M3([byte] $Opcode, [uint32] $Sequence, [byte[]] $Payload = @()) {
    $reply = [Screendeck.M3WinUsb]::Exchange($Opcode, $Sequence, $Payload)
    if ($reply[0] -ne 0) { throw "M3 opcode $Opcode failed: status=$($reply[0]) value=$($reply[1])" }
    return $reply[1]
}

$sequence = 1
$devicePath = [Screendeck.M3WinUsb]::DevicePath()
Write-Verbose "M3 WinUSB path: $devicePath"
$caps = Invoke-M3 1 $sequence; $sequence++
if (($caps -band 0x1F) -ne 0x1F) { throw "Unexpected M3 capability word: 0x$($caps.ToString('X8'))" }
$before = Invoke-M3 6 $sequence; $sequence++
Write-Host "M3 HELLO ok capabilities=0x$($caps.ToString('X8')); generation=$before"
if ($CommitTestBundle -and $ResumeTest) { throw 'Choose either -CommitTestBundle or -ResumeTest.' }
# The commit test bundles a deliberately invalid M5UI payload (96 pseudo-random
# bytes). Activating it can replace a working device UI with an unloadable one,
# so it is disabled unless the operator explicitly opts in (see F3).
if ($CommitTestBundle -and -not $AllowDestructiveBundle) {
    throw 'Refusing to commit an invalid test bundle: this can replace the working device UI. Pass -AllowDestructiveBundle only on a sacrificial device, or wait for the F3 fix to build a valid bundle.'
}

if ($CommitTestBundle -or $ResumeTest) {
    [byte[]] $payload = 0..95 | ForEach-Object { [byte](($_ * 37 + 11) -band 0xFF) }
    [byte[]] $bundle = [Screendeck.M3WinUsb]::Bundle($payload)
    $bundleCrc = [Screendeck.M3WinUsb]::Crc32($payload)
    [byte[]] $begin = [byte[]](@([BitConverter]::GetBytes([uint32]$bundle.Length)) + @([BitConverter]::GetBytes([uint32]$bundleCrc)))
    $offset = Invoke-M3 2 $sequence $begin; $sequence++
    if ($offset -gt $bundle.Length) { throw "Device resume offset exceeds test bundle size." }
    if ($ResumeTest) {
        $half = [uint32]($bundle.Length / 2)
        [byte[]] $partial = $bundle[$offset..($half - 1)]
        $partialCrc = [Screendeck.M3WinUsb]::Crc32($partial)
        [byte[]] $partialPayload = [byte[]](@([BitConverter]::GetBytes([uint32]$offset)) + @([BitConverter]::GetBytes([uint32]$partialCrc)) + @($partial))
        $received = Invoke-M3 3 $sequence $partialPayload; $sequence++
        if ($received -ne $half) { throw "M3 resume setup acknowledged $received bytes; expected $half." }
        $resumed = Invoke-M3 2 $sequence $begin; $sequence++
        if ($resumed -ne $half) { throw "M3 resume offset was $resumed; expected $half." }
        $null = Invoke-M3 5 $sequence; $sequence++
        Write-Host "M3 RESUME ok persisted_offset=$resumed; upload aborted without changing generation"
        $offset = $bundle.Length
    }
    if ($CommitTestBundle -and $offset -lt $bundle.Length) {
        [byte[]] $chunk = $bundle[$offset..($bundle.Length - 1)]
        $chunkCrc = [Screendeck.M3WinUsb]::Crc32($chunk)
        [byte[]] $chunkPayload = [byte[]](@([BitConverter]::GetBytes([uint32]$offset)) + @([BitConverter]::GetBytes([uint32]$chunkCrc)) + @($chunk))
        $received = Invoke-M3 3 $sequence $chunkPayload; $sequence++
        if ($received -ne $bundle.Length) { throw "M3 acknowledged $received bytes; expected $($bundle.Length)." }
    }
    if ($CommitTestBundle) {
        $generation = Invoke-M3 4 $sequence; $sequence++
        Write-Host "M3 COMMIT ok generation=$generation bytes=$($bundle.Length) payload_crc=0x$($bundleCrc.ToString('X8'))"
    }
}
