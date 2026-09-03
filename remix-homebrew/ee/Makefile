EE_BIN  = remixtest.elf
EE_OBJS = main.o

EE_INCS = -I$(GSKIT)/include
EE_LIBS = -L$(GSKIT)/lib -lgskit -ldmakit -lmath3d -ldraw -lgraph -lm -lkernel
EE_CFLAGS = -Wno-unused-function

all: $(EE_BIN)
	$(EE_STRIP) --strip-all $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
