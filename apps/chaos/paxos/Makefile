#TARGET ?= sky

CONTIKI_PROJECT = paxos-app
CONTIKI_WITH_CHAOS = 1
ifeq ($(dynamic),1)
	CHAOS_NODE_DYNAMIC=1
else
	ifeq ($(dynamic),2)
		CHAOS_NODE_DYNAMIC=1
	else
		CHAOS_NODE_DYNAMIC=0
	endif
endif

CFLAGS += -DPROJECT_CONF_H=\"project-conf.h\"

CONTIKI = ../../..
include $(CONTIKI)/Makefile.include

CONTIKI_TARGET_SOURCEFILES += contiki-wsn430-platform.c
include $(CONTIKI)/platform/wsn430/Makefile.common


id: burn-nodeid.upload


all: $(CONTIKI_PROJECT)

