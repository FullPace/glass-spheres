# Native simulation of the engine with a recording cv_out (no device, no Docker).
#   make -C test run
EU = ../third_party/eurorack
SRC = sim_test.cc ../src/engine.cc ../src/scales.cc \
      $(EU)/marbles/random/discrete_distribution_quantizer.cc $(EU)/marbles/random/lag_processor.cc \
      $(EU)/marbles/random/output_channel.cc $(EU)/marbles/random/quantizer.cc \
      $(EU)/marbles/random/t_generator.cc $(EU)/marbles/random/x_y_generator.cc \
      $(EU)/marbles/ramp/ramp_extractor.cc $(EU)/marbles/resources.cc \
      $(EU)/stmlib/dsp/units.cc $(EU)/stmlib/utils/random.cc
CXXFLAGS = -std=gnu++11 -O1 -g -DTEST -I$(EU) -I../third_party/mpc-vst-plugins/wrapper \
           -fsanitize=address,undefined -Wall -Wno-unused-parameter

run: sim_test
	./sim_test

sim_test: $(SRC) ../src/cv_out.h
	$(CXX) $(CXXFLAGS) $(SRC) -o $@

clean:
	rm -f sim_test

.PHONY: run clean
