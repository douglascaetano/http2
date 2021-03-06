# Parts of this Makefile where took from
# http://make.mad-scientist.net/papers/advanced-auto-dependency-generation/

CFLAGS = -Werror -Wall -g -DDEBUG=2

LIBS = -levent

DEPDIR = .d
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.Td

CC = gcc
LD = gcc

SERVER_SOURCES = server.c http2.c
CLIENT_SOURCES = client.c http2.c

.PHONY: all clean

all: client server

server: $(SERVER_SOURCES:.c=.o)
	@echo "  LD  $@"
	@$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

client: $(CLIENT_SOURCES:.c=.o)
	@echo "  LD  $@"
	@$(LD) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c $(DEPDIR)/%.d Makefile
	@echo "  CC  $<"
	@$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<
	@mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d

$(DEPDIR)/%.d: | $(DEPDIR) ;

$(DEPDIR):
	@mkdir -p $@

clean:
	-rm -rf $(DEPDIR) $(SERVER_SOURCES:.c=.o) $(CLIENT_SOURCES:.c=.o) client server

-include $(patsubst %,$(DEPDIR)/%.d,$(basename $(SERVER_SOURCES) $(CLIENT_SOURCES)))

