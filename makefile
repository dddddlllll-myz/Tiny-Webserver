CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
	CXXFLAGS += -g
else
	CXXFLAGS += -O2

endif

server: main.cpp ./Timer/Timer_Heap.cpp ./Http/Http_Conn.cpp ./Log/Log.cpp ./CGI_MySql/Sql_Conn_Pool.cpp  Webserver.cpp Config.cpp
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -lmysqlclient

clean:
	rm  -r server