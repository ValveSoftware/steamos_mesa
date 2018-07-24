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
#include "nir_deref.h"

struct split_var_state {
   void *mem_ctx;

   nir_shader *shader;
   nir_function_impl *impl;

   nir_variable *base_var;
};

struct field {
   struct field *parent;

   const struct glsl_type *type;

   unsigned num_fields;
   struct field *fields;

   nir_variable *var;
};

static const struct glsl_type *
wrap_type_in_array(const struct glsl_type *type,
                   const struct glsl_type *array_type)
{
   if (!glsl_type_is_array(array_type))
      return type;

   const struct glsl_type *elem_type =
      wrap_type_in_array(type, glsl_get_array_element(array_type));
   return glsl_array_type(elem_type, glsl_get_length(array_type));
}

static void
init_field_for_type(struct field *field, struct field *parent,
                    const struct glsl_type *type,
                    const char *name,
                    struct split_var_state *state)
{
   *field = (struct field) {
      .parent = parent,
      .type = type,
   };

   const struct glsl_type *struct_type = glsl_without_array(type);
   if (glsl_type_is_struct(struct_type)) {
      field->num_fields = glsl_get_length(struct_type),
      field->fields = ralloc_array(state->mem_ctx, struct field,
                                   field->num_fields);
      for (unsigned i = 0; i < field->num_fields; i++) {
         char *field_name = NULL;
         if (name) {
            field_name = ralloc_asprintf(state->mem_ctx, "%s_%s", name,
                                         glsl_get_struct_elem_name(struct_type, i));
         } else {
            field_name = ralloc_asprintf(state->mem_ctx, "{unnamed %s}_%s",
                                         glsl_get_type_name(struct_type),
                                         glsl_get_struct_elem_name(struct_type, i));
         }
         init_field_for_type(&field->fields[i], field,
                             glsl_get_struct_field(struct_type, i),
                             field_name, state);
      }
   } else {
      const struct glsl_type *var_type = type;
      for (struct field *f = field->parent; f; f = f->parent)
         var_type = wrap_type_in_array(var_type, f->type);

      nir_variable_mode mode = state->base_var->data.mode;
      if (mode == nir_var_local) {
         field->var = nir_local_variable_create(state->impl, var_type, name);
      } else {
         field->var = nir_variable_create(state->shader, mode, var_type, name);
      }
   }
}

static bool
split_var_list_structs(nir_shader *shader,
                       nir_function_impl *impl,
                       struct exec_list *vars,
                       struct hash_table *var_field_map,
                       void *mem_ctx)
{
   struct split_var_state state = {
      .mem_ctx = mem_ctx,
      .shader = shader,
      .impl = impl,
   };

   struct exec_list split_vars;
   exec_list_make_empty(&split_vars);

   /* To avoid list confusion (we'll be adding things as we split variables),
    * pull all of the variables we plan to split off of the list
    */
   nir_foreach_variable_safe(var, vars) {
      if (!glsl_type_is_struct(glsl_without_array(var->type)))
         continue;

      exec_node_remove(&var->node);
      exec_list_push_tail(&split_vars, &var->node);
   }

   nir_foreach_variable(var, &split_vars) {
      state.base_var = var;

      struct field *root_field = ralloc(mem_ctx, struct field);
      init_field_for_type(root_field, NULL, var->type, var->name, &state);
      _mesa_hash_table_insert(var_field_map, var, root_field);
   }

   return !exec_list_is_empty(&split_vars);
}

static void
split_struct_derefs_impl(nir_function_impl *impl,
                         struct hash_table *var_field_map,
                         nir_variable_mode modes,
                         void *mem_ctx)
{
   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_deref)
            continue;

         nir_deref_instr *deref = nir_instr_as_deref(instr);
         if (!(deref->mode & modes))
            continue;

         /* Clean up any dead derefs we find lying around.  They may refer to
          * variables we're planning to split.
          */
         if (nir_deref_instr_remove_if_unused(deref))
            continue;

         if (!glsl_type_is_vector_or_scalar(deref->type))
            continue;

         nir_variable *base_var = nir_deref_instr_get_variable(deref);
         struct hash_entry *entry =
            _mesa_hash_table_search(var_field_map, base_var);
         if (!entry)
            continue;

         struct field *root_field = entry->data;

         nir_deref_path path;
         nir_deref_path_init(&path, deref, mem_ctx);

         struct field *tail_field = root_field;
         for (unsigned i = 0; path.path[i]; i++) {
            if (path.path[i]->deref_type != nir_deref_type_struct)
               continue;

            assert(i > 0);
            assert(glsl_type_is_struct(path.path[i - 1]->type));
            assert(path.path[i - 1]->type ==
                   glsl_without_array(tail_field->type));

            tail_field = &tail_field->fields[path.path[i]->strct.index];
         }
         nir_variable *split_var = tail_field->var;

         nir_deref_instr *new_deref = NULL;
         for (unsigned i = 0; path.path[i]; i++) {
            nir_deref_instr *p = path.path[i];
            b.cursor = nir_after_instr(&p->instr);

            switch (p->deref_type) {
            case nir_deref_type_var:
               assert(new_deref == NULL);
               new_deref = nir_build_deref_var(&b, split_var);
               break;

            case nir_deref_type_array:
            case nir_deref_type_array_wildcard:
               new_deref = nir_build_deref_follower(&b, new_deref, p);
               break;

            case nir_deref_type_struct:
               /* Nothing to do; we're splitting structs */
               break;

            default:
               unreachable("Invalid deref type in path");
            }
         }

         assert(new_deref->type == deref->type);
         nir_ssa_def_rewrite_uses(&deref->dest.ssa,
                                  nir_src_for_ssa(&new_deref->dest.ssa));
         nir_deref_instr_remove_if_unused(deref);
      }
   }
}

/** A pass for splitting structs into multiple variables
 *
 * This pass splits arrays of structs into multiple variables, one for each
 * (possibly nested) structure member.  After this pass completes, no
 * variables of the given mode will contain a struct type.
 */
bool
nir_split_struct_vars(nir_shader *shader, nir_variable_mode modes)
{
   void *mem_ctx = ralloc_context(NULL);
   struct hash_table *var_field_map =
      _mesa_hash_table_create(mem_ctx, _mesa_hash_pointer,
                              _mesa_key_pointer_equal);

   assert((modes & (nir_var_global | nir_var_local)) == modes);

   bool has_global_splits = false;
   if (modes & nir_var_global) {
      has_global_splits = split_var_list_structs(shader, NULL,
                                                 &shader->globals,
                                                 var_field_map, mem_ctx);
   }

   bool progress = false;
   nir_foreach_function(function, shader) {
      if (!function->impl)
         continue;

      bool has_local_splits = false;
      if (modes & nir_var_local) {
         has_local_splits = split_var_list_structs(shader, function->impl,
                                                   &function->impl->locals,
                                                   var_field_map, mem_ctx);
      }

      if (has_global_splits || has_local_splits) {
         split_struct_derefs_impl(function->impl, var_field_map,
                                  modes, mem_ctx);

         nir_metadata_preserve(function->impl, nir_metadata_block_index |
                                               nir_metadata_dominance);
         progress = true;
      }
   }

   ralloc_free(mem_ctx);

   return progress;
}
