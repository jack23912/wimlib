/*
 * lzms-common.c
 *
 * Code shared between the compressor and decompressor for the LZMS compression
 * format.
 */

/*
 * Copyright (C) 2013 Eric Biggers
 *
 * This file is part of wimlib, a library for working with WIM files.
 *
 * wimlib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * wimlib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with wimlib; if not, see http://www.gnu.org/licenses/.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wimlib/endianness.h"
#include "wimlib/lzms.h"
#include "wimlib/util.h"

#include <pthread.h>

/***************************************************************
 * Constant tables initialized by lzms_compute_slots():        *
 ***************************************************************/

/* Table: position slot => position slot base value  */
u32 lzms_position_slot_base[LZMS_MAX_NUM_OFFSET_SYMS + 1];

/* Table: position slot => number of extra position bits  */
u8 lzms_extra_position_bits[LZMS_MAX_NUM_OFFSET_SYMS];

/* Table: log2(position) => [lower bound, upper bound] on position slot  */
u16 lzms_order_to_position_slot_bounds[30][2];

/* Table: length slot => length slot base value  */
u32 lzms_length_slot_base[LZMS_NUM_LEN_SYMS + 1];

/* Table: length slot => number of extra length bits  */
u8 lzms_extra_length_bits[LZMS_NUM_LEN_SYMS];

/* Table: length (< LZMS_NUM_FAST_LENGTHS only) => length slot  */
u8 lzms_length_slot_fast[LZMS_NUM_FAST_LENGTHS];

u32
lzms_get_slot(u32 value, const u32 slot_base_tab[], unsigned num_slots)
{
	u32 l = 0;
	u32 r = num_slots - 1;
	for (;;) {
		LZMS_ASSERT(r >= l);
		u32 slot = (l + r) / 2;
		if (value >= slot_base_tab[slot]) {
			if (value < slot_base_tab[slot + 1])
				return slot;
			else
				l = slot + 1;
		} else {
			r = slot - 1;
		}
	}
}

static void
lzms_decode_delta_rle_slot_bases(u32 slot_bases[],
				 u8 extra_bits[],
				 const u8 delta_run_lens[],
				 u32 num_run_lens,
				 u32 final,
				 u32 expected_num_slots)
{
	u32 order = 0;
	u32 delta = 1;
	u32 base = 0;
	u32 slot = 0;
	for (u32 i = 0; i < num_run_lens; i++) {
		u8 run_len = delta_run_lens[i];
		while (run_len--) {
			base += delta;
			if (slot > 0)
				extra_bits[slot - 1] = order;
			slot_bases[slot] = base;
			slot++;
		}
		delta <<= 1;
		order++;
	}
	LZMS_ASSERT(slot == expected_num_slots);

	slot_bases[slot] = final;
	extra_bits[slot - 1] = bsr32(slot_bases[slot] - slot_bases[slot - 1]);
}

/* Initialize the global position and length slot tables.  */
static void
lzms_compute_slots(void)
{
	/* If an explicit formula that maps LZMS position and length slots to
	 * slot bases exists, then it could be used here.  But until one is
	 * found, the following code fills in the slots using the observation
	 * that the increase from one slot base to the next is an increasing
	 * power of 2.  Therefore, run-length encoding of the delta of adjacent
	 * entries can be used.  */
	static const u8 position_slot_delta_run_lens[] = {
		9,   0,   9,   7,   10,  15,  15,  20,
		20,  30,  33,  40,  42,  45,  60,  73,
		80,  85,  95,  105, 6,
	};

	static const u8 length_slot_delta_run_lens[] = {
		27,  4,   6,   4,   5,   2,   1,   1,
		1,   1,   1,   0,   0,   0,   0,   0,
		1,
	};

	/* Position slots  */
	lzms_decode_delta_rle_slot_bases(lzms_position_slot_base,
					 lzms_extra_position_bits,
					 position_slot_delta_run_lens,
					 ARRAY_LEN(position_slot_delta_run_lens),
					 0x7fffffff,
					 LZMS_MAX_NUM_OFFSET_SYMS);

	for (u32 order = 0; order < 30; order++) {
		lzms_order_to_position_slot_bounds[order][0] =
			lzms_get_slot(1U << order, lzms_position_slot_base,
				      LZMS_MAX_NUM_OFFSET_SYMS);
		lzms_order_to_position_slot_bounds[order][1] =
			lzms_get_slot((1U << (order + 1)) - 1, lzms_position_slot_base,
				      LZMS_MAX_NUM_OFFSET_SYMS);
	}

	/* Length slots  */
	lzms_decode_delta_rle_slot_bases(lzms_length_slot_base,
					 lzms_extra_length_bits,
					 length_slot_delta_run_lens,
					 ARRAY_LEN(length_slot_delta_run_lens),
					 0x400108ab,
					 LZMS_NUM_LEN_SYMS);

	/* Create table mapping short lengths to length slots.  */
	for (u32 slot = 0, i = 0; i < LZMS_NUM_FAST_LENGTHS; i++) {
		if (i >= lzms_length_slot_base[slot + 1])
			slot++;
		lzms_length_slot_fast[i] = slot;
	}
}

/* Initialize the global position and length slot tables if not done so already.
 * */
void
lzms_init_slots(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;

	pthread_once(&once, lzms_compute_slots);
}

static s32
lzms_maybe_do_x86_translation(u8 data[restrict], s32 i, s32 num_op_bytes,
			      s32 * restrict closest_target_usage_p,
			      s32 last_target_usages[restrict],
			      s32 max_trans_offset, bool undo)
{
	u16 pos;

	if (undo) {
		if (i - *closest_target_usage_p <= max_trans_offset) {
			LZMS_DEBUG("Undid x86 translation at position %d "
				   "(opcode 0x%02x)", i, data[i]);
			le32 *p32 = (le32*)&data[i + num_op_bytes];
			u32 n = le32_to_cpu(*p32);
			*p32 = cpu_to_le32(n - i);
		}
		pos = i + le16_to_cpu(*(const le16*)&data[i + num_op_bytes]);
	} else {
		pos = i + le16_to_cpu(*(const le16*)&data[i + num_op_bytes]);

		if (i - *closest_target_usage_p <= max_trans_offset) {
			LZMS_DEBUG("Did x86 translation at position %d "
				   "(opcode 0x%02x)", i, data[i]);
			le32 *p32 = (le32*)&data[i + num_op_bytes];
			u32 n = le32_to_cpu(*p32);
			*p32 = cpu_to_le32(n + i);
		}
	}

	i += num_op_bytes + sizeof(le32) - 1;

	if (i - last_target_usages[pos] <= LZMS_X86_MAX_GOOD_TARGET_OFFSET)
		*closest_target_usage_p = i;

	last_target_usages[pos] = i;

	return i + 1;
}

static inline s32
lzms_may_x86_translate(const u8 p[restrict], s32 *restrict max_offset_ret)
{
	/* Switch on first byte of the opcode, assuming it is really an x86
	 * instruction.  */
	*max_offset_ret = LZMS_X86_MAX_TRANSLATION_OFFSET;
	switch (p[0]) {
	case 0x48:
		if (p[1] == 0x8b) {
			if (p[2] == 0x5 || p[2] == 0xd) {
				/* Load relative (x86_64)  */
				return 3;
			}
		} else if (p[1] == 0x8d) {
			if ((p[2] & 0x7) == 0x5) {
				/* Load effective address relative (x86_64)  */
				return 3;
			}
		}
		break;

	case 0x4c:
		if (p[1] == 0x8d) {
			if ((p[2] & 0x7) == 0x5) {
				/* Load effective address relative (x86_64)  */
				return 3;
			}
		}
		break;

	case 0xe8:
		/* Call relative  */
		*max_offset_ret = LZMS_X86_MAX_TRANSLATION_OFFSET / 2;
		return 1;

	case 0xe9:
		/* Jump relative  */
		*max_offset_ret = 0;
		return 5;

	case 0xf0:
		if (p[1] == 0x83 && p[2] == 0x05) {
			/* Lock add relative  */
			return 3;
		}
		break;

	case 0xff:
		if (p[1] == 0x15) {
			/* Call indirect  */
			return 2;
		}
		break;
	}
	*max_offset_ret = 0;
	return 1;
}

/*
 * Translate relative addresses embedded in x86 instructions into absolute
 * addresses (@undo == %false), or undo this translation (@undo == %true).
 *
 * Absolute addresses are usually more compressible by LZ factorization.
 *
 * @last_target_usages must be a temporary array of length >= 65536.
 */
void
lzms_x86_filter(u8 data[restrict], s32 size,
		s32 last_target_usages[restrict], bool undo)
{
	/*
	 * Note: this filter runs unconditionally and uses a custom algorithm to
	 * detect data regions that probably contain x86 code.
	 *
	 * 'closest_target_usage' tracks the most recent position that has a
	 * good chance of being an x86 instruction.  When the filter detects a
	 * likely x86 instruction, it updates this variable and considers the
	 * next 1023 bytes of data as valid for x86 translations.
	 *
	 * If part of the data does not, in fact, contain x86 machine code, then
	 * 'closest_target_usage' will, very likely, eventually fall more than
	 * 1023 bytes behind the current position.  This results in x86
	 * translations being disabled until the next likely x86 instruction is
	 * detected.
	 *
	 * Translations on relative call (e8 opcode) instructions are slightly
	 * more restricted.  They require that the most recent likely x86
	 * instruction was in the last 511 bytes, rather than the last 1023
	 * bytes.
	 *
	 * To identify "likely x86 instructions", the algorithm attempts to
	 * track the position of the most recent potential relative-addressing
	 * instruction that referenced each possible memory address.  If it
	 * finds two references to the same memory address within a 65535 byte
	 * window, the second reference is flagged as a likely x86 instruction.
	 * Since the instructions considered for translation necessarily use
	 * relative addressing, the algorithm does a tentative translation into
	 * absolute addresses.  In addition, so that memory addresses can be
	 * looked up in an array of reasonable size (in this code,
	 * 'last_target_usages'), only the low-order 2 bytes of each address are
	 * considered significant.
	 */

	s32 closest_target_usage = -LZMS_X86_MAX_TRANSLATION_OFFSET - 1;

	for (s32 i = 0; i < 65536; i++)
		last_target_usages[i] = -LZMS_X86_MAX_GOOD_TARGET_OFFSET - 1;

	for (s32 i = 1; i < size - 16; ) {
		s32 max_trans_offset;
		s32 n;

		n = lzms_may_x86_translate(data + i, &max_trans_offset);

		if (max_trans_offset) {
			/* Recognized opcode.  */
			i = lzms_maybe_do_x86_translation(data, i, n,
							  &closest_target_usage,
							  last_target_usages,
							  max_trans_offset,
							  undo);
		} else {
			/* Not a recognized opcode.  */
			i += n;
		}
	}
}

static void
lzms_init_lz_lru_queues(struct lzms_lz_lru_queues *lz)
{
	/* Recent offsets for LZ matches  */
	for (u32 i = 0; i < LZMS_NUM_RECENT_OFFSETS + 1; i++)
		lz->recent_offsets[i] = i + 1;

	lz->prev_offset = 0;
	lz->upcoming_offset = 0;
}

static void
lzms_init_delta_lru_queues(struct lzms_delta_lru_queues *delta)
{
	/* Recent offsets and powers for LZ matches  */
	for (u32 i = 0; i < LZMS_NUM_RECENT_OFFSETS + 1; i++) {
		delta->recent_offsets[i] = i + 1;
		delta->recent_powers[i] = 0;
	}
	delta->prev_offset = 0;
	delta->prev_power = 0;
	delta->upcoming_offset = 0;
	delta->upcoming_power = 0;
}


void
lzms_init_lru_queues(struct lzms_lru_queues *lru)
{
	lzms_init_lz_lru_queues(&lru->lz);
	lzms_init_delta_lru_queues(&lru->delta);
}

void
lzms_update_lz_lru_queues(struct lzms_lz_lru_queues *lz)
{
	if (lz->prev_offset != 0) {
		for (int i = LZMS_NUM_RECENT_OFFSETS - 1; i >= 0; i--)
			lz->recent_offsets[i + 1] = lz->recent_offsets[i];
		lz->recent_offsets[0] = lz->prev_offset;
	}
	lz->prev_offset = lz->upcoming_offset;
}

void
lzms_update_delta_lru_queues(struct lzms_delta_lru_queues *delta)
{
	if (delta->prev_offset != 0) {
		for (int i = LZMS_NUM_RECENT_OFFSETS - 1; i >= 0; i--) {
			delta->recent_offsets[i + 1] = delta->recent_offsets[i];
			delta->recent_powers[i + 1] = delta->recent_powers[i];
		}
		delta->recent_offsets[0] = delta->prev_offset;
		delta->recent_powers[0] = delta->prev_power;
	}

	delta->prev_offset = delta->upcoming_offset;
	delta->prev_power = delta->upcoming_power;
}

void
lzms_update_lru_queues(struct lzms_lru_queues *lru)
{
	lzms_update_lz_lru_queues(&lru->lz);
	lzms_update_delta_lru_queues(&lru->delta);
}
