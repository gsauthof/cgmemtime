
CFLAGS = -g -std=gnu11 -Wall -Wno-missing-braces


EXEC = cgmemtime testa
TEMP = $(EXEC)


.PHONY: all
	
all: $(EXEC)

.PHONY: clean

clean:
	rm -f $(TEMP)


.PHONY: check

check:
	bash test.sh
