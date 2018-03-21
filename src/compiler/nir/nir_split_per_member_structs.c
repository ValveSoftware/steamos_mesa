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
#include "nir_deref.h"

struct split_struct_state {
   void *dead_ctx;

   struct hash_table *var_to_member_map;
};

static nir_variable *
find_var_member(struct nir_variable *var, unsigned member,
                struct hash_table *var_to_member_map)
{
   struct hash_entry *map_entry =
      _mesa_hash_table_search(var_to_member_map, var);
   if (map_entry == NULL)
      return NULL;

   nir_variable **members = map_entry->data;
   assert(member < var->num_members);
   return members[member];
}

static const struct glsl_type *
member_type(const struct glsl_type *type, unsigned index)
{
   if (glsl_type_is_array(type)) {
      const struct glsl_type *elem =
         member_type(glsl_get_array_element(type), index);
      return glsl_get_array_instance(elem, glsl_get_length(type));
   } else {
      assert(glsl_type_is_struct(type));
      assert(index < glsl_get_length(type));
      return glsl_get_struct_field(type, index);
   }
}

static void
split_variable(struct nir_variable *var, nir_shader *shader,
               struct hash_table *var_to_member_map, void *dead_ctx)
{
   assert(var->state_slots == NULL);

   /* Constant initializers are currently not handled */
   assert(var->constant_initializer == NULL);

   nir_variable **members =
      ralloc_array(dead_ctx, nir_variable *, var->num_members);

   for (unsigned i = 0; i < var->num_members; i++) {
      char *member_name = NULL;
      if (var->name) {
         /* Calculate a reasonable variable name */
         member_name = ralloc_strdup(dead_ctx, var->name);
         const struct glsl_type *t = var->type;
         while (glsl_type_is_array(t)) {
            ralloc_strcat(&member_name, "[*]");
            t = glsl_get_array_element(t);
         }
         const char *field_name = glsl_get_struct_elem_name(t, i);
         if (field_name) {
            member_name = ralloc_asprintf(dead_ctx, "%s.%s",
                                          member_name, field_name);
         } else {
            member_name = ralloc_asprintf(dead_ctx, "%s.@%d", member_name, i);
         }
      }

      members[i] =
         nir_variable_create(shader, var->members[i].mode,
                             member_type(var->type, i), member_name);
      if (var->interface_type) {
         members[i]->interface_type =
            glsl_get_struct_field(var->interface_type, i);
      }
      members[i]->data = var->members[i];
   }

   _mesa_hash_table_insert(var_to_member_map, var, members);
}

static bool
split_variables_in_list(struct exec_list *var_list, nir_shader *shader,
                        struct hash_table *var_to_member_map, void *dead_ctx)
{
   bool progress = false;

   nir_foreach_variable_safe(var, var_list) {
      if (var->num_members == 0)
         continue;

      split_variable(var, shader, var_to_member_map, dead_ctx);
      exec_node_remove(&var->node);
      progress = true;
   }

   return progress;
}

static nir_deref_instr *
build_member_deref(nir_builder *b, nir_deref_instr *deref, nir_variable *member)
{
   if (deref->deref_type == nir_deref_type_var) {
      return nir_build_deref_var(b, member);
   } else {
      nir_deref_instr *parent =
         build_member_deref(b, nir_deref_instr_parent(deref), member);
      return nir_build_deref_follower(b, parent, deref);
   }
}

static void
rewrite_deref_instr(nir_builder *b, nir_deref_instr *deref,
                    struct hash_table *var_to_member_map)
{
   /* We must be a struct deref */
   if (deref->deref_type != nir_deref_type_struct)
      return;

   nir_deref_instr *base;
   for (base = nir_deref_instr_parent(deref);
        base && base->deref_type != nir_deref_type_var;
        base = nir_deref_instr_parent(base)) {

      /* If this struct is nested inside another, bail */
      if (base->deref_type == nir_deref_type_struct)
         return;
   }

   /* We must be on a variable with members */
   if (!base || base->var->num_members == 0)
      return;

   nir_variable *member = find_var_member(base->var, deref->strct.index,
                                          var_to_member_map);
   assert(member);

   b->cursor = nir_before_instr(&deref->instr);
   nir_deref_instr *member_deref =
      build_member_deref(b, nir_deref_instr_parent(deref), member);
   nir_ssa_def_rewrite_uses(&deref->dest.ssa,
                            nir_src_for_ssa(&member_deref->dest.ssa));

   /* The referenced variable is no longer valid, clean up the deref */
   nir_deref_instr_remove_if_unused(deref);
}

static void
rewrite_deref_var(nir_instr *instr, nir_deref_var **deref,
                 struct hash_table *var_to_member_map)
{
   if ((*deref)->var->members == 0)
      return;

   nir_deref_struct *strct = NULL;
   for (nir_deref *d = (*deref)->deref.child; d; d = d->child) {
      if (d->deref_type == nir_deref_type_struct) {
         strct = nir_deref_as_struct(d);
         break;
      }
   }
   assert(strct);

   nir_variable *member = find_var_member((*deref)->var, strct->index,
                                          var_to_member_map);

   nir_deref_var *head = nir_deref_var_create(ralloc_parent(*deref), member);
   nir_deref *tail = &head->deref;
   for (nir_deref *d = (*deref)->deref.child;
        d != &strct->deref; d = d->child) {
      nir_deref_array *arr = nir_deref_as_array(d);

      nir_deref_array *narr = nir_deref_array_create(tail);
      narr->deref.type = glsl_get_array_element(tail->type);
      narr->deref_array_type = arr->deref_array_type;
      narr->base_offset = arr->base_offset;

      if (arr->deref_array_type == nir_deref_array_type_indirect)
         nir_instr_move_src(instr, &narr->indirect, &arr->indirect);

      assert(tail->child == NULL);
      tail->child = &narr->deref;
      tail = &narr->deref;
   }

   ralloc_steal(tail, strct->deref.child);
   tail->child = strct->deref.child;

   ralloc_free(*deref);
   *deref = head;
}

static void
rewrite_intrinsic_instr(nir_intrinsic_instr *intrin,
                        struct hash_table *var_to_member_map)
{
   for (unsigned i = 0;
        i < nir_intrinsic_infos[intrin->intrinsic].num_variables; i++) {
      rewrite_deref_var(&intrin->instr, &intrin->variables[i],
                        var_to_member_map);
   }
}

static void
rewrite_tex_instr(nir_tex_instr *tex, struct hash_table *var_to_member_map)
{
   if (tex->texture)
      rewrite_deref_var(&tex->instr, &tex->texture, var_to_member_map);
   if (tex->sampler)
      rewrite_deref_var(&tex->instr, &tex->sampler, var_to_member_map);
}

bool
nir_split_per_member_structs(nir_shader *shader)
{
   bool progress = false;
   void *dead_ctx = ralloc_context(NULL);
   struct hash_table *var_to_member_map =
      _mesa_hash_table_create(dead_ctx, _mesa_hash_pointer,
                              _mesa_key_pointer_equal);

   progress |= split_variables_in_list(&shader->inputs, shader,
                                       var_to_member_map, dead_ctx);
   progress |= split_variables_in_list(&shader->outputs, shader,
                                       var_to_member_map, dead_ctx);
   progress |= split_variables_in_list(&shader->system_values, shader,
                                       var_to_member_map, dead_ctx);
   if (!progress)
      return false;

   nir_foreach_function(function, shader) {
      if (!function->impl)
         continue;

      nir_builder b;
      nir_builder_init(&b, function->impl);
      nir_foreach_block(block, function->impl) {
         nir_foreach_instr_safe(instr, block) {
            switch (instr->type) {
            case nir_instr_type_deref:
               rewrite_deref_instr(&b, nir_instr_as_deref(instr),
                                   var_to_member_map);
               break;

            case nir_instr_type_intrinsic:
               rewrite_intrinsic_instr(nir_instr_as_intrinsic(instr),
                                       var_to_member_map);
               break;

            case nir_instr_type_tex:
               rewrite_tex_instr(nir_instr_as_tex(instr), var_to_member_map);
               break;

            case nir_instr_type_call:
               unreachable("Functions must be inlined before this pass");

            default:
               break;
            }
         }
      }
   }

   ralloc_free(dead_ctx);

   return progress;
}
