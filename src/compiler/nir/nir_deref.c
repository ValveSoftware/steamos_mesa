/*
 * Copyright © 2018 Intel Corporation
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
 */

#include "nir.h"
#include "nir_builder.h"

/**
 * Recursively removes unused deref instructions
 */
bool
nir_deref_instr_remove_if_unused(nir_deref_instr *instr)
{
   bool progress = false;

   for (nir_deref_instr *d = instr; d; d = nir_deref_instr_parent(d)) {
      /* If anyone is using this deref, leave it alone */
      assert(d->dest.is_ssa);
      if (!list_empty(&d->dest.ssa.uses))
         break;

      nir_instr_remove(&d->instr);
      progress = true;
   }

   return progress;
}

bool
nir_remove_dead_derefs_impl(nir_function_impl *impl)
{
   bool progress = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type == nir_instr_type_deref &&
             nir_deref_instr_remove_if_unused(nir_instr_as_deref(instr)))
            progress = true;
      }
   }

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);

   return progress;
}

bool
nir_remove_dead_derefs(nir_shader *shader)
{
   bool progress = false;
   nir_foreach_function(function, shader) {
      if (function->impl && nir_remove_dead_derefs_impl(function->impl))
         progress = true;
   }

   return progress;
}

nir_deref_var *
nir_deref_instr_to_deref(nir_deref_instr *instr, void *mem_ctx)
{
   nir_deref *deref = NULL;

   while (instr->deref_type != nir_deref_type_var) {
      nir_deref *nderef;
      switch (instr->deref_type) {
      case nir_deref_type_array:
      case nir_deref_type_array_wildcard: {
         nir_deref_array *deref_arr = nir_deref_array_create(mem_ctx);
         if (instr->deref_type == nir_deref_type_array) {
            nir_const_value *const_index =
               nir_src_as_const_value(instr->arr.index);
            if (const_index) {
               deref_arr->deref_array_type = nir_deref_array_type_direct;
               deref_arr->base_offset = const_index->u32[0];
            } else {
               deref_arr->deref_array_type = nir_deref_array_type_indirect;
               deref_arr->base_offset = 0;
               nir_src_copy(&deref_arr->indirect, &instr->arr.index, mem_ctx);
            }
         } else {
            deref_arr->deref_array_type = nir_deref_array_type_wildcard;
         }
         nderef = &deref_arr->deref;
         break;
      }

      case nir_deref_type_struct:
         nderef = &nir_deref_struct_create(mem_ctx, instr->strct.index)->deref;
         break;

      default:
         unreachable("Invalid deref instruction type");
      }

      nderef->child = deref;
      ralloc_steal(nderef, deref);
      nderef->type = instr->type;

      deref = nderef;
      assert(instr->parent.is_ssa);
      instr = nir_src_as_deref(instr->parent);
   }

   assert(instr->deref_type == nir_deref_type_var);
   nir_deref_var *deref_var = nir_deref_var_create(mem_ctx, instr->var);
   deref_var->deref.child = deref;
   ralloc_steal(deref_var, deref);

   return deref_var;
}

static nir_deref_var *
nir_deref_src_to_deref(nir_src src, void *mem_ctx)
{
   return nir_deref_instr_to_deref(nir_src_as_deref(src), mem_ctx);
}

static bool
nir_lower_deref_instrs_tex(nir_tex_instr *tex)
{
   bool progress = false;

   /* Remove the instruction before we modify it.  This way we won't mess up
    * use-def chains when we move sources around.
    */
   nir_cursor cursor = nir_instr_remove(&tex->instr);

   unsigned new_num_srcs = 0;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      if (tex->src[i].src_type == nir_tex_src_texture_deref) {
         tex->texture = nir_deref_src_to_deref(tex->src[i].src, tex);
         progress = true;
         continue;
      } else if (tex->src[i].src_type == nir_tex_src_sampler_deref) {
         tex->sampler = nir_deref_src_to_deref(tex->src[i].src, tex);
         progress = true;
         continue;
      }

      /* Compact the sources down to remove the deref sources */
      assert(new_num_srcs <= i);
      tex->src[new_num_srcs++] = tex->src[i];
   }
   tex->num_srcs = new_num_srcs;

   nir_instr_insert(cursor, &tex->instr);

   return progress;
}

static bool
nir_lower_deref_instrs_intrin(nir_intrinsic_instr *intrin,
                              enum nir_lower_deref_flags flags)
{
   nir_intrinsic_op deref_op = intrin->intrinsic;
   nir_intrinsic_op var_op;

   switch (deref_op) {
#define CASE(a) \
   case nir_intrinsic_##a##_deref: \
      if (!(flags & nir_lower_load_store_derefs)) \
         return false; \
      var_op = nir_intrinsic_##a##_var; \
      break;
   CASE(load)
   CASE(store)
   CASE(copy)
#undef CASE

#define CASE(a) \
   case nir_intrinsic_interp_deref_##a: \
      if (!(flags & nir_lower_interp_derefs)) \
         return false; \
      var_op = nir_intrinsic_interp_var_##a; \
      break;
   CASE(at_centroid)
   CASE(at_sample)
   CASE(at_offset)
#undef CASE

#define CASE(a) \
   case nir_intrinsic_atomic_counter_##a##_deref: \
      if (!(flags & nir_lower_atomic_counter_derefs)) \
         return false; \
      var_op = nir_intrinsic_atomic_counter_##a##_var; \
      break;
   CASE(inc)
   CASE(dec)
   CASE(read)
   CASE(add)
   CASE(min)
   CASE(max)
   CASE(and)
   CASE(or)
   CASE(xor)
   CASE(exchange)
   CASE(comp_swap)
#undef CASE

#define CASE(a) \
   case nir_intrinsic_deref_atomic_##a: \
      if (!(flags & nir_lower_atomic_derefs)) \
         return false; \
      var_op = nir_intrinsic_var_atomic_##a; \
      break;
   CASE(add)
   CASE(imin)
   CASE(umin)
   CASE(imax)
   CASE(umax)
   CASE(and)
   CASE(or)
   CASE(xor)
   CASE(exchange)
   CASE(comp_swap)
#undef CASE

#define CASE(a) \
   case nir_intrinsic_image_deref_##a: \
      if (!(flags & nir_lower_image_derefs)) \
         return false; \
      var_op = nir_intrinsic_image_var_##a; \
      break;
   CASE(load)
   CASE(store)
   CASE(atomic_add)
   CASE(atomic_min)
   CASE(atomic_max)
   CASE(atomic_and)
   CASE(atomic_or)
   CASE(atomic_xor)
   CASE(atomic_exchange)
   CASE(atomic_comp_swap)
   CASE(size)
   CASE(samples)
#undef CASE

   default:
      return false;
   }

   /* Remove the instruction before we modify it.  This way we won't mess up
    * use-def chains when we move sources around.
    */
   nir_cursor cursor = nir_instr_remove(&intrin->instr);

   unsigned num_derefs = nir_intrinsic_infos[var_op].num_variables;
   assert(nir_intrinsic_infos[var_op].num_srcs + num_derefs ==
          nir_intrinsic_infos[deref_op].num_srcs);

   /* Move deref sources to variables */
   for (unsigned i = 0; i < num_derefs; i++)
      intrin->variables[i] = nir_deref_src_to_deref(intrin->src[i], intrin);

   /* Shift all the other sources down */
   for (unsigned i = 0; i < nir_intrinsic_infos[var_op].num_srcs; i++)
      nir_src_copy(&intrin->src[i], &intrin->src[i + num_derefs], intrin);

   /* Rewrite the extra sources to NIR_SRC_INIT just in case */
   for (unsigned i = 0; i < num_derefs; i++)
      intrin->src[nir_intrinsic_infos[var_op].num_srcs + i] = NIR_SRC_INIT;

   /* It's safe to just stomp the intrinsic to var intrinsic since every
    * intrinsic has room for some variables and the number of sources only
    * shrinks.
    */
   intrin->intrinsic = var_op;

   nir_instr_insert(cursor, &intrin->instr);

   return true;
}

static bool
nir_lower_deref_instrs_impl(nir_function_impl *impl,
                            enum nir_lower_deref_flags flags)
{
   bool progress = false;

   /* Walk the instructions in reverse order so that we can safely clean up
    * the deref instructions after we clean up their uses.
    */
   nir_foreach_block_reverse(block, impl) {
      nir_foreach_instr_reverse_safe(instr, block) {
         switch (instr->type) {
         case nir_instr_type_deref:
            if (list_empty(&nir_instr_as_deref(instr)->dest.ssa.uses)) {
               nir_instr_remove(instr);
               progress = true;
            }
            break;

         case nir_instr_type_tex:
            if (flags & nir_lower_texture_derefs)
               progress |= nir_lower_deref_instrs_tex(nir_instr_as_tex(instr));
            break;

         case nir_instr_type_intrinsic:
            progress |=
               nir_lower_deref_instrs_intrin(nir_instr_as_intrinsic(instr),
                                             flags);
            break;

         default:
            break; /* Nothing to do */
         }
      }
   }

   if (progress) {
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);
   }

   return progress;
}

bool
nir_lower_deref_instrs(nir_shader *shader,
                       enum nir_lower_deref_flags flags)
{
   bool progress = false;

   nir_foreach_function(function, shader) {
      if (!function->impl)
         continue;

      progress |= nir_lower_deref_instrs_impl(function->impl, flags);
   }

   return progress;
}
