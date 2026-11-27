/* SPDX-License-Identifier: MIT */
/* Publishes SM8475 Adreno 730 to UniXNU's NativeAdrenoGpu IOKit service and
 * stages AGXAccelerator (IOAcceleratorES subclass) for iPadOS 27 Metal/Turnip. */
#include "PongAdrenoCp.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *,
    const NativeAdrenoRingClock *, const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *,
    uint32_t, uint32_t);
extern "C" IOService *native_adreno_published_service(void);
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp") __attribute__((weak));
extern "C" IOService *native_adreno_accelerator_allocate(void) __attribute__((weak));
extern "C" unsigned native_adreno_accelerator_instance_size(void) __attribute__((weak));
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" volatile uint32_t native_liuqin_gpu_stats[8];

/* Optional board hooks declared weak_import in NativeAdrenoPublish/Submit;
 * Darwin static kernel link requires a weak definition to satisfy ld. */
extern "C" int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) __attribute__((weak));
extern "C" int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) { return -1; }
extern "C" kern_return_t iokit_physical_descriptor_close_notify(IOMemoryDescriptor *, void (*)(void *), void *) __attribute__((weak));
extern "C" kern_return_t iokit_physical_descriptor_close_notify(IOMemoryDescriptor *, void (*)(void *), void *) { return KERN_NOT_SUPPORTED; }
extern "C" volatile uint32_t native_gpu_dcvs[15] __attribute__((weak));
volatile uint32_t native_gpu_dcvs[15];

namespace {
PongCpBackend *gBackend = nullptr;

void publishMetalAccelerator()
{
    native_liuqin_gpu_stats[5] = 1;
    if (!native_adreno_accelerator_static_init || !native_adreno_accelerator_allocate ||
        !native_adreno_accelerator_instance_size) {
        native_liuqin_gpu_stats[6] = uint32_t(kIOReturnUnsupported);
        return;
    }
    const kern_return_t kext = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (kext) {
        native_liuqin_gpu_stats[6] = uint32_t(kext);
        IOLog("LIUQIN METAL: IOGPUFamily load failed 0x%x\n", unsigned(kext));
        return;
    }
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136) {
        native_liuqin_gpu_stats[6] = uint32_t(kIOReturnNotFound);
        IOLog("LIUQIN METAL: IOAcceleratorES metaclass invalid\n");
        return;
    }
    native_liuqin_gpu_stats[5] = 2;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        native_liuqin_gpu_stats[6] = uint32_t(kIOReturnNoResources);
        return;
    }
    native_adreno_accelerator_static_init();
    const IOReturn post = OSMetaClass::postModLoad(handle);
    if (post) {
        native_liuqin_gpu_stats[6] = uint32_t(post);
        IOLog("LIUQIN METAL: postModLoad failed 0x%x\n", unsigned(post));
        return;
    }
    if (native_adreno_accelerator_instance_size() != 136) {
        native_liuqin_gpu_stats[6] = uint32_t(kIOReturnInvalid);
        return;
    }
    native_liuqin_gpu_stats[5] = 3;
    IOService *accelerator = native_adreno_accelerator_allocate();
    if (!accelerator) {
        native_liuqin_gpu_stats[6] = uint32_t(kIOReturnNoMemory);
        return;
    }
    if (!accelerator->init()) {
        accelerator->release();
        native_liuqin_gpu_stats[6] = uint32_t(kIOReturnError);
        return;
    }
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    accelerator->setProperty("NativeSandboxGrantProcess", "*");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    native_liuqin_gpu_stats[5] = 4;
    if (!accelerator->attach(IOService::getPlatform())) {
        accelerator->release();
        native_liuqin_gpu_stats[6] = uint32_t(kIOReturnNotAttached);
        return;
    }
    accelerator->registerService();
    native_liuqin_gpu_stats[7] = 1;
    native_liuqin_gpu_stats[5] = 5;
    native_liuqin_gpu_stats[6] = 0;
    IOLog("LIUQIN METAL: AGXAccelerator registered (NativeAdrenoMetal / A730 0x%08x)\n",
          gBackend ? gBackend->info.chip_id : 0u);
}
} // namespace

bool PongAdrenoPublishClient(PongCpBackend *device)
{
    if (gBackend || !device || !device->healthy()) return false;
    gBackend = device;
    const IOReturn result = native_adreno_publish(
        device->vm, device->submit, device->ring, &device->clock,
        device->timer, &device->info, 32, device->output_bits);
    if (result) {
        IOLog("LIUQIN ADRENO: native_adreno_publish failed 0x%x\n", unsigned(result));
        return false;
    }
    IOService *service = native_adreno_published_service();
    if (!service) return false;
    service->setProperty("LiuqinAdrenoCPReady", kOSBooleanTrue);
    service->setProperty("LiuqinChipID", device->info.chip_id, 32);
    unsigned metal = 0;
    if (PE_parse_boot_argn("gpu-metal", &metal, sizeof(metal)) && metal == 1)
        publishMetalAccelerator();
    return true;
}
