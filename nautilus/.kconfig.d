deps_config := \
	src/dev/Kconfig \
	Kconfig

.config include/autoconf.h: $(deps_config)

$(deps_config):
