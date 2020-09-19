SERVER	= server
CLIENT	= client

SSRC	= server.c 
CSRC	= client.c

all: $(CLIENT) $(SERVER)

$(SERVER): $(SERVER).o reliable_udp.o helper.o
	gcc -o $(SERVER) $(SERVER).o reliable_udp.o helper.o -pthread

$(SERVER).o: $(SSRC) reliable_udp.h helper.h
	gcc -o $(SERVER).o $(SSRC) -c -pthread

$(CLIENT): $(CLIENT).o reliable_udp.o helper.o
	gcc -o $(CLIENT) $(CLIENT).o reliable_udp.o helper.o -pthread

$(CLIENT).o: $(CSRC) reliable_udp.h helper.h
	gcc -o $(CLIENT).o $(CSRC) -c -pthread

test: test.o reliable_udp.o helper.o
	gcc -o test test.o reliable_udp.o helper.o -pthread

reliable_udp.o: reliable_udp.c reliable_udp.h
	gcc -o reliable_udp.o reliable_udp.c -c

helper.o: helper.c helper.h
	gcc -o helper.o helper.c -c -pthread

clean:
	rm -f *.o
	rm -f $(CLIENT)
	rm -f $(SERVER)
	rm -f test
