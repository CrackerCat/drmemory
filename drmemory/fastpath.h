/* **********************************************************
 * Copyright (c) 2008-2009 VMware, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/***************************************************************************
 * fastpath.c: Dr. Memory shadow instrumentation fastpath
 */

#ifndef _FASTPATH_H_
#define _FASTPATH_H_ 1

#include "callstack.h" /* app_loc_t */

/* reg liveness */
enum {
    LIVE_UNKNOWN,
    LIVE_LIVE,
    LIVE_DEAD,
};
#define NUM_LIVENESS_REGS 8

typedef struct _scratch_reg_info_t {
    reg_id_t reg;
    bool used;
    bool dead;
    bool global; /* spilled across whole bb (PR 489221) */
    /* we spill if used && !dead, either via xchg w/ a dead reg or via
     * a tls spill slot
     */
    reg_id_t xchg;
    int slot;
} scratch_reg_info_t;

struct _bb_info_t; /* forward decl */
typedef struct _bb_info_t bb_info_t;

#define MAX_FASTPATH_SRCS 3
#define MAX_FASTPATH_DSTS 2
typedef struct _fastpath_info_t {
    bb_info_t *bb;

    /* Filled in by instr_ok_for_instrument_fastpath()
     * The fastpath handles up to 3 sources and 2 dests subject to:
     * - Only one source memop
     * - Only one dest memop
     * Memop opnds are always #0, even for alu where dst[0]==src[0].
     * Opnds are packed: i.e., if opnd_is_null(src[i]) then always
     * opnd_is_null(src[i+1]).
     * We handle a 2nd dest that is a register by writing same result to it.
     */
    opnd_t src[MAX_FASTPATH_SRCS];
    opnd_t dst[MAX_FASTPATH_DSTS];
    int opnum[MAX_FASTPATH_SRCS];
    bool store;
    bool load;
    bool pushpop;
    bool mem2mem;
    bool load2x; /* two mem sources */

    /* filled in by adjust_opnds_for_fastpath() */
    reg_id_t src_reg;
    reg_id_t dst_reg;
    opnd_t memop;
    int opsz; /* destination operand size */
    uint memsz; /* primary memory ref size */
    int src_opsz; /* source operand size */
    opnd_t offs; /* if sub-dword, offset within containing dword */
    bool check_definedness;

    /* filled in by instrument_fastpath() */
    bool zero_rest_of_offs; /* when calculate mi->offs, zero rest of bits in reg */
    bool pushpop_stackop;
    bool need_offs;
    bool need_slowpath;
    instr_t *slowpath;
    /* scratch registers */
    int aflags; /* plus eax for aflags */
    scratch_reg_info_t eax;
    scratch_reg_info_t reg1;
    scratch_reg_info_t reg2;
    scratch_reg_info_t reg3;
    /* is this instr using shared xl8? */
    bool use_shared;
    /* for jmp-to-slowpath optimization (PR 494769) */
    instr_t *appclone;
    instr_t *slow_store_retaddr;
    instr_t *slow_jmp;
} fastpath_info_t;

/* Share inter-instruction info across whole bb */
struct _bb_info_t {
    /* whole-bb spilling (PR 489221) */
    int aflags;
    bool eax_dead;
    bool eflags_used;
    scratch_reg_info_t reg1;
    scratch_reg_info_t reg2;
    /* the instr after which we should spill global regs */
    instr_t *spill_after;
    /* elide redundant addressable checks for base/index registers */
    bool addressable[NUM_LIVENESS_REGS];
    /* elide redundant eflags definedness check for cmp/test,jcc */
    bool eflags_defined;
    /* PR 493257: share shadow translation across multiple instrs */
    opnd_t shared_memop;      /* the orig memop that did a full load */
    int shared_disp_reg1;     /* disp from orig memop already in reg1 */
    int shared_disp_implicit; /* implicit disp from orig memop (push/pop) */
};

/* Info per bb we need to save in order to restore app state */
typedef struct _bb_saved_info_t {
    reg_id_t scratch1;
    reg_id_t scratch2;
    /* This is used to handle non-precise flushing */
    byte ignore_next_delete;
    bool eflags_saved:1;
    /* For PR 578892, to avoid DR having to store translations */
    bool check_ignore_unaddr:1;
    app_pc last_instr;
} bb_saved_info_t;

bool
instr_ok_for_instrument_fastpath(instr_t *inst, fastpath_info_t *mi, bb_info_t *bi);

#ifdef LINUX
dr_signal_action_t
event_signal_instrument(void *drcontext, dr_siginfo_t *info);
#else
bool
event_exception_instrument(void *drcontext, dr_exception_t *excpt);
#endif

void
initialize_fastpath_info(fastpath_info_t *mi, bb_info_t *bi);

void
instrument_fastpath(void *drcontext, instrlist_t *bb, instr_t *inst,
                    fastpath_info_t *mi, bool check_ignore_unaddr);

/* Whole-bb spilling */
bool
whole_bb_spills_enabled(void);

void
mark_scratch_reg_used(void *drcontext, instrlist_t *bb,
                      bb_info_t *bi, scratch_reg_info_t *si);

void
mark_eflags_used(void *drcontext, instrlist_t *bb, bb_info_t *bi);

void
fastpath_top_of_bb(void *drcontext, void *tag, instrlist_t *bb, bb_info_t *bi);

void
fastpath_pre_instrument(void *drcontext, instrlist_t *bb, instr_t *inst, bb_info_t *bi);

void
fastpath_pre_app_instr(void *drcontext, instrlist_t *bb, instr_t *inst,
                       bb_info_t *bi, fastpath_info_t *mi);

void
fastpath_bottom_of_bb(void *drcontext, void *tag, instrlist_t *bb,
                      bb_info_t *bi, bool added_instru, bool translating,
                      bool check_ignore_unaddr);

void
slow_path_xl8_sharing(app_loc_t *loc, size_t inst_sz, opnd_t memop, dr_mcontext_t *mc);

/***************************************************************************
 * For stack.c: perhaps should move stack.c's fastpath code here and avoid
 * exporting these?
 */

void
insert_spill_or_restore(void *drcontext, instrlist_t *bb, instr_t *inst,
                        scratch_reg_info_t *si, bool spill, bool just_xchg);

bool
insert_spill_global(void *drcontext, instrlist_t *bb, instr_t *inst,
                    scratch_reg_info_t *si, bool spill);

void
pick_scratch_regs(instr_t *inst, fastpath_info_t *mi, bool only_abcd, bool need3,
                  bool reg3_must_be_ecx, opnd_t no_overlap1, opnd_t no_overlap2);

void
insert_save_aflags(void *drcontext, instrlist_t *bb, instr_t *inst,
                   scratch_reg_info_t *si, int aflags);

void
insert_restore_aflags(void *drcontext, instrlist_t *bb, instr_t *inst,
                      scratch_reg_info_t *si, int aflags);

void
add_jcc_slowpath(void *drcontext, instrlist_t *bb, instr_t *inst, uint jcc_opcode,
                 fastpath_info_t *mi);

void
add_shadow_table_lookup(void *drcontext, instrlist_t *bb, instr_t *inst,
                        fastpath_info_t *mi,
                        bool get_value, bool value_in_reg2, bool need_offs,
                        bool zero_rest_of_offs,
                        reg_id_t reg1, reg_id_t reg2, reg_id_t reg3,
                        bool jcc_short_slowpath);

/***************************************************************************
 * Utility routines
 */

bool
instr_is_spill(instr_t *inst);

bool
instr_is_restore(instr_t *inst);

#endif /* _FASTPATH_H_ */
