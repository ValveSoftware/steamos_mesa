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
#include "nir_builder.h"
#include "nir_deref.h"
#include "compiler/nir_types.h"

/*
 * Lowers all copy intrinsics to sequences of load/store intrinsics.
 */

/* Walks down the deref chain and returns the next deref in the chain whose
 * child is a wildcard.  In other words, given the chain  a[1].foo[*].bar,
 * this function will return the deref to foo.  Calling it a second time
 * with the [*].bar, it will return NULL.
 */
static nir_deref *
deref_next_wildcard_parent(nir_deref *deref)
{
   for (nir_deref *tail = deref; tail->child; tail = tail->child) {
      if (tail->child->deref_type != nir_deref_type_array)
         continue;

      nir_deref_array *arr = nir_deref_as_array(tail->child);

      if (arr->deref_array_type == nir_deref_array_type_wildcard)
         return tail;
   }

   return NULL;
}

/* This function recursively walks the given deref chain and replaces the
 * given copy instruction with an equivalent sequence load/store
 * operations.
 *
 * @copy_instr    The copy instruction to replace; new instructions will be
 *                inserted before this one
 *
 * @dest_head     The head of the destination variable deref chain
 *
 * @src_head      The head of the source variable deref chain
 *
 * @dest_tail     The current tail of the destination variable deref chain;
 *                this is used for recursion and external callers of this
 *                function should call it with tail == head
 *
 * @src_tail      The current tail of the source variable deref chain;
 *                this is used for recursion and external callers of this
 *                function should call it with tail == head
 *
 * @state         The current variable lowering state
 */
static void
emit_copy_load_store(nir_intrinsic_instr *copy_instr,
                     nir_deref_var *dest_head, nir_deref_var *src_head,
                     nir_deref *dest_tail, nir_deref *src_tail,
                     nir_shader *shader)
{
   /* Find the next pair of wildcards */
   nir_deref *src_arr_parent = deref_next_wildcard_parent(src_tail);
   nir_deref *dest_arr_parent = deref_next_wildcard_parent(dest_tail);

   if (src_arr_parent || dest_arr_parent) {
      /* Wildcards had better come in matched pairs */
      assert(src_arr_parent && dest_arr_parent);

      nir_deref_array *src_arr = nir_deref_as_array(src_arr_parent->child);
      nir_deref_array *dest_arr = nir_deref_as_array(dest_arr_parent->child);

      unsigned length = glsl_get_length(src_arr_parent->type);
      /* The wildcards should represent the same number of elements */
      assert(length == glsl_get_length(dest_arr_parent->type));
      assert(length > 0);

      /* Walk over all of the elements that this wildcard refers to and
       * call emit_copy_load_store on each one of them */
      src_arr->deref_array_type = nir_deref_array_type_direct;
      dest_arr->deref_array_type = nir_deref_array_type_direct;
      for (unsigned i = 0; i < length; i++) {
         src_arr->base_offset = i;
         dest_arr->base_offset = i;
         emit_copy_load_store(copy_instr, dest_head, src_head,
                              &dest_arr->deref, &src_arr->deref, shader);
      }
      src_arr->deref_array_type = nir_deref_array_type_wildcard;
      dest_arr->deref_array_type = nir_deref_array_type_wildcard;
   } else {
      /* In this case, we have no wildcards anymore, so all we have to do
       * is just emit the load and store operations. */
      src_tail = nir_deref_tail(src_tail);
      dest_tail = nir_deref_tail(dest_tail);

      assert(src_tail->type == dest_tail->type);

      unsigned num_components = glsl_get_vector_elements(src_tail->type);
      unsigned bit_size = glsl_get_bit_size(src_tail->type);

      nir_intrinsic_instr *load =
         nir_intrinsic_instr_create(shader, nir_intrinsic_load_var);
      load->num_components = num_components;
      load->variables[0] = nir_deref_var_clone(src_head, load);
      nir_ssa_dest_init(&load->instr, &load->dest, num_components, bit_size,
                        NULL);

      nir_instr_insert_before(&copy_instr->instr, &load->instr);

      nir_intrinsic_instr *store =
         nir_intrinsic_instr_create(shader, nir_intrinsic_store_var);
      store->num_components = num_components;
      nir_intrinsic_set_write_mask(store, (1 << num_components) - 1);
      store->variables[0] = nir_deref_var_clone(dest_head, store);

      store->src[0].is_ssa = true;
      store->src[0].ssa = &load->dest.ssa;

      nir_instr_insert_before(&copy_instr->instr, &store->instr);
   }
}

/* Lowers a copy instruction to a sequence of load/store instructions
 *
 * The new instructions are placed before the copy instruction in the IR.
 */
void
nir_lower_var_copy_instr(nir_intrinsic_instr *copy, nir_shader *shader)
{
   assert(copy->intrinsic == nir_intrinsic_copy_var);
   emit_copy_load_store(copy, copy->variables[0], copy->variables[1],
                        &copy->variables[0]->deref,
                        &copy->variables[1]->deref, shader);
}

static nir_deref_instr *
build_deref_to_next_wildcard(nir_builder *b,
                             nir_deref_instr *parent,
                             nir_deref_instr ***deref_arr)
{
   for (; **deref_arr; (*deref_arr)++) {
      if ((**deref_arr)->deref_type == nir_deref_type_array_wildcard)
         return parent;

      parent = nir_build_deref_follower(b, parent, **deref_arr);
   }

   assert(**deref_arr == NULL);
   *deref_arr = NULL;
   return parent;
}

static void
emit_deref_copy_load_store(nir_builder *b,
                           nir_deref_instr *dst_deref,
                           nir_deref_instr **dst_deref_arr,
                           nir_deref_instr *src_deref,
                           nir_deref_instr **src_deref_arr)
{
   if (dst_deref_arr || src_deref_arr) {
      assert(dst_deref_arr && src_deref_arr);
      dst_deref = build_deref_to_next_wildcard(b, dst_deref, &dst_deref_arr);
      src_deref = build_deref_to_next_wildcard(b, src_deref, &src_deref_arr);
   }

   if (dst_deref_arr || src_deref_arr) {
      assert(dst_deref_arr && src_deref_arr);
      assert((*dst_deref_arr)->deref_type == nir_deref_type_array_wildcard);
      assert((*src_deref_arr)->deref_type == nir_deref_type_array_wildcard);

      unsigned length = glsl_get_length(src_deref->type);
      /* The wildcards should represent the same number of elements */
      assert(length == glsl_get_length(dst_deref->type));
      assert(length > 0);

      for (unsigned i = 0; i < length; i++) {
         nir_ssa_def *index = nir_imm_int(b, i);
         emit_deref_copy_load_store(b,
                                    nir_build_deref_array(b, dst_deref, index),
                                    dst_deref_arr + 1,
                                    nir_build_deref_array(b, src_deref, index),
                                    src_deref_arr + 1);
      }
   } else {
      assert(dst_deref->type == src_deref->type);
      assert(glsl_type_is_vector_or_scalar(dst_deref->type));

      nir_store_deref(b, dst_deref, nir_load_deref(b, src_deref), ~0);
   }
}

void
nir_lower_deref_copy_instr(nir_builder *b, nir_intrinsic_instr *copy)
{
   /* Unfortunately, there's just no good way to handle wildcards except to
    * flip the chain around and walk the list from variable to final pointer.
    */
   assert(copy->src[0].is_ssa && copy->src[1].is_ssa);
   nir_deref_instr *dst = nir_instr_as_deref(copy->src[0].ssa->parent_instr);
   nir_deref_instr *src = nir_instr_as_deref(copy->src[1].ssa->parent_instr);

   nir_deref_path dst_path, src_path;
   nir_deref_path_init(&dst_path, dst, NULL);
   nir_deref_path_init(&src_path, src, NULL);

   b->cursor = nir_before_instr(&copy->instr);
   emit_deref_copy_load_store(b, dst_path.path[0], &dst_path.path[1],
                                 src_path.path[0], &src_path.path[1]);

   nir_deref_path_finish(&dst_path);
   nir_deref_path_finish(&src_path);
}

static bool
lower_var_copies_impl(nir_function_impl *impl)
{
   nir_shader *shader = impl->function->shader;
   bool progress = false;

   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *copy = nir_instr_as_intrinsic(instr);
         if (copy->intrinsic == nir_intrinsic_copy_var)
            nir_lower_var_copy_instr(copy, shader);
         else if (copy->intrinsic == nir_intrinsic_copy_deref)
            nir_lower_deref_copy_instr(&b, copy);
         else
            continue;

         nir_instr_remove(&copy->instr);
         if (copy->intrinsic == nir_intrinsic_copy_deref) {
            nir_deref_instr_remove_if_unused(nir_src_as_deref(copy->src[0]));
            nir_deref_instr_remove_if_unused(nir_src_as_deref(copy->src[1]));
         }
         progress = true;
         ralloc_free(copy);
      }
   }

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);

   return progress;
}

/* Lowers every copy_var instruction in the program to a sequence of
 * load/store instructions.
 */
bool
nir_lower_var_copies(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function(function, shader) {
      if (function->impl)
         progress |= lower_var_copies_impl(function->impl);
   }

   return progress;
}
