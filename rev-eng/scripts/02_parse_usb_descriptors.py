#!/usr/bin/env python3
"""
02_parse_usb_descriptors.py - Extract and decode all TI usblib composite USB
configurations, class device structs, callbacks, descriptors, and string tables
from firmware_3.0.0.45.bin.
"""

import argparse
import json
import struct
from pathlib import Path
from fwlib import FirmwareImage


def decode_usb_string(fw: FirmwareImage, addr: int) -> str | None:
    hdr = fw.read(addr, 2)
    if not hdr:
        return None
    ln, dtype = hdr[0], hdr[1]
    if dtype != 0x03 or ln < 2 or ln > 200:
        return None
    body = fw.read(addr + 2, ln - 2)
    if not body:
        return None
    if ln == 4:
        return f"LANGID=0x{struct.unpack('<H', body)[0]:04X}"
    try:
        return body.decode("utf-16-le")
    except Exception:
        return body.hex()


def decode_string_table(fw: FirmwareImage, ptr: int, count: int) -> list[dict]:
    res = []
    for i in range(count):
        s_addr = fw.u32(ptr + i * 4)
        res.append({
            "index": i,
            "addr": f"0x{s_addr:08X}",
            "value": decode_usb_string(fw, s_addr),
        })
    return res


def parse_raw_descriptors(raw: bytes) -> list[dict]:
    """Parse concatenated standard USB descriptors (Config/IAD/Interface/Endpoint/HID/CDC)."""
    out = []
    off = 0
    while off + 2 <= len(raw):
        ln, dtype = raw[off], raw[off + 1]
        if ln < 2 or off + ln > len(raw):
            break
        body = raw[off:off + ln]
        entry = {"type": f"0x{dtype:02X}", "len": ln, "raw": body.hex()}
        if dtype == 0x02 and ln == 9:
            tot, nifc, cfgv, icfg, attr, pwr = struct.unpack_from("<HBBBBB", body, 2)
            entry["kind"] = "Configuration"
            entry["fields"] = {
                "wTotalLength": tot, "bNumInterfaces": nifc,
                "bConfigurationValue": cfgv, "bmAttributes": f"0x{attr:02X}",
                "bMaxPower_mA": pwr * 2,
            }
        elif dtype == 0x0B and ln == 8:
            first, cnt, cls, sub, proto, iFunc = struct.unpack_from("<BBBBBB", body, 2)
            entry["kind"] = "IAD"
            entry["fields"] = {
                "bFirstInterface": first, "bInterfaceCount": cnt,
                "class": f"{cls:02X}/{sub:02X}/{proto:02X}", "iFunction": iFunc,
            }
        elif dtype == 0x04 and ln == 9:
            inum, alt, neps, cls, sub, proto, istr = struct.unpack_from("<BBBBBBB", body, 2)
            cls_map = {0x02: "CDC-Control", 0x03: "HID", 0x08: "MSC", 0x0A: "CDC-Data", 0xFF: "Vendor"}
            entry["kind"] = "Interface"
            entry["fields"] = {
                "bInterfaceNumber": inum, "bAlternateSetting": alt,
                "bNumEndpoints": neps,
                "bInterfaceClass": f"0x{cls:02X} ({cls_map.get(cls, '?')})",
                "bInterfaceSubClass": f"0x{sub:02X}",
                "bInterfaceProtocol": f"0x{proto:02X}",
                "iInterface": istr,
            }
        elif dtype == 0x05 and ln == 7:
            ep, attr, mps, interv = struct.unpack_from("<BBHB", body, 2)
            ttypes = {0: "Control", 1: "Isoch", 2: "Bulk", 3: "Interrupt"}
            direction = "IN" if (ep & 0x80) else "OUT"
            entry["kind"] = "Endpoint"
            entry["fields"] = {
                "bEndpointAddress": f"0x{ep:02X} ({direction} EP{ep & 0x0F})",
                "bmAttributes": f"0x{attr:02X} ({ttypes.get(attr & 3, '?')})",
                "wMaxPacketSize": mps,
                "bInterval": interv,
            }
        elif dtype == 0x21 and ln >= 9:
            bcdHID, country, ndesc, rtype, rlen = struct.unpack_from("<HBBBH", body, 2)
            entry["kind"] = "HID"
            entry["fields"] = {"bcdHID": f"0x{bcdHID:04X}", "wReportDescLen": rlen}
        elif dtype == 0x24:
            sub = body[2] if ln > 2 else 0
            sub_names = {0: "Header", 1: "CallMgmt", 2: "ACM", 6: "Union"}
            entry["kind"] = f"CS_INTERFACE ({sub_names.get(sub, hex(sub))})"
        out.append(entry)
        off += ln
    return out


def parse_tiva_device_struct(fw: FirmwareImage, addr: int, kind: str) -> dict:
    """Decode TivaWare tUSBDBulkDevice / tUSBDCDCDevice / tUSBDHIDDevice / tUSBDCompositeDevice."""
    vid = fw.u16(addr)
    pid = fw.u16(addr + 2)
    max_pwr = fw.u16(addr + 4)
    pwr_attr = fw.u8(addr + 6)
    info = {
        "kind": kind,
        "sram_addr": f"0x{addr:08X}",
        "default_vid": f"0x{vid:04X}",
        "default_pid": f"0x{pid:04X}",
        "max_power_mA": max_pwr,
        "pwr_attr": f"0x{pwr_attr:02X}",
    }
    if kind == "Bulk":
        # tUSBDBulkDevice: vid(2), pid(2), maxPwr(2), pwrAttr(1), pad(1),
        # pfnRxCallback(4), pvRxCBData(4), pfnTxCallback(4), pvTxCBData(4),
        # ppui8StringDescriptors(4), ui32NumStringDescriptors(4), sPrivateData(...)
        rx_cb = fw.u32(addr + 8)
        rx_data = fw.u32(addr + 12)
        tx_cb = fw.u32(addr + 16)
        tx_data = fw.u32(addr + 20)
        str_ptr = fw.u32(addr + 24)
        str_cnt = fw.u32(addr + 28)
        info.update({
            "rx_callback": f"0x{rx_cb:08X}",
            "rx_cb_data": f"0x{rx_data:08X}",
            "tx_callback": f"0x{tx_cb:08X}",
            "tx_cb_data": f"0x{tx_data:08X}",
            "strings": decode_string_table(fw, str_ptr, min(str_cnt, 12)) if str_cnt < 20 else [],
        })
    elif kind == "CDC":
        # tUSBDCDCDevice: vid(2), pid(2), maxPwr(2), pwrAttr(1), pad(1),
        # pfnControlCallback(4), pvControlCBData(4),
        # pfnRxCallback(4), pvRxCBData(4), pfnTxCallback(4), pvTxCBData(4),
        # ppui8StringDescriptors(4), ui32NumStringDescriptors(4)
        ctrl_cb = fw.u32(addr + 8)
        ctrl_data = fw.u32(addr + 12)
        rx_cb = fw.u32(addr + 16)
        rx_data = fw.u32(addr + 20)
        tx_cb = fw.u32(addr + 24)
        tx_data = fw.u32(addr + 28)
        str_ptr = fw.u32(addr + 32)
        str_cnt = fw.u32(addr + 36)
        info.update({
            "ctrl_callback": f"0x{ctrl_cb:08X}",
            "ctrl_cb_data": f"0x{ctrl_data:08X}",
            "rx_callback": f"0x{rx_cb:08X}",
            "rx_cb_data": f"0x{rx_data:08X}",
            "tx_callback": f"0x{tx_cb:08X}",
            "tx_cb_data": f"0x{tx_data:08X}",
            "strings": decode_string_table(fw, str_ptr, min(str_cnt, 12)) if str_cnt < 20 else [],
        })
    elif kind == "Composite":
        # tUSBDCompositeDevice: vid(2), pid(2), maxPwr(2), pwrAttr(1), pad(1),
        # pfnCallback(4), ppui8StringDescriptors(4), ui32NumStringDescriptors(4),
        # ui32NumDevices(4), psDevices(4)
        cb = fw.u32(addr + 8)
        str_ptr = fw.u32(addr + 12)
        str_cnt = fw.u32(addr + 16)
        num_dev = fw.u32(addr + 20)
        ps_dev = fw.u32(addr + 24)
        info.update({
            "event_callback": f"0x{cb:08X}",
            "default_num_devices": num_dev,
            "psDevices": f"0x{ps_dev:08X}",
            "strings": decode_string_table(fw, str_ptr, min(str_cnt, 16)) if str_cnt < 32 else [],
        })
    elif kind == "HID_or_MSC":
        # Dump raw words so we can inspect both HID and MSC cleanly
        words = [f"0x{fw.u32(addr + i*4):08X}" for i in range(12)]
        info["raw_words"] = words
    return info


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("firmware", nargs="?", default="proprietary_fw/firmware_3.0.0.45.bin")
    parser.add_argument("--out", default="rev-eng/output/02_usb_descriptors.json")
    args = parser.parse_args()

    fw = FirmwareImage(args.firmware)

    # Known TivaWare device structures referenced in 0x00004708 (USBCompositeSetup)
    devices = [
        parse_tiva_device_struct(fw, 0x2000175C, "Composite"),
        parse_tiva_device_struct(fw, 0x2000164C, "CDC"),   # App UART
        parse_tiva_device_struct(fw, 0x200016B8, "CDC"),   # Aux UART
        parse_tiva_device_struct(fw, 0x2000152C, "Bulk"),  # Debug/CMD Port
        parse_tiva_device_struct(fw, 0x2000157C, "Bulk"),  # Data/EnergyTrace Port
        parse_tiva_device_struct(fw, 0x20000428, "HID_or_MSC"),   # CMSIS-DAP HID
        parse_tiva_device_struct(fw, 0x2000049C, "HID_or_MSC"),   # MSC or CMSIS-DAP 2.0
    ]

    # Also inspect the 8 tUSBBuffer structs passed to USBBufferInit (0x249FE) in main()
    usb_buf_addrs = [
        0x00035FF8, 0x00035E50, 0x0003601C, 0x00035E74,
        0x00035F08, 0x00035E08, 0x00035F2C, 0x00035E2C,
    ]
    usb_buffers = []
    for a in usb_buf_addrs:
        # tUSBBuffer: bTransmitBuffer(1+3pad), pfnCallback(4), pvCBData(4),
        # pfnTransfer(4), pfnAvailable(4), pvHandle(4), pui8Buffer(4), ui32BufferSize(4), sPrivateData(4)
        b_tx = fw.u32(a) & 1
        cb = fw.u32(a + 4)
        cb_data = fw.u32(a + 8)
        xfer = fw.u32(a + 12)
        avail = fw.u32(a + 16)
        handle = fw.u32(a + 20)
        buf_ptr = fw.u32(a + 24)
        buf_sz = fw.u32(a + 28)
        usb_buffers.append({
            "flash_addr": f"0x{a:08X}",
            "direction": "TX (Dev->Host)" if b_tx else "RX (Host->Dev)",
            "callback": f"0x{cb:08X}",
            "cb_data": f"0x{cb_data:08X}",
            "pfnTransfer": f"0x{xfer:08X}",
            "pfnAvailable": f"0x{avail:08X}",
            "pvHandle": f"0x{handle:08X}",
            "sram_buf": f"0x{buf_ptr:08X}",
            "buf_size": buf_sz,
        })

    # Find embedded class descriptor blocks in .data / .rodata
    raw_desc_blocks = {}
    for addr, max_len in [(0x000522CA, 48), (0x00052536, 48), (0x000525A2, 96), (0x000525FC, 48)]:
        raw = fw.read(addr, max_len)
        if raw:
            raw_desc_blocks[f"0x{addr:08X}"] = parse_raw_descriptors(raw)

    # Disassemble the middle of 0x00004708 to capture all config branches cleanly
    usb_init_disasm = fw.disasm_function(0x00004708, 600)

    # Search for MS OS 1.0 / 2.0 / WinUSB descriptors ("MSFT100", "WINUSB")
    msos_hits = []
    for needle in (b"MSFT100", b"M\x00S\x00F\x00T\x001\x000\x000\x00", b"WINUSB", b"DeviceInterfaceGUID"):
        pos = 0
        while True:
            idx = fw.data.find(needle, pos)
            if idx < 0:
                break
            msos_hits.append({"needle": needle.decode("latin1"), "addr": f"0x{fw.base + idx:08X}"})
            pos = idx + 1

    print("=== TivaWare USB Device Structs ===")
    for d in devices:
        print(f"  {d['kind']:<10s} @ {d['sram_addr']}  default VID:PID={d['default_vid']}:{d['default_pid']} maxPwr={d['max_power_mA']}mA")
        for k in ("event_callback", "ctrl_callback", "rx_callback", "tx_callback"):
            if k in d:
                print(f"      {k}: {d[k]} (data={d.get(k.replace('callback','cb_data'), '')})")
        if "strings" in d:
            for s in d["strings"]:
                print(f"      str[{s['index']}] @ {s['addr']}: {s['value']!r}")
        if "raw_words" in d:
            print(f"      words: {' '.join(d['raw_words'][:8])}")

    print("\n=== USB Buffers (tUSBBuffer) ===")
    for b in usb_buffers:
        print(f"  {b['flash_addr']} {b['direction']:<15s} handle={b['pvHandle']} cb={b['callback']} buf={b['sram_buf']} size={b['buf_size']}")

    print("\n=== Embedded Raw Class Descriptors ===")
    for addr, descs in raw_desc_blocks.items():
        print(f"  Block @ {addr}:")
        for item in descs:
            print(f"    {item.get('kind', item['type'])}: {item.get('fields', item['raw'])}")

    print("\n=== MS OS / WinUSB Descriptor Hits ===")
    for h in msos_hits:
        print(f"  {h['addr']}: {h['needle']!r}")

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({
        "devices": devices,
        "usb_buffers": usb_buffers,
        "raw_desc_blocks": raw_desc_blocks,
        "msos_hits": msos_hits,
    }, indent=2))
    print(f"\nSaved to {out_path}")


if __name__ == "__main__":
    main()
