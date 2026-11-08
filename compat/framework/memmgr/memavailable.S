/* Real stock-kernel KB fallback for donor MemInfo::GetCurrentBuffer().
 * Leaf implementation: no external relocations or C++ object-layout changes.
 * Retain the donor's INT_MAX failure sentinel; never manufacture memory values.
 */
.text
.p2align 2
.global dipper_memavailable_kb
.type dipper_memavailable_kb,%function
dipper_memavailable_kb:
    pacibsp
    sub sp, sp, #4096
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x0, #-100                 // AT_FDCWD
    adr x1, proc_path
    mov w2, #0x80000              // O_RDONLY | O_CLOEXEC
    mov x8, #56                   // openat
    svc #0
    tbnz x0, #63, fail
    mov x9, x0                    // fd, survives Linux syscall
read_retry:
    mov x0, x9
    add x1, sp, #16
    mov x2, #4096
    mov x8, #63                   // read
    svc #0
    cmn x0, #4                    // EINTR
    b.eq read_retry
    mov x10, x0
    mov x0, x9
    mov x8, #57                   // close even after read error
    svc #0
    cmp x10, #14
    b.lt fail
    add x11, sp, #16
    add x12, x11, x10             // end, never read beyond returned bytes
    adr x14, mem_key
    ldr x15, [x14]
    ldr w16, [x14, #8]
line:
    sub x13, x12, x11
    cmp x13, #14
    b.lt fail
    ldr x14, [x11]
    cmp x14, x15
    b.ne next_char
    ldr w14, [x11, #8]
    cmp w14, w16
    b.ne next_char
    ldrb w14, [x11, #12]
    cmp w14, #58                  // ':'
    b.eq found
next_char:
    ldrb w14, [x11], #1
    cmp w14, #10
    b.eq line
    cmp x11, x12
    b.lo next_char
    b fail
found:
    add x11, x11, #13
whitespace:
    cmp x11, x12
    b.hs fail
    ldrb w14, [x11], #1
    cmp w14, #32
    b.eq whitespace
    cmp w14, #9
    b.eq whitespace
    sub w14, w14, #48
    cmp w14, #9
    b.hi fail                     // require at least one decimal digit
    mov x0, #0
    mov x17, #0x7fffffff
digit:
    add x0, x0, x0, lsl #2
    add x0, x14, x0, lsl #1
    cmp x0, x17
    b.hi fail                     // checked signed-int overflow
    cmp x11, x12
    b.hs done
    ldrb w14, [x11], #1
    sub w14, w14, #48
    cmp w14, #9
    b.ls digit
    b done
fail:
    mov w0, #0x7fffffff
done:
    ldp x29, x30, [sp], #16
    add sp, sp, #4096
    autibsp
    ret
proc_path:
    .asciz "/proc/meminfo"
mem_key:
    .ascii "MemAvailable:"
.size dipper_memavailable_kb, .-dipper_memavailable_kb
