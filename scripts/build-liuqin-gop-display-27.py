#!/usr/bin/env python3
"""Bind the owned GOP backend to the original IOMobileFramebufferLegacy of the iPadOS 27 (24A446) fileset.

The class ABI (vtable targets and diversities, instance size, field offsets, original bindings) comes from the reviewed fixture
legacy-display-24A446.json. The kernel is arm64e: every vtable entry of the owned subclass must carry the diversity the original
callers authenticate with, so every original slot is declared under a name whose discriminator is that slot's diversity. Original
code is bound at its fileset address plus the collection slide; no original instruction or vtable is changed.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
from kernel_target import ARCH, IOS_VERSION, TRIPLE
# The OS profile's build selects the class fixtures: the reviewed iPadOS 27 ones, or the macOS 27.3 ones that
# gen-board-abi.py writes from the symbols of the macOS collection.
BUILD = os.environ.get('UNIXNU_IPSW_BUILD') or '24A446'
MACOS = os.environ.get('UNIXNU_XNU_TARGET') == 'macosx'
if ARCH != 'arm64e' or IOS_VERSION.split('.')[0] != '27' or (BUILD, MACOS) not in (('24A446', False), ('26C5035r', True)):
    raise SystemExit('This Legacy display builder supports arm64e iPadOS 27 (24A446) and macOS 27.3 (26C5035r).')
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-legacy-display'
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
ABI_PATH = Path(__file__).resolve().with_name(f'legacy-display-{BUILD}.json')
abi = json.loads(ABI_PATH.read_text())
# IOService's vtable length in this kernel and the pure-virtual target of the collection: fixed for iPadOS 27, recorded
# in the fixture for macOS.
BASE = abi.get('base_slots', 168)
slide = int(abi['collection_slide'], 16)
# Original targets are fixed collection addresses. XnuPrelink recomputes the
# BTI/B adapter displacement after placing native text; no prior build is needed.
slots = [(int(t, 16), int(d, 16)) for t, d in abi['slots']]
D, R, N = abi['display'], abi['request'], abi.get('notify', {})


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=300, **kw)


def require(v, why):
    if not v:
        raise SystemExit(why)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def discriminator(name):
    """clang ptrauth_string_discriminator: SipHash-2-4 with the fixed key, modulo 0xffff, plus one."""
    mask = (1 << 64) - 1
    rotl = lambda x, b: ((x << b) | (x >> (64 - b))) & mask  # noqa: E731
    key = bytes([0xb5, 0xd4, 0xc9, 0xeb, 0x79, 0x10, 0x4a, 0x79, 0x6f, 0xec, 0x8b, 0x1b, 0x42, 0x87, 0x81, 0xd4])
    k0, k1 = struct.unpack('<2Q', key)
    v = [0x736f6d6570736575 ^ k0, 0x646f72616e646f6d ^ k1, 0x6c7967656e657261 ^ k0, 0x7465646279746573 ^ k1]

    def rounds(n):
        for _ in range(n):
            v[0] = (v[0] + v[1]) & mask; v[1] = rotl(v[1], 13) ^ v[0]; v[0] = rotl(v[0], 32)
            v[2] = (v[2] + v[3]) & mask; v[3] = rotl(v[3], 16) ^ v[2]
            v[0] = (v[0] + v[3]) & mask; v[3] = rotl(v[3], 21) ^ v[0]
            v[2] = (v[2] + v[1]) & mask; v[1] = rotl(v[1], 17) ^ v[2]; v[2] = rotl(v[2], 32)
    data = name.encode()
    full = len(data) & ~7
    for i in range(0, full, 8):
        m = struct.unpack_from('<Q', data, i)[0]
        v[3] ^= m; rounds(2); v[0] ^= m
    b = len(data) << 56 & mask
    for i, c in enumerate(data[full:]):
        b |= c << (8 * i)
    v[3] ^= b; rounds(2); v[0] ^= b; v[2] ^= 0xff; rounds(4)
    return (v[0] ^ v[1] ^ v[2] ^ v[3]) % 0xffff + 1


# Prototypes of the original slots the owned backend overrides or calls; every other slot is an opaque dispatch target.
PROTOTYPES = {
    'isMainDisplay': 'bool isMainDisplay()', 'getDotPitch': 'IOReturn getDotPitch(uint32_t *)',
    'supportedFrameInfo': 'IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0',
    'getDisplayDimensions': 'void getDisplayDimensions(uint32_t *,uint32_t *)', 'startHardware': 'bool startHardware()',
    'kickSwapQueue': 'void kickSwapQueue()', 'completeCurrentSwap': 'void completeCurrentSwap()',
    'validateSwap': 'bool validateSwap(const OriginalLegacySwapRequest *)',
    'getRefreshAttributes': 'bool getRefreshAttributes(uint64_t *,bool)',
    'collectCompletionReports': 'IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0',
    'applyStateRequest': 'int applyStateRequest(OriginalLegacyStateRequest *,bool)',
    # Slot 366: swap_wait_gated calls it (this only, result logged) after a swap waited 5 s, then cancels its own queue (slot 367).
    'cancelSwapsAfterTimeout': 'bool cancelSwapsAfterTimeout()',
    # Slot 316 initializes optional family state and returns void; x0 is not an IOSurface result.
    'create_default_fb_surface': 'void create_default_fb_surface()'}
# A macOS fixture carries its own prototypes (the reference labels plus the family hooks it implements).
PROTOTYPES = abi.get('prototypes', PROTOTYPES)
by_slot = {slot: name for name, slot in abi['methods'].items()}
PURE_TARGET = int(abi['pure_target'], 16) if 'pure_target' in abi else slots[214][0]
pure = {i for i, (t, _) in enumerate(slots) if t == PURE_TARGET}
renames = []
for base, (name, suffix) in abi['names'].items():
    mangled = f'_ZN25IOMobileFramebufferLegacy{len(name)}{name}{suffix}'
    slot = abi['methods'].get(base) or int(base[len('opaque'):])
    require(discriminator(mangled) == slots[slot][1], f'Reviewed name {name} has the diversity of slot {slot}')
    renames.append(f'#define {base} {name}\n')
leaves = ' '.join(f'LEAF({i})' for i in abi['leaves'])
# The macOS family's swap request and notification fields (gen-board-abi.py); the iPadOS header has none of them.
MACOS_CONSTANTS = ('#define NATIVE_LEGACY_MACOS 1\n'
    f"static constexpr unsigned kLegacyCurrentSwap={D['current']};\n"
    f"static constexpr unsigned kRequestDropReasons={R.get('drop_reasons', 0)},kRequestSwapInfoA={R.get('swap_info_a', 0)},kRequestSwapInfoB={R.get('swap_info_b', 0)};\n"
    f"static constexpr unsigned kLegacySwapInfoTime={D.get('swap_info_time', 0)},kLegacySwapInfoHeld={D.get('swap_info_held', 0)};\n"
    f"static constexpr unsigned kNotifyAsyncReference={N.get('async_reference', 0)},kNotifyMask={N.get('mask', 0)},kNotifyLastTime={N.get('last_time', 0)};\n"
    ) if MACOS else ''
header = f'''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the original iPadOS 27 (24A446) class, generated from {ABI_PATH.name}. Each original slot is declared
// under a name whose pointer-authentication discriminator is the original slot's diversity (the names below), so original
// callers authenticate the owned subclass's entries. Only qualified base prototypes may be called from source.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
extern "C" OSObject *native_legacy_default_surface_create(OSObject *,uint32_t,uint32_t);
struct OriginalLegacySwapRequest;
struct OriginalLegacyStateRequest {{uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];}};
static constexpr unsigned kLegacyVsyncClients={D['vsync_clients']};
static constexpr unsigned kRequestRect0={R['rect0']},kRequestRect1={R['rect1']},kRequestMasks={R['masks']},kRequestOptions={R['options']},kRequestDims={R['dims']},kRequestSurfaces={R['surfaces']};
#define NATIVE_LEGACY_LEAVES(LEAF) {leaves}
#define NATIVE_LEGACY_DEFAULT_SURFACE 1
#define NATIVE_LEGACY_CANCEL_SWAPS 1
{MACOS_CONSTANTS}static constexpr unsigned kLegacyDefaultSurface={D['default_surface']};
{''.join(renames)}class IOMobileFramebufferLegacy : public IOService {{
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[{abi['instance_size']}-sizeof(IOService)];
public:
 bool installDefaultSurface(uint32_t width,uint32_t height) {{
  auto self=reinterpret_cast<unsigned char *>(this);
  OSObject *root=nullptr,*surface=nullptr;
  memcpy(&surface,self+kLegacyDefaultSurface,sizeof(surface));
  if(surface)return true;
  memcpy(&root,self+{D['surface_root']},sizeof(root));
  if(!root)return false;
  surface=native_legacy_default_surface_create(root,width,height);
  if(!surface)return false;
  // Transfer the factory reference to the family; its stop path releases this field.
  memcpy(self+kLegacyDefaultSurface,&surface,sizeof(surface));
  return true;
 }}
 void installBootDimensions(uint32_t width,uint32_t height) {{
  memcpy(reinterpret_cast<unsigned char *>(this)+{D['width']},&width,4);memcpy(reinterpret_cast<unsigned char *>(this)+{D['height']},&height,4);
 }}
 const OriginalLegacySwapRequest *pendingSwapRequest() const {{
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+{D['head']},sizeof(request));return request;
 }}
 // Tail queue: head {D['head']}, tail pointer {D['tail']}, each request's next {R['next']} and previous-link {R['prev']}; the head moves
 // to the current-swap slot {D['current']} for the original completion body.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {{
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+{D['current']},8);memcpy(&head,self+{D['head']},8);memcpy(&tail,self+{D['tail']},8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+{R['next']},8);memcpy(&previous,bytes+{R['prev']},8);
  if(previous!=self+{D['head']})return false;
  if(next) {{
   const void *link=self+{D['head']};
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+{R['prev']},&link,8);
  }} else if(tail!=bytes+{R['next']})return false;
  memcpy(self+{D['current']},&request,8);
  if(next) {{memcpy(self+{D['head']},&next,8);}}
  else {{const void *empty=nullptr,*emptyTail=self+{D['head']};memcpy(self+{D['head']},&empty,8);memcpy(self+{D['tail']},&emptyTail,8);}}
  return true;
 }}
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
for i in range(BASE, len(slots)):
    if i in by_slot:
        header += f' virtual {PROTOTYPES[by_slot[i]]};\n'
    elif i in pure:
        header += f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
    else:
        header += f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'
header += f'}};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)=={abi["instance_size"]},"Actual original instance extent");\n'
(OUT / 'OriginalLegacyFramebufferABI.h').write_text(header)

# Adapt NativeLegacyDisplay DPU direct scanout to auto-detect liuqin's active DPU 8.1 pipes, CTL, boot IOVA, and stride.
nld_path = ROOT / 'drivers/GopFramebuffer/NativeLegacyDisplay.cpp'
nld_text = nld_path.read_text()
if 'directBootStride' not in nld_text:
    replacements = [
        (
            '    bool directOn=false,directBoot=true;\n'
            '    static constexpr uint32_t kVig[2]={0x4000,0x6000},kCtlFlush=0x15018,kBootIova=0xb8000000U,kBootStride=0x2fa0;\n'
            '    void startDirect() {\n'
            '        unsigned mode=0;PE_parse_boot_argn("dpu-scanout",&mode,sizeof(mode));\n'
            '        if(mode<2 || !&native_ufs_dpu_map || !&native_ufs_dpu_state || !(native_ufs_dpu_state>>31) || !primary)return;\n'
            '        IODeviceMemory *memory=IODeviceMemory::withRange(0xae01000,0x8f000);\n'
            '        mdpMap=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;\n'
            '        if(memory)memory->release();\n'
            '        directPages=static_cast<uint64_t *>(IOMallocData(8192*sizeof(uint64_t)));\n'
            '        IOWorkLoop *loop=getWorkLoop();\n'
            '        latchTimer=loop?IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::latchFired):nullptr;\n'
            '        fenceTimer=loop?IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::fenceFired):nullptr;\n'
            '        if(!mdpMap || !directPages || !latchTimer || !fenceTimer || loop->addEventSource(fenceTimer)!=kIOReturnSuccess || loop->addEventSource(latchTimer)!=kIOReturnSuccess) {\n'
            '            OSSafeReleaseNULL(mdpMap);OSSafeReleaseNULL(latchTimer);OSSafeReleaseNULL(fenceTimer);\n'
            '            if(directPages){IOFreeData(directPages,8192*sizeof(uint64_t));directPages=nullptr;}\n'
            '            return;\n'
            '        }\n'
            '        mdp=reinterpret_cast<volatile uint32_t *>(mdpMap->getVirtualAddress());\n'
            '        directOn=true;gDirectDisplay=this;\n'
            '    }',
            '    bool directOn=false,directBoot=true;\n'
            '    uint32_t directPipes[4]={0x4000,0x6000,0,0},directPipeDeltas[4]={0,0,0,0};\n'
            '    unsigned directPipeCount=0;\n'
            '    uint32_t directCtlFlush=0x15018,directFlushMask=0x3;\n'
            '    uint32_t directBootIova=0xb8000000U,directBootStride=0x1c20;\n'
            '    void startDirect() {\n'
            '        unsigned mode=0;PE_parse_boot_argn("dpu-scanout",&mode,sizeof(mode));\n'
            '        if(mode<2 || !&native_ufs_dpu_map || !&native_ufs_dpu_state || !(native_ufs_dpu_state>>31) || !primary)return;\n'
            '        IODeviceMemory *memory=IODeviceMemory::withRange(0xae01000,0x8f000);\n'
            '        mdpMap=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;\n'
            '        if(memory)memory->release();\n'
            '        directPages=static_cast<uint64_t *>(IOMallocData(8192*sizeof(uint64_t)));\n'
            '        IOWorkLoop *loop=getWorkLoop();\n'
            '        latchTimer=loop?IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::latchFired):nullptr;\n'
            '        fenceTimer=loop?IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::fenceFired):nullptr;\n'
            '        if(!mdpMap || !directPages || !latchTimer || !fenceTimer || loop->addEventSource(fenceTimer)!=kIOReturnSuccess || loop->addEventSource(latchTimer)!=kIOReturnSuccess) {\n'
            '            OSSafeReleaseNULL(mdpMap);OSSafeReleaseNULL(latchTimer);OSSafeReleaseNULL(fenceTimer);\n'
            '            if(directPages){IOFreeData(directPages,8192*sizeof(uint64_t));directPages=nullptr;}\n'
            '            return;\n'
            '        }\n'
            '        mdp=reinterpret_cast<volatile uint32_t *>(mdpMap->getVirtualAddress());\n'
            '        PE_Video video={};\n'
            '        if(!PE_current_console(&video) && video.v_baseAddr && video.v_rowBytes) {\n'
            '            directBootIova=uint32_t(video.v_baseAddr);\n'
            '            directBootStride=uint32_t(video.v_rowBytes);\n'
            '        }\n'
            '        static constexpr uint32_t kCandPipes[8]={0x4000,0x6000,0x8000,0xa000,0x24000,0x26000,0x28000,0x2a000};\n'
            '        static constexpr uint32_t kCandBits[8]={1u<<0,1u<<1,1u<<2,1u<<18,1u<<11,1u<<12,1u<<24,1u<<25};\n'
            '        directPipeCount=0;directFlushMask=0;\n'
            '        for(unsigned k=0;k<8 && directPipeCount<4;++k) {\n'
            '            const uint32_t p=kCandPipes[k];\n'
            '            const uint32_t srcSize=mdp[p/4];\n'
            '            const uint32_t src0=mdp[(p+0x14)/4];\n'
            '            const uint32_t stride0=mdp[(p+0x24)/4]&0xffffu;\n'
            '            if(srcSize && stride0==directBootStride && src0>=directBootIova && src0<directBootIova+directBootStride) {\n'
            '                directPipes[directPipeCount]=p;\n'
            '                directPipeDeltas[directPipeCount]=src0-directBootIova;\n'
            '                directFlushMask|=kCandBits[k];\n'
            '                ++directPipeCount;\n'
            '            }\n'
            '        }\n'
            '        directCtlFlush=0x15018;\n'
            '        for(unsigned c=0;c<4;++c) {\n'
            '            const uint32_t ctl=0x15000u+c*0x1000u;\n'
            '            if(mdp[(ctl+0xf4u)/4]){directCtlFlush=ctl+0x18u;break;}\n'
            '        }\n'
            '        if(!directPipeCount || !directFlushMask) {\n'
            '            IOLog("dpu-scanout: no active pipe matched boot=0x%x stride=%u; staying on copy path\\n",directBootIova,directBootStride);\n'
            '            return;\n'
            '        }\n'
            '        IOLog("dpu-scanout: active pipes=%u mask=0x%x ctl=0x%x boot=0x%x/%u\\n",\n'
            '              directPipeCount,directFlushMask,directCtlFlush,directBootIova,directBootStride);\n'
            '        directOn=true;gDirectDisplay=this;\n'
            '    }',
        ),
        (
            '    void flip(uint32_t iova,uint32_t stride) {\n'
            '        shownStride=stride;\n'
            '        for(uint32_t vig:kVig){mdp[(vig+0x14)/4]=iova;mdp[(vig+0x24)/4]=stride&0xffff;}\n'
            '        __asm__ volatile("dsb sy" ::: "memory");\n'
            '        mdp[kCtlFlush/4]=0x3;\n'
            '        __asm__ volatile("dsb sy" ::: "memory");\n'
            '    }',
            '    void flip(uint32_t iova,uint32_t stride) {\n'
            '        shownStride=stride;\n'
            '        for(unsigned k=0;k<directPipeCount;++k) {\n'
            '            const uint32_t p=directPipes[k];\n'
            '            mdp[(p+0x14)/4]=iova+directPipeDeltas[k];\n'
            '            mdp[(p+0x24)/4]=stride&0xffff;\n'
            '        }\n'
            '        __asm__ volatile("dsb sy" ::: "memory");\n'
            '        mdp[directCtlFlush/4]=directFlushMask;\n'
            '        __asm__ volatile("dsb sy" ::: "memory");\n'
            '    }',
        ),
        (
            '        gpuTraceEvent(GpuTraceDisplayLatch,0,traceId(latchRequest),mdp[kCtlFlush/4],ns,shownSlot,pendingSlot);\n'
            '        if((mdp[kCtlFlush/4]&0x3) && ns<50000000ull){latchTimer->setTimeoutUS(500);return;}',
            '        gpuTraceEvent(GpuTraceDisplayLatch,0,traceId(latchRequest),mdp[directCtlFlush/4],ns,shownSlot,pendingSlot);\n'
            '        if((mdp[directCtlFlush/4]&directFlushMask) && ns<50000000ull){latchTimer->setTimeoutUS(500);return;}',
        ),
        (
            '    void directToBoot() {\n'
            '        if(!directOn || directBoot)return;\n'
            '        flip(kBootIova,kBootStride);directBoot=true;shownSlot=-1;\n'
            '        native_legacy_direct[8]=native_legacy_direct[8]+1;\n'
            '    }',
            '    void directToBoot() {\n'
            '        if(!directOn || directBoot)return;\n'
            '        flip(directBootIova,directBootStride);directBoot=true;shownSlot=-1;\n'
            '        native_legacy_direct[8]=native_legacy_direct[8]+1;\n'
            '    }',
        ),
    ]
    for old_s, new_s in replacements:
        require(old_s in nld_text, f'NativeLegacyDisplay.cpp DPU patch target missing: {old_s[:60]}')
        nld_text = nld_text.replace(old_s, new_s, 1)
    nld_path.write_text(nld_text)

print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeLegacyDisplay'], text=True), end='')



def macho(data):
    """Load commands, segments and sections of an arm64e relocatable object."""
    magic, cpu, _, _, count = struct.unpack_from('<5I', data)
    require(magic == 0xfeedfacf and cpu == 0x100000c, 'arm64 Mach-O object')
    commands, segments, offset = [], [], 32
    for _ in range(count):
        command, size = struct.unpack_from('<2I', data, offset)
        commands.append((command, offset, size))
        if command == 0x19:
            nsects = struct.unpack_from('<I', data, offset + 64)[0]
            sections = []
            for i in range(nsects):
                at = offset + 72 + 80 * i
                va, length, fileoff = struct.unpack_from('<QQI', data, at + 32)
                sections.append({'name': data[at:at + 16].rstrip(b'\0').decode(), 'va': va, 'size': length,
                                 'fileoff': fileoff, 'command_offset': at})
            segments.append({'sections': sections})
        offset += size
    return {'commands': commands, 'segments': segments}


obj = OUT / 'NativeLegacyDisplay.o'
raw = obj.read_bytes()
image = macho(raw)
st = next(p for c, p, _ in image['commands'] if c == 2)
sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
symbols = []
for i in range(count):
    ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
    symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
vtable = next(va for name, va, _ in symbols if name == '__ZTV19NativeLegacyDisplay')
entries = {}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va'] <= vtable < sec['va'] + sec['size']:
            ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
            for i in range(rc):
                offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                # extern, 8 bytes, ARM64_RELOC_AUTHENTICATED_POINTER
                if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == 11:
                    word = struct.unpack_from('<Q', raw, sec['fileoff'] + offset)[0]
                    entries[sec['va'] + offset] = (symbols[bits & 0xffffff][0], word)
compiled = [entries.get(vtable + 16 + i * 8) for i in range(len(slots))]
require(all(compiled) and vtable + 16 + len(slots) * 8 not in entries, f'Exact owned {len(slots)}-entry vtable')
for i, (name, word) in enumerate(compiled):
    require(word >> 63 and word >> 48 & 1 and word >> 49 & 3 == 0 and (word >> 32) & 0xffff == slots[i][1],
            f'Slot {i} ({name}) signed with IA, address diversity and the original diversity {slots[i][1]:#x}')
family = range(*(int(x, 16) for x in abi['family_text'])) if 'family_text' in abi else range(0xfffffe000a27c000, 0xfffffe000a29a000)
bindings = {name: int(va, 16) + slide for name, va in abi['bindings'].items()}
owned, native = [], []
for i, (name, _) in enumerate(compiled):
    if name.startswith(('__ZN19NativeLegacyDisplay', '__ZNK19NativeLegacyDisplay')):
        owned.append(i)
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure, f'No unresolved original abstract target at slot {i}')
        require(bindings.get(name, slots[i][0] + slide) == slots[i][0] + slide, 'One original address per name')
        bindings[name] = slots[i][0] + slide
    else:
        require(i < BASE and slots[i][0] not in family, f'Slot {i} ({name}) is inherited unchanged by the original family')
        native.append(i)
require(pure <= set(owned), 'Every original abstract slot is implemented by the owned backend')
undefined = {n for n, _, k in symbols if k == 1 and 'IOMobileFramebufferLegacy' in n}
require(undefined <= set(bindings), 'Every original-class import is bound: ' + ', '.join(sorted(undefined - set(bindings))))

asm = OUT / 'original-bindings.S'
# Absolute symbols get no fixup: a pointer to one is neither signed nor moved. Every original function is therefore reached through
# an owned stub in kernel text (the vtable signs and moves it like any native code), whose branch lands on the original. Data
# symbols are reached through GOT entries, which keep their absolute value.
DATA = {'__ZN25IOMobileFramebufferLegacy10gMetaClassE', '__ZTV25IOMobileFramebufferLegacy'}
text, addresses = '.text\n.p2align 2\n', ''
for name, va in sorted(bindings.items()):
    target = f'{va:#x}'
    if name in DATA:
        # Reached through GOT entries: plain pointers that the emitter does not move.
        addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
    else:
        # The absolute target lives in the other object, so the branch stays a relocation the linker resolves.
        addresses += f'.globl {name}.original\n.set {name}.original, {target}\n'
        text += f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
asm.write_text(text)
(OUT / 'original-addresses.S').write_text(addresses)
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk, '-c', asm, '-o', OUT / 'original-bindings.o'])
run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk, '-c', OUT / 'original-addresses.S', '-o', OUT / 'original-addresses.o'])
llvm = Path(run(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'
late = OUT / 'NativeLegacyDisplay.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp', obj, late])

# The board probe and the owned support code, compiled with the kernel's own C++ flags.
r = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
source = ROOT / 'kits/sheng/LegacyDisplayProbe.cpp'


def compile_cpp(src, dst, extra=()):
    cmd, args, i = [], r['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == r['file']:
            cmd.append(str(src))
        elif value == '-o':
            cmd += ['-o', str(dst)]
            i += 1
        elif value in ('-MF', '-MT', '-MJ'):
            i += 1
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W'):
            cmd.append(value)
        i += 1
    cmd += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-I', str(ROOT / '.cache/xnu-work/osfmk'), *extra]
    run(cmd, cwd=r['directory'])


def compile_c(src, dst, recipe='osfmk/DEVELOPMENT/startup.o.json'):
    """A C board core with the flags of one kernel C file (osfmk for the cores, bsd for the VFS readers)."""
    rc = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837' / recipe).read_text().strip().rstrip(','))
    cmd, args, i = [], rc['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == rc['file']:
            cmd.append(str(src))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            if value == '-o':
                cmd += ['-o', str(dst)]
            i += 1
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W'):
            cmd.append(value)
        i += 1
    cmd += ['-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only', '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(ROOT / 'kits/sheng')]
    run(cmd, cwd=rc['directory'])


compile_cpp(source, OUT / 'LegacyDisplayProbe.o')
support = OUT / 'LegacyDisplaySupport27.cpp'
support.write_text('''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Generated by build-gop-legacy-display-27.py. The iPadOS 27 IOSurface has no allocation-size getter: bound the copy by the
// surface's memory descriptor instead. Board probes not yet built for iPadOS 27 start nothing.
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" OSObject *native_legacy_surface_create(OSObject *,void *,OSDictionary *);
extern "C" IOReturn native_legacy_surface_allocate(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" OSObject *native_legacy_default_surface_create(OSObject *root,uint32_t width,uint32_t height) {
    if(!root || !width || !height || width>16384 || height>16384)return nullptr;
    const uint64_t stride=(uint64_t(width)*4+127)&~uint64_t(127);
    const uint64_t bytes=(stride*height+4095)&~uint64_t(4095);
    OSDictionary *properties=OSDictionary::withCapacity(6);
    if(!properties)return nullptr;
    const char *keys[]={"IOSurfaceWidth","IOSurfaceHeight","IOSurfaceBytesPerRow",
                        "IOSurfaceBytesPerElement","IOSurfacePixelFormat","IOSurfaceAllocSize"};
    const uint64_t values[]={width,height,stride,4,0x42475241,bytes};
    bool valid=true;
    for(unsigned i=0;i<6 && valid;++i) {
        OSNumber *number=OSNumber::withNumber(values[i],64);
        valid=number && properties->setObject(keys[i],number);
        if(number)number->release();
    }
    OSObject *surface=valid?native_legacy_surface_create(root,nullptr,properties):nullptr;
    properties->release();
    if(!surface){printf("sheng-fb: default surface factory failed\\n");return nullptr;}
    IOReturn status=native_legacy_surface_allocate(surface);
    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
    valid=status==kIOReturnSuccess && memory && memory->getLength()>=bytes &&
          native_legacy_surface_width(surface)==width && native_legacy_surface_height(surface)==height &&
          native_legacy_surface_stride(surface)==stride && native_legacy_surface_format(surface)==0x42475241 &&
          native_legacy_surface_planes(surface)==0;
    printf("sheng-fb: default surface %p status 0x%x valid %u %ux%u stride %llu bytes %llu\\n",
           surface,status,valid,width,height,stride,bytes);
    if(!valid){surface->release();return nullptr;}
    return surface;
}
extern "C" uint64_t native_legacy_surface_allocation(OSObject *surface) {
    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);return memory?memory->getLength():0;
}
// The board diagnostic page is not drawn on iPadOS 27: the keystore service's extension rows go nowhere.
extern "C" {char boot_progress_ext[8][48];}
// No boot progress animation runs on iPadOS 27: the scanout is always free and no pulse anchors to the framebuffer.
extern "C" int boot_progress_prepare_scanout(void) {return 1;}
extern "C" void boot_progress_cancel_scanout(void) {}
extern "C" void boot_progress_stage(const char *) {}
extern "C" int boot_progress_pulse_anchor(uintptr_t,uint64_t,unsigned,unsigned,unsigned) {return 0;}
// Optional GPU/DPU/trace symbols: hidden weak no-op defaults so ld emits zero chained fixup imports.
// When no GPU driver is linked, report 0/0 fences ready so DPU direct scanout does not stall waiting on a GPU fence.
extern "C" __attribute__((weak, visibility("hidden"))) bool native_adreno_fences(uint32_t *submitted, uint32_t *retired) {
    if (submitted) *submitted = 0;
    if (retired) *retired = 0;
    return true;
}
extern "C" __attribute__((weak, visibility("hidden"))) bool native_adreno_fences_detail(uint32_t *submitted, uint32_t *retired, uint32_t *detail) {
    if (submitted) *submitted = 0;
    if (retired) *retired = 0;
    if (detail) {detail[0] = 0; detail[1] = 0; detail[2] = 0;}
    return true;
}
extern "C" __attribute__((weak, visibility("hidden"))) uint32_t native_ufs_dpu_map(unsigned, const uint64_t *, unsigned) {return 0;}
extern "C" __attribute__((weak, visibility("hidden"))) volatile uint32_t native_ufs_dpu_state = 0;
extern "C" __attribute__((weak, visibility("hidden"))) int native_gpu_trace_enabled(void) {return 0;}
extern "C" __attribute__((weak, visibility("hidden"))) void native_gpu_trace_record(uint32_t, uint32_t, const uint64_t *, uint32_t) {}
extern "C" __attribute__((weak, visibility("hidden"))) uint32_t native_gpu_trace_snapshot(void *, uint32_t) {return 0;}
extern "C" __attribute__((weak, visibility("hidden"))) int boot_kthread_report_nth(const char *, unsigned, char *, unsigned, char *, unsigned) {return -1;}
extern "C" __attribute__((weak, visibility("hidden"))) volatile uint32_t native_adreno_method_stats[20][4] = {};
extern "C" __attribute__((weak, visibility("hidden"))) volatile uint32_t native_keyboard_lid_closed = 0;
static IOLock *gLiuqinGccLock = nullptr, *gLiuqinSmmuLock = nullptr;
static IOLock *liuqinGetLock(IOLock **slot) {
    IOLock *lock = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (!lock) {
        IOLock *cand = IOLockAlloc();
        if (!cand) return nullptr;
        if (__atomic_compare_exchange_n(slot, &lock, cand, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) lock = cand;
        else IOLockFree(cand);
    }
    return lock;
}
extern "C" __attribute__((weak, visibility("hidden"))) bool sm8550_apps_smmu_lock(void) {
    IOLock *lock = liuqinGetLock(&gLiuqinSmmuLock);
    if (!lock) return false;
    IOLockLock(lock);
    return true;
}
extern "C" __attribute__((weak, visibility("hidden"))) void sm8550_apps_smmu_unlock(void) {
    if (gLiuqinSmmuLock) IOLockUnlock(gLiuqinSmmuLock);
}
extern "C" __attribute__((weak, visibility("hidden"))) bool sm8550_gcc_update32(volatile uint32_t *reg, uint32_t clear, uint32_t set) {
    if (!reg) return false;
    IOLock *lock = liuqinGetLock(&gLiuqinGccLock);
    if (!lock) return false;
    IOLockLock(lock);
    *reg = (*reg & ~clear) | set;
    __asm__ volatile("dsb sy" ::: "memory");
    const bool ok = (*reg & set) == set && (*reg & (clear & ~set)) == 0;
    IOLockUnlock(lock);
    return ok;
}
''')
compile_cpp(support, OUT / 'LegacyDisplaySupport27.o')
# The keystore state service (AppleKeyStoreState) and its probe, unchanged from iPadOS 17.
keystore_source = ROOT / 'kits/sheng/KeyStoreProbe.cpp'
compile_cpp(keystore_source, OUT / 'KeyStoreProbe.o')
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeKeyStoreEvents'], text=True), end='')
keyevents = ROOT / 'build/apple-key-store-state'
keyevents_late = keyevents / 'NativeKeyStoreEvents.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeKeyStoreEvents.cpp',
     keyevents / 'NativeKeyStoreEvents.o', keyevents_late])
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeGOPPresenter'], text=True), end='')
gop = ROOT / 'build/native-gop-presenter'
gop_late = gop / 'NativeGOPPresenter.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeGOPPresenter.cpp',
     gop / 'NativeGOPPresenter.o', gop_late])

# Stage 4A input, backlight and CPUFreq drivers for Xiaomi Pad 6 Pro (liuqin, SM8475).
DEVICE = Path(os.environ.get('UNIXNU_DEVICE') or (ROOT / 'kits/sheng').resolve().parent)
csot_fw = DEVICE / 'firmware/novatek/novatek_nt36532_m81_fw_csot.bin'
tm_fw = DEVICE / 'firmware/novatek/novatek_nt36532_m81_fw_tm.bin'
require(csot_fw.is_file(), f'{csot_fw}: missing NT36532 CSOT touch firmware')
require(tm_fw.is_file(), f'{tm_fw}: missing NT36532 TM touch firmware')
print(run(['python3', ROOT / 'scripts/build-sheng-source-drivers.py', '--tree', ROOT, '--device', DEVICE,
           '--touch-firmware', csot_fw], text=True), end='')
SRC = ROOT / 'build/sheng-source-drivers'
hid_abi_path = Path(__file__).resolve().with_name(f'hid-device-{BUILD}.json')
hid_abi = json.loads(hid_abi_path.read_text())
hid_slots = [(int(t, 16), int(d, 16)) for t, d in hid_abi['slots']]
hid_pure = {i for i, (t, _) in enumerate(hid_slots) if t == PURE_TARGET}


def object_symbols(data):
    commands = macho(data)['commands']
    st = next(p for c, p, _ in commands if c == 2)
    sy, count, strings, _ = struct.unpack_from('<4I', data, st + 8)
    found = []
    for i in range(count):
        ix, kind, _, _, va = struct.unpack_from('<IBBHQ', data, sy + i * 16)
        found.append((data[strings + ix:data.index(b'\0', strings + ix)].decode(), va, kind))
    return found


def object_vtable(data, found, name, count):
    """[(symbol, signed word)] of the first count entries of vtable name (authenticated pointer relocations)."""
    va = next(v for n, v, _ in found if n == name)
    entries = {}
    for segment in macho(data)['segments']:
        for sec in segment['sections']:
            if sec['va'] <= va < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', data, sec['command_offset'] + 56)
                for i in range(rc):
                    offset, bits = struct.unpack_from('<2I', data, ro + i * 8)
                    if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == 11:
                        entries[sec['va'] + offset] = (found[bits & 0xffffff][0],
                                                       struct.unpack_from('<Q', data, sec['fileoff'] + offset)[0])
    return [entries.get(va + 16 + i * 8) for i in range(count)]


hid_bindings = {n: int(v, 16) + slide for n, v in hid_abi['bindings'].items()}
by_diversity = {}
for i, (_, d) in enumerate(hid_slots):
    by_diversity.setdefault(d, []).append(i)
hid_vtables = []
slot_of = {}
hid_raw = (SRC / 'NativeHidInput.o').read_bytes()
hid_found = object_symbols(hid_raw)
vtables = [n for n, _, k in hid_found if n.startswith('__ZTV') and k != 1 and '9MetaClassE' not in n]
for vt_name in vtables:
    for i, entry in enumerate(object_vtable(hid_raw, hid_found, vt_name, len(hid_slots))):
        if entry and '11IOHIDDevice' in entry[0]:
            require(slot_of.setdefault(entry[0], i) == i, f'{entry[0]}: one slot in every owned vtable')
for name in sorted({n for n, _, k in hid_found if k == 1 and ('11IOHIDDevice' in n)} - set(hid_bindings)):
    if name in slot_of:
        slot = slot_of[name]
    else:
        matches = by_diversity.get(discriminator(name[1:]), [])
        require(len(matches) == 1, f'{name}: one original IOHIDDevice slot with its discriminator')
        slot = matches[0]
    require(slot not in hid_pure, f'{name}: not an abstract original slot')
    hid_bindings[name] = hid_slots[slot][0] + slide
for vt_name in vtables:
    entries = object_vtable(hid_raw, hid_found, vt_name, len(hid_slots))
    require(all(entries), f'{vt_name}: at least the {len(hid_slots)} IOHIDDevice entries')
    for i, (name, word) in enumerate(entries):
        require(word >> 63 and word >> 48 & 1 and word >> 49 & 3 == 0 and (word >> 32) & 0xffff == hid_slots[i][1],
                f'{vt_name} slot {i} ({name}) signed with the original diversity {hid_slots[i][1]:#x}')
        if '11IOHIDDevice' in name:
            require(hid_bindings.get(name) == hid_slots[i][0] + slide, f'{vt_name} slot {i} ({name}) is the original entry')
hid_vtables += vtables
HID_DATA = {'__ZN11IOHIDDevice10gMetaClassE', '__ZTV11IOHIDDevice'}
hid_text, hid_addresses = '.text\n.p2align 2\n', ''
for name, va in sorted(hid_bindings.items()):
    if name in HID_DATA:
        hid_addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
    else:
        hid_addresses += f'.globl {name}.original\n.set {name}.original, {va:#x}\n'
        hid_text += f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
(OUT / 'hid-bindings.S').write_text(hid_text)
(OUT / 'hid-addresses.S').write_text(hid_addresses)
for stem in ('hid-bindings', 'hid-addresses'):
    run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk, '-c', OUT / (stem + '.S'), '-o', OUT / (stem + '.o')])
hid_late = OUT / 'NativeHidInput.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeHidInput.cpp',
     SRC / 'NativeHidInput.o', hid_late])
hid_probe = ROOT / 'kits/sheng/HidInputProbe.cpp'
compile_cpp(hid_probe, OUT / 'HidInputProbe.o')
input_objects = [hid_late, OUT / 'hid-bindings.o', OUT / 'hid-addresses.o', OUT / 'HidInputProbe.o'] + \
    [SRC / (stem + '.o') for stem in ('TouchProbe', 'ButtonProbe', 'GeniSpi', 'NvtProtocol', 'NvtTouchCore', 'NvtFirmware')]
print(f'HID: {len(hid_bindings)} original IOHIDDevice bindings, vtables {hid_vtables} match the original diversities')

# Board drivers: SM8475 EPSS CPUFreq, KTZ8866 Backlight, DPU timing probe, PMK8350 RTC, ADSP+Battery, USB OTG, and TM touch fallback.
board_dir = OUT / 'board'
board_dir.mkdir(exist_ok=True)
KITS = ROOT / 'kits/sheng'
board_objects = []
for stem in ('CpuFreqProbe', 'BacklightProbe', 'DisplayTimingProbe', 'BoardClock', 'AdspProbe', 'BatteryProbe'):
    compile_cpp(KITS / (stem + '.cpp'), board_dir / (stem + '.o'))
    board_objects.append(board_dir / (stem + '.o'))
for stem in ('AppleARMBacklight', 'ShengBattery'):
    compile_cpp(KITS / (stem + '.cpp'), board_dir / (stem + '.o'))
    run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', f'--globalize-symbol=__GLOBAL__sub_I_{stem}.cpp',
         board_dir / (stem + '.o'), board_dir / (stem + '.late.o')])
    board_objects.append(board_dir / (stem + '.late.o'))
for stem in ('GeniI2c', 'QcomMdt', 'FatVolume', 'QcomSmem', 'QcomSmp2p', 'QcomGlink', 'QcomQmp', 'QcomRpmh',
             'ShengBattmgr', 'ShengChargeAuth'):
    compile_c(KITS / (stem + '.c'), board_dir / (stem + '.o'))
    board_objects.append(board_dir / (stem + '.o'))
compile_c(KITS / 'AdspFirmware.c', board_dir / 'AdspFirmware.o', 'bsd/DEVELOPMENT/kern_exec.o.json')
board_objects.append(board_dir / 'AdspFirmware.o')
tm_bytes = tm_fw.read_bytes()
(board_dir / 'NvtFirmwareTm.c').write_text(
    '/* Generated vendor TM touch firmware; build output only. */\n#include <stdint.h>\n'
    f'const uint32_t native_touch_firmware_tm_size = {len(tm_bytes)};\n'
    f'const uint8_t native_touch_firmware_tm[{len(tm_bytes)}] = {{' +
    ','.join(str(b) for b in tm_bytes) + '};\n'
)
compile_c(board_dir / 'NvtFirmwareTm.c', board_dir / 'NvtFirmwareTm.o')
board_objects.append(board_dir / 'NvtFirmwareTm.o')
input_objects += board_objects

# USB device & host mode: SM8475 QcomDwc3 subclasses of original AppleUSBXDCI & AppleUSBXHCI on 24A446.
usb_abi_path = Path(__file__).resolve().with_name(f'usb-xdci-{BUILD}.json')
xhci_abi_path = Path(__file__).resolve().with_name(f'usb-xhci-{BUILD}.json')
usb_abi = json.loads(usb_abi_path.read_text())
usb_slots = [(int(t, 16), int(d, 16)) for t, d in usb_abi['slots']]
usb_methods = {int(k): v for k, v in usb_abi['methods'].items()}
usb_pure = {i for i, (t, _) in enumerate(usb_slots) if t == PURE_TARGET}
USB = DEVICE / 'drivers/QcomDwc3'
require((USB / 'QcomDwc3.cpp').exists(), 'QcomDwc3 module directory')
USB_OUT = board_dir / 'qcom-dwc3'
USB_OUT.mkdir(parents=True, exist_ok=True)
usb_names = ({int(k): v for k, v in usb_abi['macros'].items()} if 'macros' in usb_abi else
             {179: 'XDCI_KEEP_CONFIGURATIONS', 227: 'XDCI_DESIRED_POWER', 267: 'XDCI_TRANSPORT_ACTIVE', 268: 'XDCI_POWER_ON',
              269: 'XDCI_POWER_OFF', 270: 'XDCI_PARAMETERS', 275: 'XDCI_CONTROLLER_RESET', 277: 'XDCI_PERIODIC_USAGE',
              278: 'XDCI_PERIODIC_SET'})
usb_header = '// Generated from the reviewed AppleUSBXDCI fixture; opaque private storage only.\n#pragma once\n#include <IOKit/IOService.h>\n'
for slot, macro in usb_names.items():
    usb_header += f'#define {macro} {usb_methods[slot]["name"]}\n'
usb_header += f'''class AppleUSBXDCI : public IOService {{
 OSDeclareAbstractStructors(AppleUSBXDCI)
 unsigned char originalPrivate[{usb_abi['instance_size']}-sizeof(IOService)];
public:
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool terminate(IOOptionBits options=0) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
 unsigned long maxCapabilityForDomainState(IOPMPowerFlags) override;
 unsigned long initialPowerStateForDomainState(IOPMPowerFlags) override;
 IOReturn powerStateWillChangeTo(IOPMPowerFlags,unsigned long,IOService *) override;
 IOReturn powerStateDidChangeTo(IOPMPowerFlags,unsigned long,IOService *) override;
'''
for i in range(usb_abi.get('base_slots', 168), len(usb_slots)):
    m = usb_methods[i]
    suffix = '=0' if i in usb_pure else ''
    usb_header += f' virtual {m["returns"]} {m["name"]}({m["parameters"]}){suffix};\n'
usb_header += f'}};\nstatic_assert(sizeof(AppleUSBXDCI)=={usb_abi["instance_size"]},"Original controller instance size");\n'
(USB_OUT / 'OriginalUsbXdciABI.h').write_text(usb_header)

xhci_abi = json.loads(xhci_abi_path.read_text())
xhci_slots = [(int(t, 16), int(d, 16)) for t, d in xhci_abi['slots']]
xhci_methods = {int(k): v for k, v in xhci_abi['methods'].items()}
xhci_names = ({int(k): v for k, v in xhci_abi['macros'].items()} if 'macros' in xhci_abi else
              {188: 'XHCI_DESIRED_POWER', 315: 'XHCI_READ64', 317: 'XHCI_WRITE64'})
xhci_header = '// Generated from the reviewed AppleUSBXHCI fixture; opaque private storage only.\n#pragma once\n#include <IOKit/IOService.h>\n'
for slot, macro in xhci_names.items():
    xhci_header += f'#define {macro} {xhci_methods[slot]["name"]}\n'
xhci_header += f'''class AppleUSBXHCI : public IOService {{
 OSDeclareAbstractStructors(AppleUSBXHCI)
 unsigned char originalPrivate[{xhci_abi['instance_size']}-sizeof(IOService)];
public:
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool terminate(IOOptionBits options=0) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn callPlatformFunction(const OSSymbol *,bool,void *,void *,void *,void *) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
 unsigned long maxCapabilityForDomainState(IOPMPowerFlags) override;
 unsigned long initialPowerStateForDomainState(IOPMPowerFlags) override;
 IOReturn powerStateWillChangeTo(IOPMPowerFlags,unsigned long,IOService *) override;
 IOReturn powerStateDidChangeTo(IOPMPowerFlags,unsigned long,IOService *) override;
'''
for i in range(xhci_abi.get('base_slots', 168), len(xhci_slots)):
    m = xhci_methods[i]
    xhci_header += f' virtual {m["returns"]} {m["name"]}({m["parameters"]});\n'
xhci_header += f'}};\nstatic_assert(sizeof(AppleUSBXHCI)=={xhci_abi["instance_size"]},"Original controller instance size");\n'
(USB_OUT / 'OriginalUsbXhciABI.h').write_text(xhci_header)
usb_flags = ('-I', str(USB_OUT), '-I', str(USB), '-I', str(KITS))
compile_cpp(USB / 'QcomDwc3.cpp', USB_OUT / 'QcomDwc3.o', usb_flags)
compile_cpp(KITS / 'UsbProbe.cpp', USB_OUT / 'UsbProbe.o', usb_flags)
usb_raw = (USB_OUT / 'QcomDwc3.o').read_bytes()
usb_found = object_symbols(usb_raw)
usb_bindings = {n: int(v, 16) + slide for n, v in usb_abi['bindings'].items()}
usb_vt = '__ZTV12QcomDwc3Xdci'
usb_entries = object_vtable(usb_raw, usb_found, usb_vt, len(usb_slots))
require(all(usb_entries), f'{usb_vt}: at least the {len(usb_slots)} AppleUSBXDCI entries')
for i, (name, word) in enumerate(usb_entries):
    require(word >> 63 and word >> 48 & 1 and word >> 49 & 3 == 0 and (word >> 32) & 0xffff == usb_slots[i][1],
            f'{usb_vt} slot {i} ({name}) signed with the original diversity {usb_slots[i][1]:#x}')
    if '12AppleUSBXDCI' in name:
        require(i not in usb_pure, f'{usb_vt} slot {i} ({name}) implements an abstract original slot')
        require(usb_bindings.setdefault(name, usb_slots[i][0] + slide) == usb_slots[i][0] + slide, f'{name}: one original address')
    elif not name.startswith(('__ZN12QcomDwc3Xdci', '__ZNK12QcomDwc3Xdci')):
        require(i < usb_abi.get('base_slots', 168), f'{usb_vt} slot {i} ({name}) is inherited from IOService')
usb_slot_by_diversity = {d: i for i, (_, d) in enumerate(usb_slots)}
for name in sorted({n for n, _, k in usb_found if k == 1 and '12AppleUSBXDCI' in n} - set(usb_bindings)):
    slot = usb_slot_by_diversity.get(discriminator(name[1:]))
    require(slot is not None and slot not in usb_pure, f'{name}: an original concrete AppleUSBXDCI slot')
    usb_bindings[name] = usb_slots[slot][0] + slide
xhci_bindings = {n: int(v, 16) + slide for n, v in xhci_abi['bindings'].items()}
xhci_vt = '__ZTV12QcomDwc3Xhci'
xhci_entries = object_vtable(usb_raw, usb_found, xhci_vt, len(xhci_slots))
require(all(xhci_entries), f'{xhci_vt}: at least the {len(xhci_slots)} AppleUSBXHCI entries')
for i, (name, word) in enumerate(xhci_entries):
    require(word >> 63 and word >> 48 & 1 and word >> 49 & 3 == 0 and (word >> 32) & 0xffff == xhci_slots[i][1],
            f'{xhci_vt} slot {i} ({name}) signed with the original diversity {xhci_slots[i][1]:#x}')
    if '12AppleUSBXHCI' in name:
        require(xhci_bindings.setdefault(name, xhci_slots[i][0] + slide) == xhci_slots[i][0] + slide, f'{name}: one original address')
    elif not name.startswith(('__ZN12QcomDwc3Xhci', '__ZNK12QcomDwc3Xhci')):
        require(i < xhci_abi.get('base_slots', 168), f'{xhci_vt} slot {i} ({name}) is inherited from IOService')
XHCI_BASE = xhci_abi.get('base_slots', 168)
kernel_text = range(*(int(x, 16) for x in xhci_abi['kernel_text'])) if 'kernel_text' in xhci_abi else range(0xfffffe000b000000, 1 << 64)
xhci_io = {i for i, (t, _) in enumerate(xhci_slots[:XHCI_BASE]) if t not in kernel_text and i not in (0, 1, 7)}
for i in sorted(xhci_io):
    require('12AppleUSBXHCI' in xhci_entries[i][0] or 'QcomDwc3Xhci' in xhci_entries[i][0], f'{xhci_vt} slot {i} ({xhci_entries[i][0]}) reaches the original override')
xhci_slot_by_diversity = {d: i for i, (_, d) in enumerate(xhci_slots)}
for name in sorted({n for n, _, k in usb_found if k == 1 and '12AppleUSBXHCI' in n} - set(xhci_bindings)):
    slot = xhci_slot_by_diversity.get(discriminator(name[1:]))
    require(slot is not None, f'{name}: an original concrete AppleUSBXHCI slot')
    xhci_bindings[name] = xhci_slots[slot][0] + slide
usb_bindings.update(xhci_bindings)
USB_DATA = {'__ZN12AppleUSBXDCI10gMetaClassE', '__ZTV12AppleUSBXDCI', '__ZN12AppleUSBXHCI10gMetaClassE', '__ZTV12AppleUSBXHCI'}
usb_text, usb_addresses = '.text\n.p2align 2\n', ''
for name, va in sorted(usb_bindings.items()):
    if name in USB_DATA:
        usb_addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
    else:
        usb_addresses += f'.globl {name}.original\n.set {name}.original, {va:#x}\n'
        usb_text += f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
(USB_OUT / 'usb-bindings.S').write_text(usb_text)
(USB_OUT / 'usb-addresses.S').write_text(usb_addresses)
for stem in ('usb-bindings', 'usb-addresses'):
    run(['xcrun', 'clang', '-target', TRIPLE, '-isysroot', sdk, '-c', USB_OUT / (stem + '.S'), '-o', USB_OUT / (stem + '.o')])
usb_late = USB_OUT / 'QcomDwc3.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_QcomDwc3.cpp',
     USB_OUT / 'QcomDwc3.o', usb_late])
input_objects += [usb_late, USB_OUT / 'usb-bindings.o', USB_OUT / 'usb-addresses.o', USB_OUT / 'UsbProbe.o']
print(f'Board drivers (liuqin): {len(board_objects)} objects + USB OTG ({len(usb_bindings)} XDCI/XHCI bindings)')

# Archive owned GOP framebuffer + AppleKeyStoreState + Stage 4A input/backlight/cpufreq objects.
archive = OUT / 'libNativeLegacyDisplay.late.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late, OUT / 'original-bindings.o', OUT / 'original-addresses.o',
     OUT / 'LegacyDisplayProbe.o', OUT / 'LegacyDisplaySupport27.o', gop_late,
     OUT / 'KeyStoreProbe.o', keyevents_late, *input_objects])
sources = [
    ROOT / 'drivers/GopFramebuffer/NativeLegacyDisplay.cpp',
    ROOT / 'drivers/GopFramebuffer/NativeLegacyRequest.h',
    ROOT / 'drivers/GopFramebuffer/NativeGOPPresenter.cpp',
    ROOT / 'drivers/GopFramebuffer/NativeGOPWire.h',
    ROOT / 'drivers/GopFramebuffer/NativeHalfCodes.h',
    source,
    ROOT / 'scripts' / ABI_PATH.name,
    keystore_source,
    ROOT / 'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp',
    hid_abi_path,
    hid_probe,
    KITS / 'CpuFreqProbe.cpp',
    KITS / 'BacklightProbe.cpp',
    KITS / 'DisplayTimingProbe.cpp',
    KITS / 'BoardClock.cpp',
    KITS / 'BoardClock.h',
    KITS / 'AdspProbe.cpp',
    KITS / 'AdspFirmware.c',
    KITS / 'BatteryProbe.cpp',
    KITS / 'ShengBattery.cpp',
    KITS / 'AppleARMBacklight.cpp',
    KITS / 'UsbProbe.cpp',
    KITS / 'GeniI2c.c',
    KITS / 'GeniI2c.h',
    USB / 'QcomDwc3.cpp',
    USB / 'QcomEusb2Phy.h',
    USB / 'QcomUcsi.h',
    usb_abi_path,
    xhci_abi_path,
    csot_fw,
    tm_fw,
]


def source_name(path):
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


(OUT / 'build-report.json').write_text(json.dumps(dict(
    source_sha256={source_name(p): sha(p) for p in sources},
    generated_abi_sha256=sha(OUT / 'OriginalLegacyFramebufferABI.h'),
    kernelcache_sha256=abi['kernelcache_sha256'],
    compiled_object_sha256=sha(obj),
    archive_sha256=sha(archive),
    bindings={n: hex(v) for n, v in bindings.items()},
    virtual_entries=len(slots),
    owned_entries=owned,
    inherited_native_entries=native,
    original_bindings='fixed-collection-addresses',
    original_instructions_modified=False), indent=2) + '\n')
print(f'Built liuqin owned Legacy GOP backend on {BUILD}: {len(owned)} owned, {len(native)} native, {len(bindings)} original bindings')
