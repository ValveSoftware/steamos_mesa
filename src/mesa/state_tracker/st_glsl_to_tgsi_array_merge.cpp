/*
 * Copyright © 2017 Gert Wollny
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "program/prog_instruction.h"
#include "util/u_math.h"
#include <ostream>
#include <cassert>
#include <algorithm>

#include <iostream>

#include "st_glsl_to_tgsi_array_merge.h"

#if __cplusplus >= 201402L
#include <memory>
using std::unique_ptr;
using std::make_unique;
#endif

#define ARRAY_MERGE_DEBUG 0

#if ARRAY_MERGE_DEBUG > 0
#define ARRAY_MERGE_DUMP(x) do std::cerr << x; while (0)
#define ARRAY_MERGE_DUMP_BLOCK(x) do { x } while (0)
#else
#define ARRAY_MERGE_DUMP(x)
#define ARRAY_MERGE_DUMP_BLOCK(x)
#endif

static const char xyzw[] = "xyzw";

array_live_range::array_live_range():
   id(0),
   length(0),
   first_access(0),
   last_access(0),
   component_access_mask(0),
   used_component_count(0),
   target_array(nullptr)
{
   init_swizzles();
}

array_live_range::array_live_range(unsigned aid, unsigned alength):
   id(aid),
   length(alength),
   first_access(0),
   last_access(0),
   component_access_mask(0),
   used_component_count(0),
   target_array(nullptr)
{
   init_swizzles();
}

array_live_range::array_live_range(unsigned aid, unsigned alength, int begin,
				   int end, int sw):
   id(aid),
   length(alength),
   first_access(begin),
   last_access(end),
   component_access_mask(sw),
   used_component_count(util_bitcount(sw)),
   target_array(nullptr)
{
   init_swizzles();
}

void array_live_range::init_swizzles()
{
   for (int i = 0; i < 4; ++i)
      swizzle_map[i] = i;
}

void array_live_range::set_live_range(int _begin, int _end)
{
   set_begin(_begin);
   set_end(_end);
}

void array_live_range::set_access_mask(int mask)
{
   component_access_mask = mask;
   used_component_count = util_bitcount(mask);
}

void array_live_range::merge(array_live_range *a, array_live_range *b)
{
    if (a->array_length() < b->array_length())
       b->merge_live_range_from(a);
    else
       a->merge_live_range_from(b);
}

void array_live_range::interleave(array_live_range *a, array_live_range *b)
{
    if (a->array_length() < b->array_length())
       a->interleave_into(b);
    else
       b->interleave_into(a);
}

void array_live_range::interleave_into(array_live_range *other)
{
   for (int i = 0; i < 4; ++i) {
      swizzle_map[i] = -1;
   }

   int trgt_access_mask = other->access_mask();
   int summary_access_mask = trgt_access_mask;
   int src_swizzle_bit = 1;
   int next_free_swizzle_bit = 1;
   int k = 0;
   unsigned i;
   unsigned last_src_bit = util_last_bit(component_access_mask);

   for (i = 0; i <= last_src_bit ; ++i, src_swizzle_bit <<= 1) {

      /* Jump over empty src component slots (e.g. x__w). This is just a
       * safety measure and it is tested for, but it is very likely that the
       * emitted code always uses slots staring from x without leaving holes
       * (i.e. always xy__ not x_z_ or _yz_ etc).
       */
      if (!(src_swizzle_bit & component_access_mask))
	 continue;

      /* Find the next free access slot in the target. */
      while ((trgt_access_mask & next_free_swizzle_bit) &&
	     k < 4) {
	 next_free_swizzle_bit <<= 1;
	 ++k;
      }
      assert(k < 4 &&
	     "Interleaved array would have more then four components");

      /* Set the mapping for this component. */
      swizzle_map[i] = k;
      trgt_access_mask |= next_free_swizzle_bit;

      /* Update the joined access mask if we didn't just fill the mapping.*/
      if (src_swizzle_bit & component_access_mask)
	 summary_access_mask |= next_free_swizzle_bit;
   }

   other->set_access_mask(summary_access_mask);
   other->merge_live_range_from(this);

   ARRAY_MERGE_DUMP_BLOCK(
	    std::cerr << "Interleave " << id << " into " << other->id << ", swz:";
	    for (unsigned i = 0; i < 4; ++i) {
		std::cerr << ((swizzle_map[i] >= 0) ? xyzw[swizzle_map[i]] : '_');
	    }
	    std::cerr << '\n';
	    );
}

void array_live_range::merge_live_range_from(array_live_range *other)
{
   other->set_target(this);
   if (other->begin() < first_access)
      first_access = other->begin();
   if (other->end() > last_access)
      last_access = other->end();
}

int8_t array_live_range::remap_one_swizzle(int8_t idx) const
{
   // needs testing
   if (target_array) {
      idx = swizzle_map[idx];
      if (idx >=  0)
	 idx = target_array->remap_one_swizzle(idx);
   }
   return idx;
}

void array_live_range::set_target(array_live_range  *target)
{
   target_array = target;
}

void array_live_range::print(std::ostream& os) const
{
   os << "[id:" << id
      << ", length:" << length
      << ", (b:" << first_access
      << ", e:" << last_access
      << "), sw:" << (int)component_access_mask
      << ", nc:" << (int)used_component_count
      << "]";
}

bool array_live_range::time_doesnt_overlap(const array_live_range& other) const
{
   return (other.last_access < first_access ||
	   last_access < other.first_access);
}
