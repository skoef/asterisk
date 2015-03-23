/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2015, Mark Michelson
 *
 * Mark Michelson <mmichelson@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*** MODULEINFO
	<depend>TEST_FRAMEWORK</depend>
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

#include <arpa/nameser.h>

#include "asterisk/test.h"
#include "asterisk/module.h"
#include "asterisk/dns_core.h"
#include "asterisk/dns_resolver.h"
#include "asterisk/dns_naptr.h"

#define DNS_HEADER_SIZE 96

const char DNS_HEADER[] = {
	/* ID  == 0 */
	0x00, 0x00,
	/* QR == 1, Opcode == 0, AA == 1, TC == 0, RD == 1 */
	0x85,
	/* RA == 1, Z == 0, RCODE == 0 */
	0x80,
	/* QDCOUNT == 1 */
	0x00, 0x01,
	/* ANCOUNT == 1 */
	0x00, 0x00,
	/* NSCOUNT == 0 */
	0x00, 0x00,
	/* ARCOUNT == 0 */
	0x00, 0x00,
};

static int generate_dns_header(unsigned short num_records, char *buf)
{
	unsigned short net_num_records = htons(num_records);

	memcpy(buf, DNS_HEADER, ARRAY_LEN(DNS_HEADER));
	/* Overwrite the ANCOUNT with the actual number of answers */
	memcpy(&buf[6], &net_num_records, sizeof(num_records));

	return ARRAY_LEN(DNS_HEADER);
}

const char DNS_QUESTION [] = {
	/* goose */
	0x05, 0x67, 0x6f, 0x6f, 0x73, 0x65,
	/* feathers */
	0x08, 0x66, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x73,
	/* end label */
	0x00,
	/* NAPTR type */
	0x00, 0x23,
	/* IN class */
	0x00, 0x01,
};

static int generate_dns_question(char *buf)
{
	memcpy(buf, DNS_QUESTION, ARRAY_LEN(DNS_QUESTION));
	return ARRAY_LEN(DNS_QUESTION);
}

const char NAPTR_ANSWER [] = {
	/* Domain points to name from question */
	0xc0, 0x0c,
	/* NAPTR type */
	0x00, 0x23,
	/* IN Class */
	0x00, 0x01,
	/* TTL (12345 by default) */
	0x00, 0x00, 0x30, 0x39,
};

static int generate_dns_answer(int ttl, char *buf)
{
	int net_ttl = htonl(ttl);

	memcpy(buf, NAPTR_ANSWER, ARRAY_LEN(NAPTR_ANSWER));
	/* Overwrite TTL if one is provided */
	if (ttl) {
		memcpy(&buf[6], &net_ttl, sizeof(int));
	}

	return ARRAY_LEN(NAPTR_ANSWER);
}

static int write_dns_string(const char *string, char *buf)
{
	uint8_t len = strlen(string);
	buf[0] = len;
	if (len) {
		memcpy(&buf[1], string, len);
	}

	return len + 1;
}

static int write_dns_domain(const char *string, char *buf)
{
	char *copy = ast_strdupa(string);
	char *part;
	char *ptr = buf;

	while ((part = strsep(&copy, "."))) {
		ptr += write_dns_string(part, ptr);
	}
	ptr += write_dns_string("", ptr);

	return ptr - buf;
}

struct naptr_record {
	uint16_t order;
	uint16_t preference;
	const char *flags;
	const char *services;
	const char *regexp;
	const char *replacement;
};

static int generate_naptr_record(struct naptr_record *record, char *buf)
{
	uint16_t net_order = htons(record->order);
	uint16_t net_preference = htons(record->preference);
	char *ptr = buf;

	memcpy(ptr, &net_order, sizeof(net_order));
	ptr += sizeof(net_order);

	memcpy(ptr, &net_preference, sizeof(net_preference));
	ptr += sizeof(net_preference);

	ptr += write_dns_string(record->flags, ptr);
	ptr += write_dns_string(record->services, ptr);
	ptr += write_dns_string(record->regexp, ptr);
	ptr += write_dns_domain(record->replacement, ptr);

	return ptr - buf;
}

static struct naptr_record *test_records;
static int num_test_records;
static char ans_buffer[1024];

static void *naptr_thread(void *dns_query)
{
	struct ast_dns_query *query = dns_query;
	int i;
	char *ptr = ans_buffer;
	
	ptr += generate_dns_header(num_test_records, ptr);
	ptr += generate_dns_question(ptr);

	for (i = 0; i < num_test_records; ++i) {
		unsigned short rdlength;
		unsigned short net_rdlength;

		ptr += generate_dns_answer(0, ptr);
		rdlength = generate_naptr_record(&test_records[i], ptr + 2);
		net_rdlength = htons(rdlength);
		memcpy(ptr, &net_rdlength, 2);
		ptr += 2;
		ptr += rdlength;
	}

	ast_dns_resolver_set_result(query, 0, 0, ns_r_noerror, "goose.feathers", ans_buffer, ptr - ans_buffer);

	for (i = 0; i < num_test_records; ++i) {
		char record[128];
		ptr = record;

		ptr += generate_naptr_record(&test_records[i], ptr);
		ast_dns_resolver_add_record(query, ns_t_naptr, ns_c_in, 12345, record, ptr - record);
	}

	ast_dns_resolver_completed(query);

	ao2_ref(query, -1);
	return NULL;
}

static int naptr_resolve(struct ast_dns_query *query)
{
	pthread_t thread;

	return ast_pthread_create_detached(&thread, NULL, naptr_thread, ao2_bump(query));
}

static int naptr_cancel(struct ast_dns_query *query)
{
	return 0;
}

static struct ast_dns_resolver naptr_resolver = {
	.name = "naptr_test",
	.priority = 0,
	.resolve = naptr_resolve,
	.cancel = naptr_cancel,
};

AST_TEST_DEFINE(naptr_resolve_nominal)
{
	RAII_VAR(struct ast_dns_result *, result, NULL, ast_dns_result_free);
	const struct ast_dns_record *record;
	struct naptr_record records[] = {
		{ 100, 100, "A", "BLAH", "", "goose.down" },
		{ 100, 200, "A", "BLAH", "", "duck.down" },
	};
	enum ast_test_result_state res = AST_TEST_PASS;

	switch (cmd) {
	case TEST_INIT:
		info->name = "naptr_resolve";
		info->category = "/main/dns/naptr/";
		info->summary = "Test resolution of NAPTR records";
		info->description = "yep";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	test_records = records;
	num_test_records = 2;
	memset(ans_buffer, 0, sizeof(ans_buffer));

	ast_dns_resolver_register(&naptr_resolver);

	if (ast_dns_resolve("goose.feathers", ns_t_naptr, ns_c_in, &result)) {
		ast_test_status_update(test, "DNS resolution failed\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	if (!result) {
		ast_test_status_update(test, "DNS resolution returned no result\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	for (record = ast_dns_result_get_records(result); record; record = ast_dns_record_get_next(record)) {
		ast_log(LOG_NOTICE, "Examining record\n");
		ast_log(LOG_NOTICE, "order is %hu\n", ast_dns_naptr_get_order(record));
		ast_log(LOG_NOTICE, "preference is %hu\n", ast_dns_naptr_get_preference(record));
		ast_log(LOG_NOTICE, "flags is %s\n", ast_dns_naptr_get_flags(record));
		ast_log(LOG_NOTICE, "service is %s\n", ast_dns_naptr_get_service(record));
		ast_log(LOG_NOTICE, "regexp is %s\n", ast_dns_naptr_get_regexp(record));
		ast_log(LOG_NOTICE, "replacement is %s\n", ast_dns_naptr_get_replacement(record));
	}

cleanup:

	ast_dns_resolver_unregister(&naptr_resolver);

	test_records = NULL;
	num_test_records = 0;
	memset(ans_buffer, 0, sizeof(ans_buffer));

	return res;
}

static int unload_module(void)
{
	AST_TEST_UNREGISTER(naptr_resolve_nominal);

	return 0;
}

static int load_module(void)
{
	AST_TEST_REGISTER(naptr_resolve_nominal);

	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "DNS API Tests");
