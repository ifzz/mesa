/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jason Ekstrand (jason@jlekstrand.net)
 *
 */

#include "nir.h"

/*
 * Implements a simple pass that lowers vecN instructions to a series of
 * moves with partial writes.
 */

static bool
src_matches_dest_reg(nir_dest *dest, nir_src *src)
{
   if (dest->is_ssa || src->is_ssa)
      return false;

   return (dest->reg.reg == src->reg.reg &&
           dest->reg.base_offset == src->reg.base_offset &&
           !dest->reg.indirect &&
           !src->reg.indirect);
}

/**
 * For a given starting writemask channel and corresponding source index in
 * the vec instruction, insert a MOV to the vec instruction's dest of all the
 * writemask channels that get read from the same src reg.
 *
 * Returns the writemask of our MOV, so the parent loop calling this knows
 * which ones have been processed.
 */
static unsigned
insert_mov(nir_alu_instr *vec, unsigned start_channel,
            unsigned start_src_idx, void *mem_ctx)
{
   unsigned src_idx = start_src_idx;
   assert(src_idx < nir_op_infos[vec->op].num_inputs);

   nir_alu_instr *mov = nir_alu_instr_create(mem_ctx, nir_op_imov);
   nir_alu_src_copy(&mov->src[0], &vec->src[src_idx], mem_ctx);
   nir_alu_dest_copy(&mov->dest, &vec->dest, mem_ctx);

   mov->dest.write_mask = (1u << start_channel);
   mov->src[0].swizzle[start_channel] = vec->src[src_idx].swizzle[0];
   src_idx++;

   for (unsigned i = start_channel + 1; i < 4; i++) {
      if (!(vec->dest.write_mask & (1 << i)))
         continue;

      if (nir_srcs_equal(vec->src[src_idx].src, vec->src[start_src_idx].src)) {
         mov->dest.write_mask |= (1 << i);
         mov->src[0].swizzle[i] = vec->src[src_idx].swizzle[0];
      }
      src_idx++;
   }

   nir_instr_insert_before(&vec->instr, &mov->instr);

   return mov->dest.write_mask;
}

/**
 * Clone an ALU instruction and override the destination with the one given by
 * new_dest. It copies sources from original ALU to the new one, adjusting
 * their swizzles.
 *
 * Returns the new ALU instruction.
 */
static nir_alu_instr *
clone_alu_instr_and_override_dest(nir_alu_instr *alu_instr,
                                  nir_alu_dest *new_dest, unsigned index,
                                  void *mem_ctx)
{
   nir_alu_instr *new_alu_instr = nir_alu_instr_create(mem_ctx, alu_instr->op);

   /* Determine which dest channel was used in the parent ALU instruction */
   unsigned channel;
   for (unsigned i = 0; i < 4; i++) {
      if (alu_instr->dest.write_mask & (1 << i)) {
         channel = i;
         break;
      }
   }
   // assert(alu_instr->dest.write_mask == (1 << channel));

   for (unsigned i = 0; i < nir_op_infos[alu_instr->op].num_inputs; i++) {
      nir_alu_src_copy(&new_alu_instr->src[i], &alu_instr->src[i], mem_ctx);

      switch (alu_instr->op) {
      case nir_op_fdot2:
      case nir_op_fdot3:
      case nir_op_fdot4:
         continue;
      default:
         break;
      }

      new_alu_instr->src[i].swizzle[index] =
         alu_instr->src[i].swizzle[channel];
   }

   nir_alu_dest_copy(&new_alu_instr->dest, new_dest, mem_ctx);
   new_alu_instr->dest.write_mask = 1 << index;

   return new_alu_instr;
}

static bool
lower_vec_to_movs_block(nir_block *block, void *mem_ctx)
{
   nir_foreach_instr_safe(block, instr) {
      if (instr->type != nir_instr_type_alu)
         continue;

      nir_alu_instr *vec = nir_instr_as_alu(instr);

      switch (vec->op) {
      case nir_op_vec2:
      case nir_op_vec3:
      case nir_op_vec4:
         break;
      default:
         continue; /* The loop */
      }

      /* Since we insert multiple MOVs, we have to be non-SSA. */
      assert(!vec->dest.dest.is_ssa);

      unsigned finished_write_mask = 0;

      for (unsigned i = 0; i < 4; i++) {
         if (!(vec->dest.write_mask & (1 << i)))
            continue;

         /* We don't propagate constants by now
          * @FIXME: we could also consider propagating destination of
          * load_const instructions.
          */
         if (vec->src[i].src.is_ssa)
            continue;

         nir_register *reg = vec->src[i].src.reg.reg;

         nir_foreach_def_safe(reg, src) {
            nir_instr *parent_instr = src->reg.parent_instr;

            /* We only coalesce registers written by ALU instructions, by now.
             * @FIXME: consider other type of instructions, like intrinsics, etc.
             */
            if (parent_instr->type != nir_instr_type_alu)
               continue;

            nir_alu_instr *parent_alu_instr = nir_instr_as_alu(parent_instr);
            nir_register *parent_dest_reg = parent_alu_instr->dest.dest.reg.reg;

            /* We only override dest registers that are only used in this vecX
             * instruction.
             * @FIXME: In the future we might consider registers used more than
             * once as sources of the same vecX instruction.
             */
            if (list_length(&parent_dest_reg->uses) != 1)
               continue;

            /* @FIXME: by now I want to have fine-grained control over what ALU
             * ops are safe to propagate. The switch below can be used to
             * enable/disable this pass for the different ALU opcodes.
             * I have detected that propagating imov ops doesn't work, but still
             * have not analyzed why.
             */
            switch (parent_alu_instr->op) {
            case nir_op_imov:
               continue;
            default:
               break;
            }

            nir_alu_instr *new_alu_instr =
               clone_alu_instr_and_override_dest(parent_alu_instr, &vec->dest,
                                                 i, mem_ctx);
            finished_write_mask |= new_alu_instr->dest.write_mask;

            /* Remove the old ALU instruction */
            nir_instr_remove(&parent_alu_instr->instr);
            ralloc_free(parent_alu_instr);

            /* Remove the intermediate register, if not used anymore */
            if (list_length(&parent_dest_reg->defs) == 0)
               nir_reg_remove(parent_dest_reg);

            /* Insert the new instruction with the overwritten destination */
            nir_instr_insert_before(&vec->instr, &new_alu_instr->instr);
         }
      }

      /* First, emit a MOV for all the src channels that are in the
       * destination reg, in case other values we're populating in the dest
       * might overwrite them.
       */
      for (unsigned i = 0, src_idx = 0; i < 4; i++) {
         if (!(vec->dest.write_mask & (1 << i)))
            continue;

         if (!(finished_write_mask & (1 << i)))
            continue;

         if (src_matches_dest_reg(&vec->dest.dest, &vec->src[src_idx].src)) {
            finished_write_mask |= insert_mov(vec, i, src_idx, mem_ctx);
            break;
         }
         src_idx++;
      }

      /* Now, emit MOVs for all the other src channels. */
      for (unsigned i = 0, src_idx = 0; i < 4; i++) {
         if (!(vec->dest.write_mask & (1 << i)))
            continue;

         if (!(finished_write_mask & (1 << i)))
            finished_write_mask |= insert_mov(vec, i, src_idx, mem_ctx);

         src_idx++;
      }

      nir_instr_remove(&vec->instr);
      ralloc_free(vec);
   }

   return true;
}

static void
nir_lower_vec_to_movs_impl(nir_function_impl *impl)
{
   nir_foreach_block(impl, lower_vec_to_movs_block, ralloc_parent(impl));
}

void
nir_lower_vec_to_movs(nir_shader *shader)
{
   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         nir_lower_vec_to_movs_impl(overload->impl);
   }
}
