
DEFINES := -DDEBUG
INCS := -I/usr/include/exempi-2.0
CXXFLAGS := -std=c++11 -g -Wall -Werror
LDFLAGS := -g -lPocoNet -lPocoJSON -lPocoUtil -lPocoFoundation -lexempi

all:		picserver

picserver:	main.o
	$(CXX) -o $@ $< $(LDFLAGS)

main.o:		main.cpp
	$(CXX) $(DEFINES) $(INCS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f picserver main.o

.PHONY: clean
