"""Read a Windows minidump far enough to answer which __fastfail reason fired.

The host and the guest both lack a debugger (Windows Kits ships no Debuggers
directory here), so cdb is not available.  The minidump format is documented and
the one datum that matters is small: a 0xC0000409 exit is `__fastfail`, whose
reason is a small integer.  Distinguishing reason 2 (stack cookie) from 3
(corrupted heap list) from 7 (CRT abort, i.e. std::terminate) decides which class
of defect we are hunting -- and that is readable without symbols.

Usage: python read-minidump.py <dump>
"""
import struct
import sys

STREAM_NAMES = {
    3: "ThreadList", 4: "ModuleList", 5: "MemoryList", 6: "Exception",
    7: "SystemInfo", 9: "Memory64List", 15: "MemoryInfoList",
}

# __fastfail reasons (winnt.h FAST_FAIL_*)
FAST_FAIL = {
    0: "LEGACY_GS_VIOLATION",
    1: "VTGUARD_CHECK_FAILURE",
    2: "STACK_COOKIE_CHECK_FAILURE",
    3: "CORRUPT_LIST_ENTRY (heap metadata)",
    4: "INCORRECT_STACK",
    5: "INVALID_ARG",
    6: "GS_COOKIE_INIT",
    7: "FATAL_APP_EXIT (CRT abort / std::terminate)",
    8: "RANGE_CHECK_FAILURE",
    9: "UNSAFE_REGISTRY_ACCESS",
    10: "INVALID_STATE",
}

MODULE_SIZE = 108  # sizeof(MINIDUMP_MODULE) with 4-byte packing
MODULE_NAME_RVA_OFFSET = 20


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    with open(sys.argv[1], "rb") as handle:
        blob = handle.read()

    signature, version, n_streams, dir_rva = struct.unpack_from("<IIII", blob, 0)
    if signature != 0x504D444D:
        print("not a minidump: signature 0x%08X" % signature)
        return 1
    print("minidump: version %d, %d streams, %d bytes" % (version, n_streams, len(blob)))

    streams = {}
    for index in range(n_streams):
        s_type, size, rva = struct.unpack_from("<III", blob, dir_rva + index * 12)
        streams[s_type] = (size, rva)
        print("  stream %2d %-15s size=%-9d rva=0x%X"
              % (s_type, STREAM_NAMES.get(s_type, "?"), size, rva))

    if 7 in streams:
        _, rva = streams[7]
        # MINIDUMP_SYSTEM_INFO: 3 x USHORT, then NumberOfProcessors/ProductType
        # as two UCHARs, then four ULONG32 (MajorVersion, MinorVersion,
        # BuildNumber, PlatformId).
        arch, level, revision, ncpu, product, major, minor, build = struct.unpack_from(
            "<HHHBBIII", blob, rva)
        print("  arch=%d (9=x64)  cpus=%d  windows=%d.%d.%d"
              % (arch, ncpu, major, minor, build))

    if 6 not in streams:
        print("\nno exception stream -- nothing faulted in this dump")
        return 1

    _, rva = streams[6]
    thread_id = struct.unpack_from("<I", blob, rva)[0]
    ex_rva = rva + 8  # MINIDUMP_EXCEPTION follows the 4-byte id + 4 pad
    code, flags, nested, address, n_params, _pad = struct.unpack_from(
        "<IIQQII", blob, ex_rva)
    params = struct.unpack_from("<15Q", blob, ex_rva + 32)

    print()
    print("faulting thread id : %d" % thread_id)
    print("exception code     : 0x%08X" % code)
    print("exception address  : 0x%016X" % address)
    print("nested record      : %s" % ("yes" if nested else "no"))
    print("number parameters  : %d" % n_params)
    for index in range(min(n_params, 15)):
        print("  param[%2d]        : 0x%016X  (%d)"
              % (index, params[index], params[index]))

    print()
    if code == 0xC0000409:
        reason = params[0] if n_params >= 1 else None
        print("VERDICT: __fastfail reason %s -> %s"
              % (reason, FAST_FAIL.get(reason, "unknown")))
    elif code == 0xC0000005:
        print("VERDICT: access violation, param[0]=%d (0=read, 1=write, 8=execute), "
              "faulting address 0x%X" % (params[0], params[1]))
    elif code == 0xC00000FD:
        print("VERDICT: stack overflow")
    else:
        print("VERDICT: code 0x%08X is not a fastfail/AV/stack-overflow" % code)

    if 4 in streams:
        _, mod_rva = streams[4]
        count = struct.unpack_from("<I", blob, mod_rva)[0]
        print()
        print("modules: %d" % count)
        hit = None
        for index in range(count):
            entry = mod_rva + 4 + index * MODULE_SIZE
            base, size = struct.unpack_from("<QI", blob, entry)
            if base <= address < base + size:
                name_rva = struct.unpack_from("<I", blob,
                                              entry + MODULE_NAME_RVA_OFFSET)[0]
                length = struct.unpack_from("<I", blob, name_rva)[0]
                name = blob[name_rva + 4:name_rva + 4 + length].decode("utf-16-le")
                hit = (base, size, name)
        if hit:
            base, size, name = hit
            print("faulting address is inside: %s" % name)
            print("  base 0x%016X  RVA 0x%X  (symbolize with dumpbin /symbols)"
                  % (base, address - base))
        else:
            print("faulting address is not inside any listed module "
                  "(likely the heap or a JIT page)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
