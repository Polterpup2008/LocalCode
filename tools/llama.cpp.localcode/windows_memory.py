"""Measure file-backed resident pages with the native 64-bit Windows APIs.

psutil's Windows memory-map size field must not be treated as resident bytes.
"""
import ctypes
import os
from pathlib import Path

import psutil


def mapped_file_residency(pid, file):
    if os.name != 'nt':
        raise OSError('This measurement requires Windows')
    u32, pointer, size_t = ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t

    class Region(ctypes.Structure):
        _fields_ = [('base', pointer), ('allocation_base', pointer), ('allocation_protect', u32),
                    ('partition_id', ctypes.c_uint16), ('size', size_t),
                    ('state', u32), ('protect', u32), ('type', u32)]

    class Page(ctypes.Structure):
        _fields_ = [('address', pointer), ('attributes', size_t)]

    class SystemInfo(ctypes.Structure):
        _fields_ = [('architecture', u32), ('page_size', u32), ('minimum_address', pointer),
                    ('maximum_address', pointer), ('processor_mask', size_t), ('processors', u32),
                    ('processor_type', u32), ('allocation_granularity', u32),
                    ('processor_level', ctypes.c_uint16), ('processor_revision', ctypes.c_uint16)]

    if ctypes.sizeof(pointer) != 8:
        raise RuntimeError('Use 64-bit Python to inspect these 64-bit runtime processes')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    psapi = ctypes.WinDLL('psapi', use_last_error=True)
    kernel.OpenProcess.argtypes = [u32, ctypes.c_int, u32]
    kernel.OpenProcess.restype = pointer
    kernel.CloseHandle.argtypes = [pointer]
    kernel.CloseHandle.restype = ctypes.c_int
    kernel.VirtualQueryEx.argtypes = [pointer, pointer, ctypes.POINTER(Region), size_t]
    kernel.VirtualQueryEx.restype = size_t
    psapi.QueryWorkingSetEx.argtypes = [pointer, pointer, u32]
    psapi.QueryWorkingSetEx.restype = ctypes.c_int
    kernel.GetNativeSystemInfo.argtypes = [ctypes.POINTER(SystemInfo)]
    kernel.GetNativeSystemInfo.restype = None
    system = SystemInfo()
    kernel.GetNativeSystemInfo(ctypes.byref(system))
    page_size = system.page_size
    if not page_size:
        raise RuntimeError('Windows returned an invalid page size')
    handle = kernel.OpenProcess(0x0400 | 0x0010, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        target = str(Path(file).resolve()).casefold()
        addresses = {int(entry.addr, 16) for entry in psutil.Process(pid).memory_maps(grouped=False)
                     if entry.path.casefold() == target}
        result = {'file_virtual_bytes': 0, 'file_resident_bytes': 0, 'file_locked_bytes': 0,
                  'file_regions': len(addresses), 'page_bytes': page_size,
                  'method': 'VirtualQueryEx RegionSize + QueryWorkingSetEx Valid/Locked bits'}
        for address in sorted(addresses):
            region = Region()
            if kernel.VirtualQueryEx(handle, address, ctypes.byref(region), ctypes.sizeof(region)) != ctypes.sizeof(region):
                raise ctypes.WinError(ctypes.get_last_error())
            if region.state != 0x1000 or region.type != 0x40000:
                raise RuntimeError('Expected a committed file mapping')
            result['file_virtual_bytes'] += region.size
            end = region.base + region.size
            for begin in range(region.base, end, page_size * 65536):
                count = (min(end, begin + page_size * 65536) - begin + page_size - 1) // page_size
                pages = (Page * count)()
                for i in range(count):
                    pages[i].address = begin + i * page_size
                if not psapi.QueryWorkingSetEx(handle, ctypes.byref(pages), ctypes.sizeof(pages)):
                    raise ctypes.WinError(ctypes.get_last_error())
                for page in pages:
                    if page.attributes & 1:
                        result['file_resident_bytes'] += page_size
                        if page.attributes & (1 << 22):
                            result['file_locked_bytes'] += page_size
        return result
    finally:
        kernel.CloseHandle(handle)
