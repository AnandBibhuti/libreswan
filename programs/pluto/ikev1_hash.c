/* IKEv1 HASH payload wierdness, for Libreswan
 *
 * Copyright (C) 2019  Andrew Cagney
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include "ikev1_hash.h"

#include "state.h"
#include "crypt_prf.h"
#include "ike_alg.h"
#include "lswlog.h"
#include "demux.h"
#include "impair.h"

bool emit_v1_HASH(enum v1_hash_type hash_type, const char *what,
		  enum exchange_impairment exchange,
		  struct state *st, struct v1_hash_fixup *fixup,
		  pb_stream *rbody)
{
	zero(fixup);
	fixup->what = what;
	fixup->hash_type = hash_type;
	fixup->impair = (impair_v1_hash_exchange == exchange
			 ? impair_v1_hash_payload : SEND_NORMAL);
	if (fixup->impair == SEND_OMIT) {
		libreswan_log("IMPAIR: omitting HASH payload for %s", what);
		return true;
	}
	pb_stream hash_pbs;
	if (!ikev1_out_generic(0, &isakmp_hash_desc, rbody, &hash_pbs)) {
		return false;
	}
	if (fixup->impair == SEND_EMPTY) {
		libreswan_log("IMPAIR: sending HASH payload with no data for %s", what);
	} else {
		/* reserve space for HASH data */
		fixup->hash_data = chunk(hash_pbs.cur, st->st_oakley.ta_prf->prf_output_size);
		if (!out_zero(fixup->hash_data.len, &hash_pbs, "HASH DATA"))
			return false;
	}
	close_output_pbs(&hash_pbs);
	/* save start of rest of message for later */
	fixup->body = rbody->cur;
	return true;
}

void fixup_v1_HASH(struct state *st, const struct v1_hash_fixup *fixup,
		   msgid_t msgid, const uint8_t *roof)
{
	if (fixup->impair >= SEND_ROOF) {
		libreswan_log("IMPAIR: setting HASH payload bytes to %02x",
			      fixup->impair - SEND_ROOF);
		/* chunk_fill()? */
		memset(fixup->hash_data.ptr, fixup->impair - SEND_ROOF,
		       fixup->hash_data.len);
		return;
	} else if (fixup->impair != SEND_NORMAL) {
		/* already logged above? */
		return;
	}
	struct crypt_prf *hash =
		crypt_prf_init_symkey("HASH(1)", st->st_oakley.ta_prf,
				      "SKEYID_a", st->st_skeyid_a_nss);
	/* msgid */
	passert(sizeof(msgid_t) == sizeof(uint32_t));
	msgid_t raw_msgid = htonl(msgid);
	switch (fixup->hash_type) {
	case V1_HASH_1:
		/* HASH(1) = prf(SKEYID_a, M-ID | payload ) */
		crypt_prf_update_bytes(hash, "M-ID", &raw_msgid, sizeof(raw_msgid));
		crypt_prf_update_bytes(hash, "payload",
				       fixup->body, roof - fixup->body);
		break;
	case V1_HASH_2:
		/* HASH(2) = prf(SKEYID_a, M-ID | Ni_b | payload ) */
		crypt_prf_update_bytes(hash, "M-ID", &raw_msgid, sizeof(raw_msgid));
		crypt_prf_update_chunk(hash, "Ni_b", st->st_ni);
		crypt_prf_update_bytes(hash, "payload",
				       fixup->body, roof - fixup->body);
		break;
	case V1_HASH_3:
		/* HASH(3) = prf(SKEYID_a, 0 | M-ID | Ni_b | Nr_b) */
		crypt_prf_update_byte(hash, "0", 0);
		crypt_prf_update_bytes(hash, "M-ID", &raw_msgid, sizeof(raw_msgid));
		crypt_prf_update_chunk(hash, "Ni_b", st->st_ni);
		crypt_prf_update_chunk(hash, "Nr_b", st->st_nr);
		break;
	default:
		bad_case(fixup->hash_type);
	}
	/* stuff result into hash_data */
	passert(fixup->hash_data.len == st->st_oakley.ta_prf->prf_output_size);
	crypt_prf_final_bytes(&hash, fixup->hash_data.ptr, fixup->hash_data.len);
	if (DBGP(DBG_BASE)) {
		DBG_log("%s HASH(%u):", fixup->what, fixup->hash_type);
		DBG_dump_chunk(NULL, fixup->hash_data);
	}
}