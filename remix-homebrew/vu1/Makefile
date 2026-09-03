EE_BIN  = remixtest_vu1.elf
EE_OBJS = draw_3D.o main.o
EE_LIBS = -ldraw -lgraph -lmath3d -lpacket2 -ldma -lm -lkernel
EE_CFLAGS = -Wno-unused-function $(EXTRA_CFLAGS)
EE_DVP = dvp-as

%.o: %.vsm
	$(EE_DVP) $< -o $@

all: $(EE_BIN)
	$(EE_STRIP) --strip-all $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
