##
## speex
##

SPEEX_CONFIGURE_ARGS := \
	--enable-shared \
	--disable-static \
	--disable-binaries \
	--disable-valgrind

$(eval $(call rules-source,speex,$(SRCDIR)/steamrtdeps/speex))
$(eval $(call rules-configure,speex,i386,unix))

SPEEX_DEPENDENCY := speex
