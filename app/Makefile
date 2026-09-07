PROG1	= $(shell jq -r '.acapPackageConf.setup.appName' manifest.json)
OBJS1	= main.c timer.c display.c mediaclip.c event.c buttons.c \
	  local_vapix.c io_discovery.c local_config.c app_config.c \
	  axparam_util.c webui.c

# Everything lives in one binary and one process, exactly like the C8310
# Customizer: the embedded web UI (see webui.c / webui_start()) runs its
# own tiny HTTP server on its own port rather than going through Apache/
# httpConfig, which was confirmed to fail silently on real hardware. Same
# process means the web UI shares live timer state with the physical
# button handling with no IPC and no stale-cache asymmetry.

PROGS	= $(PROG1)
DEBUG_DIR = debug

PKGS = glib-2.0 gio-2.0 axevent axparameter libcurl jansson

CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))

CFLAGS += -Wall \
          -Wextra \
          -Wformat=2 \
          -Wpointer-arith \
          -Wbad-function-cast \
          -Wstrict-prototypes \
          -Wmissing-prototypes \
          -Wdisabled-optimization \
          -Wfloat-equal \
          -W

all:	$(PROGS)

$(PROG1): $(OBJS1)
	install -d $(DEBUG_DIR)
	$(CC) $^ $(CFLAGS) $(LIBS) $(LDFLAGS) -lm $(LDLIBS) -o $(DEBUG_DIR)/$@
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

clean:
	rm -rf $(PROGS) *.o *.eap* *_LICENSE.txt package.conf* param.conf tmp* $(DEBUG_DIR)
