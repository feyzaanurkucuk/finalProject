compile: 
	gcc  server.c -o chatserver -lpthread
	gcc  client.c -o chatclient -lpthread
	
clean: 
	rm chatclient
	rm chatserver
