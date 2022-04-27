/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "jit_emit_memory.h"
#include "jit_emit_exception.h"
#include "../jit_frontend.h"
#include "../jit_codegen.h"
#include "../../interpreter/wasm_runtime.h"

static JitReg
get_memory_boundary(JitCompContext *cc, uint32 mem_idx, uint32 bytes)
{
    JitReg memory_boundary;

    switch (bytes) {
        case 1:
        {
            memory_boundary =
                get_mem_bound_check_1byte_reg(cc->jit_frame, mem_idx);
            break;
        }
        case 2:
        {
            memory_boundary =
                get_mem_bound_check_2bytes_reg(cc->jit_frame, mem_idx);
            break;
        }
        case 4:
        {
            memory_boundary =
                get_mem_bound_check_4bytes_reg(cc->jit_frame, mem_idx);
            break;
        }
        case 8:
        {
            memory_boundary =
                get_mem_bound_check_8bytes_reg(cc->jit_frame, mem_idx);
            break;
        }
        case 16:
        {
            memory_boundary =
                get_mem_bound_check_16bytes_reg(cc->jit_frame, mem_idx);
            break;
        }
        default:
        {
            bh_assert(0);
            goto fail;
        }
    }

    return memory_boundary;
fail:
    return 0;
}

#if UINTPTR_MAX == UINT64_MAX
static JitReg
check_and_seek_on_64bit_platform(JitCompContext *cc, JitReg addr, JitReg offset,
                                 JitReg memory_boundary)
{
    JitReg long_addr, offset1;

    /* long_addr = (int64_t)addr */
    long_addr = jit_cc_new_reg_I64(cc);
    GEN_INSN(U32TOI64, long_addr, addr);

    /* offset1 = offset + long_addr */
    offset1 = jit_cc_new_reg_I64(cc);
    GEN_INSN(ADD, offset1, offset, long_addr);

    /* if (offset1 > memory_boundary) goto EXCEPTION */
    GEN_INSN(CMP, cc->cmp_reg, offset1, memory_boundary);
    if (!jit_emit_exception(cc, EXCE_OUT_OF_BOUNDS_MEMORY_ACCESS, JIT_OP_BGTU,
                            cc->cmp_reg, NULL)) {
        goto fail;
    }

    return offset1;
fail:
    return 0;
}
#else
static JitReg
check_and_seek_on_32bit_platform(JitCompContext *cc, JitReg addr, JitReg offset,
                                 JitReg memory_boundary)
{
    JitReg offset1;

    /* offset1 = offset + addr */
    offset1 = jit_cc_new_reg_I32(cc);
    GEN_INSN(ADD, offset1, offset, addr);

    /* if (offset1 < addr) goto EXCEPTION */
    GEN_INSN(CMP, cc->cmp_reg, offset1, addr);
    if (!jit_emit_exception(cc, EXCE_OUT_OF_BOUNDS_MEMORY_ACCESS, JIT_OP_BLTU,
                            cc->cmp_reg, NULL)) {
        goto fail;
    }

    /* if (offset1 > memory_boundary) goto EXCEPTION */
    GEN_INSN(CMP, cc->cmp_reg, offset1, memory_boundary);
    if (!jit_emit_exception(cc, EXCE_OUT_OF_BOUNDS_MEMORY_ACCESS, JIT_OP_BGTU,
                            cc->cmp_reg, NULL)) {
        goto fail;
    }

    return offset1;
fail:
    return 0;
}
#endif

static JitReg
check_and_seek(JitCompContext *cc, JitReg addr, uint32 offset, uint32 bytes)
{
    JitReg memory_boundary, offset1, memory_data, maddr;
    /* the default memory */
    uint32 mem_idx = 0;

    /* ---------- check ---------- */
    /* 1. shortcut if the memory size is 0*/
    if (0 == cc->cur_wasm_module->memories[mem_idx].init_page_count) {
        JitReg memory_inst, cur_mem_page_count;

        /* if (cur_mem_page_count == 0) goto EXCEPTION */
        memory_inst = get_memory_inst_reg(cc->jit_frame, mem_idx);
        cur_mem_page_count = jit_cc_new_reg_I32(cc);
        GEN_INSN(LDI32, cur_mem_page_count, memory_inst,
                 NEW_CONST(I32, offsetof(WASMMemoryInstance, cur_page_count)));
        GEN_INSN(CMP, cc->cmp_reg, cur_mem_page_count, NEW_CONST(I32, 0));
        if (!jit_emit_exception(cc, EXCE_OUT_OF_BOUNDS_MEMORY_ACCESS,
                                JIT_OP_BEQ, cc->cmp_reg, NULL)) {
            goto fail;
        }
    }

    /* 2. a complete boundary check */
    memory_boundary = get_memory_boundary(cc, mem_idx, bytes);
    if (!memory_boundary)
        goto fail;

#if UINTPTR_MAX == UINT64_MAX
    offset1 = check_and_seek_on_64bit_platform(cc, addr, NEW_CONST(I64, offset),
                                               memory_boundary);
    if (!offset1)
        goto fail;
#else
    offset1 = check_and_seek_on_32bit_platform(cc, addr, NEW_CONST(I32, offset),
                                               memory_boundary);
    if (!offset1)
        goto fail;
#endif

    /* ---------- seek ---------- */
    memory_data = get_memory_data_reg(cc->jit_frame, mem_idx);
    maddr = jit_cc_new_reg_ptr(cc);
    GEN_INSN(ADD, maddr, memory_data, offset1);

    return maddr;
fail:
    return 0;
}

bool
jit_compile_op_i32_load(JitCompContext *cc, uint32 align, uint32 offset,
                        uint32 bytes, bool sign, bool atomic)
{
    JitReg addr, maddr, value;

    POP_I32(addr);

    maddr = check_and_seek(cc, addr, offset, bytes);
    if (!maddr) {
        goto fail;
    }

    value = jit_cc_new_reg_I32(cc);
    switch (bytes) {
        case 1:
        {
            if (sign) {
                GEN_INSN(LDI8, value, maddr, NEW_CONST(I32, 0));
            }
            else {
                GEN_INSN(LDU8, value, maddr, NEW_CONST(I32, 0));
            }
            break;
        }
        case 2:
        {
            if (sign) {
                GEN_INSN(LDI16, value, maddr, NEW_CONST(I32, 0));
            }
            else {
                GEN_INSN(LDU16, value, maddr, NEW_CONST(I32, 0));
            }
            break;
        }
        case 4:
        {
            if (sign) {
                GEN_INSN(LDI32, value, maddr, NEW_CONST(I32, 0));
            }
            else {
                GEN_INSN(LDU32, value, maddr, NEW_CONST(I32, 0));
            }
            break;
        }
        default:
        {
            bh_assert(0);
            goto fail;
        }
    }

    PUSH_I32(value);
    return true;
fail:
    return false;
}

bool
jit_compile_op_i64_load(JitCompContext *cc, uint32 align, uint32 offset,
                        uint32 bytes, bool sign, bool atomic)
{
    JitReg addr, maddr, value;

    POP_I32(addr);

    maddr = check_and_seek(cc, addr, offset, bytes);
    if (!maddr) {
        goto fail;
    }

    value = jit_cc_new_reg_I64(cc);
    switch (bytes) {
        case 1:
        {
            if (sign) {
                GEN_INSN(LDI8, value, maddr, NEW_CONST(I32, 0));
            }
            else {
                GEN_INSN(LDU8, value, maddr, NEW_CONST(I32, 0));
            }
            break;
        }
        case 2:
        {
            if (sign) {
                GEN_INSN(LDI16, value, maddr, NEW_CONST(I32, 0));
            }
            else {
                GEN_INSN(LDU16, value, maddr, NEW_CONST(I32, 0));
            }
            break;
        }
        case 4:
        {
            if (sign) {
                GEN_INSN(LDI16, value, maddr, NEW_CONST(I32, 0));
            }
            else {
                GEN_INSN(LDU16, value, maddr, NEW_CONST(I32, 0));
            }
            break;
        }
        case 8:
        {
            if (sign) {
                GEN_INSN(LDI64, value, maddr, NEW_CONST(I32, 0));
            }
            else {
                GEN_INSN(LDU64, value, maddr, NEW_CONST(I32, 0));
            }
            break;
        }
        default:
        {
            bh_assert(0);
            goto fail;
        }
    }

    PUSH_I64(value);
    return true;
fail:
    return false;
}

bool
jit_compile_op_f32_load(JitCompContext *cc, uint32 align, uint32 offset)
{
    return false;
}

bool
jit_compile_op_f64_load(JitCompContext *cc, uint32 align, uint32 offset)
{
    return false;
}

bool
jit_compile_op_i32_store(JitCompContext *cc, uint32 align, uint32 offset,
                         uint32 bytes, bool atomic)
{
    JitReg value, addr, maddr;

    POP_I32(value);
    POP_I32(addr);

    maddr = check_and_seek(cc, addr, offset, bytes);
    if (!maddr) {
        goto fail;
    }

    switch (bytes) {
        case 1:
        {
            GEN_INSN(STI8, value, maddr, NEW_CONST(I32, 0));
            break;
        }
        case 2:
        {
            GEN_INSN(STI16, value, maddr, NEW_CONST(I32, 0));
            break;
        }
        case 4:
        {
            GEN_INSN(STI32, value, maddr, NEW_CONST(I32, 0));
            break;
        }
        default:
        {
            bh_assert(0);
            goto fail;
        }
    }

    return true;
fail:
    return false;
}

bool
jit_compile_op_i64_store(JitCompContext *cc, uint32 align, uint32 offset,
                         uint32 bytes, bool atomic)
{
    JitReg value, addr, maddr;

    POP_I64(value);
    POP_I32(addr);

    maddr = check_and_seek(cc, addr, offset, bytes);
    if (!maddr) {
        goto fail;
    }

    switch (bytes) {
        case 1:
        {
            GEN_INSN(STI8, value, maddr, NEW_CONST(I32, 0));
            break;
        }
        case 2:
        {
            GEN_INSN(STI16, value, maddr, NEW_CONST(I32, 0));
            break;
        }
        case 4:
        {
            GEN_INSN(STI32, value, maddr, NEW_CONST(I32, 0));
            break;
        }
        case 8:
        {
            GEN_INSN(STI64, value, maddr, NEW_CONST(I32, 0));
            break;
        }
        default:
        {
            bh_assert(0);
            goto fail;
        }
    }

    return true;
fail:
    return false;
}

bool
jit_compile_op_f32_store(JitCompContext *cc, uint32 align, uint32 offset)
{
    return false;
}

bool
jit_compile_op_f64_store(JitCompContext *cc, uint32 align, uint32 offset)
{
    return false;
}

bool
jit_compile_op_memory_size(JitCompContext *cc)
{
    return false;
}

bool
jit_compile_op_memory_grow(JitCompContext *cc, uint32 mem_idx)
{
    JitReg delta, module_inst, grow_result, res, memory_inst, prev_page_count;
    JitInsn *insn;

    /* WASMMemoryInstance->cur_page_count before enlarging */
    memory_inst = get_memory_inst_reg(cc->jit_frame, mem_idx);
    prev_page_count = jit_cc_new_reg_I32(cc);
    GEN_INSN(LDI32, prev_page_count, memory_inst,
             NEW_CONST(I32, offsetof(WASMMemoryInstance, cur_page_count)));

    /* call wasm_enlarge_memory */
#if defined(BUILD_TARGET_X86_64) || defined(BUILD_TARGET_AMD_64)
    /* Set native_ret to x86::eax */
    grow_result = jit_codegen_get_hreg_by_name("eax");
#else
    grow_result = jit_cc_new_reg_I32(cc);
#endif
    POP_I32(delta);
    module_inst = get_module_inst_reg(cc->jit_frame);
    insn = GEN_INSN(CALLNATIVE, grow_result,
                    NEW_CONST(PTR, (uintptr_t)wasm_enlarge_memory), 2);
    if (insn) {
        *(jit_insn_opndv(insn, 2)) = module_inst;
        *(jit_insn_opndv(insn, 3)) = delta;
    }

    /* check if enlarge memory success */
    res = jit_cc_new_reg_I32(cc);
    GEN_INSN(CMP, cc->cmp_reg, grow_result, NEW_CONST(I32, 0));
    GEN_INSN(SELECTNE, res, cc->cmp_reg, prev_page_count,
             NEW_CONST(I32, (int32)-1));
    PUSH_I32(res);

    /* ensure a refresh in next get_memory_XXX_reg */
    clear_memory_regs(cc->jit_frame);

    return true;
fail:
    return false;
}

#if WASM_ENABLE_BULK_MEMORY != 0
bool
jit_compile_op_memory_init(JitCompContext *cc, uint32 seg_index)
{
    return false;
}

bool
jit_compile_op_data_drop(JitCompContext *cc, uint32 seg_index)
{
    return false;
}

bool
jit_compile_op_memory_copy(JitCompContext *cc)
{
    return false;
}

bool
jit_compile_op_memory_fill(JitCompContext *cc)
{
    return false;
}
#endif

#if WASM_ENABLE_SHARED_MEMORY != 0
bool
jit_compile_op_atomic_rmw(JitCompContext *cc, uint8 atomic_op, uint8 op_type,
                          uint32 align, uint32 offset, uint32 bytes)
{
    return false;
}

bool
jit_compile_op_atomic_cmpxchg(JitCompContext *cc, uint8 op_type, uint32 align,
                              uint32 offset, uint32 bytes)
{
    return false;
}

bool
jit_compile_op_atomic_wait(JitCompContext *cc, uint8 op_type, uint32 align,
                           uint32 offset, uint32 bytes)
{
    return false;
}

bool
jit_compiler_op_atomic_notify(JitCompContext *cc, uint32 align, uint32 offset,
                              uint32 bytes)
{
    return false;
}
#endif