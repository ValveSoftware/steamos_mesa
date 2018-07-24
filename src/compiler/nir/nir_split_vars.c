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
#include "nir_vla.h"

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

static int
num_array_levels_in_array_of_vector_type(const struct glsl_type *type)
{
   int num_levels = 0;
   while (true) {
      if (glsl_type_is_array_or_matrix(type)) {
         num_levels++;
         type = glsl_get_array_element(type);
      } else if (glsl_type_is_vector_or_scalar(type)) {
         return num_levels;
      } else {
         /* Not an array of vectors */
         return -1;
      }
   }
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

struct array_level_info {
   unsigned array_len;
   bool split;
};

struct array_split {
   /* Only set if this is the tail end of the splitting */
   nir_variable *var;

   unsigned num_splits;
   struct array_split *splits;
};

struct array_var_info {
   nir_variable *base_var;

   const struct glsl_type *split_var_type;

   bool split_var;
   struct array_split root_split;

   unsigned num_levels;
   struct array_level_info levels[0];
};

static bool
init_var_list_array_infos(struct exec_list *vars,
                          struct hash_table *var_info_map,
                          void *mem_ctx)
{
   bool has_array = false;

   nir_foreach_variable(var, vars) {
      int num_levels = num_array_levels_in_array_of_vector_type(var->type);
      if (num_levels <= 0)
         continue;

      struct array_var_info *info =
         rzalloc_size(mem_ctx, sizeof(*info) +
                               num_levels * sizeof(info->levels[0]));

      info->base_var = var;
      info->num_levels = num_levels;

      const struct glsl_type *type = var->type;
      for (int i = 0; i < num_levels; i++) {
         info->levels[i].array_len = glsl_get_length(type);
         type = glsl_get_array_element(type);

         /* All levels start out initially as split */
         info->levels[i].split = true;
      }

      _mesa_hash_table_insert(var_info_map, var, info);
      has_array = true;
   }

   return has_array;
}

static struct array_var_info *
get_array_var_info(nir_variable *var,
                   struct hash_table *var_info_map)
{
   struct hash_entry *entry =
      _mesa_hash_table_search(var_info_map, var);
   return entry ? entry->data : NULL;
}

static struct array_var_info *
get_array_deref_info(nir_deref_instr *deref,
                     struct hash_table *var_info_map,
                     nir_variable_mode modes)
{
   if (!(deref->mode & modes))
      return NULL;

   return get_array_var_info(nir_deref_instr_get_variable(deref),
                             var_info_map);
}

static void
mark_array_deref_used(nir_deref_instr *deref,
                      struct hash_table *var_info_map,
                      nir_variable_mode modes,
                      void *mem_ctx)
{
   struct array_var_info *info =
      get_array_deref_info(deref, var_info_map, modes);
   if (!info)
      return;

   nir_deref_path path;
   nir_deref_path_init(&path, deref, mem_ctx);

   /* Walk the path and look for indirects.  If we have an array deref with an
    * indirect, mark the given level as not being split.
    */
   for (unsigned i = 0; i < info->num_levels; i++) {
      nir_deref_instr *p = path.path[i + 1];
      if (p->deref_type == nir_deref_type_array &&
          nir_src_as_const_value(p->arr.index) == NULL)
         info->levels[i].split = false;
   }
}

static void
mark_array_usage_impl(nir_function_impl *impl,
                      struct hash_table *var_info_map,
                      nir_variable_mode modes,
                      void *mem_ctx)
{
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         switch (intrin->intrinsic) {
         case nir_intrinsic_copy_deref:
            mark_array_deref_used(nir_src_as_deref(intrin->src[1]),
                                  var_info_map, modes, mem_ctx);
            /* Fall Through */

         case nir_intrinsic_load_deref:
         case nir_intrinsic_store_deref:
            mark_array_deref_used(nir_src_as_deref(intrin->src[0]),
                                  var_info_map, modes, mem_ctx);
            break;

         default:
            break;
         }
      }
   }
}

static void
create_split_array_vars(struct array_var_info *var_info,
                        unsigned level,
                        struct array_split *split,
                        const char *name,
                        nir_shader *shader,
                        nir_function_impl *impl,
                        void *mem_ctx)
{
   while (level < var_info->num_levels && !var_info->levels[level].split) {
      name = ralloc_asprintf(mem_ctx, "%s[*]", name);
      level++;
   }

   if (level == var_info->num_levels) {
      /* We add parens to the variable name so it looks like "(foo[2][*])" so
       * that further derefs will look like "(foo[2][*])[ssa_6]"
       */
      name = ralloc_asprintf(mem_ctx, "(%s)", name);

      nir_variable_mode mode = var_info->base_var->data.mode;
      if (mode == nir_var_local) {
         split->var = nir_local_variable_create(impl,
                                                var_info->split_var_type, name);
      } else {
         split->var = nir_variable_create(shader, mode,
                                          var_info->split_var_type, name);
      }
   } else {
      assert(var_info->levels[level].split);
      split->num_splits = var_info->levels[level].array_len;
      split->splits = rzalloc_array(mem_ctx, struct array_split,
                                    split->num_splits);
      for (unsigned i = 0; i < split->num_splits; i++) {
         create_split_array_vars(var_info, level + 1, &split->splits[i],
                                 ralloc_asprintf(mem_ctx, "%s[%d]", name, i),
                                 shader, impl, mem_ctx);
      }
   }
}

static bool
split_var_list_arrays(nir_shader *shader,
                      nir_function_impl *impl,
                      struct exec_list *vars,
                      struct hash_table *var_info_map,
                      void *mem_ctx)
{
   struct exec_list split_vars;
   exec_list_make_empty(&split_vars);

   nir_foreach_variable_safe(var, vars) {
      struct array_var_info *info = get_array_var_info(var, var_info_map);
      if (!info)
         continue;

      bool has_split = false;
      const struct glsl_type *split_type =
         glsl_without_array_or_matrix(var->type);
      for (int i = info->num_levels - 1; i >= 0; i--) {
         if (info->levels[i].split) {
            has_split = true;
            continue;
         }

         /* If the original type was a matrix type, we'd like to keep that so
          * we don't convert matrices into arrays.
          */
         if (i == info->num_levels - 1 &&
             glsl_type_is_matrix(glsl_without_array(var->type))) {
            split_type = glsl_matrix_type(glsl_get_base_type(split_type),
                                          glsl_get_components(split_type),
                                          info->levels[i].array_len);
         } else {
            split_type = glsl_array_type(split_type, info->levels[i].array_len);
         }
      }

      if (has_split) {
         info->split_var_type = split_type;
         /* To avoid list confusion (we'll be adding things as we split
          * variables), pull all of the variables we plan to split off of the
          * main variable list.
          */
         exec_node_remove(&var->node);
         exec_list_push_tail(&split_vars, &var->node);
      } else {
         assert(split_type == var->type);
         /* If we're not modifying this variable, delete the info so we skip
          * it faster in later passes.
          */
         _mesa_hash_table_remove_key(var_info_map, var);
      }
   }

   nir_foreach_variable(var, &split_vars) {
      struct array_var_info *info = get_array_var_info(var, var_info_map);
      create_split_array_vars(info, 0, &info->root_split, var->name,
                              shader, impl, mem_ctx);
   }

   return !exec_list_is_empty(&split_vars);
}

static bool
deref_has_split_wildcard(nir_deref_path *path,
                         struct array_var_info *info)
{
   if (info == NULL)
      return false;

   assert(path->path[0]->var == info->base_var);
   for (unsigned i = 0; i < info->num_levels; i++) {
      if (path->path[i + 1]->deref_type == nir_deref_type_array_wildcard &&
          info->levels[i].split)
         return true;
   }

   return false;
}

static bool
array_path_is_out_of_bounds(nir_deref_path *path,
                            struct array_var_info *info)
{
   if (info == NULL)
      return false;

   assert(path->path[0]->var == info->base_var);
   for (unsigned i = 0; i < info->num_levels; i++) {
      nir_deref_instr *p = path->path[i + 1];
      if (p->deref_type == nir_deref_type_array_wildcard)
         continue;

      nir_const_value *const_index = nir_src_as_const_value(p->arr.index);
      if (const_index && const_index->u32[0] >= info->levels[i].array_len)
         return true;
   }

   return false;
}

static void
emit_split_copies(nir_builder *b,
                  struct array_var_info *dst_info, nir_deref_path *dst_path,
                  unsigned dst_level, nir_deref_instr *dst,
                  struct array_var_info *src_info, nir_deref_path *src_path,
                  unsigned src_level, nir_deref_instr *src)
{
   nir_deref_instr *dst_p, *src_p;

   while ((dst_p = dst_path->path[dst_level + 1])) {
      if (dst_p->deref_type == nir_deref_type_array_wildcard)
         break;

      dst = nir_build_deref_follower(b, dst, dst_p);
      dst_level++;
   }

   while ((src_p = src_path->path[src_level + 1])) {
      if (src_p->deref_type == nir_deref_type_array_wildcard)
         break;

      src = nir_build_deref_follower(b, src, src_p);
      src_level++;
   }

   if (src_p == NULL || dst_p == NULL) {
      assert(src_p == NULL && dst_p == NULL);
      nir_copy_deref(b, dst, src);
   } else {
      assert(dst_p->deref_type == nir_deref_type_array_wildcard &&
             src_p->deref_type == nir_deref_type_array_wildcard);

      if ((dst_info && dst_info->levels[dst_level].split) ||
          (src_info && src_info->levels[src_level].split)) {
         /* There are no indirects at this level on one of the source or the
          * destination so we are lowering it.
          */
         assert(glsl_get_length(dst_path->path[dst_level]->type) ==
                glsl_get_length(src_path->path[src_level]->type));
         unsigned len = glsl_get_length(dst_path->path[dst_level]->type);
         for (unsigned i = 0; i < len; i++) {
            nir_ssa_def *idx = nir_imm_int(b, i);
            emit_split_copies(b, dst_info, dst_path, dst_level + 1,
                              nir_build_deref_array(b, dst, idx),
                              src_info, src_path, src_level + 1,
                              nir_build_deref_array(b, src, idx));
         }
      } else {
         /* Neither side is being split so we just keep going */
         emit_split_copies(b, dst_info, dst_path, dst_level + 1,
                           nir_build_deref_array_wildcard(b, dst),
                           src_info, src_path, src_level + 1,
                           nir_build_deref_array_wildcard(b, src));
      }
   }
}

static void
split_array_copies_impl(nir_function_impl *impl,
                        struct hash_table *var_info_map,
                        nir_variable_mode modes,
                        void *mem_ctx)
{
   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *copy = nir_instr_as_intrinsic(instr);
         if (copy->intrinsic != nir_intrinsic_copy_deref)
            continue;

         nir_deref_instr *dst_deref = nir_src_as_deref(copy->src[0]);
         nir_deref_instr *src_deref = nir_src_as_deref(copy->src[1]);

         struct array_var_info *dst_info =
            get_array_deref_info(dst_deref, var_info_map, modes);
         struct array_var_info *src_info =
            get_array_deref_info(src_deref, var_info_map, modes);

         if (!src_info && !dst_info)
            continue;

         nir_deref_path dst_path, src_path;
         nir_deref_path_init(&dst_path, dst_deref, mem_ctx);
         nir_deref_path_init(&src_path, src_deref, mem_ctx);

         if (!deref_has_split_wildcard(&dst_path, dst_info) &&
             !deref_has_split_wildcard(&src_path, src_info))
            continue;

         b.cursor = nir_instr_remove(&copy->instr);

         emit_split_copies(&b, dst_info, &dst_path, 0, dst_path.path[0],
                               src_info, &src_path, 0, src_path.path[0]);
      }
   }
}

static void
split_array_access_impl(nir_function_impl *impl,
                        struct hash_table *var_info_map,
                        nir_variable_mode modes,
                        void *mem_ctx)
{
   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type == nir_instr_type_deref) {
            /* Clean up any dead derefs we find lying around.  They may refer
             * to variables we're planning to split.
             */
            nir_deref_instr *deref = nir_instr_as_deref(instr);
            if (deref->mode & modes)
               nir_deref_instr_remove_if_unused(deref);
            continue;
         }

         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_load_deref &&
             intrin->intrinsic != nir_intrinsic_store_deref &&
             intrin->intrinsic != nir_intrinsic_copy_deref)
            continue;

         const unsigned num_derefs =
            intrin->intrinsic == nir_intrinsic_copy_deref ? 2 : 1;

         for (unsigned d = 0; d < num_derefs; d++) {
            nir_deref_instr *deref = nir_src_as_deref(intrin->src[d]);

            struct array_var_info *info =
               get_array_deref_info(deref, var_info_map, modes);
            if (!info)
               continue;

            nir_deref_path path;
            nir_deref_path_init(&path, deref, mem_ctx);

            b.cursor = nir_before_instr(&intrin->instr);

            if (array_path_is_out_of_bounds(&path, info)) {
               /* If one of the derefs is out-of-bounds, we just delete the
                * instruction.  If a destination is out of bounds, then it may
                * have been in-bounds prior to shrinking so we don't want to
                * accidentally stomp something.  However, we've already proven
                * that it will never be read so it's safe to delete.  If a
                * source is out of bounds then it is loading random garbage.
                * For loads, we replace their uses with an undef instruction
                * and for copies we just delete the copy since it was writing
                * undefined garbage anyway and we may as well leave the random
                * garbage in the destination alone.
                */
               if (intrin->intrinsic == nir_intrinsic_load_deref) {
                  nir_ssa_def *u =
                     nir_ssa_undef(&b, intrin->dest.ssa.num_components,
                                       intrin->dest.ssa.bit_size);
                  nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                                           nir_src_for_ssa(u));
               }
               nir_instr_remove(&intrin->instr);
               for (unsigned i = 0; i < num_derefs; i++)
                  nir_deref_instr_remove_if_unused(nir_src_as_deref(intrin->src[i]));
               break;
            }

            struct array_split *split = &info->root_split;
            for (unsigned i = 0; i < info->num_levels; i++) {
               if (info->levels[i].split) {
                  nir_deref_instr *p = path.path[i + 1];
                  unsigned index = nir_src_as_const_value(p->arr.index)->u32[0];
                  assert(index < info->levels[i].array_len);
                  split = &split->splits[index];
               }
            }
            assert(!split->splits && split->var);

            nir_deref_instr *new_deref = nir_build_deref_var(&b, split->var);
            for (unsigned i = 0; i < info->num_levels; i++) {
               if (!info->levels[i].split) {
                  new_deref = nir_build_deref_follower(&b, new_deref,
                                                       path.path[i + 1]);
               }
            }
            assert(new_deref->type == deref->type);

            /* Rewrite the deref source to point to the split one */
            nir_instr_rewrite_src(&intrin->instr, &intrin->src[d],
                                  nir_src_for_ssa(&new_deref->dest.ssa));
            nir_deref_instr_remove_if_unused(deref);
         }
      }
   }
}

/** A pass for splitting arrays of vectors into multiple variables
 *
 * This pass looks at arrays (possibly multiple levels) of vectors (not
 * structures or other types) and tries to split them into piles of variables,
 * one for each array element.  The heuristic used is simple: If a given array
 * level is never used with an indirect, that array level will get split.
 *
 * This pass probably could handles structures easily enough but making a pass
 * that could see through an array of structures of arrays would be difficult
 * so it's best to just run nir_split_struct_vars first.
 */
bool
nir_split_array_vars(nir_shader *shader, nir_variable_mode modes)
{
   void *mem_ctx = ralloc_context(NULL);
   struct hash_table *var_info_map =
      _mesa_hash_table_create(mem_ctx, _mesa_hash_pointer,
                              _mesa_key_pointer_equal);

   assert((modes & (nir_var_global | nir_var_local)) == modes);

   bool has_global_array = false;
   if (modes & nir_var_global) {
      has_global_array = init_var_list_array_infos(&shader->globals,
                                                   var_info_map, mem_ctx);
   }

   bool has_any_array = false;
   nir_foreach_function(function, shader) {
      if (!function->impl)
         continue;

      bool has_local_array = false;
      if (modes & nir_var_local) {
         has_local_array = init_var_list_array_infos(&function->impl->locals,
                                                     var_info_map, mem_ctx);
      }

      if (has_global_array || has_local_array) {
         has_any_array = true;
         mark_array_usage_impl(function->impl, var_info_map, modes, mem_ctx);
      }
   }

   /* If we failed to find any arrays of arrays, bail early. */
   if (!has_any_array) {
      ralloc_free(mem_ctx);
      return false;
   }

   bool has_global_splits = false;
   if (modes & nir_var_global) {
      has_global_splits = split_var_list_arrays(shader, NULL,
                                                &shader->globals,
                                                var_info_map, mem_ctx);
   }

   bool progress = false;
   nir_foreach_function(function, shader) {
      if (!function->impl)
         continue;

      bool has_local_splits = false;
      if (modes & nir_var_local) {
         has_local_splits = split_var_list_arrays(shader, function->impl,
                                                  &function->impl->locals,
                                                  var_info_map, mem_ctx);
      }

      if (has_global_splits || has_local_splits) {
         split_array_copies_impl(function->impl, var_info_map, modes, mem_ctx);
         split_array_access_impl(function->impl, var_info_map, modes, mem_ctx);

         nir_metadata_preserve(function->impl, nir_metadata_block_index |
                                               nir_metadata_dominance);
         progress = true;
      }
   }

   ralloc_free(mem_ctx);

   return progress;
}
