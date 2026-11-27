#!/usr/bin/env python3
"""Build build/adreno740/libAdreno740.a for Xiaomi Pad 6 Pro (liuqin, SM8475 Adreno 730).

Combines UniXNU's upstream ArmSmmu4K, AdrenoPM4, AdrenoFirmware, and generic NativeAdreno
runtime objects with liuqin's SM8475 Adreno 730 hardware bring-up (drivers/LiuqinAdreno)
and SHA-256-pinned A730 GMU/SQE/ZAP firmware blobs.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DEVICE = Path(os.environ.get('UNIXNU_DEVICE') or Path(__file__).resolve().parents[1]).resolve()
sys.path.insert(0, str(ROOT / 'scripts'))
from kernel_target import TRIPLE

c_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))
cxx_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))

OUT = ROOT / 'build/adreno740'
OUT.mkdir(parents=True, exist_ok=True)

# Adreno 730 RBBM_INT_0_MASK omits A740 bit 29 (IrqBitFuse); hardware masks it off on
# readback, which would fail NativeAdrenoIrq::programMask() verification.
irq_cpp = ROOT / 'drivers/Adreno740/NativeAdrenoIrq.cpp'
irq_src = irq_cpp.read_text()
if 'IrqBitTsbWrite | IrqBitFuse;' in irq_src:
    irq_cpp.write_text(irq_src.replace('IrqBitTsbWrite | IrqBitFuse;', 'IrqBitTsbWrite;', 1))
    print('Patched NativeAdrenoIrq.cpp kEnable for SM8475 Adreno 730 (dropped bit 29 IrqBitFuse)')

# SM8475 Adreno 730 routes 4 streams (gfx3d_user 0/0x400 + gfx3d_lpac 1/0x401) to bank 0.
# Extend NativeAdrenoGpuContext with a multi-stream bringUpStreams() / C entry point so
# ArmSmmu4KObserveStreams tracks all 4 routes while keeping 48-bit per-process VMs intact.
ctx_h = ROOT / 'drivers/Adreno740/NativeAdrenoGpuContext.h'
ctx_h_src = ctx_h.read_text()
if 'NativeAdrenoGpuContextBringupStreams' not in ctx_h_src:
    h_anchor = '    IOReturn bringUp(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,const unsigned slots[2],\n        const uint16_t sids[2],void (*progress)(uint32_t step),NativeAdrenoGmuContextReport *report);'
    h_ext = h_anchor + '\n    IOReturn bringUpStreams(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,const unsigned *slots,\n        const uint16_t *sids,unsigned stream_count,void (*progress)(uint32_t step),NativeAdrenoGmuContextReport *report);'
    c_anchor = 'extern "C" IOReturn NativeAdrenoGpuContextBringup(volatile uint8_t *smmu,size_t bytes,unsigned bank,unsigned asid,\n    unsigned slot0,unsigned sid0,unsigned slot1,unsigned sid1,void (*progress)(uint32_t),\n    NativeAdrenoGmuContextReport *report);'
    c_ext = c_anchor + '\nextern "C" IOReturn NativeAdrenoGpuContextBringupStreams(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,\n    const unsigned *slots,const uint16_t *sids,unsigned stream_count,void (*progress)(uint32_t),\n    NativeAdrenoGmuContextReport *report);'
    if h_anchor not in ctx_h_src or c_anchor not in ctx_h_src:
        sys.exit('NativeAdrenoGpuContext.h anchor not found for multi-stream bringup patch')
    ctx_h.write_text(ctx_h_src.replace(h_anchor, h_ext, 1).replace(c_anchor, c_ext, 1))
    print('Patched NativeAdrenoGpuContext.h with NativeAdrenoGpuContextBringupStreams')

ctx_cpp = ROOT / 'drivers/Adreno740/NativeAdrenoGpuContext.cpp'
ctx_cpp_src = ctx_cpp.read_text()
if 'NativeAdrenoGpuContextBringupStreams' not in ctx_cpp_src:
    cpp_sig = '''IOReturn NativeAdrenoGpuContext::bringUp(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,const unsigned slots[2],
    const uint16_t sids[2],void (*progress)(uint32_t),NativeAdrenoGmuContextReport *report)
{
    if(!report)return kIOReturnBadArgument;
    *report={};
    if(used_)return kIOReturnBusy;
    if(!io || !slots || !sids || bank>63 || !asid || asid>0xffff || slots[0]>127 || slots[1]>127 ||
        slots[0]==slots[1])return kIOReturnBadArgument;'''
    cpp_sig_new = '''IOReturn NativeAdrenoGpuContext::bringUp(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,const unsigned slots[2],
    const uint16_t sids[2],void (*progress)(uint32_t),NativeAdrenoGmuContextReport *report)
{
    return bringUpStreams(io,bank,asid,slots,sids,2,progress,report);
}
IOReturn NativeAdrenoGpuContext::bringUpStreams(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,const unsigned *slots,
    const uint16_t *sids,unsigned stream_count,void (*progress)(uint32_t),NativeAdrenoGmuContextReport *report)
{
    if(!report)return kIOReturnBadArgument;
    *report={};
    if(used_)return kIOReturnBusy;
    if(!io || !slots || !sids || !stream_count || stream_count>8 || bank>63 || !asid || asid>0xffff)return kIOReturnBadArgument;
    for(unsigned i=0;i<stream_count;++i) {
        if(slots[i]>127)return kIOReturnBadArgument;
        for(unsigned j=0;j<i;++j)if(slots[i]==slots[j])return kIOReturnBadArgument;
    }'''
    cpp_att = '''        const ArmSmmu4KStream routes[2]={{slots[0],sids[0]},{slots[1],sids[1]}};
        status=smmu_.attach(routes,2);'''
    cpp_att_new = '''        ArmSmmu4KStream routes[8]={};
        for(unsigned i=0;i<stream_count;++i)routes[i]={slots[i],sids[i]};
        status=smmu_.attach(routes,stream_count);'''
    cpp_c_anchor = '''    return board_gpu_context->bringUp(&io,bank,asid,slots,sids,progress,report);
}'''
    cpp_c_new = cpp_c_anchor + '''
extern "C" IOReturn NativeAdrenoGpuContextBringupStreams(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,
    const unsigned *slots,const uint16_t *sids,unsigned stream_count,void (*progress)(uint32_t),
    NativeAdrenoGmuContextReport *report)
{
    static ArmSmmu4KIo board_io;
    if(!report || board_gpu_context || !io || !slots || !sids || !stream_count || stream_count>8)return kIOReturnBadArgument;
    board_io=*io;
    board_gpu_context=new NativeAdrenoGpuContext;
    if(!board_gpu_context)return kIOReturnNoMemory;
    return board_gpu_context->bringUpStreams(&board_io,bank,asid,slots,sids,stream_count,progress,report);
}'''
    if cpp_sig not in ctx_cpp_src or cpp_att not in ctx_cpp_src or cpp_c_anchor not in ctx_cpp_src:
        sys.exit('NativeAdrenoGpuContext.cpp anchor not found for multi-stream bringup patch')
    ctx_cpp.write_text(
        ctx_cpp_src.replace(cpp_sig, cpp_sig_new, 1)
                   .replace(cpp_att, cpp_att_new, 1)
                   .replace(cpp_c_anchor, cpp_c_new, 1)
    )
    print('Patched NativeAdrenoGpuContext.cpp with bringUpStreams() (up to 8 SMMU streams)')

# SM8550-only and staged files excluded from the generic NativeAdreno set.
EXCLUDED_ADRENO740 = {
    'NativeAdrenoAccelerator.cpp',
    'NativeAdrenoClient.cpp',
    'NativeAdrenoGmuConfig.cpp',
    'NativeAdrenoGmuContext.cpp',
    'NativeAdrenoGmuMemory.cpp',
    'NativeAdrenoGmuPlatform.cpp',
    'NativeAdrenoGmuService.cpp',
    'NativeAdrenoKernelTime.cpp',
    'NativeAdrenoService.cpp',
    'NativeAdrenoSmmuService.cpp',
}

trace_hash = hashlib.sha256()
for path in sorted((ROOT / 'drivers/Adreno740').glob('Native*')):
    if path.is_file():
        trace_hash.update(path.name.encode())
        trace_hash.update(path.read_bytes())
trace_id = trace_hash.hexdigest()[:16]
(OUT / 'gpu-trace-build.json').write_text(json.dumps({
    'source_sha256': trace_hash.hexdigest(),
    'trace_source_id': trace_id,
    'target': TRIPLE,
}, indent=2) + '\n')

LIUQIN_ADRENO = DEVICE / 'drivers/LiuqinAdreno'
sources = []
sources += sorted(p for p in (ROOT / 'kits/ArmSmmu4K').glob('ArmSmmu4K*.c'))
sources += [ROOT / 'kits/AdrenoPM4/AdrenoPM4.c']
sources += [ROOT / 'kits/AdrenoFirmware/AdrenoFirmware.c']
sources += sorted(p for p in (ROOT / 'drivers/Adreno740').glob('NativeAdreno*.cpp')
                  if p.name not in EXCLUDED_ADRENO740)
sources += [
    LIUQIN_ADRENO / 'PongAdrenoPower.c',
    LIUQIN_ADRENO / 'PongAdrenoClocks.c',
    LIUQIN_ADRENO / 'PongAdrenoSmmu.c',
    LIUQIN_ADRENO / 'PongAdrenoHfi.c',
    LIUQIN_ADRENO / 'PongAdrenoGmu.c',
    LIUQIN_ADRENO / 'PongAdrenoZap.c',
    LIUQIN_ADRENO / 'PongAdrenoCp.cpp',
    LIUQIN_ADRENO / 'PongAdrenoPlatform.cpp',
    LIUQIN_ADRENO / 'LiuqinAdrenoPublish.cpp',
]

include_flags = [
    '-I', str(LIUQIN_ADRENO),
    '-I', str(ROOT / 'drivers/Adreno740'),
    '-I', str(ROOT / 'kits/ArmSmmu4K'),
    '-I', str(ROOT / 'kits/AdrenoPM4'),
    '-I', str(ROOT / 'kits/AdrenoFirmware'),
    '-I', str(ROOT / '.cache/xnu-work/osfmk'),
    '-DNATIVE_GPU_BUILD_ID=0x' + trace_id + 'ULL',
    '-Wall', '-Wextra', '-Werror',
    '-Wno-unused-parameter', '-Wno-incompatible-sysroot',
    '-Wno-c99-designator', '-Wno-reorder-init-list',
    '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
]

objects = []
for source in sources:
    recipe = c_recipe if source.suffix == '.c' else cxx_recipe
    prefix = 'liuqin-' if source.parent == LIUQIN_ADRENO else ''
    obj = OUT / (prefix + source.stem + ('.kit.o' if source.suffix == '.c' else '.o'))
    command, i = [], 0
    while i < len(recipe['arguments']):
        value = recipe['arguments'][i]
        if value == recipe['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif value in ('-MF', '-MJ'):
            i += 1
        elif value == '-Xclang' and recipe['arguments'][i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        i += 1
    command += include_flags
    result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
    if result.returncode:
        sys.exit(f'{source}:\n{result.stderr[-3000:]}')
    objects.append(obj)

# Embed SHA-256-verified Xiaomi liuqin Adreno 730 firmware blobs.
FW_DIR = ROOT / '.cache/firmware/liuqin-gpu'
FW_PINS = [
    ('_PongAdrenoGmuFirmware', 'gmu_gen70000.bin', 57272, 'b2d3f0e2a98fd6259ab9ffef16df63265542e6cc9a970d53d476e823d432042a'),
    ('_PongAdrenoSqeFirmware', 'a730_sqe.fw', 74676, 'e67d1829f57fc8326d806234b932dc3b78e28a9940df34c70752f17d5f38b7aa'),
    ('_PongAdrenoZapMdt', 'a730_zap.mdt', 3564, '1acb1e3ab783f75941eb427953c647d12ab4552c74a3c89c090f63636aaf2d9c'),
    ('_PongAdrenoZapCode', 'a730_zap.b01', 2236, '1fc98f97cf354b9704b20d0848f7b2739638539a57d88ecf5cc71ca1c5c4e6b9'),
    ('_PongAdrenoZapSignature', 'a730_zap.b02', 3416, '3d284460e2848dcfe6b03f73292a97eb579344e4763d1a8e36d0107c05865dac'),
]
assembly = ['.section __DATA_CONST,__const']
for label, filename, expected_bytes, expected_sha in FW_PINS:
    path = FW_DIR / filename
    blob = path.read_bytes() if path.is_file() else b''
    if len(blob) != expected_bytes or hashlib.sha256(blob).hexdigest() != expected_sha:
        sys.exit(f'liuqin Adreno firmware {filename} missing or hash mismatch (run fetch-liuqin-adreno-firmware.py first)')
    assembly += [
        '.p2align 12',
        f'.globl {label}',
        f'{label}:',
        f'.incbin {json.dumps(str(path))}',
        f'{label}_end:',
        '.p2align 3',
        f'.globl {label}Bytes',
        f'{label}Bytes:',
        f'.quad {label}_end-{label}',
    ]
(OUT / 'liuqin-firmware.S').write_text('\n'.join(assembly) + '\n')
subprocess.run(['xcrun', 'clang', '-target', TRIPLE, '-Wno-incompatible-sysroot',
                '-c', OUT / 'liuqin-firmware.S', '-o', OUT / 'liuqin-firmware.o'], check=True)
objects.append(OUT / 'liuqin-firmware.o')

archive = OUT / 'libAdreno740.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
defined = subprocess.run(['xcrun', 'nm', '-g', archive], capture_output=True, text=True).stdout
required = (
    '_liuqin_adreno_start_optional',
    '_native_adreno_publish',
    '_NativeAdrenoGpuContextBringupStreams',
    '_native_adreno_fences_detail',
    '_ArmSmmu4KAttachStreams',
    '_AdrenoPM4Type7',
    '_AdrenoFirmwarePlanGmu',
    '_PongAdrenoGmuStart',
    '_PongAdrenoStartZap',
)
missing = [s for s in required if f' T {s}' not in defined and f' S {s}' not in defined]
if missing:
    sys.exit('Required Adreno 730 symbols absent from libAdreno740.a: ' + ', '.join(missing))
print(f'PASS: built liuqin SM8475 Adreno 730 archive ({len(objects)} objects, all required symbols present)')
