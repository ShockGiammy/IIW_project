SERVER	= server
CLIENT	= client

SSRC	= server.c 
CSRC	= client.c

all: $(CLIENT) $(SERVER)

$(SERVER): $(SERVER).o helper.o manage_client.o
	gcc -o $(SERVER) $(SERVER).o helper.o manage_client.o -pthread

$(SERVER).o: $(SSRC) helper.h manage_client.h
	gcc -o $(SERVER).o $(SSRC) -c -pthread

$(CLIENT): $(CLIENT).o helper.o manage_client.o
	gcc -o $(CLIENT) $(CLIENT).o helper.o manage_client.o -pthread

$(CLIENT).o: $(CSRC) helper.h manage_client.h
	gcc -o $(CLIENT).o $(CSRC) -c -pthread

helper.o: helper.c helper.h
	gcc -o helper.o helper.c -c

manage_client.o : manage_client.c manage_client.h
	gcc -o manage_client.o manage_client.c -c
clean:
	rm -f *.o
	rm -f $(CLIENT)
	rm -f $(SERVER)
