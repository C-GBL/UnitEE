// M6 gate host program: boots the IL2CPP runtime on the EE and invokes the
// managed HelloEE.Main() (plan section 9, M6 acceptance).
//
// The managed side prints the feature-test results itself; this file only
// owns runtime bring-up and the pass/fail token.
#include <kernel.h>
#include <sifrpc.h>
#include <stdio.h>

#include "il2cpp-api.h"
#include "il2cpp-object-internals.h"

// bdwgc scans [current sp, ps2ur_gc_stack_bottom): the BIOS picks the boot
// sp, so the bound is captured here, above every frame that can hold a
// managed reference (see os/ps2/GcSupport.cpp).
extern "C" char* ps2ur_gc_stack_bottom;

int main(void)
{
    int stackAnchor;
    ps2ur_gc_stack_bottom = reinterpret_cast<char*>(
        (reinterpret_cast<unsigned long>(&stackAnchor) + 128) & ~15UL);

    SifInitRpc(0);
    printf("[m6] il2cpp EE bring-up\n");

    // global-metadata.dat is read through os::File relative to the data dir;
    // host: maps to the directory PCSX2 derives from the ELF, where the run
    // harness stages Metadata/global-metadata.dat.
    il2cpp_set_data_dir("host:");
    if (!il2cpp_init("IL2CPP Root Domain")) {
        printf("PS2UR_TOKEN_M6_FAIL il2cpp_init\n");
        SleepThread();
        return 1;
    }
    printf("[m6] il2cpp_init OK\n");

    // Find HelloEE.Main in the loaded images.
    const Il2CppDomain* domain = il2cpp_domain_get();
    size_t assembly_count = 0;
    const Il2CppAssembly** assemblies =
        il2cpp_domain_get_assemblies(domain, &assembly_count);
    printf("[m6] %u assemblies\n", static_cast<unsigned>(assembly_count));

    const MethodInfo* entry = nullptr;
    for (size_t i = 0; i < assembly_count && entry == nullptr; ++i) {
        const Il2CppImage* image = il2cpp_assembly_get_image(assemblies[i]);
        Il2CppClass* klass = il2cpp_class_from_name(image, "", "HelloEE");
        if (klass != nullptr) {
            entry = il2cpp_class_get_method_from_name(klass, "Main", 0);
        }
    }
    if (entry == nullptr) {
        printf("PS2UR_TOKEN_M6_FAIL no entry point\n");
        SleepThread();
        return 1;
    }

    Il2CppException* exception = nullptr;
    Il2CppObject* result = il2cpp_runtime_invoke(
        const_cast<MethodInfo*>(entry), nullptr, nullptr, &exception);
    if (exception != nullptr) {
        static char message[512];
        for (const Il2CppException* ex = exception; ex != nullptr; ex = ex->inner_ex) {
            il2cpp_format_exception(ex, message, sizeof(message));
            printf("[m6] exception: %s\n", message);
        }
        printf("PS2UR_TOKEN_M6_FAIL managed exception escaped\n");
        SleepThread();
        return 1;
    }

    const int code =
        result != nullptr ? *static_cast<int*>(il2cpp_object_unbox(result)) : -1;
    printf("[m6] managed Main returned %d\n", code);
    printf(code == 0 ? "PS2UR_TOKEN_M6_OK\n" : "PS2UR_TOKEN_M6_FAIL exit code\n");

    il2cpp_shutdown();
    SleepThread();
    return code;
}
