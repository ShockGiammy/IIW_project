SERVER	= server
CLIENT	= client

SSRC	= server.c 
CSRC	= client.c

all: $(CLIENT) $(SERVER)

$(SERVER): $(SERVER).o reliable_udp.o helper.o manage_client.o
	gcc -o $(SERVER) $(SERVER).o reliable_udp.o helper.o manage_client.o  -pthread

$(SERVER).o: $(SSRC) reliable_udp.h helper.h manage_client.h
	gcc -o $(SERVER).o $(SSRC) -c -pthread

$(CLIENT): $(CLIENT).o reliable_udp.o helper.o manage_client.o
	gcc -o $(CLIENT) $(CLIENT).o reliable_udp.o helper.o manage_client.o -pthread

$(CLIENT).o: $(CSRC) reliable_udp.h helper.h manage_client.h
	gcc -o $(CLIENT).o $(CSRC) -c -pthread

reliable_udp.o: reliable_udp.c reliable_udp.h
	gcc -o reliable_udp.o reliable_udp.c -c

helper.o: helper.c helper.h
	gcc -o helper.o helper.c -c -pthread

manage_client.o : manage_client.c manage_client.h
	gcc -o manage_client.o manage_client.c -c

clean:
	rm -f *.o
	rm -f $(CLIENT)
	rm -f $(SERVER)
