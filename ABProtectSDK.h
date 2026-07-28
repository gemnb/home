#pragma once

// ABProtect SDK - Protection Markers
// Include this header in your project and use the macros below
// to mark code regions for various protections.
//
// Available markers:
//   ABPROTECT_VM_BEGIN / ABPROTECT_VM_END         - VM virtualization
//   ABPROTECT_CFF_BEGIN / ABPROTECT_CFF_END       - Control flow flattening
//   ABPROTECT_ENCRYPT_BEGIN / ABPROTECT_ENCRYPT_END - Runtime encrypt/decrypt region
//   ABPROTECT_SUBLEQ_BEGIN / ABPROTECT_SUBLEQ_END - Subleq OISC virtualization
//   ABPROTECT_CHECK_DEBUGGER                       - Inline anti-debug check
//   ABPROTECT_CHECK_INTEGRITY                      - Inline CRC integrity check
//   ABPROTECT_CHECK_DUMP                           - Inline anti-dump check

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Internal: marker generation helpers
// ============================================================

#if defined(_MSC_VER)

#if defined(_M_X64) || defined(_M_AMD64)
// x64 MSVC: volatile local array with jmp-over pattern
// Pattern: EB 08 <8 bytes magic>
#define _ABPROTECT_MARKER_10(name, m0,m1,m2,m3,m4,m5,m6,m7) \
    do { \
        volatile char name[10] = { \
            '\xEB','\x08', m0,m1,m2,m3,m4,m5,m6,m7 \
        }; \
        (void)name; \
    } while(0)

// Function-body-safe variant (no pragma, caller must disable optimization)
#define _ABPROTECT_MARKER_10_INNER(name, m0,m1,m2,m3,m4,m5,m6,m7) \
    do { \
        volatile char name[10] = { \
            '\xEB','\x08', m0,m1,m2,m3,m4,m5,m6,m7 \
        }; \
        (void)name; \
    } while(0)

#else
// x86 MSVC: inline asm
#define _ABPROTECT_MARKER_10(name, m0,m1,m2,m3,m4,m5,m6,m7) \
    __asm { __asm _emit 0xEB __asm _emit 0x08 \
            __asm _emit m0 __asm _emit m1 __asm _emit m2 __asm _emit m3 \
            __asm _emit m4 __asm _emit m5 __asm _emit m6 __asm _emit m7 }
#define _ABPROTECT_MARKER_10_INNER(name, m0,m1,m2,m3,m4,m5,m6,m7) \
    _ABPROTECT_MARKER_10(name, m0,m1,m2,m3,m4,m5,m6,m7)
#endif

#elif defined(__GNUC__) || defined(__clang__)
#define _ABPROTECT_MARKER_10(name, m0,m1,m2,m3,m4,m5,m6,m7) \
    __asm__ volatile ( \
        ".byte 0xEB, 0x08\n" \
        ".byte " #m0 "," #m1 "," #m2 "," #m3 "," #m4 "," #m5 "," #m6 "," #m7 "\n" \
    )
#define _ABPROTECT_MARKER_10_INNER(name, m0,m1,m2,m3,m4,m5,m6,m7) \
    _ABPROTECT_MARKER_10(name, m0,m1,m2,m3,m4,m5,m6,m7)

#else
#define _ABPROTECT_MARKER_10(name, m0,m1,m2,m3,m4,m5,m6,m7)
#define _ABPROTECT_MARKER_10_INNER(name, m0,m1,m2,m3,m4,m5,m6,m7)
#endif

// ============================================================
// VM Virtualization markers
// Magic: ABVM_BEG / ABVM_END
// ============================================================

#define ABPROTECT_VM_BEGIN \
    _ABPROTECT_MARKER_10(_ab_vm_b, 'A','B','V','M','_','B','E','G')

#define ABPROTECT_VM_END \
    _ABPROTECT_MARKER_10(_ab_vm_e, 'A','B','V','M','_','E','N','D')

// ============================================================
// Control Flow Flattening markers
// Magic: ABCF_BEG / ABCF_END
// ============================================================

#define ABPROTECT_CFF_BEGIN \
    _ABPROTECT_MARKER_10(_ab_cf_b, 'A','B','C','F','_','B','E','G')

#define ABPROTECT_CFF_END \
    _ABPROTECT_MARKER_10(_ab_cf_e, 'A','B','C','F','_','E','N','D')

// ============================================================
// Runtime Encrypt/Decrypt region markers
// Magic: ABEC_BEG / ABEC_END
// ============================================================

#define ABPROTECT_ENCRYPT_BEGIN \
    _ABPROTECT_MARKER_10(_ab_ec_b, 'A','B','E','C','_','B','E','G')

#define ABPROTECT_ENCRYPT_END \
    _ABPROTECT_MARKER_10(_ab_ec_e, 'A','B','E','C','_','E','N','D')

// ============================================================
// Subleq OISC VM markers
// Magic: ABSQ_BEG / ABSQ_END
// ============================================================

#define ABPROTECT_SUBLEQ_BEGIN \
    _ABPROTECT_MARKER_10(_ab_sq_b, 'A','B','S','Q','_','B','E','G')

#define ABPROTECT_SUBLEQ_END \
    _ABPROTECT_MARKER_10(_ab_sq_e, 'A','B','S','Q','_','E','N','D')

// ============================================================
// Inline check markers (single point, no END needed)
// These are 10-byte markers; the packer replaces them with
// inline shellcode that performs the check at that location.
// Magic: ABCK_DBG / ABCK_CRC / ABCK_DMP
// ============================================================

#define ABPROTECT_CHECK_DEBUGGER \
    _ABPROTECT_MARKER_10(_ab_ck_d, 'A','B','C','K','_','D','B','G')

#define ABPROTECT_CHECK_INTEGRITY \
    _ABPROTECT_MARKER_10(_ab_ck_c, 'A','B','C','K','_','C','R','C')

#define ABPROTECT_CHECK_DUMP \
    _ABPROTECT_MARKER_10(_ab_ck_m, 'A','B','C','K','_','D','M','P')

// ============================================================
// Cloud Computing markers
// Magic: ABCL_BEG / ABCL_END
// Mark functions to be extracted and executed on the server.
// ============================================================

#define ABPROTECT_CLOUD_BEGIN \
    _ABPROTECT_MARKER_10_INNER(_ab_cl_b, 'A','B','C','L','_','B','E','G')

#define ABPROTECT_CLOUD_END \
    _ABPROTECT_MARKER_10_INNER(_ab_cl_e, 'A','B','C','L','_','E','N','D')

#ifdef __cplusplus
}
#endif
