#ifndef DOVEADM_SETTINGS_H
#define DOVEADM_SETTINGS_H

struct doveadm_settings {
	const char *mail_plugins;
	const char *mail_plugin_dir;
};

extern const struct setting_parser_info doveadm_setting_parser_info;
extern const struct doveadm_settings *doveadm_settings;

#endif