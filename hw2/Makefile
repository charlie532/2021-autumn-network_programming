CXX = g++
CFLAGS = -Wall -g
TARGET = ./work_dir/

all:
	$(CXX) $(CFLAGS) np_simple.cpp -o $(TARGET)np_simple
	$(CXX) $(CFLAGS) np_single_proc.cpp -o $(TARGET)np_single_proc
	$(CXX) $(CFLAGS) np_multi_proc.cpp -o $(TARGET)np_multi_proc
     
clean:
	rm $(TARGET)np_simple
	rm $(TARGET)np_single_proc
	rm $(TARGET)np_multi_proc
