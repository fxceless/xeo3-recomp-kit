#!/usr/bin/env python3
r"""Dump the fields of a XEX2 header that the guest kernel's loader checks.

Layout (all big-endian):
  0x00 'XEX2' | 0x04 module_flags | 0x08 pe_data_offset | 0x0C reserved
  0x10 security_info_offset | 0x14 optional_header_count | 0x18.. (key,val) pairs

  security_info:
    +0x000 header_size   +0x004 image_size
    +0x008 image_info: signature[256] +0x108 info_size +0x10C image_flags
           +0x110 load_address +0x114 image_hash[20] +0x128 import_table_count
           +0x12C import_digest[20] +0x140 media_id[16] +0x150 file_key[16]
           +0x160 export_table +0x164 header_hash[20] +0x178 game_region
    +0x17C allowed_media_types  +0x180 page_descriptor_count
"""
import struct, sys, os

MODULE_FLAGS = {
    0x01: "TITLE", 0x02: "EXPORTS_TO_TITLE", 0x04: "SYSTEM_DEBUGGER",
    0x08: "DLL_MODULE", 0x10: "MODULE_PATCH", 0x20: "PATCH_FULL",
    0x40: "PATCH_DELTA", 0x80: "USER_MODE",
}
MEDIA = {
    0x00000001: "HARDDISK", 0x00000002: "DVD_X2", 0x00000004: "DVD_CD",
    0x00000008: "DVD_5", 0x00000010: "DVD_9", 0x00000020: "SYSTEM_FLASH",
    0x00000040: "_UNKNOWN40", 0x00000080: "MEMORY_UNIT",
    0x00000100: "USB_MASS_STORAGE", 0x00000200: "NETWORK",
    0x00000400: "DIRECT_FROM_MEMORY", 0x00000800: "RAM_DRIVE",
    0x00001000: "SVOD", 0x01000000: "INSECURE_PACKAGE",
    0x02000000: "SAVEGAME", 0x04000000: "LOCAL_TRANSFER",
    0x08000000: "LIVE_SIGNED_PACKAGE", 0x10000000: "XBOX_PACKAGE",
}
IMAGE_FLAGS = {
    0x00000001: "MANUFACTURING_UTILITY", 0x00000002: "MANUFACTURING_SUPPORT_TOOLS",
    0x00000004: "XGD2_MEDIA_ONLY", 0x00000008: "DATA_CENTER_REQUIRED",
    0x00000010: "DATA_CENTER_AWARE", 0x00000020: "CARDEA_KEY",
    0x00000040: "XEIKA_KEY", 0x00000080: "TITLE_USER_MODE",
    0x00000100: "TITLE_SYSTEM_MODE", 0x00000200: "TITLE_LOCKED",
    0x00000400: "DASH_CONTEXT", 0x00000800: "REGION_FREE",
    0x00001000: "REVOCATION_CHECK_OPTIONAL", 0x00002000: "REVOCATION_CHECK_REQUIRED",
    0x00004000: "TITLE_INSECURE_SOCKETS", 0x00008000: "TITLE_REQUIRES_XBOX1_INTEROP",
    0x00010000: "TITLE_XBOX1_INTEROP", 0x00020000: "TITLE_PAL50_INCOMPATIBLE",
    0x00040000: "TITLE_INSECURE_UTILITY_DRIVE", 0x00080000: "TITLE_XAM_HOOKS",
    0x00100000: "TITLE_ACCESS_ONLINE_STORAGE", 0x00200000: "TITLE_MU_MAPPING",
    0x00400000: "TITLE_TAKEOVER", 0x00800000: "TITLE_LOCAL_ONLY",
    0x01000000: "TITLE_CONTENT_PACKAGE", 0x02000000: "TITLE_MFG_MODE",
}
SYSTEM_FLAGS = {
    0x00000001: "NO_FORCED_REBOOT", 0x00000002: "FOREGROUND_TASKS",
    0x00000004: "NO_ODD_MAPPING", 0x00000008: "HANDLE_MCE_INPUT",
    0x00000010: "RESTRICTED_HUD_FEATURES", 0x00000020: "HANDLE_GAMEPAD_DISCONNECT",
    0x00000040: "INSECURE_SOCKETS", 0x00000080: "XBOX1_INTEROPERABILITY",
    0x00000100: "DASH_CONTEXT", 0x00000200: "USES_GAME_VOICE_CHANNEL",
    0x00000400: "PAL50_INCOMPATIBLE", 0x00000800: "INSECURE_UTILITY_DRIVE",
    0x00001000: "XAM_HOOKS", 0x00002000: "ACCESS_PII", 0x00004000: "CROSS_PLATFORM_SYSTEM_LINK",
    0x00008000: "MULTIDISC_SWAP", 0x00010000: "MULTIDISC_INSECURE_MEDIA",
    0x00020000: "AP25_MEDIA", 0x00040000: "NO_CONFIRM_EXIT",
    0x00080000: "ALLOW_BACKGROUND_DOWNLOAD", 0x00100000: "CREATE_PERSISTABLE_RAMDRIVE",
    0x00200000: "INHERIT_PERSISTENT_RAMDRIVE", 0x00400000: "ALLOW_HUD_VIBRATION",
    0x00800000: "ACCESS_ALL_MUS", 0x01000000: "ACCESS_PARENTAL_CONTROLS",
    0x02000000: "TITLE_USES_ALL_MEMORY", 0x04000000: "ALLOW_AVATAR_GET_METADATA_BY_XUID",
    0x08000000: "ALLOW_CONTROLLER_SWAPPING", 0x10000000: "DASH_EXTENSIBILITY_MODULE",
    0x20000000: "ALLOW_NETWORK_READ_CANCEL", 0x40000000: "UNINTERRUPTABLE_READS",
    0x80000000: "REQUIRE_FULL_EXPERIENCE",
}
OPT_KEYS = {
    0x000002FF: "RESOURCE_INFO", 0x000003FF: "FILE_FORMAT_INFO",
    0x000004FF: "unknown_4FF", 0x000005FF: "DELTA_PATCH_DESCRIPTOR",
    0x000080FF: "BOUNDING_PATH", 0x00008105: "DEVICE_ID",
    0x00010001: "ORIGINAL_BASE_ADDRESS", 0x00010100: "ENTRY_POINT",
    0x00010201: "IMAGE_BASE_ADDRESS", 0x000103FF: "IMPORT_LIBRARIES",
    0x00018002: "CHECKSUM_TIMESTAMP", 0x00018102: "ENABLED_FOR_CALLCAP",
    0x00018200: "ENABLED_FOR_FASTCAP", 0x000183FF: "ORIGINAL_PE_NAME",
    0x000200FF: "STATIC_LIBRARIES", 0x00020104: "TLS_INFO",
    0x00020200: "DEFAULT_STACK_SIZE", 0x00020301: "DEFAULT_FS_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE", 0x00028002: "PAGE_HEAP_SIZE_AND_FLAGS",
    0x00030000: "SYSTEM_FLAGS", 0x00040006: "EXECUTION_INFO",
    0x00040201: "TITLE_WORKSPACE_SIZE", 0x00040310: "GAME_RATINGS",
    0x00040404: "LAN_KEY", 0x000405FF: "XBOX360_LOGO",
    0x000406FF: "MULTIDISC_MEDIA_IDS", 0x000407FF: "ALTERNATE_TITLE_IDS",
    0x00040801: "ADDITIONAL_TITLE_MEMORY", 0x00E10402: "EXPORTS_BY_NAME",
}


def bits(val, table):
    out = [n for b, n in sorted(table.items()) if val & b]
    left = val & ~sum(b for b in table if val & b)
    if left:
        out.append(f"?0x{left:08x}")
    return ",".join(out) or "-"


def dump(path):
    d = open(path, "rb").read(0x10000)
    print(f"=== {path}  ({os.path.getsize(path)} bytes)")
    if d[:4] != b"XEX2":
        print(f"    not XEX2: {d[:4]!r}")
        return
    mod_flags, pe_off, _res, sec, nopt = struct.unpack_from(">5I", d, 4)
    print(f"    module_flags = 0x{mod_flags:08x}  [{bits(mod_flags, MODULE_FLAGS)}]")
    print(f"    pe_data_offset = 0x{pe_off:x}  security_info @0x{sec:x}  opt_headers = {nopt}")

    hdr_size, img_size = struct.unpack_from(">2I", d, sec)
    info_size, image_flags, load_addr = struct.unpack_from(">3I", d, sec + 0x108)
    export_table = struct.unpack_from(">I", d, sec + 0x160)[0]
    game_region, allowed_media, page_count = struct.unpack_from(">3I", d, sec + 0x178)
    header_hash = d[sec + 0x164:sec + 0x178]
    media_id = d[sec + 0x140:sec + 0x150]
    print(f"    sec.header_size = 0x{hdr_size:x}  image_size = 0x{img_size:x}")
    print(f"    image_flags   = 0x{image_flags:08x}  [{bits(image_flags, IMAGE_FLAGS)}]")
    print(f"    load_address  = 0x{load_addr:08x}   export_table = 0x{export_table:08x}")
    print(f"    game_region   = 0x{game_region:08x}")
    print(f"    ALLOWED_MEDIA = 0x{allowed_media:08x}  [{bits(allowed_media, MEDIA)}]"
          f"   @file 0x{sec + 0x17C:x}")
    print(f"    page_descriptors = {page_count}   media_id = {media_id.hex()}")
    print(f"    header_hash = {header_hash.hex()}")

    opts = {}
    for i in range(nopt):
        k, v = struct.unpack_from(">2I", d, 0x18 + i * 8)
        opts[k] = v
    for k in sorted(opts):
        name = OPT_KEYS.get(k, f"0x{k:08x}")
        v = opts[k]
        extra = ""
        if k == 0x00030000:
            extra = f"  [{bits(v, SYSTEM_FLAGS)}]"
        if k == 0x00040006:  # execution info, value is an offset
            ei = d[v:v + 24]
            if len(ei) == 24:
                media_id2, ver, base_ver, title_id = struct.unpack_from(">4I", ei, 0)
                plat, exe_type, disc, disc_count, savegame = struct.unpack_from(">4BI", ei, 16)
                extra = (f"  -> titleId=0x{title_id:08x} ver=0x{ver:08x} base=0x{base_ver:08x} "
                         f"plat={plat} exeType={exe_type} disc={disc}/{disc_count}")
        if k == 0x000103FF:
            extra = f"  (import libs blob @0x{v:x})"
        print(f"    opt {k:08x} {name:<26} = 0x{v:08x}{extra}")
    print()


if __name__ == "__main__":
    for p in sys.argv[1:]:
        dump(p)
