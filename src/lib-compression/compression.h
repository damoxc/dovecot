#ifndef COMPRESSION_H
#define COMPRESSION_H

struct compression_handler {
	const char *name;
	const char *ext;
	bool (*is_compressed)(struct istream *input);
	struct istream *(*create_istream)(struct istream *input,
					  bool log_errors);
	struct ostream *(*create_ostream)(struct ostream *output, int level);
};

extern const struct compression_handler compression_handlers[];

/* Lookup handler by its name (gz, bz2) */
const struct compression_handler *compression_lookup_handler(const char *name);
/* Detect handler by looking at the first few bytes of the input stream. */
const struct compression_handler *
compression_detect_handler(struct istream *input);
/* Lookup handler based on filename extension in the path */
const struct compression_handler *
compression_lookup_handler_from_ext(const char *path);

#endif
