default: clean shared.h server client

test:	clean
	g++ -Wall -Wextra -o server server.cpp shared.h
	#g++ -Wall -Wextra -o client client.cpp shared.h

debug:	clean
	g++ -g -Wall -Wextra -o server server.cpp shared.h
	#g++ -g -Wall -Wextra -o client client.cpp shared.h

clean:
	@rm -f server
	@rm -f client
	@rm -f servertester

server: 
	@rm -f server
	g++ -Wall -Wextra -o server server.cpp 

client: 
	@rm -f client
	g++ -Wall -Wextra -o client client.cpp 

servertester: 
	@rm -f servertester
	g++ -Wall -Wextra -o servertester servertester.cpp shared.h

