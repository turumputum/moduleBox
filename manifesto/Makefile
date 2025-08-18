CFLAGS = -Wall

# Should be equivalent to your list of C files, if you don't build selectively
SRC=$(wildcard src/*.cpp)


manifesto: $(SRC)
	g++ -g --permissive -Iinc -o .build/$@ $^ $(CFLAGS) $(LIBS)

all: manifesto
