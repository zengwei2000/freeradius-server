/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file protocols/dns/base.c
 * @brief Functions to send/receive dns packets.
 *
 * @copyright 2008 The FreeRADIUS server project
 * @copyright 2021 Network RADIUS SAS (legal@networkradius.com)
 */
RCSID("$Id$")

#include <freeradius-devel/util/base.h>

#include "dns.h"
#include "attrs.h"

static uint32_t instance_count = 0;

typedef struct {
	uint16_t	code;
	uint16_t	length;
} dns_option_t;

fr_dict_t const *dict_dns;

static _Thread_local fr_dns_labels_t *fr_dns_labels;

extern fr_dict_autoload_t dns_dict[];
fr_dict_autoload_t dns_dict[] = {
	{ .out = &dict_dns, .proto = "dns" },
	{ NULL }
};

//fr_dict_attr_t const *attr_dns_packet_type;
fr_dict_attr_t const *attr_dns_packet;
fr_dict_attr_t const *attr_dns_question;
fr_dict_attr_t const *attr_dns_rr;
fr_dict_attr_t const *attr_dns_ns;
fr_dict_attr_t const *attr_dns_ar;

extern fr_dict_attr_autoload_t dns_dict_attr[];
fr_dict_attr_autoload_t dns_dict_attr[] = {
//	{ .out = &attr_dns_packet_type, .name = "Packet-Type", .type = FR_TYPE_UINT16, .dict = &dict_dns },
	{ .out = &attr_dns_packet, .name = "packet", .type = FR_TYPE_STRUCT, .dict = &dict_dns },
	{ .out = &attr_dns_question, .name = "question", .type = FR_TYPE_STRUCT, .dict = &dict_dns },
	{ .out = &attr_dns_rr, .name = "rr", .type = FR_TYPE_STRUCT, .dict = &dict_dns },
	{ .out = &attr_dns_ns, .name = "ns", .type = FR_TYPE_STRUCT, .dict = &dict_dns },
	{ .out = &attr_dns_ar, .name = "ar", .type = FR_TYPE_STRUCT, .dict = &dict_dns },
	{ NULL }
};

 char const *fr_dns_packet_codes[FR_DNS_CODE_MAX] = {
	[FR_DNS_QUERY] = "query",
	[FR_DNS_IQUERY] = "iquery",
	[FR_DNS_STATUS] = "status",
	[FR_DNS_UPDATE] = "update",
	[FR_DNS_STATEFUL_OP] = "stateful-operations",
};

bool fr_dns_packet_ok(uint8_t const *packet, size_t packet_len, bool query)
{
	if (packet_len <= DNS_HDR_LEN) {
		return false;
	}

	/*
	 *	query=0, response=1
	 */
	if (((packet[2] & 0x80) == 0) != query) {
		return false;
	}

	if (query) {
		/*
		 *	There should be at least one query, and no replies in the query!
		 */
		if (fr_net_to_uint16(packet + 4) == 0) {
			return false;
		}
		if (fr_net_to_uint16(packet + 6) != 0) {
			return false;
		}
		if (fr_net_to_uint16(packet + 8) != 0) {
			return false;
		}

		// additional records can exist!
	} else {
		/*
		 *	Replies _usually_ copy the query.  But not
		 *	always And replies can have zero or more answers.
		 */
	}

	return true;
}


/** Return the on-the-wire length of an attribute value
 *
 * @param[in] vp to return the length of.
 * @return the length of the attribute.
 */
size_t fr_dns_value_len(fr_pair_t const *vp)
{
	switch (vp->vp_type) {
	case FR_TYPE_VARIABLE_SIZE:
#ifndef NDEBUG
		if (!vp->da->flags.extra && (vp->da->flags.subtype == FLAG_ENCODE_DNS_LABEL)) {
			fr_assert_fail("DNS labels MUST be encoded/decoded with their own function, and not with generic 'string' functions");
			return 0;
		}
#endif

		if (vp->da->flags.length) return vp->da->flags.length;	/* Variable type with fixed length */

		/*
		 *	Arrays get maxed at 2^16-1
		 */
		if (vp->da->flags.array && ((vp->vp_type == FR_TYPE_STRING) || (vp->vp_type == FR_TYPE_OCTETS))) {
			if (vp->vp_length > 65535) return 65535;
		}

		return vp->vp_length;

	case FR_TYPE_STRUCTURAL:
		fr_assert_fail(NULL);
		return 0;

	default:
		return fr_value_box_network_length(&vp->data);
	}
}

fr_dns_labels_t *fr_dns_labels_get(uint8_t const *packet, size_t packet_len, bool init_mark)
{
	fr_dns_labels_t *lb = fr_dns_labels;

	if (!lb) return NULL;

	lb->start = packet;
	lb->end = packet + packet_len;

	lb->num = 1;
	lb->blocks[0].start = DNS_HDR_LEN;
	lb->blocks[0].end = DNS_HDR_LEN;

	if (init_mark) memset(lb->mark, 0, talloc_array_length(lb->mark));

	return lb;
}

/** Cleanup the memory pool used by vlog_request
 *
 */
static void _dns_labels_free(void *arg)
{
	talloc_free(arg);
	fr_dns_labels = NULL;
}


/** Resolve/cache attributes in the DNS dictionary
 *
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_dns_global_init(void)
{
	fr_dns_labels_t *lb;

	if (instance_count > 0) {
		instance_count++;
		return 0;
	}

	if (fr_dict_autoload(dns_dict) < 0) return -1;
	if (fr_dict_attr_autoload(dns_dict_attr) < 0) {
	fail:
		fr_dict_autofree(dns_dict);
		return -1;
	}

	lb =  (fr_dns_labels_t *) talloc_zero_array(NULL, uint8_t, sizeof(*lb) + sizeof(lb->blocks[0]) * 256);
	if (!lb) goto fail;

	lb->max = 256;

	lb->mark = talloc_array(lb, uint8_t, 65536);
	if (!lb->mark) {
		talloc_free(lb);
		goto fail;
	}

	fr_atexit_thread_local(fr_dns_labels, _dns_labels_free, lb);

	instance_count++;

	return 0;
}

void fr_dns_global_free(void)
{
	if (--instance_count > 0) return;

	fr_dict_autofree(dns_dict);
}

static fr_table_num_ordered_t const subtype_table[] = {
	{ L("dns_label"),			FLAG_ENCODE_DNS_LABEL },
};


static bool attr_valid(UNUSED fr_dict_t *dict, UNUSED fr_dict_attr_t const *parent,
		       UNUSED char const *name, UNUSED int attr, fr_type_t type, fr_dict_attr_flags_t *flags)
{
	/*
	 *	"arrays" of string/octets are encoded as a 16-bit
	 *	length, followed by the actual data.
	 */
	if (flags->array && ((type == FR_TYPE_STRING) || (type == FR_TYPE_OCTETS))) {
		flags->is_known_width = true;
	}

	/*
	 *	"extra" signifies that subtype is being used by the
	 *	dictionaries itself.
	 */
	if (flags->extra || !flags->subtype) return true;

	if (type != FR_TYPE_STRING) {
		fr_strerror_const("The 'dns_label' flag can only be used with attributes of type 'string'");
		return false;
	}

	flags->is_known_width = true;

	return true;
}

extern fr_dict_protocol_t libfreeradius_dns_dict_protocol;
fr_dict_protocol_t libfreeradius_dns_dict_protocol = {
	.name = "dns",
	.default_type_size = 2,
	.default_type_length = 2,
	.subtype_table = subtype_table,
	.subtype_table_len = NUM_ELEMENTS(subtype_table),
	.attr_valid = attr_valid,
};