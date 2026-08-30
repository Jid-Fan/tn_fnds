OBJS = dio.o frq.o matlabfunctions.o platinum.o star.o synthesis_w1.o test.o fft.o wavread.o pitch_shift_rb.o ../vendor/rubberband-3.3.0/single/RubberBandSingle.o
CXX = g++
CFLAGS = -O2 -msse3 -std=c++11
INCLUDES = -I. -I../vendor/rubberband-3.3.0
LIBS = 

tn_fndScream : $(OBJS) fft.h mmsystem.h windows.h wavread.h world.h
	$(CXX) -o $@ $(OBJS) $(LIBS) $(CFLAGS)

.cpp.o:
	$(CXX) $(CFLAGS) $(INCLUDES) -c $<

../vendor/rubberband-3.3.0/single/RubberBandSingle.o: ../vendor/rubberband-3.3.0/single/RubberBandSingle.cpp
	$(CXX) $(CFLAGS) -I../vendor/rubberband-3.3.0 -I../vendor/rubberband-3.3.0/src -c $< -o $@

clean:
	rm tn_fndScream $(OBJS)
