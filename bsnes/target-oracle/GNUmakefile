# bsnes target-oracle — headless 65816 debugger console driver (soe-re)
# Build: make -C bsnes target=oracle -j$(nproc)
name := bsnes-oracle
local := false
openmp := false
flags += -Wno-narrowing -Wno-multichar -g

objects := oracle $(objects)
objects := $(patsubst %,obj/%.o,$(objects))

#headless: no GUI — drop the X11 link flags the application branch added
#(dev symlinks absent on this host; oracle.o calls no X11 functions)
ifeq ($(platform),linux)
  options := $(filter-out -lX11 -lXext,$(options))
endif

obj/oracle.o: target-oracle/oracle.cpp

all: $(objects)
ifeq ($(platform),linux)
	$(strip $(compiler) -o out/$(name) $(objects) -Wl,-Bdynamic $(options) -lpthread -ldl)
else
	$(strip $(compiler) -o out/$(name) $(objects) $(options) -lpthread -ldl)
endif
