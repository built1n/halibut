# Buttress master makefile

# Requires a compiler with -MD support, currently

# `make' from top level will build in directory `build'
# `make BUILDDIR=foo' from top level will build in directory foo
ifndef REALBUILD
ifndef BUILDDIR
ifdef TEST
BUILDDIR := test
else
BUILDDIR := build
endif
endif
all:
	@test -d $(BUILDDIR) || mkdir $(BUILDDIR)
	@make -C $(BUILDDIR) -f ../Makefile REALBUILD=yes
spotless:
	@test -d $(BUILDDIR) || mkdir $(BUILDDIR)
	@make -C $(BUILDDIR) -f ../Makefile spotless REALBUILD=yes
clean:
	@test -d $(BUILDDIR) || mkdir $(BUILDDIR)
	@make -C $(BUILDDIR) -f ../Makefile clean REALBUILD=yes
else

# The `real' makefile part.

CFLAGS += -Wall -W

ifdef TEST
CFLAGS += -DLOGALLOC
LIBS += -lefence
endif

ifdef RELEASE
ifndef VERSION
VERSION := $(RELEASE)
endif
else
CFLAGS += -g
endif

ifndef VER
ifdef VERSION
VER := $(VERSION)
endif
endif
ifdef VER
VDEF := -DVERSION=\"$(VER)\"
endif

SRC := ../

MODULES := main malloc ustring error help licence version misc tree234
MODULES += input keywords contents index style biblio
MODULES += bk_text bk_xhtml bk_whlp
MODULES += winhelp

OBJECTS := $(addsuffix .o,$(MODULES))
DEPS := $(addsuffix .d,$(MODULES))

buttress: $(OBJECTS)
	$(CC) $(LFLAGS) -o buttress $(OBJECTS) $(LIBS)

%.o: $(SRC)%.c
	$(CC) $(CFLAGS) -MD -c $<

version.o: FORCE
	$(CC) $(VDEF) -MD -c $(SRC)version.c

spotless:: clean
	rm -f *.d

clean::
	rm -f *.o buttress core

FORCE: # phony target to force version.o to be rebuilt every time

-include $(DEPS)

endif
