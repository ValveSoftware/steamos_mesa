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

#ifndef MESA_GLSL_TO_TGSI_ARRAY_MERGE_H
#define MESA_GLSL_TO_TGSI_ARRAY_MERGE_H


#include "st_glsl_to_tgsi_private.h"
#include <iosfwd>

/* Until mesa/st officialy requires c++11 */
#if __cplusplus < 201103L
#define nullptr 0
#endif

/* Helper class to merge the live ranges of an arrays.
 *
 * For arrays the array length, live range, and component access needs to
 * be kept, because when live ranges are merged or arrays are interleaved
 * one can only merge or interleave an array into another with equal or more
 * elements. For interleaving it is also required that the sum of used swizzles
 * is at most four.
 */
class array_live_range {
public:
   array_live_range();
   array_live_range(unsigned aid, unsigned alength);
   array_live_range(unsigned aid, unsigned alength, int first_access,
		  int last_access, int mask);

   void set_live_range(int first_access, int last_access);
   void set_begin(int _begin){first_access = _begin;}
   void set_end(int _end){last_access = _end;}
   void set_access_mask(int s);

   static void merge(array_live_range *a, array_live_range *b);
   static void interleave(array_live_range *a, array_live_range *b);

   int array_id() const {return id;}
   int target_array_id() const {return target_array ? target_array->id : 0;}
   const array_live_range *final_target() const {return target_array ?
	       target_array->final_target() : this;}
   unsigned array_length() const { return length;}
   int begin() const { return first_access;}
   int end() const { return last_access;}
   int access_mask() const { return component_access_mask;}
   int used_components() const {return used_component_count;}

   bool time_doesnt_overlap(const array_live_range& other) const;

   void print(std::ostream& os) const;

   bool is_mapped() const { return target_array != nullptr;}

   int8_t remap_one_swizzle(int8_t idx) const;

private:
   void init_swizzles();
   void set_target(array_live_range  *target);
   void merge_live_range_from(array_live_range *other);
   void interleave_into(array_live_range *other);

   unsigned id;
   unsigned length;
   int first_access;
   int last_access;
   uint8_t component_access_mask;
   uint8_t used_component_count;
   array_live_range *target_array;
   int8_t swizzle_map[4];
};

inline
std::ostream& operator << (std::ostream& os, const array_live_range& lt) {
   lt.print(os);
   return os;
}
#endif