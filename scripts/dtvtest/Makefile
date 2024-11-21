DEP_NUMS = 01 02 03 04 05 06 07 08 09 10 11 12 13
DLOPEN_NUMS = 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15

DEP_LIBS = $(DEP_NUMS:%=libdeptls%.so)
DEP_LINKS = $(DEP_NUMS:%=-ldeptls%)

DLOPEN_LIBS = $(DLOPEN_NUMS:%=libdlopentls%.so)

.PHONY: default $(DEP_NUMS) $(DLOPEN_NUMS) clean run

default: dtvtest

clean:
	rm -f $(DEP_LIBS) $(DLOPEN_LIBS) libminaudit.so dtvtest

libdeptls%.so: $(SRCDIR)/tlslib.c
	gcc -shared -fPIC -o $@ -DINSTANCE=$(shell echo $@ | cut -c 10-11) -DLIBTYPE=dep $(SRCDIR)/tlslib.c

libdlopentls%.so: $(SRCDIR)/tlslib.c
	gcc -shared -fPIC -o $@ -DINSTANCE=$(shell echo $@ | cut -c 13-14) -DLIBTYPE=dlopen $(SRCDIR)/tlslib.c

libminaudit.so: $(SRCDIR)/minaudit.c
	gcc -shared -fPIC -o $@ $<

dtvtest: $(SRCDIR)/dtvtest.c $(DEP_LIBS)
	gcc -o $@ $(SRCDIR)/dtvtest.c -ldl -lpthread -g -Wl,-rpath,`pwd` -L`pwd` $(DEP_LINKS)

run: dtvtest libminaudit.so $(DLOPEN_LIBS)
	echo LD_AUDIT=./libminaudit.so ./dtvtest $(DLOPEN_NUMS)
	LD_AUDIT=./libminaudit.so ./dtvtest $(DLOPEN_NUMS)
	echo $?
