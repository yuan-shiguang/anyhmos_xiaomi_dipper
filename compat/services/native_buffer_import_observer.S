// Keep the caller's real BackendTexture return slot in x8. No opaque C++
// replacement object, copies, destructors, or altered return values.
.text
.p2align 2
.global _ZN4OHOS5Rosen17NativeBufferUtils34MakeBackendTextureFromNativeBufferEP18NativeWindowBufferiib
.type _ZN4OHOS5Rosen17NativeBufferUtils34MakeBackendTextureFromNativeBufferEP18NativeWindowBufferiib,%function
_ZN4OHOS5Rosen17NativeBufferUtils34MakeBackendTextureFromNativeBufferEP18NativeWindowBufferiib:
    sub sp, sp, #128
    stp x29, x30, [sp, #96]
    add x29, sp, #96
    stp x0, x1, [sp]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    str x8, [sp, #64]
    bl DipperResolveBufferImport
    mov x16, x0
    ldp x0, x1, [sp]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldr x8, [sp, #64]
    blr x16
    stp x0, x1, [sp, #112]
    ldr x0, [sp, #64]
    ldr x1, [sp]
    ldr w2, [sp, #8]
    ldr w3, [sp, #16]
    ldr w4, [sp, #24]
    bl DipperObserveBufferImport
    ldp x0, x1, [sp, #112]
    ldp x29, x30, [sp, #96]
    add sp, sp, #128
    ret
.size _ZN4OHOS5Rosen17NativeBufferUtils34MakeBackendTextureFromNativeBufferEP18NativeWindowBufferiib, .-_ZN4OHOS5Rosen17NativeBufferUtils34MakeBackendTextureFromNativeBufferEP18NativeWindowBufferiib
.section .note.GNU-stack,"",%progbits
