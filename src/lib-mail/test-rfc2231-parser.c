/* Copyright (c) 2007-2011 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "rfc822-parser.h"
#include "rfc2231-parser.h"
#include "test-common.h"

static void test_rfc2231_parser(void)
{
	const char *input =
		"; key4*=us-ascii''foo"
		"; key*2=ba%"
		"; key2*0=a"
		"; key3*0*=us-ascii'en'xyz"
		"; key*0=\"foo\""
		"; key2*1*=b%25"
		"; key3*1=plop%"
		"; key*1=baz";
	const char *output[] = {
		"key",
		"foobazba%",
		"key2*",
		"''ab%25",
		"key3*",
		"us-ascii'en'xyzplop%25",
		"key4*",
		"us-ascii''foo",
		NULL
	};
	struct rfc822_parser_context parser;
	const char *const *result;
	unsigned int i;

	test_begin("rfc2231 parser");
	rfc822_parser_init(&parser, (const void *)input, strlen(input), NULL);
	test_assert(rfc2231_parse(&parser, &result) == 0);
	for (i = 0; output[i] != NULL && result[i] != NULL; i++)
		test_assert(strcmp(output[i], result[i]) == 0);
	test_assert(output[i] == NULL && result[i] == NULL);
	test_end();
}

int main(void)
{
	static void (*test_functions[])(void) = {
		test_rfc2231_parser,
		NULL
	};
	return test_run(test_functions);
}
