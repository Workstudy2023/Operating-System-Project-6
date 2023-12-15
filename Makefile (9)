CC	= gcc -g3
CFLAGS  = -g3 -Wall
TARGET1 = oss
TARGET2 = worker

OBJS1	= parent.o
OBJS2	= child.o

all:	$(TARGET1) $(TARGET2)

$(TARGET1):	$(OBJS1)
	$(CC) -o $(TARGET1) $(OBJS1)

$(TARGET2):	$(OBJS2)
	$(CC) -o $(TARGET2) $(OBJS2)

parent.o:	parent.c
	$(CC) $(CFLAGS) -c parent.c

child.o:	child.c
	$(CC) $(CFLAGS) -c child.c

clean:
	/bin/rm -f *.o $(TARGET1) $(TARGET2)