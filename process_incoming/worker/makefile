CC				:= g++
CFLAGS		:= -Wall -std=c++11
LDFLAGS 	:= `pkg-config --libs liblzma`
SOURCES		:= 	Processor.cpp	\
							Utils.cpp \
							CompressedFileWriter.cpp \
							RecordWriter.cpp
OBJECTS		:= $(SOURCES:.cpp=.o)
TARGET		:= Processor

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $? $(LDFLAGS) -o $@

%.o: %.cpp
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)