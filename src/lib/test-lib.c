/* Copyright (c) 2007-2009 Dovecot authors, see the included COPYING file */

#include "test-lib.h"

int main(void)
{
	static void (*test_functions[])(void) = {
		test_aqueue,
		test_array,
		test_base64,
		test_bsearch_insert_pos,
		test_buffer,
		test_hex_binary,
		test_istream_crlf,
		test_istream_tee,
		test_mempool_alloconly,
		test_network,
		test_primes,
		test_priorityq,
		test_seq_range_array,
		test_strescape,
		test_str_find,
		test_str_sanitize,
		test_time_util,
		test_utc_mktime,
		NULL
	};
	return test_run(test_functions);
}
