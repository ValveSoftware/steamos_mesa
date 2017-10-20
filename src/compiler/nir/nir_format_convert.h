/*
 * Copyright © 2017 Intel Corporation
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

#include "nir_builder.h"

static inline nir_ssa_def *
nir_shift(nir_builder *b, nir_ssa_def *value, int left_shift)
{
   if (left_shift > 0)
      return nir_ishl(b, value, nir_imm_int(b, left_shift));
   else if (left_shift < 0)
      return nir_ushr(b, value, nir_imm_int(b, -left_shift));
   else
      return value;
}

static inline nir_ssa_def *
nir_mask_shift(struct nir_builder *b, nir_ssa_def *src,
               uint32_t mask, int left_shift)
{
   return nir_shift(b, nir_iand(b, src, nir_imm_int(b, mask)), left_shift);
}

static inline nir_ssa_def *
nir_mask_shift_or(struct nir_builder *b, nir_ssa_def *dst, nir_ssa_def *src,
                  uint32_t src_mask, int src_left_shift)
{
   return nir_ior(b, nir_mask_shift(b, src, src_mask, src_left_shift), dst);
}

static inline nir_ssa_def *
nir_format_unpack_uint(nir_builder *b, nir_ssa_def *packed,
                       const unsigned *bits, unsigned num_components)
{
   assert(num_components >= 1 && num_components <= 4);
   nir_ssa_def *comps[4];

   if (bits[0] >= packed->bit_size) {
      assert(bits[0] == packed->bit_size);
      assert(num_components == 1);
      return packed;
   }

   unsigned offset = 0;
   for (unsigned i = 0; i < num_components; i++) {
      assert(bits[i] < 32);
      nir_ssa_def *mask = nir_imm_int(b, (1u << bits[i]) - 1);
      comps[i] = nir_iand(b, nir_shift(b, packed, -offset), mask);
      offset += bits[i];
   }
   assert(offset <= packed->bit_size);

   return nir_vec(b, comps, num_components);
}

static inline nir_ssa_def *
nir_format_pack_uint_unmasked(nir_builder *b, nir_ssa_def *color,
                              const unsigned *bits, unsigned num_components)
{
   assert(num_components >= 1 && num_components <= 4);
   nir_ssa_def *packed = nir_imm_int(b, 0);
   unsigned offset = 0;
   for (unsigned i = 0; i < num_components; i++) {
      packed = nir_ior(b, packed, nir_shift(b, nir_channel(b, color, i),
                                               offset));
      offset += bits[i];
   }
   assert(offset <= packed->bit_size);

   return packed;
}

static inline nir_ssa_def *
nir_format_pack_uint(nir_builder *b, nir_ssa_def *color,
                     const unsigned *bits, unsigned num_components)
{
   nir_const_value mask;
   for (unsigned i = 0; i < num_components; i++) {
      assert(bits[i] < 32);
      mask.u32[i] = (1u << bits[i]) - 1;
   }
   nir_ssa_def *mask_imm = nir_build_imm(b, num_components, 32, mask);

   return nir_format_pack_uint_unmasked(b, nir_iand(b, color, mask_imm),
                                        bits, num_components);
}
