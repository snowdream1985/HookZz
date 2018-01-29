/**
 *    Copyright 2017 jmpews
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "interceptor-arm.h"

#include <stdlib.h>

#define ZZ_THUMB_TINY_REDIRECT_SIZE 4
#define ZZ_THUMB_FULL_REDIRECT_SIZE 8
#define ZZ_ARM_TINY_REDIRECT_SIZE 4
#define ZZ_ARM_FULL_REDIRECT_SIZE 8

ZzInterceptorBackend *ZzBuildInteceptorBackend(ZzAllocator *allocator) {
    if (!ZzMemoryIsSupportAllocateRXPage()) {
        return NULL;
    }
    ZZSTATUS status;
    ZzInterceptorBackend *backend = (ZzInterceptorBackend *)zz_malloc_with_zero(sizeof(ZzInterceptorBackend));

    zz_arm_writer_init(&backend->arm_writer, NULL);
    zz_arm_relocator_init(&backend->arm_relocator, NULL, &backend->arm_writer);
    zz_thumb_writer_init(&backend->thumb_writer, NULL);
    zz_thumb_relocator_init(&backend->thumb_relocator, NULL, &backend->thumb_writer);

    backend->allocator   = allocator;
    backend->enter_thunk = NULL;
    backend->half_thunk  = NULL;
    backend->leave_thunk = NULL;

    status = ZzThunkerBuildThunk(backend);

    if (status == ZZ_FAILED) {
        ZzDebugInfoLog("%s", "ZzThunkerBuildThunk return ZZ_FAILED\n");
        return NULL;
    }

    return backend;
}

ZzCodeSlice *zz_thumb_code_patch(ZzThumbAssemblerWriter *thumb_writer, ZzAllocator *allocator, zz_addr_t target_addr,
                                 zz_size_t range_size) {
    ZzCodeSlice *code_slice = NULL;
    if (range_size > 0) {
        code_slice = ZzNewNearCodeSlice(allocator, target_addr, range_size, thumb_writer->size);
    } else {
        code_slice = ZzNewCodeSlice(allocator, thumb_writer->size + 4);
    }
    if (!code_slice)
        return NULL;

    if (!ZzMemoryPatchCode((zz_addr_t)code_slice->data, thumb_writer->base, thumb_writer->size)) {

        free(code_slice);
        return NULL;
    }
    return code_slice;
}

ZzCodeSlice *zz_thumb_relocate_code_patch(ZzThumbRelocator *thumb_relocator, ZzThumbAssemblerWriter *thumb_writer,
                                          ZzAllocator *allocator, zz_addr_t target_addr, zz_size_t range_size) {
    ZzCodeSlice *code_slice = NULL;
    if (range_size > 0) {
        code_slice = ZzNewNearCodeSlice(allocator, target_addr, range_size, thumb_writer->size);
    } else {
        code_slice = ZzNewCodeSlice(allocator, thumb_writer->size + 4);
    }
    if (!code_slice)
        return NULL;

    zz_thumb_relocator_relocate_writer(thumb_relocator, (zz_addr_t)code_slice->data);

    if (!ZzMemoryPatchCode((zz_addr_t)code_slice->data, thumb_writer->base, thumb_writer->size)) {

        free(code_slice);
        return NULL;
    }
    return code_slice;
}

ZzCodeSlice *zz_arm_code_patch(ZzARMAssemblerWriter *arm_writer, ZzAllocator *allocator, zz_addr_t target_addr,
                               zz_size_t range_size) {
    ZzCodeSlice *code_slice = NULL;
    if (range_size > 0) {
        code_slice = ZzNewNearCodeSlice(allocator, target_addr, range_size, arm_writer->size);
    } else {
        code_slice = ZzNewCodeSlice(allocator, arm_writer->size + 4);
    }
    if (!code_slice)
        return NULL;

    if (!ZzMemoryPatchCode((zz_addr_t)code_slice->data, arm_writer->base, arm_writer->size)) {
        free(code_slice);
        return NULL;
    }
    return code_slice;
}

ZzCodeSlice *zz_arm_relocate_code_patch(ZzARMRelocator *arm_relocator, ZzARMAssemblerWriter *arm_writer,
                                        ZzAllocator *allocator, zz_addr_t target_addr, zz_size_t range_size) {
    ZzCodeSlice *code_slice = NULL;
    if (range_size > 0) {
        code_slice = ZzNewNearCodeSlice(allocator, target_addr, range_size, arm_writer->size);
    } else {
        code_slice = ZzNewCodeSlice(allocator, arm_writer->size + 4);
    }
    if (!code_slice)
        return NULL;

    zz_arm_relocator_relocate_writer(arm_relocator, (zz_addr_t)code_slice->data);

    if (!ZzMemoryPatchCode((zz_addr_t)code_slice->data, arm_writer->base, arm_writer->size)) {
        free(code_slice);
        return NULL;
    }
    return code_slice;
}

ZZSTATUS ZzPrepareTrampoline(ZzInterceptorBackend *self, ZzHookFunctionEntry *entry) {
    bool is_thumb            = FALSE;
    zz_addr_t target_addr    = (zz_addr_t)entry->target_ptr;
    zz_size_t redirect_limit = 0;

    ZzARMHookFunctionEntryBackend *entry_backend;
    entry_backend = (ZzARMHookFunctionEntryBackend *)zz_malloc_with_zero(sizeof(ZzARMHookFunctionEntryBackend));

    entry->backend = (struct _ZzHookFunctionEntryBackend *)entry_backend;

    is_thumb = INSTRUCTION_IS_THUMB((zz_addr_t)entry->target_ptr);
    if (is_thumb)
        target_addr = (zz_addr_t)entry->target_ptr & ~(zz_addr_t)1;

    if (is_thumb) {
        if (entry->try_near_jump) {
            entry_backend->redirect_code_size = ZZ_THUMB_TINY_REDIRECT_SIZE;
        } else {
            zz_thumb_relocator_try_relocate((zz_ptr_t)target_addr, ZZ_THUMB_FULL_REDIRECT_SIZE, &redirect_limit);
            if (redirect_limit != 0 && redirect_limit > ZZ_THUMB_TINY_REDIRECT_SIZE &&
                redirect_limit < ZZ_THUMB_FULL_REDIRECT_SIZE) {
                entry->try_near_jump              = TRUE;
                entry_backend->redirect_code_size = ZZ_THUMB_TINY_REDIRECT_SIZE;
            } else if (redirect_limit != 0 && redirect_limit < ZZ_THUMB_TINY_REDIRECT_SIZE) {
                return ZZ_FAILED;
            } else {
                entry_backend->redirect_code_size = ZZ_THUMB_FULL_REDIRECT_SIZE;
                if (target_addr % 4) {
                    entry_backend->redirect_code_size += 2;
                }
            }
        }
        self->thumb_relocator.try_relocated_length = entry_backend->redirect_code_size;
    } else {
        if (entry->try_near_jump) {
            entry_backend->redirect_code_size = ZZ_ARM_TINY_REDIRECT_SIZE;
        } else {
            zz_arm_relocator_try_relocate((zz_ptr_t)target_addr, ZZ_ARM_FULL_REDIRECT_SIZE, &redirect_limit);
            if (redirect_limit != 0 && redirect_limit > ZZ_ARM_TINY_REDIRECT_SIZE &&
                redirect_limit < ZZ_ARM_FULL_REDIRECT_SIZE) {
                entry->try_near_jump              = TRUE;
                entry_backend->redirect_code_size = ZZ_ARM_TINY_REDIRECT_SIZE;
            } else if (redirect_limit != 0 && redirect_limit < ZZ_ARM_TINY_REDIRECT_SIZE) {
                return ZZ_FAILED;
            } else {
                entry_backend->redirect_code_size = ZZ_ARM_FULL_REDIRECT_SIZE;
            }
        }
        self->arm_relocator.try_relocated_length = entry_backend->redirect_code_size;
    }

    zz_arm_relocator_init(&self->arm_relocator, (zz_ptr_t)target_addr, &self->arm_writer);
    zz_thumb_relocator_init(&self->thumb_relocator, (zz_ptr_t)target_addr, &self->thumb_writer);
    return ZZ_SUCCESS;
}

ZZSTATUS ZzBuildEnterTransferTrampoline(ZzInterceptorBackend *self, ZzHookFunctionEntry *entry) {
    char temp_code_slice_data[256]               = {0};
    ZzARMAssemblerWriter *arm_writer             = NULL;
    ZzARMAssemblerWriter *thumb_writer           = NULL;
    ZzCodeSlice *code_slice                      = NULL;
    ZzARMHookFunctionEntryBackend *entry_backend = (ZzARMHookFunctionEntryBackend *)entry->backend;
    ZZSTATUS status                              = ZZ_SUCCESS;
    bool is_thumb                                = TRUE;
    zz_addr_t target_addr                        = (zz_addr_t)entry->target_ptr;

    is_thumb = INSTRUCTION_IS_THUMB((zz_addr_t)entry->target_ptr);
    if (is_thumb)
        target_addr = (zz_addr_t)entry->target_ptr & ~(zz_addr_t)1;

    if (is_thumb) {
        thumb_writer = &self->thumb_writer;
        zz_thumb_writer_reset(thumb_writer, temp_code_slice_data);

        /* jump to on_enter_trampoline */
        zz_thumb_writer_put_ldr_reg_address(thumb_writer, ZZ_ARM_REG_PC, (zz_addr_t)entry->on_enter_trampoline);

        /* code patch */
        code_slice =
            zz_thumb_code_patch(thumb_writer, self->allocator, target_addr, zz_thumb_writer_near_jump_range_size());
        if (code_slice)
            entry->on_enter_transfer_trampoline = code_slice->data;
        else
            return ZZ_FAILED;
    } else {
        arm_writer = &self->arm_writer;
        zz_arm_writer_reset(arm_writer, temp_code_slice_data);

        /* jump to on_enter_trampoline */
        zz_arm_writer_put_ldr_reg_address(arm_writer, ZZ_ARM_REG_PC, (zz_addr_t)entry->on_enter_trampoline);

        /* code patch */
        code_slice = zz_arm_code_patch(arm_writer, self->allocator, target_addr, zz_arm_writer_near_jump_range_size());
        if (code_slice)
            entry->on_enter_transfer_trampoline = code_slice->data;
        else
            return ZZ_FAILED;
    }

    if (ZzIsEnableDebugMode()) {
        char buffer[1024] = {};
        sprintf(buffer + strlen(buffer), "%s\n", "ZzBuildEnterTransferTrampoline:");
        sprintf(buffer + strlen(buffer),
                "LogInfo: on_enter_transfer_trampoline at %p, length: %ld. and will jump to "
                "on_enter_trampoline(%p).\n",
                code_slice->data, code_slice->size, entry->on_enter_trampoline);
        ZzDebugInfoLog("%s", buffer);
    }

    free(code_slice);
    return status;
}

ZZSTATUS ZzBuildEnterTrampoline(ZzInterceptorBackend *self, ZzHookFunctionEntry *entry) {
    char temp_code_slice_data[256]               = {0};
    ZzARMAssemblerWriter *arm_writer             = NULL;
    ZzARMAssemblerWriter *thumb_writer           = NULL;
    ZzCodeSlice *code_slice                      = NULL;
    ZzARMHookFunctionEntryBackend *entry_backend = (ZzARMHookFunctionEntryBackend *)entry->backend;
    ZZSTATUS status                              = ZZ_SUCCESS;
    bool is_thumb;

    is_thumb = INSTRUCTION_IS_THUMB((zz_addr_t)entry->target_ptr);

    thumb_writer = &self->thumb_writer;
    zz_thumb_writer_reset(thumb_writer, temp_code_slice_data);

    /* prepare 2 stack space: 1. next_hop 2. entry arg */
    zz_thumb_writer_put_sub_reg_imm(thumb_writer, ZZ_ARM_REG_SP, 0xc);
    zz_thumb_writer_put_str_reg_reg_offset(thumb_writer, ZZ_ARM_REG_R1, ZZ_ARM_REG_SP, 0x0); // push r7
    zz_thumb_writer_put_ldr_b_reg_address(thumb_writer, ZZ_ARM_REG_R1, (zz_addr_t)entry);
    zz_thumb_writer_put_str_reg_reg_offset(thumb_writer, ZZ_ARM_REG_R1, ZZ_ARM_REG_SP, 0x4);
    zz_thumb_writer_put_ldr_reg_reg_offset(thumb_writer, ZZ_ARM_REG_R1, ZZ_ARM_REG_SP, 0x0); // pop r7
    zz_thumb_writer_put_add_reg_imm(thumb_writer, ZZ_ARM_REG_SP, 0x4);

    /* jump to enter thunk */
    zz_thumb_writer_put_ldr_reg_address(thumb_writer, ZZ_ARM_REG_PC, (zz_addr_t)self->enter_thunk);

    /* code patch */
    code_slice = zz_thumb_code_patch(thumb_writer, self->allocator, 0, 0);
    if (code_slice)
        entry->on_enter_trampoline = code_slice->data + 1;
    else
        return ZZ_FAILED;

    /* debug log */
    if (ZzIsEnableDebugMode()) {
        char buffer[1024] = {};
        sprintf(buffer + strlen(buffer), "%s\n", "ZzBuildEnterTrampoline:");
        sprintf(buffer + strlen(buffer),
                "LogInfo: on_enter_trampoline at %p, length: %ld. hook-entry: %p. and will jump to "
                "enter_thunk(%p)\n",
                code_slice->data, code_slice->size, (void *)entry, (void *)self->enter_thunk);
        ZzDebugInfoLog("%s", buffer);
    }

    if ((is_thumb && entry_backend->redirect_code_size == ZZ_THUMB_TINY_REDIRECT_SIZE) ||
        (!is_thumb && entry_backend->redirect_code_size == ZZ_ARM_TINY_REDIRECT_SIZE)) {
        ZzBuildEnterTransferTrampoline(self, entry);
    }

    free(code_slice);
    return status;
}

ZZSTATUS ZzBuildInvokeTrampoline(ZzInterceptorBackend *self, ZzHookFunctionEntry *entry) {
    char temp_code_slice_data[256]               = {0};
    ZzCodeSlice *code_slice                      = NULL;
    ZzARMHookFunctionEntryBackend *entry_backend = (ZzARMHookFunctionEntryBackend *)entry->backend;
    ZZSTATUS status                              = ZZ_SUCCESS;
    bool is_thumb                                = TRUE;
    zz_addr_t target_addr                        = (zz_addr_t)entry->target_ptr;
    zz_ptr_t target_end_addr                     = 0;
    zz_ptr_t restore_target_addr;

    is_thumb = INSTRUCTION_IS_THUMB((zz_addr_t)entry->target_ptr);
    if (is_thumb)
        target_addr = (zz_addr_t)entry->target_ptr & ~(zz_addr_t)1;

    if (is_thumb) {
        target_end_addr = (zz_ptr_t)((zz_addr_t)entry->target_end_ptr & ~(zz_addr_t)1);
    }

    if (is_thumb) {
        ZzThumbRelocator *thumb_relocator;
        ZzThumbAssemblerWriter *thumb_writer;
        thumb_relocator = &self->thumb_relocator;
        thumb_writer    = &self->thumb_writer;

        zz_thumb_writer_reset(thumb_writer, temp_code_slice_data);
        zz_thumb_relocator_reset(thumb_relocator, (zz_ptr_t)target_addr, thumb_writer);
        zz_size_t tmp_relocator_insn_size = 0;
        entry->target_half_ret_addr       = 0;

        if (entry->hook_type == HOOK_FUNCTION_TYPE) {
            do {
                zz_thumb_relocator_read_one(thumb_relocator, NULL);
                tmp_relocator_insn_size = thumb_relocator->input_cur - thumb_relocator->input_start;
            } while (tmp_relocator_insn_size < entry_backend->redirect_code_size);
            zz_thumb_relocator_write_all(thumb_relocator);
        } else if (entry->hook_type == HOOK_ADDRESS_TYPE) {
            do {
                zz_thumb_relocator_read_one(thumb_relocator, NULL);
                zz_thumb_relocator_write_one(thumb_relocator);
                tmp_relocator_insn_size = thumb_relocator->input_cur - thumb_relocator->input_start;
                if (thumb_relocator->input_cur >= target_end_addr && !entry->target_half_ret_addr) {
                    /* jump to rest target address */
                    zz_thumb_writer_put_ldr_reg_address(thumb_writer, ZZ_ARM_REG_PC,
                                                        (zz_addr_t)entry->on_half_trampoline);
                    entry->target_half_ret_addr = (zz_ptr_t)(thumb_writer->size + 1);
                }
            } while (tmp_relocator_insn_size < entry_backend->redirect_code_size ||
                     thumb_relocator->input_cur < target_end_addr);
        }

        /* jump to rest target address */
        restore_target_addr = (zz_ptr_t)((zz_addr_t)target_addr + tmp_relocator_insn_size);
        zz_thumb_writer_put_ldr_reg_address(thumb_writer, ZZ_ARM_REG_PC, (zz_addr_t)(restore_target_addr + 1));

        /* code patch */
        code_slice = zz_thumb_relocate_code_patch(thumb_relocator, thumb_writer, self->allocator, 0, 0);
        if (code_slice)
            entry->on_invoke_trampoline = code_slice->data + 1;
        else
            return ZZ_FAILED;
    } else {
        ZzARMRelocator *arm_relocator;
        ZzARMAssemblerWriter *arm_writer;
        arm_relocator = &self->arm_relocator;
        arm_writer    = &self->arm_writer;

        zz_arm_writer_reset(arm_writer, temp_code_slice_data);
        zz_arm_relocator_reset(arm_relocator, (zz_ptr_t)target_addr, arm_writer);
        entry->target_half_ret_addr       = 0;
        zz_size_t tmp_relocator_insn_size = 0;

        if (entry->hook_type == HOOK_FUNCTION_TYPE) {
            do {
                zz_arm_relocator_read_one(arm_relocator, NULL);
                tmp_relocator_insn_size = arm_relocator->input_cur - arm_relocator->input_start;
            } while (tmp_relocator_insn_size < entry_backend->redirect_code_size);
            zz_arm_relocator_write_all(arm_relocator);
        } else if (entry->hook_type == HOOK_ADDRESS_TYPE) {
            do {
                zz_arm_relocator_read_one(arm_relocator, NULL);
                zz_arm_relocator_write_one(arm_relocator);
                tmp_relocator_insn_size = arm_relocator->input_cur - arm_relocator->input_start;
                if (arm_relocator->input_cur >= target_end_addr && !entry->target_half_ret_addr) {
                    /* jump to rest target address */
                    zz_arm_writer_put_ldr_reg_address(arm_writer, ZZ_ARM_REG_PC, (zz_addr_t)entry->on_half_trampoline);
                    entry->target_half_ret_addr = (zz_ptr_t)arm_writer->size;
                }
            } while (tmp_relocator_insn_size < entry_backend->redirect_code_size ||
                     arm_relocator->input_cur < target_end_addr);
        }

        /* jump to rest target address */
        restore_target_addr = (zz_ptr_t)((zz_addr_t)target_addr + tmp_relocator_insn_size);
        zz_arm_writer_put_ldr_reg_address(arm_writer, ZZ_ARM_REG_PC, (zz_addr_t)restore_target_addr);

        /* code patch */
        code_slice = zz_arm_relocate_code_patch(arm_relocator, arm_writer, self->allocator, 0, 0);
        if (code_slice)
            entry->on_invoke_trampoline = code_slice->data;
        else
            return ZZ_FAILED;
    }

    /* update target_half_ret_addr */
    if (entry->hook_type == HOOK_ADDRESS_TYPE) {
        entry->target_half_ret_addr += (zz_addr_t)code_slice->data;
    }

    /* debug log */
    if (ZzIsEnableDebugMode()) {
        char buffer[1024] = {};
        sprintf(buffer + strlen(buffer), "%s\n", "ZzBuildInvokeTrampoline:");
        sprintf(buffer + strlen(buffer),
                "LogInfo: on_invoke_trampoline at %p, length: %ld. and will jump to rest code(%p).\n", code_slice->data,
                code_slice->size, restore_target_addr);
        if (is_thumb) {
            sprintf(buffer + strlen(buffer),
                    "ThumbInstructionFix: origin instruction at %p, end at %p, relocator instruction nums %d\n",
                    (&self->thumb_relocator)->input_start, (&self->thumb_relocator)->input_cur,
                    (&self->thumb_relocator)->inpos);
        } else {
            sprintf(buffer + strlen(buffer),
                    "ARMInstructionFix: origin instruction at %p, end at %p, relocator instruction nums %d\n",
                    (&self->arm_relocator)->input_start, (&self->arm_relocator)->input_cur,
                    (&self->arm_relocator)->inpos);
        }

        char origin_prologue[256] = {0};
        int t                     = 0;
        zz_ptr_t p;
        if (is_thumb) {
            for (p = (&self->thumb_relocator)->input_start; p < (&self->thumb_relocator)->input_cur; p++, t = t + 5) {
                sprintf(origin_prologue + t, "0x%.2x ", *(unsigned char *)p);
            }
        } else {
            for (p = (&self->arm_relocator)->input_start; p < (&self->arm_relocator)->input_cur; p++, t = t + 5) {
                sprintf(origin_prologue + t, "0x%.2x ", *(unsigned char *)p);
            }
        }
        sprintf(buffer + strlen(buffer), "origin_prologue: %s\n", origin_prologue);

        ZzDebugInfoLog("%s", buffer);
    }

    free(code_slice);
    return status;
}

ZZSTATUS ZzBuildHalfTrampoline(ZzInterceptorBackend *self, ZzHookFunctionEntry *entry) {
    char temp_code_slice_data[256]     = {0};
    ZzARMAssemblerWriter *arm_writer   = NULL;
    ZzARMAssemblerWriter *thumb_writer = NULL;
    ZzCodeSlice *code_slice            = NULL;
    ZZSTATUS status                    = ZZ_SUCCESS;

    thumb_writer = &self->thumb_writer;
    zz_thumb_writer_reset(thumb_writer, temp_code_slice_data);

    /* prepare 2 stack space: 1. next_hop 2. entry arg */
    zz_thumb_writer_put_sub_reg_imm(thumb_writer, ZZ_ARM_REG_SP, 0xc);
    zz_thumb_writer_put_str_reg_reg_offset(thumb_writer, ZZ_ARM_REG_R1, ZZ_ARM_REG_SP, 0x0); // push r7
    zz_thumb_writer_put_ldr_b_reg_address(thumb_writer, ZZ_ARM_REG_R1, (zz_addr_t)entry);
    zz_thumb_writer_put_str_reg_reg_offset(thumb_writer, ZZ_ARM_REG_R1, ZZ_ARM_REG_SP, 0x4);
    zz_thumb_writer_put_ldr_reg_reg_offset(thumb_writer, ZZ_ARM_REG_R1, ZZ_ARM_REG_SP, 0x0); // pop r7
    zz_thumb_writer_put_add_reg_imm(thumb_writer, ZZ_ARM_REG_SP, 0x4);

    /* jump to half_thunk */
    zz_thumb_writer_put_ldr_reg_address(thumb_writer, ZZ_ARM_REG_PC, (zz_addr_t)self->half_thunk);

    /* code patch */
    code_slice = zz_thumb_code_patch(thumb_writer, self->allocator, 0, 0);
    if (code_slice)
        entry->on_half_trampoline = code_slice->data + 1;
    else
        return ZZ_FAILED;

    free(code_slice);
    return status;
}

ZZSTATUS ZzBuildLeaveTrampoline(ZzInterceptorBackend *self, ZzHookFunctionEntry *entry) {
    char temp_code_slice_data[256] = {0};
    ZzCodeSlice *code_slice        = NULL;
    ZZSTATUS status                = ZZ_SUCCESS;
    bool is_thumb                  = TRUE;
    ZzARMAssemblerWriter *thumb_writer;

    thumb_writer = &self->thumb_writer;
    zz_thumb_writer_reset(thumb_writer, temp_code_slice_data);

    /* prepare 2 stack space: 1. next_hop 2. entry arg */
    zz_thumb_writer_put_sub_reg_imm(thumb_writer, ZZ_ARM_REG_SP, 0xc);
    zz_thumb_writer_put_str_reg_reg_offset(thumb_writer, ZZ_ARM_REG_R1, ZZ_ARM_REG_SP, 0x0); // push r7
    zz_thumb_writer_put_ldr_b_reg_address(thumb_writer, ZZ_ARM_REG_R1, (zz_addr_t)entry);
    zz_thumb_writer_put_str_reg_reg_offset(thumb_writer, ZZ_ARM_REG_R1, ZZ_ARM_REG_SP, 0x4);
    zz_thumb_writer_put_ldr_reg_reg_offset(thumb_writer, ZZ_ARM_REG_R1, ZZ_ARM_REG_SP, 0x0); // pop r7
    zz_thumb_writer_put_add_reg_imm(thumb_writer, ZZ_ARM_REG_SP, 0x4);

    /* jump to leave_thunk */
    zz_thumb_writer_put_ldr_reg_address(thumb_writer, ZZ_ARM_REG_PC, (zz_addr_t)self->leave_thunk);

    /* code patch */
    code_slice = zz_thumb_code_patch(thumb_writer, self->allocator, 0, 0);
    if (code_slice)
        entry->on_leave_trampoline = code_slice->data + 1;
    else
        return ZZ_FAILED;

    /* debug log */
    if (ZzIsEnableDebugMode()) {
        char buffer[1024] = {};
        sprintf(buffer + strlen(buffer), "%s\n", "ZzBuildLeaveTrampoline:");
        sprintf(buffer + strlen(buffer),
                "LogInfo: on_leave_trampoline at %p, length: %ld. and will jump to leave_thunk(%p).\n",
                code_slice->data, code_slice->size, self->leave_thunk);
        ZzDebugInfoLog("%s", buffer);
    }

    free(code_slice);
    return ZZ_DONE;
}

ZZSTATUS ZzActivateTrampoline(ZzInterceptorBackend *self, ZzHookFunctionEntry *entry) {
    char temp_code_slice_data[256]               = {0};
    ZzCodeSlice *code_slice                      = NULL;
    ZzARMHookFunctionEntryBackend *entry_backend = (ZzARMHookFunctionEntryBackend *)entry->backend;
    ZZSTATUS status                              = ZZ_SUCCESS;
    bool is_thumb                                = TRUE;
    zz_addr_t target_addr                        = (zz_addr_t)entry->target_ptr;

    is_thumb = INSTRUCTION_IS_THUMB((zz_addr_t)entry->target_ptr);
    if (is_thumb)
        target_addr = (zz_addr_t)entry->target_ptr & ~(zz_addr_t)1;

    if (is_thumb) {
        ZzThumbAssemblerWriter *thumb_writer;
        thumb_writer = &self->thumb_writer;
        zz_thumb_writer_reset(thumb_writer, temp_code_slice_data);
        thumb_writer->pc = target_addr + 4;

        if (entry_backend->redirect_code_size == ZZ_THUMB_TINY_REDIRECT_SIZE) {
            zz_thumb_writer_put_b_imm32(thumb_writer,
                                        (zz_addr_t)entry->on_enter_transfer_trampoline - (zz_addr_t)thumb_writer->pc);
        } else {
            zz_thumb_writer_put_ldr_reg_address(thumb_writer, ZZ_ARM_REG_PC, (zz_addr_t)entry->on_enter_trampoline);
        }
        if (!ZzMemoryPatchCode((zz_addr_t)target_addr, thumb_writer->base, thumb_writer->size))
            return ZZ_FAILED;
    } else {
        ZzARMAssemblerWriter *arm_writer;
        arm_writer = &self->arm_writer;
        zz_arm_writer_reset(arm_writer, temp_code_slice_data);
        arm_writer->pc = target_addr + 8;

        if (entry_backend->redirect_code_size == ZZ_ARM_TINY_REDIRECT_SIZE) {
            zz_arm_writer_put_b_imm(arm_writer,
                                    (zz_addr_t)entry->on_enter_transfer_trampoline - (zz_addr_t)arm_writer->pc);
        } else {
            zz_arm_writer_put_ldr_reg_address(arm_writer, ZZ_ARM_REG_PC, (zz_addr_t)entry->on_enter_trampoline);
        }
        if (!ZzMemoryPatchCode((zz_addr_t)target_addr, arm_writer->base, arm_writer->size))
            return ZZ_FAILED;
    }

    return ZZ_DONE_HOOK;
}
