CC = g++
CFLAGS = -std=c++11 -Wall -pedantic -pthread -lboost_system
C_INCLUDE_DIRS = /usr/local/include
C_INCLUDE_PARAMS = $(addprefix -I , $(C_INCLUDE_DIRS))
C_LIB_DIRS = /usr/local/lib
C_LIB_PARAMS = $(addprefix -L , $(C_LIB_DIRS))
TARGET = ./

all:
	$(CC) http_server.cpp -o $(TARGET)http_server $(C_INCLUDE_PARAMS) $(C_LIB_PARAMS) $(CFLAGS)
	$(CC) console.cpp -o $(TARGET)console.cgi $(C_INCLUDE_PARAMS) $(C_LIB_PARAMS) $(CFLAGS)

clean:
	rm -f $(TARGET)http_server
	rm -f $(TARGET)console.cgi